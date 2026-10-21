#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"

namespace ps::test {
/** @brief Fixed scalar fixtures used by public compiler contract tests. */
inline void install_scalar(OperationDefinition& operation, bool sum) {
  operation.lower = [sum](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (!query.requests[0].empty()) {
      auto task = builder.add_task(sum ? "sum" : "constant", "scalar-v1");
      if (!task.ok())
        return Result<OperationProgram>(task.status());
      for (unsigned port = 0; port < (sum ? 2U : 0U); ++port) {
        auto status = builder.read_input(task.value(), port, 0, 0);
        if (!status.ok())
          return Result<OperationProgram>(status);
      }
      auto status = builder.write_output(task.value(), 0, 0, 0);
      if (!status.ok())
        return Result<OperationProgram>(status);
    }
    return builder.finish();
  };
  operation.prepare_kernel =
      [sum](const physical::KernelDesc& kernel,
            const physical::Placement& placement,
            const KernelPreparation& query, const ResourceSet&,
            const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    const auto scalar = [](const KernelView& view) {
      return view.domain == physical::Domain::Host && view.bytes == 8 &&
             view.member == 0 && view.tile_index == 0 &&
             view.layout.element_type == ElementType::Float64 &&
             view.layout.rank == 1 && view.layout.shape[0] == 1 &&
             view.layout.strides[0] == 8 && view.layout.byte_offset == 0;
    };
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host ||
        kernel.contract != "scalar-v1" ||
        kernel.symbol != (sum ? "sum" : "constant") ||
        query.writes.size() != 1 || !scalar(query.writes[0]) ||
        query.reads.size() != (sum ? 2U : 0U) ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{})
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::InvalidArgument, "scalar fixture signature"});
    for (const auto& input : query.reads)
      if (!scalar(input))
        return Result<ps_prepared_kernel_v8>(
            {ErrorCode::InvalidArgument, "scalar fixture input"});
    struct State {
      bool sum;
      double literal;
    };
    auto state = std::make_unique<State>();
    state->sum = sum;
    state->literal = sum ? 0 : std::get<double>(query.parameters.at("value"));
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 8;
    result.context = state.release();
    result.release = [](const void* context) {
      delete static_cast<const State*>(context);
    };
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      double value = state.literal;
      if (state.sum) {
        double left, right;
        std::memcpy(&left, invocation->inputs[0].allocation, sizeof(left));
        std::memcpy(&right, invocation->inputs[1].allocation, sizeof(right));
        value = left + right;
      }
      std::memcpy(invocation->outputs[0].allocation, &value, sizeof(value));
      *status = {};
    };
    return Result<ps_prepared_kernel_v8>(result);
  };
}
inline OperationDefinition kernel_identity(const char* key) {
  OperationDefinition operation;
  operation.key = key;
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
  operation.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto task = builder.add_copy();
        if (!task.ok())
          return Result<OperationProgram>(task.status());
        auto status = builder.read_input(task.value(), 0, member.first, tile);
        if (!status.ok())
          return Result<OperationProgram>(status);
        status = builder.write_output(task.value(), 0, member.first, tile);
        if (!status.ok())
          return Result<OperationProgram>(status);
      }
    return builder.finish();
  };
  return operation;
}
inline std::shared_ptr<OperationRegistry> kernel_registry(bool freeze = true) {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition constant;
  constant.key = "core.constant";
  constant.traits.outputs[0] = multi_result::output("value");
  constant.traits.parameter_schema = {
      {"value", OperationParameterType::Float64, true}};
  install_scalar(constant, false);
  multi_result::check(registry->register_operation(std::move(constant)));
  OperationDefinition sum;
  sum.key = "math.add";
  sum.traits.outputs[0] = multi_result::output("value");
  sum.traits.input_count = 2;
  sum.traits.input_schema.resize(2);
  for (auto& port : sum.traits.input_schema) {
    port.result_schema_id = "test.multi_output";
    port.result_schema_version = 1;
  }
  install_scalar(sum, true);
  multi_result::check(registry->register_operation(std::move(sum)));
  multi_result::check(
      registry->register_operation(kernel_identity("core.identity")));
  if (freeze)
    multi_result::check(registry->freeze());
  return registry;
}
}  // namespace ps::test
