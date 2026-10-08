#pragma once

#include <array>
#include <chrono>
#include <optional>

#include "nr/types.hpp"
#include "ui/settings.hpp"

namespace uplift::ui {

// Which history-resetting sliders the user is dragging this frame (ImGui::IsItemActive).
struct DragState {
  bool local_structure = false;
  bool local_tone = false;
  bool skin_structure = false;
  bool resolution_scale = false;
  bool pass_slider = false;  // any pass's history-resetting slider
  bool face_protection = false;  // Keep faces (2026-10-08)
};

// Spec §12 slider policy. Controls that reset NR history apply live, but a dragged slider
// (LocalStructure, LocalTone, SkinStructure) applies at most every 100 ms and applies its final
// value once on release. Discrete ones (Style, AutoMask) and Intensity apply on the frame they change.
class ControlsCoalescer {
 public:
  nr::Controls Update(const Settings& live, const DragState& drag, std::chrono::steady_clock::time_point now);
  // Plan 5: passes 2..10's own controls (nullopt follows pass 1), held while one of their history-resetting
  // sliders is dragged and applied at most every 100 ms then, like pass 1's.
  std::array<std::optional<nr::Controls>, MAX_PASSES - 1u> LaterPasses(const Settings& live, bool dragging,
                                                                       std::chrono::steady_clock::time_point now);
  // Keep faces (2026-10-08): Face protection is the extra run's Skin structure, so a drag applies it as Skin structure's is (at most every 100 ms).
  float FaceProtection(const Settings& live, bool dragging, std::chrono::steady_clock::time_point now) {
    return face_protection_.Update(live.face_protection, dragging, now);
  }

 private:
  struct Slider {
    float applied = 1.f;
    std::optional<std::chrono::steady_clock::time_point> last_apply;

    float Update(float live, bool dragging, std::chrono::steady_clock::time_point now);
  };

  Slider local_structure_;
  Slider local_tone_;
  Slider skin_structure_;
  Slider face_protection_;
  bool initialized_ = false;
  std::array<std::optional<nr::Controls>, MAX_PASSES - 1u> later_ = {};
  std::optional<std::chrono::steady_clock::time_point> later_applied_;
};

}  // namespace uplift::ui
