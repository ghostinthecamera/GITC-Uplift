#include "addon/streamline_present.hpp"

#include <MinHook.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <wrl/client.h>
#include <psapi.h>

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
using Resize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

// COM method slots, numbered as in the IDXGIFactory2 and IDXGISwapChain3 vtables.
constexpr size_t CREATE_SWAPCHAIN_SLOT = 10u;                  // IDXGIFactory::CreateSwapChain
constexpr size_t CREATE_SWAPCHAIN_FOR_HWND_SLOT = 15u;         // IDXGIFactory2::CreateSwapChainForHwnd
constexpr size_t CREATE_SWAPCHAIN_FOR_COMPOSITION_SLOT = 24u;  // IDXGIFactory2::CreateSwapChainForComposition
constexpr size_t PRESENT_SLOT = 8u;                            // IDXGISwapChain::Present
constexpr size_t RESIZE_BUFFERS_SLOT = 13u;                    // IDXGISwapChain::ResizeBuffers
constexpr size_t PRESENT1_SLOT = 22u;                          // IDXGISwapChain1::Present1
constexpr size_t RESIZE_BUFFERS1_SLOT = 39u;                   // IDXGISwapChain3::ResizeBuffers1

// Streamline can implement one DXGI method more than once: on its proxy objects, and as a vtable hook on the factory class
// when the host calls its CreateDXGIFactory* without eUseDXGIFactoryProxy. This many distinct implementations of one method
// can be hooked at once.
constexpr size_t PROVIDER_IMPLEMENTATIONS = 4u;
// A method has more Streamline implementations than PROVIDER_IMPLEMENTATIONS: the method's name, then the refused address.
constexpr char PROVIDER_TABLE_FULL_LINE[] = "Before FG: no free hook for {} at {}";

// Poll's probe factories (issue 18). The raw probe comes from the system DXGI, found by full path: the system directory,
// then this file name. FACTORY1_EXPORT is the function it resolves there.
constexpr wchar_t SYSTEM_DXGI_FILE[] = L"\\dxgi.dll";
constexpr char FACTORY1_EXPORT[] = "CreateDXGIFactory1";
// The DiagnosticBeforeFG trace names each probe as the factory's source.
constexpr char RAW_PROBE_SOURCE[] = "Uplift probe";
constexpr char UPGRADED_PROBE_SOURCE[] = "Uplift upgraded probe";
// The probe's slUpgradeInterface: its result, then the raw factory and the object that came back.
constexpr char PROBE_UPGRADE_LINE[] = "Before FG DX12: probe slUpgradeInterface result={} raw={} upgraded={}";
// No hooked Streamline v2 slUpgradeInterface: a v1 host, or an entry Uplift could not hook.
constexpr char PROBE_NO_UPGRADE_LINE[] = "Before FG DX12: probe skipped slUpgradeInterface (no hooked Streamline v2 entry)";
// Streamline's proxies found by type: no probe upgrade is needed, so no third party's slUpgradeInterface hook sees one.
constexpr char PROBE_UPGRADE_NOT_NEEDED_LINE[] = "Before FG DX12: probe slUpgradeInterface not needed: Streamline's proxy classes were found by type";

// Streamline's proxy structs, as MSVC's RTTI names them in sl.interposer.dll; both names include their terminating NUL. Every
// interposer on the owner's drives, Streamline 1.5 to 2.x, carries both, except builds without a factory proxy (Dead
// Space 2023), which carry only the swapchain's. Finding a proxy class by its type needs no object made, no Streamline call
// and no third party's hook in the path (Witcher 3 with MFG Unlocker 1.5, 21 Nov 2026: MFG wraps slUpgradeInterface's result).
constexpr char FACTORY_TYPE_NAME[] = ".?AUDXGIFactory@interposer@sl@@";
constexpr char SWAPCHAIN_TYPE_NAME[] = ".?AUDXGISwapChain@interposer@sl@@";
constexpr char FACTORY_CLASS[] = "DXGIFactory";
constexpr char SWAPCHAIN_CLASS[] = "DXGISwapChain";
// x64 RTTI. A type descriptor is { vtable pointer, spare pointer, name }. A complete object locator (CompleteObjectLocator
// below) has signature 1, and its type descriptor and itself as RVAs. The primary vtable, at offset 0, has the locator's
// address in the pointer before its first method.
constexpr size_t TYPE_DESCRIPTOR_NAME_OFFSET = 2u * sizeof(void*);
constexpr uint32_t LOCATOR_SIGNATURE = 1u;
constexpr uint32_t PRIMARY_VTABLE_OFFSET = 0u;
constexpr DWORD READ_ONLY_DATA_EXCLUDED = IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE;
// A vtable candidate: the locator's address and at least the first method after it.
constexpr size_t VTABLE_CANDIDATE_BYTES = 2u * sizeof(void*);
// The class line's list: each method's name and what became of it, the methods separated.
constexpr char METHOD_RESULT_SEPARATOR[] = " ";
constexpr char METHOD_LIST_SEPARATOR[] = ", ";
// Always logged, not only with DiagnosticBeforeFG: the class, its vtable's RVA and what became of each method.
constexpr char PROXY_CLASS_LINE[] = "Before FG: Streamline's {} class found by type at interposer+{:#x}: {}";
constexpr char PROXY_CLASS_MISSING_LINE[] = "Before FG: Streamline's {} class not found by type in this interposer";
// A dxgi.dll that is not the system's: ReShade installed as the game's dxgi.dll, or another wrapper. Streamline resolves
// "dxgi.dll" by name, so where it hooks the factory class (no eUseDXGIFactoryProxy) the class it hooks is that module's
// wrapper class, which no system factory shows (The Last of Us Part II, 17 Nov 2026). Compared case-insensitively.
constexpr wchar_t DXGI_FILE_NAME[] = L"dxgi.dll";
constexpr char NON_SYSTEM_PROBE_SOURCE[] = "Uplift probe through a non-system dxgi.dll";
// The module's path, the factory call's HRESULT, and whose code answers the factory's QueryInterface.
constexpr char NON_SYSTEM_PROBE_LINE[] = "Before FG DX12: non-system dxgi.dll probe {} result={:#x} factory class={}";
constexpr char WRAPPER_CLASS[] = "the module's own";  // the module wraps factories: Streamline's class hooks land here
constexpr char SYSTEM_CLASS[] = "the system's";       // the module handed back the system factory unwrapped
constexpr char OTHER_CLASS[] = "another module's";
// The module's CreateDXGIFactory1 is not its own code: the module that owns it. Not probed.
constexpr char NON_SYSTEM_SKIPPED_LINE[] = "Before FG DX12: non-system dxgi.dll {} not probed: its CreateDXGIFactory1 is in {}";
constexpr size_t QUERY_INTERFACE_SLOT = 0u;  // IUnknown::QueryInterface: always the class's own method
// Bounds K32EnumProcessModules' retries when modules load between its calls.
constexpr int MODULE_LIST_ATTEMPTS = 4;
// K32EnumProcessModules failed (it may while modules load): its error. The by-name lookup is the dxgi.dll Streamline's own
// LoadLibraryW(L"dxgi.dll") resolves, so it is probed instead.
constexpr char MODULE_LIST_FAILED_LINE[] = "Before FG DX12: listing modules failed (error {}); probing the dxgi.dll a by-name lookup finds";
constexpr char NON_SYSTEM_NO_EXPORT_LINE[] = "Before FG DX12: non-system dxgi.dll {} not probed: no CreateDXGIFactory1";
// The system probe did not run: the system dxgi.dll (null when not loaded) has no CreateDXGIFactory1 of its own, or its call failed.
constexpr char SYSTEM_PROBE_SKIPPED_LINE[] = "Before FG DX12: no system probe: system dxgi.dll={} has no CreateDXGIFactory1 of its own";
constexpr char SYSTEM_PROBE_FAILED_LINE[] = "Before FG DX12: no system probe: CreateDXGIFactory1 failed hr={:#x}";
// A non-system dxgi.dll's export reached Streamline's factory export during Uplift's probe: the export's name.
constexpr char WRAPPER_CHAIN_REFUSED_LINE[] =
    "Before FG: a non-system dxgi.dll's CreateDXGIFactory1 called Streamline's {} during Uplift's probe; refused, nothing was hooked (issue 18)";
constexpr HRESULT PROBE_REFUSED = DXGI_ERROR_UNSUPPORTED;

// A pure forwarder: sl.interposer's one-line proxy method that loads the wrapped object (at +10h in Streamline 2, +8 in
// 1.5) and jumps to the same slot of its vtable. MSVC emits it in three forms, each with an 8-bit or a 32-bit displacement:
//   mov rcx,[rcx+off]; mov rax,[rcx]; jmp [rax+slot*8]                   48 8B 49 off, 48 8B 01, [48] FF 60/A0 disp
//   mov rcx,[rcx+off]; mov rax,[rcx]; mov r10,[rax+slot*8]; jmp r10      48 8B 49 off, 48 8B 01, 4C 8B 50/90 disp, 49 FF E2
//   mov rcx,[rcx+off]; mov r10,[rcx]; jmp [r10+slot*8]                   48 8B 49 off, 4C 8B 11, 49 FF 62/A2 disp
// The second is the proxy factory's CreateSwapChainForComposition in every Streamline 2 interposer; the third is the device
// proxy's. The linker folds identical forwarders, so one function can serve that slot for several proxy classes: a command
// list's SetGraphicsRootConstantBufferView is the swapchain's slot 38 (GitHub PR 21). Such a method changes nothing at that
// level, so Uplift never hooks one.
constexpr std::array<uint8_t, 3> LOAD_WRAPPED = {0x48u, 0x8Bu, 0x49u};  // mov rcx,[rcx+disp8]; the offset byte follows
constexpr std::array<uint8_t, 3> LOAD_VTABLE_RAX = {0x48u, 0x8Bu, 0x01u};  // mov rax,[rcx]
constexpr std::array<uint8_t, 3> LOAD_VTABLE_R10 = {0x4Cu, 0x8Bu, 0x11u};  // mov r10,[rcx]
constexpr uint8_t REX_W = 0x48u;
constexpr uint8_t REX_WR = 0x4Cu;  // REX.W with the ModRM reg field extended: r10 as destination
constexpr uint8_t REX_WB = 0x49u;  // REX.W with the ModRM r/m field extended: r10 as base or operand
constexpr uint8_t MOV_LOAD = 0x8Bu;
constexpr uint8_t JMP_INDIRECT = 0xFFu;
constexpr uint8_t MODRM_JMP_RAX_DISP8 = 0x60u;
constexpr uint8_t MODRM_JMP_RAX_DISP32 = 0xA0u;
constexpr uint8_t MODRM_MOV_R10_RAX_DISP8 = 0x50u;
constexpr uint8_t MODRM_MOV_R10_RAX_DISP32 = 0x90u;
constexpr uint8_t MODRM_JMP_R10 = 0xE2u;
constexpr uint8_t MODRM_JMP_R10_DISP8 = 0x62u;
constexpr uint8_t MODRM_JMP_R10_DISP32 = 0xA2u;
constexpr size_t DISP8_MAX = 0x7Fu;

// DiagnosticBeforeFG trace of the game's slDLSSGSetOptions calls, the one source of Mode().
// sl::ViewportHandle is a Streamline BaseStructure (next, type GUID, version) followed by its uint32_t id.
constexpr size_t VIEWPORT_ID_OFFSET = 32u;
// Raw sl::DLSSGMode values the trace counts one by one (eOff, eOn, eAuto, eDynamic); any other value shares the last bucket.
constexpr uint32_t COUNTED_RAW_MODES = DLSSG_MODE_DYNAMIC + 1u;
constexpr size_t MODE_BUCKETS = COUNTED_RAW_MODES + 1u;
// Calls whose viewport, mode or result differ from the call before are logged one by one, up to this many per session.
constexpr uint32_t MAX_OPTIONS_CHANGE_LINES = 64u;
// Between change lines, a summary of the calls by raw mode is logged at most this often.
constexpr auto OPTIONS_SUMMARY_PERIOD = std::chrono::seconds(5);
constexpr std::array<std::string_view, 4> FG_MODE_NAMES = {"UNKNOWN", "OFF", "ON", "AUTO"};  // indexed by FgMode
static_assert(FG_MODE_NAMES.size() == static_cast<size_t>(FgMode::AUTO) + 1u);
constexpr char UNREADABLE[] = "unreadable";  // a viewport id, raw mode or version the trace cannot read
constexpr char NO_OWNER[] = "none";  // slGetFeatureFunction failed, so there is no function to own
constexpr char OPTIONS_HOOK_LINE[] =
    "Before FG DX12: slGetFeatureFunction(slDLSSGSetOptions) result={} function={} owner={} hook={}";
constexpr char OPTIONS_CALL_LINE[] =
    "Before FG DX12: slDLSSGSetOptions call viewport={} id={} raw mode={} version={} result={} stored mode {} -> {}";
constexpr char OPTIONS_CHANGE_CAP_LINE[] =
    "Before FG DX12: slDLSSGSetOptions arguments keep changing; per-call lines stop here, summaries continue";
constexpr char OPTIONS_SUMMARY_LINE[] =
    "Before FG DX12: slDLSSGSetOptions calls raw mode 0/1/2/3/other={}/{}/{}/{}/{} unreadable={} warned={} refused={} stored mode={}";

// Streamline's DLSS-G plug-in under its own name, as a host that runs no downloaded (OTA) copy loads it; the slDLSSGSetOptions
// hook attaches only there or in the interposer. slShutdown unloads it, so its hook is never pinned and leaves before it unloads.
constexpr wchar_t DLSSG_PLUGIN_FILE[] = L"sl.dlss_g.dll";
constexpr char SHUTDOWN_EXPORT[] = "slShutdown";
// The game's slShutdown took the slDLSSGSetOptions hook out of the plug-in: the hooked address.
constexpr char OPTIONS_RELEASED_LINE[] = "Before FG DX12: slShutdown released the slDLSSGSetOptions hook at {}";
// MinHook could not disable that hook: the address and MinHook's status. The entry is abandoned, never touched again.
constexpr char OPTIONS_RELEASE_FAILED_LINE[] =
    "Before FG: could not take the slDLSSGSetOptions hook at {} out before slShutdown ({}); Streamline's mode is not read again at that address";
// Something other than slShutdown unloaded the plug-in that held the hook: its address. Disabling it would write MinHook's
// saved bytes into whatever module now occupies that address (MinHook refuses only when nothing is mapped there), so the
// entry is abandoned untouched. While it stays, a plug-in reloaded at that address cannot be hooked again.
constexpr char OPTIONS_PLUGIN_GONE_LINE[] =
    "Before FG: the DLSS-G plug-in holding the slDLSSGSetOptions hook at {} was unloaded before slShutdown; the hook is left untouched";
// Streamline's DLSS-G feature (sl::kFeatureDLSS_G) and the function Uplift reads its mode from.
constexpr uint32_t DLSSG_FEATURE = 1000u;
constexpr char SET_OPTIONS_NAME[] = "slDLSSGSetOptions";
// A game keeps slDLSSGSetOptions from one slGetFeatureFunction call (Streamline's SL_FEATURE_FUN_IMPORT_STATIC helper), so
// after slShutdown Uplift asks for it again itself (see RequeryOptions): what it was after, Streamline's result, the
// function and whether it is hooked now.
constexpr char GET_FEATURE_EXPORT[] = "slGetFeatureFunction";
constexpr char UPGRADE_EXPORT[] = "slUpgradeInterface";
// HUD-less (design §2): Streamline's tag exports, hooked to read the tags only. slSetTagForFrame is absent up to 2.7.15.
constexpr char SET_TAG_EXPORT[] = "slSetTag";
constexpr char SET_TAG_FOR_FRAME_EXPORT[] = "slSetTagForFrame";
constexpr char VK_CREATE_NAME[] = "vkCreateSwapchainKHR";
constexpr char OPTIONS_REQUERY_LINE[] =
    "Before FG DX12: {} after slShutdown; asking slGetFeatureFunction again for slDLSSGSetOptions result={} function={} hooked={}";

std::mutex install_mutex;
std::atomic<bool> enabled;
std::atomic<HMODULE> interposer;
Observer observer{};
thread_local bool inside_present;

// One provider call that may make ReShade create or resize its swapchain: a hooked Streamline create or resize, possibly
// nested inside another one. ReShade's init event (NoteReshade) records its chain in the innermost open scope, and closing
// a scope hands that chain to the scope around it, so the outermost call always learns the ReShade chain beneath it.
// This is the only link between a game-facing chain and ReShade's inner chain; nothing else names the slot.
class ProviderScope {
 public:
  template<class Result>
  struct Outcome {
    Result result;
    void* inner;     // ReShade's chain created or resized beneath this call, or nullptr
    bool outermost;  // no provider call encloses this one on the thread
  };

  // Runs one provider call inside a scope; the scope is closed again before the caller acts on the outcome.
  template<class Call>
  static auto Run(Call&& call) {
    ProviderScope scope;  // not const: NoteReshade writes inner_ through current_
    auto result = std::forward<Call>(call)();
    return Outcome<decltype(result)>{result, scope.inner_, scope.parent_ == nullptr};
  }
  static void NoteReshade(void* chain) {
    if (current_) current_->inner_ = chain;
  }
  static bool Open() { return current_ != nullptr; }

 private:
  ProviderScope() : parent_(std::exchange(current_, this)) {}
  ~ProviderScope() {
    current_ = parent_;
    if (parent_ && inner_) parent_->inner_ = inner_;
  }
  ProviderScope(const ProviderScope&) = delete;
  ProviderScope& operator=(const ProviderScope&) = delete;

  static thread_local ProviderScope* current_;
  ProviderScope* parent_;
  void* inner_ = nullptr;
};
thread_local ProviderScope* ProviderScope::current_ = nullptr;

template<class Fn>
struct Hook {
  std::atomic<Fn> original{nullptr};
  std::atomic<void*> target{nullptr};
  std::atomic_flag warned_enable_failure{};

  // A detour needs original during enable. Optional probes may use it only after target is published.
  Fn InstalledOriginal() const {
    return target.load(std::memory_order_acquire) ? original.load(std::memory_order_acquire) : Fn{};
  }
};

// The DXGI methods Streamline implements more than once. A spec names its method and vtable slot and declares the detour
// that each entry of the method's hook table instantiates once, so every entry calls its own original.
struct CreateMethod {
  using Fn = Create;
  static constexpr const char* NAME = "CreateSwapChain";
  static constexpr size_t SLOT = CREATE_SWAPCHAIN_SLOT;
  template<size_t Entry> static HRESULT STDMETHODCALLTYPE Detour(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
};
struct CreateHwndMethod {
  using Fn = CreateHwnd;
  static constexpr const char* NAME = "CreateSwapChainForHwnd";
  static constexpr size_t SLOT = CREATE_SWAPCHAIN_FOR_HWND_SLOT;
  template<size_t Entry>
  static HRESULT STDMETHODCALLTYPE Detour(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
};
struct CreateCompositionMethod {
  using Fn = CreateComposition;
  static constexpr const char* NAME = "CreateSwapChainForComposition";
  static constexpr size_t SLOT = CREATE_SWAPCHAIN_FOR_COMPOSITION_SLOT;
  template<size_t Entry> static HRESULT STDMETHODCALLTYPE Detour(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
};
struct PresentMethod {
  using Fn = Present;
  static constexpr const char* NAME = "Present";
  static constexpr size_t SLOT = PRESENT_SLOT;
  template<size_t Entry> static HRESULT STDMETHODCALLTYPE Detour(IDXGISwapChain*, UINT, UINT);
};
struct Present1Method {
  using Fn = Present1;
  static constexpr const char* NAME = "Present1";
  static constexpr size_t SLOT = PRESENT1_SLOT;
  template<size_t Entry> static HRESULT STDMETHODCALLTYPE Detour(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
};
struct ResizeMethod {
  using Fn = Resize;
  static constexpr const char* NAME = "ResizeBuffers";
  static constexpr size_t SLOT = RESIZE_BUFFERS_SLOT;
  template<size_t Entry> static HRESULT STDMETHODCALLTYPE Detour(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
};
struct Resize1Method {
  using Fn = Resize1;
  static constexpr const char* NAME = "ResizeBuffers1";
  static constexpr size_t SLOT = RESIZE_BUFFERS1_SLOT;
  template<size_t Entry> static HRESULT STDMETHODCALLTYPE Detour(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
};

// One DXGI method with every Streamline implementation of it that is hooked: one Hook per implementation. Entries fill in
// order and, like every hook in this file but slDLSSGSetOptions's, stay installed for the life of the process, so a filled
// entry never empties.
template<class Spec>
struct ProviderMethod {
  using Fn = typename Spec::Fn;
  std::array<Hook<Fn>, PROVIDER_IMPLEMENTATIONS> entries{};
  std::atomic_flag warned_full{};

  Fn Original(size_t entry) const { return entries[entry].original.load(std::memory_order_acquire); }
  bool Attached() const { return entries.front().target.load(std::memory_order_acquire) != nullptr; }
};

// Stable prefix of sl::DLSSGOptions from the public Streamline v2 ABI.
struct OptionsPrefix { void* next; GUID type; size_t version; uint32_t mode; };
constexpr GUID OPTIONS_TYPE = {0xfac5f1cb, 0x2dfd, 0x4f36, {0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5}};
static_assert(offsetof(OptionsPrefix, mode) == 32);
using SetOptions = int (*)(const void*, const OptionsPrefix&);
using GetFeature = int (*)(uint32_t, const char*, void*&);
using Shutdown = int (*)();
Hook<SetOptions> set_options;
// The plug-in the slDLSSGSetOptions hook is in, recorded when it is attached. Written under install_mutex.
HMODULE options_module = nullptr;
// A slDLSSGSetOptions hook that slShutdown disabled, kept until the next one is attached, then removed. Disabled, its MinHook
// entry touches no code: removing it is safe once the plug-in is gone. Written under install_mutex.
void* released_options = nullptr;
// slShutdown took the hook out and no hook has been attached since: RequeryOptions asks for the function again.
std::atomic<bool> requery_options{false};
// Counts the releases that set requery_options. Written under install_mutex, so a re-query that Streamline answered clears
// the flag only if no slShutdown on another thread set it again meanwhile.
unsigned options_releases = 0u;
// A slDLSSGSetOptions hook was attached once in this process: every later slShutdown arms the re-query, hook or not, so an
// instance whose answer could not be hooked does not stop the next one from being asked. Written under install_mutex.
bool options_attached_once = false;
Hook<GetFeature> get_feature;
Hook<Shutdown> sl_shutdown;
// The SDK takes the viewport and the frame token by reference: at the ABI they are pointers.
using SetTag = int (*)(const void* viewport, const void* tags, uint32_t count, void* command_buffer);
using SetTagForFrame = int (*)(const void* frame, const void* viewport, const void* tags, uint32_t count, void* command_buffer);
Hook<SetTag> set_tag;
Hook<SetTagForFrame> set_tag_for_frame;
std::atomic<FgMode> mode = FgMode::UNKNOWN;

// What the DiagnosticBeforeFG trace remembers of the slDLSSGSetOptions calls; written only while the trace is on.
struct OptionsTrace {
  std::mutex mutex;
  std::array<uint64_t, MODE_BUCKETS> calls{};  // since the last summary, by raw mode
  uint64_t unreadable{};                        // since the last summary: options the hook cannot read
  uint64_t warned{};                            // since the last summary: applied with eWarnOutOfVRAM
  uint64_t refused{};                           // since the last summary: not applied
  std::optional<std::chrono::steady_clock::time_point> summary_at;  // the first call starts the first period
  bool has_last = false;
  const void* last_viewport = nullptr;
  std::optional<uint32_t> last_id;
  std::optional<uint32_t> last_raw_mode;
  int last_result{};
  uint32_t change_lines{};
};
OptionsTrace options_trace;
Hook<Upgrade> upgrade;
Hook<Factory> factory, factory1;
Hook<Factory2> factory2;
ProviderMethod<CreateMethod> create;
ProviderMethod<CreateHwndMethod> create_hwnd;
ProviderMethod<CreateCompositionMethod> create_composition;
ProviderMethod<PresentMethod> present;
ProviderMethod<Present1Method> present1;
ProviderMethod<ResizeMethod> resize;
ProviderMethod<Resize1Method> resize1;
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

template<class Fn>
bool InstallDetour(Hook<Fn>& hook, void* address, Fn detour) {
  void* trampoline = nullptr;
  if (MH_CreateHook(address, reinterpret_cast<void*>(detour), &trampoline) != MH_OK) return false;
  hook.original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
  if (const auto status = MH_EnableHook(address); status != MH_OK) {
    hook.original.store(Fn{}, std::memory_order_release);
    MH_RemoveHook(address);
    if (!hook.warned_enable_failure.test_and_set(std::memory_order_relaxed)) {
      try { nr::Logf(nr::LogLevel::WARN, "Before FG: could not enable the hook at {} ({})", address, MH_StatusToString(status)); } catch (...) {}
    }
    return false;
  }
  hook.target = address;
  return true;
}

// A forwarded export or a shared native DXGI/vulkan-loader method is never an outer Streamline call.
bool ProviderOwned(void* address, HMODULE feature_module) {
  const HMODULE module = Owner(address);
  return IsInterposer(module) || (feature_module && module == feature_module);
}

bool PinOwner(void* address) {
  HMODULE pin{};
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(address), &pin);
}

// An interposer export: one address by definition, so the first one attached is the only one. Pinned, as every interposer
// hook is: it stays for the process, and Streamline never unloads its interposer (slShutdown unloads only the plug-ins).
template<class Fn>
bool Attach(Hook<Fn>& hook, void* address, Fn detour) {
  if (!address || !ProviderOwned(address, nullptr)) return false;
  std::lock_guard guard(install_mutex);
  if (hook.target) return hook.target == address;
  return PinOwner(address) && InstallDetour(hook, address, detour);
}

enum class ClaimResult { ATTACHED, NO_METHOD, OUTSIDE_INTERPOSER, PURE_FORWARDER, NO_FREE_HOOK, INSTALL_FAILED };
struct Claim {
  ClaimResult result;
  size_t entry = PROVIDER_IMPLEMENTATIONS;  // the table entry that holds or was to hold the address; PROVIDER_IMPLEMENTATIONS when none
  bool Attached() const { return result == ClaimResult::ATTACHED; }
};

template<class Spec, size_t... Entry>
constexpr std::array<typename Spec::Fn, sizeof...(Entry)> DetourTable(std::index_sequence<Entry...>) {
  return {&Spec::template Detour<Entry>...};
}

// A ModRM byte at at, then its displacement, addresses slot: the 8-bit form or the 32-bit one. at moves past both.
bool SlotDisplacement(const uint8_t*& at, uint8_t modrm_disp8, uint8_t modrm_disp32, size_t slot) {
  const size_t displacement = slot * sizeof(void*);
  if (at[0] == modrm_disp8) {
    const bool match = displacement <= DISP8_MAX && at[1] == displacement;
    at += sizeof(uint8_t) + sizeof(uint8_t);
    return match;
  }
  if (at[0] != modrm_disp32) return false;
  uint32_t value{};
  std::memcpy(&value, at + 1, sizeof(value));
  at += sizeof(uint8_t) + sizeof(value);
  return value == displacement;
}

// The method at address is a pure forwarder of slot, in any of the three forms (see LOAD_WRAPPED). address is code in the
// interposer; every byte read lies within the longest form.
bool PureForwarder(const void* address, size_t slot) {
  const auto* at = static_cast<const uint8_t*>(address);
  if (!std::equal(LOAD_WRAPPED.begin(), LOAD_WRAPPED.end(), at)) return false;
  at += LOAD_WRAPPED.size() + sizeof(uint8_t);  // past the wrapped object's offset
  if (std::equal(LOAD_VTABLE_R10.begin(), LOAD_VTABLE_R10.end(), at)) {
    at += LOAD_VTABLE_R10.size();
    if (at[0] != REX_WB || at[1] != JMP_INDIRECT) return false;
    at += sizeof(uint8_t) + sizeof(uint8_t);
    return SlotDisplacement(at, MODRM_JMP_R10_DISP8, MODRM_JMP_R10_DISP32, slot);
  }
  if (!std::equal(LOAD_VTABLE_RAX.begin(), LOAD_VTABLE_RAX.end(), at)) return false;
  at += LOAD_VTABLE_RAX.size();
  if (at[0] == REX_WR && at[1] == MOV_LOAD) {
    at += sizeof(uint8_t) + sizeof(uint8_t);
    return SlotDisplacement(at, MODRM_MOV_R10_RAX_DISP8, MODRM_MOV_R10_RAX_DISP32, slot) && at[0] == REX_WB && at[1] == JMP_INDIRECT &&
           at[2] == MODRM_JMP_R10;
  }
  if (at[0] == REX_W) ++at;
  if (at[0] != JMP_INDIRECT) return false;
  ++at;
  return SlotDisplacement(at, MODRM_JMP_RAX_DISP8, MODRM_JMP_RAX_DISP32, slot);
}

// A DXGI method: an address already in the table is attached; a new one takes the first free entry, with that entry's detour.
template<class Spec>
Claim Attach(ProviderMethod<Spec>& method, void* address) {
  static constexpr auto detours = DetourTable<Spec>(std::make_index_sequence<PROVIDER_IMPLEMENTATIONS>{});
  if (!address) return {ClaimResult::NO_METHOD};
  if (!ProviderOwned(address, nullptr)) return {ClaimResult::OUTSIDE_INTERPOSER};
  if (PureForwarder(address, Spec::SLOT)) return {ClaimResult::PURE_FORWARDER};
  std::lock_guard guard(install_mutex);
  for (size_t entry{}; entry < method.entries.size(); ++entry) {
    auto& hook = method.entries[entry];
    if (const void* target = hook.target.load(std::memory_order_relaxed)) {
      if (target == address) return {ClaimResult::ATTACHED, entry};
      continue;
    }
    const bool installed = PinOwner(address) && InstallDetour(hook, address, detours[entry]);
    return {installed ? ClaimResult::ATTACHED : ClaimResult::INSTALL_FAILED, entry};
  }
  if (!method.warned_full.test_and_set(std::memory_order_relaxed)) {
    try { nr::Logf(nr::LogLevel::WARN, PROVIDER_TABLE_FULL_LINE, Spec::NAME, address); } catch (...) {}
  }
  return {ClaimResult::NO_FREE_HOOK};
}

template<class... Args>
void TraceDxgi(std::format_string<Args...> format, Args&&... args) noexcept {
  if (!observer.diagnostic_dxgi) return;
  try { nr::Logf(nr::LogLevel::INFO, format, std::forward<Args>(args)...); } catch (...) {}
}

const char* ClaimText(const Claim& claim) {
  return claim.result == ClaimResult::ATTACHED ? "attached" : claim.result == ClaimResult::NO_METHOD ? "no method"
      : claim.result == ClaimResult::OUTSIDE_INTERPOSER ? "outside interposer" : claim.result == ClaimResult::PURE_FORWARDER ? "pure forwarder"
      : claim.result == ClaimResult::NO_FREE_HOOK ? "no free hook"
      : "hook installation failed";
}

void TraceMethod(const char* method, void* address, const Claim& claim) {
  char path[MAX_PATH + 1]{};
  const HMODULE module = address ? Owner(address) : nullptr;
  if (module) GetModuleFileNameA(module, path, MAX_PATH);
  const char* file = std::strrchr(path, '\\');
  const char* result = ClaimText(claim);
  const std::string entry = claim.entry < PROVIDER_IMPLEMENTATIONS ? std::to_string(claim.entry) : "none";
  TraceDxgi("Before FG DX12: {} method={} owner={} entry={} result={}", method, address, file ? file + 1 : path, entry, result);
}

void** Vtable(void* object) { return *static_cast<void***>(object); }

template<class Spec>
bool AttachDxgi(ProviderMethod<Spec>& method, void* object) {
  void* const address = Vtable(object)[Spec::SLOT];
  const Claim claim = Attach(method, address);
  if (observer.diagnostic_dxgi) TraceMethod(Spec::NAME, address, claim);
  return claim.Attached();
}

class PresentScope {
 public:
  PresentScope() : nested_(std::exchange(inside_present, true)) {}
  ~PresentScope() { inside_present = nested_; }
  bool Nested() const { return nested_; }
 private:
  bool nested_;
};

// Only the outermost Present observes: a Streamline implementation it calls, hooked or not, runs inside its scope.
template<class Call>
HRESULT ObservedPresent(IDXGISwapChain* chain, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters, Call&& call) {
  const PresentScope scope;
  const bool observe = !scope.Nested() && !(flags & DXGI_PRESENT_TEST);
  if (observe) observer.dxgi_present(chain, flags, parameters);
  const HRESULT result = std::forward<Call>(call)();
  if (observe && result != S_OK && observer.dxgi_retry) observer.dxgi_retry(chain);
  return result;
}

template<size_t Entry>
HRESULT STDMETHODCALLTYPE PresentMethod::Detour(IDXGISwapChain* chain, UINT interval, UINT flags) {
  return ObservedPresent(chain, flags, nullptr, [&] { return present.Original(Entry)(chain, interval, flags); });
}

template<size_t Entry>
HRESULT STDMETHODCALLTYPE Present1Method::Detour(IDXGISwapChain1* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
  return ObservedPresent(chain, flags, parameters, [&] { return present1.Original(Entry)(chain, interval, flags, parameters); });
}

template<size_t Entry>
HRESULT STDMETHODCALLTYPE ResizeMethod::Detour(IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
  const auto call = ProviderScope::Run([&] { return resize.Original(Entry)(chain, count, width, height, format, flags); });
  if (SUCCEEDED(call.result) && call.outermost && observer.dxgi_resize) observer.dxgi_resize(chain, count, nullptr, call.inner);
  return call.result;
}

template<size_t Entry>
HRESULT STDMETHODCALLTYPE Resize1Method::Detour(IDXGISwapChain3* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT* nodes,
                                               IUnknown* const* queues) {
  const auto call = ProviderScope::Run([&] { return resize1.Original(Entry)(chain, count, width, height, format, flags, nodes, queues); });
  if (SUCCEEDED(call.result) && call.outermost && observer.dxgi_resize) {
    if (count == 0u && queues) {
      DXGI_SWAP_CHAIN_DESC1 desc{};
      if (SUCCEEDED(chain->GetDesc1(&desc))) count = desc.BufferCount;
    }
    observer.dxgi_resize(chain, count, queues, call.inner);
  }
  return call.result;
}

// SetColorSpace1 is not hooked: the route reads the colour space ReShade records for the chain beneath it. Streamline's
// proxy method for it is a pure forwarder, folded with other proxy classes' slot 38 (GitHub PR 21).
void BindChain(IDXGISwapChain* chain, IUnknown* device, void* inner) {
  if (!chain) return;
  ComPtr<IDXGISwapChain1> chain1;
  const bool regular = AttachDxgi(present, chain);
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1)))) AttachDxgi(present1, chain1.Get());
  if (observer.dxgi_resize) {
    ComPtr<IDXGISwapChain3> chain3;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) {
      if (!AttachDxgi(resize, chain) || !AttachDxgi(resize1, chain3.Get())) return;
    }
  }
  if (regular) observer.dxgi_created(chain, device, inner);
}

void RequeryOptions(const char* after);

template<class Chain, class Call>
HRESULT Created(const char* method, void* factory_object, IUnknown* device, Chain** output, Call&& original) {
  TraceDxgi("Before FG DX12: enter {} factory={} device={}", method, factory_object, static_cast<void*>(device));
  const auto call = ProviderScope::Run(std::forward<Call>(original));
  TraceDxgi("Before FG DX12: return {} hr={:#x} outer={} ReShade={} outermost={}", method, static_cast<unsigned>(call.result),
            SUCCEEDED(call.result) && output ? static_cast<void*>(*output) : nullptr, call.inner, call.outermost);
  // Only the outermost provider call returns the chain the application presents on; an inner one returns a wrapped chain.
  if (SUCCEEDED(call.result) && output && call.outermost) BindChain(*output, device, call.inner);
  if (SUCCEEDED(call.result) && call.outermost) RequeryOptions(method);
  return call.result;
}

template<size_t Entry>
HRESULT STDMETHODCALLTYPE CreateMethod::Detour(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** output) {
  return Created(NAME, self, device, output, [&] { return create.Original(Entry)(self, device, desc, output); });
}

template<size_t Entry>
HRESULT STDMETHODCALLTYPE CreateHwndMethod::Detour(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrict_output, IDXGISwapChain1** output) {
  return Created(NAME, self, device, output, [&] { return create_hwnd.Original(Entry)(self, device, hwnd, desc, fullscreen, restrict_output, output); });
}

template<size_t Entry>
HRESULT STDMETHODCALLTYPE CreateCompositionMethod::Detour(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict_output,
                                                         IDXGISwapChain1** output) {
  return Created(NAME, self, device, output, [&] { return create_composition.Original(Entry)(self, device, desc, restrict_output, output); });
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
  AttachDxgi(create, base.Get());
  ComPtr<IDXGIFactory2> extended;
  if (SUCCEEDED(base.As(&extended))) {
    AttachDxgi(create_hwnd, extended.Get());
    AttachDxgi(create_composition, extended.Get());
  }
}

int WINAPI OnUpgrade(void** object) {
  TraceDxgi("Before FG DX12: enter slUpgradeInterface object={}", object ? *object : nullptr);
  const int result = upgrade.original.load(std::memory_order_acquire)(object);
  TraceDxgi("Before FG DX12: return slUpgradeInterface result={} object={}", result, result == 0 && object ? *object : nullptr);
  if (result == 0 && object) BindFactory(*object, UPGRADE_EXPORT);
  if (result == 0) RequeryOptions(UPGRADE_EXPORT);
  return result;
}

// Set while Uplift's probe calls a non-system dxgi.dll's export. That export must not reach Streamline's factory export,
// which would hook a factory class for the whole process (issue 18): such a call is refused, not passed on.
thread_local bool inside_wrapper_probe;
// Sets the flag for one call and clears it on every way out: left set by an export that unwinds, it would refuse every
// Streamline factory export the game later calls on this thread, its own included.
class WrapperProbeScope {
 public:
  WrapperProbeScope() { inside_wrapper_probe = true; }
  ~WrapperProbeScope() { inside_wrapper_probe = false; }
  WrapperProbeScope(const WrapperProbeScope&) = delete;
  WrapperProbeScope& operator=(const WrapperProbeScope&) = delete;
};
bool RefusedInProbe(const char* name, void** output) {
  if (!inside_wrapper_probe) return false;
  if (output) *output = nullptr;
  try { nr::Logf(nr::LogLevel::WARN, WRAPPER_CHAIN_REFUSED_LINE, name); } catch (...) {}
  return true;
}

HRESULT WINAPI OnFactory(REFIID iid, void** output) {
  if (RefusedInProbe("CreateDXGIFactory", output)) return PROBE_REFUSED;
  const HRESULT hr = factory.original.load(std::memory_order_acquire)(iid, output);
  if (SUCCEEDED(hr) && output) BindFactory(*output, "CreateDXGIFactory");
  return hr;
}
HRESULT WINAPI OnFactory1(REFIID iid, void** output) {
  if (RefusedInProbe("CreateDXGIFactory1", output)) return PROBE_REFUSED;
  const HRESULT hr = factory1.original.load(std::memory_order_acquire)(iid, output);
  if (SUCCEEDED(hr) && output) BindFactory(*output, "CreateDXGIFactory1");
  return hr;
}
HRESULT WINAPI OnFactory2(UINT flags, REFIID iid, void** output) {
  if (RefusedInProbe("CreateDXGIFactory2", output)) return PROBE_REFUSED;
  const HRESULT hr = factory2.original.load(std::memory_order_acquire)(flags, iid, output);
  if (SUCCEEDED(hr) && output) BindFactory(*output, "CreateDXGIFactory2");
  return hr;
}

struct ReleaseVkTick {
  VkForward& forwarded;
  ~ReleaseVkTick() { forwarded.ReleaseNativeTick(); }
};

VKAPI_ATTR VkResult VKAPI_CALL OnVkPresent(VkQueue queue, const VkPresentInfoKHR* info) {
  const PresentScope scope;
  if (scope.Nested() || !info) return vk_present.original.load(std::memory_order_acquire)(queue, info);
  VkForward forwarded{.info = *info};
  const ReleaseVkTick release_tick{forwarded};
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
  VkSwapchainCreateInfoKHR adjusted = *info;
  const auto call = ProviderScope::Run([&] {
    if (observer.vk_usage) adjusted.imageUsage |= observer.vk_usage(device, *info);
    return vk_create.original.load(std::memory_order_acquire)(device, &adjusted, allocator, chain);
  });
  const VkResult result = call.result;
  if (result == VK_SUCCESS && call.outermost) {
    RequeryOptions(VK_CREATE_NAME);
    if (observer.vk_created) observer.vk_created(device, *chain, adjusted, call.inner);
    if (const auto resolve = vk_device_proc.InstalledOriginal()) {
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

// The viewport's id, if its handle can be read; SEH-guarded because the caller's handle layout is only assumed.
std::optional<uint32_t> ViewportId(const void* viewport) noexcept {
  if (!viewport) return std::nullopt;
  uint32_t id{};
  __try {
    std::memcpy(&id, static_cast<const std::byte*>(viewport) + VIEWPORT_ID_OFFSET, sizeof(id));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return std::nullopt;
  }
  return id;
}

template<class Value>
std::string ReadText(const std::optional<Value>& value) {
  return value ? std::to_string(*value) : std::string(UNREADABLE);
}

// `readable`: the hook recognised the options' type and version; only then are their mode and version read.
void TraceSetOptions(const void* viewport, const OptionsPrefix& options, bool readable, int result, FgMode before, FgMode after) noexcept {
  try {
    const std::optional<uint32_t> id = ViewportId(viewport);
    const std::optional<uint32_t> raw_mode = readable ? std::optional<uint32_t>(options.mode) : std::nullopt;
    const std::optional<size_t> version = readable ? std::optional<size_t>(options.version) : std::nullopt;
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard guard(options_trace.mutex);
    auto& trace = options_trace;
    if (raw_mode) ++trace.calls[std::min<size_t>(*raw_mode, COUNTED_RAW_MODES)];
    else ++trace.unreadable;
    if (result == SL_RESULT_WARN_OUT_OF_VRAM) ++trace.warned;
    else if (result != SL_RESULT_OK) ++trace.refused;
    const bool changed = !trace.has_last || trace.last_viewport != viewport || trace.last_id != id || trace.last_raw_mode != raw_mode
        || trace.last_result != result || before != after;
    trace.has_last = true;
    trace.last_viewport = viewport;
    trace.last_id = id;
    trace.last_raw_mode = raw_mode;
    trace.last_result = result;
    if (changed && trace.change_lines < MAX_OPTIONS_CHANGE_LINES) {
      ++trace.change_lines;
      TraceDxgi(OPTIONS_CALL_LINE, viewport, ReadText(id), ReadText(raw_mode), ReadText(version), result, ModeName(before), ModeName(after));
      if (trace.change_lines == MAX_OPTIONS_CHANGE_LINES) TraceDxgi(OPTIONS_CHANGE_CAP_LINE);
    }
    if (!trace.summary_at) trace.summary_at = now;
    if (now - *trace.summary_at >= OPTIONS_SUMMARY_PERIOD) {
      TraceDxgi(OPTIONS_SUMMARY_LINE, trace.calls[DLSSG_MODE_OFF], trace.calls[DLSSG_MODE_ON], trace.calls[DLSSG_MODE_AUTO], trace.calls[DLSSG_MODE_DYNAMIC],
                trace.calls[COUNTED_RAW_MODES], trace.unreadable, trace.warned, trace.refused, ModeName(after));
      trace.calls = {};
      trace.unreadable = 0u;
      trace.warned = 0u;
      trace.refused = 0u;
      trace.summary_at = now;
    }
  } catch (...) {}
}

int OnSetOptions(const void* viewport, const OptionsPrefix& options) {
  const int result = set_options.original.load(std::memory_order_acquire)(viewport, options);
  const FgMode before = mode.load(std::memory_order_acquire);
  // Options this prefix cannot read say nothing of the request, so they store UNKNOWN as a refused call does: no call leaves a stale mode.
  const bool readable = options.type == OPTIONS_TYPE && options.version >= 1;
  mode.store(StoredMode(readable ? ModeFromRaw(options.mode) : FgMode::UNKNOWN, result), std::memory_order_release);
  if (observer.diagnostic_dxgi) TraceSetOptions(viewport, options, readable, result, before, mode.load(std::memory_order_acquire));
  return result;
}

// Takes the slDLSSGSetOptions hook out of the DLSS-G plug-in while it is still loaded. Disabled, not removed: a call already
// inside OnSetOptions still returns through its trampoline. A hook MinHook cannot disable is abandoned: its entry is never
// touched again, and the plug-in unloads with the patch, which goes with its image.
void ReleaseSetOptions() {
  void* target = nullptr;
  bool plugin_gone = false;
  MH_STATUS status = MH_OK;
  {
    const std::lock_guard guard(install_mutex);
    target = set_options.target.exchange(nullptr, std::memory_order_acq_rel);
    if (!target) {
      // No hook in this instance (its answer could not be hooked): the next one is still asked.
      if (options_attached_once) {
        requery_options.store(true, std::memory_order_release);
        ++options_releases;
      }
      return;
    }
    // Only the plug-in that was hooked may be written: another module at that address, or none, is left alone.
    plugin_gone = Owner(target) != std::exchange(options_module, nullptr);
    if (!plugin_gone) status = MH_DisableHook(target);
    if (!plugin_gone && status == MH_OK) released_options = target;
    // Set under the mutex that AttachSetOptions clears it under, so a release and an attach cannot leave it stale.
    requery_options.store(true, std::memory_order_release);
    ++options_releases;
  }
  try {
    if (plugin_gone) nr::Logf(nr::LogLevel::WARN, OPTIONS_PLUGIN_GONE_LINE, target);
    else if (status != MH_OK) nr::Logf(nr::LogLevel::WARN, OPTIONS_RELEASE_FAILED_LINE, target, MH_StatusToString(status));
    else TraceDxgi(OPTIONS_RELEASED_LINE, target);
  } catch (...) {}
}

// Streamline's slShutdown unloads its plug-ins (unloadPlugins, the only unload once a plug-in has started). The mode stored
// so far belonged to the instance that ends here, and the DLSS-G plug-in must be left unpatched so that it unloads: a plug-in
// that outlives slShutdown keeps the state slOnPluginLoad built for this instance, pointing into the sl.common Streamline
// unloads, and the next slInit reuses it (Phantom Blade Zero, 17 Nov 2026, from another add-on's permanent lease).
int OnShutdown() {
  if (observer.sl_shutdown) observer.sl_shutdown();
  mode.store(FgMode::UNKNOWN, std::memory_order_release);
  ReleaseSetOptions();
  return sl_shutdown.original.load(std::memory_order_acquire)();
}

// slDLSSGSetOptions lives in the DLSS-G plug-in, so unlike an interposer export it is never pinned, and it is hooked only
// once Uplift's slShutdown hook is in place to take the hook out first. The interposer that served the function is already
// pinned by Uplift's slGetFeatureFunction hook. A hook slShutdown released is removed before the next one is created: the
// reloaded plug-in often comes back at the same address.
bool AttachSetOptions(void* address) {
  if (!address || !ProviderOwned(address, GetModuleHandleW(DLSSG_PLUGIN_FILE))) return false;
  const HMODULE module = interposer.load(std::memory_order_acquire);
  const auto resolve = get_proc.original.load(std::memory_order_acquire);
  if (!module || !resolve || !Attach(sl_shutdown, reinterpret_cast<void*>(resolve(module, SHUTDOWN_EXPORT)), &OnShutdown)) return false;
  const std::lock_guard guard(install_mutex);
  if (const void* target = set_options.target.load(std::memory_order_relaxed)) return target == address;
  if (released_options) MH_RemoveHook(std::exchange(released_options, nullptr));
  if (!InstallDetour(set_options, address, &OnSetOptions)) return false;
  options_module = Owner(address);
  options_attached_once = true;
  requery_options.store(false, std::memory_order_release);
  return true;
}

int OnGetFeature(uint32_t feature, const char* name, void*& output) {
  const int hr = get_feature.original.load(std::memory_order_acquire)(feature, name, output);
  if (hr == 0 && name && !std::strcmp(name, SET_OPTIONS_NAME)) {
    const bool attached = AttachSetOptions(output);
    if (observer.diagnostic_dxgi) {
      char path[MAX_PATH + 1]{};
      if (const HMODULE module = output ? Owner(output) : nullptr) GetModuleFileNameA(module, path, MAX_PATH);
      const char* file = std::strrchr(path, '\\');
      TraceDxgi(OPTIONS_HOOK_LINE, hr, output, file ? file + 1 : path, attached);
    }
  } else if (name && !std::strcmp(name, SET_OPTIONS_NAME) && observer.diagnostic_dxgi) {
    TraceDxgi(OPTIONS_HOOK_LINE, hr, output, NO_OWNER, false);
  }
  return hr;
}

// After a slShutdown released the slDLSSGSetOptions hook, Uplift asks for the function again at points every restarted host
// passes on its own thread, with no new hook: a successful slUpgradeInterface, and the outermost swapchain creation once it
// has returned, DXGI or Vulkan. A restart is followed by a new swapchain. Outermost only, so Uplift never calls Streamline
// from inside Streamline's own creation. It calls the export as the game does, through whatever hooks are on it now, never
// Uplift's saved trampoline: a third party hooked before Uplift may have been unloaded since (RenoDX, reloaded with ReShade's
// add-ons). Before slInit the query fails and the flag stays set for the next one. Once Streamline has answered, the flag
// is cleared even when the answer cannot be hooked (another module's function, a plug-in back at an abandoned address, or a
// failed install): asking again would get the same answer at every later upgrade and creation of that instance. The next
// slShutdown arms it again (options_attached_once).
void RequeryOptions(const char* after) {
  if (!requery_options.load(std::memory_order_acquire)) return;
  unsigned releases = 0u;
  {
    const std::lock_guard guard(install_mutex);
    releases = options_releases;
  }
  const HMODULE module = interposer.load(std::memory_order_acquire);
  const auto resolve = get_proc.original.load(std::memory_order_acquire);
  const auto query = module && resolve ? reinterpret_cast<GetFeature>(resolve(module, GET_FEATURE_EXPORT)) : nullptr;
  if (!query) return;
  void* function = nullptr;
  const int result = query(DLSSG_FEATURE, SET_OPTIONS_NAME, function);
  // Through Uplift's own slGetFeatureFunction hook the function is attached already; AttachSetOptions then only confirms it.
  const bool hooked = result == SL_RESULT_OK && AttachSetOptions(function);
  if (result == SL_RESULT_OK && !hooked) {
    const std::lock_guard guard(install_mutex);
    if (options_releases == releases) requery_options.store(false, std::memory_order_release);
  }
  TraceDxgi(OPTIONS_REQUERY_LINE, after, result, function, hooked);
}

// HUD-less (design §2): the tag hooks forward the call unchanged, then report it. Nothing here calls Streamline but the original,
// takes a lock of this unit's, or lets an exception out. A null frame token is forwarded and not reported (a game may pass one).
int OnSetTag(const void* viewport, const void* tags, uint32_t count, void* command_buffer) {
  const int result = set_tag.original.load(std::memory_order_acquire)(viewport, tags, count, command_buffer);
  try {
    if (observer.hud_tags) observer.hud_tags(viewport, tags, count);
  } catch (...) {
  }
  return result;
}

int OnSetTagForFrame(const void* frame, const void* viewport, const void* tags, uint32_t count, void* command_buffer) {
  const int result = set_tag_for_frame.original.load(std::memory_order_acquire)(frame, viewport, tags, count, command_buffer);
  const void* volatile address = frame;  // a reference in the SDK: tested so the null test is not folded away
  try {
    if (address && observer.hud_tags_for_frame) observer.hud_tags_for_frame(address, viewport, tags, count);
  } catch (...) {
  }
  return result;
}

void BindExports(HMODULE module) {
  const auto resolve = [&](const char* name) { return reinterpret_cast<void*>(get_proc.original.load(std::memory_order_acquire)(module, name)); };
  // Streamline v2 returns a Result from slUpgradeInterface, and its tag exports have the signatures above. Do not attach this ABI to v1.
  if (resolve("slGetNativeInterface") && resolve(GET_FEATURE_EXPORT)) {
    Attach(upgrade, resolve(UPGRADE_EXPORT), &OnUpgrade);
    Attach(set_tag, resolve(SET_TAG_EXPORT), &OnSetTag);
    Attach(set_tag_for_frame, resolve(SET_TAG_FOR_FRAME_EXPORT), &OnSetTagForFrame);
    // HUD-less: the stored tags are released at slShutdown, so slShutdown is hooked with them, not only with slDLSSGSetOptions.
    Attach(sl_shutdown, resolve(SHUTDOWN_EXPORT), &OnShutdown);
  }
  Attach(get_feature, resolve(GET_FEATURE_EXPORT), &OnGetFeature);
  if (get_feature.InstalledOriginal()) {
    void* function = nullptr;
    OnGetFeature(DLSSG_FEATURE, SET_OPTIONS_NAME, function);
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
int FirstSetTag(const void* v, const void* t, uint32_t c, void* b) { return FirstCall<&OnSetTag>(set_tag, SET_TAG_EXPORT, v, t, c, b); }
int FirstSetTagForFrame(const void* f, const void* v, const void* t, uint32_t c, void* b) {
  return FirstCall<&OnSetTagForFrame>(set_tag_for_frame, SET_TAG_FOR_FRAME_EXPORT, f, v, t, c, b);
}
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
  else if (!std::strcmp(name, SET_TAG_EXPORT)) replacement = reinterpret_cast<FARPROC>(&FirstSetTag);
  else if (!std::strcmp(name, SET_TAG_FOR_FRAME_EXPORT)) replacement = reinterpret_cast<FARPROC>(&FirstSetTagForFrame);
  else if (!std::strcmp(name, "CreateDXGIFactory")) replacement = reinterpret_cast<FARPROC>(&FirstFactory);
  else if (!std::strcmp(name, "CreateDXGIFactory1")) replacement = reinterpret_cast<FARPROC>(&FirstFactory1);
  else if (!std::strcmp(name, "CreateDXGIFactory2")) replacement = reinterpret_cast<FARPROC>(&FirstFactory2);
  else if (!std::strcmp(name, "vkGetDeviceProcAddr")) replacement = reinterpret_cast<FARPROC>(&FirstDeviceProc);
  else if (!std::strcmp(name, "vkGetInstanceProcAddr")) replacement = reinterpret_cast<FARPROC>(&FirstInstanceProc);
  else if (!std::strcmp(name, "vkQueuePresentKHR")) replacement = reinterpret_cast<FARPROC>(&FirstVkPresent);
  if (!replacement || !IsInterposer(module) || Owner(reinterpret_cast<void*>(result)) != module) return result;
  const bool v2_only = (replacement == reinterpret_cast<FARPROC>(&FirstUpgrade) || replacement == reinterpret_cast<FARPROC>(&FirstSetTag)
                        || replacement == reinterpret_cast<FARPROC>(&FirstSetTagForFrame));
  if (v2_only && (!get_proc.original.load(std::memory_order_acquire)(module, "slGetNativeInterface") || !get_proc.original.load(std::memory_order_acquire)(module, "slGetFeatureFunction"))) return result;
  HMODULE empty = nullptr;
  interposer.compare_exchange_strong(empty, module);
  return empty && empty != module ? result : replacement;
}

// The system's dxgi.dll, found by full path: a dxgi.dll in the game folder (ReShade or another wrapper) shares the base
// name. Null when the system DXGI is not loaded.
HMODULE SystemDxgi() {
  wchar_t path[MAX_PATH]{};
  const UINT length = GetSystemDirectoryW(path, MAX_PATH);
  if (length == 0u || length >= MAX_PATH || wcscat_s(path, SYSTEM_DXGI_FILE) != 0) return nullptr;
  return GetModuleHandleW(path);
}

void* FactoryExport(HMODULE module) {
  return module ? reinterpret_cast<void*>(get_proc.original.load(std::memory_order_acquire)(module, FACTORY1_EXPORT)) : nullptr;
}

// CreateDXGIFactory1 of the system's dxgi.dll; null when the system DXGI is not loaded or Streamline owns the export.
Factory SystemFactory1(HMODULE system) {
  void* const function = FactoryExport(system);
  return function && !IsInterposer(Owner(function)) ? reinterpret_cast<Factory>(function) : nullptr;
}

// One section of a mapped image: where it starts in memory, its mapped size and its characteristics.
struct ImageSection {
  const uint8_t* start;
  size_t size;
  DWORD characteristics;
};
struct CompleteObjectLocator {
  uint32_t signature;
  uint32_t offset;
  uint32_t class_offset;
  int32_t type_descriptor;
  int32_t class_descriptor;
  int32_t self;
};

std::vector<ImageSection> Sections(HMODULE module) {
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
  std::vector<ImageSection> sections;
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  for (WORD index{}; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
    sections.push_back({base + section->VirtualAddress, section->Misc.VirtualSize, section->Characteristics});
  }
  return sections;
}

// The primary vtable of the class whose RTTI type name is name (with its NUL), read from the mapped image only; null unless
// the name, its offset-0 locator and the vtable that points at it are each found exactly once.
void** VtableByType(HMODULE module, std::string_view name) {
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  const auto sections = Sections(module);
  const uint8_t* found_name = nullptr;
  for (const auto& section : sections) {
    const auto* end = section.start + section.size;
    for (const auto* at = std::search(section.start, end, name.begin(), name.end()); at != end; at = std::search(at + 1, end, name.begin(), name.end())) {
      if (found_name) return nullptr;
      found_name = at;
    }
  }
  if (!found_name || found_name - base < static_cast<ptrdiff_t>(TYPE_DESCRIPTOR_NAME_OFFSET)) return nullptr;
  const auto descriptor = static_cast<int32_t>(found_name - base - TYPE_DESCRIPTOR_NAME_OFFSET);
  const uint8_t* locator = nullptr;
  for (const auto& section : sections) {
    if (section.characteristics & READ_ONLY_DATA_EXCLUDED || section.size < sizeof(CompleteObjectLocator)) continue;
    for (size_t at{}; at + sizeof(CompleteObjectLocator) <= section.size; at += alignof(CompleteObjectLocator)) {
      CompleteObjectLocator candidate{};
      std::memcpy(&candidate, section.start + at, sizeof(candidate));
      if (candidate.signature != LOCATOR_SIGNATURE || candidate.offset != PRIMARY_VTABLE_OFFSET || candidate.type_descriptor != descriptor
          || candidate.self != static_cast<int32_t>(section.start + at - base)) continue;
      if (locator) return nullptr;
      locator = section.start + at;
    }
  }
  if (!locator) return nullptr;
  const auto locator_address = reinterpret_cast<uintptr_t>(locator);
  void** vtable = nullptr;
  for (const auto& section : sections) {
    if (section.characteristics & READ_ONLY_DATA_EXCLUDED) continue;
    for (size_t at{}; at + VTABLE_CANDIDATE_BYTES <= section.size; at += sizeof(void*)) {
      uintptr_t value{};
      std::memcpy(&value, section.start + at, sizeof(value));
      if (value != locator_address) continue;
      if (vtable) return nullptr;
      vtable = reinterpret_cast<void**>(const_cast<uint8_t*>(section.start + at + sizeof(void*)));
    }
  }
  return vtable;
}

template<class Spec>
std::string AttachSlot(ProviderMethod<Spec>& method, void** vtable) {
  void* const address = vtable[Spec::SLOT];
  const Claim claim = Attach(method, address);
  if (observer.diagnostic_dxgi) TraceMethod(Spec::NAME, address, claim);
  return std::string(Spec::NAME) + METHOD_RESULT_SEPARATOR + ClaimText(claim);
}

void LogProxyClass(const char* name, HMODULE module, void** vtable, const std::string& methods) {
  try {
    if (!vtable) nr::Logf(nr::LogLevel::INFO, PROXY_CLASS_MISSING_LINE, name);
    else nr::Logf(nr::LogLevel::INFO, PROXY_CLASS_LINE, name, reinterpret_cast<uintptr_t>(vtable) - reinterpret_cast<uintptr_t>(module), methods);
  } catch (...) {}
}

// Streamline's proxy factory and proxy swapchain, found by type in the interposer's image: their creation, Present and
// resize methods are attached before the game makes a chain through them, however it reached them (an upgrade before
// Uplift started, a third party's wrapper around the proxy). Attach's rules apply unchanged: only interposer code, never a
// pure forwarder, first free entry. True when either class was found.
bool BindProxyClasses(HMODULE module) {
  void** const factory_table = VtableByType(module, std::string_view(FACTORY_TYPE_NAME, sizeof(FACTORY_TYPE_NAME)));
  std::string factory_methods;
  if (factory_table) {
    factory_methods = AttachSlot(create, factory_table) + METHOD_LIST_SEPARATOR + AttachSlot(create_hwnd, factory_table) + METHOD_LIST_SEPARATOR
                      + AttachSlot(create_composition, factory_table);
  }
  LogProxyClass(FACTORY_CLASS, module, factory_table, factory_methods);
  void** const chain_table = VtableByType(module, std::string_view(SWAPCHAIN_TYPE_NAME, sizeof(SWAPCHAIN_TYPE_NAME)));
  std::string chain_methods;
  if (chain_table) {
    chain_methods = AttachSlot(present, chain_table) + METHOD_LIST_SEPARATOR + AttachSlot(present1, chain_table);
    // As BindChain: the resizes only for an observer that takes them.
    if (observer.dxgi_resize) chain_methods += METHOD_LIST_SEPARATOR + AttachSlot(resize, chain_table) + METHOD_LIST_SEPARATOR + AttachSlot(resize1, chain_table);
  }
  LogProxyClass(SWAPCHAIN_CLASS, module, chain_table, chain_methods);
  return factory_table || chain_table;
}

// Set once in Poll, before the probes: Streamline's proxy classes were found by type, so no probe upgrade is needed.
bool proxy_classes_found = false;

// A system factory, and the same through slUpgradeInterface. The system class shows the vtable hooks Streamline installed
// when the dxgi.dll it resolved was the system's.
void BindSystemProbes(HMODULE system) {
  const Factory create_factory = SystemFactory1(system);
  if (!create_factory) {
    TraceDxgi(SYSTEM_PROBE_SKIPPED_LINE, static_cast<void*>(system));
    return;
  }
  ComPtr<IDXGIFactory2> raw;
  if (const HRESULT hr = create_factory(IID_PPV_ARGS(&raw)); FAILED(hr)) {
    TraceDxgi(SYSTEM_PROBE_FAILED_LINE, static_cast<unsigned>(hr));
    return;
  }
  BindFactory(raw.Get(), RAW_PROBE_SOURCE);
  // The fallback for an interposer whose proxy classes were not found by type: a host that upgraded its factory before Uplift
  // started does not call slUpgradeInterface again, and every Streamline proxy factory shares one proxy's methods. A third
  // party that hooks slUpgradeInterface sees this upgrade and may wrap its result (MFG Unlocker), which is why it is only a
  // fallback. The proxy holds its own reference to raw and releases it with its last Release; an unchanged pointer means no
  // proxy was made. Fails before slInit, when nothing holds a proxy yet.
  if (proxy_classes_found) {
    TraceDxgi(PROBE_UPGRADE_NOT_NEEDED_LINE);
    return;
  }
  const auto upgrade_original = upgrade.InstalledOriginal();
  if (!upgrade_original) {
    TraceDxgi(PROBE_NO_UPGRADE_LINE);
    return;
  }
  void* object = raw.Get();
  const int result = upgrade_original(&object);
  TraceDxgi(PROBE_UPGRADE_LINE, result, static_cast<void*>(raw.Get()), object);
  if (result != SL_RESULT_OK || object == raw.Get()) return;
  BindFactory(object, UPGRADED_PROBE_SOURCE);
  static_cast<IUnknown*>(object)->Release();
}

// Every loaded dxgi.dll that is not the system's. A process holds hundreds of modules, so the list is sized from the count
// K32EnumProcessModules reports, and asked again when modules loaded in between.
std::vector<HMODULE> NonSystemDxgi(HMODULE system) {
  const HANDLE process = GetCurrentProcess();
  std::vector<HMODULE> all;
  DWORD needed = 0u;
  for (int attempt{}; attempt < MODULE_LIST_ATTEMPTS; ++attempt) {
    const DWORD bytes = static_cast<DWORD>(all.size() * sizeof(HMODULE));
    if (!K32EnumProcessModules(process, all.data(), bytes, &needed)) {
      TraceDxgi(MODULE_LIST_FAILED_LINE, GetLastError());
      const HMODULE by_name = GetModuleHandleW(DXGI_FILE_NAME);
      return by_name && by_name != system ? std::vector<HMODULE>{by_name} : std::vector<HMODULE>{};
    }
    if (needed <= bytes) break;
    all.resize(needed / sizeof(HMODULE));
  }
  all.resize(std::min<size_t>(all.size(), needed / sizeof(HMODULE)));
  std::vector<HMODULE> found;
  for (const HMODULE module : all) {
    // A slot the last list did not fill is null, and a null module names the executable: skipped.
    wchar_t name[MAX_PATH]{};
    if (module && module != system && K32GetModuleBaseNameW(process, module, name, MAX_PATH) && _wcsicmp(name, DXGI_FILE_NAME) == 0) found.push_back(module);
  }
  return found;
}

// A factory made through each non-system dxgi.dll's own export, so its class shows the hooks Streamline put there. A
// module whose export is the system DXGI's (a forwarding stub, probed already) or Streamline's is not probed. ReShade hands
// back the system factory unwrapped while its own D3D12CreateDevice runs; the trace names whose class the factory has.
void BindNonSystemProbes(HMODULE system) {
  for (const HMODULE module : NonSystemDxgi(system)) {
    char path[MAX_PATH + 1]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    void* const function = FactoryExport(module);
    if (!function) {
      TraceDxgi(NON_SYSTEM_NO_EXPORT_LINE, path);
      continue;
    }
    const HMODULE owner = Owner(function);
    if (owner != module) {
      char owner_path[MAX_PATH + 1]{};
      if (owner) GetModuleFileNameA(owner, owner_path, MAX_PATH);
      TraceDxgi(NON_SYSTEM_SKIPPED_LINE, path, owner_path);
      continue;
    }
    ComPtr<IDXGIFactory2> wrapped;
    const HRESULT hr = [&] {
      const WrapperProbeScope scope;
      return reinterpret_cast<Factory>(function)(IID_PPV_ARGS(&wrapped));
    }();
    const HMODULE class_owner = SUCCEEDED(hr) && wrapped ? Owner(Vtable(wrapped.Get())[QUERY_INTERFACE_SLOT]) : nullptr;
    TraceDxgi(NON_SYSTEM_PROBE_LINE, path, static_cast<unsigned>(hr),
              class_owner == module ? WRAPPER_CLASS : class_owner == system ? SYSTEM_CLASS : OTHER_CLASS);
    if (SUCCEEDED(hr) && wrapped) BindFactory(wrapped.Get(), NON_SYSTEM_PROBE_SOURCE);
  }
}

// ReShade loads add-ons at device creation, after a statically linked application may already have obtained its factory,
// so the factory's Streamline methods are found from probes. No probe comes from a Streamline export: where the host does
// not set eUseDXGIFactoryProxy (Streamline's default), Streamline's CreateDXGIFactory* overwrites slots 10, 15, 16 and 24
// of a DXGI factory class vtable for the whole process, and a factory wrapper such as MFG Unlocker then routes DLSS-G's own
// swapchain creation back into DLSS-G's hook (issue 18). The class it overwrites is the one of the dxgi.dll a base-name
// lookup finds: the system's when ReShade is injected or named otherwise (its inline hook then wraps what the system
// export returns), a non-system dxgi.dll's when ReShade or another wrapper is the game's dxgi.dll. Both are probed.
void BindProbes() {
  const HMODULE system = SystemDxgi();
  BindSystemProbes(system);
  BindNonSystemProbes(system);
}

}  // namespace

bool Start(HMODULE self, const Observer& callbacks) {
  if (enabled.load()) return true;
  if (hooks::EnsureMinHook() != MH_OK) return false;
  HMODULE pin{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(self), &pin)) return false;
  void* address = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));
  if (!address || !InstallDetour(get_proc, address, &OnGetProc)) return false;
  observer = callbacks;
  TraceDxgi("Before FG DX12: diagnostic enabled");
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
  proxy_classes_found = BindProxyClasses(module);
  BindProbes();
  nr::Logf(nr::LogLevel::INFO, "Before FG hooks: upgrade={} DXGI factories={}/{}/{} creation={}/{}/{} Vulkan resolver={}/{} vkQueuePresentKHR={} tags={}/{}",
           upgrade.target != nullptr, factory.target != nullptr, factory1.target != nullptr, factory2.target != nullptr,
           create.Attached(), create_hwnd.Attached(), create_composition.Attached(),
           vk_instance_proc.target != nullptr, vk_device_proc.target != nullptr, vk_present.target != nullptr,
           set_tag.target != nullptr, set_tag_for_frame.target != nullptr);
}

void NoteReshadeSwapchain(void* chain) {
  ProviderScope::NoteReshade(chain);
  if (Installed()) TraceDxgi("Before FG trace: ReShade init swapchain={} inside provider creation={}", chain, ProviderScope::Open());
}
bool WatchGameFacing(IDXGISwapChain* chain, bool trace_method) {
  if (!chain || !Installed() || inside_present) return false;
  void* method = Vtable(chain)[PresentMethod::SLOT];
  const Claim claim = Attach(present, method);
  if (trace_method && observer.diagnostic_dxgi) TraceMethod("ReShade native Present", method, claim);
  if (!claim.Attached()) return false;
  ComPtr<IDXGISwapChain1> chain1;
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1)))) Attach(present1, Vtable(chain1.Get())[Present1Method::SLOT]);
  return true;
}
bool InsideOuterPresent() { return inside_present; }
bool Installed() { return enabled.load(std::memory_order_acquire); }
bool StreamlineLoaded() { return interposer.load(std::memory_order_acquire) != nullptr; }
FgMode Mode() { return mode.load(std::memory_order_acquire); }
std::string_view ModeName(FgMode value) { return FG_MODE_NAMES[static_cast<size_t>(value)]; }

}  // namespace uplift::addon::streamline
