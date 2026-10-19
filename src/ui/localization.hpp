#pragma once

#include <span>
#include <string>
#include <string_view>

#include "ui/settings.hpp"

namespace uplift::ui {
struct Translation {
  std::string_view english;
  std::string_view chinese;
};

[[nodiscard]] std::span<const Translation> Translations();
[[nodiscard]] std::string Localize(Language language, std::string_view text);
[[nodiscard]] std::string LocalizedLabel(Language language, std::string_view label);
}  // namespace uplift::ui
