#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"

#ifndef PS_RESULT_ATOMS_WIDTH
#define PS_RESULT_ATOMS_WIDTH 3
#endif

static atomic_uint_fast64_t starts, destroys, polls, singles, foreign_quality;
struct State {
  unsigned mode, round;
  uint64_t old_quality;
  ps_result_joint_services_v2 old_services;
};
static int single_start(void* user, void* raw, const ps_result_query_v2* query,
                        const ps_result_services_v2* service) {
  (void)user;
  (void)raw;
  (void)query;
  (void)service;
  atomic_fetch_add(&singles, 1);
  return 0;
}
static int publish(const ps_result_joint_member_v2* member, unsigned mode) {
  const ps_result_services_v2* s = member->services;
  const uint64_t at = member->key.coordinate[0];
  const double value =
      mode == 5 && at == 1 ? (double)(float)16777217 : (double)(at + 2);
  ps_result_region_v2 point = {.struct_size = sizeof(point),
                               .rank = 1,
                               .offset = {at},
                               .extent = {1}};
  if (s->begin_result(s->context) ||
      s->bind_descriptor(s->context, NULL, 0, PS_RESULT_EXACT_V2) ||
      s->publish_tensor(s->context, 0, &point, (const uint8_t*)&value,
                        sizeof(value), NULL, 0, PS_RESULT_EXACT_V2,
                        PS_RESULT_FINAL_V2) ||
      s->publish_result(s->context, 1))
    return PS_RESULT_STATUS_FAILURE_V2;
  return PS_RESULT_PUBLISH_V2;
}
static int single_poll(void* user, void* raw, const ps_result_query_v2* query,
                       const ps_result_services_v2* service) {
  (void)user;
  (void)raw;
  ps_result_joint_member_v2 member = {.key = {query->output_index, 1, {0}},
                                      .query = query,
                                      .services = service};
  return publish(&member, 0);
}
static void single_destroy(void* user, void* raw) {
  (void)user;
  (void)raw;
}
static int start(const ps_result_joint_query_v2* queries, uint32_t count,
                 void* raw, uint64_t bytes, void* user) {
  (void)user;
  atomic_fetch_add(&starts, 1);
  if (!count || bytes != sizeof(struct State))
    return 1;
  struct State* state = (struct State*)raw;
  state->mode = (unsigned)queries[0].query->parameters[0].int64_value;
  for (uint32_t i = 0; i < count; ++i)
    if (queries[i].key.rank != 1 || queries[i].key.output_index ||
        queries[i].key.coordinate[0] >= PS_RESULT_ATOMS_WIDTH)
      return 1;
  return 0;
}
static void fail(ps_result_joint_outcome_v2* out, int domain, int partial) {
  out->result = PS_RESULT_ATOM_FAILURE_V2;
  out->failure.struct_size = sizeof(out->failure);
  out->failure.code = PS_RESULT_ERROR_INVALID_ARGUMENT_V2;
  out->failure.reason = domain ? PS_RESULT_REASON_INVALID_DOMAIN_V2
                               : PS_RESULT_REASON_DIVIDE_BY_ZERO_V2;
  out->failure.origin = PS_RESULT_ORIGIN_DOMAIN_V2;
  out->failure.scope =
      domain ? PS_RESULT_SCOPE_VALIDATION_DOMAIN_V2 : PS_RESULT_SCOPE_ATOM_V2;
  if (domain) {
    out->failure.domain.first = (ps_result_atom_key_v2){0, 1, {0}};
    out->failure.domain.extent[0] = partial ? 2 : 3;
  } else {
    out->failure.atom = out->key;
  }
  memcpy(out->failure.message, "C local failure", 15);
  out->failure.message_size = 15;
}
static int poll(const ps_result_joint_member_v2* members, uint32_t count,
                void* raw, const ps_result_joint_services_v2* shared,
                ps_result_joint_outcome_v2* outcomes, uint32_t* capacity,
                void* user) {
  (void)user;
  struct State* state = (struct State*)raw;
  atomic_fetch_add(&polls, 1);
  if (*capacity < count)
    return 1;
  uint64_t report = 0;
  if (state->mode == 30 || state->mode == 31 || state->mode == 32 ||
      state->mode == 33) {
    const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                       .rank = 1,
                                       .extent = {1}};
    if (state->mode != 31)
      (void)members[0].services->consume_work(members[0].services->context, 77);
    (void)members[1].services->need_tensor(members[1].services->context, 99, 0,
                                           1, &point, 1);
    if (state->mode == 31)
      (void)members[0].services->consume_work(members[0].services->context, 77);
  }
  if (state->mode == 23)
    return PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V2;
  if (state->mode == 24)
    (void)shared->consume_work(shared->context, 1000000);
  if (state->mode == 10)
    (void)shared->quality_measured(shared->context, "system", 6, 3, -1,
                                   &report);
  if (state->mode == 18 && state->round)
    (void)state->old_services.quality_measured(state->old_services.context,
                                               "system", 6, 3, .25, &report);
  if (state->mode == 4 || state->mode == 5 || state->mode == 16 ||
      state->mode == 25) {
    const int64_t a[3] = {2, 3, 4}, b[3] = {4, 9, 16};
    int64_t x[3] = {2, 3, 4};
    if (state->mode == 5)
      x[1] = 16777217;
    if (state->mode == 25)
      x[1] = 1LL << 27;
    (void)shared->quality_integer_diagonal(shared->context, "integer-system",
                                           14, a, x, b, 3, &report);
  } else if (state->mode == 8 || state->mode == 9 || state->mode == 17 ||
             state->mode == 19 || state->mode == 27 || state->mode == 28 ||
             state->mode == 29 || state->mode == 34 ||
             (state->mode == 6 && state->round)) {
    if (shared->quality_measured(shared->context, "system", 6, 3, .25, &report))
      return 1;
    if (state->mode == 9)
      (void)shared->release_quality(shared->context, report);
  }
  for (uint32_t i = 0; i < count; ++i) {
    ps_result_joint_outcome_v2* out = outcomes + count - 1 - i;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->key = members[i].key;
    const uint64_t at = out->key.coordinate[0];
    if ((state->mode == 6 && !state->round) || state->mode == 8 ||
        ((state->mode == 17 || state->mode == 18) && !state->round)) {
      const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                         .rank = 1,
                                         .offset = {at},
                                         .extent = {1}};
      out->result = members[i].services->need_tensor(
                        members[i].services->context, 0, 0, 1, &point, 1)
                        ? 1
                        : PS_RESULT_NEED_V2;
    } else if (at == 1 &&
               (state->mode == 1 || state->mode == 34 || state->mode == 7 ||
                state->mode == 11 || state->mode == 12 || state->mode == 20 ||
                state->mode == 21 || (state->mode == 6 && state->round))) {
      if (state->mode == 12)
        (void)publish(members + i, 0);
      fail(out, state->mode == 6 || state->mode == 20 || state->mode == 21,
           state->mode == 20);
      if (state->mode == 7)
        out->failure.atom.coordinate[0] = 2;
      if (state->mode == 11)
        out->failure.scope = PS_RESULT_SCOPE_GROUP_V2;
    } else {
      if (state->mode == 26 && at == 0)
        (void)members[i].services->consume_work(members[i].services->context,
                                                77);
      out->result = publish(members + i, state->mode);
    }
    uint64_t state_handle = 0;
    if (state->mode == 29) {
      const double numbers[3] = {1, 2, 3};
      if (members[i].services->create_block_state(
              members[i].services->context,
              members[i].query->output->port.schema, (const uint8_t*)numbers,
              sizeof(numbers), &state_handle))
        return 1;
    }
    out->quality = state->mode == 17   ? (state->round ? state->old_quality : 0)
                   : state->mode == 28 ? atomic_load(&foreign_quality)
                   : state->mode == 29 ? state_handle
                                       : report;
  }
  if (state->mode == 2 && count > 1)
    outcomes[1].key = outcomes[0].key;
  if (state->mode == 3)
    outcomes[0].key.coordinate[0] = 99;
  if (state->mode == 13)
    outcomes[0].struct_size = 0;
  if (state->mode == 15) {
    ((ps_result_query_v2*)members[0].query)->output_index = 63;
    ((ps_result_joint_member_v2*)members)[0].key.output_index = 63;
    outcomes[count - 1].key.output_index = 63;
  }
  if (state->mode == 22)
    outcomes[0].failure.code = PS_RESULT_ERROR_INVALID_ARGUMENT_V2;
  if (state->mode == 27)
    atomic_store(&foreign_quality, report);
  state->old_quality = report;
  state->old_services = *shared;
  ++state->round;
  *capacity = state->mode == 14 ? count - 1 : count;
  return 0;
}
static void destroy(void* raw, void* user) {
  (void)raw;
  (void)user;
  atomic_fetch_add(&destroys, 1);
}
static void plugin_destroy(void* raw) {
  (void)raw;
}
static const ps_result_tensor_spec_v2 tensor = {
    .struct_size = sizeof(tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {PS_RESULT_ATOMS_WIDTH},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 schema = {
    .struct_size = sizeof(schema),
    .id = "test.multi_output",
    .id_size = 17,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_port_v2 input = {.struct_size = sizeof(input),
                                        .kind = PS_RESULT_OBJECT_V2,
                                        .schema = &schema};
static const ps_result_output_v2 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_REGIONAL_V2,
    .failure_delivery = PS_RESULT_PER_ATOM_OUTCOME_V2};
static const ps_result_parameter_descriptor_v2 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V2, 0, 1, 0, 34};
static const ps_result_joint_program_v2 joint = {
    sizeof(joint),
    2,
    sizeof(struct State),
    4096,
    start,
    poll,
    destroy,
    sizeof(ps_result_joint_query_v2),
    sizeof(ps_result_joint_member_v2),
    sizeof(ps_result_joint_outcome_v2),
    sizeof(ps_result_joint_services_v2)};
struct CompoundState {
  unsigned stage, mode;
  uint64_t tensor, window_handle;
  ps_result_tensor_window_v2 window;
};
static int compound_start(void* user, void* raw,
                          const ps_result_query_v2* query,
                          const ps_result_services_v2* services) {
  (void)user;
  (void)services;
  ((struct CompoundState*)raw)->mode =
      (unsigned)query->parameters[0].int64_value;
  return 0;
}
static int compound_poll(void* user, void* raw, const ps_result_query_v2* query,
                         const ps_result_services_v2* services) {
  struct CompoundState* state = (struct CompoundState*)raw;
  ps_result_region_v2 all = {.struct_size = sizeof(all),
                             .rank = 1,
                             .extent = {65}};
  ps_result_region_v2 scalar = {.struct_size = sizeof(scalar),
                                .rank = 1,
                                .extent = {1}};
  ps_result_relation_row_v2 rows[3];
  uint32_t row_count = 0;
  double total = 0;
  (void)user;
  (void)query;
  if (!state->stage++) {
    if (state->mode == 1 || state->mode == 2) {
      ps_result_region_v2 sparse[3] = {
          {.struct_size = sizeof(all), .rank = 1, .extent = {1}},
          {.struct_size = sizeof(all), .rank = 1, .offset = {2}, .extent = {1}},
          {.struct_size = sizeof(all),
           .rank = 1,
           .offset = {4},
           .extent = {1}}};
      if (services->need_tensor(services->context, 0, 0, 1, sparse, 3))
        return 1;
    } else if (services->need_tensor(services->context, 0, 0, 1, &all, 1)) {
      return 1;
    }
    return PS_RESULT_NEED_V2;
  }
  if (state->mode == 2) {
    const uint64_t hole = 1;
    (void)services->read_tensor(services->context, 0, 0, &hole, 1, &total, 8);
  }
  if (state->mode == 3) {
    ps_result_descriptor_v2 descriptor = {.struct_size = sizeof(descriptor)};
    (void)services->result_descriptor(services->context, 0, &descriptor);
  }
  if (state->mode == 4 && state->stage == 2) {
    uint64_t original;
    state->window.struct_size = sizeof(state->window);
    if (services->retain_tensor(services->context, 0, 0, &state->tensor) ||
        services->acquire_tensor_window(services->context, 0, 0, &all,
                                        &state->window, &original) ||
        state->window.object_id ||
        services->retain_window(services->context, original,
                                &state->window_handle) ||
        services->release_window(services->context, original) ||
        services->need_tensor(services->context, 0, 0, 1, &scalar, 1))
      return 1;
    return PS_RESULT_NEED_V2;
  }
  if (state->mode == 5) {
    uint64_t window, relation;
    ps_result_tensor_window_v2 captured = {.struct_size = sizeof(captured)};
    ps_result_mapped_axis_v2 axis = {.output_axis = 0,
                                     .source_origin = 64,
                                     .step = 0,
                                     .extent = 1};
    ps_result_tensor_transform_v2 transform = {.struct_size = sizeof(transform),
                                               .source_rank = 1,
                                               .axes = &axis};
    if (services->acquire_tensor_window(services->context, 0, 0, &all,
                                        &captured, &window) ||
        captured.object_id ||
        services->make_mapping(services->context, 0, 0,
                               PS_RESULT_TARGET_TENSOR_V2, 0, 1, &scalar, &axis,
                               1, &relation) ||
        services->begin_result(services->context) ||
        services->bind_descriptor(services->context, NULL, 0,
                                  PS_RESULT_EXACT_V2) ||
        services->publish_tensor_view(services->context, 0, &scalar, &window, 1,
                                      &transform, relation,
                                      PS_RESULT_FINAL_V2) ||
        services->release_window(services->context, window) ||
        services->release_relation(services->context, relation) ||
        services->publish_result(services->context, 1))
      return 1;
    return PS_RESULT_PUBLISH_V2;
  }
  if (state->mode == 4) {
    uint64_t next = 0, last = 64;
    double value = 0;
    if (services->read_retained_tensor(services->context, state->tensor, &last,
                                       1, &value, 8) ||
        value != 66)
      return 1;
    while (next < 65) {
      ps_result_tensor_row_v2 row = {.struct_size = sizeof(row)};
      if (state->window.row(state->window.context, &next, 1, &row) ||
          !row.samples || row.samples > 65 - next)
        return 1;
      for (uint64_t i = 0; i < row.samples; ++i) {
        memcpy(&value, row.data + i * row.sample_stride_bytes, 8);
        total += value;
      }
      next += row.samples;
    }
    if (services->release_window(services->context, state->window_handle) ||
        services->release_tensor(services->context, state->tensor))
      return 1;
  } else {
    for (uint64_t i = 0; i < 65;
         i += state->mode == 1 || state->mode == 2 ? 2 : 1) {
      double value = 0;
      if ((state->mode == 1 || state->mode == 2) && i > 4)
        break;
      if (services->read_tensor(services->context, 0, 0, &i, 1, &value, 8))
        return 1;
      total += value;
    }
  }
  if (state->mode == 1 || state->mode == 2) {
    for (uint64_t i = 0; i <= 4; i += 2)
      rows[row_count++] = (ps_result_relation_row_v2){
          0, 0, 1, PS_RESULT_TARGET_TENSOR_V2, 0, i, 1};
  } else {
    rows[row_count++] = (ps_result_relation_row_v2){
        0, 0, 1, PS_RESULT_TARGET_TENSOR_V2, 0, 0, 65};
  }
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, NULL, 0,
                                PS_RESULT_EXACT_V2) ||
      services->publish_tensor(services->context, 0, &scalar,
                               (const uint8_t*)&total, 8, rows, row_count,
                               PS_RESULT_EXACT_V2, PS_RESULT_FINAL_V2) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static const ps_result_tensor_spec_v2 compound_input_tensor = {
    .struct_size = sizeof(compound_input_tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {65},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 compound_input_schema = {
    .struct_size = sizeof(compound_input_schema),
    .id = "test.multi_output",
    .id_size = 17,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &compound_input_tensor,
    .tensor_count = 1};
static const ps_result_port_v2 compound_input = {
    .struct_size = sizeof(compound_input),
    .kind = PS_RESULT_OBJECT_V2,
    .schema = &compound_input_schema};
static const ps_result_tensor_spec_v2 compound_output_tensor = {
    .struct_size = sizeof(compound_output_tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {1},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 compound_output_schema = {
    .struct_size = sizeof(compound_output_schema),
    .id = "test.multi_output",
    .id_size = 17,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &compound_output_tensor,
    .tensor_count = 1};
static const ps_result_output_v2 compound_output = {
    .struct_size = sizeof(compound_output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &compound_output_schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_REGIONAL_V2};
static const ps_result_operation_v2 operations[] = {
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.result_atoms",
     .key_size = 20,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = &input,
     .input_count = 1,
     .outputs = &output,
     .output_count = 1,
     .parameters = &mode,
     .parameter_count = 1,
     .state_bytes = 1,
     .maximum_stages = 3,
     .start = single_start,
     .poll = single_poll,
     .destroy = single_destroy,
     .joint = &joint},
    {.struct_size = sizeof(ps_result_operation_v2),
     .key = "fixture.compound_tensor",
     .key_size = sizeof("fixture.compound_tensor") - 1,
     .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
     .inputs = &compound_input,
     .input_count = 1,
     .outputs = &compound_output,
     .output_count = 1,
     .parameters = &mode,
     .parameter_count = 1,
     .state_bytes = sizeof(struct CompoundState),
     .workspace_bytes = 8192,
     .maximum_stages = 4,
     .start = compound_start,
     .poll = compound_poll,
     .destroy = single_destroy}};
static const ps_result_operation_plugin_api_v2 api = {
    sizeof(api),   PS_RESULT_OPERATION_ABI_VERSION_2, operations, 2, NULL,
    plugin_destroy};
PS_RESULT_EXPORT void ps_result_atoms_counts(uint64_t* counts) {
  counts[0] = atomic_load(&starts);
  counts[1] = atomic_load(&destroys);
  counts[2] = atomic_load(&polls);
  counts[3] = atomic_load(&singles);
}
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}
