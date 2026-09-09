#ifndef PLUGINS_OPS_RGBA32F_IMAGE_GPU_H_
#define PLUGINS_OPS_RGBA32F_IMAGE_GPU_H_

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "image_shader.h" /* NOLINT(build/include_subdir) */
#include "photospider/plugin/operation_plugin_api.h"

// C11 code is intentionally also includable from the C++ built-in adapter.
// NOLINTBEGIN(readability/casting)

/* Must match ImageParameters in the shader, including integer alignment. */
typedef struct ps_image_gpu_parameters {
  uint64_t geometry[32];
  float values[8];
  uint32_t kind, radius, factor, pass;
} ps_image_gpu_parameters;

/* Host validation has established positive native view strides and coverage. */
static float ps_gpu_image_sample(const ps_operation_value_view_v7* v,
                                 uint64_t y, uint64_t x, uint64_t c) {
  const uint64_t offset =
      v->byte_offset +
      (y - v->storage_origin[0]) * (uint64_t)v->byte_strides[0] +
      (x - v->storage_origin[1]) * (uint64_t)v->byte_strides[1] +
      (v->rank == 3 ? c * (uint64_t)v->byte_strides[2] : 0);
  float value;
  memcpy(&value, v->data + offset, 4);
  return value;
}
/* Conservative native domain avoids overflowing reductions and underflowing
 * ordinary arithmetic. Rejected legal inputs retain the exact CPU path. */
static int ps_gpu_number_supported(float number, float minimum) {
  return isfinite(number) && fabsf(number) <= FLT_MAX / 1024.0F &&
         (number == 0 || fabsf(number) >= minimum);
}
static void ps_gpu_image_geometry(ps_image_gpu_parameters* p, uint32_t base,
                                  const ps_operation_value_view_v7* v) {
  p->geometry[base] = v->byte_offset;
  p->geometry[base + 1] = v->storage_origin[0];
  p->geometry[base + 2] = v->storage_origin[1];
  p->geometry[base + 3] = (uint64_t)v->byte_strides[0];
  p->geometry[base + 4] = (uint64_t)v->byte_strides[1];
  p->geometry[base + 5] = v->rank == 3 ? (uint64_t)v->byte_strides[2] : 0;
}

/* Shared C/C++ operation implementation. Host owns every buffer; this helper
 * performs no publication until successful native completion and pixel checks.
 */
static int ps_execute_gpu_image(
    uint32_t kind, const ps_operation_value_view_v7* inputs, uint32_t count,
    const ps_operation_parameter_value_v7* parameters, uint32_t parameter_count,
    ps_operation_cancelled_v7 cancelled, void* cancellation_context,
    const ps_operation_output_sink_v7* sink) {
  if (!sink || !sink->gpu || !inputs || count == 0 || kind > 7)
    return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
  const ps_gpu_service_v7* api = sink->gpu;
  ps_image_gpu_parameters p = {{0}, {0}, 0, 0, 1, 0};
  p.kind = kind;
  p.geometry[0] = sink->output_offsets[0];
  p.geometry[1] = sink->output_offsets[1];
  p.geometry[2] = sink->output_extents[0];
  p.geometry[3] = sink->output_extents[1];
  p.geometry[4] = sink->output_rank == 3 ? 4 : 1;
  p.geometry[5] = inputs[0].shape[0];
  p.geometry[6] = inputs[0].shape[1];
  p.geometry[7] = inputs[0].demand_offsets[0];
  p.geometry[8] = inputs[0].demand_extents[0];
  for (uint32_t i = 0; i < count; ++i) {
    const ps_operation_value_view_v7* v = &inputs[i];
    if (v->rank == 1)
      continue;
    if (v->byte_offset % 4 || v->shape[0] > UINT32_MAX ||
        v->shape[1] > UINT32_MAX)
      return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
    for (uint32_t axis = 0; axis < v->rank; ++axis)
      if (v->byte_strides[axis] < 0 || v->byte_strides[axis] % 4 ||
          v->storage_origin[axis] > v->demand_offsets[axis])
        return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
    for (uint64_t y = v->demand_offsets[0];
         y < v->demand_offsets[0] + v->demand_extents[0]; ++y) {
      if (cancelled && cancelled(cancellation_context))
        return PS_OPERATION_RESULT_CANCELLED_V7;
      for (uint64_t x = v->demand_offsets[1];
           x < v->demand_offsets[1] + v->demand_extents[1]; ++x)
        for (uint64_t c = 0; c < (v->rank == 3 ? 4U : 1U); ++c)
          if (!ps_gpu_number_supported(ps_gpu_image_sample(v, y, x, c),
                                       kind == 3 && i == 1 ? 1e-8F : 1e-20F))
            return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
    }
  }
  ps_gpu_image_geometry(&p, 9, &inputs[0]);
  if (kind == 3 || kind == 4)
    ps_gpu_image_geometry(&p, 15, &inputs[1]);
  if (kind == 0 || kind == 1) {
    memcpy(&p.values[0], inputs[1].data + inputs[1].byte_offset, 4);
    if (!ps_gpu_number_supported(p.values[0], 1e-8F))
      return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
  }
  if (kind == 7) {
    for (uint32_t i = 0; i < 7; ++i)
      memcpy(&p.values[i], inputs[i + 1].data + inputs[i + 1].byte_offset, 4);
    for (uint32_t i = 3; i < 7; ++i)
      if (!ps_gpu_number_supported(p.values[i], 1e-8F))
        return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
  }
  double sigma = 0;
  for (uint32_t i = 0; i < parameter_count; ++i) {
    const ps_operation_parameter_value_v7* v = &parameters[i];
    if (v->key_size == 6 && memcmp(v->key, "radius", 6) == 0)
      p.radius = (uint32_t)v->int64_value;
    if (v->key_size == 5 && memcmp(v->key, "sigma", 5) == 0)
      sigma = v->float64_value;
    if (v->key_size == 6 && memcmp(v->key, "factor", 6) == 0)
      p.factor = (uint32_t)v->int64_value;
  }
  uint8_t* output = sink->allocate_output(sink->context);
  if (!output)
    return PS_OPERATION_RESULT_FAILURE_V7;
  uint8_t* scratch = output;
  uint8_t* extra = output;
  uint64_t scratch_size = sink->output_byte_size, extra_size = scratch_size;
  if (kind == 2) {
    extra_size = (2 * p.radius + 1) * 4;
    extra = sink->allocate_scratch(sink->context, extra_size);
    scratch_size = p.geometry[8] * p.geometry[3] * 16;
    scratch = sink->allocate_scratch(sink->context, scratch_size);
    if (!scratch || !extra)
      return PS_OPERATION_RESULT_FAILURE_V7;
    double total = 0;
    for (int tap = -(int)p.radius; tap <= (int)p.radius; ++tap)
      total += exp(-(double)(tap * tap) / (2 * sigma * sigma));
    for (int tap = -(int)p.radius; tap <= (int)p.radius; ++tap) {
      const double coefficient =
          exp(-(double)(tap * tap) / (2 * sigma * sigma)) / total;
      if (coefficient != 0 && coefficient < 1e-8)
        return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
      const float weight = (float)coefficient;
      memcpy(extra + (tap + (int)p.radius) * 4, &weight, 4);
    }
  } else if (kind == 7) {
    extra_size = p.geometry[2] * 16;
    extra = sink->allocate_scratch(sink->context, extra_size);
    if (!extra)
      return PS_OPERATION_RESULT_FAILURE_V7;
    for (uint64_t row = 0; row < p.geometry[2]; ++row) {
      if (cancelled && cancelled(cancellation_context))
        return PS_OPERATION_RESULT_CANCELLED_V7;
      uint64_t span[2] = {p.geometry[3], 0};
      const double dy = (double)(p.geometry[0] + row) + .5 - p.values[1];
      for (uint64_t col = 0; col < p.geometry[3]; ++col) {
        const double dx = (double)(p.geometry[1] + col) + .5 - p.values[0];
        if (dx * dx + dy * dy <= (double)p.values[2] * p.values[2]) {
          if (span[0] == p.geometry[3])
            span[0] = col;
          span[1] = col + 1;
        }
      }
      memcpy(extra + row * 16, span, 16);
    }
  }
  ps_gpu_buffer_binding_v7 buffers[5];
  const uint8_t* addresses[5] = {
      inputs[0].data,
      (kind == 3 || kind == 4) ? inputs[1].data : inputs[0].data, output,
      scratch, extra};
  const uint64_t sizes[5] = {
      inputs[0].byte_size,
      (kind == 3 || kind == 4) ? inputs[1].byte_size : inputs[0].byte_size,
      sink->output_byte_size, scratch_size, extra_size};
  for (uint32_t i = 0; i < 5; ++i) {
    memset(&buffers[i], 0, sizeof(buffers[i]));
    buffers[i].struct_size = sizeof(buffers[i]);
    buffers[i].index = i;
    buffers[i].byte_size = sizes[i];
    buffers[i].writable = i == 2 || i == 3;
    int result = api->buffer(api->context, addresses[i], sizes[i],
                             buffers[i].writable, &buffers[i].token);
    if (result)
      return result;
  }
  ps_image_gpu_parameters params[2] = {p, p};
  params[1].pass = 1;
  ps_gpu_dispatch_v7 commands[2];
  memset(commands, 0, sizeof(commands));
  for (uint32_t i = 0; i < (kind == 2 ? 2U : 1U); ++i) {
    commands[i].struct_size = sizeof(commands[i]);
    commands[i].source = ps_image_shader;
    commands[i].source_size = sizeof(ps_image_shader) - 1;
    commands[i].entry = "image_operation";
    commands[i].entry_size = 15;
    commands[i].buffers = buffers;
    commands[i].buffer_count = 5;
    commands[i].constants = &params[i];
    commands[i].constant_size = sizeof(p);
    commands[i].constant_index = 5;
    commands[i].grid[0] = p.geometry[3];
    commands[i].grid[1] = kind == 2 && i == 0 ? p.geometry[8] : p.geometry[2];
    commands[i].grid[2] = p.geometry[4];
  }
  int code = api->execute(api->context, commands, kind == 2 ? 2 : 1);
  if (code)
    return code;
  for (uint64_t offset = 0; offset < sink->output_byte_size;
       offset += p.geometry[4] * 4) {
    if ((offset % 4096) == 0 && cancelled && cancelled(cancellation_context))
      return PS_OPERATION_RESULT_CANCELLED_V7;
    float pixel[4] = {0};
    memcpy(pixel, output + offset, p.geometry[4] * 4);
    /* RGB may be signed/HDR; only coverage and alpha are bounded. */
    for (uint64_t c = 0; c < p.geometry[4]; ++c)
      if (!isfinite(pixel[c]))
        return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
    if ((p.geometry[4] == 1 && (pixel[0] < 0 || pixel[0] > 1)) ||
        (p.geometry[4] == 4 &&
         (pixel[3] < 0 || pixel[3] > 1 ||
          (pixel[3] == 0 &&
           (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0)))))
      return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7;
  }
  return sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT32_V7,
                       sink->output_shape, sink->output_rank, inputs[0].facets,
                       inputs[0].facet_count, output, sink->output_byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V7
             : PS_OPERATION_RESULT_FAILURE_V7;
}
// NOLINTEND
#endif  // PLUGINS_OPS_RGBA32F_IMAGE_GPU_H_
