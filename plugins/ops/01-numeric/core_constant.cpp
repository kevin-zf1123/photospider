#include <cstring>
#include <utility>

#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
Status register_core_constant(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "core.constant";
  operation.traits = tensor_ops::traits(0);
  operation.traits.parameter_schema = {
      {"value", OperationParameterType::Float64, true}};
  operation.lower =
      [](const OperationLowering& query) -> Result<OperationProgram> {
    OperationProgramBuilder builder(query);
    if (!query.requests[0].empty()) {
      auto task = builder.add_task("constant", "ieee-f64-v1");
      if (!task.ok())
        return Result<OperationProgram>(task.status());
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
    if (kernel.symbol != "constant" || kernel.contract != "ieee-f64-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "unknown constant kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable,
           "constant requires CPU Host placement"});
    auto signature = tensor_ops::scalar_signature(query, 0);
    if (!signature.ok())
      return Result<ps_prepared_kernel_v8>(signature);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context = new double(std::get<double>(query.parameters.at("value")));
    result.release = [](const void* context) {
      delete static_cast<const double*>(context);
    };
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      const auto& output = invocation->outputs[0];
      std::memcpy(
          static_cast<std::uint8_t*>(output.allocation) + output.byte_offset,
          invocation->prepared_context, sizeof(double));
      *status = {};
    };
    return Result<ps_prepared_kernel_v8>(result);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
