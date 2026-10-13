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
  unsigned stage = 0;
  bool add = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using multi_result::check;
    using multi_result::take;
    const auto shape = std::vector<std::uint64_t>{1, 5};
    auto output =
        phase.query.tensor_outputs.value_or(take(Footprint::all(shape)));
    if (!output.empty() && stage == 0) {
      ++stage;
      ResultProgramNeed need;
      need.tiles.push_back({1, true, false, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (!output.empty() && stage == 1) {
      ++stage;
      std::int64_t control;
      check(phase.read_tensor(1, 0, {0, 3}, &control, sizeof(control)));
      add = control != 0;
      auto selected = output;
      if (add)
        selected = take(selected.unite(
            take(Footprint::from_regions(shape, {Region({{0, 1}, {3, 1}})}))));
      ResultProgramNeed need;
      need.tiles.push_back(take(sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, selected)));
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{},
        phase.query.tile_extent));
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
        Status{ErrorCode::ResourceExhausted,
               "conditional allocation stage " + std::to_string(stage)});
  } catch (const multi_result::Failure& failure) {
    auto status = failure.status;
    status.message = "conditional sum stage " + std::to_string(stage) + ": " +
                     status.message;
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
  for (auto& port : traits.input_schema)
    port.kind = OperationPortKind::Result;
  traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  auto& output = traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.result_schema = multi_result::schema(ElementType::Float64, {1, 5});
  output.result_schema->tensors[0].layout.spatial = true;
  output.result_schema->tensors[0].layout.channel_axis.reset();
  output.output_schema.result_schema_id = output.result_schema->id;
  output.output_schema.result_schema_version = output.result_schema->version;
  output.region_rule = OperationRegionRule::Tiled;
  output.continuation_bytes = sizeof(ConditionalSum);
  output.maximum_dependency_stages = 3;
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ConditionalSum>(allocator);
  };
  multi_result::check(registry->register_operation(std::move(operation)));
  multi_result::check(registry->freeze());
  return registry;
}
}  // namespace ps::test
