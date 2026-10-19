#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "addon/reshade_api.hpp"
#include "ui/localization.hpp"

namespace uplift::ui::gui {
using namespace ImGui;
inline thread_local Language language = Language::ENGLISH;

class LanguageScope {
 public:
  explicit LanguageScope(Language selected) : previous_(language) { language = selected; }
  ~LanguageScope() { language = previous_; }
 private:
  Language previous_;
};

inline std::string Display(std::string_view text) { return Localize(language, text); }
inline std::string Label(std::string_view text) { return LocalizedLabel(language, text); }
inline std::string Formatted(const char* format, va_list args) {
  va_list count;
  va_copy(count, args);
  const int size = std::vsnprintf(nullptr, 0u, format, count);
  va_end(count);
  if (size < 0) return Display(format);
  std::string text(static_cast<size_t>(size) + 1u, '\0');
  std::vsnprintf(text.data(), text.size(), format, args);
  text.resize(static_cast<size_t>(size));
  return Display(text);
}

inline void TextUnformatted(const char* text, const char* end = nullptr) {
  const auto shown = Display(end ? std::string_view(text, end) : std::string_view(text));
  ImGui::TextUnformatted(shown.c_str());
}
inline void TextWrapped(const char* format, ...) {
  va_list args; va_start(args, format); const auto text = Formatted(format, args); va_end(args);
  ImGui::TextWrapped("%s", text.c_str());
}
inline void TextDisabled(const char* format, ...) {
  va_list args; va_start(args, format); const auto text = Formatted(format, args); va_end(args);
  ImGui::TextDisabled("%s", text.c_str());
}
inline void TextColored(const ImVec4& color, const char* format, ...) {
  va_list args; va_start(args, format); const auto text = Formatted(format, args); va_end(args);
  ImGui::TextColored(color, "%s", text.c_str());
}
inline void BulletText(const char* format, ...) {
  va_list args; va_start(args, format); const auto text = Formatted(format, args); va_end(args);
  ImGui::BulletText("%s", text.c_str());
}
inline void SetTooltip(const char* format, ...) {
  va_list args; va_start(args, format); const auto text = Formatted(format, args); va_end(args);
  ImGui::SetTooltip("%s", text.c_str());
}
inline ImVec2 CalcTextSize(const char* text, const char* end = nullptr, bool hide = false, float wrap = -1.f) {
  const auto shown = Display(end ? std::string_view(text, end) : std::string_view(text));
  return ImGui::CalcTextSize(shown.c_str(), nullptr, hide, wrap);
}
inline void SeparatorText(const char* text) { ImGui::SeparatorText(Display(text).c_str()); }
inline bool Button(const char* label, const ImVec2& size = ImVec2(0.f, 0.f)) { return ImGui::Button(Label(label).c_str(), size); }
inline bool SmallButton(const char* label) { return ImGui::SmallButton(Label(label).c_str()); }
inline bool Checkbox(const char* label, bool* value) { return ImGui::Checkbox(Label(label).c_str(), value); }
inline bool SliderFloat(const char* label, float* value, float low, float high, const char* format = "%.3f", ImGuiSliderFlags flags = 0) { return ImGui::SliderFloat(Label(label).c_str(), value, low, high, format, flags); }
inline bool SliderInt(const char* label, int* value, int low, int high, const char* format = "%d", ImGuiSliderFlags flags = 0) { return ImGui::SliderInt(Label(label).c_str(), value, low, high, format, flags); }
inline bool InputInt(const char* label, int* value, int step = 1, int fast = 100, ImGuiInputTextFlags flags = 0) { return ImGui::InputInt(Label(label).c_str(), value, step, fast, flags); }
inline bool InputText(const char* label, char* value, size_t size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* data = nullptr) { return ImGui::InputText(Label(label).c_str(), value, size, flags, callback, data); }
inline bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0) { return ImGui::BeginCombo(Label(label).c_str(), preview ? Display(preview).c_str() : nullptr, flags); }
inline bool Combo(const char* label, int* value, const char* const items[], int count, int height = -1) {
  std::vector<std::string> text;
  std::vector<const char*> labels;
  text.reserve(static_cast<size_t>(count)); labels.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) text.push_back(Display(items[i]));
  for (const auto& item : text) labels.push_back(item.c_str());
  return ImGui::Combo(Label(label).c_str(), value, labels.data(), count, height);
}
inline bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0.f, 0.f)) { return ImGui::Selectable(Label(label).c_str(), selected, flags, size); }
inline bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0) { return ImGui::CollapsingHeader(Label(label).c_str(), flags); }
inline bool TreeNode(const char* label) { return ImGui::TreeNode(Label(label).c_str()); }
inline void OpenPopup(const char* id, ImGuiPopupFlags flags = 0) { ImGui::OpenPopup(Label(id).c_str(), flags); }
inline bool IsPopupOpen(const char* id, ImGuiPopupFlags flags = 0) { return ImGui::IsPopupOpen(Label(id).c_str(), flags); }
inline bool BeginPopupModal(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0) { return ImGui::BeginPopupModal(Label(name).c_str(), open, flags); }
}  // namespace uplift::ui::gui
