#ifndef UPLIFT_VK_COMMON_HLSLI
#define UPLIFT_VK_COMMON_HLSLI

// Plan 13 (design §4.2): what the SPIR-V builds need, and nothing for DXIL. A storage image's format must match its view's (DXC would deduce
// Rgba32f from float4); UPLIFT_VK_SOURCE_STORAGE=1 reads the source from the storage target in place (After DLSS on Vulkan), whose format is
// UPLIFT_VK_TARGET_FORMAT.
#if defined(__spirv__)
#define UPLIFT_IMAGE_FORMAT(format) [[vk::image_format(format)]]
#else
#define UPLIFT_IMAGE_FORMAT(format)
#endif

#ifndef UPLIFT_VK_SOURCE_STORAGE
#define UPLIFT_VK_SOURCE_STORAGE 0
#endif
#ifndef UPLIFT_VK_TARGET_FORMAT
#define UPLIFT_VK_TARGET_FORMAT "rgba16f"
#endif

#endif  // UPLIFT_VK_COMMON_HLSLI
