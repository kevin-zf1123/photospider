#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

#ifndef PS_CHANNEL_BAD_CASE
#define PS_CHANNEL_BAD_CASE 0
#endif
static int execute(void* user, const ps_operation_value_view_v8* inputs,
                   uint32_t count,
                   const ps_operation_parameter_value_v8* parameters,
                   uint32_t parameter_count, uint32_t backend,
                   ps_operation_cancelled_v8 cancelled, void* cancel_context,
                   const ps_operation_output_sink_v8* sink, char* diagnostic,
                   size_t diagnostic_capacity) {
  (void)user;
  (void)inputs;
  (void)parameters;
  (void)parameter_count;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (count != 1 || backend != 1)
    return PS_OPERATION_RESULT_FAILURE_V8;
  if (cancelled(cancel_context))
    return PS_OPERATION_RESULT_CANCELLED_V8;
  uint8_t* bytes = sink->allocate_output(sink->context);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V8;
  memset(bytes, 0, (size_t)sink->output_byte_size);
  return sink->publish(sink->context, sink->output_element_type,
                       sink->output_shape, sink->output_rank,
                       sink->output_facets, sink->output_facet_count, bytes,
                       sink->output_byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V8
             : PS_OPERATION_RESULT_FAILURE_V8;
}
static const ps_operation_parameter_descriptor_v8 parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v8), "indices", 7,
     PS_OPERATION_PARAMETER_STRING_V8, 1, 0, 0, 0}};
static const ps_operation_semantic_constraint_v8 semantic = {
    sizeof(ps_operation_semantic_constraint_v8),
    2,
    PS_OPERATION_ELEMENT_FLOAT32_V8,
    3,
    0,
    NULL,
    0};
static const ps_operation_port_constraint_v8 ports[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_TYPED_V8, 0, 0,
     &semantic}};
static ps_operation_extent_v8 axes[] = {
    {sizeof(ps_operation_extent_v8), PS_OPERATION_EXTENT_INPUT_AXIS_V8, 1, NULL,
     0, 0, 0, 0},
    {sizeof(ps_operation_extent_v8), PS_OPERATION_EXTENT_INPUT_AXIS_V8, 1, NULL,
     0, 0, 1, 0},
    {sizeof(ps_operation_extent_v8), PS_OPERATION_EXTENT_INDEX_LIST_COUNT_V8, 1,
     "indices", 7, 0, 0, 0}};
static ps_operation_contract_v8 contract = {
    .struct_size = sizeof(ps_operation_contract_v8),
    .dtype_rule = PS_OPERATION_DTYPE_INPUT_V8,
    .axis_count = 3,
    .axes = axes,
    .semantic_rule = PS_OPERATION_SEMANTIC_SWIZZLE_CHANNELS_V8,
    .semantic_parameter = "indices",
    .semantic_parameter_size = 7};
static const char key[] = "fixture.channel_contract";
static const ps_operation_descriptor_v8 operations[] = {
    {.struct_size = sizeof(ps_operation_descriptor_v8),
     .key = key,
     .key_size = sizeof(key) - 1,
     .input_count = 1,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .output_element_type = PS_OPERATION_ELEMENT_FLOAT32_V8,
     .shape_rule = PS_OPERATION_SHAPE_AXES_V8,
     .region_rule = PS_OPERATION_REGION_WHOLE_V8,
     .cacheable = 1,
     .parameter_count = 1,
     .parameters = parameters,
     .input_schema_count = 1,
     .input_schema = ports,
     .output_schema = {sizeof(ps_operation_port_constraint_v8),
                       PS_OPERATION_PORT_VALUE_V8, 0, 0, NULL},
     .execute = execute,
     .contract = &contract}};
static void destroy(const ps_operation_descriptor_v8* descriptors,
                    uint32_t count) {
  (void)descriptors;
  (void)count;
}
static const ps_operation_plugin_api_v8 api = {
    sizeof(ps_operation_plugin_api_v8), 1, operations, destroy};
uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_8;
}
const ps_operation_plugin_api_v8* ps_operation_plugin_get_api_v8(void) {
#if PS_CHANNEL_BAD_CASE == 1
  contract.semantic_rule = UINT32_MAX;
#elif PS_CHANNEL_BAD_CASE == 2
  axes[2].source = UINT32_MAX;
#endif
  return &api;
}
