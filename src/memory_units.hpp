#pragma once

namespace uplift {

// Keep byte-display divisions in floating point, including fractional MiB.
inline constexpr double BYTES_PER_MIB = 1024.0 * 1024.0;

// Preserve binary GiB arithmetic in the existing GB-labelled readout.
inline constexpr double BYTES_PER_GIB = 1024.0 * BYTES_PER_MIB;

}  // namespace uplift
