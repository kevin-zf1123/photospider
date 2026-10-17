#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"

namespace ps::test {
struct ScalarSum {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using multi_result::check;
    using multi_result::take;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      for (unsigned port = 0; port < 2; ++port)
        need.tiles.push_back(
            {port, 0, false,
             take(TileSet::range(
                 phase.resources, 0,
                 phase.query.input_tile_grids[port][0].tile_count()))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double left, right;
    check(phase.read_tensor(0, 0, {0}, &left, sizeof(left)));
    check(phase.read_tensor(1, 0, {0}, &right, sizeof(right)));
    const double sum = left + right;
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&sum), sizeof(sum))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
struct TensorIdentity {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using multi_result::check;
    using multi_result::take;
    const auto& spec = phase.query.output.result_schema->tensors[0];
    auto output = phase.query.tensor_outputs.value_or(
        take(Footprint::all(spec.sample_shape())));
    if (!requested && !output.empty()) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(take(sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, output)));
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    for (const auto& region : output.boxes()) {
      auto window = take(phase.tensors->at({0, 0}).acquire(region));
      ResultTensorViewTransform transform;
      for (unsigned axis = 0; axis < region.rank(); ++axis)
        transform.source_axes.push_back({static_cast<int>(axis), 0, 1});
      auto status = builder.publish_tensor_view(0, region, window, transform);
      if (status.reason == FailureReason::ViewUnavailable) {
        const auto width =
            TensorSpan::element_size(spec.descriptor.element_type);
        auto bytes = take(
            phase.allocator.allocate(take(region.element_count()) * width));
        std::uint64_t index = 0;
        auto samples =
            take(Footprint::from_regions(spec.sample_shape(), {region}));
        check(samples.visit(
            [&](const auto& at) {
              return phase.read_tensor(0, 0, at, bytes.data() + index++ * width,
                                       width);
            },
            take(region.element_count())));
        status = builder.publish_tensor(0, region,
                                        ByteView(bytes.data(), bytes.size()));
      }
      check(status);
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
inline OperationDefinition kernel_identity(const char* key) {
  OperationDefinition operation;
  operation.key = key;
  operation.traits.workspace_bytes = 8192;
  operation.traits.outputs[0] = multi_result::output("value");
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);

  operation.traits.input_schema[0].element_type_mask = 127;
  operation.traits.outputs[0].output_schema.result_schema_id.clear();
  operation.traits.outputs[0].output_schema.result_schema_version = 0;
  operation.traits.outputs[0].output_schema.element_type_mask = 127;
  operation.traits.requires_metadata_specialization = true;
  operation.specialize_metadata = [](const auto& inputs, const auto&) {
    OperationOutputSpecialization output;
    output.metadata.result_schema = inputs[0].result_schema;
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  operation.start_result = [](const ResultProgramQuery&,
                              const BufferAllocator& allocator) {
    return ResultContinuation::make<TensorIdentity>(allocator);
  };
  return operation;
}
inline std::shared_ptr<OperationRegistry> kernel_registry(bool freeze = true) {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition constant;
  constant.key = "core.constant";
  constant.traits.workspace_bytes = 8192;
  constant.traits.outputs[0] = multi_result::output("value");
  constant.traits.parameter_schema = {
      {"value", OperationParameterType::Float64, true}};
  constant.start_result = [](const ResultProgramQuery& query,
                             const BufferAllocator& allocator) {
    return ResultContinuation::make<multi_result::Program>(
        allocator, -1, std::get<double>(query.parameters.at("value")));
  };
  multi_result::check(registry->register_operation(std::move(constant)));
  OperationDefinition sum;
  sum.key = "math.add";
  sum.traits.workspace_bytes = 8192;
  sum.traits.outputs[0] = multi_result::output("value");
  sum.traits.input_count = 2;
  sum.traits.input_schema.resize(2);
  for (auto& port : sum.traits.input_schema) {
    port.result_schema_id = "test.multi_output";
    port.result_schema_version = 1;
  }
  sum.start_result = [](const ResultProgramQuery&,
                        const BufferAllocator& allocator) {
    return ResultContinuation::make<ScalarSum>(allocator);
  };
  multi_result::check(registry->register_operation(std::move(sum)));
  multi_result::check(
      registry->register_operation(kernel_identity("core.identity")));
  if (freeze)
    multi_result::check(registry->freeze());
  return registry;
}
}  // namespace ps::test
