#pragma once

#include "ngx_hooks/dlss_overrides.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// Minor (fix round 2, user decision): the DLSS-SR page is hidden, and the user does not want its
// hidden keys honoured from a hand-edited ini either -- always no overrides, whatever `settings`
// holds. The signature stays: NgxBridge::BeforeCreate calls this unconditionally, and a later plan
// may bring the page back.
[[nodiscard]] ngx_hooks::SrOverrides ToSrOverrides(const ui::Settings& settings);

}  // namespace uplift::addon
