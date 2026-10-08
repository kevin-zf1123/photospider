#include <fenv.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "photospider/plugin/result_operation_plugin_api.h"

static const ps_result_tensor_spec_v2 tensor = {
    .struct_size = sizeof(tensor),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {6},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 schema = {
    .struct_size = sizeof(schema),
    .id = "fixture.block.input",
    .id_size = 19,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_tensor_spec_v2 carry_tensor = {
    .struct_size = sizeof(carry_tensor),
    .key = "carry",
    .key_size = 5,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {1},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 carry_schema = {
    .struct_size = sizeof(carry_schema),
    .id = "fixture.block.state",
    .id_size = 19,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &carry_tensor,
    .tensor_count = 1};
static const ps_result_port_v2 input = {.struct_size = sizeof(input),
                                        .kind = PS_RESULT_OBJECT_V2,
                                        .schema = &schema};
static const ps_result_output_v2 outputs[] = {
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "value",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = PS_RESULT_REGIONAL_V2},
    {.struct_size = sizeof(ps_result_output_v2),
     .key = "control",
     .key_size = 7,
     .port = {.struct_size = sizeof(ps_result_port_v2),
              .kind = PS_RESULT_OBJECT_V2,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = PS_RESULT_REGIONAL_V2}};
static const ps_result_parameter_descriptor_v2 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V2, 0, 1, 0, 11};
static atomic_uint_fast64_t starts, destroys, calls;
struct State {
  unsigned mode, stage, roles;
  uint64_t cursor, target, incoming;
};
static int start(void* user, void* raw, const ps_result_query_v2* query,
                 const ps_result_services_v2* host) {
  (void)user;
  struct State* state = raw;
  atomic_fetch_add(&starts, 1);
  if (query->requested_count != 1 || query->requested[0].rank != 1 ||
      query->requested[0].extent[0] != 1)
    return PS_RESULT_STATUS_FAILURE_V2;
  state->target = query->requested[0].offset[0];
  state->roles = query->output_index ? 2U : 1U;
  state->mode = query->parameter_count ? query->parameters[0].int64_value : 0;
  const double zero = 0;
  return host->create_block_state(host->context, &carry_schema,
                                  (const uint8_t*)&zero, sizeof(zero),
                                  &state->incoming);
}
static int compute_impl(const ps_result_block_services_v2* host,
                        uint64_t incoming, uint64_t* outgoing, void* user) {
  struct State* state = user;
  atomic_fetch_add(&calls, 1);
  if (state->mode == 8) {
    const uint64_t illegal = UINT64_MAX;
    double ignored = 0;
    (void)host->read_tensor(host->context, 0, 0, &illegal, 1, &ignored, 8);
    *outgoing = incoming;
    return 0;
  }
  if (state->mode == 9)
    return PS_RESULT_NEED_V2;
  if (state->mode == 10) {
    uint8_t* ignored = NULL;
    (void)host->allocate_scratch(host->context, UINT64_MAX, &ignored);
    *outgoing = incoming;
    return 0;
  }
  if (state->mode == 11)
    return PS_RESULT_STATUS_FAILURE_V2;
  double carry = 0, value = 0;
  if (host->read_state(host->context, incoming, (uint8_t*)&carry, 8) ||
      host->read_tensor(host->context, 0, 0, &state->cursor, 1, &value, 8) ||
      !isfinite(value))
    return PS_RESULT_STATUS_FAILURE_V2;
  volatile double sum = carry + value;
  if (!isfinite(sum))
    return PS_RESULT_STATUS_FAILURE_V2;
  const double result = sum;
  return host->create_state(host->context, &carry_schema,
                            (const uint8_t*)&result, 8, outgoing);
}
static int compute(const ps_result_block_services_v2* host, uint64_t incoming,
                   uint64_t* outgoing, void* user) {
  fenv_t prior;
  if (fegetenv(&prior) || fesetround(FE_TONEAREST))
    return PS_RESULT_STATUS_FAILURE_V2;
  const int result = compute_impl(host, incoming, outgoing, user);
  return fesetenv(&prior) ? PS_RESULT_STATUS_FAILURE_V2 : result;
}
static int poll(void* user, void* raw, const ps_result_query_v2* query,
                const ps_result_services_v2* host) {
  (void)user;
  (void)query;
  struct State* state = raw;
  if (state->stage) {
    uint64_t outgoing = 0;
    if (host->block(host->context, 1, state->cursor, state->cursor + 1, 1,
                    state->incoming, state->mode == 7 ? NULL : compute, state,
                    &outgoing) ||
        host->release_block_state(host->context, state->incoming))
      return PS_RESULT_STATUS_FAILURE_V2;
    state->incoming = outgoing;
    ++state->cursor;
  }
  state->stage = 1;
  if (state->cursor <= state->target) {
    const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                       .rank = 1,
                                       .offset = {state->cursor},
                                       .extent = {1}};
    return host->need_tensor(host->context, 0, 0, state->roles | 12U, &point, 1)
               ? PS_RESULT_STATUS_FAILURE_V2
               : PS_RESULT_NEED_V2;
  }
  double carry = 0;
  const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                     .rank = 1,
                                     .offset = {state->target},
                                     .extent = {1}};
  const ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 0};
  uint64_t relation = 0;
  if (host->read_block_state(host->context, state->incoming, (uint8_t*)&carry,
                             sizeof(carry)) ||
      host->release_block_state(host->context, state->incoming) ||
      host->begin_result(host->context) ||
      host->bind_descriptor(host->context, &descriptor, 1,
                            PS_RESULT_EXACT_V2) ||
      host->make_prefix(host->context, 0, 0, 0, state->roles, &relation) ||
      host->publish_tensor_with_relation(host->context, 0, &point,
                                         (const uint8_t*)&carry, 8, relation,
                                         PS_RESULT_FINAL_V2) ||
      host->release_relation(host->context, relation) ||
      host->publish_result(host->context, 1))
    return PS_RESULT_STATUS_FAILURE_V2;
  state->incoming = 0;
  return PS_RESULT_PUBLISH_V2;
}
static void destroy(void* user, void* raw) {
  (void)user;
  (void)raw;
  atomic_fetch_add(&destroys, 1);
}
static void plugin_destroy(void* user) {
  (void)user;
}
static const ps_result_operation_v2 operation = {
    .struct_size = sizeof(operation),
    .key = "fixture.result_block",
    .key_size = 20,
    .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2 |
             PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V2,
    .inputs = &input,
    .input_count = 1,
    .outputs = outputs,
    .output_count = 2,
    .parameters = &mode,
    .parameter_count = 1,
    .state_bytes = sizeof(struct State),
    .workspace_bytes = 64,
    .maximum_stages = 16,
    .start = start,
    .poll = poll,
    .destroy = destroy};
static const ps_result_operation_plugin_api_v2 api = {
    sizeof(api),   PS_RESULT_OPERATION_ABI_VERSION_2, &operation, 1, NULL,
    plugin_destroy};
PS_RESULT_EXPORT void ps_result_block_counts(uint64_t* counts) {
  counts[0] = atomic_load(&starts);
  counts[1] = atomic_load(&destroys);
  counts[2] = atomic_load(&calls);
}
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}
