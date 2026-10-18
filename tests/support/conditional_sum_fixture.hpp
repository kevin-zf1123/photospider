#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"

namespace ps::test {
// A small routing fixture for generation and cache tests. The control vector
// is observed in full; a nonzero fourth control adds data[3] to other outputs.
struct ConditionalSum {
  bool add = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using multi_result::check;
    using multi_result::take;
    const auto shape = std::vector<std::uint64_t>{1, 5};
    auto output =
        phase.query.tensor_outputs.value_or(take(Footprint::all(shape)));
    if (!output.empty()) {
      std::int64_t control;
      check(phase.read_tensor(1, 0, {0, 3}, &control, sizeof(control)));
      add = control != 0;
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    for (const auto& box : output.boxes()) {
      auto bytes = take(
          phase.allocator.allocate(take(box.element_count()) * sizeof(double)));
      for (std::uint64_t i = 0; i < box.dimensions()[1].extent; ++i) {
        check(phase.consume_work(1));
        const auto at = box.dimensions()[1].offset + i;
        double value;
        check(phase.read_tensor(0, 0, {0, at}, &value, sizeof(value)));
        if (add && at != 3) {
          double extra;
          check(phase.read_tensor(0, 0, {0, 3}, &extra, sizeof(extra)));
          value += extra;
        }
        std::memcpy(bytes.data() + i * sizeof(value), &value, sizeof(value));
      }
      check(builder.publish_tensor(
          0, box, StridedLayout{0, {8, 8}, {0, box.dimensions()[1].offset}},
          std::move(bytes).freeze(), phase.query.cancellation));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const std::bad_alloc&) {
    return Result<ResultProgramPoll>(
        Status{ErrorCode::ResourceExhausted, "conditional allocation"});
  } catch (const multi_result::Failure& failure) {
    auto status = failure.status;
    status.message = "conditional sum: " + status.message;
    return Result<ResultProgramPoll>(status);
  }
};
inline std::shared_ptr<OperationRegistry> conditional_sum_registry() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "test.conditional_sum";
  auto& traits = operation.traits;
  traits.input_count = 2;
  traits.workspace_bytes = 5 * sizeof(double);
  traits.input_schema.resize(2);
  traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  auto& output = traits.outputs[0];

  output.result_schema = multi_result::schema(ElementType::Float64, {1, 5});
  output.result_schema->tensors[0].layout.spatial = true;
  output.result_schema->tensors[0].layout.channel_axis.reset();
  output.output_schema.result_schema_id = output.result_schema->id;
  output.output_schema.result_schema_version = output.result_schema->version;
  output.continuation_bytes = sizeof(ConditionalSum);
  output.maximum_dependency_stages = 1;
  operation.map_dependencies =
      [](const ResultStaticQuery& query) -> Result<ResultStaticMapping> {
    using multi_result::take;
    auto baseline = complete_input_dependencies(query);
    if (!baseline.ok())
      return baseline;
    auto mapping = baseline.take_value();
    auto selected = query.tensor_outputs.value_or(take(Footprint::all({1, 5})));
    selected = take(selected.unite(
        take(Footprint::from_regions({1, 5}, {Region({{0, 1}, {3, 1}})}))));
    for (auto& read : mapping.inputs)
      if (read.input == 0 && read.tensor_slot == 0 && !read.descriptor_only) {
        auto mapped = sdk::tile_need_for_samples(query, 0, 0, selected);
        if (!mapped.ok())
          return Result<ResultStaticMapping>(mapped.status());
        read = mapped.take_value();
      }
    return Result<ResultStaticMapping>(std::move(mapping));
  };
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ConditionalSum>(allocator);
  };
  multi_result::check(registry->register_operation(std::move(operation)));
  multi_result::check(registry->freeze());
  return registry;
}
}  // namespace ps::test
