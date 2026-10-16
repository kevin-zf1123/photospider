#include <stdint.h>
#include <string.h>

#include "photospider/plugin/result_tile_helpers.h"

static const ps_result_tensor_spec_v5 tensor = {
    .struct_size = sizeof(tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V5,
    .rank = 1,
    .shape = {1},
    .channel_axis = PS_RESULT_NO_CHANNEL_V5};
static const ps_result_schema_v5 schema = {.struct_size = sizeof(schema),
                                           .id = "test.shared",
                                           .id_size = 11,
                                           .version = 1,
                                           .tensors = &tensor,
                                           .tensor_count = 1};
static const ps_result_port_v5 input = {.struct_size = sizeof(input),
                                        .schema = &schema};
static const uint32_t projection[] = {0};
static const ps_result_output_v5 outputs[] = {
    {.struct_size = sizeof(ps_result_output_v5),
     .key = "first",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v5), .schema = &schema}},
    {.struct_size = sizeof(ps_result_output_v5),
     .key = "second",
     .key_size = 6,
     .port = {.struct_size = sizeof(ps_result_port_v5), .schema = &schema},
     .input_indices = projection,
     .input_count = 1}};
static const ps_result_shared_stage_v5 stage = {
    .struct_size = sizeof(stage),
    .key = "compute",
    .key_size = 7,
    .schema = &schema,
    .input_indices = projection,
    .input_count = 1,
    .backend = 1,
    .state_bytes = sizeof(uint32_t),
    .workspace_bytes = 4096,
    .maximum_work = 100000,
    .maximum_output_payload_bytes = 8,
    .maximum_stages = 2};
static int start(void* user, void* state, const ps_result_query_v5* query,
                 const ps_result_services_v5* host) {
  (void)user;
  (void)query;
  (void)host;
  *(uint32_t*)state = 0;
  return 0;
}
static int poll(void* user, void* state, const ps_result_query_v5* query,
                const ps_result_services_v5* host) {
  (void)user;
  const ps_result_region_v5 region = {.struct_size = sizeof(region),
                                      .rank = 1,
                                      .extent = {1}};
  double value = 1;
  if (query->shared_stage != UINT32_MAX) {
    if (!(*(uint32_t*)state)++) {
      const ps_result_tile_interval_v5 interval = {0, 1};
      const ps_result_tile_need_v5 need = {sizeof(need), 0, 0, 0, &interval, 1};
      const int status = host->need_tiles(host->context, &need);
      return status ? status : PS_RESULT_NEED_V5;
    }
    const uint64_t coordinate = 0;
    if (host->read_tensor(host->context, 0, 0, &coordinate, 1, &value, 8))
      return 1;
  } else if (query->output_index == 1) {
    if (!(*(uint32_t*)state)++) {
      const int status = host->need_shared_stage(host->context, 0);
      return status ? status : PS_RESULT_NEED_V5;
    }
    ps_result_tensor_window_v5 window = {.struct_size = sizeof(window)};
    uint64_t handle = 0, coordinate = 0;
    ps_result_tensor_row_v5 row = {.struct_size = sizeof(row)};
    if (host->acquire_shared_window(host->context, 0, 0, &region, &window,
                                    &handle))
      return 1;
    if (window.row(window.context, &coordinate, 1, &row))
      return 1;
    memcpy(&value, row.data, 8);
    if (host->release_window(host->context, handle))
      return 1;
  }
  if (host->begin_result(host->context) ||
      host->publish_tensor(host->context, 0, &region, (const uint8_t*)&value,
                           8) ||
      host->publish_result(host->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V5;
}
static void destroy(void* user, void* state) {
  (void)user;
  (void)state;
}
static void retire(void* user) {
  (void)user;
}
static const ps_result_operation_v5 operation = {
    .struct_size = sizeof(operation),
    .key = "test.shared",
    .key_size = 11,
    .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_DETERMINISTIC_V5 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
    .inputs = &input,
    .input_count = 1,
    .outputs = outputs,
    .output_count = 2,
    .state_bytes = sizeof(uint32_t),
    .workspace_bytes = 4096,
    .maximum_stages = 2,
    .start = start,
    .poll = poll,
    .destroy = destroy,
    .shared_stages = &stage,
    .shared_stage_count = 1};
static const ps_result_operation_plugin_api_v5 api = {
    .struct_size = sizeof(api),
    .abi_version = PS_RESULT_OPERATION_ABI_VERSION_5,
    .operations = &operation,
    .operation_count = 1,
    .destroy = retire};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v5*
ps_result_operation_plugin_get_api_v5(void) {
  return &api;
}
