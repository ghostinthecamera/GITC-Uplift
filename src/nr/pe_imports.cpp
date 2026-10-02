#include "nr/pe_imports.hpp"

#include <cstddef>
#include <cstring>

namespace uplift::nr {

void** FindImportAddress(HMODULE module, const char* import_name) {
  if (module == nullptr || import_name == nullptr) return nullptr;
  auto* const image = reinterpret_cast<std::byte*>(module);
  const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
  const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      image + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (imports.VirtualAddress == 0u
      || imports.VirtualAddress >= image_size
      || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
    return nullptr;
  }

  auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
      image + imports.VirtualAddress);
  const auto* const descriptor_end = reinterpret_cast<const std::byte*>(
                                         descriptor)
                                     + imports.Size;
  for (; reinterpret_cast<const std::byte*>(descriptor + 1u) <= descriptor_end
         && descriptor->Name != 0u;
       ++descriptor) {
    if (descriptor->OriginalFirstThunk == 0u
        || descriptor->OriginalFirstThunk >= image_size
        || descriptor->FirstThunk == 0u
        || descriptor->FirstThunk >= image_size) {
      continue;
    }
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
        image + descriptor->OriginalFirstThunk);
    auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(
        image + descriptor->FirstThunk);
    while (reinterpret_cast<std::byte*>(names + 1u) <= image + image_size
           && reinterpret_cast<std::byte*>(addresses + 1u) <= image + image_size
           && names->u1.AddressOfData != 0u) {
      if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
          && names->u1.AddressOfData < image_size) {
        const auto* const import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
            image + names->u1.AddressOfData);
        if (std::strcmp(
                reinterpret_cast<const char*>(import->Name),
                import_name)
            == 0) {
          return reinterpret_cast<void**>(&addresses->u1.Function);
        }
      }
      ++names;
      ++addresses;
    }
  }
  return nullptr;
}

}  // namespace uplift::nr
