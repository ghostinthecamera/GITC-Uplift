#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "ui/settings.hpp"

namespace uplift::addon {

// ui::ConfigStore over ReShade.ini [Uplift], through ReShade's global config API.
class ReshadeConfigStore final : public ui::ConfigStore {
 public:
  [[nodiscard]] std::optional<std::string> Get(std::string_view key) const override;
  void Set(std::string_view key, std::string_view value) override;
};

}  // namespace uplift::addon
