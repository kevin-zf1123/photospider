#pragma once

#include <string>
#include <vector>

#include "05-filter/certified.hpp"

namespace ps::plugin_internal::frequency {
using namespace filter;  // NOLINT(build/namespaces)
using Math = generation::Math;
using Directed = generation::Directed;
using Interval = generation::Interval;

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
  // sqrt(HW)=norm_squarefactor*sqrt(norm_squarefree). The primitive
  // root order includes its positive quadratic Gauss-sum discriminant.
  std::uint64_t root_order = 4, norm_squarefactor = 1;
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
  Region plane_region(const Region&) const;
  std::vector<ReadRegion> support(unsigned, const Region&) const override;
  std::uint64_t task_scratch(unsigned, const Region&) const override;
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override;
  void run_transform(const Kernel&, const ps_kernel_invocation_v8&) const;
  void run_local(const Kernel&, const ps_kernel_invocation_v8&) const;
  std::uint64_t window1d(Math&, std::uint64_t, std::uint64_t) const;
  std::uint64_t response(Math&, std::uint64_t, std::uint64_t) const;
};
void sincos_turn(Math&, Interval sine, Interval cosine, const Rational& turns);
OperationDefinition make_frequency(const std::string&, Kind, Profile);
OperationDefinition transform_tasks(OperationDefinition);
}  // namespace ps::plugin_internal::frequency
