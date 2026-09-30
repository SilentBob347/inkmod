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
// X4 Pro intentionally uses a *single* 1-bit stochastic pass: its stable
// display path is monochrome, so keeping 2-bit ordered dither in the cache and
// dithering that again during framebuffer output creates visible grids/moire.
// Returning only 0/3 lets DirectPixelWriter copy the already-final B/W result.
inline uint8_t quantizeImagePixel(uint8_t gray, int x, int y, bool useDithering, bool x4Pro) {
  if (x4Pro) {
    // Full-range deterministic noise threshold. Unlike an ordered Bayer tile
    // there is no repeating spatial lattice to become visible on e-ink.
    uint32_t hash = static_cast<uint32_t>(x) * 374761393u +
                    static_cast<uint32_t>(y) * 668265263u + 0x9E3779B9u;
    hash = (hash ^ (hash >> 13)) * 1274126177u;
    hash ^= hash >> 16;
    const uint8_t threshold = static_cast<uint8_t>(hash >> 24);
    return (gray < threshold) ? 0 : 3;
  }

  if (useDithering) return applyBayerDither4Level(gray, x, y);
  uint8_t level = gray / 85;
  return level > 3 ? 3 : level;
}
