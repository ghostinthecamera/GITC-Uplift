#pragma once

#include <utility>

#include "addon/reshade_api.hpp"
#include "nr/types.hpp"

namespace uplift::addon {

struct RenderedMarker {
  reshade::api::effect_technique technique{};
  reshade::api::resource_view motion{};
  nr::Size extent{};
};

// Between ReShade's frame end and the next primary Present, an add-on can render the producer and marker on the runtime's immediate list.
struct EarlyMarker {
  bool observing = false;
  reshade::api::effect_technique producer{};
  nr::Size producer_extent{};
  RenderedMarker result{};

  void Observe(reshade::api::effect_runtime* runtime, reshade::api::effect_technique technique, reshade::api::effect_technique marker,
               reshade::api::effect_technique provider, reshade::api::resource_view target) {
    namespace api = reshade::api;
    if (!observing || technique.handle == 0u || (technique != provider && technique != marker) || target.handle == 0u) return;
    api::device* device = runtime->get_device();
    const auto desc = device->get_resource_desc(device->get_resource_from_view(target));
    const nr::Size extent{desc.texture.width, desc.texture.height};
    if (technique == provider) {
      producer = provider;
      producer_extent = extent;
      return;
    }
    result = {.technique = marker};
    if (producer.handle == 0u || producer != provider || producer_extent != extent || extent.Empty()) return;
    api::resource_view view{}, srgb{};
    runtime->get_texture_binding(runtime->find_texture_variable("Uplift.fx", "UPLIFT_MV"), &view, &srgb);
    if (view.handle == 0u) return;
    const auto motion = device->get_resource_desc(device->get_resource_from_view(view));
    // ReShade shares named textures across permutations. A smaller viewport writes only the top-left rectangle; a clipped larger one lacks the full field.
    if (extent.width <= motion.texture.width && extent.height <= motion.texture.height) {
      result.motion = view;
      result.extent = extent;
    }
  }

  RenderedMarker Take(reshade::api::effect_runtime* runtime, reshade::api::effect_technique marker, reshade::api::effect_technique provider) {
    const auto seen = std::exchange(*this, {});
    if (!runtime || marker.handle == 0u || seen.result.technique != marker) return {};
    if (seen.result.motion.handle && seen.producer == provider) {
      reshade::api::resource_view current{}, srgb{};
      runtime->get_texture_binding(runtime->find_texture_variable("Uplift.fx", "UPLIFT_MV"), &current, &srgb);
      if (current == seen.result.motion) return {.technique = marker, .motion = current, .extent = seen.result.extent};
    }
    return {.technique = marker};
  }
};

}  // namespace uplift::addon
