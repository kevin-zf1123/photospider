#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

static int execute(void* user, const ps_operation_value_view_v7* inputs,
                   uint32_t count,
                   const ps_operation_parameter_value_v7* parameters,
                   uint32_t parameter_count, uint32_t backend,
                   ps_operation_cancelled_v7 cancelled, void* cancel_context,
                   const ps_operation_output_sink_v7* sink, char* diagnostic,
                   size_t diagnostic_capacity) {
  (void)user;
  (void)inputs;
  (void)parameters;
  (void)parameter_count;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (count != 1 || backend != 1)
    return PS_OPERATION_RESULT_FAILURE_V7;
  if (cancelled(cancel_context))
    return PS_OPERATION_RESULT_CANCELLED_V7;
  uint8_t* bytes = sink->allocate_output(sink->context);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V7;
  memset(bytes, 0, (size_t)sink->output_byte_size);
  return sink->publish(sink->context, sink->output_element_type,
                       sink->output_shape, sink->output_rank,
                       sink->output_facets, sink->output_facet_count, bytes,
                       sink->output_byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V7
             : PS_OPERATION_RESULT_FAILURE_V7;
}
static const ps_operation_parameter_descriptor_v7 parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v7), "expression", 10,
     PS_OPERATION_PARAMETER_STRING_V7, 1, 0, 0, 0},
    {sizeof(ps_operation_parameter_descriptor_v7), "count", 5,
     PS_OPERATION_PARAMETER_INT64_V7, 1, 0, 0, 0},
    {sizeof(ps_operation_parameter_descriptor_v7), "start", 5,
     PS_OPERATION_PARAMETER_FLOAT64_V7, 1, 0, 0, 0},
    {sizeof(ps_operation_parameter_descriptor_v7), "step", 4,
     PS_OPERATION_PARAMETER_FLOAT64_V7, 1, 0, 0, 0}};
static const ps_operation_semantic_constraint_v7 input = {
    sizeof(ps_operation_semantic_constraint_v7),
    0,
    PS_OPERATION_ELEMENT_FLOAT64_V7,
    1,
    0,
    NULL,
    0};
static const ps_operation_port_constraint_v7 ports[] = {
    {sizeof(ps_operation_port_constraint_v7), PS_OPERATION_PORT_VALUE_V7, 0, 0,
     &input}};
static const ps_operation_extent_v7 axes[] = {{sizeof(ps_operation_extent_v7),
                                               PS_OPERATION_EXTENT_PARAMETER_V7,
                                               1, "count", 5, 0, 0, 0}};
static const ps_operation_contract_v7 contract = {
    .struct_size = sizeof(ps_operation_contract_v7),
    .axis_count = 1,
    .axes = axes,
    .semantic_rule = PS_OPERATION_SEMANTIC_SAMPLE_EXPRESSION_V7,
    .semantic_parameter = "expression",
    .semantic_parameter_size = 10};
static const char key[] = "fixture.expression_contract";
static const ps_operation_descriptor_v7 operations[] = {
    {.struct_size = sizeof(ps_operation_descriptor_v7),
     .key = key,
     .key_size = sizeof(key) - 1,
     .input_count = 1,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .output_element_type = PS_OPERATION_ELEMENT_FLOAT32_V7,
     .shape_rule = PS_OPERATION_SHAPE_AXES_V7,
     .region_rule = PS_OPERATION_REGION_WHOLE_V7,
     .cacheable = 1,
     .parameter_count = 4,
     .parameters = parameters,
     .input_schema_count = 1,
     .input_schema = ports,
     .output_schema = {sizeof(ps_operation_port_constraint_v7),
                       PS_OPERATION_PORT_TYPED_V7, 0, 0, NULL},
     .execute = execute,
     .contract = &contract}};
static void destroy(const ps_operation_descriptor_v7* values, uint32_t count) {
  (void)values;
  (void)count;
}
static const ps_operation_plugin_api_v7 api = {
    sizeof(ps_operation_plugin_api_v7), 1, operations, destroy};
uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_7;
}
const ps_operation_plugin_api_v7* ps_operation_plugin_get_api_v7(void) {
  return &api;
}
