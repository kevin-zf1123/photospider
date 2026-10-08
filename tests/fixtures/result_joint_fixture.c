#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"

#define PS_BAD_RESULT_TABLE_COUNT 10U
#include "bad_result_table_support.h"

static const ps_result_tensor_spec_v2 tensor = {
    .struct_size = sizeof(tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {1},
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
static ps_result_output_v2 outputs[] = {
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "left",
     .key_size = 4,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = PS_RESULT_REGIONAL_V2},
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "right",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = PS_RESULT_REGIONAL_V2}};
static ps_result_tensor_spec_v2 roi_tensor;
static ps_result_schema_v2 roi_schema;
static ps_result_output_v2 roi_outputs[2];
static ps_result_tensor_spec_v2 wide_tensor;
static ps_result_schema_v2 wide_schema;
static ps_result_output_v2 wide_outputs[2];
static const ps_result_parameter_descriptor_v2 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V2, 0, 1, 0, 12};
static atomic_uint_fast64_t starts, destroys, polls, singles, destroy_reads,
    expired_rejections;
struct State {
  unsigned stage, mode;
  ps_result_services_v2 saved_member;
  ps_result_joint_services_v2 saved_group;
  ps_result_tensor_window_v2 window;
};
static int selected(const ps_result_query_v2* query) {
  return query->parameter_count ? (int)query->parameters[0].int64_value : 0;
}
static int publish(const ps_result_services_v2* service,
                   const ps_result_query_v2* query, double value, int dependent,
                   unsigned complete) {
  if (query->requested_count != 1 || query->requested[0].rank != 1 ||
      query->requested[0].extent[0] != 1)
    return PS_RESULT_STATUS_FAILURE_V2;
  const ps_result_region_v2 point = query->requested[0];
  const ps_result_relation_row_v2 support = {
      0, 0, 1, PS_RESULT_TARGET_TENSOR_V2, 0, 0, 1};
  if (service->begin_result(service->context) ||
      service->bind_descriptor(service->context, NULL, 0, PS_RESULT_EXACT_V2) ||
      service->publish_tensor(service->context, 0, &point,
                              (const uint8_t*)&value, sizeof(value),
                              dependent ? &support : NULL, dependent ? 1 : 0,
                              PS_RESULT_EXACT_V2, PS_RESULT_FINAL_V2) ||
      service->publish_result(service->context, complete))
    return PS_RESULT_STATUS_FAILURE_V2;
  return PS_RESULT_PUBLISH_V2;
}
static int singleton_start(void* user, void* raw,
                           const ps_result_query_v2* query,
                           const ps_result_services_v2* service) {
  (void)user;
  (void)service;
  ((struct State*)raw)->mode = (unsigned)selected(query);
  atomic_fetch_add(&singles, 1);
  return 0;
}
static int singleton_poll(void* user, void* raw,
                          const ps_result_query_v2* query,
                          const ps_result_services_v2* service) {
  (void)user;
  struct State* state = (struct State*)raw;
  if (query->output_index == 0 && !state->stage++) {
    const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                       .rank = 1,
                                       .extent = {1}};
    return service->need_tensor(service->context, 0, 0, 9, &point, 1)
               ? PS_RESULT_STATUS_FAILURE_V2
               : PS_RESULT_NEED_V2;
  }
  double value = 11;
  if (query->output_index == 0) {
    const uint64_t at = 0;
    double source = 0;
    if (service->read_tensor(service->context, 0, 0, &at, 1, &source,
                             sizeof(source)))
      return PS_RESULT_STATUS_FAILURE_V2;
    value = source + 4;
  }
  return publish(service, query, value, query->output_index == 0, 1);
}

static void singleton_destroy(void* user, void* raw) {
  (void)user;
  (void)raw;
}
static int joint_start(const ps_result_joint_query_v2* queries, uint32_t count,
                       void* raw, uint64_t bytes, void* user) {
  (void)user;
  atomic_fetch_add(&starts, 1);
  if (!count || bytes != sizeof(struct State))
    return PS_RESULT_STATUS_FAILURE_V2;
  const uint8_t* zeroed = (const uint8_t*)raw;
  for (uint64_t i = 0; i < bytes; ++i)
    if (zeroed[i])
      return PS_RESULT_STATUS_FAILURE_V2;
  struct State* state = (struct State*)raw;
  state->mode = (unsigned)selected(queries[0].query);
  if (state->mode == 12 && count != 1)
    return PS_RESULT_STATUS_FAILURE_V2;
  return state->mode == 7 ? PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V2 : 0;
}
static int joint_poll(const ps_result_joint_member_v2* members, uint32_t count,
                      void* raw, const ps_result_joint_services_v2* shared,
                      ps_result_joint_outcome_v2* outcomes, uint32_t* capacity,
                      void* user) {
  (void)user;
  struct State* state = (struct State*)raw;
  atomic_fetch_add(&polls, 1);
  if (*capacity < count || shared->struct_size != sizeof(*shared))
    return PS_RESULT_STATUS_FAILURE_V2;
  if (shared->consume_work(shared->context, 7))
    return PS_RESULT_STATUS_FAILURE_V2;
  if (state->mode == 5)
    (void)shared->consume_work(shared->context, 1000000);
  if (state->mode == 6) {
    uint8_t* ignored = NULL;
    (void)shared->scratch(shared->context, 128, &ignored);
  }
  if (state->stage && (state->mode == 9 || state->mode == 11)) {
    const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                       .rank = 1,
                                       .extent = {1}};
    (void)state->saved_member.need_tensor(state->saved_member.context, 0, 0, 1,
                                          &point, 1);
  }
  if (state->stage && state->mode == 10)
    (void)state->saved_group.consume_work(state->saved_group.context, 1);
  uint64_t handles[64] = {0};
  if (state->stage && state->mode == 8) {
    const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                       .rank = 1,
                                       .extent = {1}};
    for (uint32_t i = 0; i < count; ++i) {
      ps_result_tensor_window_v2 window = {.struct_size = sizeof(window)};
      const ps_result_services_v2* service = members[i].services;
      if (service->acquire_tensor_window(service->context, 0, 0, &point,
                                         &window, handles + i))
        return PS_RESULT_STATUS_FAILURE_V2;
    }
    if (count == 2) {
      const ps_result_services_v2* wrong = members[1].services;
      (void)wrong->release_window(wrong->context, handles[0]);
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t index = members[i].query->output_index;
    const ps_result_services_v2* service = members[i].services;
    int result;
    if (!state->stage && state->mode == 11 && index == 0) {
      state->saved_member = *service;
      result = PS_RESULT_STATUS_CANCELLED_V2;
    } else if (!state->stage &&
               (index == 0 || state->mode == 8 || state->mode == 11)) {
      const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                         .rank = 1,
                                         .extent = {1}};
      result = service->need_tensor(service->context, 0, 0, 9, &point, 1)
                   ? PS_RESULT_STATUS_FAILURE_V2
                   : PS_RESULT_NEED_V2;
    } else {
      double value = 7 + 4 * index;
      const int dependent = index == 0;
      if (dependent) {
        const uint64_t coordinate = 0;
        double source = 0;
        if (service->read_tensor(service->context, 0, 0, &coordinate, 1,
                                 &source, sizeof(source)))
          return PS_RESULT_STATUS_FAILURE_V2;
        value = source + 4 + 4 * index;
        if (state->mode == 0) {
          const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                             .rank = 1,
                                             .extent = {1}};
          uint64_t handle = 0;
          state->window.struct_size = sizeof(state->window);
          if (service->acquire_tensor_window(service->context, 0, 0, &point,
                                             &state->window, &handle))
            return PS_RESULT_STATUS_FAILURE_V2;
        }
      }
      result = publish(service, members[i].query, value, dependent,
                       !(state->mode == 4 && index == 1));
      if (index == 1 && state->mode != 11)
        state->saved_member = *service;
    }
    outcomes[count - 1 - i] =
        (ps_result_joint_outcome_v2){.struct_size = sizeof(*outcomes),
                                     .key = members[i].key,
                                     .result = result};
  }
  state->saved_group = *shared;
  ++state->stage;
  *capacity = state->mode == 1 ? count - 1 : count;
  if (state->mode == 2 && count > 1)
    outcomes[1].key = outcomes[0].key;
  if (state->mode == 3)
    outcomes[0].key.output_index = 63;
  return 0;
}
static void joint_destroy(void* raw, void* user) {
  (void)user;
  struct State* state = (struct State*)raw;
  if (state->window.row) {
    const uint64_t at = 0;
    ps_result_tensor_row_v2 row = {.struct_size = sizeof(row)};
    if (!state->window.row(state->window.context, &at, 1, &row) &&
        row.bytes >= sizeof(double)) {
      double value = 0;
      memcpy(&value, row.data, sizeof(value));
      if (value == 3)
        atomic_fetch_add(&destroy_reads, 1);
    }
  }
  if (state->saved_member.consume_work &&
      state->saved_member.consume_work(state->saved_member.context, 1) == 6)
    atomic_fetch_add(&expired_rejections, 1);
  atomic_fetch_add(&destroys, 1);
}
static void plugin_destroy(void* raw) {
  (void)raw;
  ps_test_bad_result_retired();
}
static ps_result_joint_program_v2 joint = {sizeof(joint),
                                           1,
                                           sizeof(struct State),
                                           64,
                                           joint_start,
                                           joint_poll,
                                           joint_destroy,
                                           sizeof(ps_result_joint_query_v2),
                                           sizeof(ps_result_joint_member_v2),
                                           sizeof(ps_result_joint_outcome_v2),
                                           sizeof(ps_result_joint_services_v2)};
static ps_result_operation_v2 operation = {
    .struct_size = sizeof(operation),
    .key = "fixture.result_joint",
    .key_size = 20,
    .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
    .inputs = &input,
    .input_count = 1,
    .outputs = outputs,
    .output_count = 2,
    .parameters = &mode,
    .parameter_count = 1,
    .state_bytes = sizeof(struct State),
    .maximum_stages = 4,
    .start = singleton_start,
    .poll = singleton_poll,
    .destroy = singleton_destroy,
    .joint = &joint};
static ps_result_operation_v2 operations[3];
static const ps_result_operation_plugin_api_v2 api = {
    sizeof(api),   PS_RESULT_OPERATION_ABI_VERSION_2, operations, 3, NULL,
    plugin_destroy};
PS_RESULT_EXPORT void ps_result_joint_counts(uint64_t* counts) {
  counts[0] = atomic_load(&starts);
  counts[1] = atomic_load(&destroys);
  counts[2] = atomic_load(&polls);
  counts[3] = atomic_load(&singles);
  counts[4] = atomic_load(&destroy_reads);
  counts[5] = atomic_load(&expired_rejections);
}
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
#ifdef PS_RESULT_BAD_TABLES
  static ps_result_joint_program_v2 bad_joint;
  static ps_result_output_v2 bad_outputs[sizeof(outputs) / sizeof(outputs[0])];
  ps_result_operation_v2 selected = operation;
  bad_joint = joint;
  memcpy(bad_outputs, outputs, sizeof(outputs));
  selected.joint = &bad_joint;
  selected.outputs = bad_outputs;
  switch (ps_test_bad_case) {
    case 1:
      bad_joint.struct_size = offsetof(ps_result_joint_program_v2, destroy);
      break;
    case 2:
      bad_joint.contract = 2;
      break;
    case 3:
      bad_joint.destroy = NULL;
      break;
    case 4:
      bad_joint.state_bytes = 0;
      break;
    case 5:
      bad_outputs[1].observation_kind = PS_RESULT_REQUEST_RECORD_V2;
      break;
    case 6:
      selected.struct_size = offsetof(ps_result_operation_v2, joint);
      break;
    case 7:
      bad_joint.query_size = 0;
      break;
    case 8:
      bad_joint.member_size = 0;
      break;
    case 9:
      bad_joint.outcome_size = 0;
      break;
    case 10:
      bad_joint.services_size = 0;
      break;
  }
#else
  const ps_result_operation_v2 selected = operation;
#endif
  operations[0] = selected;
  roi_tensor = tensor;
  roi_tensor.shape[0] = 2;
  roi_schema = schema;
  roi_schema.tensors = &roi_tensor;
  roi_outputs[0] = selected.outputs[0];
  roi_outputs[1] = selected.outputs[1];
  roi_outputs[1].port.schema = &roi_schema;
  operations[1] = selected;
  operations[1].key = "fixture.result_joint_roi";
  operations[1].key_size = 24;
  operations[1].outputs = roi_outputs;
  wide_tensor = tensor;
  wide_tensor.shape[0] = UINT64_C(1) << 61;
  wide_schema = schema;
  wide_schema.tensors = &wide_tensor;
  wide_outputs[0] = selected.outputs[0];
  wide_outputs[1] = selected.outputs[1];
  wide_outputs[1].port.schema = &wide_schema;
  operations[2] = selected;
  operations[2].key = "fixture.result_joint_wide";
  operations[2].key_size = 25;
  operations[2].outputs = wide_outputs;
  return &api;
}
