#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"

static const ps_result_image_spec_v1 image = {
    .struct_size = sizeof(image),
    .key = "pixels",
    .key_size = 6,
    .element_type = 4,
    .rank = 2,
    .shape = {2, 4},
    .frames = 2,
    .layers = 2,
    .height_axis = 0,
    .width_axis = 1,
    .storage_order = 1,
    .channel_axis = PS_RESULT_NO_CHANNEL_V1};
static const ps_result_schema_v1 schema = {.struct_size = sizeof(schema),
                                           .id = "example.image",
                                           .id_size = 13,
                                           .version = 1,
                                           .publication = 1,
                                           .images = &image,
                                           .image_count = 1};
static const ps_result_port_v1 inputs[] = {
    {.struct_size = sizeof(ps_result_port_v1), .kind = 6, .schema = &schema},
    {.struct_size = sizeof(ps_result_port_v1),
     .kind = 1,
     .element_type = 2,
     .rank = 1,
     .shape = {1}}};
static const ps_result_output_v1 regional_output = {
    .struct_size = sizeof(ps_result_output_v1),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v1),
             .kind = 6,
             .schema = &schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_REGIONAL_V1};
static const ps_result_output_v1 whole_output = {
    .struct_size = sizeof(ps_result_output_v1),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v1),
             .kind = 6,
             .schema = &schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_WHOLE_V1};
static const ps_result_output_v1 scalar_output = {
    .struct_size = sizeof(ps_result_output_v1),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v1),
             .kind = 1,
             .element_type = 4,
             .rank = 1,
             .shape = {128}},
    .input_count = 0,
    .execution = PS_RESULT_REGIONAL_V1};
static const ps_operation_parameter_descriptor_v11 mode = {
    sizeof(mode), "mode", 4, PS_OPERATION_PARAMETER_INT64_V11, 0, 1, 0, 12};
struct State {
  unsigned stage;
  int64_t shift, mode;
  uint64_t handle;
  ps_result_services_v1 saved;
};
struct Work {
  const ps_result_services_v1* services;
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
  (void)work->services->read_value(work->services->context, 1, &at, 1, &value,
                                   8);
  return 1;
}
static int tile(void* raw, const ps_cpu_tile_v1* box) {
  return range(raw, box->begin[0], box->end[0], box->slot);
}
static int start(void* user, void* state, const ps_result_query_v1* query,
                 const ps_result_services_v1* services) {
  (void)user;
  if (query->parameter_count && (query->parameters[0].int64_value == 5 ||
                                 query->parameters[0].int64_value == 6))
    ((struct State*)state)->saved = *services;
  return services->abi_version == 1 &&
                 services->struct_size == sizeof(*services)
             ? 0
             : 6;
}
static ps_result_region_v1 unit(const uint64_t at[4]) {
  ps_result_region_v1 r = {0};
  r.struct_size = sizeof(r);
  r.rank = 4;
  for (unsigned i = 0; i < 4; ++i) {
    r.offset[i] = at[i];
    r.extent[i] = 1;
  }
  return r;
}
static uint64_t flatten(const uint64_t at[4],
                        const ps_result_image_spec_v1* image) {
  return ((at[0] * image->layers + at[1]) * image->shape[0] + at[2]) *
             image->shape[1] +
         at[3];
}
static int resolve(void* user, const ps_result_port_v1* inputs, uint32_t count,
                   const ps_operation_parameter_value_v11* parameters,
                   uint32_t parameter_count,
                   const ps_result_port_v1* prototypes, uint32_t output_count,
                   const ps_result_metadata_sink_v1* sink) {
  (void)user;
  if (parameter_count && parameters[0].int64_value == 4) {
    _Alignas(8) uint8_t raw[sizeof(ps_result_port_v1) + 8];
    memcpy(raw + 1, inputs, sizeof(ps_result_port_v1));
    (void)sink->set_output(sink->context, 0,
                           (const ps_result_port_v1*)(raw + 1));
    return 0;
  }
  if (parameter_count && parameters[0].int64_value == 9) {
    _Alignas(8) uint32_t short_record = 4;
    (void)sink->set_output(sink->context, 0,
                           (const ps_result_port_v1*)&short_record);
    return 0;
  }
  if (count != 2 || !output_count || inputs[0].kind != PS_RESULT_OBJECT_V1 ||
      !inputs[0].schema || inputs[0].schema->image_count != 1 ||
      inputs[0].schema->images[0].rank != 2 ||
      inputs[0].schema->images[0].element_type != 4 ||
      inputs[0].schema->images[0].height_axis != 0 ||
      inputs[0].schema->images[0].width_axis != 1 ||
      inputs[0].schema->images[0].channel_axis != PS_RESULT_NO_CHANNEL_V1)
    return 6;
  // Callback-local nested records exercise synchronous host copying.
  ps_result_image_spec_v1 image = inputs[0].schema->images[0];
  ps_result_schema_v1 schema = *inputs[0].schema;
  schema.images = &image;
  ps_result_port_v1 output = inputs[0];
  output.schema = &schema;
  if (sink->set_output(sink->context, 0, &output))
    return 1;
  for (uint32_t i = 1; i < output_count; ++i)
    if (sink->set_output(sink->context, i, prototypes + i))
      return 1;
  return 0;
}
static int copy(void* user, void* raw, const ps_result_query_v1* q,
                const ps_result_services_v1* s) {
  ps_result_query_v1 resolved = *q;
  const ps_result_image_spec_v1* spec = q->output->port.schema->images;
  ps_result_region_v1 all = {
      .struct_size = sizeof(all),
      .rank = 4,
      .extent = {spec->frames, spec->layers, spec->shape[0], spec->shape[1]}};
  if (!q->requested_kind) {
    resolved.requested = &all;
    resolved.requested_count = 1;
    q = &resolved;
  }
  struct State* state = (struct State*)raw;
  uint64_t zero = 0;
  (void)user;
  if (q->image_slot)
    return 6;
  if (q->parameter_count)
    state->mode = q->parameters[0].int64_value;
  ps_result_region_v1 control = {.struct_size = sizeof(control),
                                 .rank = 1,
                                 .extent = {1}};
  if (state->stage == 0) {
    if (state->mode == 5)
      (void)state->saved.consume_work(state->saved.context, 1);
    if (state->mode == 6 && state->saved.cpu_parallel)
      (void)state->saved.cpu_parallel->run(state->saved.cpu_parallel->context,
                                           0, 1, 0, range, NULL);
    state->stage = 1;
    return s->need_value(s->context, 1, 2, &control, 1) ? 1 : PS_RESULT_NEED_V1;
  }
  if (state->stage == 1) {
    if (s->read_value(s->context, 1, &zero, 1, &state->shift, 8) ||
        state->shift < 0 || state->shift > 1)
      return 6;
    ps_result_region_v1 needed[64];
    uint32_t count = 0;
    for (uint32_t r = 0; r < q->requested_count; ++r) {
      const ps_result_region_v1* box = q->requested + r;
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
      ps_result_relation_row_v1 descriptor = {0, 1, 2, 0, 0, 0, 1};
      if (s->begin_result(s->context) ||
          s->bind_descriptor(s->context, &descriptor, 1, 1) ||
          s->publish_result(s->context, 1))
        return 1;
      return PS_RESULT_PUBLISH_V1;
    }
    return s->need_image(s->context, 0, 0, 1, needed, count)
               ? 1
               : PS_RESULT_NEED_V1;
  }
  if (state->stage == 2) {
    if (s->retain_image(s->context, 0, 0, &state->handle))
      return 1;
    state->stage = 3;
    return s->need_value(s->context, 1, 2, &control, 1) ? 1 : PS_RESULT_NEED_V1;
  }
  if (s->begin_result(s->context) || s->bind_descriptor(s->context, NULL, 0, 1))
    return 1;
  for (uint32_t r = 0; r < q->requested_count; ++r) {
    const ps_result_region_v1* box = q->requested + r;
    float pixels[64];
    ps_result_relation_row_v1 rows[128];
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
            if (count == 64 || s->read_retained_image(s->context, state->handle,
                                                      in, 4, pixels + count, 4))
              return 1;
            rows[2 * count] = (ps_result_relation_row_v1){
                flatten(out, spec), 0, 1, 2, 0, flatten(in, spec), 1};
            rows[2 * count + 1] = (ps_result_relation_row_v1){
                flatten(out, spec), 1, 2, 0, 0, 0, 1};
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
      (void)s->read_retained_image(s->context, state->handle, bad, 4, pixels,
                                   4);
    }
    if (state->mode == 2) {
      (void)s->release_image(s->context, state->handle);
      (void)s->release_image(s->context, state->handle);
    }
    if (state->mode == 3)
      return 2;
    if (state->mode == 7 && count)
      rows[0].target = 99;
    if (state->mode == 8 && count)
      rows[0].target = PS_RESULT_TARGET_VALUE_V1;
    if (s->publish_image(s->context, 0, box, (const uint8_t*)pixels, count * 4,
                         rows, count * 2, 1, 15))
      return 1;
  }
  if (state->mode != 2 && s->release_image(s->context, state->handle))
    return 1;
  if (s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V1;
}
static int scalar(void* user, void* state, const ps_result_query_v1* q,
                  const ps_result_services_v1* s) {
  (void)user;
  (void)state;
  if (!q->requested_count) {
    ps_result_region_v1 empty = {.struct_size = sizeof(empty), .rank = 1};
    return s->publish_value(s->context, &empty, NULL, 0, NULL, 0, 1)
               ? 1
               : PS_RESULT_PUBLISH_V1;
  }
  for (uint32_t i = 0; i < q->requested_count; ++i) {
    float samples[128];
    const ps_result_region_v1* box = q->requested + i;
    if (box->rank != 1 || box->extent[0] > 128)
      return 6;
    for (uint64_t n = 0; n < box->extent[0]; ++n)
      samples[n] = (float)(box->offset[0] + n);
    if (s->publish_value(s->context, box, (const uint8_t*)samples,
                         box->extent[0] * 4, NULL, 0, 1))
      return 1;
  }
  return PS_RESULT_PUBLISH_V1;
}

static const ps_result_field_spec_v1 field = {
    .struct_size = sizeof(field),
    .key = "rows",
    .key_size = 4,
    .element_type = 2,
    .rows = {.kind = 5, .divisor = 1}};
static const ps_result_schema_v1 row_schema = {
    .struct_size = sizeof(row_schema),
    .id = "fixture.rows",
    .id_size = 12,
    .version = 1,
    .publication = 3,
    .fields = &field,
    .field_count = 1};
static const ps_result_output_v1 row_output = {
    .struct_size = sizeof(row_output),
    .key = "rows",
    .key_size = 4,
    .port = {.struct_size = sizeof(ps_result_port_v1),
             .kind = 6,
             .schema = &row_schema},
    .input_count = UINT32_MAX,
    .execution = 2};
static int prefix(void* user, void* raw, const ps_result_query_v1* q,
                  const ps_result_services_v1* s) {
  struct State* state = (struct State*)raw;
  (void)user;
  uint64_t zero = 0;
  uint32_t control = q->input_count == 2 ? 1 : 0;
  ps_result_region_v1 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (!state->stage) {
    state->stage = 1;
    return s->need_value(s->context, control, 2, &one, 1) ? 1 : 100;
  }
  if (state->stage == 1) {
    if (s->read_value(s->context, control, &zero, 1, &state->shift, 8) ||
        state->shift < 0 || state->shift > 4)
      return 6;
    ps_result_relation_row_v1 basis = {0, control, 2, 0, 0, 0, 1};
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
static const ps_result_port_v1 row_input = {.struct_size = sizeof(row_input),
                                            .kind = 6,
                                            .schema = &row_schema};
static const ps_result_output_v1 number_output = {
    .struct_size = sizeof(number_output),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v1),
             .kind = 1,
             .element_type = 4,
             .rank = 1,
             .shape = {1}},
    .input_count = UINT32_MAX,
    .execution = 1};
static int sum_rows(void* user, void* raw, const ps_result_query_v1* q,
                    const ps_result_services_v1* s) {
  struct State* state = (struct State*)raw;
  (void)q;
  if (!state->stage) {
    state->stage = 1;
    return s->need_result(s->context, 0, 0, user == NULL, 1) ? 1 : 100;
  }
  if (state->stage == 1) {
    ps_result_descriptor_v1 facts = {.struct_size = sizeof(facts)};
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
  ps_result_relation_row_v1 basis = {0, 0, 8, 3, 0, 0, 1},
                            data = {0, 0, 1, 1, 0, 0, state->handle};
  ps_result_region_v1 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (s->bind_descriptor(s->context, &basis, 1, 1) ||
      s->publish_value(s->context, &one, (const uint8_t*)&sum, 4,
                       state->handle ? &data : NULL, state->handle ? 1 : 0, 1))
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
static const ps_operation_facet_view_v11 lut_facet = {
    sizeof(lut_facet), "photospider.semantic", 20, 1,
    lut_payload,       sizeof(lut_payload)};
static const ps_operation_facet_view_v11 scalar_facet = {
    sizeof(scalar_facet), "photospider.semantic", 20, 1,
    scalar_payload,       sizeof(scalar_payload)};
static const ps_result_port_v1 typed_inputs[] = {
    {.struct_size = sizeof(ps_result_port_v1),
     .kind = PS_RESULT_TYPED_V1,
     .element_type = 4,
     .rank = 1,
     .shape = {4},
     .facets = &lut_facet,
     .facet_count = 1,
     .semantic_kind = 8},
    {.struct_size = sizeof(ps_result_port_v1),
     .kind = PS_RESULT_SCALAR_V1,
     .element_type = 4,
     .rank = 1,
     .shape = {1},
     .minimum = 0,
     .maximum = 1}};
static const ps_result_output_v1 typed_output = {
    .struct_size = sizeof(typed_output),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v1),
             .kind = PS_RESULT_TYPED_V1,
             .element_type = 4,
             .rank = 1,
             .shape = {1},
             .facets = &scalar_facet,
             .facet_count = 1,
             .semantic_kind = 1},
    .input_count = UINT32_MAX,
    .execution = 1};
static int typed(void* user, void* raw, const ps_result_query_v1* q,
                 const ps_result_services_v1* s) {
  (void)user;
  struct State* state = (struct State*)raw;
  if (q->inputs[0].kind != 5 || q->inputs[1].kind != 2 ||
      q->output->port.kind != 5)
    return 6;
  ps_result_region_v1 lut = {.struct_size = sizeof(lut),
                             .rank = 1,
                             .extent = {4}},
                      one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (!state->stage) {
    state->stage = 1;
    return s->need_value(s->context, 0, 1, &lut, 1) ||
                   s->need_value(s->context, 1, 5, &one, 1)
               ? 1
               : 100;
  }
  float sum = 0, factor = 0;
  uint64_t at = 0;
  if (s->read_value(s->context, 1, &at, 1, &factor, 4))
    return 1;
  for (at = 0; at < 4; ++at) {
    float sample = 0;
    if (s->read_value(s->context, 0, &at, 1, &sample, 4))
      return 1;
    sum += sample;
  }
  sum *= factor;
  ps_result_relation_row_v1 rows[] = {{0, 0, 1, 0, 0, 0, 4},
                                      {0, 1, 5, 0, 0, 0, 1}};
  return s->publish_value(s->context, &one, (const uint8_t*)&sum, 4, rows, 2, 1)
             ? 1
             : 101;
}
static const uint32_t control_port = 1;
static const ps_result_output_v1 mixed_outputs[] = {
    {.struct_size = sizeof(ps_result_output_v1),
     .key = "image",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v1),
              .kind = 6,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = 2},
    {.struct_size = sizeof(ps_result_output_v1),
     .key = "number",
     .key_size = 6,
     .port = {.struct_size = sizeof(ps_result_port_v1),
              .kind = 1,
              .element_type = 4,
              .rank = 1,
              .shape = {1}},
     .input_count = 0,
     .execution = 1},
    {.struct_size = sizeof(ps_result_output_v1),
     .key = "lut",
     .key_size = 3,
     .port = {.struct_size = sizeof(ps_result_port_v1),
              .kind = 5,
              .element_type = 4,
              .rank = 1,
              .shape = {4},
              .facets = &lut_facet,
              .facet_count = 1,
              .semantic_kind = 8},
     .input_count = 0,
     .execution = 1},
    {.struct_size = sizeof(ps_result_output_v1),
     .key = "metadata",
     .key_size = 8,
     .port = {.struct_size = sizeof(ps_result_port_v1),
              .kind = 6,
              .schema = &row_schema},
     .input_indices = &control_port,
     .input_count = 1,
     .execution = 2}};
static int mixed(void* user, void* raw, const ps_result_query_v1* q,
                 const ps_result_services_v1* s) {
  if (q->output_index == 0)
    return copy(user, raw, q, s);
  if (q->output_index == 3)
    return prefix(user, raw, q, s);
  if (q->output_index == 2)
    return scalar(user, raw, q, s);
  if (q->parameter_count && q->parameters[0].int64_value == 12) {
    ps_result_region_v1 one = {.struct_size = sizeof(one),
                               .rank = 1,
                               .extent = {1}};
    ps_result_relation_row_v1 escaped = {0, 0, 1, 2, 0, 0, 1};
    float value = 0;
    if (s->bind_descriptor(s->context, NULL, 0, 3))
      return 1;
    return s->publish_value(s->context, &one, (const uint8_t*)&value, 4,
                            &escaped, 1, 1)
               ? 1
               : 101;
  }
  const ps_result_image_spec_v1* image = q->inputs[0].schema->images;
  float count = (float)(image->frames * image->layers * image->shape[0] *
                        image->shape[1]);
  ps_result_region_v1 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  return s->publish_value(s->context, &one, (const uint8_t*)&count, 4, NULL, 0,
                          1)
             ? 1
             : 101;
}
static const ps_result_image_spec_v1 gpu_image = {
    .struct_size = sizeof(gpu_image),
    .key = "pixels",
    .key_size = 6,
    .element_type = 4,
    .rank = 2,
    .shape = {1, 1},
    .frames = 1,
    .layers = 1,
    .height_axis = 0,
    .width_axis = 1,
    .channel_axis = UINT32_MAX,
    .storage_order = 1};
static const ps_result_schema_v1 gpu_schema = {
    .struct_size = sizeof(gpu_schema),
    .id = "fixture.gpu",
    .id_size = 11,
    .version = 1,
    .publication = 1,
    .images = &gpu_image,
    .image_count = 1};
static const ps_result_output_v1 gpu_output = {
    .struct_size = sizeof(gpu_output),
    .key = "image",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v1),
             .kind = 6,
             .schema = &gpu_schema},
    .input_count = 0,
    .execution = 1};
static int native(void* user, void* raw, const ps_result_query_v1* q,
                  const ps_result_services_v1* s) {
  (void)user;
  (void)raw;
  (void)q;
  if (!s->gpu || s->gpu->backend != PS_GPU_BACKEND_METAL_V11)
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
  ps_gpu_buffer_binding_v11 buffers[2] = {
      {.struct_size = sizeof(ps_gpu_buffer_binding_v11),
       .index = 0,
       .token = a,
       .byte_size = 4},
      {.struct_size = sizeof(ps_gpu_buffer_binding_v11),
       .index = 1,
       .token = b,
       .byte_size = 4,
       .writable = 1}};
  static const char shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\nkernel void "
      "scale(device const float* a [[buffer(0)]],device float* b "
      "[[buffer(1)]], uint i [[thread_position_in_grid]]){b[i]=a[i]*.5f;}";
  ps_gpu_dispatch_v11 command = {0};
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
  ps_result_region_v1 all = {.struct_size = sizeof(all),
                             .rank = 4,
                             .extent = {1, 1, 1, 1}};
  if (s->begin_result(s->context) ||
      s->bind_descriptor(s->context, NULL, 0, 1) ||
      s->publish_image(s->context, 0, &all, out, 4, NULL, 0, 1, 15) ||
      s->publish_result(s->context, 1))
    return 1;
  return 101;
}
static void destroy_state(void* user, void* state) {
  (void)user;
  (void)state;
}
static void destroy_plugin(void* context) {
  (void)context;
}
#define COPY_OP(KEY, OUTPUT, TILES)                                   \
  {.struct_size = sizeof(ps_result_operation_v1),                     \
   .key = KEY,                                                        \
   .key_size = sizeof(KEY) - 1,                                       \
   .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC | \
            PS_OPERATION_FLAG_SIDE_EFFECT_FREE,                       \
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
static ps_result_operation_v1 operations[] = {
    COPY_OP("fixture.result.copy", &regional_output, 0),
    COPY_OP("fixture.result.whole", &whole_output, 0),
    COPY_OP("fixture.result.tiles", &whole_output, 1),
    {.struct_size = sizeof(ps_result_operation_v1),
     .key = "fixture.result.scalar",
     .key_size = 21,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .outputs = &scalar_output,
     .output_count = 1,
     .workspace_bytes = 65536,
     .maximum_stages = 2,
     .start = start,
     .poll = scalar,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v1),
     .key = "fixture.result.mixed",
     .key_size = 20,
     .parameters = &mode,
     .parameter_count = 1,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
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
    {.struct_size = sizeof(ps_result_operation_v1),
     .key = "fixture.result.rows",
     .key_size = 19,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
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
    {.struct_size = sizeof(ps_result_operation_v1),
     .key = "fixture.result.rows_sum",
     .key_size = 23,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
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
    {.struct_size = sizeof(ps_result_operation_v1),
     .key = "fixture.result.rows_first",
     .key_size = 25,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
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
    {.struct_size = sizeof(ps_result_operation_v1),
     .key = "fixture.result.native",
     .key_size = 21,
     .parameters = &mode,
     .parameter_count = 1,
     .flags = PS_OPERATION_FLAG_GPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .outputs = &gpu_output,
     .output_count = 1,
     .workspace_bytes = 65536,
     .maximum_stages = 2,
     .start = start,
     .poll = native,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v1),
     .key = "fixture.result.typed",
     .key_size = 20,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .inputs = typed_inputs,
     .input_count = 2,
     .outputs = &typed_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 4,
     .start = start,
     .poll = typed,
     .destroy = destroy_state}};
static ps_result_operation_plugin_api_v1 api = {
    sizeof(api), 1, operations, 10, NULL, destroy_plugin};
PS_OPERATION_EXPORT const ps_result_operation_plugin_api_v1*
ps_result_operation_plugin_get_api_v1(void) {
#ifdef PS_BAD_RESULT_CASE
#if PS_BAD_RESULT_CASE == 1
  api.struct_size -= 4;
#elif PS_BAD_RESULT_CASE == 2
  api.abi_version = 2;
#elif PS_BAD_RESULT_CASE == 3
  operations[0].inputs = NULL;
#elif PS_BAD_RESULT_CASE == 4
  operations[1].struct_size -= 4;
#elif PS_BAD_RESULT_CASE == 5
  operations[0].outputs = &gpu_output;
  operations[0].cpu_staged_tiles = 99;
#elif PS_BAD_RESULT_CASE == 6
  {
    static ps_result_output_v1 invalid;
    invalid = scalar_output;
    invalid.port.element_type_mask = 0x8000;
    operations[0].outputs = &invalid;
  }
#endif
#endif
  return &api;
}
