#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "photospider/plugin/operation_module_api.h"

#ifndef PS_MODULE_BAD
#define PS_MODULE_BAD 0
#endif
#define STR(x) {x, sizeof(x) - 1}
#define TRY(x)                    \
  do {                            \
    ps_kernel_status_v8 s_ = (x); \
    if (s_.code)                  \
      return s_;                  \
  } while (0)

static void (*notify)(void*, uint32_t);
static void* notify_context;
static ps_resource_id_v8 resource_id;
PS_OPERATION_MODULE_EXPORT void ps_fixture_probe(
    void (*callback)(void*, uint32_t), void* context,
    const ps_resource_id_v8* resource) {
  notify = callback;
  notify_context = context;
  if (resource)
    resource_id = *resource;
}
static void event(uint32_t kind) {
  if (notify)
    notify(notify_context, kind);
}
#if !defined(_WIN32)
__attribute__((destructor)) static void unloaded(void) {
  event(8);
}
#endif
static ps_kernel_status_v8 status(uint32_t code) {
  ps_kernel_status_v8 value = {0};
  value.code = code;
  return value;
}
struct StaticState {
  int64_t mode;
  uint64_t length;
};
struct KernelState {
  const struct StaticState* state;
  const uint8_t* resource;
};
static void release_static(const void* context) {
  event(2);
  free((void*)context);
}
static void release_kernel(const void* context) {
  event(4);
  free((void*)context);
}
static void destroy(void* context) {
  (void)context;
  event(7);
}
static ps_kernel_status_v8 prepare(void* context,
                                   const ps_static_query_v8* query,
                                   const ps_preparation_sink_v8* sink,
                                   ps_operation_state_v8* result) {
  struct StaticState* state;
  ps_schema_v8 output;
  (void)context;
  if (query->input_count != 1 || query->parameter_count != 1 ||
      query->parameters[0].type != PS_PARAMETER_INT64_V8 ||
      query->inputs[0].tensor_count != 1 ||
      query->inputs[0].tensors[0].rank != 1)
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  state = (struct StaticState*)calloc(1, sizeof(*state));
  if (!state)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  result->context = state;
  result->release = release_static;
  state->mode = query->parameters[0].int64_value;
  state->length = query->inputs[0].tensors[0].shape[0];
  event(1);
  if (state->mode == 10)
    return status(PS_KERNEL_OPERATION_FAILED_V8);
  output = query->inputs[0];
  TRY(sink->set_output(sink->context, 0, &output, NULL, UINT32_MAX));
  if (state->mode == 14)
    TRY(sink->require_resource(sink->context, &resource_id));
  if (state->mode == 15) {
    /* An ignored second assignment must still reject static preparation. */
    (void)sink->set_output(sink->context, 0, &output, NULL, UINT32_MAX);
    return status(PS_KERNEL_CANCELLED_V8);
  }
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 validate_resources(
    void* context, const void* raw, const ps_resource_view_v8* resources,
    uint32_t count) {
  const struct StaticState* state = (const struct StaticState*)raw;
  (void)context;
  event(9);
  if (state->mode != 14)
    return status(count ? PS_KERNEL_TYPE_MISMATCH_V8 : PS_KERNEL_OK_V8);
  if (count != 1 ||
      resources[0].identity.byte_length != resource_id.byte_length ||
      memcmp(resources[0].identity.sha256, resource_id.sha256, 32) ||
      ((const uint8_t*)resources[0].data)[0] != 42)
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 lower(void* context,
                                 const ps_lowering_query_v8* query,
                                 const ps_operation_builder_v8* builder) {
  const struct StaticState* state =
      (const struct StaticState*)query->static_state;
  const ps_placement_v8 placement = {PS_BACKEND_CPU_V8,
                                     PS_KERNEL_DOMAIN_HOST_V8,
                                     STR("cpu"),
                                     {NULL, 0}};
  const ps_string_v8 symbol = STR("increment"),
                     contract = STR("increment-u8-v1");
  const uint64_t scratch[3] = {0, 0, 0};
  uint32_t r;
  (void)context;
  event(5);
  if (query->input_grid_count != 1 || query->output_grid_count != 1 ||
      query->input_grids[0].shape[0] != state->length ||
      query->output_grids[0].tile_count != query->input_grids[0].tile_count)
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  if (state->mode == 11) {
    (void)builder->read_input_schema(builder->context, UINT32_MAX, 0);
    return status(PS_KERNEL_CANCELLED_V8);
  }
  for (r = 0; r < query->request_count; ++r) {
    const ps_tile_request_v8* request = &query->requests[r];
    uint64_t tile;
    for (tile = request->begin; tile < request->end; ++tile) {
      uint32_t task;
      uint8_t delta = 3;
      TRY(builder->add_task(builder->context, symbol, contract, &placement,
                            scratch, 0, &task));
      if (state->mode == 1) {
        uint32_t copy, temporary, private_tile;
        TRY(builder->add_temporary(builder->context, &query->outputs[0],
                                   &temporary));
        TRY(builder->add_copy(builder->context, &placement, &copy));
        TRY(builder->read_input(builder->context, copy, 0, 0, tile,
                                PS_KERNEL_DOMAIN_HOST_V8));
        TRY(builder->write_temporary(builder->context, copy, temporary, 0, tile,
                                     &private_tile));
        TRY(builder->read_tile(builder->context, task, private_tile, 0));
        TRY(builder->depends_on(builder->context, task, copy));
      } else if (state->mode == 2) {
        TRY(builder->read_input_schema(builder->context, task, 0));
      } else {
        TRY(builder->read_input(builder->context, task, 0, 0, tile,
                                PS_KERNEL_DOMAIN_HOST_V8));
      }
      TRY(builder->set_parameters(builder->context, task, &delta,
                                  sizeof(delta)));
      TRY(builder->write_output(builder->context, task, request->output,
                                request->member, tile));
    }
  }
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 lower_copy(void* context,
                                      const ps_lowering_query_v8* query,
                                      const ps_operation_builder_v8* builder) {
  const ps_placement_v8 placement = {PS_BACKEND_CPU_V8,
                                     PS_KERNEL_DOMAIN_HOST_V8,
                                     STR("cpu"),
                                     {NULL, 0}};
  uint32_t r;
  (void)context;
  for (r = 0; r < query->request_count; ++r) {
    uint64_t tile;
    const ps_tile_request_v8* request = &query->requests[r];
    for (tile = request->begin; tile < request->end; ++tile) {
      uint32_t copy;
      TRY(builder->add_copy(builder->context, &placement, &copy));
      TRY(builder->read_input(builder->context, copy, 0, request->member, tile,
                              PS_KERNEL_DOMAIN_HOST_V8));
      TRY(builder->write_output(builder->context, copy, request->output,
                                request->member, tile));
    }
  }
  return status(PS_KERNEL_OK_V8);
}
static void invoke(const ps_kernel_invocation_v8* query,
                   ps_kernel_status_v8* result) {
  const struct KernelState* state =
      (const struct KernelState*)query->prepared_context;
  const ps_kernel_mutable_view_v8* output = &query->outputs[0];
  uint8_t* bytes = (uint8_t*)output->allocation + output->byte_offset;
  uint64_t x;
  event(6);
  if (query->input_count &&
      (!query->input_object_ids || !query->input_object_ids[0])) {
    *result = status(PS_KERNEL_INVALID_ARGUMENT_V8);
    return;
  }
  if (query->cancellation.is_cancelled &&
      query->cancellation.is_cancelled(query->cancellation.context)) {
    *result = status(PS_KERNEL_CANCELLED_V8);
    return;
  }
  for (x = 0; x < output->layout->shape[0]; ++x) {
    uint8_t value = state->resource ? *state->resource : 0;
    if (state->state->mode != 2) {
      const ps_kernel_const_view_v8* input = &query->inputs[0];
      value +=
          ((const uint8_t*)input->allocation)[input->byte_offset +
                                              x * input->layout->strides[0]];
    }
    bytes[x * output->layout->strides[0]] =
        value + *(const uint8_t*)query->parameters;
  }
  *result = status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 prepare_kernel(void* context,
                                          const ps_kernel_preparation_v8* query,
                                          ps_prepared_kernel_v8* result) {
  const struct StaticState* state =
      (const struct StaticState*)query->static_state;
  struct KernelState* kernel;
  (void)context;
  if (query->struct_size != sizeof(*query) || query->symbol.size != 9 ||
      memcmp(query->symbol.data, "increment", 9) ||
      query->contract.size != 15 ||
      memcmp(query->contract.data, "increment-u8-v1", 15) ||
      query->write_count != 1 ||
      query->read_count != (state->mode == 2 ? 0U : 1U) ||
      query->parameter_bytes != 1 || query->scratch[0] || query->scratch[1] ||
      query->scratch[2] || query->writes[0].layout.element_type != 1 ||
      query->writes[0].layout.rank != 1 ||
      query->writes[0].bytes != query->writes[0].layout.shape[0] ||
      query->placement.backend != PS_BACKEND_CPU_V8 || query->native_gpu)
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  kernel = (struct KernelState*)calloc(1, sizeof(*kernel));
  if (!kernel)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  kernel->state = state;
  if (state->mode == 14) {
    if (query->resource_count != 1) {
      free(kernel);
      return status(PS_KERNEL_NOT_FOUND_V8);
    }
    kernel->resource = (const uint8_t*)query->resources[0].data;
  }
  result->struct_size = sizeof(*result);
  result->abi_version = state->mode == 13 ? 7 : 8;
  result->context = kernel;
  result->release = release_kernel;
  result->invoke_cpu = invoke;
  event(3);
  return status(state->mode == 12 ? PS_KERNEL_NOT_FOUND_V8 : PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 prepare_null(void* context,
                                        const ps_static_query_v8* query,
                                        const ps_preparation_sink_v8* sink,
                                        ps_operation_state_v8* result) {
  (void)context;
  (void)query;
  (void)sink;
  result->release = release_static;
  event(1);
  return status(PS_KERNEL_OK_V8);
}
static const ps_tensor_spec_v8 tensor = {
    .struct_size = sizeof(ps_tensor_spec_v8),
    .key = STR("samples"),
    .element_type = 1,
    .rank = 1,
    .shape = {4},
    .channel_axis = UINT32_MAX};
static const ps_schema_v8 schema = {.struct_size = sizeof(ps_schema_v8),
                                    .id = STR("fixed.module.test"),
                                    .version = 1,
                                    .tensor_count = 1,
                                    .tensors = &tensor};
static const ps_port_contract_v8 port = {
    .struct_size = sizeof(ps_port_contract_v8),
    .schema_id = STR("fixed.module.test"),
    .tensor_key = STR("samples"),
    .schema_version = 1,
    .element_type = 1,
    .rank = 1};
static const ps_operation_output_v8 output = {
    .struct_size = sizeof(ps_operation_output_v8),
    .key = STR("out"),
    .contract = {.struct_size = sizeof(ps_port_contract_v8),
                 .schema_id = STR("fixed.module.test"),
                 .schema_version = 1},
    .prototype = &schema,
    .input_count = UINT32_MAX};
static const ps_parameter_spec_v8 parameter = {
    .struct_size = sizeof(ps_parameter_spec_v8),
    .key = STR("mode"),
    .type = PS_PARAMETER_INT64_V8,
    .required = 1};
static const ps_operation_v8 operations[] = {
    {.struct_size = sizeof(ps_operation_v8),
     .key = STR("module.increment"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_PURE_V8 |
              PS_OPERATION_DETERMINISTIC_V8,
     .input_count = 1,
     .inputs = &port,
     .outputs = &output,
     .output_count = 1,
     .parameters = &parameter,
     .parameter_count = PS_MODULE_BAD == 5 ? 129 : 1,
     .prepare = prepare,
     .validate_resources = validate_resources,
     .lower = PS_MODULE_BAD == 4 ? NULL : lower,
     .prepare_kernel = prepare_kernel},
    {.struct_size = sizeof(ps_operation_v8),
#if PS_MODULE_BAD == 3
     .key = STR("module.increment"),
#else
     .key = STR("module.copy"),
#endif
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_PURE_V8 |
              PS_OPERATION_DETERMINISTIC_V8,
     .input_count = 1,
     .inputs = &port,
     .outputs = &output,
     .output_count = 1,
     .parameters = &parameter,
     .parameter_count = 1,
     .prepare = prepare,
     .lower = lower_copy},
    {.struct_size = sizeof(ps_operation_v8),
     .key = STR("module.null"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_PURE_V8 |
              PS_OPERATION_DETERMINISTIC_V8,
     .input_count = 1,
     .inputs = &port,
     .outputs = &output,
     .output_count = 1,
     .prepare = prepare_null,
     .lower = lower_copy}};
static const ps_operation_module_v8 module = {
    .struct_size = PS_MODULE_BAD == 2 ? 0 : sizeof(ps_operation_module_v8),
    .abi_version = PS_MODULE_BAD == 1 ? 7 : 8,
    .operations = operations,
    .operation_count = 3,
    .destroy = destroy};
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  return &module;
}
