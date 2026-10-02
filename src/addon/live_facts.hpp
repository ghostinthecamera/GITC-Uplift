#pragma once

// Plan 14 (design §1.6): the facts the add-ons give Setup and the working status card (ui::SetupFacts), from the settings and from what the shown
// context reports. Both add-ons call these from their overlay handlers; the rest of the facts (what is possible on this API, the notes) each host fills
// itself, because they come from its own device entry.

#include "ipc/protocol.hpp"
#include "ui/settings.hpp"
#include "ui/status_text.hpp"
#if defined(_WIN64)
#include "addon/device_context.hpp"
#endif

namespace uplift::addon {

// The stored preference, read and never written. `explicit_dlss`: a Vulkan device, where Auto stays at Present (ui::SourcePickOf).
void FillPreference(ui::SetupFacts* facts, const ui::Settings& settings, bool explicit_dlss);

// What runs: the decided placement (none for NONE), the latest recording's motion provider only while `working` (the card says NR works), the gaps, the
// applied resolution and the sizes, from a context's status (64-bit add-on) or the helper's (the 32-bit add-on, and Direct3D 9 in the 64-bit one).
#if defined(_WIN64)
void FillRunning(ui::SetupFacts* facts, const ContextStatus& status, bool working);
#endif
void FillRunning(ui::SetupFacts* facts, const ipc::Status& status, bool working);

}  // namespace uplift::addon
