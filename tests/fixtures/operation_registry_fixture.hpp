#pragma once

#include <cstdint>

#include "photospider/plugin/operation_module_api.h"

namespace operation_registry_fixture {
inline ps_tensor_spec_v8 make_tensor() {
  ps_tensor_spec_v8 tensor{};
  tensor.struct_size = sizeof(tensor);
  tensor.key = {"value", 5};
  tensor.element_type = 3;
  tensor.rank = 1;
  tensor.shape[0] = 1;
  tensor.channel_axis = UINT32_MAX;
  return tensor;
}
inline const auto tensor = make_tensor();
inline ps_schema_v8 make_schema() {
  ps_schema_v8 schema{};
  schema.struct_size = sizeof(schema);
  schema.id = {"fixture.number", 14};
  schema.version = 1;
  schema.tensors = &tensor;
  schema.tensor_count = 1;
  return schema;
}
inline const auto schema = make_schema();
inline ps_operation_output_v8 make_output(
    const ps_schema_v8* result_schema = &schema) {
  ps_operation_output_v8 output{};
  output.struct_size = sizeof(output);
  output.key = {"value", 5};
  output.contract.struct_size = sizeof(output.contract);
  output.contract.schema_id = result_schema->id;
  output.contract.schema_version = result_schema->version;
  output.prototype = result_schema;
  return output;
}
inline const auto output = make_output();
/** @brief Import-only fixture; any unexpected lowering fails explicitly. */
inline ps_kernel_status_v8 never(void*, const ps_lowering_query_v8*,
                                 const ps_operation_builder_v8*) {
  ps_kernel_status_v8 status{};
  status.code = PS_KERNEL_OPERATION_FAILED_V8;
  return status;
}
inline ps_operation_v8 make_operation(
    const char* key, std::uint32_t key_size,
    const ps_parameter_spec_v8* parameters, std::uint32_t parameter_count,
    const ps_operation_output_v8* result_output = &output) {
  ps_operation_v8 operation{};
  operation.struct_size = sizeof(operation);
  operation.key = {key, key_size};
  operation.flags = PS_OPERATION_DETERMINISTIC_V8 | PS_OPERATION_PURE_V8 |
                    PS_OPERATION_CPU_V8;
  operation.outputs = result_output;
  operation.output_count = 1;
  operation.parameters = parameters;
  operation.parameter_count = parameter_count;
  operation.lower = never;
  return operation;
}
inline ps_operation_module_v8 make_api(const ps_operation_v8* operation,
                                       void (*destroy)(void*)) {
  return {sizeof(ps_operation_module_v8),
          PS_OPERATION_MODULE_ABI_VERSION_8,
          operation,
          1,
          nullptr,
          destroy};
}
}  // namespace operation_registry_fixture
