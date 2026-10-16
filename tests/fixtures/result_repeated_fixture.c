#include <stdint.h>

#include "photospider/plugin/result_operation_plugin_api.h"

// Preserve the historical case id for the missing heterogeneous resolver.
#define PS_BAD_RESULT_TABLE_COUNT 6U
#include "bad_result_table_support.h"

static const ps_result_tensor_spec_v5 tensor = {
    .struct_size = sizeof(tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V5,
    .rank = 1,
    .shape = {3},
    .channel_axis = PS_RESULT_NO_CHANNEL_V5};
static const ps_result_schema_v5 schema = {.struct_size = sizeof(schema),
                                           .id = "fixture.repeated",
                                           .id_size = 16,
                                           .version = 1,
                                           .tensors = &tensor,
                                           .tensor_count = 1};
static const ps_result_port_v5 group = {.struct_size = sizeof(group),
                                        .schema = &schema};
static const ps_result_output_v5 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v5), .schema = &schema},
    .input_count = UINT32_MAX};
static int start(void* user, void* state, const ps_result_query_v5* query,
                 const ps_result_services_v5* services) {
  (void)user;
  (void)state;
  (void)query;
  (void)services;
  return 0;
}
static int poll(void* user, void* state, const ps_result_query_v5* query,
                const ps_result_services_v5* services) {
  (void)user;
  (void)state;
  (void)query;
  (void)services;
  return PS_RESULT_STATUS_FAILURE_V5;
}
static void destroy(void* user, void* state) {
  (void)user;
  (void)state;
}
static void retire(void* user) {
  (void)user;
  ps_test_bad_result_retired();
}
static ps_result_operation_v5 operation = {
    .struct_size = sizeof(operation),
    .key = "fixture.repeated.fixed",
    .key_size = sizeof("fixture.repeated.fixed") - 1,
    .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
    .outputs = &output,
    .output_count = 1,
    .state_bytes = 1,
    .workspace_bytes = 2048,
    .maximum_stages = 1,
    .start = start,
    .poll = poll,
    .destroy = destroy,
    .repeated_input = &group,
    .repeated_minimum = 1,
    .repeated_maximum = 4,
    .repeated_match = 1};
static const ps_result_operation_plugin_api_v5 api = {
    .struct_size = sizeof(api),
    .abi_version = PS_RESULT_OPERATION_ABI_VERSION_5,
    .operations = &operation,
    .operation_count = 1,
    .destroy = retire};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v5*
ps_result_operation_plugin_get_api_v5(void) {
#ifdef PS_RESULT_BAD_TABLES
  operation.repeated_match = ps_test_bad_case == 6 ? 0 : 1;
#endif
  return &api;
}
