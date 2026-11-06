#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace uplift::bridge {

// What the add-on asks of a game device's private-device bridge (D3D11Bridge, D3D10Bridge, VkBridge, GlBridge) without knowing its API: the
// private Direct3D 12 device and queue, whether Retry now can replace them, the stop and its latch, and the panel's figures. Recording
// (BeginFrame, Run), the mask (CopyMask, ReleaseMask) and teardown stay on each bridge: their arguments and rules differ per API (GL's
// current-context rule and no GL call at destroy, Vulkan's retire-behind-fences and present-queue wait, D3D10's keyed relay, D3D11's DLSS
// hand-off and motion ring).
// D3D10Bridge: Stopped() is true once its D3D11 bridge or its relay stopped, while Latch() is the D3D11 bridge's text, which a relay stop
// reaches at the relay's next latch sync (after the next keyed step, or at BeginFrame): Stopped() can be true while Latch() is still empty.
// Never destroyed through this type: every owner holds a bridge by its own type.
class PrivateDeviceBridge {
 public:
  PrivateDeviceBridge(const PrivateDeviceBridge&) = delete;
  PrivateDeviceBridge& operator=(const PrivateDeviceBridge&) = delete;

  [[nodiscard]] virtual ID3D12Device* Device() const = 0;  // native: NGX and every Uplift object use it
  [[nodiscard]] virtual ID3D12CommandQueue* Queue() const = 0;
  [[nodiscard]] virtual bool Independent() const = 0;  // D3D12Side::Independent: whether Retry now can replace it
  // Stops the bridge for the session and releases every queued cross-API wait; the first reason is kept.
  virtual void Stop(std::string reason) = 0;
  [[nodiscard]] virtual std::string_view Latch() const = 0;  // the card's reason; empty while it runs
  [[nodiscard]] virtual bool Stopped() const = 0;
  [[nodiscard]] virtual uint64_t SharedBytes() const = 0;  // the shared surfaces, which the panel counts as Uplift's
  // The game API's name as the bridge's texts spell it ("The {} bridge stopped: ..."): Direct3D 11, Direct3D 10, Vulkan, OpenGL.
  [[nodiscard]] virtual std::string_view Label() const = 0;
  // Recordings that went without NR because the private allocator ring was busy (on Vulkan and OpenGL also a CPU-ordered recording still in
  // flight past its capped wait; on Direct3D 11 also DLSS-stage evaluates), since the bridge was made. D3D10Bridge: its D3D11 bridge's count.
  [[nodiscard]] virtual uint64_t BusySkips() const = 0;

 protected:
  PrivateDeviceBridge() = default;
  ~PrivateDeviceBridge() = default;
};

}  // namespace uplift::bridge
