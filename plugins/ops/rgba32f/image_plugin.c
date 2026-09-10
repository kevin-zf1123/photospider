#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "image_gpu.h"  // NOLINT(build/include_subdir)
#include "photospider/plugin/operation_plugin_api.h"

static const ps_operation_port_constraint_v8 ports_gain[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_RGBA_FLOAT32_V8,
     0, 0, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0, 0x41800000U, NULL}};
static const ps_operation_port_constraint_v8 ports_opacity[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_RGBA_FLOAT32_V8,
     0, 0, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0, 0x3f800000U, NULL}};

/* Host validation proves each requested coordinate is inside valid coverage.
 * Match its unsigned distance arithmetic: a broadcast origin may exceed
 * INT64_MAX. */
static float sample(const ps_operation_value_view_v8* value, uint64_t y,
                    uint64_t x, uint64_t c) {
  const uint64_t coordinate[] = {y, x, c};
  uint64_t positive = value->byte_offset, negative = 0;
  for (uint32_t axis = 0; axis < value->rank; ++axis) {
    const uint64_t origin = value->storage_origin[axis],
                   coord = coordinate[axis];
    const int64_t stride = value->byte_strides[axis];
    if (stride == 0 || coord == origin)
      continue;
    const uint64_t distance = coord >= origin ? coord - origin : origin - coord;
    const uint64_t magnitude =
        stride < 0 ? UINT64_C(0) - (uint64_t)stride : (uint64_t)stride;
    const uint64_t span = distance * magnitude;
    if ((coord < origin) != (stride < 0))
      negative += span;
    else
      positive += span;
  }
  float number = 0;
  memcpy(&number, value->data + (positive - negative), 4);
  return number;
}

/* The ABI8 host validates regional views and supplies nearest/gradual
 * arithmetic. All pointers are callback-local; output and scratch are owned by
 * the host. */
static int execute_image(void* state, const ps_operation_value_view_v8* inputs,
                         uint32_t count,
                         const ps_operation_parameter_value_v8* parameters,
                         uint32_t parameter_count, uint32_t backend,
                         ps_operation_cancelled_v8 cancelled,
                         void* cancellation_context,
                         const ps_operation_output_sink_v8* sink,
                         char* diagnostic, size_t diagnostic_capacity) {
  if (backend == 2)
    return ps_execute_gpu_image(state ? 1 : 0, inputs, count, parameters,
                                parameter_count, cancelled,
                                cancellation_context, sink);
  (void)parameters;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (count != 2 || parameter_count != 0 || backend != 1 || !inputs || !sink ||
      inputs[0].rank != 3 || inputs[0].shape[2] != 4 || inputs[1].rank != 1 ||
      inputs[0].demand_offsets[2] != 0 || inputs[0].demand_extents[2] != 4 ||
      inputs[1].demand_offsets[0] != 0 || inputs[1].demand_extents[0] != 1)
    return PS_OPERATION_RESULT_FAILURE_V8;
  uint8_t* bytes = sink->allocate_output(sink->context);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V8;
  const float factor = ps_image_scalar(&inputs[1]);
  size_t target = 0;
  for (uint64_t y = sink->output_offsets[0];
       y < sink->output_offsets[0] + sink->output_extents[0]; ++y) {
    if (cancelled && cancelled(cancellation_context))
      return PS_OPERATION_RESULT_CANCELLED_V8;
    for (uint64_t x = sink->output_offsets[1];
         x < sink->output_offsets[1] + sink->output_extents[1]; ++x) {
      for (uint64_t c = 0; c < 4; ++c) {
        float number = sample(&inputs[0], y, x, c);
        if (state || c < 3)
          number *= factor;
        memcpy(bytes + target, &number, sizeof(number));
        target += sizeof(number);
      }
    }
  }
  const int accepted =
      sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT32_V8,
                    sink->output_shape, sink->output_rank, inputs[0].facets,
                    inputs[0].facet_count, bytes, sink->output_byte_size);
  return accepted ? PS_OPERATION_RESULT_SUCCESS_V8
                  : PS_OPERATION_RESULT_FAILURE_V8;
}

static const ps_operation_port_constraint_v8 ports_image[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_RGBA_FLOAT32_V8,
     0, 0, NULL}};
static const ps_operation_port_constraint_v8 ports_mask[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_RGBA_FLOAT32_V8,
     0, 0, NULL},
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_FLOAT32_MASK_V8,
     0, 0, NULL}};
static const ps_operation_port_constraint_v8 ports_over[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_RGBA_FLOAT32_V8,
     0, 0, NULL},
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_RGBA_FLOAT32_V8,
     0, 0, NULL}};
static const ps_operation_parameter_descriptor_v8 gaussian_parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v8), "radius", 6,
     PS_OPERATION_PARAMETER_INT64_V8, 1, 1, 1, 64},
    {sizeof(ps_operation_parameter_descriptor_v8), "sigma", 5,
     PS_OPERATION_PARAMETER_FLOAT64_V8, 1, 1, 0.1, 64}};
static uint64_t clamp_axis(uint64_t coordinate, int tap, uint64_t length) {
  if (tap < 0)
    return coordinate < (uint64_t)-tap ? 0 : coordinate - (uint64_t)-tap;
  const uint64_t room = length - 1 - coordinate;
  return coordinate + ((uint64_t)tap < room ? (uint64_t)tap : room);
}
static int mask_state, over_state;
static int execute_regional(
    void* state, const ps_operation_value_view_v8* inputs, uint32_t count,
    const ps_operation_parameter_value_v8* parameters, uint32_t parameter_count,
    uint32_t backend, ps_operation_cancelled_v8 cancelled,
    void* cancellation_context, const ps_operation_output_sink_v8* sink,
    char* diagnostic, size_t diagnostic_capacity) {
  if (backend == 2)
    return ps_execute_gpu_image(!state                 ? 2
                                : state == &mask_state ? 3
                                                       : 4,
                                inputs, count, parameters, parameter_count,
                                cancelled, cancellation_context, sink);
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (!inputs || !sink || backend != 1 || count != (state ? 2U : 1U) ||
      parameter_count != (state ? 0U : 2U))
    return PS_OPERATION_RESULT_FAILURE_V8;
  uint8_t* output = sink->allocate_output(sink->context);
  if (!output)
    return PS_OPERATION_RESULT_FAILURE_V8;
  const uint64_t y0 = sink->output_offsets[0], x0 = sink->output_offsets[1];
  const uint64_t height = sink->output_extents[0],
                 width = sink->output_extents[1];
  if (!state) {
    const int radius = (int)parameters[0].int64_value;
    const double sigma = parameters[1].float64_value;
    uint8_t* weights =
        sink->allocate_scratch(sink->context, (uint64_t)(2 * radius + 1) * 8);
    const uint64_t first_row = inputs[0].demand_offsets[0];
    uint8_t* scratch = sink->allocate_scratch(
        sink->context, inputs[0].demand_extents[0] * width * 16);
    if (!weights || !scratch)
      return PS_OPERATION_RESULT_FAILURE_V8;
    double total = 0;
    for (int tap = -radius; tap <= radius; ++tap) {
      const double weight = exp(-(double)(tap * tap) / (2.0 * sigma * sigma));
      total += weight;
      memcpy(weights + (tap + radius) * 8, &weight, 8);
    }
    for (int tap = -radius; tap <= radius; ++tap) {
      double weight = 0;
      memcpy(&weight, weights + (tap + radius) * 8, 8);
      weight /= total;
      memcpy(weights + (tap + radius) * 8, &weight, 8);
    }
    for (uint64_t y = first_row; y < first_row + inputs[0].demand_extents[0];
         ++y) {
      if (cancelled && cancelled(cancellation_context))
        return PS_OPERATION_RESULT_CANCELLED_V8;
      for (uint64_t x = x0; x < x0 + width; ++x)
        for (uint64_t c = 0; c < 4; ++c) {
          double sum = 0;
          for (int tap = -radius; tap <= radius; ++tap) {
            double weight = 0;
            memcpy(&weight, weights + (tap + radius) * 8, 8);
            const double product =
                sample(&inputs[0], y, clamp_axis(x, tap, inputs[0].shape[1]),
                       c) *
                weight;
            sum += product;
          }
          const float rounded = (float)sum;
          memcpy(scratch + (((y - first_row) * width + x - x0) * 4 + c) * 4,
                 &rounded, 4);
        }
    }
    for (uint64_t y = y0; y < y0 + height; ++y) {
      if (cancelled && cancelled(cancellation_context))
        return PS_OPERATION_RESULT_CANCELLED_V8;
      for (uint64_t x = x0; x < x0 + width; ++x)
        for (uint64_t c = 0; c < 4; ++c) {
          double sum = 0;
          for (int tap = -radius; tap <= radius; ++tap) {
            double weight = 0;
            memcpy(&weight, weights + (tap + radius) * 8, 8);
            const uint64_t row = clamp_axis(y, tap, inputs[0].shape[0]);
            float number = 0;
            memcpy(&number,
                   scratch + (((row - first_row) * width + x - x0) * 4 + c) * 4,
                   4);
            const double product = number * weight;
            sum += product;
          }
          const float rounded = (float)sum;
          memcpy(output + (((y - y0) * width + x - x0) * 4 + c) * 4, &rounded,
                 4);
        }
    }
  } else {
    for (uint64_t y = y0; y < y0 + height; ++y) {
      if (cancelled && cancelled(cancellation_context))
        return PS_OPERATION_RESULT_CANCELLED_V8;
      for (uint64_t x = x0; x < x0 + width; ++x) {
        const float factor = state == &mask_state
                                 ? sample(&inputs[1], y, x, 0)
                                 : 1.0F - sample(&inputs[0], y, x, 3);
        for (uint64_t c = 0; c < 4; ++c) {
          const float attenuated =
              sample(&inputs[state == &mask_state ? 0 : 1], y, x, c) * factor;
          const float number = state == &mask_state
                                   ? attenuated
                                   : sample(&inputs[0], y, x, c) + attenuated;
          memcpy(output + (((y - y0) * width + x - x0) * 4 + c) * 4, &number,
                 4);
        }
      }
    }
  }
  return sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT32_V8,
                       sink->output_shape, sink->output_rank, inputs[0].facets,
                       inputs[0].facet_count, output, sink->output_byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V8
             : PS_OPERATION_RESULT_FAILURE_V8;
}

/* S3 callbacks use only host-validated region views and host output buffers. */
static int execute_s3(void* state, const ps_operation_value_view_v8* inputs,
                      uint32_t count,
                      const ps_operation_parameter_value_v8* parameters,
                      uint32_t parameter_count, uint32_t backend,
                      ps_operation_cancelled_v8 cancelled,
                      void* cancellation_context,
                      const ps_operation_output_sink_v8* sink, char* diagnostic,
                      size_t diagnostic_capacity) {
  if (backend == 2)
    return ps_execute_gpu_image(state                    ? 7
                                : sink->output_rank == 3 ? 5
                                                         : 6,
                                inputs, count, parameters, parameter_count,
                                cancelled, cancellation_context, sink);
  (void)diagnostic;
  (void)diagnostic_capacity;
  const int brush = state != NULL;
  if (!inputs || !sink || backend != 1 || count != (brush ? 8U : 1U) ||
      parameter_count != (brush ? 0U : 1U))
    return PS_OPERATION_RESULT_FAILURE_V8;
  uint8_t* output = sink->allocate_output(sink->context);
  if (!output)
    return PS_OPERATION_RESULT_FAILURE_V8;
  const uint64_t channels = sink->output_rank == 3 ? 4 : 1;
  const uint64_t factor = brush ? 1 : (uint64_t)parameters[0].int64_value;
  float args[7] = {0};
  if (brush) {
    for (uint32_t i = 0; i < 7; ++i)
      args[i] = ps_image_scalar(&inputs[i + 1]);
  }
  size_t target = 0;
  for (uint64_t y = sink->output_offsets[0];
       y < sink->output_offsets[0] + sink->output_extents[0]; ++y) {
    if (cancelled && cancelled(cancellation_context))
      return PS_OPERATION_RESULT_CANCELLED_V8;
    for (uint64_t x = sink->output_offsets[1];
         x < sink->output_offsets[1] + sink->output_extents[1]; ++x) {
      const double dx = (double)x + .5 - args[0], dy = (double)y + .5 - args[1];
      const int inside =
          brush && dx * dx + dy * dy <= (double)args[2] * args[2];
      const uint64_t y0 = y * factor, x0 = x * factor;
      const uint64_t h =
          factor < inputs[0].shape[0] - y0 ? factor : inputs[0].shape[0] - y0;
      const uint64_t w =
          factor < inputs[0].shape[1] - x0 ? factor : inputs[0].shape[1] - x0;
      for (uint64_t c = 0; c < channels; ++c) {
        float number;
        if (brush) {
          number = sample(&inputs[0], y, x, c);
          if (inside) {
            const float source = c == 3 ? args[6] : args[3 + c] * args[6];
            const float back = number * (1.0F - args[6]);
            number = source + back;
          }
        } else {
          double sum = 0;
          for (uint64_t row = y0; row < y0 + h; ++row)
            for (uint64_t col = x0; col < x0 + w; ++col)
              sum += sample(&inputs[0], row, col, c);
          number = (float)(sum / (double)(h * w));
        }
        memcpy(output + target, &number, 4);
        target += 4;
      }
    }
  }
  return sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT32_V8,
                       sink->output_shape, sink->output_rank, inputs[0].facets,
                       inputs[0].facet_count, output, sink->output_byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V8
             : PS_OPERATION_RESULT_FAILURE_V8;
}
static const ps_operation_port_constraint_v8 ports_mask_only[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_FLOAT32_MASK_V8,
     0, 0, NULL}};
static const ps_operation_port_constraint_v8 ports_brush[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_RGBA_FLOAT32_V8,
     0, 0, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0xff7fffffU, 0x7f7fffffU, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0xff7fffffU, 0x7f7fffffU, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0x00800000U, 0x7f7fffffU, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0xff7fffffU, 0x7f7fffffU, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0xff7fffffU, 0x7f7fffffU, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0xff7fffffU, 0x7f7fffffU, NULL},
    {sizeof(ps_operation_port_constraint_v8),
     PS_OPERATION_PORT_FLOAT32_SCALAR_V8, 0, 0x3f800000U, NULL}};
static const ps_operation_parameter_descriptor_v8 shrink_parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v8), "factor", 6,
     PS_OPERATION_PARAMETER_INT64_V8, 1, 1, 1, 16}};
static int brush_state;
static int opacity_state;
static const ps_operation_contract_v8 preserve_semantics = {
    .struct_size = sizeof(ps_operation_contract_v8),
    .semantic_rule = PS_OPERATION_SEMANTIC_PRESERVE_V8};
static const ps_operation_descriptor_v8 operations[] = {
    {sizeof(ps_operation_descriptor_v8),
     "image.exposure_gain",
     19,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
         PS_OPERATION_FLAG_CPU_FALLBACK | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V8,
     0,
     NULL,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V8,
     PS_OPERATION_REGION_ELEMENTWISE_V8,
     0,
     1,
     0,
     NULL,
     2,
     ports_gain,
     {sizeof(ps_operation_port_constraint_v8),
      PS_OPERATION_PORT_RGBA_FLOAT32_V8, 0, 0, NULL},
     execute_image,
     NULL,
     0,
     0,
     NULL,
     0,
     NULL,
     0,
     &preserve_semantics,
     PS_OPERATION_OBSERVATION_ATOMIC_V8,
     PS_OPERATION_FAILURE_REQUEST_ONLY_V8},
    {sizeof(ps_operation_descriptor_v8),
     "image.opacity",
     13,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
         PS_OPERATION_FLAG_CPU_FALLBACK | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V8,
     0,
     NULL,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V8,
     PS_OPERATION_REGION_ELEMENTWISE_V8,
     0,
     1,
     0,
     NULL,
     2,
     ports_opacity,
     {sizeof(ps_operation_port_constraint_v8),
      PS_OPERATION_PORT_RGBA_FLOAT32_V8, 0, 0, NULL},
     execute_image,
     &opacity_state,
     0,
     0,
     NULL,
     0,
     NULL,
     0,
     &preserve_semantics,
     PS_OPERATION_OBSERVATION_ATOMIC_V8,
     PS_OPERATION_FAILURE_REQUEST_ONLY_V8},
    {sizeof(ps_operation_descriptor_v8),
     "image.gaussian_blur",
     19,
     1,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
         PS_OPERATION_FLAG_CPU_FALLBACK | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V8,
     0,
     NULL,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V8,
     PS_OPERATION_REGION_HALO_V8,
     0,
     1,
     2,
     gaussian_parameters,
     1,
     ports_image,
     {sizeof(ps_operation_port_constraint_v8),
      PS_OPERATION_PORT_RGBA_FLOAT32_V8, 0, 0, NULL},
     execute_regional,
     NULL,
     1032,
     1,
     "radius",
     6,
     NULL,
     0,
     &preserve_semantics,
     PS_OPERATION_OBSERVATION_ATOMIC_V8,
     PS_OPERATION_FAILURE_REQUEST_ONLY_V8},
    {sizeof(ps_operation_descriptor_v8),
     "image.mask",
     10,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
         PS_OPERATION_FLAG_CPU_FALLBACK | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V8,
     0,
     NULL,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V8,
     PS_OPERATION_REGION_ELEMENTWISE_V8,
     0,
     1,
     0,
     NULL,
     2,
     ports_mask,
     {sizeof(ps_operation_port_constraint_v8),
      PS_OPERATION_PORT_RGBA_FLOAT32_V8, 0, 0, NULL},
     execute_regional,
     &mask_state,
     0,
     0,
     NULL,
     0,
     NULL,
     0,
     &preserve_semantics,
     PS_OPERATION_OBSERVATION_ATOMIC_V8,
     PS_OPERATION_FAILURE_REQUEST_ONLY_V8},
    {sizeof(ps_operation_descriptor_v8),
     "image.source_over",
     17,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
         PS_OPERATION_FLAG_CPU_FALLBACK | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     PS_OPERATION_ELEMENT_FLOAT32_V8,
     0,
     NULL,
     PS_OPERATION_SHAPE_MATCH_INPUTS_V8,
     PS_OPERATION_REGION_ELEMENTWISE_V8,
     0,
     1,
     0,
     NULL,
     2,
     ports_over,
     {sizeof(ps_operation_port_constraint_v8),
      PS_OPERATION_PORT_RGBA_FLOAT32_V8, 0, 0, NULL},
     execute_regional,
     &over_state,
     0,
     0,
     NULL,
     0,
     NULL,
     0,
     &preserve_semantics,
     PS_OPERATION_OBSERVATION_ATOMIC_V8,
     PS_OPERATION_FAILURE_REQUEST_ONLY_V8},
    {.struct_size = sizeof(ps_operation_descriptor_v8),
     .key = "image.downsample_box",
     .key_size = 20,
     .input_count = 1,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
              PS_OPERATION_FLAG_CPU_FALLBACK | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .output_element_type = PS_OPERATION_ELEMENT_FLOAT32_V8,
     .shape_rule = PS_OPERATION_SHAPE_SHRINK_V8,
     .region_rule = PS_OPERATION_REGION_SHRINK_V8,
     .cacheable = 1,
     .parameter_count = 1,
     .parameters = shrink_parameters,
     .input_schema_count = 1,
     .input_schema = ports_image,
     .output_schema = {sizeof(ps_operation_port_constraint_v8),
                       PS_OPERATION_PORT_RGBA_FLOAT32_V8, 0, 0, NULL},
     .execute = execute_s3,
     .user_data = NULL,
     .spatial_factor_parameter = "factor",
     .spatial_factor_parameter_size = 6,
     .contract = &preserve_semantics},
    {.struct_size = sizeof(ps_operation_descriptor_v8),
     .key = "mask.downsample_box",
     .key_size = 19,
     .input_count = 1,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
              PS_OPERATION_FLAG_CPU_FALLBACK | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .output_element_type = PS_OPERATION_ELEMENT_FLOAT32_V8,
     .shape_rule = PS_OPERATION_SHAPE_SHRINK_V8,
     .region_rule = PS_OPERATION_REGION_SHRINK_V8,
     .cacheable = 1,
     .parameter_count = 1,
     .parameters = shrink_parameters,
     .input_schema_count = 1,
     .input_schema = ports_mask_only,
     .output_schema = {sizeof(ps_operation_port_constraint_v8),
                       PS_OPERATION_PORT_FLOAT32_MASK_V8, 0, 0, NULL},
     .execute = execute_s3,
     .user_data = NULL,
     .spatial_factor_parameter = "factor",
     .spatial_factor_parameter_size = 6,
     .contract = &preserve_semantics},
    {.struct_size = sizeof(ps_operation_descriptor_v8),
     .key = "image.brush_circle",
     .key_size = 18,
     .input_count = 8,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
              PS_OPERATION_FLAG_CPU_FALLBACK | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .output_element_type = PS_OPERATION_ELEMENT_FLOAT32_V8,
     .shape_rule = PS_OPERATION_SHAPE_PRESERVE_FIRST_V8,
     .region_rule = PS_OPERATION_REGION_ELEMENTWISE_V8,
     .cacheable = 1,
     .parameter_count = 0,
     .parameters = NULL,
     .input_schema_count = 8,
     .input_schema = ports_brush,
     .output_schema = {sizeof(ps_operation_port_constraint_v8),
                       PS_OPERATION_PORT_RGBA_FLOAT32_V8, 0, 0, NULL},
     .execute = execute_s3,
     .user_data = &brush_state,
     .workspace_input_multiplier = 1,
     .spatial_factor_parameter = NULL,
     .spatial_factor_parameter_size = 0,
     .contract = &preserve_semantics}};
static void destroy(const ps_operation_descriptor_v8* records, uint32_t count) {
  (void)records;
  (void)count;
}
static const ps_operation_plugin_api_v8 api = {
    sizeof(ps_operation_plugin_api_v8),
    sizeof(operations) / sizeof(operations[0]), operations, destroy};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_8;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v8*
ps_operation_plugin_get_api_v8(void) {
  return &api;
}
