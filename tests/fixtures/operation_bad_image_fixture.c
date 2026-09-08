#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

static const ps_operation_port_constraint_v4 ports_gain[] = {
    {sizeof(ps_operation_port_constraint_v4),
     PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V4, 0, 0},
    {sizeof(ps_operation_port_constraint_v4),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V4, 0, 0x41800000U}};
static const ps_operation_port_constraint_v4 ports_opacity[] = {
    {sizeof(ps_operation_port_constraint_v4),
     PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V4, 0, 0},
    {sizeof(ps_operation_port_constraint_v4),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V4, 0, 0x3f800000U}};

static int execute_image(void* state, const ps_operation_value_view_v4* inputs,
                         uint32_t count,
                         const ps_operation_parameter_value_v4* parameters,
                         uint32_t parameter_count, uint32_t backend,
                         ps_operation_cancelled_v4 cancelled,
                         void* cancellation_context,
                         const ps_operation_output_sink_v4* sink,
                         char* diagnostic, size_t diagnostic_capacity) {
  (void)parameters;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (count != 2 || parameter_count != 0 || backend != 1 || !inputs || !sink ||
      inputs[0].rank != 3 || inputs[0].shape[2] != 4 || inputs[1].rank != 1 ||
      inputs[1].byte_size != 4 || inputs[0].demand_offsets[2] != 0 ||
      inputs[0].demand_extents[2] != 4 || inputs[1].demand_offsets[0] != 0 ||
      inputs[1].demand_extents[0] != 1)
    return PS_OPERATION_RESULT_FAILURE_V4;
  uint8_t* bytes = (uint8_t*)malloc((size_t)inputs[0].byte_size);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V4;
  memcpy(bytes, inputs[0].data, (size_t)inputs[0].byte_size);
  float factor = 0;
  memcpy(&factor, inputs[1].data, sizeof(factor));
  if (factor == 16) {
    /* Exercise host sink overflow without allocating a large payload. */
    const uint64_t shape[] = {2, UINT64_MAX, 4};
    const int accepted = sink->publish(
        sink->context, PS_OPERATION_ELEMENT_FLOAT32_V4, shape, 3,
        inputs[0].facets, inputs[0].facet_count, bytes, inputs[0].byte_size);
    free(bytes);
    return accepted ? PS_OPERATION_RESULT_SUCCESS_V4
                    : PS_OPERATION_RESULT_FAILURE_V4;
  }
  for (size_t offset = 0; offset < inputs[0].byte_size; offset += 16) {
    if (cancelled && cancelled(cancellation_context)) {
      free(bytes);
      return PS_OPERATION_RESULT_CANCELLED_V4;
    }
    for (size_t channel = 0; channel < (state ? 4U : 3U); ++channel) {
      float number = 0;
      memcpy(&number, bytes + offset + channel * 4, sizeof(number));
      number *= factor;
      memcpy(bytes + offset + channel * 4, &number, sizeof(number));
    }
  }
  const int accepted =
      sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT32_V4,
                    inputs[0].shape, inputs[0].rank, inputs[0].facets,
                    inputs[0].facet_count, bytes, inputs[0].byte_size - 4);
  free(bytes);
  (void)accepted;
  return PS_OPERATION_RESULT_SUCCESS_V4;
}

static int opacity_state;
static const ps_operation_descriptor_v4 operations[] = {
    {sizeof(ps_operation_descriptor_v4),
     "image.exposure_gain",
     19,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V4,
     0,
     NULL,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V4,
     PS_OPERATION_REGION_ELEMENTWISE_V4,
     0,
     1,
     0,
     NULL,
     2,
     ports_gain,
     {sizeof(ps_operation_port_constraint_v4),
      PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V4, 0, 0},
     execute_image,
     NULL,
     0,
     0},
    {sizeof(ps_operation_descriptor_v4),
     "image.opacity",
     13,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V4,
     0,
     NULL,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V4,
     PS_OPERATION_REGION_ELEMENTWISE_V4,
     0,
     1,
     0,
     NULL,
     2,
     ports_opacity,
     {sizeof(ps_operation_port_constraint_v4),
      PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V4, 0, 0},
     execute_image,
     &opacity_state,
     0,
     0}};
static void destroy(const ps_operation_descriptor_v4* records, uint32_t count) {
  (void)records;
  (void)count;
}
static const ps_operation_plugin_api_v4 api = {
    sizeof(ps_operation_plugin_api_v4), 2, operations, destroy};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_4;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v4*
ps_operation_plugin_get_api_v4(void) {
  return &api;
}
