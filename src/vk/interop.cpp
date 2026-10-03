#include "vk/interop.hpp"

#include <bit>
#include <utility>

#include "vk/format.hpp"

namespace uplift::vk {

ImportResult Interop::ImportImage(HANDLE nt_handle, DXGI_FORMAT format, nr::Size size) {
  ImportResult out;
  const VkFormat vk_format = VkFormatOf(format);
  if (vk_format == VK_FORMAT_UNDEFINED) {
    out.result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    out.step = "the format";
    return out;
  }
  if (device_.vkGetMemoryWin32HandlePropertiesKHR == nullptr) {
    out.result = VK_ERROR_EXTENSION_NOT_PRESENT;
    out.step = "vkGetMemoryWin32HandlePropertiesKHR";
    return out;
  }
  const VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
  };
  const VkImageCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = vk_format,
      .extent = {size.width, size.height, 1u},
      .mipLevels = 1u,
      .arrayLayers = 1u,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  SharedImage image = {.size = size, .format = format};
  // Through the device's own chain, so ReShade registers the image (key decision e).
  out.step = "vkCreateImage(external)";
  out.result = device_.vkCreateImage(device_.handle, &create, nullptr, &image.image);
  if (out.result != VK_SUCCESS) return out;
  const auto fail = [&](VkResult result, const char* step) {
    out.result = result;
    out.step = step;
    device_.vkDestroyImage(device_.handle, image.image, nullptr);  // the device exists here: the layer's own destroy is fine
    FreeMemory(&image);
    return out;
  };

  VkMemoryRequirements requirements = {};
  device_.vkGetImageMemoryRequirements(device_.handle, image.image, &requirements);
  // For the Direct3D 12 handle types the spec allows this query, and it narrows the memory types to the ones the handle can be imported as.
  VkMemoryWin32HandlePropertiesKHR properties = {.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
  if (const VkResult result =
          device_.vkGetMemoryWin32HandlePropertiesKHR(device_.handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, nt_handle, &properties);
      result != VK_SUCCESS) {
    return fail(result, "vkGetMemoryWin32HandlePropertiesKHR");
  }
  const uint32_t allowed = requirements.memoryTypeBits & properties.memoryTypeBits;
  if (allowed == 0u) return fail(VK_ERROR_FEATURE_NOT_PRESENT, "the memory type");
  // Key decision e: the device-local type among the handle's and the image's bits when the record has the physical device, else the lowest.
  uint32_t memory_type = static_cast<uint32_t>(std::countr_zero(allowed));
  if (device_.record.physical != VK_NULL_HANDLE) {
    if (const Loader* const loader = Loader::Get()) {
      VkPhysicalDeviceMemoryProperties memory = {};
      loader->get_memory_properties(device_.record.physical, &memory);
      for (uint32_t index = 0u; index < memory.memoryTypeCount; ++index) {
        if ((allowed & (1u << index)) != 0u && (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u) {
          memory_type = index;
          break;
        }
      }
    }
  }
  const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image.image,
  };
  const VkImportMemoryWin32HandleInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
      .handle = nt_handle,
  };
  const VkMemoryAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
  };
  if (const VkResult result = device_.vkAllocateMemory(device_.handle, &allocate, nullptr, &image.memory); result != VK_SUCCESS) {
    return fail(result, "vkAllocateMemory(import)");
  }
  if (const VkResult result = device_.vkBindImageMemory(device_.handle, image.image, image.memory, 0u); result != VK_SUCCESS) {
    return fail(result, "vkBindImageMemory(import)");
  }
  out.image = image;
  out.result = VK_SUCCESS;
  out.step = "";
  return out;
}

VkResult Interop::ImportTimeline(HANDLE nt_handle, VkSemaphore* semaphore, const char** step) {
  const VkSemaphoreTypeCreateInfo timeline = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0u,
  };
  const VkSemaphoreCreateInfo create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timeline};
  *step = "vkCreateSemaphore(timeline)";
  VkResult result = device_.vkCreateSemaphore(device_.handle, &create, nullptr, semaphore);
  if (result != VK_SUCCESS) return result;
  *step = "vkImportSemaphoreWin32HandleKHR";
  if (device_.vkImportSemaphoreWin32HandleKHR == nullptr) {
    result = VK_ERROR_EXTENSION_NOT_PRESENT;
  } else {
    const VkImportSemaphoreWin32HandleInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
        .semaphore = *semaphore,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
        .handle = nt_handle,
    };
    result = device_.vkImportSemaphoreWin32HandleKHR(device_.handle, &import);
  }
  if (result != VK_SUCCESS) {
    DestroySemaphore(semaphore);
  }
  return result;
}

void Interop::DestroySemaphore(VkSemaphore* semaphore) {
  if (*semaphore != VK_NULL_HANDLE) {
    device_.vkDestroySemaphore(device_.handle, *semaphore, nullptr);
  }
  *semaphore = VK_NULL_HANDLE;
}

void Interop::Note(VkResult result) {
  if (result == VK_ERROR_DEVICE_LOST) {
    device_lost_ = true;
  }
}

void Interop::FreeMemory(SharedImage* image) {
  if (image->memory != VK_NULL_HANDLE) {
    device_.vkFreeMemory(device_.handle, image->memory, nullptr);
    image->memory = VK_NULL_HANDLE;
  }
}

bool Interop::Seal(Retired* retired, GameHost& host) {
  const VkFenceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  if (device_.vkCreateFence(device_.handle, &create, nullptr, &fence) != VK_SUCCESS) return false;
  if (!host.FlushWithFence(fence)) {
    Note(device_.vkGetFenceStatus(device_.handle, fence));  // batch 3 review, minor 3: the host's bool hides a VK_ERROR_DEVICE_LOST of the submit
    device_.vkDestroyFence(device_.handle, fence, nullptr);
    return false;
  }
  retired->fence = fence;
  return true;
}

void Interop::Retire(SharedImage* image, GameHost& host) {
  if (image->image == VK_NULL_HANDLE && image->memory == VK_NULL_HANDLE) return;
  Retired retired = {.image = *image};
  Seal(&retired, host);  // a failure leaves the fence null: FreeFinished tries again, FreeAll ends it
  retired_.push_back(retired);
  *image = {};
}

void Interop::RetireSemaphore(VkSemaphore* semaphore, GameHost& host) {
  if (*semaphore == VK_NULL_HANDLE) return;
  Retired retired = {.semaphore = *semaphore};
  Seal(&retired, host);
  retired_.push_back(retired);
  *semaphore = VK_NULL_HANDLE;
}

bool Interop::FreeFinished(GameHost& host) {
  bool freed = false;
  for (auto retired = retired_.begin(); retired != retired_.end();) {
    if (retired->fence == VK_NULL_HANDLE) {
      Seal(&*retired, host);
    }
    bool finished = false;
    if (retired->fence != VK_NULL_HANDLE) {
      const VkResult status = device_.vkGetFenceStatus(device_.handle, retired->fence);
      Note(status);
      finished = (status != VK_NOT_READY);  // signalled, or the device is lost and nothing more will run
    }
    if (finished) {
      device_.vkDestroyFence(device_.handle, retired->fence, nullptr);
      if (retired->image.image != VK_NULL_HANDLE) {
        device_.vkDestroyImage(device_.handle, retired->image.image, nullptr);  // the device exists here (not destroy_device)
      }
      FreeMemory(&retired->image);
      DestroySemaphore(&retired->semaphore);
      retired = retired_.erase(retired);
      freed = true;
    } else {
      ++retired;
    }
  }
  return freed;
}

void Interop::FreeNow(SharedImage* image, GameHost& host) {
  if (image->image != VK_NULL_HANDLE) {
    host.DestroyImage(image->image);
  }
  FreeMemory(image);
  *image = {};
}

bool Interop::Idle() {
  if (!retired_.empty()) return false;
  for (WaitFence& wait : waits_) {
    if (wait.pending && wait.fence != VK_NULL_HANDLE) {
      const VkResult status = device_.vkGetFenceStatus(device_.handle, wait.fence);
      Note(status);
      if (status == VK_NOT_READY) return false;
      wait.pending = false;  // signalled, or the device is lost and nothing more will run
    }
  }
  return true;
}

void Interop::FreeAll(GameHost& host) {
  for (Retired& retired : retired_) {
    if (retired.fence != VK_NULL_HANDLE) {
      device_.vkDestroyFence(device_.handle, retired.fence, nullptr);
    }
    FreeNow(&retired.image, host);
    DestroySemaphore(&retired.semaphore);
  }
  retired_.clear();
  for (WaitFence& wait : waits_) {
    if (wait.fence != VK_NULL_HANDLE) {
      device_.vkDestroyFence(device_.handle, wait.fence, nullptr);
    }
    wait = {};
  }
}

WaitResult Interop::FlushAndWait(WaitSlot slot, GameHost& host, uint32_t timeout_ms) {
  WaitFence& wait = waits_[static_cast<size_t>(slot)];
  if (wait.fence == VK_NULL_HANDLE) {
    const VkFenceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (device_.vkCreateFence(device_.handle, &create, nullptr, &wait.fence) != VK_SUCCESS) {
      wait.fence = VK_NULL_HANDLE;
      return WaitResult::FAILED;
    }
  }
  if (wait.pending) {
    const VkResult status = device_.vkGetFenceStatus(device_.handle, wait.fence);
    if (status == VK_NOT_READY) return WaitResult::BUSY;
    Note(status);
    if (status != VK_SUCCESS) return WaitResult::FAILED;
    wait.pending = false;
  }
  if (const VkResult reset = device_.vkResetFences(device_.handle, 1u, &wait.fence); reset != VK_SUCCESS) {
    Note(reset);
    return WaitResult::FAILED;
  }
  if (!host.FlushWithFence(wait.fence)) {
    // Batch 3 review, minor 3: the host's bool hides the VkResult of the submit. A lost device answers the fence's status with VK_ERROR_DEVICE_LOST
    // (an unsignalled fence of a healthy one says VK_NOT_READY, which Note ignores), so the latch can say so instead of "a submission failed".
    Note(device_.vkGetFenceStatus(device_.handle, wait.fence));
    return WaitResult::FAILED;
  }
  wait.pending = true;
  const VkResult waited = device_.vkWaitForFences(device_.handle, 1u, &wait.fence, VK_TRUE, static_cast<uint64_t>(timeout_ms) * 1'000'000u);
  if (waited == VK_SUCCESS) {
    wait.pending = false;
    return WaitResult::DONE;
  }
  if (waited == VK_TIMEOUT) return WaitResult::TIMEOUT;
  Note(waited);
  return WaitResult::FAILED;
}

uint64_t Interop::Counter(VkSemaphore semaphore) {
  uint64_t value = 0u;
  if (semaphore == VK_NULL_HANDLE || device_.vkGetSemaphoreCounterValue == nullptr) return UINT64_MAX;
  const VkResult result = device_.vkGetSemaphoreCounterValue(device_.handle, semaphore, &value);
  if (result != VK_SUCCESS) {
    Note(result);
    return UINT64_MAX;
  }
  return value;
}

}  // namespace uplift::vk
