#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/effects.hpp"
#include "05-filter/gather_patch.hpp"

namespace ps::plugin_internal::filter {
Effect::Effect(EffectKind k, Profile prof,
               const std::vector<OperationMetadata>& in, const Parameters& p)
    : kind(k),
      profile(prof),
      type(tensor(in.at(0), true).descriptor.element_type) {
  check(numeric_ops::sequence_profile_available(profile));
  const auto& source = tensor(in[0], true);
  const auto& control = tensor(in.at(1), true);
  require(
      source.sample_shape().size() >= 2 && source.sample_shape().size() <= 8,
      "filter effect input rank 2..8");
  require(floating(type) && floating(control.descriptor.element_type),
          "filter effect requires Float32/64");
  if (kind == EffectKind::Contrast) {
    require(source.sample_shape() == control.sample_shape() &&
                type == control.descriptor.element_type,
            "local contrast matching fields");
    amount = real(p, "amount");
    scale = real(p, "detail_scale");
  } else {
    auto gp = p;
    std::uint64_t radius = 1;
    if (gather()) {
      maximum = real(p, "max_radius");
      // This check bounds the statically declared support and scratch.
      parameter(maximum >= 0 && maximum < 0x1p62,
                "variable gather support extent");
      radius = static_cast<std::uint64_t>(maximum);
      const auto mode = text(p, "broadcast");
      parameter(mode == "scalar" || mode == "spatial", "radius broadcast");
      scalar_radius = mode == "scalar";
      radius_narrow = control.descriptor.element_type == ElementType::Float32;
    } else {
      threshold = real(p, "threshold");
      blend = real(p, "blend");
      gp["boundary"] = std::string("clamp");
      require(control.descriptor.element_type == type,
              "directional guide dtype");
    }
    gp["anchor_y"] = static_cast<std::int64_t>(radius);
    gp["anchor_x"] = static_cast<std::int64_t>(radius);
    gp["cval"] = 0.0;
    geometry.emplace(source, gp, 2 * radius + 1, 2 * radius + 1, Kind::BoxMean);
    parameter(geometry->boundary != Boundary::Constant,
              "effect boundary excludes constant");
    const auto a = integer(p, "component_axis");
    parameter(a >= -1 && (a < 0 || (static_cast<std::uint64_t>(a) <
                                        geometry->dims.size() &&
                                    a != geometry->ya && a != geometry->xa)),
              "effect component axis");
    component_axis = static_cast<int>(a);
    if (scalar_radius) {
      require(control.sample_shape() == std::vector<std::uint64_t>{1},
              "scalar radius requires [1]");
    } else {
      auto shape = source.sample_shape();
      if (a >= 0)
        shape[a] = 1;
      require(control.sample_shape() == shape,
              "effect control spatial/batch shape");
    }
  }
  add_output(this, field_schema(in[0], type));
  if (gather())
    add_output(this, field_schema(in[0], ElementType::UInt8));
}
std::vector<ReadRegion> Effect::support(unsigned port, const Region& q) const {
  if (gather() && port == 1)
    return {};
  if (identity())
    return {{0, 0, q}};
  if (kind == EffectKind::Contrast)
    return {{0, 0, q}, {1, 0, q}};
  std::vector<ReadRegion> reads;
  for (const auto& r : geometry->support(q))
    reads.push_back({0, 0, r});
  const auto add_control = [&](const Region& region) {
    auto d = region.dimensions();
    if (component_axis >= 0)
      d[component_axis] = {0, 1};
    reads.push_back({1, 0, Region(std::move(d))});
  };
  if (scalar_radius)
    reads.push_back({1, 0, Region::whole({1})});
  else if (gather())
    add_control(q);
  else
    for (const auto& r : geometry->support(q))
      add_control(r);
  return reads;
}
std::uint64_t Effect::task_scratch(unsigned port, const Region& q) const {
  if (identity() || (gather() && port == 1))
    return 0;
  return generation::checked_add(
      sizeof(generation::Math) + 128,
      gather_patch(*this) ? gather_patch_bytes(*this, q) : 0);
}
OperationDefinition make_effect(const std::string& key, EffectKind kind,
                                Profile profile) {
  std::vector<Parameter> p;
  if (kind == EffectKind::Contrast) {
    p = {{"amount", Type::Float64}, {"detail_scale", Type::Float64}};
  } else {
    p = {{"y_axis", Type::Int64},
         {"x_axis", Type::Int64},
         {"component_axis", Type::Int64}};
    if (kind == EffectKind::Directional)
      generation::append(
          &p, {{"threshold", Type::Float64}, {"blend", Type::Float64}});
    else
      generation::append(&p, {{"max_radius", Type::Float64},
                              {"broadcast", Type::String},
                              {"boundary", Type::String}});
  }
  std::vector<std::pair<std::string, SchemaTemplate>> outputs{
      {"output", tensor_schema(ElementType::Float64, {1, 1})}};
  if (kind == EffectKind::Box || kind == EffectKind::Disk)
    outputs.emplace_back("valid", tensor_schema(ElementType::UInt8, {1, 1}));
  return generation::operation(key, 2, std::move(p), outputs,
                               [kind, profile](const auto& in, const auto& p) {
                                 return std::make_shared<Effect>(kind, profile,
                                                                 in, p);
                               });
}
}  // namespace ps::plugin_internal::filter
