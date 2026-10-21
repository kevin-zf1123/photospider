#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/artifact.hpp"
#include "photospider/compiler/compiler.hpp"
#include "photospider/plugin/fixed_invocation.hpp"
#include "photospider/plugin/operation_program.hpp"
#include "support/status_check.hpp"

namespace ps::test::fixed_fixture {
inline std::atomic<unsigned> lowerings{0}, preparations{0};
inline ps::SchemaTemplate schema() {
  ps::SchemaTemplate result;
  result.id = "physical.compiler.test";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ps::ElementType::UInt8, {4}};
  result.tensors.push_back(std::move(tensor));
  return result;
}
inline ps::OperationDefinition operation(const std::string& name,
                                         unsigned inputs, bool effect = false,
                                         unsigned outputs = 1) {
  ps::OperationDefinition result;
  result.key = name;
  result.traits.input_count = inputs;
  result.traits.side_effect_free = !effect;
  ps::OperationPortConstraint port;
  port.result_schema_id = "physical.compiler.test";
  port.result_schema_version = 1;
  result.traits.input_schema.assign(inputs, port);
  result.traits.outputs.resize(outputs);
  for (unsigned i = 0; i < outputs; ++i) {
    result.traits.outputs[i].key = "out" + std::to_string(i);
    result.traits.outputs[i].output_schema = port;
    result.traits.outputs[i].result_schema = schema();
  }
  result.lower = [effect](const ps::OperationLowering& query)
      -> ps::Result<ps::OperationProgram> {
    ++lowerings;
    ps::OperationProgramBuilder builder(query);
    bool selected = effect;
    for (const auto& request : query.requests)
      selected |= !request.empty();
    if (selected) {
      auto task = builder.add_task("increment", "increment-v1", {}, {}, effect);
      if (!task.ok())
        return ps::Result<ps::OperationProgram>(task.status());
      for (unsigned i = 0; i < query.inputs.size(); ++i) {
        auto status = builder.read_input(task.value(), i, 0, 0);
        if (!status.ok())
          return ps::Result<ps::OperationProgram>(status);
      }
      for (unsigned i = 0; i < query.outputs.size(); ++i) {
        auto status = builder.write_output(task.value(), i, 0, 0);
        if (!status.ok())
          return ps::Result<ps::OperationProgram>(status);
      }
    }
    return builder.finish();
  };
  result.prepare_kernel =
      [](const ps::physical::KernelDesc& kernel, const ps::physical::Placement&,
         const ps::KernelPreparation&, const ps::ResourceSet&,
         const ps_native_gpu_service_v2*) -> ps::Result<ps_prepared_kernel_v8> {
    ++preparations;
    if (kernel.symbol != "increment" || kernel.contract != "increment-v1")
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::TypeMismatch, "unknown kernel contract"});
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 8;
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      for (unsigned oi = 0; oi < invocation->output_count; ++oi) {
        const auto& output = invocation->outputs[oi];
        auto* destination =
            static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
        for (std::uint64_t x = 0; x < output.layout->shape[0]; ++x) {
          std::uint8_t value = 1;
          for (unsigned ii = 0; ii < invocation->input_count; ++ii) {
            const auto& input = invocation->inputs[ii];
            value += static_cast<const std::uint8_t*>(
                input.allocation)[input.byte_offset + x];
          }
          destination[x] = value;
        }
      }
      *status = {};
    };
    return ps::Result<ps_prepared_kernel_v8>(result);
  };
  return result;
}
}  // namespace ps::test::fixed_fixture
