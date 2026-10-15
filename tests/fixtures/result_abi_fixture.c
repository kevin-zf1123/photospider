#include <stdint.h>
#include <string.h>

#include "photospider/plugin/result_tile_helpers.h"

#ifndef PS_TEST_ABI_MODE
#define PS_TEST_ABI_MODE 0
#endif
static const ps_result_tensor_spec_v4 tensor = {
    .struct_size = sizeof(tensor),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT32_V4,
    .rank = 1,
    .shape = {2},
    .channel_axis = PS_RESULT_NO_CHANNEL_V4};
static const ps_result_schema_v4 schema = {
    .struct_size = sizeof(schema),
    .id = "test.abi",
    .id_size = 8,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V4,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_port_v4 input = {.struct_size = sizeof(input),
                                        .kind = PS_RESULT_OBJECT_V4,
                                        .schema = &schema};
static const ps_result_output_v4 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v4),
             .kind = PS_RESULT_OBJECT_V4,
             .schema = &schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_WHOLE_V4,
    .flags = PS_RESULT_OUTPUT_PRESERVE_VIEWS_V4,
    .failure_delivery = PS_TEST_ABI_MODE == 6 ? 1 : 0};
static void (*lifetime_event)(void*, uint32_t);
static void* lifetime_user;
PS_RESULT_EXPORT void ps_result_abi_set_lifetime_event(void (*event)(void*,
                                                                     uint32_t),
                                                       void* user) {
  lifetime_event = event;
  lifetime_user = user;
}
struct State {
  unsigned stage;
};
static int start(void* user, void* raw, const ps_result_query_v4* q,
                 const ps_result_services_v4* s) {
  (void)user;
  (void)q;
  ((struct State*)raw)->stage = 0;
  return s->abi_version == PS_RESULT_OPERATION_ABI_VERSION_4 &&
                 s->struct_size == sizeof(*s)
             ? 0
             : 6;
}
static void destroy(void* user, void* raw) {
  (void)user;
  (void)raw;
  if (lifetime_event)
    lifetime_event(lifetime_user, 1);
}
static void destroy_api(void* user) {
  (void)user;
  if (lifetime_event)
    lifetime_event(lifetime_user, 2);
}
static int poll(void* user, void* raw, const ps_result_query_v4* q,
                const ps_result_services_v4* s) {
  (void)user;
  struct State* state = (struct State*)raw;
  ps_result_region_v4 region;
  if (ps_result_output_region_v4(q, 0, &region))
    return 6;
  if (q->input_count && !state->stage++)
    return ps_result_need_whole_v4(s, 0) ? 1 : PS_RESULT_NEED_V4;
  if (s->begin_result(s->context))
    return 1;
  if (!q->input_count) {
    if (lifetime_event)
      lifetime_event(lifetime_user, 0);
    uint8_t* bytes = NULL;
    const float values[2] = {3, 7};
    if (s->allocate_scratch(s->context, sizeof(values), &bytes))
      return 1;
    memcpy(bytes, values, sizeof(values));
    if (s->publish_tensor_buffer(s->context, 0, &region, bytes, sizeof(values)))
      return 1;
  } else {
    ps_result_tensor_window_v4 window = {.struct_size = sizeof(window)};
    uint64_t handle = 0;
    if (s->acquire_tensor_window(s->context, 0, 0, &region, &window, &handle))
      return 1;
    const ps_result_view_axis_v4 axis = {.output_axis = 0,
                                         .source_origin = 1,
                                         .step = -1,
                                         .extent = 1};
    const ps_result_tensor_transform_v4 transform = {
        .struct_size = sizeof(transform),
        .source_rank = 1,
        .axes = &axis};
    if (s->publish_tensor_view(s->context, 0, &region, &handle, 1,
                               &transform) ||
        s->release_window(s->context, handle))
      return 1;
  }
  return s->publish_result(s->context, 1) ? 1 : PS_RESULT_PUBLISH_V4;
}
static const ps_result_operation_v4 operations[] = {
    {.struct_size =
         sizeof(ps_result_operation_v4) - (PS_TEST_ABI_MODE == 3 ? 4 : 0),
     .key = "test.c_buffer",
     .key_size = sizeof("test.c_buffer") - 1,
     .flags = PS_RESULT_FLAG_CPU_V4 | PS_RESULT_FLAG_DETERMINISTIC_V4 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V4,
     .inputs = NULL,
     .input_count = 0,
     .outputs = &output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .maximum_stages = 2,
     .workspace_bytes = 4096,
     .start = start,
     .poll = poll,
     .destroy = destroy},
    {.struct_size =
         sizeof(ps_result_operation_v4) - (PS_TEST_ABI_MODE == 3 ? 4 : 0),
     .key = "test.c_reverse_view",
     .key_size = sizeof("test.c_reverse_view") - 1,
     .flags = PS_RESULT_FLAG_CPU_V4 | PS_RESULT_FLAG_DETERMINISTIC_V4 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V4,
     .inputs = &input,
     .input_count = 1,
     .outputs = &output,
     .output_count = 1,
     .state_bytes = sizeof(struct State),
     .maximum_stages = 2,
     .workspace_bytes = 4096,
     .start = start,
     .poll = poll,
     .destroy = destroy}};
static const ps_result_operation_plugin_api_v4 api = {
    .struct_size = sizeof(api) - (PS_TEST_ABI_MODE == 2 ? 4 : 0),
    .abi_version =
        PS_TEST_ABI_MODE == 1 ? 2 : PS_RESULT_OPERATION_ABI_VERSION_4,
    .operations = operations,
    .operation_count = 2,
    .destroy = destroy_api};
#if PS_TEST_ABI_MODE == 4
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v4*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}
#elif PS_TEST_ABI_MODE == 5
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v4*
ps_result_operation_plugin_get_api_v1(void) {
  return &api;
}
#else
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v4*
ps_result_operation_plugin_get_api_v4(void) {
  return &api;
}
#endif
