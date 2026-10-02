#include "ipc/settings_text.hpp"

namespace uplift::ipc {

std::optional<std::string> TextConfigStore::Get(std::string_view key) const {
  const auto found = values_.find(key);
  if (found == values_.end()) return std::nullopt;
  return found->second;
}

void TextConfigStore::Set(std::string_view key, std::string_view value) {
  values_.insert_or_assign(std::string(key), std::string(value));
}

std::string TextConfigStore::Serialize() const {
  std::string text;
  for (const auto& [key, value] : values_) {
    text += key;
    text += '=';
    text += value;
    text += '\n';
  }
  return text;
}

TextConfigStore TextConfigStore::Parse(std::string_view text) {
  TextConfigStore store;
  while (!text.empty()) {
    const size_t newline = text.find('\n');
    std::string_view line = text.substr(0u, newline);
    text = (newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1u));
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1u);
    }
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos || equals == 0u) continue;
    store.Set(line.substr(0u, equals), line.substr(equals + 1u));
  }
  return store;
}

}  // namespace uplift::ipc
