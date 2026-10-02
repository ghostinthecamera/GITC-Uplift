#include "client/d3d9_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <iterator>
#include <span>
#include <utility>

#include "addon/environment.hpp"
#include "ipc/control_block.hpp"
#include "nr/log.hpp"
#include "nr/types.hpp"

namespace uplift::client {
namespace {

using Microsoft::WRL::ComPtr;

constexpr auto GPU_CAP = std::chrono::seconds(2);  // the design's R29 cap on the game thread's waits
constexpr double MIB = 1024.0 * 1024.0;

using FormatMap = D3D9FormatMap;

// Design §2.8: what each back-buffer format crosses as.
constexpr FormatMap FORMATS[] = {
    {D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, 4u},
    {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, 4u},
    {D3DFMT_A2B10G10R10, D3DFMT_A2B10G10R10, DXGI_FORMAT_R10G10B10A2_UNORM, 4u},
    {D3DFMT_A2R10G10B10, D3DFMT_A2B10G10R10, DXGI_FORMAT_R10G10B10A2_UNORM, 4u},
    {D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u},
};
// Design §4.1: UPLIFT_MASK. ReShade makes an R8 texture X8R8G8B8 on D3D9 (its own override), so the mask crosses as 4-byte BGRA and NR
// reads the red channel. Every DXGI format here is one color::DescribeMaskFormat accepts. BLOCK reads back in the source's own format.
constexpr FormatMap MASK_FORMATS[] = {
    {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, 4u},
    {D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, 4u},
    {D3DFMT_R16F, D3DFMT_R16F, DXGI_FORMAT_R16_FLOAT, 2u},
    {D3DFMT_R32F, D3DFMT_R32F, DXGI_FORMAT_R32_FLOAT, 4u},
    {D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u},
};
// LaunchPad's UPLIFT_MV (RG16F on ReShade's D3D9 backend is G16R16F).
constexpr FormatMap MOTION_FORMAT = {D3DFMT_G16R16F, D3DFMT_G16R16F, DXGI_FORMAT_R16G16_FLOAT, 4u};

const FormatMap* FindFormat(std::span<const FormatMap> table, D3DFORMAT format) {
  const auto found = std::ranges::find(table, format, &FormatMap::source);
  return (found == table.end() ? nullptr : &*found);
}

void CopyRows(void* to, size_t to_pitch, const void* from, size_t from_pitch, size_t row_bytes, uint32_t rows) {
  for (uint32_t row = 0u; row < rows; ++row) {
    std::memcpy(static_cast<std::byte*>(to) + row * to_pitch, static_cast<const std::byte*>(from) + row * from_pitch, row_bytes);
  }
}

// The frame copy that could not be made for lack of memory: a 32-bit game runs out of address space first (its Plan 9 text, byte for
// byte), a 64-bit one out of memory. `failed_call` names what failed.
std::string NotEnoughMemory(std::string_view failed_call, uint32_t code) {
  const std::string_view lacking = (IS_32_BIT ? "address space in this 32-bit game" : "memory");
  return std::format("Not enough {} for the frame copy ({} {:#010x})", lacking, failed_call, code);
}

// The helper's block, given as `mapping` (which this closes: the view keeps the section) and mapped: "" on success with `*view` set,
// else why not. `row_bytes` and `rows` are the image's, which the block's pitch and size must cover; `what` names it ("frame", "mask").
std::string MapBlock(std::string_view what, HANDLE mapping, uint64_t bytes, uint32_t pitch, uint64_t row_bytes, uint32_t rows,
                     std::byte** view) {
  std::string problem;
  if (pitch < row_bytes || uint64_t{pitch} * rows > bytes) {
    problem = std::format("The {} block the helper made does not fit the {}", what, what);
  } else {
    *view = static_cast<std::byte*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0u, 0u, static_cast<SIZE_T>(bytes)));
    if (*view == nullptr) {
      problem = NotEnoughMemory("MapViewOfFile", static_cast<uint32_t>(GetLastError()));
    }
  }
  CloseHandle(mapping);
  return problem;
}

}  // namespace

std::unique_ptr<D3D9Client> D3D9Client::Create(IDirect3DDevice9* device, std::string* error) {
  const auto fail = [error](std::string text) {
    *error = std::move(text);
    return std::unique_ptr<D3D9Client>();
  };
  D3DDEVICE_CREATION_PARAMETERS parameters = {};
  ComPtr<IDirect3D9> factory;
  D3DADAPTER_IDENTIFIER9 identifier = {};
  if (FAILED(device->GetCreationParameters(&parameters)) || FAILED(device->GetDirect3D(&factory))
      || FAILED(factory->GetAdapterIdentifier(parameters.AdapterOrdinal, 0u, &identifier))) {
    return fail("the game's Direct3D 9 adapter could not be read");
  }
  // WARP and other vendors are refused, as on D3D12 (addon::IsNvidiaDevice); cross-adapter sharing is out of scope.
  if (identifier.VendorId != addon::NVIDIA_VENDOR_ID) {
    return fail("Uplift needs an NVIDIA GPU; this game renders on another adapter");
  }
  std::unique_ptr<D3D9Client> client(new D3D9Client());
  client->device_ = device;
  // The LUID: IDirect3D9 has none, so a temporary IDirect3D9Ex from the system d3d9.dll (full path: the game folder's d3d9.dll
  // can be a proxy such as ReShade or FF13Fix) gives the same adapter's. The module stays loaded: the game has it anyway.
  wchar_t system_directory[MAX_PATH] = {};
  const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
  const HMODULE system_d3d9 =
      (length > 0u && length < MAX_PATH ? LoadLibraryW((std::wstring(system_directory, length) + L"\\d3d9.dll").c_str()) : nullptr);
  using CreateExFunction = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
  const auto create_ex =
      (system_d3d9 != nullptr ? reinterpret_cast<CreateExFunction>(GetProcAddress(system_d3d9, "Direct3DCreate9Ex")) : nullptr);
  ComPtr<IDirect3D9Ex> factory_ex;
  if (create_ex == nullptr || FAILED(create_ex(D3D_SDK_VERSION, &factory_ex))
      || FAILED(factory_ex->GetAdapterLUID(parameters.AdapterOrdinal, &client->luid_))) {
    return fail("the game's adapter could not be identified (Direct3D 9Ex from the system d3d9.dll)");
  }
  ComPtr<IDirect3DDevice9Ex> device_ex;
  client->ex_ = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device_ex)));
  return client;
}

D3D9Client::~D3D9Client() {
  ReleaseAll();
}

void D3D9Client::ReleaseGameObjects() {
  if (view_ != nullptr) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  view_bytes_ = 0u;
  view_pitch_ = 0u;
  twin_.Reset();
  sysmem_.Reset();
  plain_.Reset();
  shared_surface_.Reset();
  shared_texture_.Reset();
  kmt_handle_ = nullptr;
  query_.Reset();
  ReleaseImage(&mask_);  // D3DPOOL_DEFAULT twins too: a live one makes the game's Reset fail
  ReleaseImage(&motion_);
  gpu_pending_ = false;
  width_ = 0u;
  height_ = 0u;
  source_format_ = D3DFMT_UNKNOWN;
  twin_format_ = D3DFMT_UNKNOWN;
  bytes_per_pixel_ = 0u;
  vram_bytes_ = 0u;
  floor_probed_ = false;
}

void D3D9Client::ReleaseAll() {
  ReleaseGameObjects();
  failure_.reset();
  mask_failure_.reset();
  motion_failure_.reset();
  in_flight_ = false;
}

void D3D9Client::ReleaseImage(Image* image) {
  if (image->view != nullptr) {
    UnmapViewOfFile(image->view);
  }
  *image = {};
}

void D3D9Client::ReleaseMask() {
  ReleaseImage(&mask_);
}

bool D3D9Client::WaitForGpu() {
  if (!query_ && FAILED(device_->CreateQuery(D3DQUERYTYPE_EVENT, &query_))) return false;
  if (FAILED(query_->Issue(D3DISSUE_END))) return false;
  const auto deadline = std::chrono::steady_clock::now() + GPU_CAP;
  for (;;) {
    const HRESULT result = query_->GetData(nullptr, 0u, D3DGETDATA_FLUSH);
    if (result == S_OK) return true;
    if (result != S_FALSE) return false;
    if (std::chrono::steady_clock::now() >= deadline) {
      gpu_pending_ = true;
      return false;
    }
    Sleep(0u);
  }
}

ipc::Target D3D9Client::Describe(IDirect3DSurface9* source, bool running, bool at_present) {
  ipc::Target target;
  D3DSURFACE_DESC description = {};
  if (source == nullptr || FAILED(source->GetDesc(&description))) {
    ReleaseGameObjects();
    ipc::CopyText(target.problem, "The back buffer could not be read");
    return target;
  }
  target.width = description.Width;
  target.height = description.Height;
  const FormatMap* const map = FindFormat(FORMATS, description.Format);
  target.dxgi_format = (map != nullptr ? static_cast<uint32_t>(map->dxgi) : 0u);
  std::string problem;
  if (map == nullptr) {
    problem = std::format("Unsupported back-buffer format (D3DFMT {})", static_cast<int>(description.Format));
  } else if (at_present && description.MultiSampleType != D3DMULTISAMPLE_NONE) {
    // GetRenderTargetData refuses these, and ReShade resolves them only for the effects: at the marker NR reads that copy.
    problem = "Multisampled back buffers need the Uplift technique on Direct3D 9: NR then runs on ReShade's resolved copy";
  }
  const bool usable = (running && problem.empty());
  if (!usable) {
    ReleaseGameObjects();
  }
  if (!running) {
    failure_.reset();  // a re-enable retries what failed
    mask_failure_.reset();
    motion_failure_.reset();
  }
  if (usable && failure_ && failure_->width == description.Width && failure_->height == description.Height
      && failure_->format == description.Format) {
    problem = failure_->text;
  } else if (usable && (width_ != description.Width || height_ != description.Height || twin_format_ != map->twin || (ex_ ? shared_surface_ == nullptr : sysmem_ == nullptr))) {
    ReleaseGameObjects();  // objects made for another frame: a new block comes for the new size
    HRESULT result = S_OK;
    if (ex_) {
      result = device_->CreateTexture(description.Width, description.Height, 1u, D3DUSAGE_RENDERTARGET, map->twin, D3DPOOL_DEFAULT,
                                      &shared_texture_, &kmt_handle_);
      if (SUCCEEDED(result)) {
        result = shared_texture_->GetSurfaceLevel(0u, &shared_surface_);
      }
    } else {
      if (map->source != map->twin) {
        result = device_->CreateRenderTarget(description.Width, description.Height, map->twin, D3DMULTISAMPLE_NONE, 0u, FALSE, &twin_,
                                             nullptr);
      }
      if (SUCCEEDED(result)) {
        result = device_->CreateOffscreenPlainSurface(description.Width, description.Height, map->twin, D3DPOOL_SYSTEMMEM, &sysmem_,
                                                      nullptr);
      }
      if (SUCCEEDED(result)) {
        result = device_->CreateOffscreenPlainSurface(description.Width, description.Height, map->twin, D3DPOOL_DEFAULT, &plain_, nullptr);
      }
    }
    if (FAILED(result)) {
      ReleaseGameObjects();
      if (result == D3DERR_DEVICELOST || result == D3DERR_DEVICENOTRESET) {
        // An Alt-Tab out of exclusive fullscreen: every creation fails until the game resets the device. Not a failure of this
        // size (nothing is cached, nothing is logged per frame): the next present tries again.
        problem = "The Direct3D 9 device is lost; NR resumes once the game resets it";
      } else {
        const bool memory = (result == E_OUTOFMEMORY || result == D3DERR_OUTOFVIDEOMEMORY);
        problem = (memory ? NotEnoughMemory("Direct3D 9 error", static_cast<uint32_t>(result))
                          : std::format("The back buffer cannot be copied for Uplift's helper (D3DFMT {}, HRESULT {:#010x})",
                                        static_cast<int>(description.Format), static_cast<uint32_t>(result)));
        failure_ = Failure{.width = description.Width, .height = description.Height, .format = description.Format, .text = problem};
        nr::Log(nr::LogLevel::WARN, problem);
      }
    } else {
      width_ = description.Width;
      height_ = description.Height;
      source_format_ = description.Format;
      twin_format_ = map->twin;
      bytes_per_pixel_ = map->bytes_per_pixel;
      const uint64_t frame_bytes = uint64_t{width_} * height_ * bytes_per_pixel_;
      // VRAM only: the SYSTEMMEM surface and the block are system memory.
      vram_bytes_ = (ex_ ? frame_bytes : (twin_ ? frame_bytes : 0u) + frame_bytes);
      if (ex_) {
        ++kmt_generation_;
      }
    }
  }
  if (shared_texture_ != nullptr && problem.empty()) {
    target.kmt_handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(kmt_handle_));
    target.kmt_generation = kmt_generation_;
  }
  target.block_mapped = (view_ != nullptr ? 1u : 0u);
  ipc::CopyText(target.problem, problem);
  return target;
}

void D3D9Client::Apply(const ipc::Reply& reply, NrLink& link) {
  in_flight_ = false;  // a FRAME reply means every earlier request has replied: the helper is done with the block
  HANDLE mapping = nullptr;
  const bool pulled = (reply.handles.block != 0u && link.Pull(reply.handles.block, &mapping));
  // Every NT handle in a reply is taken exactly once, so the helper's table never keeps one. The mask's and the motion's are the ones
  // a SHARE reply that landed late handed out (its mapping on BLOCK): this client forgets that ask below, and asks again.
  for (const uint64_t remote : {reply.handles.color, reply.handles.mask, reply.handles.motion, reply.handles.to12, reply.handles.to11}) {
    HANDLE unused = nullptr;
    if (remote != 0u && link.Pull(remote, &unused)) {
      CloseHandle(unused);
    }
  }
  for (Image* const image : {&mask_, &motion_}) {
    if (image->late) {
      ReleaseImage(image);
    }
  }
  if (reply.running == 0u) {
    ReleaseGameObjects();
  }
  if (!pulled) return;
  if (ex_ || width_ == 0u) {
    CloseHandle(mapping);  // a block for a frame this client no longer has
    return;
  }
  if (view_ != nullptr) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  const std::string problem = MapBlock("frame", mapping, reply.handles.block_bytes, reply.handles.block_row_pitch,
                                       uint64_t{width_} * bytes_per_pixel_, height_, &view_);
  if (!problem.empty()) {
    // Not retried for this size and format until NR is re-enabled; the next FRAME says so, and the helper drops its heap.
    failure_ = Failure{.width = width_, .height = height_, .format = source_format_, .text = problem};
    nr::Log(nr::LogLevel::WARN, problem);
    ReleaseGameObjects();
    return;
  }
  view_bytes_ = reply.handles.block_bytes;
  view_pitch_ = reply.handles.block_row_pitch;
}

D3D9Client::Ensure D3D9Client::EnsureImage(Image* image, ipc::RequestKind kind, const FormatMap& map, uint32_t width, uint32_t height,
                                           std::optional<Failure>* failure, NrLink& link) {
  if (image->Ready() && image->width == width && image->height == height && image->source_format == map.source) return Ensure::READY;
  ReleaseImage(image);
  if (*failure && (*failure)->width == width && (*failure)->height == height && (*failure)->format == map.source) return Ensure::FAILED;
  const bool is_mask = (kind == ipc::RequestKind::SHARE_MASK);
  const std::string_view name = (is_mask ? "UPLIFT_MASK" : "UPLIFT_MV");
  // KMT: the add-on's own shared render-target twin, which the helper opens by its handle. BLOCK: the SYSTEMMEM surface the readback
  // fills (the source's format); the helper makes the block.
  ipc::Share share = {.width = width, .height = height, .dxgi_format = static_cast<uint32_t>(map.dxgi)};
  HANDLE kmt_handle = nullptr;
  HRESULT result = S_OK;
  if (ex_) {
    result = device_->CreateTexture(width, height, 1u, D3DUSAGE_RENDERTARGET, map.twin, D3DPOOL_DEFAULT, &image->texture, &kmt_handle);
    if (SUCCEEDED(result)) {
      result = image->texture->GetSurfaceLevel(0u, &image->surface);
    }
    share.kmt_handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(kmt_handle));
  } else {
    result = device_->CreateOffscreenPlainSurface(width, height, map.source, D3DPOOL_SYSTEMMEM, &image->sysmem, nullptr);
  }
  if (result == D3DERR_DEVICELOST || result == D3DERR_DEVICENOTRESET) {
    ReleaseImage(image);  // an Alt-Tab out of exclusive fullscreen: not a failure of this size, so nothing is cached; the next frame tries again
    return Ensure::FAILED;
  }
  std::string problem;
  if (FAILED(result)) {
    const bool memory = (result == E_OUTOFMEMORY || result == D3DERR_OUTOFVIDEOMEMORY);
    problem = (memory ? NotEnoughMemory("Direct3D 9 error", static_cast<uint32_t>(result))
                      : std::format("{} cannot be copied for Uplift's helper (D3DFMT {}, HRESULT {:#010x})", name,
                                    static_cast<int>(map.source), static_cast<uint32_t>(result)));
  } else {
    const std::optional<ipc::Reply> reply = link.Share(kind, share);
    if (!reply) {
      // Late: the helper may still be making it. Nothing is touched until a FRAME reply says every request has replied, and Apply
      // then closes the mapping that reply may carry and forgets the ask (key decision h); the next copy asks again.
      image->late = true;
      in_flight_ = true;
      return Ensure::LATE;
    }
    if (reply->ok == 0u) {
      problem = std::format("{} could not be shared with Uplift's helper (D3DFMT {}: {})", name, static_cast<int>(map.source),
                            ipc::TextOf(reply->error));
    } else if (!ex_) {
      HANDLE mapping = nullptr;
      const uint64_t remote = (is_mask ? reply->handles.mask : reply->handles.motion);
      if (remote == 0u || !link.Pull(remote, &mapping)) {
        problem = std::format("{} could not be shared with Uplift's helper (D3DFMT {}: no block came)", name, static_cast<int>(map.source));
      } else {
        image->pitch = (is_mask ? reply->handles.mask_row_pitch : reply->handles.motion_row_pitch);
        problem = MapBlock((is_mask ? "mask" : "motion"), mapping, (is_mask ? reply->handles.mask_bytes : reply->handles.motion_bytes),
                           image->pitch, uint64_t{width} * map.bytes_per_pixel, height, &image->view);
      }
    }
  }
  if (!problem.empty()) {
    // Logged once: not retried for this size and format until NR is re-enabled (Plan 7's Minor 3).
    *failure = Failure{.width = width, .height = height, .format = map.source, .text = problem};
    nr::Log(nr::LogLevel::WARN, problem);
    ReleaseImage(image);
    return Ensure::FAILED;
  }
  image->width = width;
  image->height = height;
  image->source_format = map.source;
  image->bytes_per_pixel = map.bytes_per_pixel;
  image->vram_bytes = (ex_ ? uint64_t{width} * height * map.bytes_per_pixel : 0u);  // the KMT twin; the block and the SYSTEMMEM surface are system memory
  return Ensure::READY;
}

MaskCopy D3D9Client::CopyMask(IDirect3DTexture9* mask, NrLink& link) {
  if (in_flight_) return {.busy = true};  // the helper may still read the shared twin (KMT) or the mask block (BLOCK)
  D3DSURFACE_DESC description = {};
  const FormatMap* const map = ((mask != nullptr && mask->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(mask->GetLevelDesc(0u, &description)))
                                    ? FindFormat(MASK_FORMATS, description.Format)
                                    : nullptr);
  if (map == nullptr) {
    ReleaseMask();
    return {.unsupported = true, .d3d9_format = static_cast<uint32_t>(description.Format)};
  }
  const Ensure ensured = EnsureImage(&mask_, ipc::RequestKind::SHARE_MASK, *map, description.Width, description.Height, &mask_failure_, link);
  if (ensured == Ensure::LATE) return {.busy = true};
  Microsoft::WRL::ComPtr<IDirect3DSurface9> source;
  if (ensured != Ensure::READY || FAILED(mask->GetSurfaceLevel(0u, &source))) return {};
  // KMT: StretchRect into the twin (X8 becomes A8, exactly); the next RUN's event query, issued later on this device, covers it. BLOCK:
  // an asynchronous readback, collected by the next Run after its own query.
  if (ex_) {
    if (FAILED(device_->StretchRect(source.Get(), nullptr, mask_.surface.Get(), nullptr, D3DTEXF_NONE))) return {};
    mask_.fresh = true;
  } else {
    if (FAILED(device_->GetRenderTargetData(source.Get(), mask_.sysmem.Get()))) return {};
    mask_.pending = true;
  }
  return {.size = nr::Size{description.Width, description.Height}};
}

bool D3D9Client::Run(IDirect3DSurface9* source, IDirect3DTexture9* motion, addon::TriggerPoint point, NrLink& link) {
  run_sent_ = false;
  if (in_flight_ || width_ == 0u || source == nullptr) return false;
  D3DSURFACE_DESC description = {};
  if (FAILED(source->GetDesc(&description))) return false;
  const FormatMap* const map = FindFormat(FORMATS, description.Format);
  // The marker's source can be ReShade's resolved copy: any format that crosses as this frame's twin does.
  if (map == nullptr || map->twin != twin_format_ || description.Width != width_ || description.Height != height_
      || description.MultiSampleType != D3DMULTISAMPLE_NONE) {
    return false;
  }
  ipc::Run run = {.point = static_cast<uint32_t>(point)};
  if (!nr::MeetsNrFloor({width_, height_})) {
    // Below NR's floor nothing can come back: no copy, and after the first RUN at this size (whose reply's status says why) no RUN at all,
    // so the frame costs the game nothing.
    if (floor_probed_) return false;
    ReleaseImage(&motion_);  // this RUN carries `motion` = 0, which retires the helper's copy: keep none here either
    const bool replied = link.Run(run).has_value();
    run_sent_ = link.RunSent();
    floor_probed_ = run_sent_;  // a RUN that was refused (another request outstanding) is tried again
    if (!replied) {
      in_flight_ = true;
    }
    return false;
  }
  if (gpu_pending_) {
    // An earlier frame's 2 s wait ran out, so its copy may still be queued on the game's GPU. No RUN is out for it (the helper is
    // not using the block or the texture), and this frame sends and touches nothing until the query shows the GPU passed it.
    if (query_ != nullptr && query_->GetData(nullptr, 0u, D3DGETDATA_FLUSH) == S_FALSE) return false;
    gpu_pending_ = false;
  }
  // LaunchPad's motion (design §4.3): shared on demand, as the mask. A frame without one, or with one NR cannot take, lets it go; the
  // RUN then carries `motion` = 0, which retires the helper's. A RUN that goes out with `motion` = 0 for any reason (a failed copy too)
  // drops the add-on's image as well, so the next frame shares it again.
  Microsoft::WRL::ComPtr<IDirect3DSurface9> motion_source;
  D3DSURFACE_DESC motion_description = {};
  if (motion != nullptr && motion->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(motion->GetLevelDesc(0u, &motion_description))
      && motion_description.Format == MOTION_FORMAT.source) {
    const Ensure ensured = EnsureImage(&motion_, ipc::RequestKind::SHARE_MOTION, MOTION_FORMAT, motion_description.Width,
                                       motion_description.Height, &motion_failure_, link);
    if (ensured == Ensure::LATE) return false;  // `in_flight_` is set, and nothing has been touched yet
    if (ensured == Ensure::READY && FAILED(motion->GetSurfaceLevel(0u, &motion_source))) {
      motion_source.Reset();
    }
  } else {
    ReleaseImage(&motion_);
  }
  if (ex_) {
    if (shared_surface_ == nullptr || FAILED(device_->StretchRect(source, nullptr, shared_surface_.Get(), nullptr, D3DTEXF_NONE))) {
      return false;
    }
    const bool motion_copied =
        (motion_source != nullptr && SUCCEEDED(device_->StretchRect(motion_source.Get(), nullptr, motion_.surface.Get(), nullptr, D3DTEXF_NONE)));
    if (!WaitForGpu()) return false;  // one wait covers the colour, the motion and the mask copy of the last effects
    run.motion = (motion_copied ? 1u : 0u);
    if (run.motion == 0u) {
      ReleaseImage(&motion_);
    }
    run.mask_fresh = ((mask_.surface != nullptr && mask_.fresh) ? 1u : 0u);
    const std::optional<ipc::Reply> reply = link.Run(run);
    run_sent_ = link.RunSent();
    if (!reply) {
      in_flight_ = true;
      return false;
    }
    if (reply->submitted != 0u && run.mask_fresh != 0u) {
      mask_.fresh = false;
    }
    // `ok = 0` is the helper's catch path: its `wrote` may say NR wrote while the GPU still works on the texture.
    return reply->ok != 0u && reply->wrote != 0u
           && SUCCEEDED(device_->StretchRect(shared_surface_.Get(), nullptr, source, nullptr, D3DTEXF_NONE));
  }
  if (view_ == nullptr || sysmem_ == nullptr || plain_ == nullptr) return false;
  // BLOCK (design §2.4): out through GetRenderTargetData and a locked SYSTEMMEM surface, into the helper's block.
  IDirect3DSurface9* readable = source;
  if (description.Format != twin_format_) {
    if (twin_ == nullptr || FAILED(device_->StretchRect(source, nullptr, twin_.Get(), nullptr, D3DTEXF_NONE))) return false;
    readable = twin_.Get();
  }
  if (FAILED(device_->GetRenderTargetData(readable, sysmem_.Get()))) return false;
  const bool motion_read =
      (motion_source != nullptr && SUCCEEDED(device_->GetRenderTargetData(motion_source.Get(), motion_.sysmem.Get())));
  // The readbacks' wait, capped: LockRect would wait for them without one. It also covers the mask's readback, issued at the last
  // finish_effects, so the lock below does not wait.
  if (!WaitForGpu()) return false;
  const size_t row_bytes = size_t{width_} * bytes_per_pixel_;
  D3DLOCKED_RECT locked = {};
  if (FAILED(sysmem_->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return false;
  CopyRows(view_, view_pitch_, locked.pBits, static_cast<size_t>(locked.Pitch), row_bytes, height_);
  sysmem_->UnlockRect();
  // The motion and the mask go into their blocks before the RUN; one that cannot be read is simply left out of it.
  const auto collect = [](const Image& image) {
    D3DLOCKED_RECT rows = {};
    if (FAILED(image.sysmem->LockRect(&rows, nullptr, D3DLOCK_READONLY))) return false;
    CopyRows(image.view, image.pitch, rows.pBits, static_cast<size_t>(rows.Pitch), size_t{image.width} * image.bytes_per_pixel, image.height);
    image.sysmem->UnlockRect();
    return true;
  };
  run.motion = ((motion_read && collect(motion_)) ? 1u : 0u);
  if (run.motion == 0u) {
    ReleaseImage(&motion_);
  }
  if (mask_.pending && mask_.view != nullptr && collect(mask_)) {
    mask_.pending = false;
    mask_.fresh = true;
  }
  run.mask_fresh = ((mask_.view != nullptr && mask_.fresh) ? 1u : 0u);
  const std::optional<ipc::Reply> reply = link.Run(run);
  run_sent_ = link.RunSent();
  if (!reply) {
    in_flight_ = true;  // the helper may still be reading or writing the block: nothing touches it until a FRAME reply
    return false;
  }
  if (reply->submitted != 0u && run.mask_fresh != 0u) {
    mask_.fresh = false;
  }
  if (reply->ok == 0u || reply->wrote == 0u) return false;  // ok = 0: the helper's catch path, the block may still be written
  if (FAILED(sysmem_->LockRect(&locked, nullptr, 0u))) return false;
  CopyRows(locked.pBits, static_cast<size_t>(locked.Pitch), view_, view_pitch_, row_bytes, height_);
  sysmem_->UnlockRect();
  return SUCCEEDED(device_->UpdateSurface(sysmem_.Get(), nullptr, plain_.Get(), nullptr))
         && SUCCEEDED(device_->StretchRect(plain_.Get(), nullptr, source, nullptr, D3DTEXF_NONE));
}

std::string D3D9Client::Line() const {
  if (ex_) {
    return std::format("Direct3D 9 ({}): NR runs in Uplift's 64-bit helper (Direct3D 9Ex shared texture, {:.1f} MiB)", BITNESS,
                       static_cast<double>(vram_bytes_) / MIB);
  }
  return std::format("Direct3D 9 ({}): NR runs in Uplift's 64-bit helper (frames cross through shared memory, {:.1f} MiB)", BITNESS,
                     static_cast<double>(view_bytes_) / MIB);
}

}  // namespace uplift::client
