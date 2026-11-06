#pragma once

#include <d3d12.h>
#include <dxgiformat.h>

#include "nr/types.hpp"

namespace uplift::bridge {

// A share a bridge could not make, cached by what it was made for: size, format and creation flags. The bridge tries again only when that key
// changes or NR is re-enabled. `Cause` is why, in the form the bridge reports it.
template <typename Cause>
struct ShareFailureOf {
  nr::Size size;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  Cause cause = {};
  // NONE for every share whose flags are fixed by its kind; set only where one texture kind is made with flags that vary by caller.
  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
  [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format, D3D12_RESOURCE_FLAGS other_flags = D3D12_RESOURCE_FLAG_NONE) const {
    return size == other_size && format == other_format && flags == other_flags;
  }
};

}  // namespace uplift::bridge
