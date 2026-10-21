#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
Status register_math_add(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "math.add";
  operation.traits = tensor_ops::traits(2);
  for (auto& port : operation.traits.input_schema) {
    port.result_schema_id.clear();
    port.result_schema_version = 0;
    port.tensor_key.clear();
    port.element_type = static_cast<std::uint32_t>(ElementType::Float64);
    port.rank = 1;
  }
  operation.traits.requires_metadata_specialization = true;
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    for (const auto& input : inputs) {
      auto valid = tensor_ops::check_tensor(input);
      if (!valid.ok())
        return Result<std::vector<OperationOutputSpecialization>>(valid);
      if (input.result_schema->tensors[0].sample_shape() !=
          std::vector<std::uint64_t>{1})
        return Result<std::vector<OperationOutputSpecialization>>(
            Status{ErrorCode::TypeMismatch,
                   "math.add requires Float64 scalar tensors"});
    }
    OperationOutputSpecialization output;
    output.metadata.result_schema =
        std::make_shared<SchemaTemplate>(tensor_ops::scalar_schema());
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  operation.lower =
      [](const OperationLowering& query) -> Result<OperationProgram> {
    OperationProgramBuilder builder(query);
    if (!query.requests[0].empty()) {
      auto task = builder.add_task("add", "ieee-f64-v1");
      if (!task.ok())
        return Result<OperationProgram>(task.status());
      for (unsigned port = 0; port < 2; ++port) {
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
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "add" || kernel.contract != "ieee-f64-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "unknown scalar addition kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable,
           "scalar addition requires CPU Host placement"});
    auto signature = tensor_ops::scalar_signature(query, 2);
    if (!signature.ok())
      return Result<ps_prepared_kernel_v8>(signature);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      double numbers[2];
      for (unsigned port = 0; port < 2; ++port) {
        const auto& input = invocation->inputs[port];
        std::memcpy(&numbers[port],
                    static_cast<const std::uint8_t*>(input.allocation) +
                        input.byte_offset,
                    sizeof(double));
      }
      const double sum = numbers[0] + numbers[1];
      const auto& output = invocation->outputs[0];
      std::memcpy(
          static_cast<std::uint8_t*>(output.allocation) + output.byte_offset,
          &sum, sizeof(sum));
      *status = {};
    };
    return Result<ps_prepared_kernel_v8>(result);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
