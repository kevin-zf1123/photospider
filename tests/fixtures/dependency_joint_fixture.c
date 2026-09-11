#include <string.h>

#include "photospider/plugin/dependency_plugin_api.h"

static unsigned starts, destroys;
PS_OPERATION_EXPORT unsigned ps_joint_starts(void) {
  return starts;
}
PS_OPERATION_EXPORT unsigned ps_joint_destroys(void) {
  return destroys;
}
static int start(const ps_dependency_query_v9* q, void* state, uint64_t bytes,
                 void* user) {
  (void)q;
  (void)state;
  (void)bytes;
  (void)user;
  return PS_OPERATION_RESULT_SUCCESS_V9;
}
static int publish(const ps_dependency_query_v9* q,
                   const ps_dependency_services_v9* host, uint64_t* handle) {
  uint8_t* data = host->allocate_output(host->context, &q->outputs[0], handle);
  if (!data)
    return PS_OPERATION_RESULT_FAILURE_V9;
  double number = 10 + q->output_index;
  memcpy(data, &number, sizeof(number));
  return PS_OPERATION_RESULT_SUCCESS_V9;
}
static int poll(const ps_dependency_query_v9* q, void* state,
                const ps_dependency_services_v9* host, void* user) {
  (void)state;
  (void)user;
  uint64_t handle;
  int result = publish(q, host, &handle);
  if (!result && !host->publish_output(host->context, handle))
    result = 1;
  return result;
}
static void destroy(void* state, void* user) {
  (void)state;
  (void)user;
}
static int joint_start(const ps_dependency_query_v9* const* queries,
                       uint32_t count, void* state, uint64_t size, void* user) {
  (void)count;
  (void)user;
  ++starts;
  if (size != sizeof(int) || *(int*)state)
    return PS_OPERATION_RESULT_FAILURE_V9;
  *(int*)state = (int)queries[0]->metadata.parameters[0].int64_value;
  return *(int*)state == 8 ? PS_OPERATION_RESULT_FAILURE_V9
                           : PS_OPERATION_RESULT_SUCCESS_V9;
}
static void joint_destroy(void* state, void* user) {
  (void)state;
  (void)user;
  ++destroys;
}
static int joint_poll(const ps_dependency_joint_member_v9* members,
                      uint32_t count, void* state,
                      const ps_dependency_joint_services_v9* shared,
                      ps_dependency_atom_outcome_v9* outcomes,
                      uint32_t* out_count, void* user) {
  (void)user;
  const int mode = *(int*)state;
  if (mode == 9)
    shared->consume_work(shared->context, UINT64_MAX);
  uint8_t* scratch = NULL;
  if (!shared->scratch(shared->context, 8, &scratch))
    return 1;
  memset(scratch, 0, 8);
  uint64_t handles[64] = {0};
  for (uint32_t i = 0; i < count; ++i) {
    outcomes[i].output_index = members[i].query->output_index;
    outcomes[i].result =
        publish(members[i].query, members[i].services, &handles[i]);
  }
  for (uint32_t i = 0; i < count; ++i) {
    const ps_dependency_services_v9* host = members[i].services;
    if (mode == 5 && i == 0) {
      outcomes[i].result = PS_OPERATION_RESULT_FAILURE_V9;
    } else if (mode == 6 && i == 0) {
      host->publish_output(host->context, handles[1]);
    } else if (mode != 7 || i != 0) {
      host->publish_output(host->context, handles[i]);
    }
  }
  *out_count = mode == 1 ? count - 1 : count;
  if (mode == 2)
    outcomes[1].output_index = outcomes[0].output_index;
  if (mode == 3)
    outcomes[1].output_index = 64;
  return mode == 4 ? PS_OPERATION_RESULT_FAILURE_V9
                   : PS_OPERATION_RESULT_SUCCESS_V9;
}
static const ps_dependency_joint_program_v9 joint = {
    sizeof(ps_dependency_joint_program_v9),
    0,
    sizeof(int),
    8,
    joint_start,
    joint_poll,
    joint_destroy};
static const ps_dependency_program_v9 program = {
    sizeof(ps_dependency_program_v9),
    2,
    0,
    0,
    1,
    NULL,
    start,
    poll,
    destroy,
    &joint};
static ps_operation_descriptor_v9 descriptor;
static ps_operation_parameter_descriptor_v9 parameter;
static uint64_t shapes[2] = {1, 2};
static void release(const ps_operation_descriptor_v9* records, uint32_t count) {
  (void)records;
  (void)count;
}
static const ps_operation_plugin_api_v9 api = {
    sizeof(ps_operation_plugin_api_v9), 1, &descriptor, release};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_9;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v9*
ps_operation_plugin_get_api_v9(void) {
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.struct_size = sizeof(descriptor);
  descriptor.key = "test.c_joint";
  descriptor.key_size = 12;
  descriptor.flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
                     PS_OPERATION_FLAG_SIDE_EFFECT_FREE;
  descriptor.output_count = 2;
  descriptor.dependency_program = &program;
  memset(&parameter, 0, sizeof(parameter));
  parameter.struct_size = sizeof(parameter);
  parameter.key = "mode";
  parameter.key_size = 4;
  parameter.type = PS_OPERATION_PARAMETER_INT64_V9;
  parameter.required = 1;
  descriptor.parameters = &parameter;
  descriptor.parameter_count = 1;
  for (uint32_t i = 0; i < 2; ++i) {
    ps_operation_output_descriptor_v9* output = &descriptor.outputs[i];
    output->struct_size = sizeof(*output);
    output->key = i ? "second" : "first";
    output->key_size = i ? 6 : 5;
    output->output_element_type = PS_OPERATION_ELEMENT_FLOAT64_V9;
    output->shape_rule = PS_OPERATION_SHAPE_FIXED_V9;
    output->output_shape = &shapes[i];
    output->output_rank = 1;
    output->region_rule = PS_OPERATION_REGION_DEPENDENCY_V9;
    output->failure_delivery = PS_OPERATION_FAILURE_PER_ATOM_V9;
    output->output_schema.struct_size = sizeof(output->output_schema);
    output->output_schema.kind = PS_OPERATION_PORT_VALUE_V9;
  }
  return &api;
}
