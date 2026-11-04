// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "05-filter/restoration_noise_math.hpp"
#include "07-grade/exact_weight.hpp"
#include "07-grade/statistical_common.hpp"

namespace ps::plugin_internal::grade::statistics {
struct WeightedSample {
  double value, weight;
  std::uint64_t index;
};
struct Rank {
  std::uint64_t index;
  double fraction;
};
// Determine floor((n-1)*p) from the exact binary64 significand. Floating
// multiplication can round an almost-integral rank to the adjacent integer.
inline Rank rank(std::uint64_t n, double p) {
  if (p == 0 || n == 1) {
    return {0, 0};
  }
  if (p == 1) {
    return {n - 1, 0};
  }
  const auto b = bits(p);
  const unsigned exponent = (b >> 52) & 2047U;
  const auto significand =
      (b & ((UINT64_C(1) << 52) - 1)) | (exponent ? UINT64_C(1) << 52 : 0);
  const unsigned shift = exponent ? 1075 - exponent : 1074;
  const unsigned __int128 product =
      static_cast<unsigned __int128>(n - 1) * significand;
  if (shift >= 128) {
    return {0,
            std::ldexp(static_cast<double>(product), -static_cast<int>(shift))};
  }
  const auto i = static_cast<std::uint64_t>(product >> shift);
  const auto rem = product - (static_cast<unsigned __int128>(i) << shift);
  return {i, std::ldexp(static_cast<double>(rem), -static_cast<int>(shift))};
}
struct Quantiles {
  bool weighted;
  void* storage;
  std::uint64_t count = 0;
  ExactWeight exact_mass;
  bool negative_zero = false;
  void add(double weight) {
    ++count;
    if (weighted) {
      exact_mass.add(weight);
    }
  }
  double mass() const {
    return weighted ? exact_mass.rounded() : static_cast<double>(count);
  }
  // The caller dispatches only unweighted collection here. Storage layout and
  // signed-zero metadata remain identical to push; no exact-mass work applies.
  __attribute__((always_inline)) void push_binary(double value) {
    negative_zero |= value == 0 && std::signbit(value);
    static_cast<double*>(storage)[count++] = value;
  }
  // A caller-admitted all-one mask means every complete RGB contributes one
  // consecutive binary sample. Layout and admission proofs precede this call.
  template <class T>
  bool append_dense_linear(const T* input, std::uint64_t n,
                           const fast_color::Triple& row,
                           unsigned channels = 3) {
    if (weighted) {
      return false;
    }
    bool negative = false;
    auto* output = static_cast<double*>(storage) + count;
    const bool accepted =
        channels == 3
            ? dense_linear_span<T, 3>(input, output, n, row, &negative)
            : channels == 4 &&
                  dense_linear_span<T, 4>(input, output, n, row, &negative);
    if (!accepted) {
      return false;
    }
    count += n;
    negative_zero |= negative;
    return true;
  }
  void push(double value, double weight, std::uint64_t index) {
    negative_zero |= value == 0 && std::signbit(value);
    if (weighted) {
      static_cast<WeightedSample*>(storage)[count] = {value, weight, index};
    } else {
      static_cast<double*>(storage)[count] = value;
    }
    add(weight);
  }
  template <class Poll>
  bool weighted_multiselect(const double* probabilities, unsigned k,
                            double* out, Poll poll) {
    struct Query {
      std::pair<ExactWeight, bool> target;
      unsigned output;
    };
    Query queries[4];
    std::pair<double, unsigned> order[4];
    for (unsigned j = 0; j < k; ++j) {
      order[j] = {probabilities[j], j};
    }
    std::sort(order, order + k);
    unsigned size = 0;
    auto* a = static_cast<WeightedSample*>(storage);
    bool endpoints = false;
    for (unsigned j = 0; j < k; ++j) {
      endpoints |= probabilities[j] == 0 || probabilities[j] == 1;
    }
    if (endpoints) {
      auto low = a[0], high = a[0];
      for (std::uint64_t i = 1; i < count; ++i) {
        if (!(i & 4095U)) {
          poll();
        }
        if (a[i].value < low.value ||
            (a[i].value == low.value && a[i].index < low.index)) {
          low = a[i];
        }
        if (a[i].value > high.value ||
            (a[i].value == high.value && a[i].index > high.index)) {
          high = a[i];
        }
      }
      for (unsigned j = 0; j < k; ++j) {
        if (probabilities[j] == 0) {
          out[j] = low.value;
        } else if (probabilities[j] == 1) {
          out[j] = high.value;
        }
      }
    }
    for (unsigned j = 0; j < k; ++j) {
      if (order[j].first > 0 && order[j].first < 1) {
        queries[size++] = {exact_mass.threshold(order[j].first),
                           order[j].second};
      }
    }
    const auto reached = [](const ExactWeight& mass, const Query& q) {
      const auto comparison = mass.compare(q.target.first);
      return comparison > 0 || (comparison == 0 && !q.target.second);
    };
    std::uint64_t remaining = 8 * count;
    const auto select = [&](auto&& self, std::uint64_t lo, std::uint64_t hi,
                            unsigned first, unsigned last,
                            const ExactWeight& before, unsigned depth) -> bool {
      if (first == last) {
        return true;
      }
      if (lo == hi || depth == 64 || hi - lo > remaining) {
        return false;
      }
      remaining -= hi - lo;
      poll();
      if (hi - lo <= 32) {
        std::sort(a + lo, a + hi, [](const auto& x, const auto& y) {
          return x.value < y.value || (x.value == y.value && x.index < y.index);
        });
        auto cumulative = before;
        unsigned q = first;
        for (auto i = lo; i < hi && q < last; ++i) {
          cumulative.add(a[i].weight);
          while (q < last && reached(cumulative, queries[q])) {
            out[queries[q++].output] = a[i].value;
          }
        }
        return q == last;
      }
      double candidates[]{a[lo].value, a[lo + (hi - lo) / 2].value,
                          a[hi - 1].value};
      std::sort(candidates, candidates + 3);
      const double pivot = candidates[1];
      std::uint64_t low = lo, at = lo, high = hi, iterations = 0;
      ExactWeight less, equal;
      while (at < high) {
        if (!(++iterations & 4095U)) {
          poll();
        }
        if (a[at].value < pivot) {
          less.add(a[at].weight);
          std::swap(a[low++], a[at++]);
        } else if (pivot < a[at].value) {
          std::swap(a[at], a[--high]);
        } else {
          equal.add(a[at].weight);
          ++at;
        }
      }
      auto left = before;
      left.add(less);
      auto middle = left;
      middle.add(equal);
      unsigned left_end = first;
      while (left_end < last && reached(left, queries[left_end])) {
        ++left_end;
      }
      unsigned right_begin = left_end;
      while (right_begin < last && reached(middle, queries[right_begin])) {
        out[queries[right_begin++].output] = pivot;
      }
      return self(self, lo, low, first, left_end, before, depth + 1) &&
             self(self, high, hi, right_begin, last, middle, depth + 1);
    };
    return select(select, 0, count, 0, size, ExactWeight{}, 0);
  }
  template <class Poll>
  void evaluate(const double* probabilities, unsigned k, double* out,
                Poll poll) {
    if (!count) {
      std::fill(out, out + k, 0);
      return;
    }
    if (weighted) {
      if (count >= 4096 && k <= 4 && !negative_zero &&
          weighted_multiselect(probabilities, k, out, poll)) {
        return;
      }
      // A bounded partition attempt may permute records, but retains every
      // value/weight/index. Full stable-key sorting remains the exact fallback.
      auto* a = static_cast<WeightedSample*>(storage);
      std::uint64_t comparisons = 0;
      std::sort(a, a + count, [&](const auto& x, const auto& y) {
        if (!(++comparisons & 16383U)) {
          poll();
        }
        return x.value < y.value || (x.value == y.value && x.index < y.index);
      });
      // Probabilities need not be ordered; scan sorted values just once.
      std::pair<double, unsigned> order[64];
      for (unsigned j = 0; j < k; ++j) {
        order[j] = {probabilities[j], j};
      }
      std::sort(order, order + k);
      std::uint64_t i = 0;
      ExactWeight cumulative;
      cumulative.add(a[0].weight);
      for (unsigned j = 0; j < k; ++j) {
        const auto p = order[j].first;
        if (p == 0) {
          out[order[j].second] = a[0].value;
          continue;
        }
        if (p == 1) {
          out[order[j].second] = a[count - 1].value;
          continue;
        }
        const auto target = exact_mass.threshold(p);
        while (i + 1 < count) {
          const int comparison = cumulative.compare(target.first);
          if (comparison > 0 || (comparison == 0 && !target.second)) {
            break;
          }
          if (!(i & 4095U)) {
            poll();
          }
          cumulative.add(a[++i].weight);
        }
        out[order[j].second] = a[i].value;
      }
      return;
    }
    Rank ranks[64];
    std::uint64_t indices[128];
    unsigned m = 0;
    for (unsigned j = 0; j < k; ++j) {
      ranks[j] = rank(count, probabilities[j]);
      indices[m++] = ranks[j].index;
      if (ranks[j].fraction != 0) {
        indices[m++] = ranks[j].index + 1;
      }
    }
    if (!negative_zero && count >= 4096 && k <= 4) {
      // Sampling only proposes intervals. A complete SIMD count/compaction
      // proves each requested rank lies inside; every failed proof returns to
      // exact multiselection on the untouched original values.
      auto* a = static_cast<double*>(storage);
      auto* scratch = a + count;
      double sample[513];
      for (unsigned j = 0; j < 513; ++j) {
        sample[j] = a[(count - 1) * j / 512];
      }
      poll();
      std::sort(sample, sample + 513);
      double proposed[64];
      bool accepted = true;
      for (unsigned j = 0; j < k; ++j) {
        const auto r = ranks[j];
        const auto upper = r.index + (r.fraction != 0);
        const auto at = static_cast<unsigned>(
            static_cast<unsigned __int128>(r.index) * 512 / (count - 1));
        const double low = at > 32 ? sample[at - 32] : -INFINITY;
        const double high = at + 32 < 512 ? sample[at + 32] : INFINITY;
        const auto selected = restoration::bracket(
            a, scratch, count, low, high, [&](std::size_t) { poll(); });
        if (selected.below > r.index ||
            selected.below + selected.retained <= upper ||
            selected.retained > count / 2) {
          accepted = false;
          break;
        }
        std::uint64_t comparisons = 0;
        const auto less = [&](double x, double y) {
          if (!(++comparisons & 16383U)) {
            poll();
          }
          return x < y;
        };
        const auto hi = upper - selected.below, lo = r.index - selected.below;
        std::nth_element(scratch, scratch + hi, scratch + selected.retained,
                         less);
        const double right = scratch[hi];
        const double left =
            lo == hi ? right : *std::max_element(scratch, scratch + hi, less);
        proposed[j] = r.fraction == 0 || (left != 0 && left == right)
                          ? left
                          : (1.0 - r.fraction) * left + r.fraction * right;
      }
      if (accepted) {
        std::copy(proposed, proposed + k, out);
        return;
      }
    }
    std::sort(indices, indices + m);
    m = std::unique(indices, indices + m) - indices;
    auto* a = static_cast<double*>(storage);
    // Equal finite binary64 values differ in representation only at zero.
    // Save the requested zero ranks before nth_element destroys row-major
    // order. This exceptional path needs O(K) bits, not an index per image
    // sample.
    double zero_values[128]{};
    bool zero_rank[128]{};
    if (negative_zero) {
      std::uint64_t negatives = 0, zeros = 0;
      for (std::uint64_t i = 0; i < count; ++i) {
        if (!(i & 4095U)) {
          poll();
        }
        negatives += a[i] < 0;
        zeros += a[i] == 0;
      }
      unsigned j = 0;
      while (j < m && indices[j] < negatives) {
        ++j;
      }
      std::uint64_t ordinal = negatives;
      for (std::uint64_t i = 0;
           i < count && j < m && indices[j] < negatives + zeros; ++i) {
        if (!(i & 4095U)) {
          poll();
        }
        if (a[i] != 0) {
          continue;
        }
        if (ordinal == indices[j]) {
          zero_values[j] = a[i];
          zero_rank[j++] = true;
        }
        ++ordinal;
      }
    }
    // Partition recursively at the median requested rank, rather than invoking
    // full-array selection K times. Duplicate values retain exact order stats.
    const auto select_ranks = [&](auto&& self, std::uint64_t lo,
                                  std::uint64_t hi, unsigned first,
                                  unsigned last) -> void {
      if (first == last) {
        return;
      }
      poll();
      const auto mid = first + (last - first) / 2;
      const auto index = indices[mid];
      std::uint64_t comparisons = 0;
      const auto less = [&](double x, double y) {
        if (!(++comparisons & 16383U)) {
          poll();
        }
        return x < y;
      };
      if (index == lo) {
        std::iter_swap(a + index, std::min_element(a + lo, a + hi, less));
      } else if (index + 1 == hi) {
        std::iter_swap(a + index, std::max_element(a + lo, a + hi, less));
      } else {
        std::nth_element(a + lo, a + index, a + hi, less);
      }
      self(self, lo, index, first, mid);
      self(self, index + 1, hi, mid + 1, last);
    };
    select_ranks(select_ranks, 0, count, 0, m);
    for (unsigned j = 0; j < m; ++j) {
      if (zero_rank[j]) {
        a[indices[j]] = zero_values[j];
      }
    }
    for (unsigned j = 0; j < k; ++j) {
      const auto r = ranks[j];
      const double low = a[r.index];
      // A constant nonzero interval remains exactly constant. Independent
      // rounded products can otherwise manufacture unequal low/high controls
      // for an image with only one distinct value. Zero interpolation keeps
      // its ordinary arithmetic sign behavior.
      out[j] = r.fraction == 0 || (low != 0 && low == a[r.index + 1])
                   ? low
                   : (1 - r.fraction) * low + r.fraction * a[r.index + 1];
    }
  }
};
struct ExposureParameters {
  double target = .18, minimum = -16, maximum = 16;
  explicit ExposureParameters(const Parameters& p)
      : target(real(p, "target", .18)),
        minimum(real(p, "ev_min", -16)),
        maximum(real(p, "ev_max", 16)) {}
  std::pair<double, bool> operator()(double q, std::uint64_t count) const {
    if (!count || q <= 0) {
      return {0, true};
    }
    return {std::clamp(std::log2(target / q), minimum, maximum), false};
  }
};
struct LevelsParameters {
  double gamma, out0, out1;
  explicit LevelsParameters(const Parameters& p)
      : gamma(real(p, "gamma", 1)),
        out0(real(p, "out0", 0)),
        out1(real(p, "out1", 1)) {}
  bool operator()(double low, double high, std::uint64_t count,
                  double* out) const {
    if (!count || low == high) {
      const double placeholder[]{0, 1, 1, 0, 1};
      std::copy(placeholder, placeholder + 5, out);
      return true;
    }
    out[0] = low;
    out[1] = high;
    out[2] = gamma;
    out[3] = out0;
    out[4] = out1;
    return false;
  }
};
}  // namespace ps::plugin_internal::grade::statistics
