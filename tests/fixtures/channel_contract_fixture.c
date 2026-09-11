#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

#ifndef PS_CHANNEL_BAD_CASE
#define PS_CHANNEL_BAD_CASE 0
#endif
static int execute(void* user, const ps_operation_value_view_v9* inputs,
                   uint32_t count,
                   const ps_operation_parameter_value_v9* parameters,
                   uint32_t parameter_count, uint32_t backend,
                   ps_operation_cancelled_v9 cancelled, void* cancel_context,
                   const ps_operation_output_sink_v9* sink, char* diagnostic,
                   size_t diagnostic_capacity) {
  (void)user;
  (void)inputs;
  (void)parameters;
  (void)parameter_count;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (count != 1 || backend != 1)
    return PS_OPERATION_RESULT_FAILURE_V9;
  if (cancelled(cancel_context))
    return PS_OPERATION_RESULT_CANCELLED_V9;
  uint8_t* bytes = sink->allocate_output(sink->context);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V9;
  memset(bytes, 0, (size_t)sink->output_byte_size);
  return sink->publish(sink->context, sink->output_element_type,
                       sink->output_shape, sink->output_rank,
                       sink->output_facets, sink->output_facet_count, bytes,
                       sink->output_byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V9
             : PS_OPERATION_RESULT_FAILURE_V9;
}
static const ps_operation_parameter_descriptor_v9 parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v9), "indices", 7,
     PS_OPERATION_PARAMETER_STRING_V9, 1, 0, 0, 0}};
static const ps_operation_semantic_constraint_v9 semantic = {
    sizeof(ps_operation_semantic_constraint_v9),
    2,
    PS_OPERATION_ELEMENT_FLOAT32_V9,
    3,
    0,
    NULL,
    0};
static const ps_operation_port_constraint_v9 ports[] = {
    {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_TYPED_V9, 0, 0,
     &semantic}};
static ps_operation_extent_v9 axes[] = {
    {sizeof(ps_operation_extent_v9), PS_OPERATION_EXTENT_INPUT_AXIS_V9, 1, NULL,
     0, 0, 0, 0, NULL, 0, 1, 1},
    {sizeof(ps_operation_extent_v9), PS_OPERATION_EXTENT_INPUT_AXIS_V9, 1, NULL,
     0, 0, 1, 0, NULL, 0, 1, 1},
    {sizeof(ps_operation_extent_v9), PS_OPERATION_EXTENT_INDEX_LIST_COUNT_V9, 1,
     "indices", 7, 0, 0, 0, NULL, 0, 1, 1}};
static ps_operation_contract_v9 contract = {
    .struct_size = sizeof(ps_operation_contract_v9),
    .dtype_rule = PS_OPERATION_DTYPE_INPUT_V9,
    .axis_count = 3,
    .axes = axes,
    .semantic_rule = PS_OPERATION_SEMANTIC_SWIZZLE_CHANNELS_V9,
    .semantic_parameter = "indices",
    .semantic_parameter_size = 7};
static const char key[] = "fixture.channel_contract";
static const ps_operation_descriptor_v9 operations[] = {
    {sizeof(ps_operation_descriptor_v9),
     key,
     sizeof(key) - 1,
     1,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     1,
     1,
     parameters,
     1,
     ports,
     execute,
     0,
     0,
     0,
     0,
     1,
     {{sizeof(ps_operation_output_descriptor_v9),
       "value",
       5,
       PS_OPERATION_ELEMENT_FLOAT32_V9,
       0,
       0,
       PS_OPERATION_SHAPE_AXES_V9,
       PS_OPERATION_REGION_WHOLE_V9,
       0,
       {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_VALUE_V9, 0,
        0, NULL},
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
static void destroy(const ps_operation_descriptor_v9* descriptors,
                    uint32_t count) {
  (void)descriptors;
  (void)count;
}
static const ps_operation_plugin_api_v9 api = {
    sizeof(ps_operation_plugin_api_v9), 1, operations, destroy};
uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_9;
}
const ps_operation_plugin_api_v9* ps_operation_plugin_get_api_v9(void) {
#if PS_CHANNEL_BAD_CASE == 1
  contract.semantic_rule = UINT32_MAX;
#elif PS_CHANNEL_BAD_CASE == 2
  axes[2].source = UINT32_MAX;
#endif
  return &api;
}
