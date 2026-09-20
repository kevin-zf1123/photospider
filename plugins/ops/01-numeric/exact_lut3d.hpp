#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/exact_polynomial.hpp"

namespace ps::plugin_internal::numeric_ops {
// All local coordinates share the positive product H0*H1*H2 denominator.
// Axis differences have <2100 bits, weights <6300 and weighted component
// numerators <8400 bits. The existing admitted polynomial arena also covers
// final IEEE midpoint alignment. No per-point big-integer owner is retained.
class ExactLut3d final {
 public:
  struct Support {
    std::array<std::uint8_t, 8> vertices{};
    unsigned count = 0;
  };
  using Cell = std::array<std::array<std::uint64_t, 2>, 3>;

 private:
  ExactPolynomial math_;
  using Index = ExactPolynomial::Index;
  std::array<Index, 8> weights_{};
  Index denominator_ = 0;
  Support support_;
  Status prepare(const Cell& cell, const std::array<std::uint64_t, 3>& query,
                 bool tetrahedral) {
    support_ = {};
    const auto zero = math_.integer(0);
    std::array<Index, 3> n{}, h{}, complement{};
    for (unsigned i = 0; i < 3; ++i) {
      const auto first = math_.binary(cell[i][0], 1074);
      const auto last = math_.binary(cell[i][1], 1074);
      const auto x = math_.binary(query[i], 1074);
      n[i] = math_.add(x, first, true);
      h[i] = math_.add(last, first, true);
      if (math_.sign(h[i]) < 0) {
        n[i] = math_.add(zero, n[i], true);
        h[i] = math_.add(zero, h[i], true);
      }
      complement[i] = math_.add(h[i], n[i], true);
    }
    if (!math_.status().ok())
      return math_.status();
    denominator_ = math_.multiply(math_.multiply(h[0], h[1]), h[2]);
    for (auto& weight : weights_)
      weight = math_.integer(0);
    std::array<unsigned, 3> order{0, 1, 2};
    if (tetrahedral) {
      // Stable descending exact-rational order; equal fractions retain axis
      // index order. Three-element insertion sorting has a fixed work bound.
      const auto base = math_.mark();
      for (unsigned i = 1; i < 3; ++i) {
        unsigned j = i;
        while (j) {
          const auto a = order[j - 1], b = order[j];
          const auto left = math_.multiply(n[a], h[b]);
          const auto right = math_.multiply(n[b], h[a]);
          if (!math_.status().ok())
            return math_.status();
          const auto comparison = math_.compare_magnitude(left, right);
          math_.restore(base);
          if (comparison >= 0)
            break;
          order[j] = a;
          order[--j] = b;
        }
      }
      std::array<Index, 3> scaled{};
      for (unsigned i = 0; i < 3; ++i)
        scaled[i] = math_.multiply(math_.multiply(n[i], h[(i + 1) % 3]),
                                   h[(i + 2) % 3]);
      math_.copy(math_.add(denominator_, scaled[order[0]], true), weights_[0]);
      math_.copy(math_.add(scaled[order[0]], scaled[order[1]], true),
                 weights_[1]);
      math_.copy(math_.add(scaled[order[1]], scaled[order[2]], true),
                 weights_[2]);
      math_.copy(scaled[order[2]], weights_[3]);
    } else {
      const auto base = math_.mark();
      for (unsigned vertex = 0; vertex < 8; ++vertex) {
        const auto a = vertex & 1 ? n[0] : complement[0];
        const auto b = vertex & 2 ? n[1] : complement[1];
        const auto c = vertex & 4 ? n[2] : complement[2];
        math_.copy(math_.multiply(math_.multiply(a, b), c), weights_[vertex]);
        math_.restore(base);
      }
    }
    if (!math_.status().ok())
      return math_.status();
    unsigned vertex = 0;
    for (unsigned i = 0; i < (tetrahedral ? 4U : 8U); ++i) {
      if (tetrahedral && i)
        vertex |= 1U << order[i - 1];
      else if (!tetrahedral)
        vertex = i;
      if (math_.sign(weights_[i])) {
        support_.vertices[support_.count] = static_cast<std::uint8_t>(vertex);
        // Compact into preallocated destinations, preserving later sources.
        math_.copy(weights_[i], weights_[support_.count++]);
      }
    }
    return math_.status();
  }

 public:
  explicit ExactLut3d(SequenceProfile profile) : math_(profile) {}
  // Caller supplies distinct finite cell endpoints and an already clamped
  // query inside each stored-index cell. Every denominator is then nonzero
  // and all prepared weights are nonnegative with a positive total.
  Result<Support> support(const Cell& cell,
                          const std::array<std::uint64_t, 3>& query,
                          bool tetrahedral,
                          const std::function<Status(std::uint64_t)>& consume) {
    math_.begin(consume);
    struct End {
      ExactPolynomial& math;
      ~End() { math.end(); }
    } end{math_};
    auto status = prepare(cell, query, tetrahedral);
    return status.ok() ? Result<Support>(support_) : Result<Support>(status);
  }
  // colors follows support()'s compact vertex order. Source bits are finite,
  // exact-widened binary64; zero-weight vertices have never been read.
  Result<std::array<std::uint64_t, 3>> evaluate(
      const Cell& cell, const std::array<std::uint64_t, 3>& query,
      bool tetrahedral,
      const std::array<std::array<std::uint64_t, 3>, 8>& colors, bool narrow,
      const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::array<std::uint64_t, 3>>;
    math_.begin(consume);
    struct End {
      ExactPolynomial& math;
      ~End() { math.end(); }
    } end{math_};
    auto status = prepare(cell, query, tetrahedral);
    if (!status.ok())
      return Answer(status);
    const auto base = math_.mark();
    std::array<std::uint64_t, 3> result{};
    for (unsigned channel = 0; channel < 3; ++channel) {
      math_.restore(base);
      const auto sum = math_.integer(0);
      const auto temporary = math_.mark();
      bool all_negative_zero = true;
      for (unsigned i = 0; i < support_.count; ++i) {
        all_negative_zero &= colors[i][channel] == (UINT64_C(1) << 63);
        const auto value = math_.binary(colors[i][channel], 1074);
        math_.copy(math_.add(sum, math_.multiply(weights_[i], value)), sum);
        if (!math_.status().ok())
          return Answer(math_.status());
        math_.restore(temporary);
      }
      if (!math_.sign(sum) && all_negative_zero) {
        result[channel] = UINT64_C(1) << (narrow ? 31 : 63);
      } else {
        auto value = math_.round(sum, denominator_, narrow, -1074);
        if (!value.ok())
          return Answer(value.status());
        result[channel] = value.value();
      }
    }
    return Answer(result);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
