#pragma once

#include <string>
#include <vector>

#include "05-filter/advanced.hpp"
#include "05-filter/certified.hpp"

namespace ps::plugin_internal::filter {
enum class EffectKind { Contrast, Box, Disk, Directional };
struct Effect final : Program {
  EffectKind kind;
  Profile profile;
  ElementType type;
  std::optional<Geometry> geometry;
  int component_axis = -1;
  bool scalar_radius = false;
  bool radius_narrow = false;
  double amount = 0, scale = 1, maximum = 0, threshold = 0, blend = 0;
  Effect(EffectKind, Profile, const std::vector<OperationMetadata>&,
         const Parameters&);
  bool gather() const {
    return kind == EffectKind::Box || kind == EffectKind::Disk;
  }
  bool identity() const {
    return kind == EffectKind::Contrast      ? amount == 0
           : kind == EffectKind::Directional ? blend == 0
                                             : maximum == 0;
  }
  Coordinate control_position(Coordinate p) const {
    if (scalar_radius)
      return {};
    if (component_axis >= 0)
      p[component_axis] = 0;
    return p;
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override;
  std::uint64_t task_scratch(unsigned, const Region&) const override;
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override;
};
std::uint64_t contrast_pixel(Atom, Atom, double, double, generation::Math&,
                             const Fuel&);
std::uint64_t remap_pixel(Atom, Atom, double, double, double, generation::Math&,
                          const Fuel&);
OperationDefinition make_effect(const std::string&, EffectKind, Profile);
OperationDefinition make_local_laplacian(const std::string&, Profile);
OperationDefinition make_smaa();
}  // namespace ps::plugin_internal::filter
