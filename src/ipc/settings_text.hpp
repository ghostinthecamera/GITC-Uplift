#pragma once

// Plan 9 (design §2.1): the settings cross the process boundary as text. The add-on runs ui::SaveSettings into a
// TextConfigStore and copies Serialize() into ControlBlock::settings_text; the helper parses that text back and runs
// ui::LoadSettings over it.

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "ui/settings.hpp"

namespace uplift::ipc {

// An in-memory ui::ConfigStore: "key=value" lines, keys sorted. Values hold no newline.
class TextConfigStore final : public ui::ConfigStore {
 public:
  [[nodiscard]] std::optional<std::string> Get(std::string_view key) const override;
  void Set(std::string_view key, std::string_view value) override;

  [[nodiscard]] std::string Serialize() const;
  // Lines without an '=' and blank lines are skipped; a trailing '\r' is dropped; the last of two equal keys wins.
  [[nodiscard]] static TextConfigStore Parse(std::string_view text);

 private:
  std::map<std::string, std::string, std::less<>> values_;
};

}  // namespace uplift::ipc
