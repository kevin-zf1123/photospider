#include <utility>
#include <vector>
#pragma once

#include "photospider/plugin/operation_registry.hpp"

namespace ps::input_internal {
// A preserve flag alone is not evidence of a port-0 identity Data map.
// Check the complete prepared domain, even for a single-channel request. The
// prepared seal already establishes disjoint/complete piece coverage; this
// additional proof is allocation-free and bounded by the declared map size.
inline Result<bool> planar_exact_identity_view(
    const OperationTraits& traits, const std::vector<OperationMetadata>& inputs,
    const ValueDescriptor& output, const FootprintLimits& limits = {}) {
  using Answer = Result<bool>;
  if (inputs.empty() || !inputs[0].planar_layout ||
      inputs[0].descriptor.shape != output.shape ||
      inputs[0].descriptor.element_type != output.element_type ||
      traits.outputs.size() != 1 ||
      !traits.outputs[0].static_dependency_pieces) {
    return Answer(false);
  }
  const auto& pieces = *traits.outputs[0].static_dependency_pieces;
  if (pieces.empty()) {
    return Answer(false);
  }
  for (const auto& piece : pieces) {
    if (limits.cancellation.cancelled()) {
      return Answer(
          Status{ErrorCode::Cancelled, "identity view proof cancelled"});
    }
    bool data = false;
    for (const auto& need : piece.inputs) {
      if (limits.consume_work) {
        auto charged = limits.consume_work(1 + need.axes.size());
        if (!charged.ok()) {
          return Answer(charged);
        }
      }
      if (!(need.roles & static_cast<std::uint32_t>(DependencyRole::Data))) {
        continue;
      }
      if (need.port != 0 || need.axes.size() != output.shape.size()) {
        return Answer(false);
      }
      data = true;
      for (std::size_t axis = 0; axis < need.axes.size(); ++axis) {
        const auto& map = need.axes[axis];
        if (map.observation_axis >= 0) {
          if (map.observation_axis != static_cast<std::int32_t>(axis) ||
              map.translation != 0) {
            return Answer(false);
          }
        } else {
          if (map.fixed.extent != 1) {
            return Answer(false);
          }
          for (const auto& box : piece.coverage.boxes()) {
            if (limits.consume_work) {
              auto charged = limits.consume_work(1);
              if (!charged.ok()) {
                return Answer(charged);
              }
            }
            const auto& d = box.dimensions()[axis];
            if (d.extent != 1 || d.offset != map.fixed.offset) {
              return Answer(false);
            }
          }
        }
      }
    }
    if (!data) {
      return Answer(false);
    }
  }
  return Answer(true);
}
// One shared authorization calculation for executor and registry. In
// particular, descriptor-only tags never become sample reads, and shared
// weights are fetched once after canonical union. No rectangular bounding-box
// approximation.
inline Result<std::vector<Footprint>> planar_exact_requirements(
    const OperationTraits& traits, const std::vector<OperationMetadata>& inputs,
    const ValueDescriptor& output, const Region& region,
    const FootprintLimits& limits = {}) {
  using Answer = Result<std::vector<Footprint>>;
  if (!traits.planar_exact_dependencies || traits.outputs.size() != 1 ||
      !traits.outputs[0].static_dependency_pieces) {
    return Answer(
        Status{ErrorCode::InvalidArgument, "missing exact planar contract"});
  }
  auto coverage = Footprint::from_regions(output.shape, {region}, limits);
  if (!coverage.ok()) {
    return Answer(coverage.status());
  }
  std::vector<std::vector<std::uint64_t>> shapes;
  for (const auto& input : inputs) {
    shapes.push_back(input.descriptor.shape);
  }
  std::vector<DependencyMapPiece> pieces;
  for (const auto& piece : *traits.outputs[0].static_dependency_pieces) {
    auto hit = piece.coverage.intersect(coverage.value(), limits);
    if (!hit.ok()) {
      return Answer(hit.status());
    }
    if (!hit.value().empty()) {
      pieces.push_back({hit.take_value(), piece.inputs});
    }
  }
  auto certificate = DependencyCertificate::create_mapped(
      "planar-exact-v1", coverage.value(), std::move(shapes), std::move(pieces),
      limits);
  if (!certificate.ok()) {
    return Answer(certificate.status());
  }
  auto needs = certificate.value().backward(coverage.value(), limits);
  if (!needs.ok()) {
    return Answer(needs.status());
  }
  std::vector<Footprint> result;
  for (const auto& input : inputs) {
    auto empty = Footprint::none(input.descriptor.shape, limits);
    if (!empty.ok()) {
      return Answer(empty.status());
    }
    result.push_back(empty.take_value());
  }
  for (const auto& need : needs.value()) {
    auto united = result[need.port].unite(need.samples, limits);
    if (!united.ok()) {
      return Answer(united.status());
    }
    result[need.port] = united.take_value();
  }
  return Answer(std::move(result));
}
}  // namespace ps::input_internal
