#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"
#include "photospider/plugin/result_tile_helpers.h"

#define PS_BAD_RESULT_TABLE_COUNT 11U
#include "bad_result_table_support.h"

static const ps_result_tensor_spec_v5 image = {
    .struct_size = sizeof(image),
    .key = "pixels",
    .key_size = 6,
    .element_type = 4,
    .rank = 2,
    .shape = {2, 4},
    .batch_rank = 2,
    .batch_shape = {2, 2},
    .spatial = 1,
    .height_axis = 0,
    .width_axis = 1,
    .storage_order = 1,
    .channel_axis = PS_RESULT_NO_CHANNEL_V5};
static const ps_result_schema_v5 schema = {.struct_size = sizeof(schema),
                                           .id = "example.image",
                                           .id_size = 13,
                                           .version = 1,
                                           .tensors = &image,
                                           .tensor_count = 1};
#define TENSOR_SCHEMA(NAME, ID, DTYPE, COUNT)             \
  static const ps_result_tensor_spec_v5 NAME##_tensor = { \
      .struct_size = sizeof(ps_result_tensor_spec_v5),    \
      .key = "samples",                                   \
      .key_size = 7,                                      \
      .element_type = DTYPE,                              \
      .rank = 1,                                          \
      .shape = {COUNT},                                   \
      .channel_axis = PS_RESULT_NO_CHANNEL_V5};           \
  static const ps_result_schema_v5 NAME = {               \
      .struct_size = sizeof(ps_result_schema_v5),         \
      .id = ID,                                           \
      .id_size = sizeof(ID) - 1,                          \
      .version = 1,                                       \
      .tensors = &NAME##_tensor,                          \
      .tensor_count = 1}
TENSOR_SCHEMA(control_schema, "fixture.control", 2, 1);
TENSOR_SCHEMA(number_schema, "fixture.number", 4, 1);
TENSOR_SCHEMA(sequence_schema, "fixture.sequence", 4, 128);
static const ps_result_port_v5 inputs[] = {
    {.struct_size = sizeof(ps_result_port_v5), .schema = &schema},
    {.struct_size = sizeof(ps_result_port_v5), .schema = &control_schema}};
static char member_key[] = "pixels";
static int member_marker;
static const ps_result_port_v5 member_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v5),
     .schema = &schema,
     .element_type = 4,
     .rank = 2,
     .tensor_key = member_key,
     .tensor_key_size = 6},
    {.struct_size = sizeof(ps_result_port_v5), .schema = &control_schema}};
static int sole_marker;
static const ps_result_port_v5 sole_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v5),
     .rank = 2,
     .element_type_mask = 8},
    {.struct_size = sizeof(ps_result_port_v5), .schema = &control_schema}};
static const ps_result_port_v5 sole_fixed_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v5),
     .schema = &schema,
     .rank = 2,
     .element_type_mask = 8},
    {.struct_size = sizeof(ps_result_port_v5), .schema = &control_schema}};
static const ps_result_tensor_spec_v5 bundle_tensors[] = {
    {.struct_size = sizeof(ps_result_tensor_spec_v5),
     .key = "first",
     .key_size = 5,
     .element_type = 4,
     .rank = 1,
     .shape = {1}},
    {.struct_size = sizeof(ps_result_tensor_spec_v5),
     .key = "second",
     .key_size = 6,
     .element_type = 3,
     .rank = 1,
     .shape = {1}}};
static const ps_result_schema_v5 bundle_schema = {
    .struct_size = sizeof(ps_result_schema_v5),
    .id = "test.bundle",
    .id_size = 11,
    .version = 1,
    .tensors = bundle_tensors,
    .tensor_count = 2};
static const ps_result_port_v5 bundle_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v5), .schema = &bundle_schema},
    {.struct_size = sizeof(ps_result_port_v5), .schema = &control_schema}};
static int sole_predicate(const ps_result_port_v5* port) {
  return port->tensor_key == NULL && port->tensor_key_size == 0 &&
         port->element_type == 0 && port->element_type_mask == 8 &&
         port->rank == 2 && port->schema != NULL &&
         port->schema->tensor_count == 1;
}
PS_RESULT_EXPORT void fixture_mutate_member_key(int mutate) {
  memcpy(member_key, mutate ? "broken" : "pixels", 6);
}
static const ps_result_output_v5 regional_output = {
    .struct_size = sizeof(ps_result_output_v5),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v5), .schema = &schema},
    .input_count = UINT32_MAX};
static const ps_result_output_v5 whole_output = {
    .struct_size = sizeof(ps_result_output_v5),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v5), .schema = &schema},
    .input_count = UINT32_MAX};
static const ps_result_output_v5 scalar_output = {
    .struct_size = sizeof(ps_result_output_v5),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v5),
             .schema = &sequence_schema},
    .input_count = 0};
static const ps_result_parameter_descriptor_v5 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V5, 0, 1, 0, 18};
static const ps_result_parameter_descriptor_v5 scalar_parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v5), "scale", 5,
     PS_RESULT_PARAMETER_FLOAT64_V5, 0, 1, 0, 4},
    {sizeof(ps_result_parameter_descriptor_v5), "offset", 6,
     PS_RESULT_PARAMETER_INT64_V5, 0, 1, -2, 2}};
struct State {
  unsigned stage;
  int64_t shift, mode;
  uint64_t handle, window_handle, other_window;
  ps_result_tensor_window_v5 window;
  ps_result_services_v5 saved;
};
struct Work {
  const ps_result_services_v5* services;
  float* samples;
};
static int range(void* raw, uint64_t first, uint64_t end, uint32_t slot) {
  struct Work* work = (struct Work*)raw;
  (void)slot;
  if (work->services->cancelled(work->services->context))
    return 2;
  for (uint64_t i = first; i < end; ++i)
    work->samples[i] += 1.0f;
  return 0;
}
static int forbidden_range(void* raw, uint64_t first, uint64_t end,
                           uint32_t slot) {
  struct Work* work = (struct Work*)raw;
  uint64_t at = 0;
  int64_t value = 0;
  (void)first;
  (void)end;
  (void)slot;
  (void)work->services->read_tensor(work->services->context, 1, 0, &at, 1,
                                    &value, 8);
  return 1;
}
static int tile(void* raw, const ps_cpu_tile_v2* box) {
  return range(raw, box->begin[0], box->end[0], box->slot);
}
static int start(void* user, void* state, const ps_result_query_v5* query,
                 const ps_result_services_v5* services) {
  if (user == &sole_marker && !sole_predicate(query->inputs))
    return 6;
  if (user == &member_marker &&
      (query->inputs[0].tensor_key_size != 6 ||
       memcmp(query->inputs[0].tensor_key, "pixels", 6)))
    return 6;
  if (query->parameter_count && (query->parameters[0].int64_value == 5 ||
                                 query->parameters[0].int64_value == 6))
    ((struct State*)state)->saved = *services;
  return services->abi_version == PS_RESULT_OPERATION_ABI_VERSION_5 &&
                 services->struct_size == sizeof(*services)
             ? 0
             : 6;
}
static ps_result_region_v5 unit(const uint64_t at[4]) {
  ps_result_region_v5 r = {0};
  r.struct_size = sizeof(r);
  r.rank = 4;
  for (unsigned i = 0; i < 4; ++i) {
    r.offset[i] = at[i];
    r.extent[i] = 1;
  }
  return r;
}
static int resolve(void* user, const ps_result_port_v5* inputs, uint32_t count,
                   const ps_result_parameter_value_v5* parameters,
                   uint32_t parameter_count,
                   const ps_result_port_v5* prototypes, uint32_t output_count,
                   const ps_result_metadata_sink_v5* sink) {
  if (user == &sole_marker && !sole_predicate(inputs))
    return 6;
  if (user == &member_marker && (inputs[0].tensor_key_size != 6 ||
                                 memcmp(inputs[0].tensor_key, "pixels", 6)))
    return 6;
  if (parameter_count && parameters[0].int64_value == 4) {
    _Alignas(8) uint8_t raw[sizeof(ps_result_port_v5) + 8];
    memcpy(raw + 1, inputs, sizeof(ps_result_port_v5));
    (void)sink->set_output(sink->context, 0,
                           (const ps_result_port_v5*)(raw + 1));
    return 0;
  }
  if (parameter_count && parameters[0].int64_value == 9) {
    _Alignas(8) uint32_t short_record = 4;
    (void)sink->set_output(sink->context, 0,
                           (const ps_result_port_v5*)&short_record);
    return 0;
  }
  if (count != 2 || !output_count || !inputs[0].schema ||
      inputs[0].schema->tensor_count != 1 ||
      inputs[0].schema->tensors[0].rank != 2 ||
      inputs[0].schema->tensors[0].element_type != 4 ||
      inputs[0].schema->tensors[0].height_axis != 0 ||
      inputs[0].schema->tensors[0].width_axis != 1 ||
      inputs[0].schema->tensors[0].channel_axis != PS_RESULT_NO_CHANNEL_V5)
    return 6;
  // Callback-local nested records exercise synchronous host copying.
  ps_result_tensor_spec_v5 image = inputs[0].schema->tensors[0];
  ps_result_schema_v5 schema = *inputs[0].schema;
  schema.tensors = &image;
  ps_result_port_v5 output = prototypes[0];
  output.schema = &schema;
  if (sink->set_output(sink->context, 0, &output))
    return 1;
  for (uint32_t i = 1; i < output_count; ++i)
    if (sink->set_output(sink->context, i, prototypes + i))
      return 1;
  return 0;
}
static int copy(void* user, void* raw, const ps_result_query_v5* q,
                const ps_result_services_v5* s) {
  const ps_result_tensor_spec_v5* spec = q->output->port.schema->tensors;
  ps_result_region_v5 requested_region;
  if (ps_result_output_region_v5(q, 0, &requested_region))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  const uint32_t requested_count = 1;
  struct State* state = (struct State*)raw;
  uint64_t zero = 0;
  (void)user;
  if (q->parameter_count)
    state->mode = q->parameters[0].int64_value;
  ps_result_region_v5 control = {.struct_size = sizeof(control),
                                 .rank = 1,
                                 .extent = {1}};
  if (state->stage == 0) {
    if (state->mode == 5)
      (void)state->saved.consume_work(state->saved.context, 1);
    if (state->mode == 6 && state->saved.cpu_parallel)
      (void)state->saved.cpu_parallel->run(state->saved.cpu_parallel->context,
                                           0, 1, 0, range, NULL);
    state->stage = 1;
    return ps_result_need_regions_v5(s, q, 1, 0, &control, 1)
               ? 1
               : PS_RESULT_NEED_V5;
  }
  if (state->stage == 1) {
    if (s->read_tensor(s->context, 1, 0, &zero, 1, &state->shift, 8) ||
        state->shift < 0 || state->shift > 1)
      return 6;
    ps_result_region_v5 needed[64];
    uint32_t count = 0;
    for (uint32_t r = 0; r < requested_count; ++r) {
      const ps_result_region_v5* box = &requested_region + r;
      for (uint64_t n = box->offset[0]; n < box->offset[0] + box->extent[0];
           ++n)
        for (uint64_t l = box->offset[1]; l < box->offset[1] + box->extent[1];
             ++l)
          for (uint64_t y = box->offset[2]; y < box->offset[2] + box->extent[2];
               ++y)
            for (uint64_t x = box->offset[3];
                 x < box->offset[3] + box->extent[3]; ++x) {
              uint64_t at[] = {n, l, y,
                               (x + (uint64_t)state->shift) % spec->shape[1]};
              if (count == 64)
                return 6;
              needed[count++] = unit(at);
            }
    }
    state->stage = 2;
    if (!count) {
      if (s->begin_result(s->context) || s->publish_result(s->context, 1))
        return 1;
      return PS_RESULT_PUBLISH_V5;
    }
    return ps_result_need_regions_v5(s, q, 0, 0, needed, count)
               ? 1
               : PS_RESULT_NEED_V5;
  }
  if (state->stage == 2) {
    if (s->retain_tensor(s->context, 0, 0, &state->handle))
      return 1;
    state->stage = 3;
    return ps_result_need_regions_v5(s, q, 1, 0, &control, 1)
               ? 1
               : PS_RESULT_NEED_V5;
  }
  if (s->begin_result(s->context))
    return 1;
  for (uint32_t r = 0; r < requested_count; ++r) {
    const ps_result_region_v5* box = &requested_region + r;
    float pixels[64];
    uint32_t count = 0;
    for (uint64_t n = box->offset[0]; n < box->offset[0] + box->extent[0]; ++n)
      for (uint64_t l = box->offset[1]; l < box->offset[1] + box->extent[1];
           ++l)
        for (uint64_t y = box->offset[2]; y < box->offset[2] + box->extent[2];
             ++y)
          for (uint64_t x = box->offset[3]; x < box->offset[3] + box->extent[3];
               ++x) {
            uint64_t in[] = {n, l, y,
                             (x + (uint64_t)state->shift) % spec->shape[1]};
            if (count == 64 ||
                s->read_retained_tensor(s->context, state->handle, in, 4,
                                        pixels + count, 4))
              return 1;
            ++count;
          }
    struct Work work = {s, pixels};
    if (state->mode == 10 && s->cpu_parallel)
      (void)s->cpu_parallel->run(s->cpu_parallel->context, 1, 1, 0,
                                 forbidden_range, &work);
    if (state->mode == 11 && s->cpu_parallel)
      (void)s->cpu_parallel->run(s->cpu_parallel->context, 1, 0, 0, range,
                                 &work);
    if (user == &whole_output && s->cpu_parallel &&
        s->cpu_parallel->run(s->cpu_parallel->context, count, 1, 0, range,
                             &work))
      return 1;
    if (s->cpu_tiles) {
      ps_cpu_tile_stage_v2 stage = {sizeof(stage), {count, 1, 1}, {1, 1, 1}, 0};
      if (s->cpu_tiles->run(s->cpu_tiles->context, &stage, tile, &work))
        return 1;
    }
    if (state->mode == 1) {
      uint64_t bad[] = {99, 0, 0, 0};
      (void)s->read_retained_tensor(s->context, state->handle, bad, 4, pixels,
                                    4);
    }
    if (state->mode == 2) {
      (void)s->release_tensor(s->context, state->handle);
      (void)s->release_tensor(s->context, state->handle);
    }
    if (state->mode == 3)
      return 2;
    if (s->publish_tensor(s->context, 0, box, (const uint8_t*)pixels,
                          count * 4))
      return 1;
  }
  if (state->mode != 2 && s->release_tensor(s->context, state->handle))
    return 1;
  if (s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
struct WindowWork {
  ps_result_tensor_window_v5 window;
  uint64_t at[8];
};
static int window_worker(void* raw, uint64_t begin, uint64_t end,
                         uint32_t slot) {
  struct WindowWork* work = (struct WindowWork*)raw;
  (void)begin;
  (void)end;
  (void)slot;
  ps_result_tensor_rectangle_v5 run = {.struct_size = sizeof(run)};
  return work->window.rectangle(work->window.context, work->at,
                                work->window.region.rank, &run);
}
static int zero_copy(void* user, void* raw, const ps_result_query_v5* query,
                     const ps_result_services_v5* services) {
  ps_result_region_v5 requested_region;
  if (ps_result_output_region_v5(query, 0, &requested_region))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  const uint32_t requested_count = 1;
  struct State* state = (struct State*)raw;
  (void)user;
  const ps_result_tensor_spec_v5* image = query->inputs[0].schema->tensors;
  ps_result_region_v5 control = {.struct_size = sizeof(control),
                                 .rank = 1,
                                 .extent = {1}};
  if (query->parameter_count)
    state->mode = query->parameters[0].int64_value;
  if (!state->stage) {
    state->stage = 1;
    return ((state->mode == 10 || state->mode == 11)
                ? ps_result_need_descriptor_v5(services, 0)
                : ps_result_need_regions_v5(services, query, 0, 0,
                                            &requested_region, requested_count))
               ? 1
               : PS_RESULT_NEED_V5;
  }
  if (state->stage == 1) {
    state->stage = 2;
    if (services->retain_tensor(services->context, 0, 0, &state->handle))
      return 1;
    if (state->mode != 11 && requested_count) {
      ps_result_region_v5 first = requested_region;
      first.extent[0] = first.extent[1] = 1;
      state->window.struct_size = sizeof(state->window);
      if (services->acquire_tensor_window(services->context, 0, 0, &first,
                                          &state->window,
                                          &state->window_handle) ||
          services->retain_window(services->context, state->window_handle,
                                  &state->other_window))
        return 1;
    }
    return ps_result_need_regions_v5(services, query, 1, 0, &control, 1)
               ? 1
               : PS_RESULT_NEED_V5;
  }
  if (state->window_handle) {
    ps_result_tensor_row_v5 row = {.struct_size = sizeof(row)};
    uint64_t at[8];
    memcpy(at, state->window.region.offset, sizeof(at));
    if (state->mode == 1)
      at[0] = image->batch_shape[0];
    if (state->window.row(state->window.context, at, 4, &row))
      return 6;
    if (state->mode == 7) {
      // A worker-facing table cannot borrow the host phase. Ignore the first
      // failure and report a different callback status: the host retains the
      // complete original owning-window work-limit cause.
      for (uint32_t attempt = 0; attempt < 100000; ++attempt)
        if (state->window.row(state->window.context, at, 4, &row))
          return 6;
      return 1;
    }
    const uint8_t* pointer = row.data;
    if (services->release_window(services->context, state->window_handle))
      return 1;
    if (state->mode == 2)
      (void)services->release_window(services->context, state->window_handle);
    if (state->window.row(state->window.context, at, 4, &row) ||
        row.data != pointer)
      return 6;
    if (services->cpu_parallel) {
      struct WindowWork work = {state->window, {0}};
      memcpy(work.at, at, sizeof(at));
      if (services->cpu_parallel->run(services->cpu_parallel->context, 1, 1, 0,
                                      window_worker, &work))
        return 1;
    }
  }
  if (state->mode == 18) {
    for (uint32_t i = 0; i < 1025; ++i) {
      ps_result_tensor_window_v5 window = {.struct_size = sizeof(window)};
      uint64_t handle = 0;
      if (services->acquire_retained_window(services->context, state->handle,
                                            &requested_region, &window,
                                            &handle) ||
          services->release_window(services->context, handle))
        return 1;
    }
  }
  if (state->mode == 3)
    return 2;
  if (services->begin_result(services->context))
    return 1;
  ps_result_view_axis_v5 axes[4] = {{0, 0, 1, 1},
                                    {1, 0, 1, 1},
                                    {2, 0, 1, 1},
                                    {3, 0, 1, 1}};
  ps_result_tensor_transform_v5 transform = {.struct_size = sizeof(transform),
                                             .source_rank = 4,
                                             .axes = axes};
  if (state->mode == 14)
    transform.source_rank = 3;
  if (state->mode == 15)
    axes[0].extent = 2;
  if (state->mode == 16)
    axes[3].source_origin = image->shape[1];
  const ps_result_region_v5* requested = &requested_region;
  const uint32_t count = requested_count;
  for (uint32_t box = 0; box < count; ++box)
    for (uint64_t frame = requested[box].offset[0];
         frame < requested[box].offset[0] + requested[box].extent[0]; ++frame)
      for (uint64_t layer = requested[box].offset[1];
           layer < requested[box].offset[1] + requested[box].extent[1];
           ++layer) {
        ps_result_region_v5 region = requested[box];
        region.offset[0] = frame;
        region.offset[1] = layer;
        region.extent[0] = region.extent[1] = 1;
        ps_result_tensor_window_v5 window = {.struct_size = sizeof(window)};
        uint64_t handle = 0;
        if (services->acquire_retained_window(services->context, state->handle,
                                              &region, &window, &handle) ||
            services->publish_tensor_view(
                services->context, 0, &region, &handle, 1,
                state->mode >= 13 ? &transform : NULL) ||
            services->release_window(services->context, handle))
          return 1;
      }
  if (state->other_window &&
      services->release_window(services->context, state->other_window))
    return 1;
  uint64_t report[3] = {0, 0, 0};
  for (uint32_t box = 0; box < count; ++box) {
    uint64_t elements = 1;
    for (uint32_t axis = 0; axis < requested[box].rank; ++axis)
      elements *= requested[box].extent[axis];
    report[0] += elements;
    report[1] += elements;
  }
  if (state->mode == 12)
    report[2] = 1;
  (void)services->report_diagnostic(services->context, 0,
                                    (const uint8_t*)report, sizeof(report));
  if (state->mode == 8 && state->window_handle) {
    uint64_t at[8];
    memcpy(at, state->window.region.offset, sizeof(at));
    ps_result_tensor_row_v5 row = {.struct_size = sizeof(row)};
    (void)state->window.row(state->window.context, at, 4, &row);
  }
  if (services->release_tensor(services->context, state->handle) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static int scalar(void* user, void* state, const ps_result_query_v5* q,
                  const ps_result_services_v5* s) {
  (void)user;
  (void)state;
  int64_t offset = 0;
  double scale = 1;
  for (uint32_t i = 0; i < q->parameter_count; ++i) {
    const ps_result_parameter_value_v5* parameter = q->parameters + i;
    if (parameter->key_size == 6 && !memcmp(parameter->key, "offset", 6))
      offset = parameter->int64_value;
    else if (parameter->key_size == 5 && !memcmp(parameter->key, "scale", 5))
      scale = parameter->float64_value;
    else
      return 6;
  }
  ps_result_region_v5 requested_region;
  if (ps_result_output_region_v5(q, 0, &requested_region))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  const uint32_t requested_count = 1;
  const uint32_t count = requested_count;
  const ps_result_region_v5* boxes = &requested_region;
  if (s->begin_result(s->context))
    return 1;
  for (uint32_t i = 0; i < count; ++i) {
    float samples[128];
    const ps_result_region_v5* box = boxes + i;
    if (box->rank != 1 || box->extent[0] > 128)
      return 6;
    for (uint64_t n = 0; n < box->extent[0]; ++n)
      samples[n] = (float)(((int64_t)(box->offset[0] + n) + offset) * scale);
    if (s->publish_tensor(s->context, 0, box, (const uint8_t*)samples,
                          box->extent[0] * 4))
      return 1;
  }
  return s->publish_result(s->context, 1) ? 1 : PS_RESULT_PUBLISH_V5;
}

static const ps_result_output_v5 number_output = {
    .struct_size = sizeof(number_output),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v5),
             .schema = &number_schema},
    .input_count = UINT32_MAX};
TENSOR_SCHEMA(lut_schema, "fixture.lut", 4, 4);
static const ps_result_output_v5 mixed_outputs[] = {
    {.struct_size = sizeof(ps_result_output_v5),
     .key = "image",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v5), .schema = &schema},
     .input_count = UINT32_MAX},
    {.struct_size = sizeof(ps_result_output_v5),
     .key = "number",
     .key_size = 6,
     .port = {.struct_size = sizeof(ps_result_port_v5),
              .schema = &number_schema},
     .input_count = 0},
    {.struct_size = sizeof(ps_result_output_v5),
     .key = "lut",
     .key_size = 3,
     .port = {.struct_size = sizeof(ps_result_port_v5), .schema = &lut_schema},
     .input_count = 0}};
static int mixed(void* user, void* raw, const ps_result_query_v5* q,
                 const ps_result_services_v5* s) {
  if (q->output_index == 0)
    return copy(user, raw, q, s);
  if (q->output_index == 2)
    return scalar(user, raw, q, s);
  if (q->parameter_count && q->parameters[0].int64_value == 12)
    return ps_result_to_whole_v5(s, q, 0) ? 1 : PS_RESULT_NEED_V5;
  const ps_result_tensor_spec_v5* image = q->inputs[0].schema->tensors;
  float count = (float)(image->batch_shape[0] * image->batch_shape[1] *
                        image->shape[0] * image->shape[1]);
  ps_result_region_v5 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (s->begin_result(s->context) ||
      s->publish_tensor(s->context, 0, &one, (const uint8_t*)&count, 4) ||
      s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static const ps_result_tensor_spec_v5 gpu_image = {
    .struct_size = sizeof(gpu_image),
    .key = "pixels",
    .key_size = 6,
    .element_type = 4,
    .rank = 2,
    .shape = {1, 1},
    .batch_rank = 2,
    .batch_shape = {1, 1},
    .spatial = 1,
    .height_axis = 0,
    .width_axis = 1,
    .channel_axis = UINT32_MAX,
    .storage_order = 1};
static const ps_result_schema_v5 gpu_schema = {
    .struct_size = sizeof(gpu_schema),
    .id = "fixture.gpu",
    .id_size = 11,
    .version = 1,
    .tensors = &gpu_image,
    .tensor_count = 1};
static const ps_result_output_v5 gpu_output = {
    .struct_size = sizeof(gpu_output),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v5), .schema = &gpu_schema},
    .input_count = 0};
static int native(void* user, void* raw, const ps_result_query_v5* q,
                  const ps_result_services_v5* s) {
  (void)user;
  (void)raw;
  (void)q;
  if (!s->gpu || s->gpu->struct_size != sizeof(*s->gpu) ||
      s->gpu->abi_version != PS_GPU_ABI_VERSION_1 ||
      s->gpu->backend != PS_GPU_BACKEND_METAL_V1)
    return 3;
  if (q->parameter_count && q->parameters[0].int64_value == 1)
    (void)s->gpu->release(s->gpu->context, 0);
  if (q->parameter_count && q->parameters[0].int64_value == 2) {
    uint64_t ignored = 0;
    (void)s->gpu->buffer(s->gpu->context, NULL, 4, 2, &ignored);
  }
  uint8_t* in = NULL;
  uint8_t* out = NULL;
  uint64_t a = 0, b = 0;
  float number = 8;
  if (s->allocate_scratch(s->context, 4, &in) ||
      s->allocate_scratch(s->context, 4, &out))
    return 4;
  memcpy(in, &number, 4);
  if (s->gpu->buffer(s->gpu->context, in, 4, 0, &a) ||
      s->gpu->buffer(s->gpu->context, out, 4, 1, &b))
    return 1;
  ps_gpu_buffer_binding_v1 buffers[2] = {
      {.struct_size = sizeof(ps_gpu_buffer_binding_v1),
       .index = 0,
       .token = a,
       .byte_size = 4},
      {.struct_size = sizeof(ps_gpu_buffer_binding_v1),
       .index = 1,
       .token = b,
       .byte_size = 4,
       .writable = 1}};
  static const char shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\nkernel void "
      "scale(device const float* a [[buffer(0)]],device float* b "
      "[[buffer(1)]], uint i [[thread_position_in_grid]]){b[i]=a[i]*.5f;}";
  ps_gpu_dispatch_v1 command = {0};
  command.struct_size = sizeof(command);
  command.source = shader;
  command.source_size = sizeof(shader) - 1;
  command.entry = "scale";
  command.entry_size = 5;
  command.buffers = buffers;
  command.buffer_count = 2;
  command.grid[0] = command.grid[1] = command.grid[2] = 1;
  if (s->gpu->execute(s->gpu->context, &command, 1) ||
      s->gpu->release(s->gpu->context, a) ||
      s->gpu->release(s->gpu->context, b))
    return 1;
  ps_result_region_v5 all = {.struct_size = sizeof(all),
                             .rank = 4,
                             .extent = {1, 1, 1, 1}};
  if (s->begin_result(s->context) ||
      s->publish_tensor(s->context, 0, &all, out, 4) ||
      s->publish_result(s->context, 1))
    return 1;
  return 101;
}
static int native_input(void* user, void* raw, const ps_result_query_v5* q,
                        const ps_result_services_v5* s) {
  (void)user;
  struct State* state = (struct State*)raw;
  ps_result_region_v5 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  const int64_t mode = q->parameter_count ? q->parameters[0].int64_value : 0;
  if (!state->stage++)
    return (mode == 1 ? ps_result_need_descriptor_v5(s, 0)
                      : ps_result_need_regions_v5(s, q, 0, 0, &one, 1))
               ? 1
               : PS_RESULT_NEED_V5;
  ps_result_tensor_window_v5 window = {.struct_size = sizeof(window)};
  uint64_t window_handle = 0;
  if (s->acquire_native_tensor_window(s->context, 0, mode == 2 ? 1 : 0, &one,
                                      &window, &window_handle))
    return 1;
  const uint64_t at = 0;
  ps_result_tensor_row_v5 row = {.struct_size = sizeof(row)};
  if (window.row(window.context, &at, 1, &row))
    return 1;
  uint8_t* output = NULL;
  uint64_t input = 0, result = 0;
  if (!s->gpu || s->gpu->struct_size != sizeof(*s->gpu) ||
      s->gpu->abi_version != PS_GPU_ABI_VERSION_1 ||
      s->gpu->backend != PS_GPU_BACKEND_METAL_V1)
    return 3;
  if (s->allocate_scratch(s->context, 4, &output) ||
      s->gpu->buffer(s->gpu->context, row.data, 4, 0, &input) ||
      s->gpu->buffer(s->gpu->context, output, 4, 1, &result))
    return 1;
  ps_gpu_buffer_binding_v1 bindings[] = {
      {sizeof(ps_gpu_buffer_binding_v1), 0, input, 0, 4, 0},
      {sizeof(ps_gpu_buffer_binding_v1), 1, result, 0, 4, 1}};
  const char shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\nkernel void "
      "half_value(device const float* a [[buffer(0)]],device float* b "
      "[[buffer(1)]],uint i [[thread_position_in_grid]]){b[i]=a[i]*.5f;}";
  ps_gpu_dispatch_v1 command = {0};
  command.struct_size = sizeof(command);
  command.source = shader;
  command.source_size = sizeof(shader) - 1;
  command.entry = "half_value";
  command.entry_size = 10;
  command.buffers = bindings;
  command.buffer_count = 2;
  command.grid[0] = command.grid[1] = command.grid[2] = 1;
  if (s->gpu->execute(s->gpu->context, &command, 1) ||
      s->gpu->release(s->gpu->context, input) ||
      s->gpu->release(s->gpu->context, result) ||
      s->release_window(s->context, window_handle) ||
      s->begin_result(s->context) ||
      s->publish_tensor(s->context, 0, &one, output, 4) ||
      s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static const ps_result_port_v5 native_number_input = {
    .struct_size = sizeof(ps_result_port_v5),
    .schema = &number_schema};
static void destroy_state(void* user, void* state) {
  (void)user;
  (void)state;
}
static void destroy_plugin(void* context) {
  (void)context;
  ps_test_bad_result_retired();
}
#define COPY_OP(KEY, OUTPUT, TILES)                                   \
  {.struct_size = sizeof(ps_result_operation_v5),                     \
   .key = KEY,                                                        \
   .key_size = sizeof(KEY) - 1,                                       \
   .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 | \
            PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,                       \
   .inputs = inputs,                                                  \
   .input_count = 2,                                                  \
   .outputs = OUTPUT,                                                 \
   .output_count = 1,                                                 \
   .parameters = &mode,                                               \
   .parameter_count = 1,                                              \
   .state_bytes = sizeof(struct State),                               \
   .workspace_bytes = 65536,                                          \
   .maximum_stages = 8,                                               \
   .user_data = (void*)OUTPUT,                                        \
   .cpu_staged_tiles = TILES,                                         \
   .resolve_metadata = resolve,                                       \
   .start = start,                                                    \
   .poll = copy,                                                      \
   .destroy = destroy_state}
static const ps_result_tensor_spec_v5 reshape_input_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v5),
    .key = "samples",
    .key_size = 7,
    .element_type = 1,
    .rank = 2,
    .shape = {2, 3},
    .channel_axis = PS_RESULT_NO_CHANNEL_V5};
static const ps_result_tensor_spec_v5 reshape_output_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v5),
    .key = "samples",
    .key_size = 7,
    .element_type = 1,
    .rank = 2,
    .shape = {3, 2},
    .channel_axis = PS_RESULT_NO_CHANNEL_V5};
static const ps_result_schema_v5 reshape_input_schema = {
    .struct_size = sizeof(ps_result_schema_v5),
    .id = "fixture.tensor.input",
    .id_size = 20,
    .version = 1,
    .tensors = &reshape_input_tensor,
    .tensor_count = 1};
static const ps_result_schema_v5 reshape_output_schema = {
    .struct_size = sizeof(ps_result_schema_v5),
    .id = "fixture.tensor.output",
    .id_size = 21,
    .version = 1,
    .tensors = &reshape_output_tensor,
    .tensor_count = 1};
static const ps_result_port_v5 reshape_input = {
    .struct_size = sizeof(ps_result_port_v5),
    .schema = &reshape_input_schema,
    .tensor_key = "samples",
    .tensor_key_size = 7,
    .element_type = 1,
    .rank = 2};
static const ps_result_output_v5 reshape_output = {
    .struct_size = sizeof(ps_result_output_v5),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v5),
             .schema = &reshape_output_schema,
             .tensor_key = "samples",
             .tensor_key_size = 7},
    .input_count = UINT32_MAX};
static int reshape_poll(void* user, void* raw, const ps_result_query_v5* query,
                        const ps_result_services_v5* services) {
  (void)user;
  (void)query;
  struct State* state = (struct State*)raw;
  ps_result_region_v5 source = {.struct_size = sizeof(source),
                                .rank = 2,
                                .extent = {2, 3}};
  ps_result_region_v5 output = {.struct_size = sizeof(output),
                                .rank = 2,
                                .extent = {3, 2}};
  if (!state->stage++)
    return ps_result_need_regions_v5(services, query, 0, 0, &source, 1)
               ? 1
               : PS_RESULT_NEED_V5;
  uint64_t handle = 0;
  ps_result_tensor_window_v5 window = {.struct_size = sizeof(window)};
  if (services->acquire_tensor_window(services->context, 0, 0, &source, &window,
                                      &handle))
    return 1;
  uint64_t at[] = {0, 0};
  ps_result_tensor_row_v5 row = {.struct_size = sizeof(row)};
  if (window.row(window.context, at, 2, &row) || row.samples != 3 ||
      (row.sample_stride_bytes != -1 && row.sample_stride_bytes != 0))
    return 6;
  uint8_t first = row.data[0], third;
  memcpy(&third, row.data + 2 * row.sample_stride_bytes, 1);
  if (third != (row.sample_stride_bytes ? first - 2 : first))
    return 6;
  const ps_result_tensor_transform_v5 transform = {
      .struct_size = sizeof(transform),
      .reshape = 1};
  if (services->begin_result(services->context) ||
      services->publish_tensor_view(services->context, 0, &output, &handle, 1,
                                    &transform) ||
      services->release_window(services->context, handle) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static const ps_result_tensor_spec_v5 bounded_tensor = {
    .struct_size = sizeof(bounded_tensor),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V5,
    .rank = 2,
    .shape = {1, 4},
    .spatial = 1,
    .height_axis = 0,
    .width_axis = 1,
    .storage_order = 1,
    .channel_axis = PS_RESULT_NO_CHANNEL_V5};
static const ps_result_schema_v5 bounded_schema = {
    .struct_size = sizeof(bounded_schema),
    .id = "fixture.bounded.tensor",
    .id_size = sizeof("fixture.bounded.tensor") - 1,
    .version = 1,
    .tensors = &bounded_tensor,
    .tensor_count = 1};
static const ps_result_output_v5 bounded_output_outputs[] = {
    {.struct_size = sizeof(ps_result_output_v5),
     .key = "left",
     .key_size = 4,
     .port = {.struct_size = sizeof(ps_result_port_v5),
              .schema = &bounded_schema},
     .input_count = 0,
     .flags = PS_RESULT_OUTPUT_PAYLOAD_BOUND_V5,
     .maximum_output_payload_bytes = 0},
    {.struct_size = sizeof(ps_result_output_v5),
     .key = "right",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v5),
              .schema = &bounded_schema},
     .input_count = 0,
     .flags = PS_RESULT_OUTPUT_PAYLOAD_BOUND_V5,
     .maximum_output_payload_bytes = 8}};
static int bounded_output_policy(const ps_result_query_v5* query) {
  return query->output_index < 2 && query->output &&
         query->output->struct_size == sizeof(ps_result_output_v5) &&
         query->output->flags == PS_RESULT_OUTPUT_PAYLOAD_BOUND_V5 &&
         query->output->maximum_output_payload_bytes ==
             (query->output_index ? 8 : 0);
}
static int bounded_single_start(void* user, void* raw,
                                const ps_result_query_v5* query,
                                const ps_result_services_v5* service) {
  (void)user;
  (void)raw;
  (void)service;
  return bounded_output_policy(query) ? 0 : 6;
}
static int bounded_single_poll(void* user, void* raw,
                               const ps_result_query_v5* query,
                               const ps_result_services_v5* service) {
  ps_result_region_v5 requested_region;
  if (ps_result_output_region_v5(query, 0, &requested_region))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  const uint32_t requested_count = 1;
  (void)user;
  (void)raw;
  if (!bounded_output_policy(query) || requested_count != 1 ||
      requested_region.rank != 2 || requested_region.extent[0] != 1 ||
      requested_region.extent[1] != 1)
    return 6;
  const double value = query->output_index ? 11 : 7;
  uint8_t* payload = NULL;
  if (service->allocate_scratch(service->context, sizeof(value), &payload))
    return PS_RESULT_STATUS_FAILURE_V5;
  memcpy(payload, &value, sizeof(value));
  if (service->begin_result(service->context) ||
      service->publish_tensor_buffer(service->context, 0, &requested_region,
                                     payload, sizeof(value)) ||
      service->publish_result(service->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static const ps_result_tensor_spec_v5 sparse_wide_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v5),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V5,
    .rank = 2,
    .shape = {1, UINT64_C(1) << 61},
    .spatial = 1,
    .height_axis = 0,
    .width_axis = 1,
    .storage_order = 1,
    .channel_axis = PS_RESULT_NO_CHANNEL_V5};
static const ps_result_schema_v5 sparse_wide_schema = {
    .struct_size = sizeof(ps_result_schema_v5),
    .id = "fixture.sparse_wide",
    .id_size = sizeof("fixture.sparse_wide") - 1,
    .version = 1,
    .tensors = &sparse_wide_tensor,
    .tensor_count = 1};
static const ps_result_output_v5 sparse_wide_output = {
    .struct_size = sizeof(ps_result_output_v5),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v5),
             .schema = &sparse_wide_schema},
    .flags = PS_RESULT_OUTPUT_PAYLOAD_BOUND_V5,
    .maximum_output_payload_bytes = 8};
static int sparse_wide_start(void* user, void* state,
                             const ps_result_query_v5* query,
                             const ps_result_services_v5* services) {
  (void)user;
  (void)state;
  (void)query;
  (void)services;
  return PS_RESULT_STATUS_OK_V5;
}
static int sparse_wide_poll(void* user, void* state,
                            const ps_result_query_v5* query,
                            const ps_result_services_v5* service) {
  ps_result_region_v5 requested_region;
  if (ps_result_output_region_v5(query, 0, &requested_region))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  const uint32_t requested_count = 1;
  (void)user;
  (void)state;
  if (requested_count != 1 || requested_region.rank != 2 ||
      requested_region.extent[1] != 1 || requested_region.extent[0] != 1)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  const double value = 11;
  if (service->begin_result(service->context) ||
      service->publish_tensor(service->context, 0, &requested_region,
                              (const uint8_t*)&value, sizeof(value)) ||
      service->publish_result(service->context, 1))
    return PS_RESULT_STATUS_FAILURE_V5;
  return PS_RESULT_PUBLISH_V5;
}
static const ps_result_tensor_spec_v5 prefix_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v5),
    .key = "samples",
    .key_size = 7,
    .element_type = 1,
    .rank = 1,
    .shape = {6},
    .channel_axis = PS_RESULT_NO_CHANNEL_V5};
static const ps_result_schema_v5 prefix_schema = {
    .struct_size = sizeof(ps_result_schema_v5),
    .id = "fixture.prefix",
    .id_size = 14,
    .version = 1,
    .tensors = &prefix_tensor,
    .tensor_count = 1};
static const ps_result_port_v5 prefix_input = {
    .struct_size = sizeof(ps_result_port_v5),
    .schema = &prefix_schema};
static const ps_result_output_v5 prefix_output = {
    .struct_size = sizeof(ps_result_output_v5),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v5),
             .schema = &prefix_schema},
    .input_count = UINT32_MAX};
static int prefix_poll(void* user, void* raw, const ps_result_query_v5* query,
                       const ps_result_services_v5* services) {
  (void)user;
  struct State* state = (struct State*)raw;
  ps_result_region_v5 domain = {.struct_size = sizeof(domain),
                                .rank = 1,
                                .extent = {6}};
  if (!state->stage++)
    return ps_result_need_regions_v5(services, query, 0, 0, &domain, 1)
               ? 1
               : PS_RESULT_NEED_V5;
  uint8_t values[6], carry = 0;
  for (uint64_t i = 0; i < 6; ++i) {
    uint8_t value;
    if (services->read_tensor(services->context, 0, 0, &i, 1, &value, 1))
      return 1;
    carry += value;
    values[i] = carry;
  }
  if (services->begin_result(services->context) ||
      services->publish_tensor(services->context, 0, &domain, values, 6) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static int neighborhood_poll(void* user, void* raw,
                             const ps_result_query_v5* query,
                             const ps_result_services_v5* services) {
  (void)user;
  struct State* state = (struct State*)raw;
  const ps_result_region_v5 domain = {.struct_size = sizeof(domain),
                                      .rank = 1,
                                      .extent = {6}};
  if (!state->stage++)
    return ps_result_need_regions_v5(services, query, 0, 0, &domain, 1)
               ? 1
               : PS_RESULT_NEED_V5;
  const int64_t mode =
      query->parameter_count ? query->parameters[0].int64_value : 0;
  uint8_t values[6] = {0};
  for (uint64_t i = 0; i < 6; ++i)
    for (int64_t tap = -1; tap <= 1; ++tap) {
      int64_t source = (int64_t)i + tap;
      if (mode == 8)
        source = (source + 6) % 6;
      if (source < 0 || source >= 6)
        continue;
      const uint64_t at = (uint64_t)source;
      uint8_t value;
      if (services->read_tensor(services->context, 0, 0, &at, 1, &value, 1))
        return 1;
      values[i] += value;
    }
  if (services->begin_result(services->context) ||
      services->publish_tensor(services->context, 0, &domain, values, 6) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static int cartesian_poll(void* user, void* raw,
                          const ps_result_query_v5* query,
                          const ps_result_services_v5* services) {
  (void)user;
  struct State* state = (struct State*)raw;
  const int64_t mode =
      query->parameter_count ? query->parameters[0].int64_value : 0;
  const ps_result_region_v5 domain = {.struct_size = sizeof(domain),
                                      .rank = 1,
                                      .extent = {6}};
  const ps_result_region_v5 source = {.struct_size = sizeof(source),
                                      .rank = 1,
                                      .offset = {1},
                                      .extent = {3}};
  if (!state->stage++)
    return ps_result_need_regions_v5(services, query, 0, 0, &source, 1)
               ? 1
               : PS_RESULT_NEED_V5;
  if (mode == 10 || mode == 11) {
    ps_result_tensor_window_v5 window = {.struct_size = sizeof(window)};
    uint64_t handle = 0;
    if (services->acquire_tensor_window(services->context, 0, 0, &source,
                                        &window, &handle))
      return 1;
    for (uint64_t i = 0; i < 2000; ++i) {
      const uint64_t at = 3;
      int failed;
      if (mode == 10) {
        ps_result_tensor_row_v5 row = {.struct_size = sizeof(row)};
        failed = window.row(window.context, &at, 1, &row);
      } else {
        ps_result_tensor_rectangle_v5 rectangle = {.struct_size =
                                                       sizeof(rectangle)};
        failed = window.rectangle(window.context, &at, 1, &rectangle);
      }
      if (failed)
        return 1;
    }
    if (services->release_window(services->context, handle))
      return 1;
  }
  uint8_t sum = 0, values[6];
  for (uint64_t i = 1; i < 4; ++i) {
    uint8_t value;
    if (services->read_tensor(services->context, 0, 0, &i, 1, &value, 1))
      return 1;
    sum += value;
  }
  memset(values, sum, sizeof(values));
  if (services->begin_result(services->context) ||
      services->publish_tensor(services->context, 0, &domain, values, 6) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static int fallback_enabled = 1, fallback_disabled = 0;
static _Atomic uint32_t impure_calls;
PS_RESULT_EXPORT uint32_t fixture_result_impure_calls(void) {
  return atomic_load(&impure_calls);
}
static int impure_start(void* user, void* raw, const ps_result_query_v5* q,
                        const ps_result_services_v5* s) {
  (void)user;
  (void)q;
  (void)s;
  *(float*)raw = q->shared_stage == UINT32_MAX
                     ? 0.0f
                     : (float)(atomic_fetch_add(&impure_calls, 1) + 1);
  return PS_RESULT_STATUS_OK_V5;
}
static int impure_poll(void* user, void* raw, const ps_result_query_v5* q,
                       const ps_result_services_v5* s) {
  (void)user;
  (void)q;
  const ps_result_region_v5 one = {.struct_size = sizeof(one),
                                   .rank = 1,
                                   .extent = {1}};
  if (q->shared_stage == UINT32_MAX) {
    if (*(float*)raw == 0.0f) {
      *(float*)raw = 1.0f;
      return s->need_shared_stage(s->context, 0) ? 1 : PS_RESULT_NEED_V5;
    }
    ps_result_tensor_window_v5 window = {.struct_size = sizeof(window)};
    uint64_t handle = 0;
    const ps_result_view_axis_v5 axis = {.output_axis = 0,
                                         .step = 1,
                                         .extent = 1};
    const ps_result_tensor_transform_v5 transform = {
        .struct_size = sizeof(transform),
        .source_rank = 1,
        .axes = &axis};
    if (s->acquire_shared_window(s->context, 0, 0, &one, &window, &handle) ||
        s->begin_result(s->context) ||
        s->publish_tensor_view(s->context, 0, &one, &handle, 1, &transform) ||
        s->release_window(s->context, handle) ||
        s->publish_result(s->context, 1))
      return 1;
    return PS_RESULT_PUBLISH_V5;
  }
  if (s->begin_result(s->context) ||
      s->publish_tensor(s->context, 0, &one, raw, sizeof(float)) ||
      s->publish_result(s->context, 1))
    return PS_RESULT_STATUS_FAILURE_V5;
  return PS_RESULT_PUBLISH_V5;
}
static const ps_result_output_v5 impure_output = {
    .struct_size = sizeof(ps_result_output_v5),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v5),
             .schema = &number_schema},
    .input_count = 0};
static const ps_result_shared_stage_v5 impure_stage = {
    .struct_size = sizeof(impure_stage),
    .key = "effect",
    .key_size = 6,
    .schema = &number_schema,
    .backend = 1,
    .mandatory = 1,
    .state_bytes = sizeof(float),
    .maximum_work = 10000,
    .maximum_output_payload_bytes = sizeof(float),
    .maximum_stages = 1};
static _Atomic uint32_t fallback_starts[2][2][16];
static _Atomic uint32_t fallback_destroys[2][2][16];
static _Atomic uint32_t fallback_dispatches;
struct FallbackState {
  uint32_t enabled, backend, mode;
};
PS_RESULT_EXPORT uint32_t fixture_result_attempts(uint32_t enabled,
                                                  uint32_t backend,
                                                  uint32_t mode,
                                                  uint32_t destroyed) {
  if (enabled > 1 || backend < 1 || backend > 2 || mode >= 16)
    return 0;
  return atomic_load(destroyed ? &fallback_destroys[enabled][backend - 1][mode]
                               : &fallback_starts[enabled][backend - 1][mode]);
}
PS_RESULT_EXPORT uint32_t fixture_result_dispatches(void) {
  return atomic_load(&fallback_dispatches);
}
static int fallback_publish(const ps_result_services_v5* s) {
  const float value = 7;
  const ps_result_region_v5 one = {.struct_size = sizeof(one),
                                   .rank = 1,
                                   .extent = {1}};
  return s->begin_result(s->context) ||
         s->publish_tensor(s->context, 0, &one, (const uint8_t*)&value, 4) ||
         s->publish_result(s->context, 1);
}
static int fallback_start(void* user, void* raw, const ps_result_query_v5* q,
                          const ps_result_services_v5* s) {
  struct FallbackState* state = raw;
  state->enabled = *(const int*)user;
  state->backend = q->backend;
  state->mode = q->parameter_count ? q->parameters[0].int64_value : 0;
  if (state->backend < 1 || state->backend > 2 || state->mode >= 16)
    return 6;
  atomic_fetch_add(
      &fallback_starts[state->enabled][state->backend - 1][state->mode], 1);
  if (q->backend == 2 && (state->mode == 12 || state->mode == 13)) {
    uint64_t at = 0;
    float value;
    (void)s->read_tensor(s->context, 0, 0, &at, 1, &value, 4);
    return state->mode == 12 ? 3 : 2;
  }
  if (q->backend == 2 && state->mode == 0)
    return 3;
  if (q->backend == 2 && state->mode == 10)
    return fallback_publish(s) ? 1 : 3;
  return 0;
}
static int fallback_poll(void* user, void* raw, const ps_result_query_v5* q,
                         const ps_result_services_v5* s) {
  (void)user;
  (void)q;
  const struct FallbackState* state = raw;
  if (state->mode == 14) {
    (void)s->publish_result(s->context, 2);
    return PS_RESULT_PUBLISH_V5;
  }
  if (state->mode == 15) {
    if (fallback_publish(s))
      return 1;
    (void)s->publish_result(s->context, 1);
    return 2;
  }
  if (state->backend == 1) {
    if (state->mode == 11 && s->publish_result(NULL, 1) != 6)
      return 1;
    return fallback_publish(s) ? 1 : PS_RESULT_PUBLISH_V5;
  }
  switch (state->mode) {
    case 1:
      return 3;
    case 2:
      return 1;
    case 3:
      return 99;
    case 4:
      return 2;
    case 5:
      return fallback_publish(s) ? 1 : 3;
    case 6: {
      const float value = 7;
      const ps_result_region_v5 one = {.struct_size = sizeof(one),
                                       .rank = 1,
                                       .extent = {1}};
      if (s->begin_result(s->context))
        return 1;
      (void)s->publish_tensor(s->context, 99, &one, (const uint8_t*)&value, 4);
      return 3;
    }
    case 7: {
      uint64_t at = 0;
      float value;
      (void)s->read_tensor(s->context, 0, 0, &at, 1, &value, 4);
      return 3;
    }
    case 8:
      return s->begin_result(s->context) ? 1 : 3;
    case 9: {
      if (!s->gpu || s->gpu->struct_size != sizeof(*s->gpu) ||
          s->gpu->abi_version != PS_GPU_ABI_VERSION_1 ||
          s->gpu->backend != PS_GPU_BACKEND_METAL_V1)
        return 3;
      uint8_t* out = NULL;
      uint64_t token = 0;
      if (s->allocate_scratch(s->context, 4, &out) ||
          s->gpu->buffer(s->gpu->context, out, 4, 1, &token))
        return 1;
      const ps_gpu_buffer_binding_v1 binding = {.struct_size = sizeof(binding),
                                                .token = token,
                                                .byte_size = 4,
                                                .writable = 1};
      static const char shader[] =
          "#include <metal_stdlib>\nusing namespace metal;\nkernel void "
          "write_seven(device float* out [[buffer(0)]]){out[0]=7.f;}";
      ps_gpu_dispatch_v1 command = {0};
      command.struct_size = sizeof(command);
      command.source = shader;
      command.source_size = sizeof(shader) - 1;
      command.entry = "write_seven";
      command.entry_size = 11;
      command.buffers = &binding;
      command.buffer_count = 1;
      command.grid[0] = command.grid[1] = command.grid[2] = 1;
      if (s->gpu->execute(s->gpu->context, &command, 1) ||
          s->gpu->release(s->gpu->context, token))
        return 1;
      atomic_fetch_add(&fallback_dispatches, 1);
      return 3;
    }
    default:
      return 1;
  }
}
static void fallback_destroy(void* user, void* raw) {
  (void)user;
  const struct FallbackState* state = raw;
  if (state->backend >= 1 && state->backend <= 2 && state->mode < 16)
    atomic_fetch_add(
        &fallback_destroys[state->enabled][state->backend - 1][state->mode], 1);
}
#define FALLBACK_OP(KEY, FLAG, USER)                        \
  {.struct_size = sizeof(ps_result_operation_v5),           \
   .key = KEY,                                              \
   .key_size = sizeof(KEY) - 1,                             \
   .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_GPU_V5 | \
            PS_RESULT_FLAG_DETERMINISTIC_V5 |               \
            PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5 | FLAG,      \
   .outputs = &number_output,                               \
   .output_count = 1,                                       \
   .parameters = &mode,                                     \
   .parameter_count = 1,                                    \
   .state_bytes = sizeof(struct FallbackState),             \
   .workspace_bytes = 16,                                   \
   .maximum_stages = 2,                                     \
   .user_data = USER,                                       \
   .start = fallback_start,                                 \
   .poll = fallback_poll,                                   \
   .destroy = fallback_destroy}
static ps_result_operation_v5 operations[] = {
    COPY_OP("fixture.result.copy", &regional_output, 0),
    COPY_OP("fixture.result.whole", &whole_output, 0),
    COPY_OP("fixture.result.tiles", &whole_output, 1),
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.scalar",
     .key_size = 21,
     .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .outputs = &scalar_output,
     .output_count = 1,
     .parameters = scalar_parameters,
     .parameter_count = 2,
     .workspace_bytes = 65536,
     .maximum_stages = 2,
     .start = start,
     .poll = scalar,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.mixed",
     .key_size = 20,
     .parameters = &mode,
     .parameter_count = 1,
     .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .inputs = inputs,
     .input_count = 2,
     .outputs = mixed_outputs,
     .output_count = 3,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 32,
     .resolve_metadata = resolve,
     .start = start,
     .poll = mixed,
     .destroy = destroy_state},

    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.native",
     .key_size = 21,
     .parameters = &mode,
     .parameter_count = 1,
     .flags = PS_RESULT_FLAG_GPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .outputs = &gpu_output,
     .output_count = 1,
     .workspace_bytes = 65536,
     .maximum_stages = 2,
     .start = start,
     .poll = native,
     .destroy = destroy_state},

    COPY_OP("fixture.result.view", &regional_output, 0),
    COPY_OP("fixture.result.view_whole", &whole_output, 0),
    COPY_OP("fixture.result.view_gpu", &whole_output, 0),
    COPY_OP("fixture.result.member_fixed", &regional_output, 0),
    COPY_OP("fixture.result.member_owned", &regional_output, 0),
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.reshape",
     .key_size = 22,
     .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .inputs = &reshape_input,
     .input_count = 1,
     .outputs = &reshape_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 16,
     .maximum_stages = 2,
     .start = start,
     .poll = reshape_poll,
     .destroy = destroy_state},
    COPY_OP("fixture.result.sole", &regional_output, 0),
    COPY_OP("fixture.result.sole_fixed", &regional_output, 0),
    COPY_OP("fixture.result.bundle", &regional_output, 0),
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.prefix",
     .key_size = 21,
     .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .inputs = &prefix_input,
     .input_count = 1,
     .outputs = &prefix_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 16,
     .maximum_stages = 2,
     .parameters = &mode,
     .parameter_count = 1,
     .start = start,
     .poll = prefix_poll,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.neighborhood",
     .key_size = 27,
     .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .inputs = &prefix_input,
     .input_count = 1,
     .outputs = &prefix_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 16,
     .maximum_stages = 2,
     .parameters = &mode,
     .parameter_count = 1,
     .start = start,
     .poll = neighborhood_poll,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.cartesian",
     .key_size = 24,
     .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .inputs = &prefix_input,
     .input_count = 1,
     .outputs = &prefix_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 16,
     .maximum_stages = 2,
     .parameters = &mode,
     .parameter_count = 1,
     .start = start,
     .poll = cartesian_poll,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.native_input",
     .key_size = 27,
     .flags = PS_RESULT_FLAG_GPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .inputs = &native_number_input,
     .input_count = 1,
     .outputs = &number_output,
     .output_count = 1,
     .parameters = &mode,
     .parameter_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 2,
     .start = start,
     .poll = native_input,
     .destroy = destroy_state},
    FALLBACK_OP("fixture.result.fallback", PS_RESULT_FLAG_CPU_FALLBACK_V5,
                &fallback_enabled),
    FALLBACK_OP("fixture.result.no_fallback", 0, &fallback_disabled),
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.impure",
     .key_size = sizeof("fixture.result.impure") - 1,
     .flags = PS_RESULT_FLAG_CPU_V5,
     .shared_stages = &impure_stage,
     .shared_stage_count = 1,
     .outputs = &impure_output,
     .output_count = 1,
     .state_bytes = sizeof(float),
     .workspace_bytes = 16,
     .maximum_stages = 2,
     .start = impure_start,
     .poll = impure_poll,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.bounded_output",
     .key_size = sizeof("fixture.result.bounded_output") - 1,
     .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .outputs = bounded_output_outputs,
     .output_count = 2,
     .state_bytes = 1,
     .workspace_bytes = 8,
     .maximum_stages = 1,
     .start = bounded_single_start,
     .poll = bounded_single_poll,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v5),
     .key = "fixture.result.sparse_wide",
     .key_size = sizeof("fixture.result.sparse_wide") - 1,
     .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
     .outputs = &sparse_wide_output,
     .output_count = 1,
     .state_bytes = 1,
     .maximum_stages = 1,
     .start = sparse_wide_start,
     .poll = sparse_wide_poll,
     .destroy = destroy_state}};
static const ps_result_extension_identity_v5 diagnostic_id = {
    sizeof(ps_result_extension_identity_v5),
    1,
    "fixture.work",
    12,
    "counters/1",
    10};
static int diagnostic_validate(void* u, const uint8_t* bytes, uint32_t size) {
  (void)u;
  uint64_t n[3];
  if (size != 24)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  memcpy(n, bytes, 24);
  return n[1] <= n[0] && n[2] <= n[0] - n[1]
             ? 0
             : PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
}
static int diagnostic_merge(void* u, const uint8_t* a, const uint8_t* b,
                            uint8_t* out, uint32_t size) {
  (void)u;
  uint64_t x[3], y[3];
  if (size != 24)
    return 1;
  memcpy(x, a, 24);
  memcpy(y, b, 24);
  for (unsigned i = 0; i < 3; ++i) {
    if (y[i] > UINT64_MAX - x[i])
      return PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V5;
    x[i] += y[i];
  }
  memcpy(out, x, 24);
  return 0;
}
static const ps_result_diagnostic_definition_v5 diagnostic = {
    sizeof(ps_result_diagnostic_definition_v5),
    {sizeof(ps_result_extension_identity_v5), 1, "fixture.work", 12,
     "counters/1", 10},
    24,
    0,
    NULL,
    diagnostic_validate,
    diagnostic_merge};
static ps_result_operation_plugin_api_v5 api = {
    sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_5,
    operations,  sizeof(operations) / sizeof(operations[0]),
    NULL,        destroy_plugin,
    NULL,        0,
    &diagnostic, 1};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v5*
ps_result_operation_plugin_get_api_v5(void) {
  for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
    operations[i].diagnostics = &diagnostic_id;
    operations[i].diagnostic_count = 1;
  }
  operations[12].inputs = sole_inputs;
  operations[12].user_data = &sole_marker;
  operations[13].inputs = sole_fixed_inputs;
  operations[13].resolve_metadata = NULL;
  operations[13].user_data = &sole_marker;
  operations[14].inputs = bundle_inputs;
  operations[14].resolve_metadata = NULL;
  operations[9].inputs = operations[10].inputs = member_inputs;
  operations[9].resolve_metadata = NULL;
  operations[10].user_data = &member_marker;
  operations[6].poll = operations[7].poll = operations[8].poll = zero_copy;
  operations[8].flags = PS_RESULT_FLAG_GPU_V5 |
                        PS_RESULT_FLAG_DETERMINISTIC_V5 |
                        PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5;
#ifdef PS_RESULT_BAD_TABLES
  static ps_result_operation_plugin_api_v5 bad_api;
  static ps_result_operation_v5
      bad_operations[sizeof(operations) / sizeof(operations[0])];
  static ps_result_output_v5 invalid;
  bad_api = api;
  memcpy(bad_operations, operations, sizeof(operations));
  bad_api.operations = bad_operations;
  invalid = regional_output;
  switch (ps_test_bad_case) {
    case 1:
      bad_api.struct_size -= 4;
      break;
    case 2:
      bad_api.abi_version = PS_RESULT_OPERATION_ABI_VERSION_5 + 1;
      break;
    case 3:
      bad_operations[0].inputs = NULL;
      break;
    case 4:
      bad_operations[1].struct_size -= 4;
      break;
    case 5:
      bad_operations[0].outputs = &gpu_output;
      bad_operations[0].cpu_staged_tiles = 99;
      break;
    case 6:
      invalid = scalar_output;
      invalid.port.element_type_mask = 0x8000;
      bad_operations[0].outputs = &invalid;
      break;
    case 7:
      bad_operations[19].flags &= ~PS_RESULT_FLAG_GPU_V5;
      break;
    case 8:
      bad_operations[19].flags &= ~PS_RESULT_FLAG_CPU_V5;
      break;
    case 9:
      invalid.flags |= 8;
      bad_operations[0].outputs = &invalid;
      break;
    case 10:
      invalid.flags = 0;
      invalid.maximum_output_payload_bytes = 32;
      bad_operations[0].outputs = &invalid;
      break;
    case 11:
      invalid.struct_size = offsetof(ps_result_output_v5, flags);
      bad_operations[0].outputs = &invalid;
      break;
  }
  return &bad_api;
#endif
  return &api;
}
