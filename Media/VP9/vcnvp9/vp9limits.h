// Renoir VCN 2.2 VP9 coded-dimension limits. The full rectangle requires a
// tier-1 descriptor: the implicit tier-0 reference layout produces incorrect
// inter-frame chroma here even when the allocation itself is large enough.
#ifndef VCNVP9_LIMITS_H_
#define VCNVP9_LIMITS_H_

#include <stdbool.h>
#include <stdint.h>

#define VCN_VP9_MAX_WIDTH 8192u
#define VCN_VP9_MAX_HEIGHT 4352u
#define VCN_VP9_MAX_PIXELS (VCN_VP9_MAX_WIDTH * VCN_VP9_MAX_HEIGHT)
#define VCN_VP9_TIER0_MAX_PIXELS (8192u * 4320u)

static inline bool vcnvp9_bounds_supported(uint32_t width, uint32_t height) {
  return width > 0 && height > 0 && width <= VCN_VP9_MAX_WIDTH &&
         height <= VCN_VP9_MAX_HEIGHT;
}

static inline bool vcnvp9_dimensions_supported(uint32_t width, uint32_t height) {
  return vcnvp9_bounds_supported(width, height) &&
         (uint64_t)width * height <= VCN_VP9_MAX_PIXELS;
}

#endif
