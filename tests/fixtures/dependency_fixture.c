#include <stdint.h>
#include <string.h>

#include "photospider/plugin/dependency_plugin_api.h"

#ifndef PS_DEPENDENCY_BAD_CASE
#define PS_DEPENDENCY_BAD_CASE 0
#endif

struct State {
  uint64_t owner;
  uint32_t stage;
  int mode;
};
static uint32_t starts, destroys;
PS_OPERATION_EXPORT uint32_t ps_dependency_fixture_starts(void) {
  return starts;
}
PS_OPERATION_EXPORT uint32_t ps_dependency_fixture_destroys(void) {
  return destroys;
}

static int validate(const ps_dependency_metadata_query_v8* q, void* user) {
  (void)user;
  return q->input_count == 1 && q->inputs[0].rank == 1 &&
                 q->inputs[0].shape[0] >= 3
             ? PS_OPERATION_RESULT_SUCCESS_V8
             : PS_DEPENDENCY_INVALID_ARGUMENT_V8;
}
static int start(const ps_dependency_query_v8* q, void* state, uint64_t bytes,
                 void* user) {
  (void)user;
  ++starts;
  if (bytes != sizeof(struct State))
    return PS_OPERATION_RESULT_FAILURE_V8;
  const uint8_t* zero = state;
  for (uint64_t i = 0; i < bytes; ++i)
    if (zero[i])
      return PS_OPERATION_RESULT_FAILURE_V8;
  struct State* s = state;
  if (q->metadata.parameter_count)
    s->mode = (int)q->metadata.parameters[0].int64_value;
  return s->mode == 4 ? PS_OPERATION_RESULT_FAILURE_V8
                      : PS_OPERATION_RESULT_SUCCESS_V8;
}
static int need(const ps_dependency_query_v8* q,
                const ps_dependency_services_v8* host, uint64_t index) {
  ps_dependency_run_v8 run = {0};
  run.struct_size = sizeof(run);
  run.rank = 1;
  run.offsets[0] = index;
  run.extents[0] = 1;
  ps_dependency_association_v8 a = {0};
  a.struct_size = sizeof(a);
  a.output_rank = q->observation_kind ? 0 : 1;
  a.output[0] = q->observation_kind ? 0 : q->outputs[0].offsets[0];
  a.roles = 1;
  a.run_count = 1;
  a.runs = &run;
  const int mode = (int)q->metadata.parameters[0].int64_value;
  ps_dependency_run_v8 repeated[17];
  for (uint32_t i = 0; i < 17; ++i)
    repeated[i] = run;
  if (mode == 6 || mode == 8) {
    a.runs = repeated;
    a.run_count = mode == 6 ? 17 : 4;
    a.roles = mode == 8 ? 15 : 1;
  }
  if (mode == 7) {
    for (uint32_t i = 0; i < 17; ++i)
      host->associate(host->context, &a);
    return PS_DEPENDENCY_NEED_V8;
  }
  if (mode == 9)
    a.runs = NULL;
  if (mode == 10)
    run.rank = 2;
  return host->associate(host->context, &a) ? PS_DEPENDENCY_NEED_V8
                                            : PS_OPERATION_RESULT_FAILURE_V8;
}
static int sample(const ps_dependency_services_v8* host, uint64_t owner,
                  uint64_t index, uint32_t dtype, double* result) {
  uint8_t bytes[8] = {0};
  uint64_t width = dtype == PS_OPERATION_ELEMENT_UINT8_V8     ? 1
                   : dtype == PS_OPERATION_ELEMENT_FLOAT32_V8 ? 4
                                                              : 8;
  int ok = owner
               ? host->read_owner(host->context, owner, &index, 1, bytes, width)
               : host->read(host->context, 0, &index, 1, bytes, width);
  if (!ok)
    return 0;
  if (dtype == PS_OPERATION_ELEMENT_UINT8_V8)
    *result = bytes[0];
  else if (dtype == PS_OPERATION_ELEMENT_INT64_V8) {
    int64_t n;
    memcpy(&n, bytes, 8);
    *result = (double)n;
  } else if (dtype == PS_OPERATION_ELEMENT_FLOAT32_V8) {
    float n;
    memcpy(&n, bytes, 4);
    *result = n;
  } else
    memcpy(result, bytes, 8);
  return 1;
}
static int poll(const ps_dependency_query_v8* q, void* state,
                const ps_dependency_services_v8* host, void* user) {
  (void)user;
  struct State* s = state;
  const uint64_t last = q->metadata.inputs[0].shape[0] - 1;
  if (s->stage == 0) {
    s->stage = 1;
    return need(q, host, 0);
  }
  if (s->stage == 1) {
    ps_dependency_fragment_v8 view = {0};
    view.struct_size = sizeof(view);
    if (host->fragment_count(host->context, 0) != 1 ||
        !host->fragment(host->context, 0, 0, &view) || view.offsets[0] != 0 ||
        view.extents[0] != 1 ||
        view.element_type != q->metadata.inputs[0].element_type)
      return PS_OPERATION_RESULT_FAILURE_V8;
    s->owner = host->retain_input(host->context, 0, 0);
    if (!s->owner)
      return PS_OPERATION_RESULT_FAILURE_V8;
    s->stage = 2;
    return need(q, host, last);
  }
  double left = 0, right = 0;
  if (!sample(host, s->owner, 0, q->metadata.inputs[0].element_type, &left) ||
      !sample(host, 0, last, q->metadata.inputs[0].element_type, &right))
    return PS_OPERATION_RESULT_FAILURE_V8;
  if (s->mode == 1) {
    double ignored;
    sample(host, 0, 1, q->metadata.inputs[0].element_type, &ignored);
  }
  if (s->mode == 2)
    host->release_owner(host->context, s->owner + 999);
  if (!host->release_owner(host->context, s->owner))
    return PS_OPERATION_RESULT_FAILURE_V8;
  if (s->mode == 3) {
    double ignored;
    sample(host, s->owner, 0, q->metadata.inputs[0].element_type, &ignored);
  }
  uint64_t count = 0;
  for (uint32_t i = 0; i < q->output_count; ++i)
    count += q->outputs[i].extents[0];
  const double result =
      left + right + (q->observation_kind ? (double)count : 0);
  for (uint32_t i = 0; i < q->output_count; ++i) {
    if (s->mode == 11) {
      ps_dependency_run_v8 single = q->outputs[i];
      single.extents[0] = 1;
      for (uint64_t j = 0; j < q->outputs[i].extents[0]; ++j) {
        single.offsets[0] = q->outputs[i].offsets[0] + j;
        uint64_t handle = 0;
        uint8_t* data = host->allocate_output(host->context, &single, &handle);
        if (!data)
          return PS_OPERATION_RESULT_FAILURE_V8;
        memcpy(data, &result, 8);
        if (!host->publish_output(host->context, handle))
          return PS_OPERATION_RESULT_FAILURE_V8;
      }
      continue;
    }
    uint64_t handle = 0;
    uint8_t* data =
        host->allocate_output(host->context, &q->outputs[i], &handle);
    if (!data)
      return PS_OPERATION_RESULT_FAILURE_V8;
    for (uint64_t j = 0; j < q->outputs[i].extents[0]; ++j)
      memcpy(data + j * 8, &result, 8);
    if (!host->publish_output(host->context, handle))
      return PS_OPERATION_RESULT_FAILURE_V8;
    if (s->mode == 5)
      host->publish_output(host->context, handle);
  }
  return PS_OPERATION_RESULT_SUCCESS_V8;
}
static void destroy(void* state, void* user) {
  (void)state;
  (void)user;
  ++destroys;
}
static const ps_dependency_program_v8 program = {
    PS_DEPENDENCY_BAD_CASE == 1 ? 0 : sizeof(program),
    8,
    1,
    PS_DEPENDENCY_BAD_CASE == 2 ? 1 : 0,
    PS_DEPENDENCY_BAD_CASE == 3 ? 0 : sizeof(struct State),
    validate,
    start,
    PS_DEPENDENCY_BAD_CASE == 4 ? NULL : poll,
    destroy};
static const ps_operation_parameter_descriptor_v8 parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v8), "mode", 4,
     PS_OPERATION_PARAMETER_INT64_V8, 1, 1, 0, 11}};
static const ps_operation_port_constraint_v8 ports[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_VALUE_V8, 0, 0,
     NULL}};
static const uint64_t wide_shape[] = {UINT64_C(1) << 61};
#define DESCRIPTOR(name, observation, wide)                           \
  {.struct_size = sizeof(ps_operation_descriptor_v8),                 \
   .key = name,                                                       \
   .key_size = sizeof(name) - 1,                                      \
   .input_count = 1,                                                  \
   .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC | \
            PS_OPERATION_FLAG_SIDE_EFFECT_FREE,                       \
   .output_element_type = PS_OPERATION_ELEMENT_FLOAT64_V8,            \
   .shape_rule = wide ? PS_OPERATION_SHAPE_FIXED_V8                   \
                      : PS_OPERATION_SHAPE_PRESERVE_FIRST_V8,         \
   .output_rank = wide ? 1 : 0,                                       \
   .output_shape = wide ? wide_shape : NULL,                          \
   .region_rule = PS_OPERATION_REGION_DEPENDENCY_V8,                  \
   .cacheable = 1,                                                    \
   .parameter_count = 1,                                              \
   .parameters = parameters,                                          \
   .input_schema_count = 1,                                           \
   .input_schema = ports,                                             \
   .output_schema = {sizeof(ps_operation_port_constraint_v8),         \
                     PS_OPERATION_PORT_VALUE_V8, 0, 0, NULL},         \
   .observation_kind = observation,                                   \
   .dependency_program = &program}
static const ps_operation_descriptor_v8 operations[] = {
    DESCRIPTOR("fixture.fragment", 0, 0), DESCRIPTOR("fixture.terminal", 1, 0),
    DESCRIPTOR("fixture.wide", 0, 1)};
static void destroy_api(const ps_operation_descriptor_v8* records,
                        uint32_t count) {
  (void)records;
  (void)count;
}
static const ps_operation_plugin_api_v8 api = {sizeof(api), 3, operations,
                                               destroy_api};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_8;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v8*
ps_operation_plugin_get_api_v8(void) {
  return &api;
}
