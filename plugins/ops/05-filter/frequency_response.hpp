#pragma once

#include <algorithm>
#include <cstdint>
#include <new>
#include <optional>

#include "05-filter/frequency_fast.hpp"

namespace ps::plugin_internal::frequency {
// Invocation-private unrounded enclosures. No coefficient is baked to RN64
// between exp(axis) and the complete response's final rounding certificate.
struct ResponseAxes final {
  const Frequency& state;
  const Fuel& fuel;
  Ball* values;
  std::uint8_t* active;
  std::uint64_t y0, x0, height, width, stride, terms;
  ResponseAxes(const Frequency& s, Math& m, const Fuel& consumer,
               const ps_kernel_mutable_view_v8& view, void* storage)
      : state(s),
        fuel(consumer),
        values(static_cast<Ball*>(storage)),
        y0(view.layout->origin[0]),
        x0(view.layout->origin[1]),
        height(view.layout->shape[0]),
        width(view.layout->shape[1]),
        stride(2 * (height + width)),
        terms(s.kind == Kind::Notch  ? 2 * s.centers.size()
              : s.kind == Kind::Band ? 2
                                     : 1) {
    active = reinterpret_cast<std::uint8_t*>(values + terms * stride);
    for (std::uint64_t term = 0; term < terms; ++term) {
      fuel.poll();
      active[term] = 1;
      if (s.kind == Kind::Notch && (term & 1)) {
        const auto& center = s.centers[term / 2];
        auto& e = m.exact;
        const auto cy =
            m.scale(m.mul(m.value(center.first), m.value(s.grid.dy)), 2);
        const auto cx =
            m.scale(m.mul(m.value(center.second), m.value(s.grid.dx)), 2);
        if (m.is_zero(e.modulo(cy, 1)) && m.is_zero(e.modulo(cx, 1)))
          active[term] = 0;
        check(e.status());
      }
      if (!active[term])
        continue;
      for (unsigned axis = 0; axis < 2; ++axis) {
        const auto n = s.dims[axis], begin = axis == 0 ? y0 : x0;
        const auto count = axis == 0 ? height : width;
        const auto spacing = axis == 0 ? s.grid.dy : s.grid.dx;
        for (unsigned mirrored = 0;
             mirrored < (s.kind == Kind::Radial ? 1U : 2U); ++mirrored)
          for (std::uint64_t j = 0; j < count; ++j) {
            fuel.poll(512);
            auto k = begin + j;
            if (mirrored)
              k = (n - k) % n;
            Ball exponent;
            if (s.kind == Kind::Notch) {
              const auto& center = s.centers[term / 2];
              const auto center_value =
                  (axis == 0 ? center.first : center.second) *
                  ((term & 1) ? -1 : 1);
              const auto delta = torus_ball(frequency_coordinate(k, n, spacing),
                                            center_value, spacing);
              const auto q =
                  dd_divide(delta, Ball::point(axis == 0 ? s.sy : s.sx));
              exponent = dd_product(q, q).times(-.5);
            } else {
              // The squared signed frequency is identical at conjugate indices.
              const auto f =
                  frequency_coordinate(std::min(k, n - k), n, spacing);
              const auto q =
                  dd_divide(f, Ball::point(term == 0 ? s.high : s.low));
              exponent = dd_product(q, q).times(-.5);
            }
            const auto value =
                exponent.point() && exponent.hi == 0 ? Ball{1, 0, 0}
                : s.kind == Kind::Notch ? exp_ball<18>(exponent, fuel)
                                        : exp_ball(exponent, fuel);
            new (values + term * stride +
                 (axis == 0 ? mirrored * height + j
                            : 2 * height + mirrored * width + j)) Ball(value);
          }
      }
    }
    if (s.kind != Kind::Radial)
      return;
    for (std::uint64_t term = 0; term < terms; ++term) {
      fuel.poll();
      if (!active[term])
        continue;
      const auto partner = term;
      // Radial squared frequency is invariant under axis reflection.
      for (std::uint64_t j = 0; j < height; ++j) {
        fuel.poll(8);
        new (values + term * stride + height + j)
            Ball(values[partner * stride + j]);
      }
      for (std::uint64_t j = 0; j < width; ++j) {
        fuel.poll(8);
        new (values + term * stride + 2 * height + width + j)
            Ball(values[partner * stride + 2 * height + j]);
      }
    }
  }
  std::optional<std::uint64_t> evaluate(std::uint64_t y,
                                        std::uint64_t x) const {
    const auto& s = state;
    const auto ny = (s.dims[0] - y) % s.dims[0],
               nx = (s.dims[1] - x) % s.dims[1];
    const bool mirrored = ny * s.dims[1] + nx < y * s.dims[1] + x;
    return evaluate_mapped(y, x, mirrored);
  }
  std::optional<std::uint64_t> evaluate_mapped(std::uint64_t y, std::uint64_t x,
                                               bool mirrored) const {
    const auto& s = state;
    const auto yy = y - y0 + (mirrored ? height : 0);
    const auto xx = x - x0 + 2 * height + (mirrored ? width : 0);
    const auto factor = [&](std::uint64_t term) {
      fuel.poll(256);
      return dd_product(values[term * stride + yy], values[term * stride + xx]);
    };
    if (s.kind == Kind::Notch) {
      Ball value{1, 0, 0};
      if (s.depth == 0)
        return bits(1);
      for (std::uint64_t term = 0; term < terms; ++term) {
        fuel.poll(256);
        if (active[term])
          value =
              dd_product(value, Ball{1, 0, 0} + (-factor(term).times(s.depth)));
      }
      return finish_ball(value, false, s.profile);
    }
    if (x == 0 && y == 0)
      return bits(s.pass == "low" || s.pass == "reject" ? 1 : 0);
    auto value = factor(0);
    if (s.kind == Kind::Band)
      value = value + (-factor(1));
    if (s.pass == "high" || s.pass == "reject")
      value = Ball{1, 0, 0} + (-value);
    return finish_ball(value, false, s.profile);
  }
};
}  // namespace ps::plugin_internal::frequency
