#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"

static const ps_result_tensor_spec_v2 image = {
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
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 schema = {.struct_size = sizeof(schema),
                                           .id = "example.image",
                                           .id_size = 13,
                                           .version = 1,
                                           .publication = 1,
                                           .tensors = &image,
                                           .tensor_count = 1};
#define TENSOR_SCHEMA(NAME, ID, DTYPE, COUNT, FACETS, FACET_COUNT) \
  static const ps_result_tensor_spec_v2 NAME##_tensor = {          \
      .struct_size = sizeof(ps_result_tensor_spec_v2),             \
      .key = "samples",                                            \
      .key_size = 7,                                               \
      .element_type = DTYPE,                                       \
      .rank = 1,                                                   \
      .shape = {COUNT},                                            \
      .channel_axis = PS_RESULT_NO_CHANNEL_V2,                     \
      .facets = FACETS,                                            \
      .facet_count = FACET_COUNT};                                 \
  static const ps_result_schema_v2 NAME = {                        \
      .struct_size = sizeof(ps_result_schema_v2),                  \
      .id = ID,                                                    \
      .id_size = sizeof(ID) - 1,                                   \
      .version = 1,                                                \
      .publication = 1,                                            \
      .tensors = &NAME##_tensor,                                   \
      .tensor_count = 1}
TENSOR_SCHEMA(control_schema, "fixture.control", 2, 1, NULL, 0);
TENSOR_SCHEMA(number_schema, "fixture.number", 4, 1, NULL, 0);
TENSOR_SCHEMA(sequence_schema, "fixture.sequence", 4, 128, NULL, 0);
static const ps_result_port_v2 inputs[] = {
    {.struct_size = sizeof(ps_result_port_v2), .kind = 6, .schema = &schema},
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = &control_schema}};
static char member_key[] = "pixels";
static int member_marker;
static const ps_result_port_v2 member_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = 6,
     .schema = &schema,
     .element_type = 4,
     .rank = 2,
     .tensor_key = member_key,
     .tensor_key_size = 6},
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = &control_schema}};
static int sole_marker;
static const ps_result_port_v2 sole_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = 6,
     .rank = 2,
     .element_type_mask = 8},
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = &control_schema}};
static const ps_result_port_v2 sole_fixed_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = 6,
     .schema = &schema,
     .rank = 2,
     .element_type_mask = 8},
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = &control_schema}};
static const ps_result_tensor_spec_v2 bundle_tensors[] = {
    {.struct_size = sizeof(ps_result_tensor_spec_v2),
     .key = "first",
     .key_size = 5,
     .element_type = 4,
     .rank = 1,
     .shape = {1}},
    {.struct_size = sizeof(ps_result_tensor_spec_v2),
     .key = "second",
     .key_size = 6,
     .element_type = 3,
     .rank = 1,
     .shape = {1}}};
static const ps_result_schema_v2 bundle_schema = {
    .struct_size = sizeof(ps_result_schema_v2),
    .id = "test.bundle",
    .id_size = 11,
    .version = 1,
    .publication = 1,
    .tensors = bundle_tensors,
    .tensor_count = 2};
static const ps_result_port_v2 bundle_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = 6,
     .schema = &bundle_schema},
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = &control_schema}};
static int sole_predicate(const ps_result_port_v2* port) {
  return port->tensor_key == NULL && port->tensor_key_size == 0 &&
         port->element_type == 0 && port->element_type_mask == 8 &&
         port->rank == 2 && port->schema != NULL &&
         port->schema->tensor_count == 1;
}
PS_RESULT_EXPORT void fixture_mutate_member_key(int mutate) {
  memcpy(member_key, mutate ? "broken" : "pixels", 6);
}
static const ps_result_output_v2 regional_output = {
    .struct_size = sizeof(ps_result_output_v2),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = 6,
             .schema = &schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_REGIONAL_V2};
static const ps_result_output_v2 whole_output = {
    .struct_size = sizeof(ps_result_output_v2),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = 6,
             .schema = &schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_WHOLE_V2};
static const ps_result_output_v2 scalar_output = {
    .struct_size = sizeof(ps_result_output_v2),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &sequence_schema},
    .input_count = 0,
    .execution = PS_RESULT_REGIONAL_V2};
static const ps_result_parameter_descriptor_v2 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V2, 0, 1, 0, 17};
static const ps_result_parameter_descriptor_v2 scalar_parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v2), "scale", 5,
     PS_RESULT_PARAMETER_FLOAT64_V2, 0, 1, 0, 4},
    {sizeof(ps_result_parameter_descriptor_v2), "offset", 6,
     PS_RESULT_PARAMETER_INT64_V2, 0, 1, -2, 2}};
struct State {
  unsigned stage;
  int64_t shift, mode;
  uint64_t handle, window_handle, other_window;
  ps_result_tensor_window_v2 window;
  ps_result_services_v2 saved;
};
struct Work {
  const ps_result_services_v2* services;
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
static int tile(void* raw, const ps_cpu_tile_v1* box) {
  return range(raw, box->begin[0], box->end[0], box->slot);
}
static int start(void* user, void* state, const ps_result_query_v2* query,
                 const ps_result_services_v2* services) {
  if (user == &sole_marker && !sole_predicate(query->inputs))
    return 6;
  if (user == &member_marker &&
      (query->inputs[0].tensor_key_size != 6 ||
       memcmp(query->inputs[0].tensor_key, "pixels", 6)))
    return 6;
  if (query->parameter_count && (query->parameters[0].int64_value == 5 ||
                                 query->parameters[0].int64_value == 6))
    ((struct State*)state)->saved = *services;
  return services->abi_version == PS_RESULT_OPERATION_ABI_VERSION_2 &&
                 services->struct_size == sizeof(*services)
             ? 0
             : 6;
}
static ps_result_region_v2 unit(const uint64_t at[4]) {
  ps_result_region_v2 r = {0};
  r.struct_size = sizeof(r);
  r.rank = 4;
  for (unsigned i = 0; i < 4; ++i) {
    r.offset[i] = at[i];
    r.extent[i] = 1;
  }
  return r;
}
static uint64_t flatten(const uint64_t at[4],
                        const ps_result_tensor_spec_v2* image) {
  return ((at[0] * image->batch_shape[1] + at[1]) * image->shape[0] + at[2]) *
             image->shape[1] +
         at[3];
}
static int resolve(void* user, const ps_result_port_v2* inputs, uint32_t count,
                   const ps_result_parameter_value_v2* parameters,
                   uint32_t parameter_count,
                   const ps_result_port_v2* prototypes, uint32_t output_count,
                   const ps_result_metadata_sink_v2* sink) {
  if (user == &sole_marker && !sole_predicate(inputs))
    return 6;
  if (user == &member_marker && (inputs[0].tensor_key_size != 6 ||
                                 memcmp(inputs[0].tensor_key, "pixels", 6)))
    return 6;
  if (parameter_count && parameters[0].int64_value == 17) {
    ps_result_port_v2 output = prototypes[0];
    output.minimum = -0.0f;
    (void)sink->set_output(sink->context, 0, &output);
    return 0;
  }
  if (parameter_count && parameters[0].int64_value == 4) {
    _Alignas(8) uint8_t raw[sizeof(ps_result_port_v2) + 8];
    memcpy(raw + 1, inputs, sizeof(ps_result_port_v2));
    (void)sink->set_output(sink->context, 0,
                           (const ps_result_port_v2*)(raw + 1));
    return 0;
  }
  if (parameter_count && parameters[0].int64_value == 9) {
    _Alignas(8) uint32_t short_record = 4;
    (void)sink->set_output(sink->context, 0,
                           (const ps_result_port_v2*)&short_record);
    return 0;
  }
  if (count != 2 || !output_count || inputs[0].kind != PS_RESULT_OBJECT_V2 ||
      !inputs[0].schema || inputs[0].schema->tensor_count != 1 ||
      inputs[0].schema->tensors[0].rank != 2 ||
      inputs[0].schema->tensors[0].element_type != 4 ||
      inputs[0].schema->tensors[0].height_axis != 0 ||
      inputs[0].schema->tensors[0].width_axis != 1 ||
      inputs[0].schema->tensors[0].channel_axis != PS_RESULT_NO_CHANNEL_V2)
    return 6;
  // Callback-local nested records exercise synchronous host copying.
  ps_result_tensor_spec_v2 image = inputs[0].schema->tensors[0];
  ps_result_schema_v2 schema = *inputs[0].schema;
  schema.tensors = &image;
  ps_result_port_v2 output = prototypes[0];
  output.schema = &schema;
  if (sink->set_output(sink->context, 0, &output))
    return 1;
  for (uint32_t i = 1; i < output_count; ++i)
    if (sink->set_output(sink->context, i, prototypes + i))
      return 1;
  return 0;
}
static int copy(void* user, void* raw, const ps_result_query_v2* q,
                const ps_result_services_v2* s) {
  ps_result_query_v2 resolved = *q;
  const ps_result_tensor_spec_v2* spec = q->output->port.schema->tensors;
  ps_result_region_v2 all = {
      .struct_size = sizeof(all),
      .rank = 4,
      .extent = {spec->batch_shape[0], spec->batch_shape[1], spec->shape[0],
                 spec->shape[1]}};
  if (!q->requested_kind) {
    resolved.requested = &all;
    resolved.requested_count = 1;
    q = &resolved;
  }
  struct State* state = (struct State*)raw;
  uint64_t zero = 0;
  (void)user;
  if (q->tensor_slot)
    return 6;
  if (q->parameter_count)
    state->mode = q->parameters[0].int64_value;
  ps_result_region_v2 control = {.struct_size = sizeof(control),
                                 .rank = 1,
                                 .extent = {1}};
  if (state->stage == 0) {
    if (state->mode == 5)
      (void)state->saved.consume_work(state->saved.context, 1);
    if (state->mode == 6 && state->saved.cpu_parallel)
      (void)state->saved.cpu_parallel->run(state->saved.cpu_parallel->context,
                                           0, 1, 0, range, NULL);
    state->stage = 1;
    return s->need_tensor(s->context, 1, 0, 6, &control, 1) ? 1
                                                            : PS_RESULT_NEED_V2;
  }
  if (state->stage == 1) {
    if (s->read_tensor(s->context, 1, 0, &zero, 1, &state->shift, 8) ||
        state->shift < 0 || state->shift > 1)
      return 6;
    ps_result_region_v2 needed[64];
    uint32_t count = 0;
    for (uint32_t r = 0; r < q->requested_count; ++r) {
      const ps_result_region_v2* box = q->requested + r;
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
      ps_result_relation_row_v2 descriptor = {
          0, 1, 6, PS_RESULT_TARGET_TENSOR_V2, 0, 0, 1};
      if (s->begin_result(s->context) ||
          s->bind_descriptor(s->context, &descriptor, 1, 1) ||
          s->publish_result(s->context, 1))
        return 1;
      return PS_RESULT_PUBLISH_V2;
    }
    return s->need_tensor(s->context, 0, 0, 1, needed, count)
               ? 1
               : PS_RESULT_NEED_V2;
  }
  if (state->stage == 2) {
    if (s->retain_tensor(s->context, 0, 0, &state->handle))
      return 1;
    state->stage = 3;
    return s->need_tensor(s->context, 1, 0, 6, &control, 1) ? 1
                                                            : PS_RESULT_NEED_V2;
  }
  if (s->begin_result(s->context) || s->bind_descriptor(s->context, NULL, 0, 1))
    return 1;
  for (uint32_t r = 0; r < q->requested_count; ++r) {
    const ps_result_region_v2* box = q->requested + r;
    float pixels[64];
    ps_result_relation_row_v2 rows[128];
    uint32_t count = 0;
    for (uint64_t n = box->offset[0]; n < box->offset[0] + box->extent[0]; ++n)
      for (uint64_t l = box->offset[1]; l < box->offset[1] + box->extent[1];
           ++l)
        for (uint64_t y = box->offset[2]; y < box->offset[2] + box->extent[2];
             ++y)
          for (uint64_t x = box->offset[3]; x < box->offset[3] + box->extent[3];
               ++x) {
            uint64_t out[] = {n, l, y, x},
                     in[] = {n, l, y,
                             (x + (uint64_t)state->shift) % spec->shape[1]};
            if (count == 64 ||
                s->read_retained_tensor(s->context, state->handle, in, 4,
                                        pixels + count, 4))
              return 1;
            rows[2 * count] = (ps_result_relation_row_v2){
                flatten(out, spec), 0, 1, 2, 0, flatten(in, spec), 1};
            rows[2 * count + 1] = (ps_result_relation_row_v2){
                flatten(out, spec), 1, 6, PS_RESULT_TARGET_TENSOR_V2, 0, 0, 1};
            ++count;
          }
    struct Work work = {s, pixels};
    if (state->mode == 10 && s->cpu_parallel)
      (void)s->cpu_parallel->run(s->cpu_parallel->context, 1, 1, 0,
                                 forbidden_range, &work);
    if (state->mode == 11 && s->cpu_parallel)
      (void)s->cpu_parallel->run(s->cpu_parallel->context, 1, 0, 0, range,
                                 &work);
    if (s->cpu_parallel && s->cpu_parallel->run(s->cpu_parallel->context, count,
                                                1, 0, range, &work))
      return 1;
    if (s->cpu_tiles) {
      ps_cpu_tile_stage_v1 stage = {sizeof(stage), {count, 1, 1}, {1, 1, 1}, 0};
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
    if (state->mode == 7 && count)
      rows[0].target = 99;
    if (state->mode == 8 && count)
      rows[0].target = 0;
    if (s->publish_tensor(s->context, 0, box, (const uint8_t*)pixels, count * 4,
                          rows, count * 2, 1, 15))
      return 1;
  }
  if (state->mode != 2 && s->release_tensor(s->context, state->handle))
    return 1;
  if (s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
struct WindowWork {
  ps_result_tensor_window_v2 window;
  uint64_t at[8];
};
static int window_worker(void* raw, uint64_t begin, uint64_t end,
                         uint32_t slot) {
  struct WindowWork* work = (struct WindowWork*)raw;
  (void)begin;
  (void)end;
  (void)slot;
  ps_result_tensor_rectangle_v2 run = {.struct_size = sizeof(run)};
  return work->window.rectangle(work->window.context, work->at,
                                work->window.region.rank, &run);
}
static int zero_copy(void* user, void* raw, const ps_result_query_v2* query,
                     const ps_result_services_v2* services) {
  struct State* state = (struct State*)raw;
  (void)user;
  const ps_result_tensor_spec_v2* image = query->inputs[0].schema->tensors;
  ps_result_region_v2 all = {
      .struct_size = sizeof(all),
      .rank = 4,
      .extent = {image->batch_shape[0], image->batch_shape[1], image->shape[0],
                 image->shape[1]}};
  ps_result_region_v2 control = {.struct_size = sizeof(control),
                                 .rank = 1,
                                 .extent = {1}};
  if (query->parameter_count)
    state->mode = query->parameters[0].int64_value;
  if (!state->stage) {
    state->stage = 1;
    return services->need_tensor(
               services->context, 0, 0,
               state->mode == 10 || state->mode == 11 ? 8 : 1,
               query->requested_kind ? query->requested : &all,
               query->requested_kind ? query->requested_count : 1)
               ? 1
               : PS_RESULT_NEED_V2;
  }
  if (state->stage == 1) {
    state->stage = 2;
    if (services->retain_tensor(services->context, 0, 0, &state->handle))
      return 1;
    if (state->mode != 11 &&
        (!query->requested_kind || query->requested_count)) {
      ps_result_region_v2 first =
          query->requested_kind ? query->requested[0] : all;
      first.extent[0] = first.extent[1] = 1;
      state->window.struct_size = sizeof(state->window);
      if (services->acquire_tensor_window(services->context, 0, 0, &first,
                                          &state->window,
                                          &state->window_handle) ||
          services->retain_window(services->context, state->window_handle,
                                  &state->other_window))
        return 1;
    }
    return services->need_tensor(services->context, 1, 0, 2, &control, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  }
  if (state->window_handle) {
    ps_result_tensor_row_v2 row = {.struct_size = sizeof(row)};
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
  if (state->mode == 3)
    return 2;
  if (services->begin_result(services->context))
    return 1;
  ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
  if (services->bind_descriptor(services->context, &descriptor, 1,
                                PS_RESULT_EXACT_V2))
    return 1;
  ps_result_mapped_axis_v2 axes[4] = {{0, 0, 1, 1},
                                      {1, 0, 1, 1},
                                      {2, 0, 1, 1},
                                      {3, 0, 1, 1}};
  ps_result_tensor_transform_v2 transform = {.struct_size = sizeof(transform),
                                             .source_rank = 4,
                                             .axes = axes};
  uint64_t relation = 0;
  if (services->make_mapping(services->context, 0, 0,
                             PS_RESULT_TARGET_TENSOR_V2, 0, 1, &all, axes, 4,
                             &relation))
    return 1;
  if (state->mode == 14)
    transform.source_rank = 3;
  if (state->mode == 15)
    axes[0].extent = 2;
  if (state->mode == 16)
    axes[3].source_origin = image->shape[1];
  const ps_result_region_v2* requested =
      query->requested_kind ? query->requested : &all;
  const uint32_t count = query->requested_kind ? query->requested_count : 1;
  for (uint32_t box = 0; box < count; ++box)
    for (uint64_t frame = requested[box].offset[0];
         frame < requested[box].offset[0] + requested[box].extent[0]; ++frame)
      for (uint64_t layer = requested[box].offset[1];
           layer < requested[box].offset[1] + requested[box].extent[1];
           ++layer) {
        ps_result_region_v2 region = requested[box];
        region.offset[0] = frame;
        region.offset[1] = layer;
        region.extent[0] = region.extent[1] = 1;
        ps_result_tensor_window_v2 window = {.struct_size = sizeof(window)};
        uint64_t handle = 0;
        if (services->acquire_retained_window(services->context, state->handle,
                                              &region, &window, &handle) ||
            services->publish_tensor_view(services->context, 0, &region,
                                          &handle, 1,
                                          state->mode >= 13 ? &transform : NULL,
                                          relation, PS_RESULT_FINAL_V2) ||
            services->release_window(services->context, handle))
          return 1;
      }
  if (state->other_window &&
      services->release_window(services->context, state->other_window))
    return 1;
  ps_result_numeric_report_v2 numeric = {.struct_size = sizeof(numeric),
                                         .profile = 1};
  memcpy(numeric.implementation, "fixture-view/abi2", 18);
  for (uint32_t box = 0; box < count; ++box) {
    uint64_t elements = 1;
    for (uint32_t axis = 0; axis < requested[box].rank; ++axis)
      elements *= requested[box].extent[axis];
    numeric.view_elements += elements;
  }
  if (state->mode == 12)
    numeric.strict_fallbacks = 1;
  (void)services->report_numeric(services->context, &numeric);
  if (state->mode == 8 && state->window_handle) {
    uint64_t at[8];
    memcpy(at, state->window.region.offset, sizeof(at));
    ps_result_tensor_row_v2 row = {.struct_size = sizeof(row)};
    (void)state->window.row(state->window.context, at, 4, &row);
  }
  if (services->release_relation(services->context, relation) ||
      services->release_tensor(services->context, state->handle) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static int scalar(void* user, void* state, const ps_result_query_v2* q,
                  const ps_result_services_v2* s) {
  (void)user;
  (void)state;
  int64_t offset = 0;
  double scale = 1;
  for (uint32_t i = 0; i < q->parameter_count; ++i) {
    const ps_result_parameter_value_v2* parameter = q->parameters + i;
    if (parameter->key_size == 6 && !memcmp(parameter->key, "offset", 6))
      offset = parameter->int64_value;
    else if (parameter->key_size == 5 && !memcmp(parameter->key, "scale", 5))
      scale = parameter->float64_value;
    else
      return 6;
  }
  ps_result_region_v2 all = {
      .struct_size = sizeof(all),
      .rank = 1,
      .extent = {q->output->port.schema->tensors[0].shape[0]}};
  const uint32_t count = q->requested_kind ? q->requested_count : 1;
  const ps_result_region_v2* boxes = q->requested_kind ? q->requested : &all;
  if (s->begin_result(s->context) || s->bind_descriptor(s->context, NULL, 0, 1))
    return 1;
  for (uint32_t i = 0; i < count; ++i) {
    float samples[128];
    const ps_result_region_v2* box = boxes + i;
    if (box->rank != 1 || box->extent[0] > 128)
      return 6;
    for (uint64_t n = 0; n < box->extent[0]; ++n)
      samples[n] = (float)(((int64_t)(box->offset[0] + n) + offset) * scale);
    if (s->publish_tensor(s->context, 0, box, (const uint8_t*)samples,
                          box->extent[0] * 4, NULL, 0, 1, 15))
      return 1;
  }
  return s->publish_result(s->context, 1) ? 1 : PS_RESULT_PUBLISH_V2;
}

static const ps_result_field_spec_v2 field = {
    .struct_size = sizeof(field),
    .key = "rows",
    .key_size = 4,
    .element_type = 2,
    .rows = {.kind = 5, .divisor = 1}};
static const ps_result_schema_v2 row_schema = {
    .struct_size = sizeof(row_schema),
    .id = "fixture.rows",
    .id_size = 12,
    .version = 1,
    .publication = 3,
    .fields = &field,
    .field_count = 1};
static const ps_result_output_v2 row_output = {
    .struct_size = sizeof(row_output),
    .key = "rows",
    .key_size = 4,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = 6,
             .schema = &row_schema},
    .input_count = UINT32_MAX,
    .execution = 2};
static int prefix(void* user, void* raw, const ps_result_query_v2* q,
                  const ps_result_services_v2* s) {
  struct State* state = (struct State*)raw;
  (void)user;
  uint64_t zero = 0;
  uint32_t control = q->input_count == 2 ? 1 : 0;
  ps_result_region_v2 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (!state->stage) {
    state->stage = 1;
    return s->need_tensor(s->context, control, 0, 6, &one, 1) ? 1 : 100;
  }
  if (state->stage == 1) {
    if (s->read_tensor(s->context, control, 0, &zero, 1, &state->shift, 8) ||
        state->shift < 0 || state->shift > 4)
      return 6;
    ps_result_relation_row_v2 basis = {
        0, control, 6, PS_RESULT_TARGET_TENSOR_V2, 0, 0, 1};
    if (s->begin_result(s->context) ||
        s->bind_descriptor(s->context, &basis, 1, 1))
      return 1;
    if (!state->shift) {
      if (s->publish_field(s->context, 0, 0, NULL, 0, 1, 15) ||
          s->publish_result(s->context, 1))
        return 1;
      return 101;
    }
  }
  if (state->stage == 1 || state->stage == 3) {
    int64_t number = (int64_t)(++state->handle);
    state->stage = 2;
    return s->append_field(s->context, 0, 1, (const uint8_t*)&number, 8) ? 1
                                                                         : 100;
  }
  if (s->publish_field(s->context, 0, state->handle, NULL, 0, 1, 15) ||
      s->publish_result(s->context, state->handle == (uint64_t)state->shift))
    return 1;
  state->stage = 3;
  return 101;
}
static const ps_result_port_v2 row_input = {.struct_size = sizeof(row_input),
                                            .kind = 6,
                                            .schema = &row_schema};
static const ps_result_output_v2 number_output = {
    .struct_size = sizeof(number_output),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &number_schema},
    .input_count = UINT32_MAX,
    .execution = 1};
static int sum_rows(void* user, void* raw, const ps_result_query_v2* q,
                    const ps_result_services_v2* s) {
  struct State* state = (struct State*)raw;
  (void)q;
  if (!state->stage) {
    state->stage = 1;
    return s->need_result(s->context, 0, 0, user == NULL, 1) ? 1 : 100;
  }
  if (state->stage == 1) {
    ps_result_descriptor_v2 facts = {.struct_size = sizeof(facts)};
    if (s->result_descriptor(s->context, 0, &facts))
      return 1;
    state->handle = facts.rows[0];
    state->stage = 2;
    if (state->handle)
      return s->need_field_read(s->context, 0, 0, 0, state->handle) ? 1 : 100;
  }
  float sum = 0;
  for (uint64_t i = 0; i < state->handle; ++i) {
    int64_t number = 0;
    if (s->read_io(s->context, 0, i * 8, (uint8_t*)&number, 8))
      return 1;
    sum += (float)number;
  }
  ps_result_relation_row_v2 basis = {0, 0, 8, 3, 0, 0, 1},
                            data = {0, 0, 1, 1, 0, 0, state->handle};
  ps_result_region_v2 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (s->begin_result(s->context) ||
      s->bind_descriptor(s->context, &basis, 1, 1) ||
      s->publish_tensor(s->context, 0, &one, (const uint8_t*)&sum, 4,
                        state->handle ? &data : NULL, state->handle ? 1 : 0, 1,
                        15) ||
      s->publish_result(s->context, 1))
    return 1;
  return 101;
}
static const uint8_t lut_payload[] = {
    3,   0,   0,   0,   108, 117, 116, 1,   0,   0,   0,   5,   0,   0,   0,
    116, 97,  98,  108, 101, 5,   0,   0,   0,   118, 97,  108, 117, 101, 13,
    0,   0,   0,   100, 105, 109, 101, 110, 115, 105, 111, 110, 108, 101, 115,
    115, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   13,  0,   0,   0,
    100, 105, 109, 101, 110, 115, 105, 111, 110, 108, 101, 115, 115, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   240, 63,  5,   0,   0,   0,
    105, 110, 100, 101, 120, 0,   0,   0,   0};
static const uint8_t scalar_payload[] = {
    6,   0,   0,   0,   115, 99,  97,  108, 97,  114, 1,   0,   0,   0,   6,
    0,   0,   0,   110, 117, 109, 98,  101, 114, 5,   0,   0,   0,   118, 97,
    108, 117, 101, 13,  0,   0,   0,   100, 105, 109, 101, 110, 115, 105, 111,
    110, 108, 101, 115, 115, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    13,  0,   0,   0,   100, 105, 109, 101, 110, 115, 105, 111, 110, 108, 101,
    115, 115, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0};
static const ps_result_facet_view_v2 lut_facet = {
    sizeof(lut_facet), "photospider.semantic", 20, 1,
    lut_payload,       sizeof(lut_payload)};
static const ps_result_facet_view_v2 scalar_facet = {
    sizeof(scalar_facet), "photospider.semantic", 20, 1,
    scalar_payload,       sizeof(scalar_payload)};
TENSOR_SCHEMA(lut_schema, "fixture.lut", 4, 4, &lut_facet, 1);
TENSOR_SCHEMA(typed_number_schema, "fixture.typed_number", 4, 1, &scalar_facet,
              1);
static const ps_result_port_v2 typed_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = &lut_schema,
     .element_type = 4,
     .rank = 1,
     .requires_semantics = 1,
     .semantic_kind = 8},
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = &typed_number_schema,
     .element_type = 4,
     .rank = 1,
     .scalar_bounds = 1,
     .minimum = 0,
     .maximum = 1}};
static const ps_result_output_v2 typed_output = {
    .struct_size = sizeof(typed_output),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &typed_number_schema,
             .element_type = 4,
             .rank = 1,
             .requires_semantics = 1,
             .semantic_kind = 1},
    .input_count = UINT32_MAX,
    .execution = 1};
static int typed(void* user, void* raw, const ps_result_query_v2* q,
                 const ps_result_services_v2* s) {
  (void)user;
  struct State* state = (struct State*)raw;
  if (q->inputs[0].kind != PS_RESULT_OBJECT_V2 ||
      q->inputs[1].kind != PS_RESULT_OBJECT_V2 ||
      q->output->port.kind != PS_RESULT_OBJECT_V2)
    return 6;
  ps_result_region_v2 lut = {.struct_size = sizeof(lut),
                             .rank = 1,
                             .extent = {4}},
                      one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (!state->stage) {
    state->stage = 1;
    return s->need_tensor(s->context, 0, 0, 1, &lut, 1) ||
                   s->need_tensor(s->context, 1, 0, 5, &one, 1)
               ? 1
               : 100;
  }
  float sum = 0, factor = 0;
  uint64_t at = 0;
  if (s->read_tensor(s->context, 1, 0, &at, 1, &factor, 4))
    return 1;
  for (at = 0; at < 4; ++at) {
    float sample = 0;
    if (s->read_tensor(s->context, 0, 0, &at, 1, &sample, 4))
      return 1;
    sum += sample;
  }
  sum *= factor;
  ps_result_relation_row_v2 rows[] = {
      {0, 0, 1, PS_RESULT_TARGET_TENSOR_V2, 0, 0, 4},
      {0, 1, 5, PS_RESULT_TARGET_TENSOR_V2, 0, 0, 1}};
  if (s->begin_result(s->context) ||
      s->bind_descriptor(s->context, NULL, 0, 1) ||
      s->publish_tensor(s->context, 0, &one, (const uint8_t*)&sum, 4, rows, 2,
                        1, 15) ||
      s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static const uint32_t control_port = 1;
static const ps_result_output_v2 mixed_outputs[] = {
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "image",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = 6,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = 2},
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "number",
     .key_size = 6,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &number_schema},
     .input_count = 0,
     .execution = 1},
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "lut",
     .key_size = 3,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &lut_schema,
              .requires_semantics = 1,
              .semantic_kind = 8},
     .input_count = 0,
     .execution = 1},
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "metadata",
     .key_size = 8,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = 6,
              .schema = &row_schema},
     .input_indices = &control_port,
     .input_count = 1,
     .execution = 2}};
static int mixed(void* user, void* raw, const ps_result_query_v2* q,
                 const ps_result_services_v2* s) {
  if (q->output_index == 0)
    return copy(user, raw, q, s);
  if (q->output_index == 3)
    return prefix(user, raw, q, s);
  if (q->output_index == 2)
    return scalar(user, raw, q, s);
  if (q->parameter_count && q->parameters[0].int64_value == 12) {
    ps_result_region_v2 one = {.struct_size = sizeof(one),
                               .rank = 1,
                               .extent = {1}};
    ps_result_relation_row_v2 escaped = {0, 0, 1, 2, 0, 0, 1};
    float value = 0;
    if (s->begin_result(s->context) ||
        s->bind_descriptor(s->context, NULL, 0, 3))
      return 1;
    if (s->publish_tensor(s->context, 0, &one, (const uint8_t*)&value, 4,
                          &escaped, 1, 1, 15) ||
        s->publish_result(s->context, 1))
      return 1;
    return PS_RESULT_PUBLISH_V2;
  }
  const ps_result_tensor_spec_v2* image = q->inputs[0].schema->tensors;
  float count = (float)(image->batch_shape[0] * image->batch_shape[1] *
                        image->shape[0] * image->shape[1]);
  ps_result_region_v2 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (s->begin_result(s->context) ||
      s->bind_descriptor(s->context, NULL, 0, 1) ||
      s->publish_tensor(s->context, 0, &one, (const uint8_t*)&count, 4, NULL, 0,
                        1, 15) ||
      s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static const ps_result_tensor_spec_v2 gpu_image = {
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
static const ps_result_schema_v2 gpu_schema = {
    .struct_size = sizeof(gpu_schema),
    .id = "fixture.gpu",
    .id_size = 11,
    .version = 1,
    .publication = 1,
    .tensors = &gpu_image,
    .tensor_count = 1};
static const ps_result_output_v2 gpu_output = {
    .struct_size = sizeof(gpu_output),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = 6,
             .schema = &gpu_schema},
    .input_count = 0,
    .execution = 1};
static int native(void* user, void* raw, const ps_result_query_v2* q,
                  const ps_result_services_v2* s) {
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
  ps_result_region_v2 all = {.struct_size = sizeof(all),
                             .rank = 4,
                             .extent = {1, 1, 1, 1}};
  if (s->begin_result(s->context) ||
      s->bind_descriptor(s->context, NULL, 0, 1) ||
      s->publish_tensor(s->context, 0, &all, out, 4, NULL, 0, 1, 15) ||
      s->publish_result(s->context, 1))
    return 1;
  return 101;
}
static int native_input(void* user, void* raw, const ps_result_query_v2* q,
                        const ps_result_services_v2* s) {
  (void)user;
  struct State* state = (struct State*)raw;
  ps_result_region_v2 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  const int64_t mode = q->parameter_count ? q->parameters[0].int64_value : 0;
  if (!state->stage++)
    return s->need_tensor(s->context, 0, 0, mode == 1 ? 8 : 1, &one, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  ps_result_tensor_window_v2 window = {.struct_size = sizeof(window)};
  uint64_t window_handle = 0;
  if (s->acquire_native_tensor_window(s->context, 0, mode == 2 ? 1 : 0, &one,
                                      &window, &window_handle))
    return 1;
  const uint64_t at = 0;
  ps_result_tensor_row_v2 row = {.struct_size = sizeof(row)};
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
  ps_result_relation_row_v2 support = {0, 0, 1, PS_RESULT_TARGET_TENSOR_V2,
                                       0, 0, 1};
  if (s->gpu->execute(s->gpu->context, &command, 1) ||
      s->gpu->release(s->gpu->context, input) ||
      s->gpu->release(s->gpu->context, result) ||
      s->release_window(s->context, window_handle) ||
      s->begin_result(s->context) ||
      s->bind_descriptor(s->context, NULL, 0, 1) ||
      s->publish_tensor(s->context, 0, &one, output, 4, &support, 1, 1, 15) ||
      s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static const ps_result_port_v2 native_number_input = {
    .struct_size = sizeof(ps_result_port_v2),
    .kind = PS_RESULT_OBJECT_V2,
    .schema = &number_schema};
static void destroy_state(void* user, void* state) {
  (void)user;
  (void)state;
}
static void destroy_plugin(void* context) {
  (void)context;
}
#define COPY_OP(KEY, OUTPUT, TILES)                                   \
  {.struct_size = sizeof(ps_result_operation_v2),                     \
   .key = KEY,                                                        \
   .key_size = sizeof(KEY) - 1,                                       \
   .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 | \
            PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,                       \
   .inputs = inputs,                                                  \
   .input_count = 2,                                                  \
   .outputs = OUTPUT,                                                 \
   .output_count = 1,                                                 \
   .parameters = &mode,                                               \
   .parameter_count = 1,                                              \
   .state_bytes = sizeof(struct State),                               \
   .workspace_bytes = 65536,                                          \
   .maximum_stages = 8,                                               \
   .cpu_staged_tiles = TILES,                                         \
   .resolve_metadata = resolve,                                       \
   .start = start,                                                    \
   .poll = copy,                                                      \
   .destroy = destroy_state}
static const ps_result_tensor_spec_v2 reshape_input_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v2),
    .key = "samples",
    .key_size = 7,
    .element_type = 1,
    .rank = 2,
    .shape = {2, 3},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_tensor_spec_v2 reshape_output_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v2),
    .key = "samples",
    .key_size = 7,
    .element_type = 1,
    .rank = 2,
    .shape = {3, 2},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 reshape_input_schema = {
    .struct_size = sizeof(ps_result_schema_v2),
    .id = "fixture.tensor.input",
    .id_size = 20,
    .version = 1,
    .publication = 1,
    .tensors = &reshape_input_tensor,
    .tensor_count = 1};
static const ps_result_schema_v2 reshape_output_schema = {
    .struct_size = sizeof(ps_result_schema_v2),
    .id = "fixture.tensor.output",
    .id_size = 21,
    .version = 1,
    .publication = 1,
    .tensors = &reshape_output_tensor,
    .tensor_count = 1};
static const ps_result_port_v2 reshape_input = {
    .struct_size = sizeof(ps_result_port_v2),
    .kind = PS_RESULT_OBJECT_V2,
    .schema = &reshape_input_schema,
    .tensor_key = "samples",
    .tensor_key_size = 7,
    .element_type = 1,
    .rank = 2};
static const ps_result_output_v2 reshape_output = {
    .struct_size = sizeof(ps_result_output_v2),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &reshape_output_schema,
             .tensor_key = "samples",
             .tensor_key_size = 7},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_WHOLE_V2};
static int reshape_poll(void* user, void* raw, const ps_result_query_v2* query,
                        const ps_result_services_v2* services) {
  (void)user;
  (void)query;
  struct State* state = (struct State*)raw;
  ps_result_region_v2 source = {.struct_size = sizeof(source),
                                .rank = 2,
                                .extent = {2, 3}};
  ps_result_region_v2 output = {.struct_size = sizeof(output),
                                .rank = 2,
                                .extent = {3, 2}};
  if (!state->stage++)
    return services->need_tensor(services->context, 0, 0, 9, &source, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  uint64_t handle = 0, relation = 0;
  ps_result_tensor_window_v2 window = {.struct_size = sizeof(window)};
  if (services->acquire_tensor_window(services->context, 0, 0, &source, &window,
                                      &handle))
    return 1;
  uint64_t at[] = {0, 0};
  ps_result_tensor_row_v2 row = {.struct_size = sizeof(row)};
  if (window.row(window.context, at, 2, &row) || row.samples != 3 ||
      (row.sample_stride_bytes != -1 && row.sample_stride_bytes != 0))
    return 6;
  uint8_t first = row.data[0], third;
  memcpy(&third, row.data + 2 * row.sample_stride_bytes, 1);
  if (third != (row.sample_stride_bytes ? first - 2 : first))
    return 6;
  const ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
  const ps_result_tensor_transform_v2 transform = {
      .struct_size = sizeof(transform),
      .reshape = 1};
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, &descriptor, 1, 1) ||
      services->make_reshape(services->context, 0, 0, 0, 1, &output, &source,
                             &relation) ||
      services->publish_tensor_view(services->context, 0, &output, &handle, 1,
                                    &transform, relation, 15) ||
      services->release_relation(services->context, relation) ||
      services->release_window(services->context, handle) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
TENSOR_SCHEMA(whole_view_schema, "fixture.whole.tensor", 3, 4, NULL, 0);
static const ps_result_port_v2 whole_view_input = {
    .struct_size = sizeof(ps_result_port_v2),
    .kind = PS_RESULT_OBJECT_V2,
    .schema = &whole_view_schema};
#define WHOLE_VIEW_OUTPUT(FLAGS, BYTES)               \
  {.struct_size = sizeof(ps_result_output_v2),        \
   .key = "value",                                    \
   .key_size = 5,                                     \
   .port = {.struct_size = sizeof(ps_result_port_v2), \
            .kind = PS_RESULT_OBJECT_V2,              \
            .schema = &whole_view_schema},            \
   .input_count = UINT32_MAX,                         \
   .execution = PS_RESULT_WHOLE_V2,                   \
   .flags = FLAGS,                                    \
   .maximum_output_payload_bytes = BYTES}
static const ps_result_output_v2 whole_view_outputs[] = {
    WHOLE_VIEW_OUTPUT(
        PS_RESULT_OUTPUT_PRESERVE_VIEWS_V2 | PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2,
        0),
    WHOLE_VIEW_OUTPUT(PS_RESULT_OUTPUT_PRESERVE_VIEWS_V2 |
                          PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V2 |
                          PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2,
                      0),
    WHOLE_VIEW_OUTPUT(PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2, 32)};
static _Atomic uint32_t whole_view_counts[3];
PS_RESULT_EXPORT uint32_t fixture_result_whole_view_counts(uint32_t which) {
  return which < 3 ? atomic_load(&whole_view_counts[which]) : UINT32_MAX;
}
static int whole_view_policy(void* user, const ps_result_query_v2* query) {
  const ps_result_output_v2* declared = (const ps_result_output_v2*)user;
  return query->output && query->output->struct_size == sizeof(*declared) &&
         query->output->flags == declared->flags &&
         query->output->maximum_output_payload_bytes ==
             declared->maximum_output_payload_bytes &&
         query->output->execution == PS_RESULT_WHOLE_V2;
}
static int whole_view_start(void* user, void* state,
                            const ps_result_query_v2* query,
                            const ps_result_services_v2* services) {
  (void)state;
  (void)services;
  atomic_fetch_add(&whole_view_counts[0], 1);
  return whole_view_policy(user, query) ? 0 : 6;
}
static int whole_view_poll(void* user, void* raw,
                           const ps_result_query_v2* query,
                           const ps_result_services_v2* services) {
  struct State* state = (struct State*)raw;
  if (!whole_view_policy(user, query))
    return 6;
  const ps_result_region_v2 region = {.struct_size = sizeof(region),
                                      .rank = 1,
                                      .extent = {4}};
  if (!state->stage++)
    return services->need_tensor(services->context, 0, 0, 13, &region, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  atomic_fetch_add(&whole_view_counts[1], 1);
  ps_result_tensor_window_v2 window = {.struct_size = sizeof(window)};
  uint64_t handle = 0, relation = 0;
  if (services->acquire_tensor_window(services->context, 0, 0, &region, &window,
                                      &handle))
    return 1;
  const ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
  const ps_result_mapped_axis_v2 axis = {0, 0, 1, 1};
  const ps_result_tensor_transform_v2 transform = {
      .struct_size = sizeof(transform),
      .source_rank = 1,
      .axes = &axis};
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, &descriptor, 1,
                                PS_RESULT_EXACT_V2) ||
      services->make_mapping(services->context, 0, 0,
                             PS_RESULT_TARGET_TENSOR_V2, 0, 5, &region, &axis,
                             1, &relation))
    return 1;
  if (!(query->output->flags & PS_RESULT_OUTPUT_PRESERVE_VIEWS_V2) ||
      (query->parameter_count && query->parameters[0].int64_value == 1)) {
    double packed[4];
    for (uint64_t at = 0; at < 4; ++at) {
      ps_result_tensor_row_v2 row = {.struct_size = sizeof(row)};
      if (window.row(window.context, &at, 1, &row))
        return 1;
      memcpy(packed + at, row.data, 8);
    }
    if (services->publish_tensor_with_relation(
            services->context, 0, &region, (const uint8_t*)packed,
            sizeof(packed), relation, PS_RESULT_FINAL_V2))
      return 1;
  } else if (services->publish_tensor_view(services->context, 0, &region,
                                           &handle, 1, &transform, relation,
                                           PS_RESULT_FINAL_V2)) {
    return 1;
  }
  if (services->release_window(services->context, handle) ||
      services->release_relation(services->context, relation) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static void whole_view_destroy(void* user, void* state) {
  (void)user;
  (void)state;
  atomic_fetch_add(&whole_view_counts[2], 1);
}
#define WHOLE_VIEW_OP(KEY, OUTPUT)                                    \
  {.struct_size = sizeof(ps_result_operation_v2),                     \
   .key = KEY,                                                        \
   .key_size = sizeof(KEY) - 1,                                       \
   .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 | \
            PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,                       \
   .inputs = &whole_view_input,                                       \
   .input_count = 1,                                                  \
   .outputs = &whole_view_outputs[OUTPUT],                            \
   .output_count = 1,                                                 \
   .parameters = &mode,                                               \
   .parameter_count = 1,                                              \
   .state_bytes = sizeof(struct State),                               \
   .maximum_stages = 2,                                               \
   .user_data = (void*)&whole_view_outputs[OUTPUT],                   \
   .start = whole_view_start,                                         \
   .poll = whole_view_poll,                                           \
   .destroy = whole_view_destroy}
static const ps_result_output_v2 bounded_joint_outputs[] = {
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "left",
     .key_size = 4,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &whole_view_schema},
     .input_count = 0,
     .execution = PS_RESULT_REGIONAL_V2,
     .failure_delivery = PS_RESULT_PER_ATOM_OUTCOME_V2,
     .flags = PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2,
     .maximum_output_payload_bytes = 0},
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "right",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &whole_view_schema},
     .input_count = 0,
     .execution = PS_RESULT_REGIONAL_V2,
     .failure_delivery = PS_RESULT_PER_ATOM_OUTCOME_V2,
     .flags = PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2,
     .maximum_output_payload_bytes = 8}};
static int bounded_joint_policy(const ps_result_query_v2* query) {
  return query->output_index < 2 && query->output &&
         query->output->struct_size == sizeof(ps_result_output_v2) &&
         query->output->flags == PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2 &&
         query->output->maximum_output_payload_bytes ==
             (query->output_index ? 8 : 0);
}
static int bounded_single_start(void* user, void* raw,
                                const ps_result_query_v2* query,
                                const ps_result_services_v2* service) {
  (void)user;
  (void)raw;
  (void)service;
  return bounded_joint_policy(query) ? 0 : 6;
}
static int bounded_single_poll(void* user, void* raw,
                               const ps_result_query_v2* query,
                               const ps_result_services_v2* service) {
  (void)user;
  (void)raw;
  if (!bounded_joint_policy(query) || query->requested_count != 1 ||
      query->requested[0].rank != 1 || query->requested[0].extent[0] != 1)
    return 6;
  const double value = query->output_index ? 11 : 7;
  if (service->begin_result(service->context) ||
      service->bind_descriptor(service->context, NULL, 0, PS_RESULT_EXACT_V2) ||
      service->publish_tensor(service->context, 0, query->requested,
                              (const uint8_t*)&value, 8, NULL, 0,
                              PS_RESULT_EXACT_V2, PS_RESULT_FINAL_V2) ||
      service->publish_result(service->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static int bounded_joint_start(const ps_result_joint_query_v2* queries,
                               uint32_t count, void* raw, uint64_t bytes,
                               void* user) {
  (void)raw;
  (void)user;
  if (!count || bytes != 1)
    return 6;
  for (uint32_t i = 0; i < count; ++i)
    if (!bounded_joint_policy(queries[i].query))
      return 6;
  return 0;
}
static int bounded_joint_poll(const ps_result_joint_member_v2* members,
                              uint32_t count, void* raw,
                              const ps_result_joint_services_v2* shared,
                              ps_result_joint_outcome_v2* outcomes,
                              uint32_t* capacity, void* user) {
  (void)shared;
  if (*capacity < count)
    return 6;
  for (uint32_t i = 0; i < count; ++i)
    outcomes[i] = (ps_result_joint_outcome_v2){
        .struct_size = sizeof(ps_result_joint_outcome_v2),
        .key = members[i].key,
        .result = bounded_single_poll(user, raw, members[i].query,
                                      members[i].services)};
  *capacity = count;
  return 0;
}
static void bounded_joint_destroy(void* raw, void* user) {
  (void)raw;
  (void)user;
}
static const ps_result_joint_program_v2 bounded_joint = {
    .struct_size = sizeof(ps_result_joint_program_v2),
    .contract = 2,
    .state_bytes = 1,
    .workspace_bytes = 0,
    .start = bounded_joint_start,
    .poll = bounded_joint_poll,
    .destroy = bounded_joint_destroy,
    .query_size = sizeof(ps_result_joint_query_v2),
    .member_size = sizeof(ps_result_joint_member_v2),
    .outcome_size = sizeof(ps_result_joint_outcome_v2),
    .services_size = sizeof(ps_result_joint_services_v2)};
static const ps_result_tensor_spec_v2 prefix_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v2),
    .key = "samples",
    .key_size = 7,
    .element_type = 1,
    .rank = 1,
    .shape = {6},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 prefix_schema = {
    .struct_size = sizeof(ps_result_schema_v2),
    .id = "fixture.prefix",
    .id_size = 14,
    .version = 1,
    .publication = 1,
    .tensors = &prefix_tensor,
    .tensor_count = 1};
static const ps_result_port_v2 prefix_input = {
    .struct_size = sizeof(ps_result_port_v2),
    .kind = PS_RESULT_OBJECT_V2,
    .schema = &prefix_schema};
static const ps_result_output_v2 prefix_output = {
    .struct_size = sizeof(ps_result_output_v2),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &prefix_schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_WHOLE_V2};
static int prefix_poll(void* user, void* raw, const ps_result_query_v2* query,
                       const ps_result_services_v2* services) {
  (void)user;
  struct State* state = (struct State*)raw;
  ps_result_region_v2 domain = {.struct_size = sizeof(domain),
                                .rank = 1,
                                .extent = {6}};
  if (!state->stage++)
    return services->need_tensor(services->context, 0, 0, 9, &domain, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  uint64_t relation = 0;
  const int64_t failure_mode =
      query->parameter_count ? query->parameters[0].int64_value : 0;
  if (failure_mode) {
    if (failure_mode == 4) {
      if (services->make_prefix(services->context, 0, 0, 0, 1, &relation))
        return 1;
      (void)services->release_relation(services->context, relation);
      (void)services->release_relation(services->context, relation);
    } else {
      (void)services->make_prefix(services->context, failure_mode == 1 ? 1 : 0,
                                  0, 0, failure_mode == 2 ? 8 : 1,
                                  failure_mode == 3 ? NULL : &relation);
    }
    return PS_RESULT_PUBLISH_V2;
  }
  if (services->make_prefix(services->context, 0, 0, 0, 1, &relation))
    return 1;
  uint8_t values[6], carry = 0;
  for (uint64_t i = 0; i < 6; ++i) {
    uint8_t value;
    if (services->read_tensor(services->context, 0, 0, &i, 1, &value, 1))
      return 1;
    carry += value;
    values[i] = carry;
  }
  const ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, &descriptor, 1, 1) ||
      services->publish_tensor_with_relation(services->context, 0, &domain,
                                             values, 6, relation, 15) ||
      services->release_relation(services->context, relation) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static int neighborhood_poll(void* user, void* raw,
                             const ps_result_query_v2* query,
                             const ps_result_services_v2* services) {
  (void)user;
  struct State* state = (struct State*)raw;
  const ps_result_region_v2 domain = {.struct_size = sizeof(domain),
                                      .rank = 1,
                                      .extent = {6}};
  if (!state->stage++)
    return services->need_tensor(services->context, 0, 0, 9, &domain, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  const int64_t mode =
      query->parameter_count ? query->parameters[0].int64_value : 0;
  const uint64_t radii[2] = {1, 1};
  uint64_t relation = 0;
  if (mode && mode < 8) {
    (void)services->make_neighborhood(
        services->context, mode == 1 ? 1 : 0, 0, 0, mode == 2 ? 8 : 1,
        mode == 6 ? NULL : radii, mode == 5 ? 2 : 1, mode == 4 ? 2 : 0,
        mode == 3 ? NULL : &relation);
    if (mode == 7) {
      (void)services->release_relation(services->context, relation);
      (void)services->release_relation(services->context, relation);
    }
    return PS_RESULT_PUBLISH_V2;
  }
  if (services->make_neighborhood(services->context, 0, 0, 0, 1, radii, 1,
                                  mode == 8, &relation))
    return 1;
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
  const ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, &descriptor, 1, 1) ||
      services->publish_tensor_with_relation(services->context, 0, &domain,
                                             values, 6, relation, 15) ||
      services->release_relation(services->context, relation) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static int cartesian_poll(void* user, void* raw,
                          const ps_result_query_v2* query,
                          const ps_result_services_v2* services) {
  (void)user;
  struct State* state = (struct State*)raw;
  const int64_t mode =
      query->parameter_count ? query->parameters[0].int64_value : 0;
  const ps_result_region_v2 domain = {.struct_size = sizeof(domain),
                                      .rank = 1,
                                      .extent = {6}};
  const ps_result_region_v2 source = {.struct_size = sizeof(source),
                                      .rank = 1,
                                      .offset = {1},
                                      .extent = {3}};
  if (!state->stage++)
    return services->need_tensor(services->context, 0, 0, 9, &source, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  if (mode == 10 || mode == 11) {
    ps_result_tensor_window_v2 window = {.struct_size = sizeof(window)};
    uint64_t handle = 0;
    if (services->acquire_tensor_window(services->context, 0, 0, &source,
                                        &window, &handle))
      return 1;
    for (uint64_t i = 0; i < 2000; ++i) {
      const uint64_t at = 3;
      int failed;
      if (mode == 10) {
        ps_result_tensor_row_v2 row = {.struct_size = sizeof(row)};
        failed = window.row(window.context, &at, 1, &row);
      } else {
        ps_result_tensor_rectangle_v2 rectangle = {.struct_size =
                                                       sizeof(rectangle)};
        failed = window.rectangle(window.context, &at, 1, &rectangle);
      }
      if (failed)
        return 1;
    }
    if (services->release_window(services->context, handle))
      return 1;
  }
  uint64_t relation = 0;
  int code = services->make_tensor_cartesian(
      services->context, mode == 1 ? 1 : 0, mode == 2 ? 1 : 0,
      mode == 3 ? 1 : 0, mode == 4 ? 8 : 1, mode == 6 ? UINT64_MAX : 1,
      mode == 7 ? UINT64_MAX : 3, mode == 5 ? 3 : PS_RESULT_CONSERVATIVE_V2,
      mode == 8 ? NULL : &relation);
  if (mode && mode < 10) {
    if (mode == 9 && !code) {
      if (services->release_relation(services->context, relation))
        return 1;
      code = services->release_relation(services->context, relation);
    }
    return code ? PS_RESULT_PUBLISH_V2 : 1;
  }
  if (code)
    return code;
  uint8_t sum = 0, values[6];
  for (uint64_t i = 1; i < 4; ++i) {
    uint8_t value;
    if (services->read_tensor(services->context, 0, 0, &i, 1, &value, 1))
      return 1;
    sum += value;
  }
  memset(values, sum, sizeof(values));
  const ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, &descriptor, 1, 1) ||
      services->publish_tensor_with_relation(services->context, 0, &domain,
                                             values, 6, relation, 15) ||
      services->release_relation(services->context, relation) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static int fallback_enabled = 1, fallback_disabled = 0;
static _Atomic uint32_t impure_calls;
PS_RESULT_EXPORT uint32_t fixture_result_impure_calls(void) {
  return atomic_load(&impure_calls);
}
static int impure_start(void* user, void* raw, const ps_result_query_v2* q,
                        const ps_result_services_v2* s) {
  (void)user;
  (void)q;
  (void)s;
  *(float*)raw = (float)(atomic_fetch_add(&impure_calls, 1) + 1);
  return PS_RESULT_STATUS_OK_V2;
}
static int impure_poll(void* user, void* raw, const ps_result_query_v2* q,
                       const ps_result_services_v2* s) {
  (void)user;
  (void)q;
  const ps_result_region_v2 one = {.struct_size = sizeof(one),
                                   .rank = 1,
                                   .extent = {1}};
  if (s->begin_result(s->context) ||
      s->bind_descriptor(s->context, NULL, 0, 1) ||
      s->publish_tensor(s->context, 0, &one, raw, sizeof(float), NULL, 0, 1,
                        15) ||
      s->publish_result(s->context, 1))
    return PS_RESULT_STATUS_FAILURE_V2;
  return PS_RESULT_PUBLISH_V2;
}
static const ps_result_output_v2 impure_output = {
    .struct_size = sizeof(ps_result_output_v2),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &number_schema},
    .input_count = 0,
    .execution = PS_RESULT_WHOLE_V2};
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
static int fallback_publish(const ps_result_services_v2* s) {
  const float value = 7;
  const ps_result_region_v2 one = {.struct_size = sizeof(one),
                                   .rank = 1,
                                   .extent = {1}};
  return s->begin_result(s->context) ||
         s->bind_descriptor(s->context, NULL, 0, 1) ||
         s->publish_tensor(s->context, 0, &one, (const uint8_t*)&value, 4, NULL,
                           0, 1, 15) ||
         s->publish_result(s->context, 1);
}
static int fallback_start(void* user, void* raw, const ps_result_query_v2* q,
                          const ps_result_services_v2* s) {
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
static int fallback_poll(void* user, void* raw, const ps_result_query_v2* q,
                         const ps_result_services_v2* s) {
  (void)user;
  (void)q;
  const struct FallbackState* state = raw;
  if (state->mode == 14) {
    (void)s->publish_result(s->context, 2);
    return PS_RESULT_PUBLISH_V2;
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
    return fallback_publish(s) ? 1 : PS_RESULT_PUBLISH_V2;
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
      const ps_result_region_v2 one = {.struct_size = sizeof(one),
                                       .rank = 1,
                                       .extent = {1}};
      if (s->begin_result(s->context))
        return 1;
      (void)s->publish_tensor(s->context, 99, &one, (const uint8_t*)&value, 4,
                              NULL, 0, 1, 15);
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
  {.struct_size = sizeof(ps_result_operation_v2),           \
   .key = KEY,                                              \
   .key_size = sizeof(KEY) - 1,                             \
   .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_GPU_V2 | \
            PS_RESULT_FLAG_DETERMINISTIC_V2 |               \
            PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2 | FLAG,      \
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
static ps_result_operation_v2 operations[] = {
    COPY_OP("fixture.result.copy", &regional_output, 0),
    COPY_OP("fixture.result.whole", &whole_output, 0),
    COPY_OP("fixture.result.tiles", &whole_output, 1),
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.scalar",
     .key_size = 21,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .outputs = &scalar_output,
     .output_count = 1,
     .parameters = scalar_parameters,
     .parameter_count = 2,
     .workspace_bytes = 65536,
     .maximum_stages = 2,
     .start = start,
     .poll = scalar,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.mixed",
     .key_size = 20,
     .parameters = &mode,
     .parameter_count = 1,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = inputs,
     .input_count = 2,
     .outputs = mixed_outputs,
     .output_count = 4,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 32,
     .resolve_metadata = resolve,
     .start = start,
     .poll = mixed,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.rows",
     .key_size = 19,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = inputs + 1,
     .input_count = 1,
     .outputs = &row_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 32,
     .start = start,
     .poll = prefix,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.rows_sum",
     .key_size = 23,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = &row_input,
     .input_count = 1,
     .outputs = &number_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 32,
     .start = start,
     .poll = sum_rows,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.rows_first",
     .key_size = 25,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = &row_input,
     .input_count = 1,
     .outputs = &number_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 32,
     .user_data = (void*)1,
     .start = start,
     .poll = sum_rows,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.native",
     .key_size = 21,
     .parameters = &mode,
     .parameter_count = 1,
     .flags = PS_RESULT_FLAG_GPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .outputs = &gpu_output,
     .output_count = 1,
     .workspace_bytes = 65536,
     .maximum_stages = 2,
     .start = start,
     .poll = native,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.typed",
     .key_size = 20,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = typed_inputs,
     .input_count = 2,
     .outputs = &typed_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 4,
     .start = start,
     .poll = typed,
     .destroy = destroy_state},
    COPY_OP("fixture.result.view", &regional_output, 0),
    COPY_OP("fixture.result.view_whole", &whole_output, 0),
    COPY_OP("fixture.result.view_gpu", &whole_output, 0),
    COPY_OP("fixture.result.member_fixed", &regional_output, 0),
    COPY_OP("fixture.result.member_owned", &regional_output, 0),
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.reshape",
     .key_size = 22,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = &reshape_input,
     .input_count = 1,
     .outputs = &reshape_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .maximum_stages = 2,
     .data_movement = 1,
     .start = start,
     .poll = reshape_poll,
     .destroy = destroy_state},
    COPY_OP("fixture.result.sole", &regional_output, 0),
    COPY_OP("fixture.result.sole_fixed", &regional_output, 0),
    COPY_OP("fixture.result.bundle", &regional_output, 0),
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.prefix",
     .key_size = 21,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = &prefix_input,
     .input_count = 1,
     .outputs = &prefix_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .maximum_stages = 2,
     .parameters = &mode,
     .parameter_count = 1,
     .start = start,
     .poll = prefix_poll,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.neighborhood",
     .key_size = 27,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = &prefix_input,
     .input_count = 1,
     .outputs = &prefix_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .maximum_stages = 2,
     .parameters = &mode,
     .parameter_count = 1,
     .start = start,
     .poll = neighborhood_poll,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.cartesian",
     .key_size = 24,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = &prefix_input,
     .input_count = 1,
     .outputs = &prefix_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .maximum_stages = 2,
     .parameters = &mode,
     .parameter_count = 1,
     .start = start,
     .poll = cartesian_poll,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.native_input",
     .key_size = 27,
     .flags = PS_RESULT_FLAG_GPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
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
    FALLBACK_OP("fixture.result.fallback", PS_RESULT_FLAG_CPU_FALLBACK_V2,
                &fallback_enabled),
    FALLBACK_OP("fixture.result.no_fallback", 0, &fallback_disabled),
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.impure",
     .key_size = sizeof("fixture.result.impure") - 1,
     .flags = PS_RESULT_FLAG_CPU_V2,
     .outputs = &impure_output,
     .output_count = 1,
     .state_bytes = sizeof(float),
     .workspace_bytes = 16,
     .maximum_stages = 2,
     .start = impure_start,
     .poll = impure_poll,
     .destroy = destroy_state},
    WHOLE_VIEW_OP("fixture.result.whole_auto", 0),
    WHOLE_VIEW_OP("fixture.result.whole_strict", 1),
    WHOLE_VIEW_OP("fixture.result.whole_copy", 2),
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result.bounded_joint",
     .key_size = sizeof("fixture.result.bounded_joint") - 1,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .outputs = bounded_joint_outputs,
     .output_count = 2,
     .state_bytes = 1,
     .maximum_stages = 1,
     .start = bounded_single_start,
     .poll = bounded_single_poll,
     .destroy = destroy_state,
     .joint = &bounded_joint}};
static ps_result_operation_plugin_api_v2 api = {
    sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_2,
    operations,  sizeof(operations) / sizeof(operations[0]),
    NULL,        destroy_plugin};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  operations[16].inputs = sole_inputs;
  operations[16].user_data = &sole_marker;
  operations[17].inputs = sole_fixed_inputs;
  operations[17].resolve_metadata = NULL;
  operations[17].user_data = &sole_marker;
  operations[18].inputs = bundle_inputs;
  operations[18].resolve_metadata = NULL;
  operations[13].inputs = operations[14].inputs = member_inputs;
  operations[13].resolve_metadata = NULL;
  operations[14].user_data = &member_marker;
  operations[10].poll = operations[11].poll = operations[12].poll = zero_copy;
  operations[10].data_movement = operations[11].data_movement =
      operations[12].data_movement = 1;
  operations[12].flags = PS_RESULT_FLAG_GPU_V2 |
                         PS_RESULT_FLAG_DETERMINISTIC_V2 |
                         PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2;
#ifdef PS_BAD_RESULT_CASE
#if PS_BAD_RESULT_CASE == 1
  api.struct_size -= 4;
#elif PS_BAD_RESULT_CASE == 2
  api.abi_version = 3;
#elif PS_BAD_RESULT_CASE == 3
  operations[0].inputs = NULL;
#elif PS_BAD_RESULT_CASE == 4
  operations[1].struct_size -= 4;
#elif PS_BAD_RESULT_CASE == 5
  operations[0].outputs = &gpu_output;
  operations[0].cpu_staged_tiles = 99;
#elif PS_BAD_RESULT_CASE == 7
  operations[23].flags &= ~PS_RESULT_FLAG_GPU_V2;
#elif PS_BAD_RESULT_CASE == 8
  operations[23].flags &= ~PS_RESULT_FLAG_CPU_V2;
#elif PS_BAD_RESULT_CASE >= 9 && PS_BAD_RESULT_CASE <= 14
  {
    static ps_result_output_v2 invalid;
    invalid = whole_view_outputs[0];
#if PS_BAD_RESULT_CASE == 9
    invalid.flags |= 8;
#elif PS_BAD_RESULT_CASE == 10
    invalid.flags = PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V2;
#elif PS_BAD_RESULT_CASE == 11
    invalid.flags |= PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V2;
    invalid.execution = PS_RESULT_REGIONAL_V2;
#elif PS_BAD_RESULT_CASE == 12
    operations[0].flags |= PS_RESULT_FLAG_GPU_V2;
#elif PS_BAD_RESULT_CASE == 13
    invalid.flags = 0;
    invalid.maximum_output_payload_bytes = 32;
#elif PS_BAD_RESULT_CASE == 14
    invalid.struct_size = offsetof(ps_result_output_v2, flags);
#endif
    operations[0].outputs = &invalid;
  }
#elif PS_BAD_RESULT_CASE == 6
  {
    static ps_result_output_v2 invalid;
    invalid = scalar_output;
    invalid.port.element_type_mask = 0x8000;
    operations[0].outputs = &invalid;
  }
#endif
#endif
  return &api;
}
