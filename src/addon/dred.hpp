#pragma once

#include <d3d12.h>

namespace uplift::addon {

// Plan 17 (1.0.1 design §4): Device Removed Extended Data on Uplift's PRIVATE Direct3D 12 devices (the bridges' and the 64-bit helper's), never on a Direct3D
// 12 game's own device, through the retail runtime's DRED settings, without the debug layer.
enum class DredMode { OFF, PAGE_FAULTS, BREADCRUMBS_AND_PAGE_FAULTS };
// The mode for this process's private devices. The first call decides (and logs it) and later ones change nothing: D3D12Side::Create decides before its
// first device, and the DRED cost case's control decides OFF before any. Writes no setting: the two below do, each where the device is made.
void DecideDred(DredMode mode);
// The decided mode on the device factory's own configuration, which its independent devices read (OFF set explicitly). Nothing before a decision.
void ApplyDred(ID3D12DeviceFactory* factory);
// The decided mode on the PROCESS-WIDE settings (D3D12GetDebugInterface), which every device made afterwards by D3D12CreateDevice reads, Uplift's or not
// (a mod's or an overlay's in the same game): so only on the fallback path without a device factory, right before Uplift's own D3D12CreateDevice. Once
// per process; nothing when OFF, undecided, or the interface is missing.
void ApplyDredGlobal();
// After a private device's removal, at ERROR, a handful of lines: each command list still in flight (its name and its queue's, the last breadcrumb it completed
// and the next op, "Uplift NR frame on Uplift private queue: completed 12/20, next: Dispatch"), and for a page fault its address with the names of the
// allocations there, existing and recently freed. One line says so when DRED has nothing.
void LogDred(ID3D12Device* device);

}  // namespace uplift::addon
