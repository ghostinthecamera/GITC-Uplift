#include "addon/streamline_present.hpp"

#include <MinHook.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>
#include <utility>
#include <wrl/client.h>

#include "ngx_hooks/minhook_once.hpp"
#include "nr/log.hpp"

namespace uplift::addon::streamline {
namespace {

using Microsoft::WRL::ComPtr;
using Upgrade = int(WINAPI*)(void**);
using Factory = HRESULT(WINAPI*)(REFIID, void**);
using Factory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using Create = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateComposition = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

std::mutex install_mutex;
std::atomic<bool> enabled;
std::atomic<HMODULE> interposer;
Observer observer{};
thread_local bool inside_present;
thread_local void** creating_chain;

template<class Fn>
struct Hook {
  std::atomic<Fn> original{nullptr};
  std::atomic<void*> target{nullptr};
};

// Stable prefix of sl::DLSSGOptions from the public Streamline v2 ABI.
struct OptionsPrefix { void* next; GUID type; size_t version; uint32_t mode; };
constexpr GUID OPTIONS_TYPE = {0xfac5f1cb, 0x2dfd, 0x4f36, {0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5}};
static_assert(offsetof(OptionsPrefix, mode) == 32);
using SetOptions = int (*)(const void*, const OptionsPrefix&);
using GetFeature = int (*)(uint32_t, const char*, void*&);
Hook<SetOptions> set_options;
Hook<GetFeature> get_feature;
std::atomic<FgMode> mode = FgMode::UNKNOWN;
Hook<Upgrade> upgrade;
Hook<Factory> factory, factory1;
Hook<Factory2> factory2;
Hook<Create> create;
Hook<CreateHwnd> create_hwnd;
Hook<CreateComposition> create_composition;
Hook<Present> present;
Hook<Present1> present1;
using Resize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using Color = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
Hook<Resize> resize;
Hook<Resize1> resize1;
Hook<Color> color;
Hook<PFN_vkCreateSwapchainKHR> vk_create;
Hook<PFN_vkDestroySwapchainKHR> vk_destroy;
Hook<PFN_vkGetSwapchainImagesKHR> vk_images;
Hook<PFN_vkAcquireNextImageKHR> vk_acquire;
Hook<PFN_vkAcquireNextImage2KHR> vk_acquire2;
Hook<PFN_vkQueuePresentKHR> vk_present;
Hook<PFN_vkGetInstanceProcAddr> vk_instance_proc;
Hook<PFN_vkGetDeviceProcAddr> vk_device_proc;
Hook<decltype(&GetProcAddress)> get_proc;

HMODULE Owner(void* code) {
  HMODULE owner = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(code), &owner);
  return owner;
}

bool IsInterposer(HMODULE module) {
  if (module == nullptr) return false;
  if (const HMODULE known = interposer.load(std::memory_order_acquire)) return known == module;
  wchar_t path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return false;
  const wchar_t* name = wcsrchr(path, L'\\');
  return _wcsicmp(name ? name + 1 : path, L"sl.interposer.dll") == 0;
}

// A forwarded export or a shared native DXGI/vulkan-loader method is never an outer Streamline call.
template<class Fn>
bool Attach(Hook<Fn>& hook, void* address, Fn detour, HMODULE feature_module = nullptr) {
  if (!address) return false;
  const HMODULE module = Owner(address);
  if (!IsInterposer(module) && (!feature_module || module != feature_module)) return false;
  std::lock_guard guard(install_mutex);
  if (hook.target) return hook.target == address;
  HMODULE pin{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(address), &pin)) return false;
  void* trampoline = nullptr;
  if (MH_CreateHook(address, reinterpret_cast<void*>(detour), &trampoline) != MH_OK) return false;
  hook.original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
  if (MH_EnableHook(address) != MH_OK) {
    MH_RemoveHook(address);
    return false;
  }
  hook.target = address;
  return true;
}

template<class... Args>
void TraceDxgi(std::format_string<Args...> format, Args&&... args) noexcept {
  if (!observer.diagnostic_dxgi) return;
  try { nr::Logf(nr::LogLevel::INFO, format, std::forward<Args>(args)...); } catch (...) {}
}

void TraceMethod(const char* method, void* address, void* target, bool attached) {
  char path[MAX_PATH + 1]{};
  const HMODULE module = address ? Owner(address) : nullptr;
  if (module) GetModuleFileNameA(module, path, MAX_PATH);
  const char* file = std::strrchr(path, '\\');
  const char* result = attached ? "attached" : !address ? "no method" : !IsInterposer(module) ? "outside interposer"
      : target && target != address ? "different target already hooked" : "hook installation failed";
  TraceDxgi("Before FG DX12: {} method={} owner={} target={} result={}", method, address, file ? file + 1 : path, target, result);
}

template<class Fn>
bool AttachDxgi(const char* method, Hook<Fn>& hook, void* address, Fn detour) {
  const bool attached = Attach(hook, address, detour);
  if (observer.diagnostic_dxgi) TraceMethod(method, address, hook.target.load(), attached);
  return attached;
}

void** Vtable(void* object) { return *static_cast<void***>(object); }

class PresentScope {
 public:
  PresentScope() : nested_(std::exchange(inside_present, true)) {}
  ~PresentScope() { inside_present = nested_; }
  bool Nested() const { return nested_; }
 private:
  bool nested_;
};

HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* chain, UINT interval, UINT flags) {
  const PresentScope scope;
  const bool observe = !scope.Nested() && !(flags & DXGI_PRESENT_TEST);
  if (observe) observer.dxgi_present(chain, flags, nullptr);
  const HRESULT result = present.original.load(std::memory_order_acquire)(chain, interval, flags);
  if (observe && result != S_OK && observer.dxgi_retry) observer.dxgi_retry(chain);
  return result;
}

HRESULT STDMETHODCALLTYPE OnPresent1(IDXGISwapChain1* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
  const PresentScope scope;
  const bool observe = !scope.Nested() && !(flags & DXGI_PRESENT_TEST);
  if (observe) observer.dxgi_present(chain, flags, parameters);
  const HRESULT result = present1.original.load(std::memory_order_acquire)(chain, interval, flags, parameters);
  if (observe && result != S_OK && observer.dxgi_retry) observer.dxgi_retry(chain);
  return result;
}

HRESULT STDMETHODCALLTYPE OnResize(IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
  void* inner = nullptr;
  void** parent = std::exchange(creating_chain, &inner);
  const HRESULT hr = resize.original.load(std::memory_order_acquire)(chain, count, width, height, format, flags);
  creating_chain = parent;
  if (SUCCEEDED(hr) && observer.dxgi_resize) observer.dxgi_resize(chain, count, nullptr, inner);
  return hr;
}

HRESULT STDMETHODCALLTYPE OnResize1(IDXGISwapChain3* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT* nodes, IUnknown* const* queues) {
  void* inner = nullptr;
  void** parent = std::exchange(creating_chain, &inner);
  const HRESULT hr = resize1.original.load(std::memory_order_acquire)(chain, count, width, height, format, flags, nodes, queues);
  creating_chain = parent;
  if (SUCCEEDED(hr) && observer.dxgi_resize) {
    if (count == 0u && queues) {
      DXGI_SWAP_CHAIN_DESC1 desc{};
      if (SUCCEEDED(chain->GetDesc1(&desc))) count = desc.BufferCount;
    }
    observer.dxgi_resize(chain, count, queues, inner);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE OnColor(IDXGISwapChain3* chain, DXGI_COLOR_SPACE_TYPE space) {
  const HRESULT hr = color.original.load(std::memory_order_acquire)(chain, space);
  if (SUCCEEDED(hr) && observer.dxgi_color) observer.dxgi_color(chain, space);
  return hr;
}

void BindChain(IDXGISwapChain* chain, IUnknown* device, void* inner) {
  if (!chain) return;
  ComPtr<IDXGISwapChain1> chain1;
  const bool regular = AttachDxgi("Present", present, Vtable(chain)[8], &OnPresent);
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1)))) AttachDxgi("Present1", present1, Vtable(chain1.Get())[22], &OnPresent1);
  if (observer.dxgi_resize && observer.dxgi_color) {
    ComPtr<IDXGISwapChain3> chain3;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) {
      if (!AttachDxgi("ResizeBuffers", resize, Vtable(chain)[13], &OnResize) || !AttachDxgi("SetColorSpace1", color, Vtable(chain3.Get())[38], &OnColor)
          || !AttachDxgi("ResizeBuffers1", resize1, Vtable(chain3.Get())[39], &OnResize1)) return;
    }
  }
  if (regular) observer.dxgi_created(chain, device, inner);
}

template<class Chain, class Call>
HRESULT Created(const char* method, void* factory_object, IUnknown* device, Chain** output, Call&& original) {
  TraceDxgi("Before FG DX12: enter {} factory={} device={}", method, factory_object, static_cast<void*>(device));
  void* inner = nullptr;
  void** parent = std::exchange(creating_chain, &inner);
  const HRESULT result = original();
  creating_chain = parent;
  if (parent && inner) *parent = inner;
  TraceDxgi("Before FG DX12: return {} hr={:#x} outer={} ReShade={}", method, static_cast<unsigned>(result), SUCCEEDED(result) && output ? static_cast<void*>(*output) : nullptr, inner);
  if (SUCCEEDED(result) && output) BindChain(*output, device, inner);
  return result;
}

HRESULT STDMETHODCALLTYPE OnCreate(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** output) {
  return Created("CreateSwapChain", self, device, output, [&] { return create.original.load(std::memory_order_acquire)(self, device, desc, output); });
}

HRESULT STDMETHODCALLTYPE OnCreateHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrict_output, IDXGISwapChain1** output) {
  return Created("CreateSwapChainForHwnd", self, device, output, [&] { return create_hwnd.original.load(std::memory_order_acquire)(self, device, hwnd, desc, fullscreen, restrict_output, output); });
}

HRESULT STDMETHODCALLTYPE OnCreateComposition(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict_output,
                                               IDXGISwapChain1** output) {
  return Created("CreateSwapChainForComposition", self, device, output, [&] { return create_composition.original.load(std::memory_order_acquire)(self, device, desc, restrict_output, output); });
}

void BindFactory(void* object, const char* source) {
  TraceDxgi("Before FG DX12: inspect factory from {} object={}", source, object);
  if (!object) return;
  ComPtr<IDXGIFactory> base;
  const HRESULT hr = static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&base));
  if (FAILED(hr)) {
    TraceDxgi("Before FG DX12: object={} has no IDXGIFactory hr={:#x}", object, static_cast<unsigned>(hr));
    return;
  }
  AttachDxgi("CreateSwapChain", create, Vtable(base.Get())[10], &OnCreate);
  ComPtr<IDXGIFactory2> extended;
  if (SUCCEEDED(base.As(&extended))) {
    AttachDxgi("CreateSwapChainForHwnd", create_hwnd, Vtable(extended.Get())[15], &OnCreateHwnd);
    AttachDxgi("CreateSwapChainForComposition", create_composition, Vtable(extended.Get())[24], &OnCreateComposition);
  }
}

int WINAPI OnUpgrade(void** object) {
  TraceDxgi("Before FG DX12: enter slUpgradeInterface object={}", object ? *object : nullptr);
  const int result = upgrade.original.load(std::memory_order_acquire)(object);
  TraceDxgi("Before FG DX12: return slUpgradeInterface result={} object={}", result, result == 0 && object ? *object : nullptr);
  if (result == 0 && object) BindFactory(*object, "slUpgradeInterface");
  return result;
}

HRESULT WINAPI OnFactory(REFIID iid, void** output) {
  const HRESULT hr = factory.original.load(std::memory_order_acquire)(iid, output);
  if (SUCCEEDED(hr) && output) BindFactory(*output, "CreateDXGIFactory");
  return hr;
}
HRESULT WINAPI OnFactory1(REFIID iid, void** output) {
  const HRESULT hr = factory1.original.load(std::memory_order_acquire)(iid, output);
  if (SUCCEEDED(hr) && output) BindFactory(*output, "CreateDXGIFactory1");
  return hr;
}
HRESULT WINAPI OnFactory2(UINT flags, REFIID iid, void** output) {
  const HRESULT hr = factory2.original.load(std::memory_order_acquire)(flags, iid, output);
  if (SUCCEEDED(hr) && output) BindFactory(*output, "CreateDXGIFactory2");
  return hr;
}

VKAPI_ATTR VkResult VKAPI_CALL OnVkPresent(VkQueue queue, const VkPresentInfoKHR* info) {
  const PresentScope scope;
  if (scope.Nested() || !info) return vk_present.original.load(std::memory_order_acquire)(queue, info);
  VkForward forwarded{.info = *info};
  observer.vk_present(queue, info, &forwarded);
  if (forwarded.failure != VK_SUCCESS) {
    if (info->pResults) std::fill_n(info->pResults, info->swapchainCount, forwarded.failure);
    return forwarded.failure;
  }
  return vk_present.original.load(std::memory_order_acquire)(queue, &forwarded.info);
}

VKAPI_ATTR VkResult VKAPI_CALL OnVkImages(VkDevice device, VkSwapchainKHR chain, uint32_t* count, VkImage* images) {
  const VkResult result = vk_images.original.load(std::memory_order_acquire)(device, chain, count, images);
  if (result == VK_SUCCESS && images && observer.vk_images) observer.vk_images(device, chain, *count, images);
  return result;
}
VKAPI_ATTR void VKAPI_CALL OnVkDestroy(VkDevice device, VkSwapchainKHR chain, const VkAllocationCallbacks* allocator) {
  if (observer.vk_destroyed) observer.vk_destroyed(device, chain);
  vk_destroy.original.load(std::memory_order_acquire)(device, chain, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL OnVkAcquire(VkDevice device, VkSwapchainKHR chain, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* index) {
  const VkResult result = vk_acquire.original.load(std::memory_order_acquire)(device, chain, timeout, sem, fence, index);
  if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && observer.vk_acquired) observer.vk_acquired(device, chain, *index);
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL OnVkAcquire2(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* index) {
  const VkResult result = vk_acquire2.original.load(std::memory_order_acquire)(device, info, index);
  if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && observer.vk_acquired) observer.vk_acquired(device, info->swapchain, *index);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL OnVkCreate(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* chain) {
  void* inner = nullptr;
  void** parent = std::exchange(creating_chain, &inner);
  VkSwapchainCreateInfoKHR adjusted = *info;
  if (observer.vk_usage) adjusted.imageUsage |= observer.vk_usage(device, *info);
  const VkResult result = vk_create.original.load(std::memory_order_acquire)(device, &adjusted, allocator, chain);
  creating_chain = parent;
  if (parent && inner) *parent = inner;
  if (result == VK_SUCCESS && !parent) {
    if (observer.vk_created) observer.vk_created(device, *chain, adjusted, inner);
    if (const auto resolve = vk_device_proc.original.load(std::memory_order_acquire)) {
      Attach(vk_images, reinterpret_cast<void*>(resolve(device, "vkGetSwapchainImagesKHR")), &OnVkImages);
      Attach(vk_acquire, reinterpret_cast<void*>(resolve(device, "vkAcquireNextImageKHR")), &OnVkAcquire);
      Attach(vk_acquire2, reinterpret_cast<void*>(resolve(device, "vkAcquireNextImage2KHR")), &OnVkAcquire2);
      Attach(vk_destroy, reinterpret_cast<void*>(resolve(device, "vkDestroySwapchainKHR")), &OnVkDestroy);
      try {
        const auto images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(resolve(device, "vkGetSwapchainImagesKHR"));
        uint32_t count = 0u;
        if (images && images(device, *chain, &count, nullptr) == VK_SUCCESS && count != 0u) {
          std::vector<VkImage> values(count);
          if (images(device, *chain, &count, values.data()) == VK_SUCCESS && observer.vk_images) observer.vk_images(device, *chain, count, values.data());
        }
      } catch (...) { nr::Log(nr::LogLevel::WARN, "Before FG: could not observe the application-visible Vulkan images"); }
    }
  }
  return result;
}
void WatchVk(const char* name, PFN_vkVoidFunction result) {
  if (!result || !name) return;
  auto* address = reinterpret_cast<void*>(result);
  if (!std::strcmp(name, "vkQueuePresentKHR")) Attach(vk_present, address, &OnVkPresent);
  else if (!std::strcmp(name, "vkCreateSwapchainKHR")) Attach(vk_create, address, &OnVkCreate);
  else if (!std::strcmp(name, "vkDestroySwapchainKHR")) Attach(vk_destroy, address, &OnVkDestroy);
  else if (!std::strcmp(name, "vkGetSwapchainImagesKHR")) Attach(vk_images, address, &OnVkImages);
  else if (!std::strcmp(name, "vkAcquireNextImageKHR")) Attach(vk_acquire, address, &OnVkAcquire);
  else if (!std::strcmp(name, "vkAcquireNextImage2KHR")) Attach(vk_acquire2, address, &OnVkAcquire2);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL OnDeviceProc(VkDevice device, const char* name) {
  const auto result = vk_device_proc.original.load(std::memory_order_acquire)(device, name);
  WatchVk(name, result);
  return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL OnInstanceProc(VkInstance instance, const char* name) {
  const auto result = vk_instance_proc.original.load(std::memory_order_acquire)(instance, name);
  WatchVk(name, result);
  return result;
}

int OnSetOptions(const void* viewport, const OptionsPrefix& options) {
  const int result = set_options.original.load(std::memory_order_acquire)(viewport, options);
  if (result == 0 && options.type == OPTIONS_TYPE && options.version >= 1) {
    const FgMode current = options.mode == 0u ? FgMode::OFF : options.mode == 1u ? FgMode::ON : (options.mode == 2u || options.mode == 3u) ? FgMode::AUTO : FgMode::UNKNOWN;
    mode.store(current, std::memory_order_release);
  }
  return result;
}

int OnGetFeature(uint32_t feature, const char* name, void*& output) {
  const int hr = get_feature.original.load(std::memory_order_acquire)(feature, name, output);
  if (hr == 0 && name && !std::strcmp(name, "slDLSSGSetOptions")) {
    Attach(set_options, output, &OnSetOptions, GetModuleHandleW(L"sl.dlss_g.dll"));
  }
  return hr;
}

void BindExports(HMODULE module) {
  const auto resolve = [&](const char* name) { return reinterpret_cast<void*>(get_proc.original.load(std::memory_order_acquire)(module, name)); };
  // Streamline v2 returns a Result from slUpgradeInterface. Do not attach this ABI to v1.
  if (resolve("slGetNativeInterface") && resolve("slGetFeatureFunction")) Attach(upgrade, resolve("slUpgradeInterface"), &OnUpgrade);
  Attach(get_feature, resolve("slGetFeatureFunction"), &OnGetFeature);
  if (get_feature.original.load(std::memory_order_acquire)) {
    void* function = nullptr;
    OnGetFeature(1000u, "slDLSSGSetOptions", function);
  }
  Attach(factory, resolve("CreateDXGIFactory"), &OnFactory);
  Attach(factory1, resolve("CreateDXGIFactory1"), &OnFactory1);
  Attach(factory2, resolve("CreateDXGIFactory2"), &OnFactory2);
  Attach(vk_instance_proc, resolve("vkGetInstanceProcAddr"), &OnInstanceProc);
  Attach(vk_device_proc, resolve("vkGetDeviceProcAddr"), &OnDeviceProc);
  for (const auto* name : {"vkQueuePresentKHR", "vkCreateSwapchainKHR", "vkDestroySwapchainKHR", "vkGetSwapchainImagesKHR", "vkAcquireNextImageKHR", "vkAcquireNextImage2KHR"})
    WatchVk(name, reinterpret_cast<PFN_vkVoidFunction>(resolve(name)));
}

// Delay installation until the caller invokes the resolved entry. GetProcAddress can run under the loader lock.
template<auto Detour, class Fn, class... Args>
auto FirstCall(Hook<Fn>& hook, const char* name, Args... args) {
  if (void* target = hook.target.load(std::memory_order_acquire)) return reinterpret_cast<Fn>(target)(args...);
  const auto module = interposer.load(std::memory_order_acquire);
  const auto original = reinterpret_cast<Fn>(get_proc.original.load(std::memory_order_acquire)(module, name));
  Attach(hook, reinterpret_cast<void*>(original), Detour);
  return original(args...);
}

int FirstFeature(uint32_t feature, const char* name, void*& output) {
  HMODULE module = interposer.load(std::memory_order_acquire);
  const auto original = reinterpret_cast<GetFeature>(get_proc.original.load(std::memory_order_acquire)(module, "slGetFeatureFunction"));
  Attach(get_feature, reinterpret_cast<void*>(original), &OnGetFeature);
  return original(feature, name, output);
}

int WINAPI FirstUpgrade(void** v) { return FirstCall<&OnUpgrade>(upgrade, "slUpgradeInterface", v); }
HRESULT WINAPI FirstFactory(REFIID iid, void** v) { return FirstCall<&OnFactory>(factory, "CreateDXGIFactory", iid, v); }
HRESULT WINAPI FirstFactory1(REFIID iid, void** v) { return FirstCall<&OnFactory1>(factory1, "CreateDXGIFactory1", iid, v); }
HRESULT WINAPI FirstFactory2(UINT flags, REFIID iid, void** v) { return FirstCall<&OnFactory2>(factory2, "CreateDXGIFactory2", flags, iid, v); }
PFN_vkVoidFunction VKAPI_CALL FirstDeviceProc(VkDevice d, const char* n) { return FirstCall<&OnDeviceProc>(vk_device_proc, "vkGetDeviceProcAddr", d, n); }
PFN_vkVoidFunction VKAPI_CALL FirstInstanceProc(VkInstance i, const char* n) { return FirstCall<&OnInstanceProc>(vk_instance_proc, "vkGetInstanceProcAddr", i, n); }
VkResult VKAPI_CALL FirstVkPresent(VkQueue q, const VkPresentInfoKHR* i) { return FirstCall<&OnVkPresent>(vk_present, "vkQueuePresentKHR", q, i); }

FARPROC WINAPI OnGetProc(HMODULE module, LPCSTR name) {
  const FARPROC result = get_proc.original.load(std::memory_order_acquire)(module, name);
  if (!result || reinterpret_cast<uintptr_t>(name) <= 0xffffu || !enabled.load(std::memory_order_acquire)) return result;
  if (name[0] != 's' && name[0] != 'C' && name[0] != 'v') return result;
  FARPROC replacement = nullptr;
  if (!std::strcmp(name, "slUpgradeInterface")) replacement = reinterpret_cast<FARPROC>(&FirstUpgrade);
  else if (!std::strcmp(name, "slGetFeatureFunction")) replacement = reinterpret_cast<FARPROC>(&FirstFeature);
  else if (!std::strcmp(name, "CreateDXGIFactory")) replacement = reinterpret_cast<FARPROC>(&FirstFactory);
  else if (!std::strcmp(name, "CreateDXGIFactory1")) replacement = reinterpret_cast<FARPROC>(&FirstFactory1);
  else if (!std::strcmp(name, "CreateDXGIFactory2")) replacement = reinterpret_cast<FARPROC>(&FirstFactory2);
  else if (!std::strcmp(name, "vkGetDeviceProcAddr")) replacement = reinterpret_cast<FARPROC>(&FirstDeviceProc);
  else if (!std::strcmp(name, "vkGetInstanceProcAddr")) replacement = reinterpret_cast<FARPROC>(&FirstInstanceProc);
  else if (!std::strcmp(name, "vkQueuePresentKHR")) replacement = reinterpret_cast<FARPROC>(&FirstVkPresent);
  if (!replacement || !IsInterposer(module) || Owner(reinterpret_cast<void*>(result)) != module) return result;
  if (replacement == reinterpret_cast<FARPROC>(&FirstUpgrade) && (!get_proc.original.load(std::memory_order_acquire)(module, "slGetNativeInterface") || !get_proc.original.load(std::memory_order_acquire)(module, "slGetFeatureFunction"))) return result;
  HMODULE empty = nullptr;
  interposer.compare_exchange_strong(empty, module);
  return empty && empty != module ? result : replacement;
}

}  // namespace

bool Start(HMODULE self, const Observer& callbacks) {
  if (enabled.load()) return true;
  if (hooks::EnsureMinHook() != MH_OK) return false;
  HMODULE pin{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(self), &pin)) return false;
  void* address = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));
  void* trampoline = nullptr;
  if (!address || MH_CreateHook(address, reinterpret_cast<void*>(&OnGetProc), &trampoline) != MH_OK) return false;
  get_proc.original.store(reinterpret_cast<decltype(&GetProcAddress)>(trampoline), std::memory_order_release);
  observer = callbacks;
  TraceDxgi("Before FG DX12: issue 12 diagnostic enabled");
  if (MH_EnableHook(address) != MH_OK) {
    MH_RemoveHook(address);
    return false;
  }
  get_proc.target = address;
  hooks::HoldMinHook();
  enabled.store(true, std::memory_order_release);
  Poll();
  return true;
}

void Poll() {
  if (!enabled.load(std::memory_order_acquire)) return;
  static std::atomic<bool> bound;
  if (bound.load(std::memory_order_relaxed)) return;
  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (!module || bound.exchange(true)) return;
  interposer.store(module, std::memory_order_release);
  TraceDxgi("Before FG DX12: binding Streamline interposer={}", static_cast<void*>(module));
  BindExports(module);
  // ReShade loads add-ons at device creation, after a statically linked application may already have obtained its factory.
  // A separate Streamline factory exposes the same provider methods without upgrading or replacing the application's object.
  if (factory1.original.load(std::memory_order_acquire)) {
    ComPtr<IDXGIFactory2> probe;
    if (SUCCEEDED(factory1.original.load(std::memory_order_acquire)(IID_PPV_ARGS(&probe)))) BindFactory(probe.Get(), "Uplift probe");
  }
  nr::Logf(nr::LogLevel::INFO, "Before FG hooks: upgrade={} DXGI factories={}/{}/{} creation={}/{}/{} Vulkan resolver={}/{} vkQueuePresentKHR={}",
           upgrade.target != nullptr, factory.target != nullptr, factory1.target != nullptr, factory2.target != nullptr,
           create.target != nullptr, create_hwnd.target != nullptr, create_composition.target != nullptr,
           vk_instance_proc.target != nullptr, vk_device_proc.target != nullptr, vk_present.target != nullptr);
}

void NoteReshadeSwapchain(void* chain) {
  if (creating_chain) *creating_chain = chain;
  if (Installed()) TraceDxgi("Before FG trace: ReShade init swapchain={} inside provider creation={}", chain, creating_chain != nullptr);
}
bool WatchGameFacing(IDXGISwapChain* chain, bool trace_method) {
  if (!chain || !Installed() || inside_present) return false;
  void* method = Vtable(chain)[8];
  const bool attached = IsInterposer(Owner(method)) && Attach(present, method, &OnPresent);
  if (trace_method && observer.diagnostic_dxgi) TraceMethod("ReShade native Present", method, present.target.load(), attached);
  if (!attached) return false;
  ComPtr<IDXGISwapChain1> chain1;
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1)))) Attach(present1, Vtable(chain1.Get())[22], &OnPresent1);
  return true;
}
bool InsideOuterPresent() { return inside_present; }
bool Installed() { return enabled.load(std::memory_order_acquire); }
FgMode Mode() { return mode.load(std::memory_order_acquire); }

}  // namespace uplift::addon::streamline
