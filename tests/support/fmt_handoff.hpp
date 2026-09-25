#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace ps::handoff_testing {
inline void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
inline void take(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
struct Probe final {
  std::atomic<unsigned> preparations{0}, callbacks{0};
  std::function<void()> on_callback;
  std::function<void()> after_copy;
};
inline OperationDefinition probe_operation(
    const std::shared_ptr<Probe>& probe, bool movement = false,
    DataMovementViewPolicy policy = DataMovementViewPolicy::Auto,
    std::function<void(OperationOutputSpecialization&)> alter = {}) {
  OperationDefinition op;
  op.key = "review.transfer_probe";  // Deliberately no builtin prefix.
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.planar_storage_capable = true;
  op.traits.requires_metadata_specialization = true;
  op.traits.cacheable = false;
  op.traits.parameter_schema = {{"tag", OperationParameterType::Int64}};
  auto& out = op.traits.outputs[0];
  out.key = "values";
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 1;
  out.continuation_bytes = 64;
  out.maximum_dependency_stages = 2;
  op.prepare_static = [probe, movement, policy, alter](const auto& inputs,
                                                       const auto& params) {
    ++probe->preparations;
    OperationOutputSpecialization out;
    out.metadata = inputs[0];
    out.regional_atomic = true;
    DependencyMappedNeed map;
    for (std::size_t a = 0; a < inputs[0].descriptor.shape.size(); ++a) {
      DependencyAxis axis;
      axis.observation_axis = static_cast<std::int32_t>(a);
      map.axes.push_back(axis);
    }
    out.static_dependency_pieces = std::vector<DependencyMapPiece>{
        {take(Footprint::all(inputs[0].descriptor.shape)), {map}}};
    if (movement) {
      out.data_movement = DataMovementKind::BitwiseMapped;
      out.data_movement_view_policy = policy;
      out.preserve_output_views = policy != DataMovementViewPolicy::Materialize;
    }
    if (alter)
      alter(out);
    OperationPreparation result;
    result.outputs.push_back(std::move(out));
    result.state = std::make_shared<std::int64_t>(
        std::get<std::int64_t>(params.at("tag")));
    return Result<OperationPreparation>(std::move(result));
  };
  op.start_dependency = [](const DependencyQuery&, const BufferAllocator&) {
    return Result<DependencyContinuation>(
        Status{ErrorCode::Internal, "probe only implements planar execution"});
  };
  op.planar_callback = [probe](const PlanarOperationInvocation& call) {
    ++probe->callbacks;
    require(call.prepared && call.prepared->state(),
            "missing borrowed preparation");
    require(*static_cast<const std::int64_t*>(call.prepared->state()) ==
                std::get<std::int64_t>(call.parameters.at("tag")),
            "wrong prepared state");
    if (probe->on_callback)
      probe->on_callback();
    const auto& map = call.prepared->traits()
                          .outputs[0]
                          .static_dependency_pieces->front()
                          .inputs[0];
    auto copied = copy_planar_region(
        call.output_region, map, &call.inputs[0], nullptr, call.output,
        *call.output_metadata.planar_layout,
        Value::element_size(call.output_metadata.descriptor.element_type),
        call.cancellation);
    if (copied.ok() && probe->after_copy)
      probe->after_copy();
    return copied;
  };
  return op;
}
inline WorkflowDocument probe_document(const PlanarImage& image) {
  WorkflowDocument document;
  const auto& c = image.config();
  document.inputs = {
      {1,
       "image",
       image.descriptor(),
       Region::whole(image.descriptor().shape),
       {},
       image.facets(),
       PlanarImageLayout{c.order, c.height_axis, c.width_axis, c.channel_axis,
                         c.row_pitch_bytes, c.groups}}};
  document.nodes = {{1,
                     "review.transfer_probe",
                     {WorkflowInputReference{1}},
                     {{"tag", std::int64_t{7}}}}};
  document.outputs = {{"result", 1, "values"}};
  return document;
}
inline PlanarImage probe_image() {
  const ValueDescriptor d{ElementType::UInt8, {3, 5, 2}};
  auto image = take(PlanarImage::create(d, {}));
  std::vector<std::uint8_t> bytes(30);
  for (unsigned i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>(i * 7 + 1);
  take(image.publish(Region::whole(d.shape), bytes.data(), bytes.size()));
  return image;
}
inline ExecutionBindings probe_bindings(const PlanarImage& image) {
  ExecutionBindings result;
  result.inputs.push_back(
      {"image", {}, {}, {}, std::make_shared<const PlanarImage>(image)});
  return result;
}
}  // namespace ps::handoff_testing
