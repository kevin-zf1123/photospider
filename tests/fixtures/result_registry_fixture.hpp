#ifndef TESTS_FIXTURES_RESULT_REGISTRY_FIXTURE_HPP_
#define TESTS_FIXTURES_RESULT_REGISTRY_FIXTURE_HPP_

#include <cstdint>

#include "photospider/plugin/result_operation_plugin_api.h"

namespace result_registry_fixture {
inline ps_result_tensor_spec_v2 make_tensor() {
  ps_result_tensor_spec_v2 tensor{};
  tensor.struct_size = sizeof(tensor);
  tensor.key = "value";
  tensor.key_size = 5;
  tensor.element_type = PS_RESULT_ELEMENT_FLOAT64_V2;
  tensor.rank = 1;
  tensor.shape[0] = 1;
  tensor.channel_axis = PS_RESULT_NO_CHANNEL_V2;
  return tensor;
}
inline const auto tensor = make_tensor();
inline ps_result_schema_v2 make_schema() {
  ps_result_schema_v2 schema{};
  schema.struct_size = sizeof(schema);
  schema.id = "fixture.number";
  schema.id_size = 14;
  schema.version = 1;
  schema.publication = PS_RESULT_COMPLETE_BUNDLE_V2;
  schema.tensors = &tensor;
  schema.tensor_count = 1;
  return schema;
}
inline const auto schema = make_schema();
inline ps_result_output_v2 make_output(
    const ps_result_schema_v2* result_schema = &schema) {
  ps_result_output_v2 output{};
  output.struct_size = sizeof(output);
  output.key = "value";
  output.key_size = 5;
  output.port.struct_size = sizeof(output.port);
  output.port.kind = PS_RESULT_OBJECT_V2;
  output.port.schema = result_schema;
  output.execution = PS_RESULT_WHOLE_V2;
  return output;
}
inline const auto output = make_output();
inline int never(void*, void*, const ps_result_query_v2*,
                 const ps_result_services_v2*) {
  return 1;
}
inline void destroy_state(void*, void*) {}
inline ps_result_operation_v2 make_operation(
    const char* key, std::uint32_t key_size,
    const ps_result_parameter_descriptor_v2* parameters,
    std::uint32_t parameter_count,
    const ps_result_output_v2* result_output = &output) {
  ps_result_operation_v2 operation{};
  operation.struct_size = sizeof(operation);
  operation.key = key;
  operation.key_size = key_size;
  operation.flags = PS_RESULT_FLAG_DETERMINISTIC_V2 |
                    PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2 | PS_RESULT_FLAG_CPU_V2;
  operation.outputs = result_output;
  operation.output_count = 1;
  operation.parameters = parameters;
  operation.parameter_count = parameter_count;
  operation.maximum_stages = 1;
  operation.start = never;
  operation.poll = never;
  operation.destroy = destroy_state;
  return operation;
}
inline ps_result_operation_plugin_api_v2 make_api(
    const ps_result_operation_v2* operation, void (*destroy)(void*)) {
  return {sizeof(ps_result_operation_plugin_api_v2),
          PS_RESULT_OPERATION_ABI_VERSION_2,
          operation,
          1,
          nullptr,
          destroy};
}
}  // namespace result_registry_fixture

#endif  // TESTS_FIXTURES_RESULT_REGISTRY_FIXTURE_HPP_
