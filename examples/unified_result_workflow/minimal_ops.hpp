#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace unified_example {
using Poll = ps::Result<ps::ResultProgramPoll>;
inline ps::SchemaTemplate image_schema() {
  ps::SchemaTemplate schema;
  schema.id = "example.image";
  ps::ResultTensorSpec slot;
  slot.batch_axes = {1, 1};
  slot.layout.spatial = true;
  slot.key = "pixels";
  slot.batch_axes[0] = slot.batch_axes[1] = 2;
  slot.descriptor = {ps::ElementType::Float32, {2, 4}};
  slot.layout.channel_axis = {};
  schema.tensors.push_back(std::move(slot));
  return schema;
}
inline ps::SchemaTemplate numeric_schema(bool count = false) {
  ps::SchemaTemplate schema;
  schema.id = count ? "example.count" : "example.control";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ps::ElementType::Int64,
                       count ? std::vector<std::uint64_t>{1}
                             : std::vector<std::uint64_t>{2, 2, 2, 4}};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
inline ps::ResultRef input_control(const ps::ResourceBudget& root,
                                   const std::vector<std::int64_t>& values) {
  if (values.size() != 32)
    throw std::runtime_error("control requires 32 selectors");
  auto builder =
      ps::ResultBuilder::start(root, numeric_schema(), "example.control.source")
          .take_value();
  auto status = builder.bind_descriptor_relation(
      ps::ResultRelation::cartesian(root, 1, {}).take_value());
  if (!status.ok())
    throw std::runtime_error(status.message);
  status = builder.publish_tensor(
      0, ps::Region::whole({2, 2, 2, 4}),
      ps::ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                   values.size() * sizeof(std::int64_t)),
      ps::ResultRelation::cartesian(root, 32, {}).take_value(),
      {true, true, true, true});
  if (!status.ok())
    throw std::runtime_error(status.message);
  return builder.seal().take_value();
}
inline ps::OperationTraits traits(std::uint32_t inputs, std::uint64_t state) {
  ps::OperationTraits traits;
  traits.input_count = inputs;
  traits.input_schema.resize(inputs);
  auto& output = traits.outputs[0];
  output.dependency_version = 2;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.continuation_bytes = state;
  output.maximum_dependency_stages = 128;
  traits.workspace_bytes = 65536;
  return traits;
}
inline void result_port(ps::OperationPortConstraint* port,
                        const ps::SchemaTemplate& schema) {
  port->kind = ps::OperationPortKind::Result;
  port->result_schema_id = std::string(schema.id);
  port->result_schema_version = schema.version;
}
inline void result_output(ps::OperationOutputTraits* output,
                          const ps::SchemaTemplate& schema) {
  result_port(&output->output_schema, schema);
  output->result_schema = schema;
}
inline std::uint64_t flat(const std::vector<std::uint64_t>& at,
                          const std::vector<std::uint64_t>& shape) {
  std::uint64_t index = 0;
  for (std::size_t axis = 0; axis < at.size(); ++axis)
    index = index * shape[axis] + at[axis];
  return index;
}
inline ps::ResultRef input_image(const ps::ResourceBudget& budget,
                                 float bias = 0) {
  auto schema = image_schema();
  auto builder =
      ps::ResultBuilder::start(budget, schema, "example.source").take_value();
  auto descriptor =
      ps::ResultRelation::cartesian(budget, 1, {0, 8, 0, 0}).take_value();
  if (!builder.bind_descriptor_relation(std::move(descriptor)).ok())
    throw std::runtime_error("source descriptor");
  std::vector<float> pixels;
  for (std::uint64_t n = 0; n < 2; ++n)
    for (std::uint64_t l = 0; l < 2; ++l)
      for (std::uint64_t y = 0; y < 2; ++y)
        for (std::uint64_t x = 0; x < 4; ++x)
          pixels.push_back(bias + 1000 * n + 100 * l + 10 * y + x);
  auto relation =
      ps::ResultRelation::cartesian(budget, 32, {0, 1, 0, 0}).take_value();
  auto status = builder.publish_tensor(
      0, ps::Region::whole({2, 2, 2, 4}),
      ps::ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                   pixels.size() * 4),
      std::move(relation), {true, true, true, true});
  if (!status.ok())
    throw std::runtime_error(std::string(status.message));
  return builder.seal().take_value();
}
struct Gather final {
  unsigned stage = 0;
  ps::Footprint output;
  ps::ResourceVector<std::int64_t> controls;
  ps::ResourceVector<std::array<std::uint64_t, 4>> coordinates;
  Poll poll(const ps::ResultProgramPhase& phase) {
    auto scratch =
        phase.resources.reserve(ps::ResourceCapacity::host(16384, 16384));
    if (!scratch.ok())
      return Poll(scratch.status());
    const auto make_builder = [&] {
      return ps::ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {},
          phase.association
              ? std::vector<std::uint64_t>(phase.association->begin(),
                                           phase.association->end())
              : std::vector<std::uint64_t>{},
          phase.query.tile_height, phase.query.tile_width,
          phase.query.resources);
    };
    if (phase.query.output_index == 1 ||
        (phase.query.tensor_outputs && phase.query.tensor_outputs->empty())) {
      auto made = make_builder();
      if (!made.ok())
        return Poll(made.status());
      auto builder = made.take_value();
      auto relation = ps::ResultRelation::cartesian(phase.resources, 1, {});
      if (!relation.ok())
        return Poll(relation.status());
      auto status = builder.bind_descriptor_relation(relation.value());
      if (!status.ok())
        return Poll(status);
      if (!phase.query.tensor_outputs || !phase.query.tensor_outputs->empty()) {
        const std::int64_t count = 32;
        status = builder.publish_tensor(
            0, ps::Region::whole({1}),
            ps::ByteView(reinterpret_cast<const std::uint8_t*>(&count),
                         sizeof(count)),
            relation.take_value(), {true, true, true, true});
        if (!status.ok())
          return Poll(status);
      }
      auto sealed = builder.seal();
      return sealed.ok()
                 ? Poll(ps::ResultPublication{sealed.take_value(), true})
                 : Poll(sealed.status());
    }
    if (stage == 0) {
      output =
          phase.query.tensor_outputs
              ? *phase.query.tensor_outputs
              : ps::Footprint::all(
                    phase.query.output.result_schema->tensors[0].sample_shape())
                    .take_value();
      stage = 1;
      ps::ResultProgramNeed need;
      need.tensors.push_back({2, 0, output, 6});
      return Poll(std::move(need));
    }
    if (stage == 1) {
      ps::ResultProgramNeed need;
      controls = ps::ResourceVector<std::int64_t>(
          ps::ResourceAllocator<std::int64_t>(phase.resources));
      coordinates = ps::ResourceVector<std::array<std::uint64_t, 4>>(
          ps::ResourceAllocator<std::array<std::uint64_t, 4>>(phase.resources));
      std::vector<ps::Region> branches[2];
      auto status = output.visit(
          [&](const auto& at) -> ps::Status {
            std::int64_t control = 0;
            auto read = phase.read_tensor(2, 0, at, &control, 8);
            if (!read.ok())
              return read;
            if (control < 0 || control > 7)
              return {ps::ErrorCode::InvalidArgument, "selector outside 0..7"};
            controls.push_back(control);
            coordinates.push_back({at[0], at[1], at[2], at[3]});
            auto source = at;
            source[3] = (at[3] + static_cast<std::uint64_t>(control >> 1)) % 4;
            std::vector<ps::RegionDimension> dims;
            for (auto n : source)
              dims.push_back({n, 1});
            branches[control & 1].emplace_back(std::move(dims));
            return phase.consume_work(1);
          },
          32, phase.query.cancellation);
      if (!status.ok())
        return Poll(status);
      for (std::uint32_t branch = 0; branch < 2; ++branch)
        if (!branches[branch].empty()) {
          auto samples =
              ps::Footprint::from_regions(output.shape(), branches[branch]);
          if (!samples.ok())
            return Poll(samples.status());
          need.tensors.push_back({branch, 0, samples.take_value(), 1});
        }
      stage = 2;
      return Poll(std::move(need));
    }
    auto made = make_builder();
    if (!made.ok())
      return Poll(made.status());
    auto builder = made.take_value();
    auto descriptor = ps::ResultRelation::cartesian(phase.resources, 1, {});
    if (!descriptor.ok())
      return Poll(descriptor.status());
    auto status = builder.bind_descriptor_relation(descriptor.take_value());
    if (!status.ok())
      return Poll(status);
    auto rows = ps::ResultRelation::sample_rows(
        phase.resources, 32, coordinates.size() * 2,
        [&](std::uint64_t row) -> ps::Result<ps::ResultRelationRow> {
          const auto i = row / 2;
          auto source = std::vector<std::uint64_t>(coordinates[i].begin(),
                                                   coordinates[i].end());
          source[3] =
              (source[3] + static_cast<std::uint64_t>(controls[i] >> 1)) % 4;
          ps::ResultSupport support =
              row % 2 ? ps::ResultSupport{2,
                                          6,
                                          flat(std::vector<std::uint64_t>(
                                                   coordinates[i].begin(),
                                                   coordinates[i].end()),
                                               output.shape()),
                                          1,
                                          ps::ResultSupportTarget::Tensor,
                                          0}
                      : ps::ResultSupport{
                            static_cast<std::uint32_t>(controls[i] & 1),
                            1,
                            flat(source, output.shape()),
                            1,
                            ps::ResultSupportTarget::Tensor,
                            0};
          return ps::Result<ps::ResultRelationRow>(
              {flat(std::vector<std::uint64_t>(coordinates[i].begin(),
                                               coordinates[i].end()),
                    output.shape()),
               support});
        });
    if (!rows.ok())
      return Poll(rows.status());
    const auto relation = rows.take_value();
    for (std::size_t i = 0; i < coordinates.size(); ++i) {
      auto at = std::vector<std::uint64_t>(coordinates[i].begin(),
                                           coordinates[i].end());
      auto source = at;
      source[3] =
          (source[3] + static_cast<std::uint64_t>(controls[i] >> 1)) % 4;
      float pixel = 0;
      status = phase.read_tensor(controls[i] & 1, 0, source, &pixel, 4);
      if (!status.ok())
        return Poll(status);
      std::vector<ps::RegionDimension> dims;
      for (auto n : at)
        dims.push_back({n, 1});
      status = builder.publish_tensor(
          0, ps::Region(std::move(dims)),
          ps::ByteView(reinterpret_cast<const std::uint8_t*>(&pixel), 4),
          relation, {true, true, true, true});
      if (!status.ok())
        return Poll(status);
    }
    auto result = builder.seal();
    if (!result.ok())
      return Poll(result.status());
    return Poll(ps::ResultPublication{result.take_value(), true});
  }
};
inline ps::Status register_gather(ps::OperationRegistry* registry) {
  ps::OperationDefinition definition;
  definition.key = "example.gather";
  definition.traits = traits(3, sizeof(Gather));
  auto schema = image_schema();
  result_port(&definition.traits.input_schema[0], schema);
  result_port(&definition.traits.input_schema[1], schema);
  result_port(&definition.traits.input_schema[2], numeric_schema());
  result_output(&definition.traits.outputs[0], schema);
  definition.traits.outputs[0].key = "image";
  auto count = definition.traits.outputs[0];
  count.key = "count";
  result_output(&count, numeric_schema(true));
  count.output_schema.element_type =
      static_cast<std::uint32_t>(ps::ElementType::Int64);
  count.input_indices = std::vector<std::uint32_t>{};
  definition.traits.outputs.push_back(std::move(count));
  definition.traits.requires_metadata_specialization = true;
  definition.specialize_metadata = [](const auto& inputs, const auto&) {
    using Answer = ps::Result<std::vector<ps::OperationOutputSpecialization>>;
    for (std::size_t i = 0; i < 3; ++i)
      if (!inputs[i].result_schema ||
          !inputs[i].result_schema->same_schema(i == 2 ? numeric_schema()
                                                       : image_schema()))
        return Answer(
            ps::Status{ps::ErrorCode::TypeMismatch, "gather input schema"});
    std::vector<ps::OperationOutputSpecialization> outputs(2);
    outputs[0].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(image_schema());
    outputs[1].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(numeric_schema(true));
    return Answer(std::move(outputs));
  };
  definition.start_result = [](const ps::ResultProgramQuery&,
                               const ps::BufferAllocator& allocator) {
    return ps::ResultContinuation::make<Gather>(allocator);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace unified_example
