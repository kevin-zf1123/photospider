#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

static const ps_operation_port_constraint_v3 ports_gain[] = {
    {sizeof(ps_operation_port_constraint_v3),
     PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V3, 0, 0},
    {sizeof(ps_operation_port_constraint_v3),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V3, 0, 0x41800000U}};
static const ps_operation_port_constraint_v3 ports_opacity[] = {
    {sizeof(ps_operation_port_constraint_v3),
     PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V3, 0, 0},
    {sizeof(ps_operation_port_constraint_v3),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V3, 0, 0x3f800000U}};

/* The ABI3 host validates dense image/scalar ports before entry and supplies
 * nearest/ties-even rounding with gradual underflow. Input and facet pointers
 * are borrowed only until return; publish copies bytes before we free them.
 * Spatial demand is advisory in S1: every callback returns the whole image. */
static int execute_image(void* state, const ps_operation_value_view_v3* inputs,
                         uint32_t count,
                         const ps_operation_parameter_value_v3* parameters,
                         uint32_t parameter_count, uint32_t backend,
                         ps_operation_cancelled_v3 cancelled,
                         void* cancellation_context,
                         const ps_operation_output_sink_v3* sink,
                         char* diagnostic, size_t diagnostic_capacity) {
  (void)parameters;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (count != 2 || parameter_count != 0 || backend != 1 || !inputs || !sink ||
      inputs[0].rank != 3 || inputs[0].shape[2] != 4 || inputs[1].rank != 1 ||
      inputs[1].byte_size != 4 || inputs[0].demand_offsets[2] != 0 ||
      inputs[0].demand_extents[2] != 4 || inputs[1].demand_offsets[0] != 0 ||
      inputs[1].demand_extents[0] != 1)
    return PS_OPERATION_RESULT_FAILURE_V3;
  uint8_t* bytes = (uint8_t*)malloc((size_t)inputs[0].byte_size);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V3;
  memcpy(bytes, inputs[0].data, (size_t)inputs[0].byte_size);
  float factor = 0;
  memcpy(&factor, inputs[1].data, sizeof(factor));
  for (size_t offset = 0; offset < inputs[0].byte_size; offset += 16) {
    if (cancelled && cancelled(cancellation_context)) {
      free(bytes);
      return PS_OPERATION_RESULT_CANCELLED_V3;
    }
    for (size_t channel = 0; channel < (state ? 4U : 3U); ++channel) {
      float number = 0;
      memcpy(&number, bytes + offset + channel * 4, sizeof(number));
      number *= factor;
      memcpy(bytes + offset + channel * 4, &number, sizeof(number));
    }
  }
  const int accepted =
      sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT32_V3,
                    inputs[0].shape, inputs[0].rank, inputs[0].facets,
                    inputs[0].facet_count, bytes, inputs[0].byte_size);
  free(bytes);
  return accepted ? PS_OPERATION_RESULT_SUCCESS_V3
                  : PS_OPERATION_RESULT_FAILURE_V3;
}

static int opacity_state;
static const ps_operation_descriptor_v3 operations[] = {
    {sizeof(ps_operation_descriptor_v3),
     "image.exposure_gain",
     19,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V3,
     0,
     NULL,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V3,
     PS_OPERATION_REGION_ELEMENTWISE_V3,
     0,
     1,
     0,
     NULL,
     2,
     ports_gain,
     {sizeof(ps_operation_port_constraint_v3),
      PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V3, 0, 0},
     execute_image,
     NULL},
    {sizeof(ps_operation_descriptor_v3),
     "image.opacity",
     13,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V3,
     0,
     NULL,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V3,
     PS_OPERATION_REGION_ELEMENTWISE_V3,
     0,
     1,
     0,
     NULL,
     2,
     ports_opacity,
     {sizeof(ps_operation_port_constraint_v3),
      PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V3, 0, 0},
     execute_image,
     &opacity_state}};
static void destroy(const ps_operation_descriptor_v3* records, uint32_t count) {
  (void)records;
  (void)count;
}
static const ps_operation_plugin_api_v3 api = {
    sizeof(ps_operation_plugin_api_v3), 2, operations, destroy};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_3;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v3*
ps_operation_plugin_get_api_v3(void) {
  return &api;
}
