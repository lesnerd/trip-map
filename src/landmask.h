#pragma once

// Equirectangular land/water mask. Row 0 is the north edge. One bit per cell,
// most-significant bit first within each byte. Sampled by the map kernel.
inline constexpr int kLandW = 720;
inline constexpr int kLandH = 360;
inline constexpr int kLandMaskBytes = (kLandW * kLandH) / 8;

extern const unsigned char kLandMask[kLandMaskBytes];
