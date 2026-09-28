#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

static int execute(void* user, const ps_operation_value_view_v11* inputs,
                   uint32_t count,
                   const ps_operation_parameter_value_v11* parameters,
                   uint32_t parameter_count, uint32_t backend,
                   ps_operation_cancelled_v11 cancelled, void* cancel_context,
                   const ps_operation_output_sink_v11* sink, char* diagnostic,
                   size_t diagnostic_capacity) {
  (void)user;
  (void)inputs;
  (void)parameters;
  (void)parameter_count;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (count != 1 || backend != 1)
    return PS_OPERATION_RESULT_FAILURE_V11;
  if (cancelled(cancel_context))
    return PS_OPERATION_RESULT_CANCELLED_V11;
  uint8_t* bytes = sink->allocate_output(sink->context);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V11;
  memset(bytes, 0, (size_t)sink->output_byte_size);
  return sink->publish(sink->context, sink->output_element_type,
                       sink->output_shape, sink->output_rank,
                       sink->output_facets, sink->output_facet_count, bytes,
                       sink->output_byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V11
             : PS_OPERATION_RESULT_FAILURE_V11;
}
static const ps_operation_parameter_descriptor_v11 parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v11), "expression", 10,
     PS_OPERATION_PARAMETER_STRING_V11, 1, 0, 0, 0},
    {sizeof(ps_operation_parameter_descriptor_v11), "count", 5,
     PS_OPERATION_PARAMETER_INT64_V11, 1, 0, 0, 0},
    {sizeof(ps_operation_parameter_descriptor_v11), "start", 5,
     PS_OPERATION_PARAMETER_FLOAT64_V11, 1, 0, 0, 0},
    {sizeof(ps_operation_parameter_descriptor_v11), "step", 4,
     PS_OPERATION_PARAMETER_FLOAT64_V11, 1, 0, 0, 0}};
static const ps_operation_semantic_constraint_v11 input = {
    sizeof(ps_operation_semantic_constraint_v11),
    0,
    PS_OPERATION_ELEMENT_FLOAT64_V11,
    1,
    0,
    NULL,
    0};
static const ps_operation_port_constraint_v11 ports[] = {
    {sizeof(ps_operation_port_constraint_v11), PS_OPERATION_PORT_VALUE_V11, 0,
     0, &input}};
static const ps_operation_extent_v11 axes[] = {
    {sizeof(ps_operation_extent_v11), PS_OPERATION_EXTENT_PARAMETER_V11, 1,
     "count", 5, 0, 0, 0, NULL, 0, 1, 1}};
static const ps_operation_contract_v11 contract = {
    .struct_size = sizeof(ps_operation_contract_v11),
    .axis_count = 1,
    .axes = axes,
    .semantic_rule = PS_OPERATION_SEMANTIC_SAMPLE_EXPRESSION_V11,
    .semantic_parameter = "expression",
    .semantic_parameter_size = 10};
static const char key[] = "fixture.expression_contract";
static const ps_operation_descriptor_v11 operations[] = {
    {sizeof(ps_operation_descriptor_v11),
     key,
     sizeof(key) - 1,
     1,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     1,
     4,
     parameters,
     1,
     ports,
     execute,
     0,
     0,
     0,
     0,
     1,
     {{sizeof(ps_operation_output_descriptor_v11),
       "value",
       5,
       PS_OPERATION_ELEMENT_FLOAT32_V11,
       0,
       0,
       PS_OPERATION_SHAPE_AXES_V11,
       PS_OPERATION_REGION_WHOLE_V11,
       0,
       {sizeof(ps_operation_port_constraint_v11), PS_OPERATION_PORT_TYPED_V11,
        0, 0, NULL},
       0,
       0,
       0,
       0,
       &contract,
       0,
       0,
       0,
       0,
       NULL}}}};
static void destroy(const ps_operation_descriptor_v11* values, uint32_t count) {
  (void)values;
  (void)count;
}
static const ps_operation_plugin_api_v11 api = {
    sizeof(ps_operation_plugin_api_v11), 1, operations, destroy};
uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_11;
}
const ps_operation_plugin_api_v11* ps_operation_plugin_get_api_v11(void) {
  return &api;
}
