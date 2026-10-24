#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

// Native SMAA 1x LOW arithmetic. Texture coordinates are measured in texels;
// integer positions name texel centers. The shader profile and table provenance
// are pinned in FIL-19B. Every arithmetic operation on color/weights is RN32.
namespace ps::plugin_internal::filter::smaa {
using Pair = std::array<float, 2>;
using Quad = std::array<float, 4>;
float area_texel(std::int64_t y, std::int64_t x, unsigned channel);
float search_texel(std::int64_t y, std::int64_t x);
inline float mix(float a, float b, float t) {
  return a * (1.0F - t) + b * t;
}
template <class Fetch>
float linear(Fetch fetch, double y, double x) {
  const auto iy = static_cast<std::int64_t>(std::floor(y));
  const auto ix = static_cast<std::int64_t>(std::floor(x));
  const float fy = static_cast<float>(y - iy);
  const float fx = static_cast<float>(x - ix);
  const float a = mix(fetch(iy, ix), fetch(iy, ix + 1), fx);
  const float b = mix(fetch(iy + 1, ix), fetch(iy + 1, ix + 1), fx);
  return mix(a, b, fy);
}
template <class Fetch>
Pair linear_pair(Fetch fetch, double y, double x) {
  const auto iy = static_cast<std::int64_t>(std::floor(y));
  const auto ix = static_cast<std::int64_t>(std::floor(x));
  const float fy = static_cast<float>(y - iy), fx = static_cast<float>(x - ix);
  const auto a = fetch(iy, ix), b = fetch(iy, ix + 1);
  const auto c = fetch(iy + 1, ix), d = fetch(iy + 1, ix + 1);
  Pair result;
  for (unsigned i = 0; i < 2; ++i)
    result[i] = mix(mix(a[i], b[i], fx), mix(c[i], d[i], fx), fy);
  return result;
}
template <class Guide>
Pair edges(Guide guide, std::int64_t y, std::int64_t x) {
  const float center = guide(y, x), left = guide(y, x - 1);
  const float top = guide(y - 1, x);
  const float dx = std::abs(center - left), dy = std::abs(center - top);
  Pair result{dx >= 0.15F ? 1.0F : 0.0F, dy >= 0.15F ? 1.0F : 0.0F};
  if (result[0] + result[1] == 0)
    return result;
  const float right = std::abs(center - guide(y, x + 1));
  const float bottom = std::abs(center - guide(y + 1, x));
  const float left2 = std::abs(left - guide(y, x - 2));
  const float top2 = std::abs(top - guide(y - 2, x));
  const float contrast = std::max({dx, dy, right, bottom, left2, top2});
  result[0] *= 2.0F * dx >= contrast ? 1.0F : 0.0F;
  result[1] *= 2.0F * dy >= contrast ? 1.0F : 0.0F;
  return result;
}
inline float search_length(Pair e, bool positive) {
  // Original normalized SearchTex transform, reduced to texel coordinates.
  const float x = std::fma(32.0F, e[0], positive ? 33.0F : 0.0F);
  const float y = std::fma(-32.0F, e[1], 32.0F);
  return linear(search_texel, y, x);
}
template <class Edge>
float search(Edge edge, double y, double x, bool vertical, bool positive) {
  Pair e = vertical ? Pair{1, 0} : Pair{0, 1};
  const double start = vertical ? y : x;
  for (unsigned i = 0;
       i < 4 && e[vertical ? 0 : 1] > 0.8281F && e[vertical ? 1 : 0] == 0;
       ++i) {
    e = linear_pair(edge, y, x);
    (vertical ? y : x) += positive ? 2 : -2;
  }
  if (vertical)
    std::swap(e[0], e[1]);
  const float adjust =
      std::fma(-255.0F / 127.0F, search_length(e, positive), 3.25F);
  return static_cast<float>((vertical ? y : x) - start) +
         (positive ? -adjust : adjust);
}
inline Pair area(float d0, float d1, float e0, float e1) {
  const float x = std::fma(16.0F, std::nearbyint(4.0F * e0), std::sqrt(d0));
  const float y = std::fma(16.0F, std::nearbyint(4.0F * e1), std::sqrt(d1));
  Pair result;
  for (unsigned c = 0; c < 2; ++c)
    result[c] =
        linear([&](auto yy, auto xx) { return area_texel(yy, xx, c); }, y, x);
  return result;
}
template <class Edge>
Quad weights(Edge edge, std::int64_t y, std::int64_t x) {
  Quad result{};
  const Pair e = edge(y, x);
  if (e[1] > 0) {
    const float left = -0.25F + search(edge, y - 0.125, x - 0.25, false, false);
    const float right = 1.25F + search(edge, y - 0.125, x + 1.25, false, true);
    const float e0 = linear([&](auto yy, auto xx) { return edge(yy, xx)[0]; },
                            y - 0.25, static_cast<double>(x) + left);
    const float e1 = linear([&](auto yy, auto xx) { return edge(yy, xx)[0]; },
                            y - 0.25, static_cast<double>(x) + right + 1);
    const Pair a = area(std::abs(std::nearbyint(left)),
                        std::abs(std::nearbyint(right)), e0, e1);
    result[0] = a[0];
    result[1] = a[1];
  }
  if (e[0] > 0) {
    const float up = -0.25F + search(edge, y - 0.25, x - 0.125, true, false);
    const float down = 1.25F + search(edge, y + 1.25, x - 0.125, true, true);
    const float e0 = linear([&](auto yy, auto xx) { return edge(yy, xx)[1]; },
                            static_cast<double>(y) + up, x - 0.25);
    const float e1 = linear([&](auto yy, auto xx) { return edge(yy, xx)[1]; },
                            static_cast<double>(y) + down + 1, x - 0.25);
    const Pair a = area(std::abs(std::nearbyint(up)),
                        std::abs(std::nearbyint(down)), e0, e1);
    result[2] = a[0];
    result[3] = a[1];
  }
  return result;
}
struct Blend {
  bool active = false, horizontal = false;
  float positive = 0, negative = 0, wp = 0, wn = 0;
};
template <class Weights>
Blend blend(Weights weights, std::int64_t y, std::int64_t x) {
  const auto c = weights(y, x);
  const Quad a{weights(y, x + 1)[3], weights(y + 1, x)[1], c[2], c[0]};
  Blend b;
  if (((a[0] + a[1]) + a[2]) + a[3] < 1e-5F)
    return b;
  b.active = true;
  b.horizontal = std::max(a[0], a[2]) > std::max(a[1], a[3]);
  b.positive = a[b.horizontal ? 0 : 1];
  b.negative = a[b.horizontal ? 2 : 3];
  const float sum = b.positive + b.negative;
  b.wp = b.positive / sum;
  b.wn = b.negative / sum;
  return b;
}
template <class Color>
float neighborhood(Color color, std::int64_t y, std::int64_t x, Blend b) {
  const float positive =
      linear(color, static_cast<double>(y) + (b.horizontal ? 0 : b.positive),
             static_cast<double>(x) + (b.horizontal ? b.positive : 0));
  const float negative =
      linear(color, static_cast<double>(y) - (b.horizontal ? 0 : b.negative),
             static_cast<double>(x) - (b.horizontal ? b.negative : 0));
  return positive * b.wp + negative * b.wn;
}
}  // namespace ps::plugin_internal::filter::smaa
