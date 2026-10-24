#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "05-filter/arithmetic.hpp"

namespace ps::plugin_internal::filter {
inline constexpr std::size_t kBilateralRangeCacheEntries = 4096;
enum class LocalKind { Median, Percentile, Bilateral, Joint, Guided, Vector };
struct LocalFilter final : Program {
  LocalKind kind;
  Profile profile;
  ElementType type;
  Geometry geometry;
  int component_axis = -1;
  unsigned components = 1;
  std::array<double, 4> scales{1, 1, 1, 1};
  double quantile = .5, sy = 1, sx = 1, sr = 1, epsilon = 1;
  std::string policy, empty;
  LocalFilter(LocalKind, Profile, const std::vector<OperationMetadata>&,
              const Parameters&);
  bool rank_filter() const;
  bool guided() const;
  bool identity() const;
  Coordinate guide_position(Coordinate, unsigned) const;
  std::vector<ReadRegion> support(unsigned, const Region&) const override;
  std::uint64_t task_scratch(unsigned, const Region&) const override;
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override;
};
SchemaTemplate field_schema(const OperationMetadata&, ElementType);
std::uint64_t radius_extent(std::int64_t);
std::vector<double> real_list(const std::string&);
OperationDefinition make_local(const std::string&, LocalKind, Profile);
void run_rank(const LocalFilter&, const Kernel&, const ps_kernel_invocation_v8&,
              const Work&);
void run_bilateral(const LocalFilter&, const Kernel&,
                   const ps_kernel_invocation_v8&, const Work&);
void run_guided(const LocalFilter&, const Kernel&,
                const ps_kernel_invocation_v8&, const Work&);
OperationDefinition make_hysteresis();
OperationDefinition make_nms();
OperationDefinition make_gabor_kernel(const std::string&, Profile);
OperationDefinition make_gabor_response(const std::string&, Profile);
OperationDefinition make_pyramid(const std::string&, int, Profile);
OperationDefinition make_band();
OperationDefinition make_pyramid_assembly();
}  // namespace ps::plugin_internal::filter
