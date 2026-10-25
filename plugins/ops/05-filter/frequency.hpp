#pragma once

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
struct Frequency final : Program {
  Kind kind;
  Profile profile;
  Grid grid;
  // sqrt(HW)=norm_squarefactor*sqrt(norm_squarefree). Exact refinement uses
  // a frequency-specific primitive order including the Gauss discriminant.
  std::uint64_t norm_squarefactor = 1;
  std::uint64_t norm_squarefree = 1, norm_discriminant = 1;
  std::vector<unsigned> axes, broadcast;
  std::vector<std::uint64_t> dims;
  ElementType type = ElementType::Float64;
  std::string mode, pass, sampling;
  double alpha = 0, low = 0, high = 0, sy = 0, sx = 0, depth = 0;
  std::int64_t order = 1;
  std::vector<std::pair<double, double>> centers;
  Region selected;
  Frequency(Kind, Profile, const std::vector<OperationMetadata>&,
            const Parameters&);
  bool transform() const { return kind <= Kind::Irfft; }
  bool inverse() const { return kind == Kind::Ifft || kind == Kind::Irfft; }
  bool real_input() const { return kind == Kind::Rfft; }
  std::uint64_t transform_width() const {
    const auto w = grid.shape[grid.xa];
    return (kind == Kind::Rfft || kind == Kind::Irfft) && w % 2 == 0 ? w / 2
                                                                     : w;
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
