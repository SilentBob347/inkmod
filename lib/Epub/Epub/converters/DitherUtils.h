#pragma once

#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}


// Final image quantizer used by JPEG/PNG decoders.
// X4 Pro keeps a clean 2-bit source cache with NO spatial dithering here.
// Its stable panel path is monochrome, so the one and only halftone pass is
// performed later by DirectPixelWriter. This avoids both double-dither moire
// and the salt-and-pepper look of per-pixel stochastic 1-bit conversion.
inline uint8_t quantizeImagePixel(uint8_t gray, int x, int y, bool useDithering, bool x4Pro) {
  if (x4Pro) {
    // Slightly wider highlight range keeps pale source detail from vanishing.
    if (gray < 58) return 0;
    if (gray < 126) return 1;
    if (gray < 202) return 2;
    return 3;
  }

  if (useDithering) return applyBayerDither4Level(gray, x, y);
  uint8_t level = gray / 85;
  return level > 3 ? 3 : level;
}
