#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/arithmetic.hpp"

namespace ps::plugin_internal::filter {
enum class FieldKind {
  Central,
  Sobel,
  Scharr,
  Magnitude,
  Orientation,
  Laplacian4,
  Laplacian8,
  Hessian,
  Structure,
  Eigen,
  LogKernel,
  LogFilter,
  Difference,
  Unsharp,
  Detail,
  Moments,
  Covariance
};
struct Fields final : Program {
  FieldKind kind;
  Profile profile;
  ElementType type = ElementType::Float64;
  std::optional<Geometry> geometry;
  std::shared_ptr<Filter> small, large;
  double dx = 1, dy = 1, scale = 1, gain = 1, threshold = 0, sigma = 1;
  std::uint64_t radius = 0, ddof = 0;
  bool normalized = false;
  std::string mode;
  Fields(FieldKind, Profile, const std::vector<OperationMetadata>&,
         const Parameters&);
  std::vector<ReadRegion> support(unsigned, const Region&) const override;
  std::uint64_t task_scratch(unsigned, const Region&) const override;
  std::uint64_t staging_scratch(const Region&) const;
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override;
  bool pointwise() const;
  bool identity() const;
  bool window_rows() const {
    return type == ElementType::Float32 &&
           (kind == FieldKind::Structure || kind == FieldKind::Moments ||
            kind == FieldKind::Covariance) &&
           geometry->kh <= 1024 && geometry->kw <= 1024;
  }
};
std::vector<std::pair<int, int>> stencil_offsets(FieldKind, unsigned);
OperationDefinition make_field(const std::string&, FieldKind, Profile);
void run_stencil(const Fields&, const Kernel&, const ps_kernel_invocation_v8&,
                 const Work&);
void run_pointwise(const Fields&, const Kernel&, const ps_kernel_invocation_v8&,
                   const Work&);
void run_log(const Fields&, const Kernel&, const ps_kernel_invocation_v8&,
             const Work&);
void run_staged(const Fields&, const Kernel&, const ps_kernel_invocation_v8&,
                const Work&);
std::uint64_t log_coefficient(generation::Math&, double, std::uint64_t,
                              std::uint64_t, bool);
std::optional<std::uint64_t> fused_field(Atom, Atom, double, bool, Profile,
                                         const Fuel&);
std::uint64_t field_angle(generation::Math&, Rational, Rational, bool, bool,
                          bool);
}  // namespace ps::plugin_internal::filter
