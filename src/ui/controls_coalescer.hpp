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
  bool face_protection = false;  // Keep faces
  bool resolution_scale_after_dlss = false;  // split chain: a later stage group's Custom scale, held as Setup's
  bool resolution_scale_present = false;
};

// Controls that reset NR history apply live, but a dragged slider
// (LocalStructure, LocalTone, SkinStructure) applies at DRAG_PERIOD intervals and applies its final
// value once on release. Discrete ones (Style, AutoMask) and Intensity apply on the frame they change.
class ControlsCoalescer {
 public:
  nr::Controls Update(const Settings& live, const DragState& drag, std::chrono::steady_clock::time_point now);
  // Passes 2..10's own controls (nullopt follows pass 1), held while one of their history-resetting
  // sliders is dragged and applied at DRAG_PERIOD intervals then, like pass 1's.
  std::array<std::optional<nr::Controls>, MAX_PASSES - 1u> LaterPasses(const Settings& live, bool dragging,
                                                                       std::chrono::steady_clock::time_point now);
  // Keep faces: Face protection is the extra run's Skin structure, so a drag applies it as Skin structure's is (at DRAG_PERIOD intervals).
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
