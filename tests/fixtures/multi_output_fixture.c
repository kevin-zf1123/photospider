#include <stddef.h>

#include "photospider/plugin/result_operation_plugin_api.h"

static const ps_result_tensor_spec_v3 tensor = {
    .struct_size = sizeof(tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V3,
    .rank = 1,
    .shape = {1},
    .channel_axis = PS_RESULT_NO_CHANNEL_V3};
static const ps_result_schema_v3 schema = {
    .struct_size = sizeof(schema),
    .id = "test.multi_output",
    .id_size = 17,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V3,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_output_v3 outputs[] = {
    {.struct_size = sizeof(ps_result_output_v3),
     .key = "first",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v3),
              .kind = PS_RESULT_OBJECT_V3,
              .schema = &schema},
     .execution = PS_RESULT_WHOLE_V3},
    {.struct_size = sizeof(ps_result_output_v3),
     .key = "second",
     .key_size = 6,
     .port = {.struct_size = sizeof(ps_result_port_v3),
              .kind = PS_RESULT_OBJECT_V3,
              .schema = &schema},
     .execution = PS_RESULT_WHOLE_V3}};
static int publish(void* user, void* state, const ps_result_query_v3* query,
                   const ps_result_services_v3* services) {
  (void)user;
  (void)state;
  const double number = 10.0 + query->output_index;
  const ps_result_region_v3 whole = {.struct_size = sizeof(whole),
                                     .rank = 1,
                                     .extent = {1}};
  if (services->begin_result(services->context))
    return 1;
  if (!(!query->requested.whole && !query->requested.tile_count) &&
      services->publish_tensor(services->context, 0, &whole,
                               (const uint8_t*)&number, sizeof(number)))
    return 1;
  return services->publish_result(services->context, 1) ? 1
                                                        : PS_RESULT_PUBLISH_V3;
}
static int start(void* user, void* state, const ps_result_query_v3* query,
                 const ps_result_services_v3* services) {
  (void)user;
  (void)state;
  (void)query;
  (void)services;
  return 0;
}
static void destroy_state(void* user, void* state) {
  (void)user;
  (void)state;
}
static void destroy(void* context) {
  (void)context;
}
static const ps_result_operation_v3 operation = {
    .struct_size = sizeof(operation),
    .key = "test.c_results",
    .key_size = 14,
    .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_DETERMINISTIC_V3 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
    .outputs = outputs,
    .output_count = 2,
    .maximum_stages = 1,
    .start = start,
    .poll = publish,
    .destroy = destroy_state};
static const ps_result_operation_plugin_api_v3 api = {
    .struct_size = sizeof(api),
    .abi_version = PS_RESULT_OPERATION_ABI_VERSION_3,
    .operations = &operation,
    .operation_count = 1,
    .destroy = destroy};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v3*
ps_result_operation_plugin_get_api_v3(void) {
  return &api;
}
