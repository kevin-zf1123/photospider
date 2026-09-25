// CPU specializations of the pinned Slang blur/morph equations. Apache-2.0;
// see third_party/NOTICE.md. Keep per-pixel operation order and separate
// mul/add.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "runtime.hpp"  // NOLINT(build/include_subdir)
#if defined(PIXELOE_NEON)
#include <arm_neon.h>
#elif defined(PIXELOE_AVX2)
#include <immintrin.h>
#endif

namespace px {
#if defined(PIXELOE_NEON) || defined(PIXELOE_AVX2)
namespace {
#if defined(PIXELOE_NEON)
using Vec = float32x4_t;
constexpr uint32_t kLanes = 4;
Vec load(const float* p) {
  return vld1q_f32(p);
}
void store(float* p, Vec v) {
  vst1q_f32(p, v);
}
Vec splat(float v) {
  return vdupq_n_f32(v);
}
Vec add(Vec a, Vec b) {
  return vaddq_f32(a, b);
}
Vec sub(Vec a, Vec b) {
  return vsubq_f32(a, b);
}
Vec mul(Vec a, Vec b) {
  return vmulq_f32(a, b);
}
Vec divide(Vec a, Vec b) {
  return vdivq_f32(a, b);
}
Vec minimum(Vec a, Vec b) {
  return vminnmq_f32(a, b);
}
Vec maximum(Vec a, Vec b) {
  return vmaxnmq_f32(a, b);
}
#else
using Vec = __m256;
constexpr uint32_t kLanes = 8;
Vec load(const float* p) {
  return _mm256_loadu_ps(p);
}
void store(float* p, Vec v) {
  _mm256_storeu_ps(p, v);
}
Vec splat(float v) {
  return _mm256_set1_ps(v);
}
Vec add(Vec a, Vec b) {
  return _mm256_add_ps(a, b);
}
Vec sub(Vec a, Vec b) {
  return _mm256_sub_ps(a, b);
}
Vec mul(Vec a, Vec b) {
  return _mm256_mul_ps(a, b);
}
Vec divide(Vec a, Vec b) {
  return _mm256_div_ps(a, b);
}
// Inputs here are finite (or the initial reduction infinity). Fix the AVX
// equal-zero tie to match numeric min/max, including the sign of zero.
Vec minimum(Vec a, Vec b) {
  return _mm256_blendv_ps(_mm256_min_ps(a, b), _mm256_or_ps(a, b),
                          _mm256_cmp_ps(a, b, _CMP_EQ_OQ));
}
Vec maximum(Vec a, Vec b) {
  return _mm256_blendv_ps(_mm256_max_ps(a, b), _mm256_and_ps(a, b),
                          _mm256_cmp_ps(a, b, _CMP_EQ_OQ));
}
#endif
uint32_t number(const Arguments& a, const char* name) {
  return static_cast<uint32_t>(a.get(name).u);
}
float* data(const Arguments& a, const char* name) {
  return static_cast<float*>(a.get(name).data);
}
int pad(int x, int n, bool reflect) {
  if (reflect) {
    if (x < 0)
      x = -x;
    else if (x >= n)
      x = 2 * (n - 1) - x;
  }
  return std::clamp(x, 0, n - 1);
}
void blur(Context& c, const Arguments& a, uint32_t planes, int radius,
          bool rows) {
  const uint32_t h = number(a, "height"), w = number(a, "width"),
                 offset = number(a, "offset");
  const bool reflect = number(a, "reflect") != 0;
  const auto* src = data(a, rows ? "src" : "tmp");
  auto* dst = data(a, rows ? "tmp" : "dst");
  const auto* taps = data(a, "tables") + offset;
  const uint32_t mode = rows ? 0 : number(a, "mode");
  const auto* extra = mode == 1 ? data(a, "add_src") : dst;
  const size_t hw = static_cast<size_t>(h) * w;
  for (uint32_t plane = 0; plane < planes; ++plane) {
    for (uint32_t y = 0; y < h; ++y) {
      c.check();
      const size_t row = plane * hw + static_cast<size_t>(y) * w;
      // Reuse vertical offsets across the entire row; no gather is needed.
      size_t vertical[65];
      if (!rows) {
        for (int k = 0; k <= 2 * radius; ++k) {
          vertical[k] =
              plane * hw + static_cast<size_t>(pad(
                               static_cast<int>(y) + k - radius, h, reflect)) *
                               w;
        }
      }
      const uint32_t begin = rows ? std::min<uint32_t>(radius, w) : 0;
      const uint32_t end = rows && w > static_cast<uint32_t>(radius)
                               ? w - radius
                               : (rows ? 0 : w);
      auto scalar = [&](uint32_t x) {
        float acc = 0;
        for (int k = 0; k <= 2 * radius; ++k) {
          const size_t index =
              rows ? row + pad(static_cast<int>(x) + k - radius, w, reflect)
                   : vertical[k] + x;
          acc += taps[k] * src[index];
        }
        if (mode)
          acc += extra[row + x];
        dst[row + x] = acc;
      };
      uint32_t x = 0;
      for (; x < begin; ++x)
        scalar(x);
      // Four independent accumulators hide multiply/add latency, preserving
      // ascending tap order in each SIMD lane. No horizontal FP reduction.
      for (; x + 4 * kLanes <= end; x += 4 * kLanes) {
        Vec s0 = splat(0), s1 = s0, s2 = s0, s3 = s0;
        for (int k = 0; k <= 2 * radius; ++k) {
          const auto* p = src + (rows ? row + x + k - radius : vertical[k] + x);
          const Vec t = splat(taps[k]);
          s0 = add(s0, mul(t, load(p)));
          s1 = add(s1, mul(t, load(p + kLanes)));
          s2 = add(s2, mul(t, load(p + 2 * kLanes)));
          s3 = add(s3, mul(t, load(p + 3 * kLanes)));
        }
        if (mode) {
          s0 = add(s0, load(extra + row + x));
          s1 = add(s1, load(extra + row + x + kLanes));
          s2 = add(s2, load(extra + row + x + 2 * kLanes));
          s3 = add(s3, load(extra + row + x + 3 * kLanes));
        }
        store(dst + row + x, s0);
        store(dst + row + x + kLanes, s1);
        store(dst + row + x + 2 * kLanes, s2);
        store(dst + row + x + 3 * kLanes, s3);
      }
      for (; x < w; ++x)
        scalar(x);
    }
  }
}
float morph_scalar(const float* src, const float* se, uint32_t ks, size_t base,
                   uint32_t y, uint32_t x, uint32_t h, uint32_t w,
                   bool dilate) {
  float acc = dilate ? -std::numeric_limits<float>::infinity()
                     : std::numeric_limits<float>::infinity();
  const int r = ks / 2;
  for (uint32_t dy = 0; dy < ks; ++dy) {
    const int sy = static_cast<int>(y) + static_cast<int>(dy) - r;
    for (uint32_t dx = 0; dx < ks; ++dx) {
      const int sx = static_cast<int>(x) + static_cast<int>(dx) - r;
      const float v = sy >= 0 && sy < static_cast<int>(h) && sx >= 0 &&
                              sx < static_cast<int>(w)
                          ? src[base + static_cast<size_t>(sy) * w + sx]
                          : 0;
      const float k = se[dy * ks + dx];
      acc =
          dilate ? std::fmax(acc, v + k - 1.0f) : std::fmin(acc, v - k + 1.0f);
    }
  }
  return acc;
}
Vec morph_vector(const float* src, const float* se, uint32_t ks, size_t base,
                 uint32_t y, uint32_t x, uint32_t h, uint32_t w, bool dilate) {
  Vec acc = splat(dilate ? -std::numeric_limits<float>::infinity()
                         : std::numeric_limits<float>::infinity());
  const int r = ks / 2;
  for (uint32_t dy = 0; dy < ks; ++dy) {
    const int sy = static_cast<int>(y) + static_cast<int>(dy) - r;
    for (uint32_t dx = 0; dx < ks; ++dx) {
      Vec v = sy >= 0 && sy < static_cast<int>(h)
                  ? load(src + base + static_cast<size_t>(sy) * w + x + dx - r)
                  : splat(0);
      const Vec k = splat(se[dy * ks + dx]);
      acc = dilate ? maximum(acc, sub(add(v, k), splat(1)))
                   : minimum(acc, add(sub(v, k), splat(1)));
    }
  }
  return acc;
}
void morphology(Context& c, const Arguments& a, uint32_t planes, bool blend) {
  const uint32_t h = number(a, "height"), w = number(a, "width");
  const size_t hw = static_cast<size_t>(h) * w;
  const auto* src = data(a, blend ? "img" : "src");
  auto* dst = data(a, "dst");
  const uint32_t ks = number(a, blend ? "ks_erode" : "ks"),
                 kd = blend ? number(a, "ks_dilate") : ks;
  const auto* se = data(a, blend ? "se_erode" : "se");
  const auto* sd = blend ? data(a, "se_dilate") : se;
  const bool dilate = !blend && number(a, "mode") == 1;
  const bool clamp = !blend && number(a, "do_clamp") != 0;
  const uint32_t norm = blend ? number(a, "norm_mode") : 0;
  const auto* raw = blend ? data(a, "w_raw") : nullptr;
  auto* weights = blend ? data(a, "w_out") : nullptr;
  const uint32_t r = std::max(ks, kd) / 2;
  for (uint32_t plane = 0; plane < planes; ++plane) {
    const size_t base = plane * hw, wb = (plane / 3) * hw;
    const uint32_t slot = norm == 1 ? 0 : plane / 3;
    const float lo = norm ? data(a, "wmin")[slot] : 0,
                denom = norm ? data(a, "wmax")[slot] - lo + 1e-8f : 1;
    for (uint32_t y = 0; y < h; ++y) {
      c.check();
      const size_t row = static_cast<size_t>(y) * w;
      uint32_t x = 0;
      auto scalar = [&](uint32_t sx) {
        float v = morph_scalar(src, se, ks, base, y, sx, h, w, dilate);
        if (blend) {
          const float weight =
              norm ? (raw[wb + row + sx] - lo) / denom : raw[wb + row + sx];
          float d = morph_scalar(src, sd, kd, base, y, sx, h, w, true);
          d = std::fmin(std::fmax(d, 0.0f), 1.0f);
          v = v * weight + d * (1.0f - weight);
          if (plane % 3 == 0)
            weights[wb + row + sx] = weight;
        } else if (clamp) {
          v = std::fmin(std::fmax(v, 0.0f), 1.0f);
        }
        dst[base + row + sx] = v;
      };
      for (; x < std::min(r, w); ++x)
        scalar(x);
      for (; x + kLanes + r <= w; x += kLanes) {
        Vec v = morph_vector(src, se, ks, base, y, x, h, w, dilate);
        if (blend) {
          Vec weight = load(raw + wb + row + x);
          if (norm)
            weight = divide(sub(weight, splat(lo)), splat(denom));
          Vec d = morph_vector(src, sd, kd, base, y, x, h, w, true);
          d = minimum(maximum(d, splat(0)), splat(1));
          v = add(mul(v, weight), mul(d, sub(splat(1), weight)));
          if (plane % 3 == 0)
            store(weights + wb + row + x, weight);
        } else if (clamp) {
          v = minimum(maximum(v, splat(0)), splat(1));
        }
        store(dst + base + row + x, v);
      }
      for (; x < w; ++x)
        scalar(x);
    }
  }
}
}  // namespace
#endif
bool dispatch_simd(Context& c, const char* entry, std::array<uint32_t, 3> grid,
                   const Arguments& a) {
#if defined(PIXELOE_NEON) || defined(PIXELOE_AVX2)
  const bool rows = std::strncmp(entry, "lr_rows_r", 9) == 0;
  if (rows || std::strncmp(entry, "lr_cols_r", 9) == 0) {
    // Radius comes from validated internal pipeline options; match known entry
    // names rather than parsing arbitrary suffixes.
    for (int r : {2, 4, 8, 16, 32}) {
      const std::string name =
          std::string(rows ? "lr_rows_r" : "lr_cols_r") + std::to_string(r);
      if (entry == name) {
        blur(c, a, grid[2], r, rows);
        return true;
      }
    }
  }
  if (std::strcmp(entry, "morph") == 0 || std::strcmp(entry, "oe_blend") == 0) {
    morphology(c, a, grid[2], std::strcmp(entry, "oe_blend") == 0);
    return true;
  }
#else
  (void)c;
  (void)entry;
  (void)grid;
  (void)a;
#endif
  return false;
}
}  // namespace px
