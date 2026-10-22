#pragma once

#include "03-generation/noise_math.hpp"

namespace ps::plugin_internal::generation {
// Unrounded basis values accept exact octave coordinates. In particular, the
// lattice address is selected before any public floating-point rounding.
template <class E>
typename E::Rational fractal_basis(E& math,
                                   const std::array<typename E::Rational, 3>& q,
                                   bool perlin, std::uint64_t seed,
                                   std::uint64_t stream, std::uint64_t frame,
                                   const std::array<std::int64_t, 2>& period) {
  using R = typename E::Rational;
  std::array<R, 3> fraction, fade;
  std::array<std::int64_t, 3> cell{};
  const auto one = math.integer(1);
  const unsigned dimensions = perlin ? 3 : 2;
  bool lattice = true;
  for (unsigned axis = 0; axis < dimensions; ++axis) {
    if (perlin) {
      const auto reduced = math.modulo(q[axis], 256);
      cell[axis] = math.small_floor(reduced);
      fraction[axis] = math.subtract(reduced, math.integer(cell[axis]));
    } else {
      const auto magnitude = math.absolute(q[axis]);
      const auto whole = math.small_floor(magnitude);
      const auto tail = math.subtract(magnitude, math.integer(whole));
      cell[axis] = q[axis].negative
                       ? -static_cast<std::int64_t>(whole) - !E::zero(tail)
                       : static_cast<std::int64_t>(whole);
      fraction[axis] = math.subtract(q[axis], math.integer(cell[axis]));
    }
    lattice &= E::zero(fraction[axis]);
    const auto& f = fraction[axis];
    fade[axis] = math.multiply(
        math.cube(f),
        math.add(
            math.multiply(f, math.subtract(math.multiply(math.integer(6), f),
                                           math.integer(15))),
            math.integer(10)));
  }
  if (lattice)
    return math.integer(0);
  auto sum = math.integer(0);
  constexpr int gx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
  constexpr int gy[8] = {0, 0, 1, -1, 1, 1, -1, -1};
  for (unsigned corner = 0; corner < (1U << dimensions); ++corner) {
    std::array<R, 3> delta;
    for (unsigned axis = 0; axis < dimensions; ++axis)
      delta[axis] =
          math.subtract(fraction[axis], math.integer((corner >> axis) & 1));
    R dot;
    if (perlin) {
      const auto hash =
          perm(perm(perm(static_cast<unsigned>(cell[0]) + (corner & 1)) +
                    static_cast<unsigned>(cell[1]) + ((corner >> 1) & 1)) +
               static_cast<unsigned>(cell[2]) + (corner >> 2)) &
          15;
      auto u = delta[hash < 8 ? 0 : 1];
      auto v = delta[hash < 4 ? 1 : (hash == 12 || hash == 14) ? 0 : 2];
      if (hash & 1)
        u = math.negate(u);
      if (hash & 2)
        v = math.negate(v);
      dot = math.add(u, v);
    } else {
      std::array<std::int64_t, 2> address{cell[0] + (corner & 1),
                                          cell[1] + (corner >> 1)};
      for (unsigned axis = 0; axis < 2; ++axis) {
        if (period[axis]) {
          address[axis] %= period[axis];
          if (address[axis] < 0)
            address[axis] += period[axis];
        }
      }
      if (!math.work(10))
        return {};
      const auto hash =
          philox(address[0], address[1], 0, 6, seed, stream, frame)[0] & 7;
      dot = math.add(math.multiply(math.integer(gx[hash]), delta[0]),
                     math.multiply(math.integer(gy[hash]), delta[1]));
    }
    for (unsigned axis = 0; axis < dimensions; ++axis)
      dot = math.multiply(dot, (corner & (1U << axis))
                                   ? fade[axis]
                                   : math.subtract(one, fade[axis]));
    sum = math.add(sum, dot);
  }
  return sum;
}
}  // namespace ps::plugin_internal::generation
