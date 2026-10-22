#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "03-generation/math.hpp"

namespace ps::plugin_internal::generation {
inline std::array<std::uint64_t, 4> philox(std::int64_t x, std::int64_t y,
                                           std::uint64_t channel,
                                           unsigned domain, std::uint64_t seed,
                                           std::uint64_t stream,
                                           std::uint64_t frame) {
  std::array<std::uint64_t, 4> c{
      static_cast<std::uint32_t>(x) |
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 32),
      frame,
      stream | (channel << 32) | (static_cast<std::uint64_t>(domain) << 48), 0};
  std::uint64_t k0 = seed, k1 = 0;
  for (unsigned round = 0; round < 10; ++round) {
    const auto a =
        static_cast<unsigned __int128>(UINT64_C(0xd2e7470ee14c6c93)) * c[0];
    const auto b =
        static_cast<unsigned __int128>(UINT64_C(0xca5a826395121157)) * c[2];
    c = {static_cast<std::uint64_t>(b >> 64) ^ c[1] ^ k0,
         static_cast<std::uint64_t>(b),
         static_cast<std::uint64_t>(a >> 64) ^ c[3] ^ k1,
         static_cast<std::uint64_t>(a)};
    k0 += UINT64_C(0x9e3779b97f4a7c15);
    k1 += UINT64_C(0xbb67ae8584caa73b);
  }
  return c;
}
inline constexpr std::array<unsigned char, 256> permutation{
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,
    225, 140, 36,  103, 30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190,
    6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203, 117,
    35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136,
    171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158,
    231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,
    245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209,
    76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,
    164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,
    202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,
    58,  17,  182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,
    154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
    19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,
    228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,
    145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184,
    84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
    222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156,
    180};  // NOLINT(whitespace/indent_namespace)
inline unsigned perm(unsigned i) {
  return permutation[i & 255];
}

template <class E>
std::uint64_t perlin_exact(E& math, const std::array<double, 3>& q,
                           bool narrow) {
  using R = typename E::Rational;
  std::array<R, 3> f, fade;
  std::array<unsigned, 3> cell;
  const auto one = math.integer(1);
  bool lattice = true;
  for (unsigned a = 0; a < 3; ++a) {
    const auto reduced = math.modulo(math.binary(bits(q[a])), 256);
    cell[a] = math.small_floor(reduced);
    f[a] = math.subtract(reduced, math.integer(cell[a]));
    lattice &= E::zero(f[a]);
    fade[a] = math.multiply(
        math.cube(f[a]),
        math.add(math.multiply(
                     f[a], math.subtract(math.multiply(math.integer(6), f[a]),
                                         math.integer(15))),
                 math.integer(10)));
  }
  if (lattice) {
    return 0;
  }
  auto sum = math.integer(0);
  for (unsigned c = 0; c < 8; ++c) {
    const unsigned dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
    const unsigned h =
        perm(perm(perm(cell[0] + dx) + cell[1] + dy) + cell[2] + dz) & 15;
    const std::array<R, 3> delta{math.subtract(f[0], math.integer(dx)),
                                 math.subtract(f[1], math.integer(dy)),
                                 math.subtract(f[2], math.integer(dz))};
    auto u = delta[h < 8 ? 0 : 1], v = delta[h < 4                  ? 1
                                             : (h == 12 || h == 14) ? 0
                                                                    : 2];
    if (h & 1) {
      u = math.negate(u);
    }
    if (h & 2) {
      v = math.negate(v);
    }
    auto contribution = math.add(u, v);
    for (unsigned a = 0; a < 3; ++a) {
      contribution = math.multiply(
          contribution,
          (c & (1U << a)) ? fade[a] : math.subtract(one, fade[a]));
    }
    sum = math.add(sum, contribution);
  }
  if (E::zero(sum)) {
    sum.negative = false;
  }
  const auto result = math.rounded(sum, narrow);
  check(math.status());
  return result;
}
template <class E>
std::uint64_t gradient_exact(E& math, const std::array<double, 3>& q,
                             bool narrow, std::uint64_t seed,
                             std::uint64_t stream, std::uint64_t frame,
                             const std::array<std::int64_t, 2>& period) {
  using R = typename E::Rational;
  std::array<R, 2> f, fade;
  std::array<std::int64_t, 2> cell;
  const auto one = math.integer(1);
  for (unsigned a = 0; a < 2; ++a) {
    cell[a] = static_cast<std::int64_t>(std::floor(q[a]));
    f[a] = math.subtract(math.binary(bits(q[a])), math.integer(cell[a]));
    fade[a] = math.multiply(
        math.cube(f[a]),
        math.add(math.multiply(
                     f[a], math.subtract(math.multiply(math.integer(6), f[a]),
                                         math.integer(15))),
                 math.integer(10)));
  }
  auto sum = math.integer(0);
  constexpr int gx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
  constexpr int gy[8] = {0, 0, 1, -1, 1, 1, -1, -1};
  for (unsigned c = 0; c < 4; ++c) {
    std::int64_t ix = cell[0] + (c & 1), iy = cell[1] + (c >> 1);
    if (period[0]) {
      ix %= period[0];
      if (ix < 0) {
        ix += period[0];
      }
    }
    if (period[1]) {
      iy %= period[1];
      if (iy < 0) {
        iy += period[1];
      }
    }
    const auto h = philox(ix, iy, 0, 6, seed, stream, frame)[0] & 7;
    const auto dot =
        math.add(math.multiply(math.integer(gx[h]),
                               math.subtract(f[0], math.integer(c & 1))),
                 math.multiply(math.integer(gy[h]),
                               math.subtract(f[1], math.integer(c >> 1))));
    sum = math.add(
        sum,
        math.multiply(
            math.multiply(dot, (c & 1) ? fade[0] : math.subtract(one, fade[0])),
            (c & 2) ? fade[1] : math.subtract(one, fade[1])));
  }
  if (E::zero(sum)) {
    sum.negative = false;
  }
  const auto result = math.rounded(sum, narrow);
  check(math.status());
  return result;
}
}  // namespace ps::plugin_internal::generation
