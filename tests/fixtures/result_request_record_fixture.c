#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"

#define PS_BAD_RESULT_TABLE_COUNT 4U
#include "bad_result_table_support.h"

static const ps_result_tensor_spec_v2 tensor = {
    .struct_size = sizeof(tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {5},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 schema = {
    .struct_size = sizeof(schema),
    .id = "test.request_record",
    .id_size = 19,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_port_v2 input = {.struct_size = sizeof(input),
                                        .kind = PS_RESULT_OBJECT_V2,
                                        .schema = &schema};
static ps_result_output_v2 outputs[] = {
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "value",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = PS_RESULT_WHOLE_V2,
     .observation_kind = PS_RESULT_REQUEST_RECORD_V2,
     .failure_delivery = PS_RESULT_REQUEST_FAILURE_ONLY_V2},
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "local",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = PS_RESULT_REGIONAL_V2,
     .observation_kind = PS_RESULT_ATOMIC_V2,
     .failure_delivery = PS_RESULT_REQUEST_FAILURE_ONLY_V2}};
static const ps_result_parameter_descriptor_v2 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V2, 0, 1, 0, 3};
static atomic_uint_fast64_t starts, destroys;
struct State {
  unsigned stage;
};
static int start(void* user, void* raw, const ps_result_query_v2* query,
                 const ps_result_services_v2* services) {
  (void)user;
  (void)raw;
  atomic_fetch_add(&starts, 1);
  return query->output->observation_kind == (query->output_index == 0
                                                 ? PS_RESULT_REQUEST_RECORD_V2
                                                 : PS_RESULT_ATOMIC_V2) &&
                 query->output->failure_delivery ==
                     PS_RESULT_REQUEST_FAILURE_ONLY_V2 &&
                 services->abi_version == PS_RESULT_OPERATION_ABI_VERSION_2
             ? 0
             : PS_RESULT_STATUS_FAILURE_V2;
}
static int poll(void* user, void* raw, const ps_result_query_v2* query,
                const ps_result_services_v2* services) {
  (void)user;
  struct State* state = (struct State*)raw;
  const int64_t selected_mode =
      query->parameter_count ? query->parameters[0].int64_value : 0;
  const ps_result_region_v2 whole = {.struct_size = sizeof(whole),
                                     .rank = 1,
                                     .extent = {5}};
  const ps_result_region_v2* regions =
      query->requested_kind == 2 && selected_mode != 1 ? query->requested
                                                       : &whole;
  const uint32_t count = query->requested_kind == 2 && selected_mode != 1
                             ? query->requested_count
                             : 1;
  if (!state->stage++)
    return services->need_tensor(services->context, 0, 0, 9, regions, count)
               ? PS_RESULT_STATUS_FAILURE_V2
               : PS_RESULT_NEED_V2;
  if (selected_mode == 3) {
    const uint64_t outside = 4;
    double ignored = 0;
    (void)services->read_tensor(services->context, 0, 0, &outside, 1, &ignored,
                                8);
    return PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V2;
  }
  uint64_t cardinality = query->requested_kind == 2 ? 0 : 5;
  if (query->requested_kind == 2)
    for (uint32_t r = 0; r < query->requested_count; ++r)
      cardinality += query->requested[r].extent[0];
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, NULL, 0, PS_RESULT_EXACT_V2))
    return PS_RESULT_STATUS_FAILURE_V2;
  for (uint32_t r = 0; r < count; ++r) {
    const ps_result_region_v2* region = regions + r;
    if (region->rank != 1 || region->extent[0] > 5)
      return PS_RESULT_STATUS_FAILURE_V2;
    double values[5];
    ps_result_relation_row_v2 relation[5];
    for (uint64_t i = 0; i < region->extent[0]; ++i) {
      const uint64_t at = region->offset[0] + i;
      if (services->read_tensor(services->context, 0, 0, &at, 1, values + i, 8))
        return PS_RESULT_STATUS_FAILURE_V2;
      if (query->output->observation_kind == PS_RESULT_REQUEST_RECORD_V2)
        values[i] += (double)cardinality;
      relation[i] = (ps_result_relation_row_v2){
          at, 0, 1, PS_RESULT_TARGET_TENSOR_V2, 0, at, 1};
    }
    if (services->publish_tensor(services->context, 0, region,
                                 (const uint8_t*)values, region->extent[0] * 8,
                                 relation, (uint32_t)region->extent[0],
                                 PS_RESULT_EXACT_V2, PS_RESULT_FINAL_V2))
      return PS_RESULT_STATUS_FAILURE_V2;
  }
  if (services->publish_result(services->context, selected_mode != 2))
    return PS_RESULT_STATUS_FAILURE_V2;
  return PS_RESULT_PUBLISH_V2;
}
static void destroy(void* user, void* raw) {
  (void)user;
  (void)raw;
  atomic_fetch_add(&destroys, 1);
}
static void destroy_plugin(void* user) {
  (void)user;
  ps_test_bad_result_retired();
}
static ps_result_operation_v2 operation = {
    .struct_size = sizeof(operation),
    .key = "fixture.request_record",
    .key_size = 22,
    .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
    .inputs = &input,
    .input_count = 1,
    .outputs = outputs,
    .output_count = 2,
    .parameters = &mode,
    .parameter_count = 1,
    .state_bytes = sizeof(struct State),
    .maximum_stages = 2,
    .start = start,
    .poll = poll,
    .destroy = destroy};
static const ps_result_operation_plugin_api_v2 api = {
    sizeof(api),   PS_RESULT_OPERATION_ABI_VERSION_2, &operation, 1, NULL,
    destroy_plugin};
PS_RESULT_EXPORT void ps_request_record_counts(uint64_t* entered,
                                               uint64_t* retired) {
  *entered = atomic_load(&starts);
  *retired = atomic_load(&destroys);
}
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
#ifdef PS_RESULT_BAD_TABLES
  static ps_result_operation_plugin_api_v2 bad_api;
  static ps_result_operation_v2 bad_operation;
  static ps_result_output_v2 bad_outputs[sizeof(outputs) / sizeof(outputs[0])];
  bad_api = api;
  bad_operation = operation;
  memcpy(bad_outputs, outputs, sizeof(outputs));
  bad_operation.outputs = bad_outputs;
  bad_api.operations = &bad_operation;
  switch (ps_test_bad_case) {
    case 1:
      bad_outputs[0].observation_kind = 2;
      break;
    case 2:
      bad_outputs[0].failure_delivery = 1;
      break;
    case 3:
      bad_outputs[0].struct_size =
          offsetof(ps_result_output_v2, observation_kind);
      break;
    case 4:
      bad_outputs[1].failure_delivery = UINT32_MAX;
      break;
  }
  return &bad_api;
#endif
  return &api;
}
