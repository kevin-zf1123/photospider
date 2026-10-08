#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "photospider/plugin/result_operation_plugin_api.h"

static const ps_result_tensor_spec_v2 tensor = {
    .struct_size = sizeof(tensor),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {5},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 schema = {
    .struct_size = sizeof(schema),
    .id = "fixture.checkpoint.input",
    .id_size = 24,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_tensor_spec_v2 state_tensor = {
    .struct_size = sizeof(state_tensor),
    .key = "carry",
    .key_size = 5,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {2},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 state_schema = {
    .struct_size = sizeof(state_schema),
    .id = "fixture.checkpoint.state",
    .id_size = 24,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &state_tensor,
    .tensor_count = 1};
static const ps_result_port_v2 input = {.struct_size = sizeof(input),
                                        .kind = PS_RESULT_OBJECT_V2,
                                        .schema = &schema};
static ps_result_output_v2 outputs[2];
static const ps_result_parameter_descriptor_v2 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V2, 0, 1, 0, 18};
static atomic_uint_fast64_t starts, destroys, reads, restores, publishes,
    verified_copies;
static atomic_uint gate_entered;
static atomic_uint gate_release = 1;
struct State {
  unsigned mode, stage;
  uint64_t cursor, target, borrowed, pending_state;
  double carry;
  ps_result_services_v2 saved;
};
static int start(void* user, void* raw, const ps_result_query_v2* query,
                 const ps_result_services_v2* host) {
  (void)user;
  (void)host;
  struct State* state = raw;
  state->mode = query->parameter_count ? query->parameters[0].int64_value : 0;
  state->target = query->requested_count ? query->requested[0].offset[0] : 4;
  atomic_fetch_add(&starts, 1);
  return 0;
}
static int restore(const ps_result_services_v2* host,
                   const ps_result_checkpoint_v2* checkpoint, double* carry) {
  return checkpoint->byte_size != 16 ||
         host->checkpoint_read(host->context, checkpoint->handle, 0, carry,
                               4) ||
         host->checkpoint_read(host->context, checkpoint->handle, 4,
                               (uint8_t*)carry + 4, 4);
}
static int prohibited_block(const ps_result_block_services_v2* block,
                            uint64_t incoming, uint64_t* outgoing, void* user) {
  (void)block;
  const ps_result_services_v2* outer = user;
  ps_result_checkpoint_v2 ignored = {.struct_size = sizeof(ignored)};
  (void)outer->checkpoint_before(outer->context, 1, 0, &ignored);
  *outgoing = incoming;
  return 0;
}
static int save(const ps_result_services_v2* host, struct State* state) {
  double encoded[2] = {state->carry, 123.5};
  uint8_t expected[6], actual[6];
  memcpy(expected, (uint8_t*)encoded + 6, sizeof(expected));
  uint64_t handle = 0;
  if (host->create_block_state(host->context, &state_schema,
                               (const uint8_t*)encoded, sizeof(encoded),
                               &handle))
    return 1;
  memset(encoded, 0xff, sizeof(encoded));
  if (state->mode == 17) {
    uint64_t outgoing = 0;
    (void)host->block(host->context, 7, 0, 1, 1, handle, prohibited_block,
                      (void*)host, &outgoing);
  }
  if (state->mode == 15 && host->release_block_state(host->context, handle))
    return 1;
  if (host->checkpoint_publish(host->context, state->mode == 14 ? 0 : 1,
                               state->cursor, handle) ||
      host->release_block_state(host->context, handle))
    return 1;
  atomic_fetch_add(&publishes, 1);
  ps_result_checkpoint_v2 copy = {.struct_size = sizeof(copy)};
  if (host->checkpoint_before(host->context, 1, state->cursor, &copy))
    return 1;
  if (copy.handle) {
    if (state->mode == 18) {
      atomic_store(&gate_entered, 1);
      while (!atomic_load(&gate_release)) {
        const struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
      }
    }
    double carry = 0;
    if (restore(host, &copy, &carry) || carry != state->carry ||
        host->checkpoint_read(host->context, copy.handle, 6, actual,
                              sizeof(actual)) ||
        memcmp(actual, expected, sizeof(actual))) {
      return 1;
    }
    atomic_fetch_add(&verified_copies, 1);
    state->borrowed = copy.handle;
    state->saved = *host;
    if (state->mode == 4)
      (void)host->checkpoint_read(host->context, copy.handle, 15, actual, 2);
    if (state->mode == 6)
      (void)host->checkpoint_publish(host->context, 1, state->cursor,
                                     copy.handle);
    if (state->mode == 11)
      (void)host->checkpoint_read(host->context, copy.handle, 0, actual, 0);
  }
  return 0;
}
static int publish(const ps_result_services_v2* host, struct State* state) {
  const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                     .rank = 1,
                                     .offset = {state->target},
                                     .extent = {1}};
  const ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 0};
  uint64_t relation = 0;
  if (host->begin_result(host->context) ||
      host->bind_descriptor(host->context, &descriptor, 1,
                            PS_RESULT_EXACT_V2) ||
      host->make_prefix(host->context, 0, 0, 0, 5, &relation) ||
      host->publish_tensor_with_relation(
          host->context, 0, &point, (const uint8_t*)&state->carry,
          sizeof(state->carry), relation, PS_RESULT_FINAL_V2) ||
      host->release_relation(host->context, relation) ||
      host->publish_result(host->context, 1))
    return PS_RESULT_STATUS_FAILURE_V2;
  return PS_RESULT_PUBLISH_V2;
}
static int poll(void* user, void* raw, const ps_result_query_v2* query,
                const ps_result_services_v2* host) {
  (void)user;
  (void)query;
  struct State* state = raw;
  if (!state->stage) {
    ps_result_checkpoint_v2 checkpoint = {.struct_size = sizeof(checkpoint)};
    double ignored;
    if (state->mode == 1)
      (void)host->checkpoint_publish(host->context, 1, 0, UINT64_MAX);
    if (state->mode == 2)
      checkpoint.struct_size = 0;
    if (state->mode == 3)
      (void)host->checkpoint_read(host->context, UINT64_MAX, 0, &ignored, 8);
    if (state->mode == 13)
      checkpoint.reserved = 1;
    if (host->checkpoint_before(host->context, state->mode == 7 ? 0 : 1,
                                state->target, &checkpoint))
      return PS_RESULT_STATUS_FAILURE_V2;
    if (checkpoint.handle) {
      if (restore(host, &checkpoint, &state->carry))
        return PS_RESULT_STATUS_FAILURE_V2;
      atomic_fetch_add(&restores, 1);
      state->cursor = checkpoint.sequence + 1;
      if (state->mode == 8) {
        uint64_t at = 0;
        (void)host->read_tensor(host->context, 0, 0, &at, 1, &ignored, 8);
      }
    }
    if (state->mode == 12) {
      const double retained[2] = {42, 123.5};
      if (host->create_block_state(host->context, &state_schema,
                                   (const uint8_t*)retained, sizeof(retained),
                                   &state->pending_state))
        return PS_RESULT_STATUS_FAILURE_V2;
    }
    state->stage = 1;
  } else {
    double value = 0;
    if (state->pending_state) {
      ps_result_checkpoint_v2 copy = {.struct_size = sizeof(copy)};
      if (host->checkpoint_publish(host->context, 2, 17,
                                   state->pending_state) ||
          host->release_block_state(host->context, state->pending_state) ||
          host->checkpoint_before(host->context, 2, 17, &copy) ||
          !copy.handle || restore(host, &copy, &value) || value != 42)
        return PS_RESULT_STATUS_FAILURE_V2;
      state->pending_state = 0;
    }
    if (state->mode == 5 && state->borrowed)
      (void)host->checkpoint_read(host->context, state->borrowed, 0, &value, 8);
    if (state->mode == 9 && state->borrowed)
      (void)state->saved.checkpoint_read(state->saved.context, state->borrowed,
                                         0, &value, 8);
    if (host->read_tensor(host->context, 0, 0, &state->cursor, 1, &value, 8))
      return PS_RESULT_STATUS_FAILURE_V2;
    atomic_fetch_add(&reads, 1);
    if (!isfinite(value))
      return PS_RESULT_STATUS_FAILURE_V2;
    volatile double sum = state->carry + value;
    if (!isfinite(sum))
      return PS_RESULT_STATUS_FAILURE_V2;
    state->carry = sum;
    if (save(host, state))
      return PS_RESULT_STATUS_FAILURE_V2;
    ++state->cursor;
  }
  if (state->cursor > state->target) {
    if ((state->mode == 8 || state->mode == 16) && state->target == 2) {
      atomic_store(&gate_entered, 1);
      while (!atomic_load(&gate_release)) {
        const struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
        if (host->consume_work(host->context, 0))
          return PS_RESULT_STATUS_FAILURE_V2;
      }
    }
    return publish(host, state);
  }
  const ps_result_region_v2 point = {.struct_size = sizeof(point),
                                     .rank = 1,
                                     .offset = {state->cursor},
                                     .extent = {1}};
  return host->need_tensor(host->context, 0, 0, 13, &point, 1)
             ? PS_RESULT_STATUS_FAILURE_V2
             : PS_RESULT_NEED_V2;
}
static void destroy(void* user, void* raw) {
  (void)user;
  (void)raw;
  atomic_fetch_add(&destroys, 1);
}
static void plugin_destroy(void* user) {
  (void)user;
}
static ps_result_operation_v2 operations[2];
static const ps_result_operation_plugin_api_v2 api = {
    sizeof(api),   PS_RESULT_OPERATION_ABI_VERSION_2, operations, 2, NULL,
    plugin_destroy};
PS_RESULT_EXPORT void ps_result_checkpoint_counts(uint64_t* counts) {
  counts[0] = atomic_load(&starts);
  counts[1] = atomic_load(&destroys);
  counts[2] = atomic_load(&reads);
  counts[3] = atomic_load(&restores);
  counts[4] = atomic_load(&publishes);
  counts[5] = atomic_load(&verified_copies);
}
PS_RESULT_EXPORT unsigned ps_result_checkpoint_gate(unsigned action) {
  if (action == 1) {
    atomic_store(&gate_entered, 0);
    atomic_store(&gate_release, 0);
  } else if (action == 2) {
    atomic_store(&gate_release, 1);
  }
  return atomic_load(&gate_entered);
}
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  const char* keys[] = {"fixture.result_checkpoint",
                        "fixture.terminal_checkpoint"};
  for (unsigned i = 0; i < 2; ++i) {
    outputs[i] = (ps_result_output_v2){
        .struct_size = sizeof(outputs[i]),
        .key = "value",
        .key_size = 5,
        .port = {.struct_size = sizeof(ps_result_port_v2),
                 .kind = PS_RESULT_OBJECT_V2,
                 .schema = &schema},
        .input_count = UINT32_MAX,
        .execution = PS_RESULT_REGIONAL_V2,
        .observation_kind = i == 1 ? PS_RESULT_REQUEST_RECORD_V2 : 0};
    operations[i] = (ps_result_operation_v2){
        .struct_size = sizeof(operations[i]),
        .key = keys[i],
        .key_size = strlen(keys[i]),
        .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
                 PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
        .inputs = &input,
        .input_count = 1,
        .outputs = outputs + i,
        .output_count = 1,
        .parameters = &mode,
        .parameter_count = 1,
        .state_bytes = sizeof(struct State),
        .workspace_bytes = 64,
        .maximum_stages = 16,
        .start = start,
        .poll = poll,
        .destroy = destroy};
  }
  return &api;
}
