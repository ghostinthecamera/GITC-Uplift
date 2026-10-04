#include "helper/transports.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <format>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

#include "addon/dred.hpp"
#include "addon/frame_trigger.hpp"
#include "bridge/d3d11_bridge.hpp"
#include "bridge/record_nr.hpp"
#include "color/encoding.hpp"
#include "ipc/control_block.hpp"
#include "nr/log.hpp"

namespace uplift::helper {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t BLOCK_ALIGN = 64ull * 1024ull;  // a heap opened from a file mapping is placed in 64 KiB units

// Plan 12 (protocol 5): a shared texture's allocation size, which an OpenGL memory-object import needs (the texture's own size is not it).
uint64_t AllocationBytes(ID3D12Device* device, ID3D12Resource* resource) {
  const D3D12_RESOURCE_DESC description = resource->GetDesc();
  return device->GetResourceAllocationInfo(0u, 1u, &description).SizeInBytes;
}

// What one recording on the side's next list came to.
struct Recorded {
  bool busy = false;       // the ring had no free allocator: nothing was recorded
  bool submitted = false;  // the list was executed
  bool wrote = false;      // NR wrote targets.color on it
};

// One bridged recording on the side's next list, as D3D11Bridge does it: `before` and `after` add each transport's own
// commands around bridge::RecordNr (`after` always runs, so its transitions restore the states; `wrote` says whether NR
// wrote the colour target). A recording that throws is closed, never executed.
Recorded RecordRun(bridge::D3D12Side& side, addon::DeviceContext& context, addon::TriggerPoint point,
                   const bridge::BridgeTargets& targets, const std::function<void(ID3D12GraphicsCommandList*)>& before,
                   const std::function<void(ID3D12GraphicsCommandList*, bool wrote)>& after) {
  if (!side.SlotFree()) return {.busy = true};
  ID3D12GraphicsCommandList* const list = side.BeginList();
  if (list == nullptr) return {};
  before(list);
  bool recorded = true;
  bool wrote = false;
  try {
    wrote = bridge::RecordNr(context, point, list, targets);
  } catch (...) {
    // Half a recording never executes.
    nr::Log(nr::LogLevel::ERR, "NR's recording on the helper's Direct3D 12 device threw; this frame goes without NR");
    recorded = false;
  }
  after(list, recorded && wrote);
  if (!recorded) {
    side.CloseList();
    return {};
  }
  return {.submitted = side.ExecuteList(), .wrote = wrote};
}

// The first non-empty text, viewed: what Describe reports while NR is off (the latch, then the add-on's problem, then a share
// failure of this size). The views stay valid because each names a member of the transport.
std::string_view FirstText(std::initializer_list<std::string_view> texts) {
  for (const std::string_view text : texts) {
    if (!text.empty()) return text;
  }
  return {};
}

// A copy between a texture (subresource 0) and a buffer holding it at `footprint`. The buffer is written by the copy out and
// read by the copy in, both on one list, so each copy moves it out of COMMON and back explicitly (the debug layer rejects a
// buffer read and then written in one list on the strength of implicit promotion alone).
void CopyTextureBuffer(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, ID3D12Resource* buffer,
                       const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, bool to_buffer) {
  D3D12_TEXTURE_COPY_LOCATION placed = {};
  placed.pResource = buffer;
  placed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  placed.PlacedFootprint = footprint;
  D3D12_TEXTURE_COPY_LOCATION indexed = {};
  indexed.pResource = texture;
  indexed.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  indexed.SubresourceIndex = 0u;
  const D3D12_RESOURCE_STATES buffer_state = (to_buffer ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE);
  bridge::Transition(list, buffer, D3D12_RESOURCE_STATE_COMMON, buffer_state);
  if (to_buffer) {
    list->CopyTextureRegion(&placed, 0u, 0u, 0u, &indexed, nullptr);
  } else {
    list->CopyTextureRegion(&indexed, 0u, 0u, 0u, &placed, nullptr);
  }
  bridge::Transition(list, buffer, buffer_state, D3D12_RESOURCE_STATE_COMMON);
}

// A committed texture in COMMON, private to the helper (BLOCK's target).
HRESULT CreateTexture(ID3D12Device* device, nr::Size size, DXGI_FORMAT format, ComPtr<ID3D12Resource>* texture) {
  const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC description = {
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Alignment = 0u,
      .Width = size.width,
      .Height = size.height,
      .DepthOrArraySize = 1u,
      .MipLevels = 1u,
      .Format = format,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
  };
  return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         IID_PPV_ARGS(texture->ReleaseAndGetAddressOf()));
}

/// One image shared with the add-on on a CPU-ordered transport: BLOCK's frame, mask and motion, and KMT's mask and motion.
struct SharedImage {
  ComPtr<ID3D12Resource> texture;  // BLOCK: the helper's own, COMMON between lists; KMT: the add-on's, opened here
  ComPtr<ID3D12Heap> heap;         // BLOCK: the add-on's file mapping, opened as a heap
  ComPtr<ID3D12Resource> buffer;   // BLOCK: placed in `heap`; the copies go between it and `texture`
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  nr::Size size;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint64_t bytes = 0u;  // the helper's own memory: the texture and the block (0 for KMT: the texture is the add-on's allocation)
};

// What MakeBlock hands the add-on: the file mapping (it pulls it with DUPLICATE_CLOSE_SOURCE, so this side never closes it once
// handed over), its size and the texture's row pitch.
struct BlockHandle {
  HANDLE mapping = nullptr;
  uint64_t bytes = 0u;
  uint32_t row_pitch = 0u;
};

// BLOCK's recipe (design §4.2), one function for the frame and for the mask and motion: the texture, the footprint, the
// mapping (64 KiB aligned), OpenExistingHeapFromFileMapping, and the placed buffer (ALLOW_CROSS_ADAPTER when the heap says
// SHARED_CROSS_ADAPTER). `what` names the image in the texts. "" on success, else why not; the caller retires what was made
// before a failure.
std::string MakeBlock(bridge::D3D12Side& side, std::string_view what, nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel,
                      SharedImage* image, BlockHandle* handle) {
  ID3D12Device* const device = side.Device();
  if (const HRESULT result = CreateTexture(device, size, format, &image->texture); FAILED(result)) {
    return std::format("The {} could not be copied into Uplift's helper (DXGI_FORMAT {}, target texture HRESULT {:#010x})", what,
                       static_cast<int>(format), static_cast<uint32_t>(result));
  }
  const D3D12_RESOURCE_DESC description = image->texture->GetDesc();
  uint64_t total = 0u;
  device->GetCopyableFootprints(&description, 0u, 1u, 0u, &image->footprint, nullptr, nullptr, &total);
  const uint64_t heap_bytes = (total + BLOCK_ALIGN - 1u) & ~(BLOCK_ALIGN - 1u);
  ComPtr<ID3D12Device3> device3;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3)))) {
    return "Uplift's helper needs Windows 10 1703 or newer to share frames through memory";
  }
  const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(heap_bytes >> 32u),
                                            static_cast<DWORD>(heap_bytes & 0xFFFFFFFFu), nullptr);
  if (mapping == nullptr) {
    return std::format("Not enough memory for the {} block ({} MiB, CreateFileMapping {:#010x})", what, heap_bytes >> 20u,
                       static_cast<uint32_t>(GetLastError()));
  }
  if (const HRESULT result = device3->OpenExistingHeapFromFileMapping(mapping, IID_PPV_ARGS(&image->heap)); FAILED(result)) {
    CloseHandle(mapping);
    return std::format("The {} block could not be opened as a Direct3D 12 heap (HRESULT {:#010x})", what, static_cast<uint32_t>(result));
  }
  const D3D12_HEAP_DESC heap_description = image->heap->GetDesc();
  D3D12_RESOURCE_DESC buffer_description = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Alignment = 0u,
      .Width = heap_bytes,
      .Height = 1u,
      .DepthOrArraySize = 1u,
      .MipLevels = 1u,
      .Format = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      .Flags = D3D12_RESOURCE_FLAG_NONE,
  };
  if ((heap_description.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) != 0) {
    buffer_description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
  }
  if (const HRESULT result = device->CreatePlacedResource(image->heap.Get(), 0u, &buffer_description, D3D12_RESOURCE_STATE_COMMON,
                                                          nullptr, IID_PPV_ARGS(&image->buffer));
      FAILED(result)) {
    CloseHandle(mapping);
    return std::format("The {} block could not be placed in the heap (HRESULT {:#010x})", what, static_cast<uint32_t>(result));
  }
  image->size = size;
  image->format = format;
  image->bytes = size.Pixels() * bytes_per_pixel + heap_bytes;
  handle->mapping = mapping;
  handle->bytes = heap_bytes;
  handle->row_pitch = image->footprint.Footprint.RowPitch;
  return {};
}

// `image` onto the list in the state NR reads it in (COPY_SOURCE for a mask, SHADER_READ for the motion): BLOCK copies the block's
// rows into the texture first; KMT's texture is already the add-on's.
void BringIn(ID3D12GraphicsCommandList* list, const SharedImage& image, D3D12_RESOURCE_STATES state) {
  if (image.buffer == nullptr) {
    bridge::Transition(list, image.texture.Get(), D3D12_RESOURCE_STATE_COMMON, state);
    return;
  }
  bridge::Transition(list, image.texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
  CopyTextureBuffer(list, image.texture.Get(), image.buffer.Get(), image.footprint, false);
  bridge::Transition(list, image.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
}

// BLOCK and KMT: no GPU wait on anything the add-on signals (the add-on orders its own side on the CPU before it sends
// RUN), so a RUN is one list, one `progress` signal, and a reply the helper sets when that signal is reached. The mask and the
// motion ride along (design §4.2): KMT opens the add-on's shared textures, BLOCK makes a block for each.
class CpuOrderedTransport : public Transport {
 public:
  void Run(const ipc::Run& run, addon::DeviceContext& context, ipc::Reply* reply, uint64_t* complete_at) override {
    // 1.1.2: a recording without LaunchPad's motion keeps the shared motion texture (the add-on keeps its share too); it goes when NR stops.
    ID3D12Resource* const target = Target();
    if (target == nullptr || !latch_.empty()) {
      reply->ok = 0u;
      ipc::CopyText(reply->error, latch_.empty() ? std::string_view("no frame is shared with the helper") : std::string_view(latch_));
      return;
    }
    // A fresh UPLIFT_MASK, and this frame's UPLIFT_MV, when the add-on says it wrote them and they were shared.
    const SharedImage* const mask = ((run.mask_fresh != 0u && mask_.texture != nullptr) ? &mask_ : nullptr);
    const SharedImage* const motion = ((run.motion != 0u && motion_.texture != nullptr) ? &motion_ : nullptr);
    const bridge::BridgeTargets targets = {
        .color = target,
        .mask = (mask != nullptr ? mask->texture.Get() : nullptr),
        .motion = (motion != nullptr ? motion->texture.Get() : nullptr),
    };
    const Recorded recorded = RecordRun(
        side_, context, static_cast<addon::TriggerPoint>(run.point), targets,
        [this, mask, motion](ID3D12GraphicsCommandList* list) {
          CopyIn(list);
          if (mask != nullptr) {
            BringIn(list, *mask, D3D12_RESOURCE_STATE_COPY_SOURCE);
          }
          if (motion != nullptr) {
            BringIn(list, *motion, bridge::SHADER_READ);
          }
        },
        [this, mask, motion](ID3D12GraphicsCommandList* list, bool wrote) {
          CopyOut(list, wrote);
          if (motion != nullptr) {
            bridge::Transition(list, motion->texture.Get(), bridge::SHADER_READ, D3D12_RESOURCE_STATE_COMMON);
          }
          if (mask != nullptr) {
            bridge::Transition(list, mask->texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
          }
        });
    reply->ok = 1u;
    reply->waited = 1u;
    if (recorded.busy) {
      reply->busy = 1u;
      return;
    }
    if (!recorded.submitted) return;  // nothing was submitted: the reply is final now
    reply->submitted = 1u;
    if (mask != nullptr && !mask_logged_) {
      mask_logged_ = true;  // once per attach: the e2e case and the checklist read it as "the mask reached NR"
      nr::Logf(nr::LogLevel::INFO, "mask: UPLIFT_MASK reached NR ({}x{}, DXGI_FORMAT {})", mask->size.width, mask->size.height,
               static_cast<int>(mask->format));
    }
    if (motion != nullptr && !motion_logged_) {
      motion_logged_ = true;
      nr::Logf(nr::LogLevel::INFO, "motion: UPLIFT_MV reached NR ({}x{})", motion->size.width, motion->size.height);
    }
    const uint64_t value = side_.LastSignalled() + 1u;  // `progress` only grows, whatever transport ran before this one
    const bool signalled = side_.SignalProgress(value);
    last_use_ = value;
    reply->signalled = signalled ? 1u : 0u;
    // A refused signal leaves nothing to wait for: an immediate reply that says NR did not write.
    if (!signalled) return;
    reply->wrote = recorded.wrote ? 1u : 0u;
    *complete_at = value;
  }
  void ReleaseMask() override { Retire(&mask_); }
  void Stop(uint64_t /*waited11*/, std::string_view reason) override {
    if (latch_.empty()) latch_ = std::string(reason);
  }

  // SHARE_MASK and SHARE_MOTION (design §4.2): replaces what was shared before. A latch, an empty size or a format NR cannot read
  // replies `ok = 0` with the reason.
  void Share(ipc::RequestKind kind, const ipc::Share& share, ipc::Reply* reply) override {
    const bool is_mask = (kind == ipc::RequestKind::SHARE_MASK);
    SharedImage& image = (is_mask ? mask_ : motion_);
    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(share.dxgi_format);
    const nr::Size size = {share.width, share.height};
    const std::optional<color::MaskFormatInfo> mask_format = (is_mask ? color::DescribeMaskFormat(format) : std::nullopt);
    if (!latch_.empty() || size.Empty() || (is_mask ? !mask_format : format != DXGI_FORMAT_R16G16_FLOAT)) {
      reply->ok = 0u;
      ipc::CopyText(reply->error, latch_.empty() ? std::string_view("this image cannot be shared") : std::string_view(latch_));
      return;
    }
    Retire(&image);
    BlockHandle handle;
    const std::string problem =
        Provide(share, (is_mask ? "mask" : "motion"), (is_mask ? mask_format->bytes_per_pixel : 4u), &image, &handle);
    if (!problem.empty()) {
      Retire(&image);  // whatever Provide made before it failed
      nr::Log(nr::LogLevel::WARN, problem);
      reply->ok = 0u;
      ipc::CopyText(reply->error, problem);
      return;
    }
    if (handle.mapping != nullptr) {
      (is_mask ? reply->handles.mask : reply->handles.motion) = reinterpret_cast<uintptr_t>(handle.mapping);
      (is_mask ? reply->handles.mask_bytes : reply->handles.motion_bytes) = handle.bytes;
      (is_mask ? reply->handles.mask_row_pitch : reply->handles.motion_row_pitch) = handle.row_pitch;
    }
    reply->ok = 1u;
  }

 protected:
  explicit CpuOrderedTransport(bridge::D3D12Side& side) : side_(side) {}
  // The texture NR runs on (COMMON in and out), or null while there is none.
  [[nodiscard]] virtual ID3D12Resource* Target() const = 0;
  virtual void CopyIn(ID3D12GraphicsCommandList* /*list*/) {}
  virtual void CopyOut(ID3D12GraphicsCommandList* /*list*/, bool /*wrote*/) {}
  // Makes `image` for a Share: BLOCK a block of the add-on's format (its mapping in `handle`), KMT opens `share.kmt_handle`.
  // "" on success, else why not.
  virtual std::string Provide(const ipc::Share& share, std::string_view what, uint32_t bytes_per_pixel, SharedImage* image,
                              BlockHandle* handle) = 0;

  // Everything goes to the retire list, which keeps it until the queue passed the newest submitted recording.
  void Retire(SharedImage* image) {
    side_.Retire(std::move(image->texture), nullptr, last_use_);  // the queue may still read it
    side_.Retire(std::move(image->buffer), std::move(image->heap), last_use_);
    *image = {};
  }
  // NR is off, the transport stopped, or the session ended: the mask and the motion go (an effect that comes back asks again).
  void RetireImages() {
    Retire(&mask_);
    Retire(&motion_);
  }

  bridge::D3D12Side& side_;
  std::string latch_;       // set once: the transport stopped for the session
  std::string problem_;     // Describe's problem text
  uint64_t last_use_ = 0u;  // the value after the newest submitted recording: what the surfaces wait for
  SharedImage mask_;
  SharedImage motion_;
  bool mask_logged_ = false;
  bool motion_logged_ = false;
};

// Plain D3D9 (design §2.4): the add-on writes the frame into a shared-memory block; this side opens it as a D3D12 heap
// (OpenExistingHeapFromFileMapping, measured bit-exact by the probe), places a buffer in it, and copies block -> texture,
// NR, texture -> block on one list. The mask and the motion are blocks of their own (MakeBlock).
class BlockTransport final : public CpuOrderedTransport {
 public:
  explicit BlockTransport(bridge::D3D12Side& side) : CpuOrderedTransport(side) {}
  ~BlockTransport() override {
    Retire(&frame_);
    RetireImages();
  }

  addon::TargetInfo Describe(const ipc::Target& target, bool enabled, bool running, ipc::Handles* handles) override {
    side_.FreeFinished();
    const nr::Size size = {target.width, target.height};
    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(target.dxgi_format);
    addon::TargetInfo info = {.color_space = static_cast<color::ColorSpace>(target.color_space), .size = size, .format = format};
    problem_ = std::string(ipc::TextOf(target.problem));
    if (!enabled) {
      failure_.reset();  // the user's own off: a share failure is forgotten only then, and stays while NR is merely off
    }
    if (!running || !latch_.empty()) {
      RetireImages();
    }
    const std::optional<color::FormatInfo> described = color::DescribeFormat(format);
    const bool failed_here = (failure_ && failure_->size == size && failure_->format == format);
    if (!running || !latch_.empty() || !problem_.empty() || !described || size.Empty() || failed_here) {
      // NR is off, the transport stopped, or this is an image NR cannot take (DeviceContext says why): nothing is kept for it.
      // The problem is reported while NR is off too, so the Session stays OFF instead of loading only to unload again; a
      // share failure is not retried for this size and format until NR is re-enabled.
      Retire(&frame_);
      info.problem = FirstText({latch_, problem_, (failed_here ? std::string_view(failure_->text) : std::string_view())});
      return info;
    }
    // A block is (re)made, and handed out in this reply, while the add-on says it has none mapped: a block handed out when the
    // add-on had no objects to map it (its `running` lags a late FRAME reply by a present) is thereby asked for again, not lost.
    if (!frame_.texture || frame_.size != size || frame_.format != format || target.block_mapped == 0u) {
      Retire(&frame_);
      BlockHandle handle;
      if (const std::string text = MakeBlock(side_, "frame", size, format, described->bytes_per_pixel, &frame_, &handle); !text.empty()) {
        Retire(&frame_);  // whatever MakeBlock made before it failed
        failure_ = Failure{.size = size, .format = format, .text = text};
        nr::Log(nr::LogLevel::WARN, text);
        info.problem = failure_->text;
        return info;
      }
      failure_.reset();
      handles->block = reinterpret_cast<uintptr_t>(handle.mapping);
      handles->block_bytes = handle.bytes;
      handles->block_row_pitch = handle.row_pitch;
    }
    info.resource = frame_.texture.Get();
    return info;
  }
  void Detach() override {
    Retire(&frame_);
    RetireImages();
    failure_.reset();
  }
  [[nodiscard]] uint64_t SurfaceBytes() const override { return frame_.bytes + mask_.bytes + motion_.bytes; }
  [[nodiscard]] std::string Line() const override { return "frames cross through shared memory"; }

 protected:
  [[nodiscard]] ID3D12Resource* Target() const override { return (frame_.buffer ? frame_.texture.Get() : nullptr); }
  void CopyIn(ID3D12GraphicsCommandList* list) override {
    bridge::Transition(list, frame_.texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    CopyTextureBuffer(list, frame_.texture.Get(), frame_.buffer.Get(), frame_.footprint, false);
    bridge::Transition(list, frame_.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
  }
  void CopyOut(ID3D12GraphicsCommandList* list, bool wrote) override {
    if (!wrote) return;
    bridge::Transition(list, frame_.texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyTextureBuffer(list, frame_.texture.Get(), frame_.buffer.Get(), frame_.footprint, true);
    bridge::Transition(list, frame_.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  }
  // A block of the add-on's size and format, mapped by the add-on: it reads the mapping, the row pitch and the byte count in the reply.
  std::string Provide(const ipc::Share& share, std::string_view what, uint32_t bytes_per_pixel, SharedImage* image,
                      BlockHandle* handle) override {
    return MakeBlock(side_, what, {share.width, share.height}, static_cast<DXGI_FORMAT>(share.dxgi_format), bytes_per_pixel, image,
                     handle);
  }

 private:
  struct Failure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string text;
  };

  SharedImage frame_;  // NR's target in the frame's format, COMMON between lists; its block is the add-on's frame
  std::optional<Failure> failure_;
};

// D3D9Ex, and D3D11 without fences (design §2.3): the add-on's legacy shared texture, opened here by its KMT handle. The
// add-on's event query orders its writes before RUN, and this side's reply is set when its GPU work is done. The mask and the motion
// are shared textures of the add-on's too (`Share` names their handles).
class KmtTransport final : public CpuOrderedTransport {
 public:
  explicit KmtTransport(bridge::D3D12Side& side) : CpuOrderedTransport(side) {}
  ~KmtTransport() override {
    Release();
    RetireImages();
  }

  addon::TargetInfo Describe(const ipc::Target& target, bool enabled, bool running, ipc::Handles* /*handles*/) override {
    side_.FreeFinished();
    addon::TargetInfo info = {.color_space = static_cast<color::ColorSpace>(target.color_space),
                              .size = {target.width, target.height},
                              .format = static_cast<DXGI_FORMAT>(target.dxgi_format)};
    problem_ = std::string(ipc::TextOf(target.problem));
    if (!enabled) {
      failure_.reset();  // the user's own off: a share failure is forgotten only then, and stays while NR is merely off
    }
    if (!running || !latch_.empty()) {
      RetireImages();
    }
    // By size and format, as BLOCK and FENCED: the add-on makes a new texture (a new handle and generation) whenever it lost the
    // old one, so keying on the texture would retry, and log, at every present the add-on's objects come and go.
    const bool failed_here = (failure_ && failure_->size == info.size && failure_->format == info.format);
    // NR is off, the transport stopped, the add-on reports a problem or has no shared texture (yet: NR waits for one), or a
    // texture of this size could not be opened. Reported while NR is off too, so the Session stays OFF instead of loading only to
    // unload.
    if (!running || !latch_.empty() || !problem_.empty() || target.kmt_handle == 0u || failed_here) {
      Release();
      info.problem = FirstText({latch_, problem_, (failed_here ? std::string_view(failure_->text) : std::string_view())});
      return info;
    }
    if (!opened_ || handle_ != target.kmt_handle || generation_ != target.kmt_generation) {
      Release();
      const HRESULT result = side_.Device()->OpenSharedHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(target.kmt_handle)),
                                                              IID_PPV_ARGS(&opened_));
      if (FAILED(result)) {
        failure_ = Failure{.size = info.size,
                           .format = info.format,
                           .text = std::format("The back buffer could not be shared with Uplift's helper (DXGI_FORMAT {}, HRESULT {:#010x})",
                                               target.dxgi_format, static_cast<uint32_t>(result))};
        nr::Log(nr::LogLevel::WARN, failure_->text);
        info.problem = failure_->text;
        return info;
      }
      failure_.reset();
      handle_ = target.kmt_handle;
      generation_ = target.kmt_generation;
    }
    info.resource = opened_.Get();
    return info;
  }
  void Detach() override {
    Release();
    RetireImages();
    failure_.reset();
  }
  // The textures are the add-on's allocations; opening them here adds no video memory of its own.
  [[nodiscard]] uint64_t SurfaceBytes() const override { return 0u; }
  [[nodiscard]] std::string Line() const override { return "shared texture, CPU-ordered"; }

 protected:
  [[nodiscard]] ID3D12Resource* Target() const override { return opened_.Get(); }
  // The add-on's shared render-target texture, opened by the global handle it names.
  std::string Provide(const ipc::Share& share, std::string_view what, uint32_t /*bytes_per_pixel*/, SharedImage* image,
                      BlockHandle* /*handle*/) override {
    if (share.kmt_handle == 0u) return std::format("The {} names no shared texture", what);
    const HRESULT result = side_.Device()->OpenSharedHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(share.kmt_handle)),
                                                            IID_PPV_ARGS(&image->texture));
    if (FAILED(result)) {
      return std::format("The {} could not be shared with Uplift's helper (DXGI_FORMAT {}, HRESULT {:#010x})", what,
                         static_cast<int>(share.dxgi_format), static_cast<uint32_t>(result));
    }
    image->size = {share.width, share.height};
    image->format = static_cast<DXGI_FORMAT>(share.dxgi_format);
    return {};
  }

 private:
  struct Failure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string text;
  };

  void Release() {
    side_.Retire(std::move(opened_), nullptr, last_use_);  // the queue may still read it
    opened_.Reset();
    handle_ = 0u;
    generation_ = 0u;
  }

  ComPtr<ID3D12Resource> opened_;
  uint64_t handle_ = 0u;
  uint32_t generation_ = 0u;
  std::optional<Failure> failure_;
};

// D3D11.4 (design §2.4): Plan 7's NT textures and two shared fences, split across the process boundary. The add-on's side
// runs RunBridgedFrame; this side is its D3D12 half: Wait(to12, in), NR on the shared colour, Signal(progress) and
// Signal(to11, out) -- always, once the queue has waited, even when the ring was busy or the recording failed (R17: the
// add-on queues its D3D11 wait only after this reply says the signal was submitted).
// Plan 11 (Vulkan design §5): `cpu_ordered` (SHARED_CPU) is the same textures with no fences: no handle is made or handed out, the queue does not
// wait for the add-on, and the reply is set when the GPU has passed this recording's `progress` signal (BLOCK's and KMT's reply-at-completion),
// because the add-on orders its own side on the CPU (it waited for its copy in before RUN, and waits for the reply before it copies out).
class FencedTransport final : public Transport {
 public:
  FencedTransport(bridge::D3D12Side& side, bool cpu_ordered, std::string* error)
      : side_(side), cpu_ordered_(cpu_ordered), progress_base_(side.LastSignalled()) {
    if (cpu_ordered_) return;
    if (FAILED(side_.CreateSharedFence(&to12_, &to12_handle_)) || FAILED(side_.CreateSharedFence(&to11_, &to11_handle_))) {
      *error = "the shared fences could not be created";
      to12_.Reset();
      to11_.Reset();
    }
  }
  ~FencedTransport() override {
    // Nothing the queue may still read is freed under it, and no wait stays queued on a fence about to go (Detach).
    Detach();
    // The add-on pulls the fence handles with the first FRAME reply; one it never took is closed here.
    for (const HANDLE handle : {to12_handle_, to11_handle_}) {
      if (handle != nullptr) CloseHandle(handle);
    }
  }
  [[nodiscard]] bool Ready() const { return cpu_ordered_ || (to12_ != nullptr && to11_ != nullptr); }

  addon::TargetInfo Describe(const ipc::Target& target, bool enabled, bool running, ipc::Handles* handles) override {
    side_.FreeFinished();
    // The fences' handles go out once, with the first FRAME reply.
    if (to12_handle_ != nullptr || to11_handle_ != nullptr) {
      handles->to12 = reinterpret_cast<uintptr_t>(std::exchange(to12_handle_, nullptr));
      handles->to11 = reinterpret_cast<uintptr_t>(std::exchange(to11_handle_, nullptr));
    }
    const nr::Size size = {target.width, target.height};
    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(target.dxgi_format);
    addon::TargetInfo info = {.color_space = static_cast<color::ColorSpace>(target.color_space), .size = size, .format = format};
    problem_ = std::string(ipc::TextOf(target.problem));
    if (!enabled) {
      failure_.reset();  // the user's own off: a share failure is forgotten only then, and stays while NR is merely off
    }
    const std::optional<color::FormatInfo> described = color::DescribeFormat(format);
    const bool failed_here = (failure_ && failure_->size == size && failure_->format == format);
    const bool idle = (!running || !latch_.empty());
    if (idle) {
      Retire(&mask_);
      Retire(&motion_);
    }
    if (idle || !problem_.empty() || !described || size.Empty() || failed_here) {
      // NR is off, the transport stopped, or this is an image NR cannot take (DeviceContext says why): nothing is kept for it.
      // The problem is reported while NR is off too, so the Session stays OFF instead of loading only to unload again; a
      // share failure is not retried for this size and format until NR is re-enabled.
      Retire(&color_);
      info.problem = FirstText({latch_, problem_, (failed_here ? std::string_view(failure_->text) : std::string_view())});
      return info;
    }
    if (!color_.resource || color_.size != size || color_.format != format) {
      Retire(&color_);
      HANDLE handle = nullptr;
      // Plan 7's flags (design §9 g): a committed texture on a shared heap, render-target-capable.
      const HRESULT result =
          side_.CreateShared(size, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, L"Uplift shared back buffer", &color_.resource, &handle);
      if (FAILED(result)) {
        failure_ = Failure{.size = size,
                           .format = format,
                           .text = std::format("The back buffer could not be shared with Uplift's helper (DXGI_FORMAT {}, HRESULT {:#010x})",
                                               static_cast<int>(format), static_cast<uint32_t>(result))};
        nr::Log(nr::LogLevel::WARN, failure_->text);
        info.problem = failure_->text;
        return info;
      }
      failure_.reset();
      color_.size = size;
      color_.format = format;
      color_.bytes = size.Pixels() * described->bytes_per_pixel;
      handles->color = reinterpret_cast<uintptr_t>(handle);
      handles->color_bytes = AllocationBytes(side_.Device(), color_.resource.Get());
    }
    info.resource = color_.resource.Get();
    return info;
  }

  void Run(const ipc::Run& run, addon::DeviceContext& context, ipc::Reply* reply, uint64_t* complete_at) override {
    waited12_ = run.in;  // for Stop, whether or not the queue took it
    reply->ok = 1u;
    // 1.1.2 (a player's freeze): a recording without LaunchPad's motion keeps the shared motion texture (RG16F, +32 MiB at 4K), as the add-on keeps
    // its share; it goes when NR stops (Describe's idle path, Detach) or at a new size (Share).
    if (!cpu_ordered_ && FAILED(side_.Queue()->Wait(to12_.Get(), run.in))) return;  // nothing queued: the add-on stops, with no signal owed
    reply->waited = 1u;
    Recorded recorded;
    const bool usable = (color_.resource != nullptr && latch_.empty());
    ID3D12Resource* const mask = ((run.mask_fresh != 0u && mask_.resource) ? mask_.resource.Get() : nullptr);
    ID3D12Resource* const motion = ((run.motion != 0u && motion_.resource) ? motion_.resource.Get() : nullptr);
    if (usable) {
      const bridge::BridgeTargets targets = {.color = color_.resource.Get(), .mask = mask, .motion = motion};
      recorded = RecordRun(
          side_, context, static_cast<addon::TriggerPoint>(run.point), targets,
          [&targets](ID3D12GraphicsCommandList* list) {
            if (targets.mask != nullptr) {
              bridge::Transition(list, targets.mask, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            }
            if (targets.motion != nullptr) {
              bridge::Transition(list, targets.motion, D3D12_RESOURCE_STATE_COMMON, bridge::SHADER_READ);
            }
          },
          [&targets](ID3D12GraphicsCommandList* list, bool /*wrote*/) {
            if (targets.motion != nullptr) {
              bridge::Transition(list, targets.motion, bridge::SHADER_READ, D3D12_RESOURCE_STATE_COMMON);
            }
            if (targets.mask != nullptr) {
              bridge::Transition(list, targets.mask, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
          });
    }
    reply->busy = recorded.busy ? 1u : 0u;
    if (cpu_ordered_) {
      // BLOCK's and KMT's ending: a reply that is final now when nothing was submitted, else one the runtime sets once `progress` reaches
      // `value`. `progress` only grows, whatever transport ran before this one.
      if (!recorded.submitted) return;
      reply->submitted = 1u;
      const uint64_t value = side_.LastSignalled() + 1u;
      const bool signalled = side_.SignalProgress(value);
      color_.last_use = value;
      if (mask != nullptr) {
        mask_.last_use = value;
      }
      if (motion != nullptr) {
        motion_.last_use = value;
      }
      reply->signalled = signalled ? 1u : 0u;
      if (!signalled) return;  // nothing would tell the add-on when the GPU is done: an immediate reply that says NR did not write
      reply->wrote = recorded.wrote ? 1u : 0u;
      *complete_at = value;
      return;
    }
    // Always, once the queue waited: `out` is the only value the add-on's D3D11 side ever waits for. progress first: when
    // the queue refuses it, to11 is never signalled either.
    const uint64_t progress_value = progress_base_ + run.out;
    const bool progress = side_.SignalProgress(progress_value);
    const bool to11_signalled = SUCCEEDED(side_.Queue()->Signal(to11_.Get(), run.out));
    reply->signalled = (progress && to11_signalled) ? 1u : 0u;
    if (recorded.submitted) {
      // Held until progress passes it, even when the signal above failed (Plan 7 final review C-1).
      color_.last_use = progress_value;
      if (mask != nullptr) {
        mask_.last_use = progress_value;
      }
      if (motion != nullptr) {
        motion_.last_use = progress_value;
      }
    }
    reply->submitted = recorded.submitted ? 1u : 0u;
    reply->wrote = (recorded.submitted && recorded.wrote) ? 1u : 0u;
  }

  void ReleaseMask() override { Retire(&mask_); }

  void FenceValues(uint64_t* to12, uint64_t* to11) const override {
    *to12 = (to12_ != nullptr ? to12_->GetCompletedValue() : 0u);
    *to11 = (to11_ != nullptr ? to11_->GetCompletedValue() : 0u);
  }

  void Share(ipc::RequestKind kind, const ipc::Share& share, ipc::Reply* reply) override {
    const bool is_mask = (kind == ipc::RequestKind::SHARE_MASK);
    Surface& surface = (is_mask ? mask_ : motion_);
    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(share.dxgi_format);
    const nr::Size size = {share.width, share.height};
    // Plan 7 §9 g: D3D11 opens a D3D12 shared texture only when it is render-target-capable, and outside R8, R16 and the
    // display formats only when it is also simultaneous-access.
    const std::optional<color::MaskFormatInfo> mask_format = (is_mask ? color::DescribeMaskFormat(format) : std::nullopt);
    if (!latch_.empty() || size.Empty() || (is_mask ? !mask_format : format != DXGI_FORMAT_R16G16_FLOAT)) {
      reply->ok = 0u;
      ipc::CopyText(reply->error, latch_.empty() ? std::string_view("this image cannot be shared") : std::string_view(latch_));
      return;
    }
    Retire(&surface);
    HANDLE handle = nullptr;
    const HRESULT result = side_.CreateShared(size, format,
                                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
                                              (is_mask ? L"Uplift shared mask" : L"Uplift shared motion"), &surface.resource, &handle);
    if (FAILED(result)) {
      reply->ok = 0u;
      ipc::CopyText(reply->error, std::format("HRESULT {:#010x}", static_cast<uint32_t>(result)));
      return;
    }
    surface.size = size;
    surface.format = format;
    surface.bytes = size.Pixels() * (is_mask ? mask_format->bytes_per_pixel : 4u);
    (is_mask ? reply->handles.mask : reply->handles.motion) = reinterpret_cast<uintptr_t>(handle);
    (is_mask ? reply->handles.mask_bytes : reply->handles.motion_bytes) = AllocationBytes(side_.Device(), surface.resource.Get());
    reply->ok = 1u;
  }

  void Stop(uint64_t waited11, std::string_view reason) override {
    if (latch_.empty()) {
      latch_ = std::string(reason);
      // 1.1.2 (a player's freeze): a removal the add-on's watchdog saw first never reaches DeviceContext::NoteDeviceRemoved, so say here what DRED found
      // (the lists in flight, and a page fault's allocations by name).
      if (FAILED(side_.Device()->GetDeviceRemovedReason())) {
        addon::LogDred(side_.Device());
      }
    }
    ReleaseWaits(waited11);
  }
  void Detach() override {
    // The game's device is going: nothing will signal to12 again, and the queue may be waiting on it (Plan 7 final review I-2).
    ReleaseWaits(0u);
    Retire(&color_);
    Retire(&mask_);
    Retire(&motion_);
    failure_.reset();
  }
  [[nodiscard]] uint64_t SurfaceBytes() const override { return color_.bytes + mask_.bytes + motion_.bytes; }
  [[nodiscard]] std::string Line() const override {
    return cpu_ordered_ ? "shared textures, CPU-ordered" : "shared textures and fences";
  }

 private:
  struct Surface {
    ComPtr<ID3D12Resource> resource;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0u;
    uint64_t last_use = 0u;  // the `progress` value after the newest submitted recording that used it
  };
  struct Failure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string text;
  };

  void Retire(Surface* surface) {
    side_.Retire(std::move(surface->resource), nullptr, surface->last_use);
    *surface = {};
  }
  // Each fence up to the highest value a wait on it was queued for (the add-on's D3D11 side for to11, this queue for to12);
  // nothing waits beyond those. A refused signal (a removed device) needs no retry: that fence already reads UINT64_MAX.
  void ReleaseWaits(uint64_t waited11) {
    if (to11_ != nullptr && to11_->GetCompletedValue() < waited11) {
      to11_->Signal(waited11);
    }
    if (to12_ != nullptr && to12_->GetCompletedValue() < waited12_) {
      to12_->Signal(waited12_);
    }
  }

  bridge::D3D12Side& side_;
  bool cpu_ordered_;
  ComPtr<ID3D12Fence> to12_;  // the add-on's D3D11 signals, this queue waits
  ComPtr<ID3D12Fence> to11_;  // this queue signals, the add-on's D3D11 waits
  HANDLE to12_handle_ = nullptr;
  HANDLE to11_handle_ = nullptr;
  Surface color_;
  Surface mask_;
  Surface motion_;
  std::optional<Failure> failure_;
  std::string latch_;
  std::string problem_;
  uint64_t waited12_ = 0u;  // the highest to12 value this queue was asked to wait for
  // The add-on numbers its hand-offs from 1; `progress` must only grow, so this transport's values sit above what the side
  // signalled before it existed.
  uint64_t progress_base_ = 0u;
};

}  // namespace

void Transport::Share(ipc::RequestKind /*kind*/, const ipc::Share& /*share*/, ipc::Reply* reply) {
  reply->ok = 0u;
  ipc::CopyText(reply->error, "this transport shares no mask or motion");
}

std::unique_ptr<Transport> MakeTransport(ipc::Transport kind, bridge::D3D12Side& side, std::string* error) {
  switch (kind) {
    case ipc::Transport::BLOCK:
      return std::make_unique<BlockTransport>(side);
    case ipc::Transport::KMT:
      return std::make_unique<KmtTransport>(side);
    case ipc::Transport::FENCED:
    case ipc::Transport::SHARED_CPU: {
      auto transport = std::make_unique<FencedTransport>(side, kind == ipc::Transport::SHARED_CPU, error);
      return transport->Ready() ? std::move(transport) : nullptr;
    }
    case ipc::Transport::NONE:
      break;
  }
  *error = "no transport was asked for";
  return nullptr;
}

}  // namespace uplift::helper
