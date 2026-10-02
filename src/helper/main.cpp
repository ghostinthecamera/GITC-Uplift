// Plan 9 (design §2.1, §2.2): gitc-uplift-helper64.exe, the 64-bit process that runs NR for a 32-bit game. It has no window and no
// UI. gitc-uplift.addon32 starts it in a kill-on-close job with four inherited handles (the control block, the request and reply
// events, and a SYNCHRONIZE handle to the game), passed on the command line as hex values:
//   gitc-uplift-helper64.exe --block H --request H --reply H --game H
// Exit codes: 0 after QUIT or 30 s idle; 2 a bad command line; 3 a build or protocol mismatch; 4 no D3D12 device.
#include <Windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <string>
#include <thread>

#include "bridge/d3d12_side.hpp"
#include "build_id.hpp"
#include "helper/helper.hpp"
#include "ipc/control_block.hpp"
#include "ipc/protocol.hpp"
#include "nr/log.hpp"

namespace {

namespace ipc = uplift::ipc;

HANDLE ParseHandle(const wchar_t* text) {
  return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::wcstoull(text, nullptr, 16)));
}

// The block's helper_state, published with a barrier: the add-on polls it from its own process.
void Publish(ipc::ControlBlock* block, ipc::HelperState state) {
  InterlockedExchange(reinterpret_cast<volatile LONG*>(&block->helper_state), static_cast<LONG>(state));
}

int Fail(ipc::ControlBlock* block, std::string_view why, int code) {
  ipc::CopyText(block->helper_error, why);
  Publish(block, ipc::HelperState::FAILED);
  return code;
}

#ifdef UPLIFT_HELPER_IDENTITY_NR
// The test build only. UPLIFT_D3D12_DEBUG_LAYER=1, the GPU tests' own switch (the launched helper inherits the test process's
// environment), turns the D3D12 debug layer on in the helper too, before its device exists. Each error it reports goes to the
// log ring as "D3D12 debug layer: ...", which the helper GPU test checks for.
bool EnableDebugLayerIfAsked() {
  wchar_t buffer[8] = {};
  if (GetEnvironmentVariableW(L"UPLIFT_D3D12_DEBUG_LAYER", buffer, static_cast<DWORD>(std::size(buffer))) == 0u
      || std::wcscmp(buffer, L"1") != 0) {
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12Debug> debug;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) return false;
  debug->EnableDebugLayer();
  return true;
}

void LogDebugLayerErrors(ID3D12Device* device) {
  Microsoft::WRL::ComPtr<ID3D12InfoQueue1> queue;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
  DWORD cookie = 0u;
  queue->RegisterMessageCallback(
      [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR description, void*) {
        if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          uplift::nr::Logf(uplift::nr::LogLevel::ERR, "D3D12 debug layer: {}", description);
        }
      },
      D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
}
#endif

}  // namespace

int APIENTRY wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*command_line*/, int /*show*/) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  HANDLE mapping = nullptr;
  HANDLE game = nullptr;
  uplift::helper::Events events;
  for (int index = 1; index + 1 < __argc; index += 2) {
    const std::wstring key = __wargv[index];
    HANDLE* target = (key == L"--block"     ? &mapping
                      : key == L"--request" ? &events.request
                      : key == L"--reply"   ? &events.reply
                      : key == L"--game"    ? &game
                                            : nullptr);
    if (target == nullptr) return 2;
    *target = ParseHandle(__wargv[index + 1]);
  }
  if (mapping == nullptr || events.request == nullptr || events.reply == nullptr || game == nullptr) return 2;
  // The whole mapping: a block from another build may be a different size, and only its header is touched then.
  auto* const block = static_cast<ipc::ControlBlock*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0u, 0u, 0u));
  if (block == nullptr) return 2;
  std::string why;
  if (!ipc::HeaderMatches(*block, UPLIFT_BUILD_ID, &why)) {
    return Fail(block, why, 3);
  }
  // The helper never outlives the game: this covers a failed job assignment and a hung main thread (the job is the other
  // guard).
  std::thread([game] {
    WaitForSingleObject(game, INFINITE);
    TerminateProcess(GetCurrentProcess(), 0u);
  }).detach();
  uplift::helper::InstallLogSink(block, uplift::nr::LogLevel::INFO);
  const LUID luid = {.LowPart = block->luid_low, .HighPart = block->luid_high};
  Microsoft::WRL::ComPtr<ID3D12Device> created;  // D3D12Side keeps its own reference
  std::string error;
#ifdef UPLIFT_HELPER_IDENTITY_NR
  const bool debug_layer = EnableDebugLayerIfAsked();
#endif
  std::unique_ptr<uplift::bridge::D3D12Side> side = uplift::bridge::D3D12Side::Create(luid, &created, &error);
  if (side == nullptr) {
    return Fail(block, error, 4);
  }
#ifdef UPLIFT_HELPER_IDENTITY_NR
  if (debug_layer) {
    LogDebugLayerErrors(side->Device());
  }
#endif
  block->helper_pid = GetCurrentProcessId();
  Publish(block, ipc::HelperState::READY);
  uplift::nr::Logf(uplift::nr::LogLevel::INFO, "helper ready (build {})", UPLIFT_BUILD_ID);
  uplift::helper::Helper helper(block, events, std::move(side));
  return helper.Loop();
}
