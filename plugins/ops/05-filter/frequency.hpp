#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "05-filter/certified.hpp"

namespace ps::plugin_internal::frequency {
using namespace filter;  // NOLINT(build/namespaces)
using Math = generation::Math;
using Directed = generation::Directed;
using Interval = generation::Interval;

inline std::uint64_t bluestein_size(std::uint64_t n) {
  auto residual = n;
  for (auto factor : {2U, 3U, 5U})
    while (residual % factor == 0)
      residual /= factor;
  if (residual <= 31)
    return 0;
  // Shape products are bounded by 2^40 at static preparation.
  std::uint64_t size = 1;
  while (size < 2 * n - 1)
    size *= 2;
  return size;
}
// These Fermat primes have primitive root 3 and a radix-two Rader convolution.
// Other large residuals keep Bluestein; no unbounded primality search is
// needed.
inline std::uint64_t rader_generator(std::uint64_t n) {
  return n == 257 || n == 65537 ? 3 : 0;
}
inline std::uint64_t convolution_scratch(std::uint64_t n) {
  if (rader_generator(n))
    return n - 1;
  const auto size = bluestein_size(n);
  return size ? n + 2 * size : 0;
}
enum class Kind {
  Fft,
  Ifft,
  Rfft,
  Irfft,
  Project,
  Shift,
  Unshift,
  Window,
  Apply,
  Statistics,
  Radial,
  Band,
  Notch,
  Multiply
};

// Caller-owned interpretation. No data-dependent metadata or validity scan.
struct Grid {
  std::vector<std::uint64_t> shape;
  unsigned ya = 0, xa = 1;
  std::string packing, order, norm, unit, identity;
  double dy = 1, dx = 1, oy = 0, ox = 0;
  Grid() = default;
  explicit Grid(const Parameters&);
  std::vector<std::uint64_t> stored_shape() const;
};
struct TransformTables;
struct Frequency final : Program {
  Kind kind;
  Profile profile;
  Grid grid;
  std::vector<unsigned> axes, broadcast;
  std::vector<std::uint64_t> dims;
  ElementType type = ElementType::Float64;
  std::string mode, pass, sampling;
  double alpha = 0, low = 0, high = 0, sy = 0, sx = 0, depth = 0;
  std::int64_t order = 1;
  std::vector<std::pair<double, double>> centers;
  Region selected;
  // Created only for the prepared-kernel copy, never static operation state.
  std::shared_ptr<TransformTables> tables;
  Frequency(Kind, Profile, const std::vector<OperationMetadata>&,
            const Parameters&);
  bool transform() const { return kind <= Kind::Irfft; }
  bool inverse() const { return kind == Kind::Ifft || kind == Kind::Irfft; }
  bool real_input() const { return kind == Kind::Rfft; }
  bool compact_real() const {
    return (kind == Kind::Rfft || kind == Kind::Irfft) &&
           grid.shape[grid.xa] % 2 == 0;
  }
  std::uint64_t working_width() const {
    const auto w = grid.shape[grid.xa];
    return compact_real() ? w / 2 + 1 : w;
  }
  std::uint64_t row_length() const {
    const auto w = grid.shape[grid.xa];
    return compact_real() ? w / 2 : w;
  }
  std::uint64_t column_count() const {
    const auto h = grid.shape[grid.ya], w = grid.shape[grid.xa];
#if defined(__x86_64__)
    const auto maximum = h >= 1024 ? UINT64_C(64) : UINT64_C(8);
#else
    const auto maximum = UINT64_C(8);
#endif
    return h > 1 && std::max(h, w) >= 1024 && !convolution_scratch(h)
               ? std::min(maximum, working_width())
               : 0;
  }
  std::uint64_t column_scratch() const {
    const auto count = column_count();
    return count * (grid.shape[grid.ya] + (count > 8 ? 1 : 0));
  }
  Region plane_region(const Region&) const;
  std::vector<ReadRegion> support(unsigned, const Region&) const override;
  std::uint64_t task_scratch(unsigned, const Region&) const override;
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override;
  void run_v9(const Kernel&, const ps_kernel_invocation_v9&) const override;
  void run_transform(const Kernel&, const ps_kernel_invocation_v8&,
                     ps_kernel_work_v9 = {}) const;
  void run_local(const Kernel&, const ps_kernel_invocation_v8&,
                 ps_kernel_work_v9 = {}) const;
  std::uint64_t window1d(Math&, std::uint64_t, std::uint64_t) const;
  std::uint64_t response(Math&, std::uint64_t, std::uint64_t) const;
};
void sincos_turn(Math&, Interval sine, Interval cosine, const Rational& turns);
OperationDefinition make_frequency(const std::string&, Kind, Profile);
OperationDefinition transform_tasks(OperationDefinition);
}  // namespace ps::plugin_internal::frequency
