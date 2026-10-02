#pragma once

#include <Windows.h>

#include <d3d9.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "addon/frame_trigger.hpp"
#include "client/nr_link.hpp"
#include "ipc/protocol.hpp"

namespace uplift::client {

// Design §2.8 and §4.1: what a D3D9 format crosses as. An X8R8G8B8 or A2R10G10B10 image crosses through a converting StretchRect into its
// twin (both exact both ways, the D3D9 draft's §1.2); the others cross as they are.
struct D3D9FormatMap {
  D3DFORMAT source;
  D3DFORMAT twin;
  DXGI_FORMAT dxgi;
  uint32_t bytes_per_pixel;
};

// Plan 9 (design §2.3, §2.4, §2.8): a D3D9 device's frames to and from NR through an NrLink. KMT on a 9Ex device, BLOCK
// otherwise. Plan 10 (design §4): the same two transports carry UPLIFT_MASK and LaunchPad's UPLIFT_MV. ReShade-free: it takes native
// pointers and calls only StretchRect, GetRenderTargetData, UpdateSurface, LockRect/UnlockRect, CreateTexture, CreateRenderTarget,
// CreateOffscreenPlainSurface, CreateQuery/Issue/GetData and, on ReShade's effect textures, GetLevelDesc and GetSurfaceLevel.
// Its CPU wait is the design's R29 one, capped at 2 s: a frame whose GPU work outlasts the cap is skipped, and the frames after it
// are skipped until that work is done (nothing is latched). Not thread-safe: the add-on's lock. Everything it makes is
// D3DPOOL_DEFAULT (or SYSTEMMEM), released by ReleaseAll() before the device is reset or destroyed, and made again lazily
// afterwards.
class D3D9Client {
 public:
  // nullptr + `error` when the device's adapter is not NVIDIA's or its LUID cannot be read. The adapter is
  // GetCreationParameters().AdapterOrdinal; the LUID comes from a temporary IDirect3D9Ex made through the SYSTEM d3d9.dll by
  // full path (GetSystemDirectoryW), never the game folder's proxy (ReShade, FF13Fix).
  static std::unique_ptr<D3D9Client> Create(IDirect3DDevice9* device, std::string* error);
  ~D3D9Client();
  D3D9Client(const D3D9Client&) = delete;
  D3D9Client& operator=(const D3D9Client&) = delete;

  [[nodiscard]] ipc::Transport Kind() const { return ex_ ? ipc::Transport::KMT : ipc::Transport::BLOCK; }  // KMT on a 9Ex device
  [[nodiscard]] LUID Luid() const { return luid_; }
  // FRAME's target for `source` (the back buffer at PRESENT; a marker-expected frame passes at_present = false). While
  // `running` (the last FRAME reply said the helper's Session is not OFF) it makes the twins and, on KMT, the shared texture, and
  // the target says whether a frame block is mapped (`block_mapped`): the helper hands one out again while it is not, so a block
  // that arrived when these objects did not exist yet (a late first FRAME reply) is asked for afresh, never lost. `at_present`
  // refuses a multisampled source: "Multisampled back buffers need the Uplift technique on Direct3D 9: NR then runs on ReShade's
  // resolved copy". The colour space is the caller's (a D3D9 swap chain reports none).
  ipc::Target Describe(IDirect3DSurface9* source, bool running, bool at_present);
  // The FRAME reply's news (the reply of a FRAME sent this present): pull and map a new block; release everything when the
  // helper's NR is not running.
  void Apply(const ipc::Reply& reply, NrLink& link);
  // At the trigger point: out, RUN, back. True when NR's result reached `source`. `source` is the back buffer at PRESENT, or the
  // event's own render-target surface (bit 0 masked) at the marker and after the effects. Plan 10 (design §4.3): `motion` is this
  // frame's UPLIFT_MV texture (G16R16F; the texture variable's binding, which on D3D9 IS the IDirect3DTexture9, bit 0 masked) at the
  // Uplift technique while LaunchPad feeds it, null elsewhere. Its level 0 goes out beside the colour (KMT: StretchRect into a shared
  // twin; BLOCK: GetRenderTargetData into a SYSTEMMEM surface, both before the one event query) and the RUN carries `motion`. A frame
  // without one releases the motion, which the helper retires with that RUN.
  bool Run(IDirect3DSurface9* source, IDirect3DTexture9* motion, addon::TriggerPoint point, NrLink& link);
  // The last Run put a RUN on the wire (the helper has seen its point); false when it skipped or failed before sending.
  [[nodiscard]] bool LastRunSent() const { return run_sent_; }
  // At the end of the effects (design §4.3): UPLIFT_MASK toward the next RUN. `mask` is the texture variable's binding: on D3D9 a
  // shader-resource view IS the IDirect3DTexture9 (bit 0 masked), not a surface (design §4.1); its level 0 is the source. KMT:
  // StretchRect into a shared twin (an X8R8G8B8 mask becomes A8R8G8B8); BLOCK: GetRenderTargetData into a SYSTEMMEM surface, collected by
  // the next Run once its own event query has passed. While a request has no reply nothing is touched (`busy`): the helper may still
  // read the shared twin or the mask block. A format outside the design's table is `unsupported`, named in `d3d9_format`.
  MaskCopy CopyMask(IDirect3DTexture9* mask, NrLink& link);
  void ReleaseMask();  // no enabled effect writes UPLIFT_MASK: the twin, the SYSTEMMEM surface and the mapping go
  // Every object this client made, on both sides of the device: the DEFAULT-pool twins, plain surface, shared texture and query
  // (a live one makes IDirect3DDevice9::Reset fail with D3DERR_INVALIDCALL), the SYSTEMMEM surface and the block's view. Called at
  // destroy_command_queue (ReShade raises it before every Reset and ResetEx), destroy_device, when the helper is gone.
  // The client stays usable: the next Describe(running) makes its objects again.
  void ReleaseAll();
  // VRAM only: the twins, the plain surface, the KMT texture, and the KMT twins of the mask and the motion
  [[nodiscard]] uint64_t SharedBytes() const { return vram_bytes_ + mask_.vram_bytes + motion_.vram_bytes; }
  [[nodiscard]] std::string Line() const;  // "Direct3D 9 (32-bit or 64-bit): NR runs in Uplift's 64-bit helper (...)"

 private:
  D3D9Client() = default;

  // A size and format whose objects or block could not be made: not retried until NR is re-enabled (Plan 7's Minor 3).
  struct Failure {
    uint32_t width = 0u;
    uint32_t height = 0u;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    std::string text;
  };
  // The mask or the motion shared with the helper (design §4.2). KMT: `texture` is a shared render-target twin the helper opens by its
  // handle (a D3DPOOL_DEFAULT object: ReleaseAll frees it before a Reset). BLOCK: `view` is the helper's file mapping for it, mapped
  // here, and `sysmem` the SYSTEMMEM surface GetRenderTargetData fills, in the source's own format.
  struct Image {
    Microsoft::WRL::ComPtr<IDirect3DTexture9> texture;  // KMT
    Microsoft::WRL::ComPtr<IDirect3DSurface9> surface;  // KMT: its level 0
    Microsoft::WRL::ComPtr<IDirect3DSurface9> sysmem;   // BLOCK
    std::byte* view = nullptr;                          // BLOCK
    uint32_t pitch = 0u;                                // BLOCK: the block's row pitch
    uint32_t width = 0u;
    uint32_t height = 0u;
    D3DFORMAT source_format = D3DFMT_UNKNOWN;  // the source texture's; the twin's or the SYSTEMMEM surface's follows the table
    uint32_t bytes_per_pixel = 0u;
    uint64_t vram_bytes = 0u;  // KMT: the twin
    bool pending = false;      // BLOCK: a readback was issued; the next Run collects it once its own query has passed
    bool fresh = false;        // the helper has not been told about the newest copy yet
    bool late = false;         // its SHARE reply has not come: Apply forgets the ask (design §4.2, key decision h)
    [[nodiscard]] bool Ready() const { return surface != nullptr || view != nullptr; }
  };
  enum class Ensure { READY,
                      FAILED,
                      LATE };

  // Makes `image` for a source of this size and format when it has none, asking the helper: READY, FAILED (cached per size and format in
  // `failure`, logged once), or LATE (the reply did not come; `in_flight_` is set and nothing was made usable).
  Ensure EnsureImage(Image* image, ipc::RequestKind kind, const D3D9FormatMap& map, uint32_t width, uint32_t height,
                     std::optional<Failure>* failure, NrLink& link);
  void ReleaseImage(Image* image);
  // An event query issued now and polled with D3DGETDATA_FLUSH, capped at 2 s. True when the GPU passed it; on a cap, false and
  // `gpu_pending_` is set (the query is then still outstanding).
  bool WaitForGpu();
  void ReleaseGameObjects();

  IDirect3DDevice9* device_ = nullptr;  // not owned: valid until destroy_device (a Reset keeps it, after ReleaseAll)
  bool ex_ = false;
  LUID luid_ = {};

  // The frame the objects below were made for (all zero while there are none).
  uint32_t width_ = 0u;
  uint32_t height_ = 0u;
  D3DFORMAT source_format_ = D3DFMT_UNKNOWN;  // the back buffer's, at Describe
  D3DFORMAT twin_format_ = D3DFMT_UNKNOWN;    // what crosses: the source's format, or its twin
  uint32_t bytes_per_pixel_ = 0u;
  Microsoft::WRL::ComPtr<IDirect3DSurface9> twin_;            // BLOCK: a render-target copy in the twin format, when the source's differs
  Microsoft::WRL::ComPtr<IDirect3DSurface9> sysmem_;          // BLOCK: the SYSTEMMEM surface GetRenderTargetData fills, and UpdateSurface reads
  Microsoft::WRL::ComPtr<IDirect3DSurface9> plain_;           // BLOCK: the DEFAULT plain surface UpdateSurface fills, and StretchRect reads
  Microsoft::WRL::ComPtr<IDirect3DTexture9> shared_texture_;  // KMT: the shared render-target texture the helper opens
  Microsoft::WRL::ComPtr<IDirect3DSurface9> shared_surface_;  // KMT: its level 0
  HANDLE kmt_handle_ = nullptr;                               // a global name (a value in both processes), never closed
  uint32_t kmt_generation_ = 0u;                              // bumped whenever a new shared texture was made
  Microsoft::WRL::ComPtr<IDirect3DQuery9> query_;
  std::byte* view_ = nullptr;  // BLOCK: the helper's frame block, mapped here
  uint64_t view_bytes_ = 0u;
  uint32_t view_pitch_ = 0u;
  uint64_t vram_bytes_ = 0u;
  bool floor_probed_ = false;  // a frame below NR's floor already sent its one RUN (the status then says why)
  bool in_flight_ = false;     // a RUN has no reply yet: nothing touches the block (or the shared texture) until a FRAME reply
  bool gpu_pending_ = false;   // a WaitForGpu ran out its 2 s: this side's copy may still run on the GPU, so frames skip until it is done
  bool run_sent_ = false;

  std::optional<Failure> failure_;
  Image mask_;
  Image motion_;
  std::optional<Failure> mask_failure_;
  std::optional<Failure> motion_failure_;
};

}  // namespace uplift::client
