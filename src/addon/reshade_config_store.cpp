#include "addon/reshade_config_store.hpp"

#include <algorithm>

#include "addon/reshade_api.hpp"

namespace uplift::addon {

std::optional<std::string> ReshadeConfigStore::Get(std::string_view key) const {
  const std::string key_text(key);
  size_t size = 0u;
  if (!reshade::get_config_value(nullptr, ui::CONFIG_SECTION, key_text.c_str(), nullptr, &size)) return std::nullopt;
  std::string value(size, '\0');
  if (!reshade::get_config_value(nullptr, ui::CONFIG_SECTION, key_text.c_str(), value.data(), &size)) return std::nullopt;
  value.resize(size);
  while (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  // ReShade splits a value at commas into NUL-separated elements; a path may contain commas.
  std::ranges::replace(value, '\0', ',');
  return value;
}

void ReshadeConfigStore::Set(std::string_view key, std::string_view value) {
  reshade::set_config_value(nullptr, ui::CONFIG_SECTION, std::string(key).c_str(), std::string(value).c_str());
}

}  // namespace uplift::addon
