#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

static int execute(void* user, const ps_operation_value_view_v9* inputs,
                   uint32_t count,
                   const ps_operation_parameter_value_v9* parameters,
                   uint32_t parameter_count, uint32_t backend,
                   ps_operation_cancelled_v9 cancelled, void* cancel_context,
                   const ps_operation_output_sink_v9* sink, char* diagnostic,
                   size_t diagnostic_capacity) {
  (void)user;
  (void)inputs;
  (void)count;
  (void)parameters;
  (void)parameter_count;
  (void)backend;
  (void)cancelled;
  (void)cancel_context;
  (void)diagnostic;
  (void)diagnostic_capacity;
  double number = 10.0 + sink->output_index;
  return sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT64_V9,
                       sink->output_shape, sink->output_rank, NULL, 0,
                       (const uint8_t*)&number, sizeof(number))
             ? PS_OPERATION_RESULT_SUCCESS_V9
             : PS_OPERATION_RESULT_FAILURE_V9;
}
static ps_operation_descriptor_v9 descriptor;
static void destroy(const ps_operation_descriptor_v9* records, uint32_t count) {
  (void)records;
  (void)count;
}
static const ps_operation_plugin_api_v9 api = {
    sizeof(ps_operation_plugin_api_v9), 1, &descriptor, destroy};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_9;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v9*
ps_operation_plugin_get_api_v9(void) {
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.struct_size = sizeof(descriptor);
  descriptor.key = "test.c_results";
  descriptor.key_size = 14;
  descriptor.flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
                     PS_OPERATION_FLAG_SIDE_EFFECT_FREE;
  descriptor.cacheable = 1;
  descriptor.execute = execute;
  descriptor.output_count = 2;
  for (uint32_t i = 0; i < 2; ++i) {
    ps_operation_output_descriptor_v9* output = &descriptor.outputs[i];
    output->struct_size = sizeof(*output);
    output->key = i ? "second" : "first";
    output->key_size = i ? 6 : 5;
    output->output_element_type = PS_OPERATION_ELEMENT_FLOAT64_V9;
    output->shape_rule = PS_OPERATION_SHAPE_SCALAR_V9;
    output->region_rule = PS_OPERATION_REGION_WHOLE_V9;
    output->output_schema.struct_size = sizeof(output->output_schema);
    output->output_schema.kind = PS_OPERATION_PORT_VALUE_V9;
  }
  return &api;
}
