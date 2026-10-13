#include <stddef.h>
#include <stdint.h>

#include "photospider/plugin/result_tile_helpers.h"

#define TENSOR_SCHEMA(NAME, ID, DTYPE, COUNT, FACETS, FACET_COUNT) \
  static const ps_result_tensor_spec_v3 NAME##_tensor = {          \
      .struct_size = sizeof(ps_result_tensor_spec_v3),             \
      .key = "samples",                                            \
      .key_size = 7,                                               \
      .element_type = DTYPE,                                       \
      .rank = 1,                                                   \
      .shape = {COUNT},                                            \
      .channel_axis = PS_RESULT_NO_CHANNEL_V3,                     \
      .facets = FACETS,                                            \
      .facet_count = FACET_COUNT};                                 \
  static const ps_result_schema_v3 NAME = {                        \
      .struct_size = sizeof(ps_result_schema_v3),                  \
      .id = ID,                                                    \
      .id_size = sizeof(ID) - 1,                                   \
      .version = 1,                                                \
      .publication = 1,                                            \
      .tensors = &NAME##_tensor,                                   \
      .tensor_count = 1}
TENSOR_SCHEMA(control_schema, "fixture.control", 2, 1, NULL, 0);
TENSOR_SCHEMA(number_schema, "fixture.number", 4, 1, NULL, 0);

static const ps_result_port_v3 control_input = {
    .struct_size = sizeof(control_input),
    .kind = PS_RESULT_OBJECT_V3,
    .schema = &control_schema};
static const ps_result_parameter_descriptor_v3 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V3, 0, 1, 0, 8};
struct State {
  unsigned stage;
  int64_t shift;
  uint64_t handle;
};
static int start(void* user, void* raw, const ps_result_query_v3* query,
                 const ps_result_services_v3* services) {
  (void)user;
  (void)query;
  *(struct State*)raw = (struct State){0};
  return services->abi_version == PS_RESULT_OPERATION_ABI_VERSION_3 &&
                 services->struct_size == sizeof(*services)
             ? 0
             : 6;
}
static void destroy_state(void* user, void* raw) {
  (void)user;
  (void)raw;
}
static void destroy_api(void* user) {
  (void)user;
}
static const ps_result_field_spec_v3 field = {
    .struct_size = sizeof(field),
    .key = "rows",
    .key_size = 4,
    .element_type = 2,
    .rows = {.kind = 5, .divisor = 1}};
static const ps_result_schema_v3 row_schema = {
    .struct_size = sizeof(row_schema),
    .id = "fixture.rows",
    .id_size = 12,
    .version = 1,
    .publication = 3,
    .fields = &field,
    .field_count = 1};
static const ps_result_output_v3 row_output = {
    .struct_size = sizeof(row_output),
    .key = "rows",
    .key_size = 4,
    .port = {.struct_size = sizeof(ps_result_port_v3),
             .kind = 6,
             .schema = &row_schema},
    .input_count = UINT32_MAX,
    .execution = 2};
static int prefix(void* user, void* raw, const ps_result_query_v3* q,
                  const ps_result_services_v3* s) {
  struct State* state = (struct State*)raw;
  (void)user;
  uint64_t zero = 0;
  uint32_t control = q->input_count == 2 ? 1 : 0;
  ps_result_region_v3 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (!state->stage) {
    state->stage = 1;
    return ps_result_need_regions_v3(s, q, control, &one, 1) ? 1 : 100;
  }
  if (state->stage == 1) {
    if (s->read_tensor(s->context, control, 0, &zero, 1, &state->shift, 8) ||
        state->shift < 0 || state->shift > 4)
      return 6;

    if (s->begin_result(s->context))
      return 1;
    if (!state->shift) {
      if (s->publish_field(s->context, 0, 0) ||
          s->publish_result(s->context, 1))
        return 1;
      return 101;
    }
  }
  if (state->stage == 1 || state->stage == 3) {
    int64_t number = (int64_t)(++state->handle);
    state->stage = 2;
    if (s->append_field(s->context, 0, 1, (const uint8_t*)&number, 8))
      return 1;
  }
  if (s->publish_field(s->context, 0, state->handle) ||
      s->publish_result(s->context, state->handle == (uint64_t)state->shift))
    return 1;
  state->stage = 3;
  return 101;
}
static const ps_result_port_v3 row_input = {.struct_size = sizeof(row_input),
                                            .kind = 6,
                                            .schema = &row_schema};
static const ps_result_output_v3 number_output = {
    .struct_size = sizeof(number_output),
    .key = "number",
    .key_size = 6,
    .port = {.struct_size = sizeof(ps_result_port_v3),
             .kind = PS_RESULT_OBJECT_V3,
             .schema = &number_schema},
    .input_count = UINT32_MAX,
    .execution = 1};
static int sum_rows(void* user, void* raw, const ps_result_query_v3* q,
                    const ps_result_services_v3* s) {
  struct State* state = (struct State*)raw;
  const int64_t mode = q->parameter_count ? q->parameters[0].int64_value : 0;
  if (!state->stage) {
    state->stage = 1;
    if (mode != 1)
      return (mode == 2 ? ps_result_need_descriptor_v3(s, 0)
                        : ps_result_need_whole_v3(s, 0))
                 ? 1
                 : 100;
  }
  if (mode >= 1 && mode <= 7) {
    int64_t untouched = 12345;
    const int status = s->read_field(
        s->context, 0, mode == 3 ? 1 : 0, mode == 4 ? UINT64_MAX : 0,
        mode == 5 ? UINT64_MAX : 1, mode == 7 ? NULL : (uint8_t*)&untouched,
        mode == 6 ? 7 : 8);
    if (!status || untouched != 12345)
      return 1;
    /* Sticky service failure must outrank the callback's cancellation. */
    return PS_RESULT_STATUS_CANCELLED_V3;
  }
  ps_result_descriptor_v3 facts = {.struct_size = sizeof(facts)};
  if (s->result_descriptor(s->context, 0, &facts))
    return 1;
  state->handle = user && facts.rows[0] ? 1 : facts.rows[0];
  if (s->read_field(s->context, 0, 0, facts.rows[0], 0, NULL, 0))
    return 1;
  float sum = 0;
  if (mode == 8) {
    int64_t numbers[4] = {0};
    if (state->handle > 4 ||
        s->read_field(s->context, 0, 0, 0, state->handle, (uint8_t*)numbers,
                      state->handle * 8))
      return 1;
    for (uint64_t i = 0; i < state->handle; ++i)
      sum += (float)numbers[i];
  } else {
    for (uint64_t i = 0; i < state->handle; ++i) {
      int64_t number = 0;
      if (s->read_field(s->context, 0, 0, i, 1, (uint8_t*)&number, 8))
        return 1;
      sum += (float)number;
    }
  }

  ps_result_region_v3 one = {.struct_size = sizeof(one),
                             .rank = 1,
                             .extent = {1}};
  if (s->begin_result(s->context) ||
      s->publish_tensor(s->context, 0, &one, (const uint8_t*)&sum, 4) ||
      s->publish_result(s->context, 1))
    return 1;
  return 101;
}

static const ps_result_operation_v3 operations[] = {
    {.struct_size = sizeof(ps_result_operation_v3),
     .key = "fixture.result.rows",
     .key_size = 19,
     .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_DETERMINISTIC_V3 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
     .inputs = &control_input,
     .input_count = 1,
     .outputs = &row_output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 32,
     .start = start,
     .poll = prefix,
     .destroy = destroy_state},
    {.struct_size = sizeof(ps_result_operation_v3),
     .key = "fixture.result.rows_sum",
     .key_size = 23,
     .parameters = &mode,
     .parameter_count = 1,
     .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_DETERMINISTIC_V3 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
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
    {.struct_size = sizeof(ps_result_operation_v3),
     .key = "fixture.result.rows_first",
     .key_size = 25,
     .parameters = &mode,
     .parameter_count = 1,
     .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_DETERMINISTIC_V3 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
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
     .destroy = destroy_state}};
static const ps_result_operation_plugin_api_v3 api = {
    .struct_size = sizeof(api),
    .abi_version = PS_RESULT_OPERATION_ABI_VERSION_3,
    .operations = operations,
    .operation_count = 3,
    .destroy = destroy_api};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v3*
ps_result_operation_plugin_get_api_v3(void) {
  return &api;
}
