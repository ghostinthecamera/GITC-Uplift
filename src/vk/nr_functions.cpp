#include "vk/nr_functions.hpp"

#include <string>

#include "vk/loader.hpp"

namespace uplift::vk {

std::optional<NrFunctions> NrFunctions::Open(VkDevice device) {
  const Loader* const loader = Loader::Get();
  if (loader == nullptr || device == VK_NULL_HANDLE) return std::nullopt;
  NrFunctions opened;
  bool complete = true;
  const auto resolve = [&](const char* name) { return loader->get_device_proc_addr(device, name); };
#define UPLIFT_VK_NR_RESOLVE(name)                            \
  opened.name = reinterpret_cast<PFN_##name>(resolve(#name)); \
  complete = complete && (opened.name != nullptr);
  UPLIFT_VK_NR_FUNCTIONS(UPLIFT_VK_NR_RESOLVE)
#undef UPLIFT_VK_NR_RESOLVE
#define UPLIFT_VK_NR_RESOLVE_KHR(name)                                                         \
  opened.name = reinterpret_cast<PFN_##name>(resolve(#name));                                  \
  if (opened.name == nullptr) {                                                                \
    opened.name = reinterpret_cast<PFN_##name>(resolve((std::string(#name) + "KHR").c_str())); \
  }                                                                                            \
  complete = complete && (opened.name != nullptr);
  UPLIFT_VK_NR_FUNCTIONS_WITH_KHR_ALIAS(UPLIFT_VK_NR_RESOLVE_KHR)
#undef UPLIFT_VK_NR_RESOLVE_KHR
  opened.vkCmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(resolve("vkCmdPushDescriptorSetKHR"));
  complete = complete && (opened.vkCmdPushDescriptorSetKHR != nullptr);
  if (!complete) return std::nullopt;
  return opened;
}

}  // namespace uplift::vk
