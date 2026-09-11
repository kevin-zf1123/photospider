#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

static const ps_operation_port_constraint_v9 ports_gain[] = {
    {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_RGBA_FLOAT32_V9,
     0, 0, NULL},
    {sizeof(ps_operation_port_constraint_v9),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V9, 0, 0x41800000U, NULL}};
static const ps_operation_port_constraint_v9 ports_opacity[] = {
    {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_RGBA_FLOAT32_V9,
     0, 0, NULL},
    {sizeof(ps_operation_port_constraint_v9),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V9, 0, 0x3f800000U, NULL}};

static int execute_image(void* state, const ps_operation_value_view_v9* inputs,
                         uint32_t count,
                         const ps_operation_parameter_value_v9* parameters,
                         uint32_t parameter_count, uint32_t backend,
                         ps_operation_cancelled_v9 cancelled,
                         void* cancellation_context,
                         const ps_operation_output_sink_v9* sink,
                         char* diagnostic, size_t diagnostic_capacity) {
  (void)parameters;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (count != 2 || parameter_count != 0 || backend != 1 || !inputs || !sink ||
      inputs[0].rank != 3 || inputs[0].shape[2] != 4 || inputs[1].rank != 1 ||
      inputs[1].byte_size != 4 || inputs[0].demand_offsets[2] != 0 ||
      inputs[0].demand_extents[2] != 4 || inputs[1].demand_offsets[0] != 0 ||
      inputs[1].demand_extents[0] != 1)
    return PS_OPERATION_RESULT_FAILURE_V9;
  uint8_t* bytes = (uint8_t*)malloc((size_t)inputs[0].byte_size);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V9;
  memcpy(bytes, inputs[0].data, (size_t)inputs[0].byte_size);
  float factor = 0;
  memcpy(&factor, inputs[1].data, sizeof(factor));
  if (factor == 16) {
    /* Exercise host sink overflow without allocating a large payload. */
    const uint64_t shape[] = {2, UINT64_MAX, 4};
    const int accepted = sink->publish(
        sink->context, PS_OPERATION_ELEMENT_FLOAT32_V9, shape, 3,
        inputs[0].facets, inputs[0].facet_count, bytes, inputs[0].byte_size);
    free(bytes);
    return accepted ? PS_OPERATION_RESULT_SUCCESS_V9
                    : PS_OPERATION_RESULT_FAILURE_V9;
  }
  for (size_t offset = 0; offset < inputs[0].byte_size; offset += 16) {
    if (cancelled && cancelled(cancellation_context)) {
      free(bytes);
      return PS_OPERATION_RESULT_CANCELLED_V9;
    }
    for (size_t channel = 0; channel < (state ? 4U : 3U); ++channel) {
      float number = 0;
      memcpy(&number, bytes + offset + channel * 4, sizeof(number));
      number *= factor;
      memcpy(bytes + offset + channel * 4, &number, sizeof(number));
    }
  }
  const int accepted =
      sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT32_V9,
                    inputs[0].shape, inputs[0].rank, inputs[0].facets,
                    inputs[0].facet_count, bytes, inputs[0].byte_size - 4);
  free(bytes);
  (void)accepted;
  return PS_OPERATION_RESULT_SUCCESS_V9;
}

static const ps_operation_contract_v9 preserve_contract = {
    .struct_size = sizeof(ps_operation_contract_v9),
    .semantic_rule = PS_OPERATION_SEMANTIC_PRESERVE_V9};
static int opacity_state;
static const ps_operation_descriptor_v9 operations[] = {
    {sizeof(ps_operation_descriptor_v9),
     "image.exposure_gain",
     19,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     1,
     0,
     NULL,
     2,
     ports_gain,
     execute_image,
     NULL,
     0,
     0,
     NULL,
     1,
     {{sizeof(ps_operation_output_descriptor_v9),
       "value",
       5,
       PS_OPERATION_ELEMENT_FLOAT32_V9,
       0,
       NULL,
       PS_OPERATION_SHAPE_PRESERVE_FIRST_V9,
       PS_OPERATION_REGION_ELEMENTWISE_V9,
       0,
       {sizeof(ps_operation_port_constraint_v9),
        PS_OPERATION_PORT_RGBA_FLOAT32_V9, 0, 0, NULL},
       NULL,
       0,
       NULL,
       0,
       &preserve_contract,
       PS_OPERATION_OBSERVATION_ATOMIC_V9,
       PS_OPERATION_FAILURE_REQUEST_ONLY_V9,
       0,
       0,
       NULL}}},
    {sizeof(ps_operation_descriptor_v9),
     "image.opacity",
     13,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     1,
     0,
     NULL,
     2,
     ports_opacity,
     execute_image,
     &opacity_state,
     0,
     0,
     NULL,
     1,
     {{sizeof(ps_operation_output_descriptor_v9),
       "value",
       5,
       PS_OPERATION_ELEMENT_FLOAT32_V9,
       0,
       NULL,
       PS_OPERATION_SHAPE_PRESERVE_FIRST_V9,
       PS_OPERATION_REGION_ELEMENTWISE_V9,
       0,
       {sizeof(ps_operation_port_constraint_v9),
        PS_OPERATION_PORT_RGBA_FLOAT32_V9, 0, 0, NULL},
       NULL,
       0,
       NULL,
       0,
       &preserve_contract,
       PS_OPERATION_OBSERVATION_ATOMIC_V9,
       PS_OPERATION_FAILURE_REQUEST_ONLY_V9,
       0,
       0,
       NULL}}}};
static void destroy(const ps_operation_descriptor_v9* records, uint32_t count) {
  (void)records;
  (void)count;
}
static const ps_operation_plugin_api_v9 api = {
    sizeof(ps_operation_plugin_api_v9), 2, operations, destroy};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_9;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v9*
ps_operation_plugin_get_api_v9(void) {
  return &api;
}
