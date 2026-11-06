#pragma once

#include <utility>

#include "addon/reshade_api.hpp"
#include "nr/types.hpp"
#include "text/effect_names.hpp"

namespace uplift::addon {

// The Uplift technique rendered before the add-on's present event: the technique and, when the motion producer rendered on a target of the same
// size in the same window, UPLIFT_MV's view with the rectangle it wrote. `technique` is 0 when none rendered this frame.
struct RenderedMarker {
  reshade::api::effect_technique technique{};
  reshade::api::resource_view motion{};
  nr::Size extent{};
  [[nodiscard]] bool Rendered() const { return technique.handle != 0u; }
};

// Between ReShade's frame end (reshade_present) and the add-on's next present event, another add-on (REST, for one) can render the motion producer
// (Launchpad or Lumenite) and the Uplift technique on the runtime's immediate list; NR then runs at Present on that UPLIFT_MV
// (TriggerPoint::EARLY_MARKER). The rules are pure, over what the render-technique event carries; Observe and Take are their adapters, the only
// readers of ReShade's runtime here.
struct EarlyMarker {
  bool observing = false;                     // the window is open: BeginObserving at reshade_present, until Take
  reshade::api::effect_technique producer{};  // the provider that rendered in this window, and its target's size
  nr::Size producer_extent{};
  RenderedMarker result{};                    // the marker that rendered in this window

  void BeginObserving() { *this = {.observing = true}; }
  // The window watches a render-technique event of the provider or the marker, on a real target.
  [[nodiscard]] bool Watches(reshade::api::effect_technique technique, reshade::api::effect_technique marker, reshade::api::effect_technique provider,
                             reshade::api::resource_view target) const {
    return observing && technique.handle != 0u && (technique == provider || technique == marker) && target.handle != 0u;
  }
  void ProducerRendered(reshade::api::effect_technique provider, nr::Size extent) {
    producer = provider;
    producer_extent = extent;
  }
  // UPLIFT_MV holds the provider's vectors for this frame only when the provider rendered in this window, on a target of the same, non-empty size.
  [[nodiscard]] bool ProducerMatches(reshade::api::effect_technique provider, nr::Size extent) const {
    return producer.handle != 0u && producer == provider && producer_extent == extent && !extent.Empty();
  }
  // The marker rendered on a target of `extent`; `motion` is UPLIFT_MV's binding then and `motion_size` its texture's size (0 while unbound). ReShade
  // shares named textures across permutations: a smaller viewport writes only the top-left `extent` rectangle, which is kept, while a clipped larger
  // one lacks the full field, so its motion is dropped.
  void MarkerRendered(reshade::api::effect_technique marker, reshade::api::effect_technique provider, nr::Size extent,
                      reshade::api::resource_view motion, nr::Size motion_size) {
    result = {.technique = marker};
    if (!ProducerMatches(provider, extent) || motion.handle == 0u) return;
    if (extent.width <= motion_size.width && extent.height <= motion_size.height) {
      result.motion = motion;
      result.extent = extent;
    }
  }
  // Take must read UPLIFT_MV's binding again: the marker saw motion, and the provider is still `provider`.
  [[nodiscard]] bool WantsBinding(reshade::api::effect_technique provider) const { return result.motion.handle != 0u && producer == provider; }
  // The window's outcome, consumed (the window closes either way): the marker when it rendered, with its motion while UPLIFT_MV's binding is still
  // `current`, the view it saw; none without a runtime, without a marker technique, or when the marker that rendered is not `marker`.
  RenderedMarker Decide(bool have_runtime, reshade::api::effect_technique marker, reshade::api::effect_technique provider,
                        reshade::api::resource_view current) {
    const EarlyMarker seen = std::exchange(*this, {});
    if (!have_runtime || marker.handle == 0u || seen.result.technique != marker) return {};
    if (seen.WantsBinding(provider) && current == seen.result.motion) return {.technique = marker, .motion = current, .extent = seen.result.extent};
    return {.technique = marker};
  }

  // The adapters. Observe: a render-technique event while the window is open (both add-ons' handlers). Take: the present event's read.
  void Observe(reshade::api::effect_runtime* runtime, reshade::api::effect_technique technique, reshade::api::effect_technique marker,
               reshade::api::effect_technique provider, reshade::api::resource_view target) {
    if (!Watches(technique, marker, provider, target)) return;
    reshade::api::device* const device = runtime->get_device();
    const nr::Size extent = SizeOf(device, target);
    if (technique == provider) {
      ProducerRendered(provider, extent);
      return;
    }
    const reshade::api::resource_view motion = (ProducerMatches(provider, extent) ? UpliftMvBinding(runtime) : reshade::api::resource_view{});
    MarkerRendered(marker, provider, extent, motion, (motion.handle != 0u ? SizeOf(device, motion) : nr::Size{}));
  }
  RenderedMarker Take(reshade::api::effect_runtime* runtime, reshade::api::effect_technique marker, reshade::api::effect_technique provider) {
    const reshade::api::resource_view current =
        (runtime != nullptr && WantsBinding(provider) ? UpliftMvBinding(runtime) : reshade::api::resource_view{});
    return Decide(runtime != nullptr, marker, provider, current);
  }

  static nr::Size SizeOf(reshade::api::device* device, reshade::api::resource_view view) {
    const reshade::api::resource_desc desc = device->get_resource_desc(device->get_resource_from_view(view));
    return {desc.texture.width, desc.texture.height};
  }
  static reshade::api::resource_view UpliftMvBinding(reshade::api::effect_runtime* runtime) {
    reshade::api::resource_view view{}, srgb{};
    runtime->get_texture_binding(runtime->find_texture_variable(uplift::text::UPLIFT_FX_EFFECT, uplift::text::UPLIFT_MOTION_TEXTURE), &view, &srgb);
    return view;
  }
};

}  // namespace uplift::addon
