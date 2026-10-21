#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "photospider/plugin/operation_module_api.h"

#ifndef PS_BAD_CONTRACT_CASE
#define PS_BAD_CONTRACT_CASE 0
#endif
#define STR(s) {s, sizeof(s) - 1}
#define TRY(call)                         \
  do {                                    \
    ps_kernel_status_v8 status_ = (call); \
    if (status_.code)                     \
      return status_;                     \
  } while (0)

static ps_kernel_status_v8 status(uint32_t code) {
  ps_kernel_status_v8 result = {0};
  result.code = code;
  return result;
}
static const ps_parameter_spec_v8 parameters[] = {
    {.struct_size = sizeof(ps_parameter_spec_v8),
     .key = STR("dtype"),
     .type = PS_PARAMETER_STRING_V8,
     .required = 1}};
static ps_port_contract_v8 input = {.struct_size = sizeof(input),
                                    .rank = 1,
                                    .element_type_mask = 12};
static ps_tensor_spec_v8 tensor = {.struct_size = sizeof(tensor),
                                   .key = STR("samples"),
                                   .element_type = 4,
                                   .rank = 1,
                                   .shape = {1},
                                   .channel_axis = UINT32_MAX};
static ps_schema_v8 schema = {.struct_size = sizeof(schema),
                              .id = STR("fixture.contract"),
                              .version = 1,
                              .tensor_count = 1,
                              .tensors = &tensor};
static ps_operation_output_v8 output = {
    .struct_size = sizeof(output),
    .key = STR("value"),
    .contract = {.struct_size = sizeof(ps_port_contract_v8),
                 .schema_id = STR("fixture.contract"),
                 .schema_version = 1,
                 .rank = 1,
                 .element_type_mask = 12},
    .prototype = &schema,
    .input_count = UINT32_MAX};
static ps_kernel_status_v8 prepare(void* user, const ps_static_query_v8* query,
                                   const ps_preparation_sink_v8* sink,
                                   ps_operation_state_v8* state) {
  (void)user;
  (void)state;
  if (!query->input_count || query->input_count > 4 ||
      query->parameter_count != 1 ||
      query->parameters[0].type != PS_PARAMETER_STRING_V8)
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  for (uint32_t i = 0; i < query->input_count; ++i)
    if (query->inputs[i].tensor_count != 1 ||
        query->inputs[i].tensors[0].batch_rank)
      return status(PS_KERNEL_TYPE_MISMATCH_V8);
  const ps_string_v8 dtype = query->parameters[0].string_value;
  ps_schema_v8 selected = schema;
  ps_tensor_spec_v8 member = tensor;
  if (dtype.size == 7 && !memcmp(dtype.data, "float32", 7))
    member.element_type = 4;
  else if (dtype.size == 7 && !memcmp(dtype.data, "float64", 7))
    member.element_type = 3;
  else
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  member.shape[0] = query->input_count;
  selected.tensors = &member;
  return sink->set_output(sink->context, 0, &selected, NULL, UINT32_MAX);
}
static ps_kernel_status_v8 lower(void* user, const ps_lowering_query_v8* query,
                                 const ps_operation_builder_v8* builder) {
  (void)user;
  const ps_string_v8 symbol = STR("gather"), contract = STR("first-values-v1");
  const ps_placement_v8 placement = {PS_BACKEND_CPU_V8,
                                     PS_KERNEL_DOMAIN_HOST_V8,
                                     STR("cpu"),
                                     {NULL, 0}};
  const uint64_t scratch[3] = {0};
  for (uint32_t r = 0; r < query->request_count; ++r) {
    const ps_tile_request_v8* request = &query->requests[r];
    for (uint64_t tile = request->begin; tile < request->end; ++tile) {
      uint32_t task;
      TRY(builder->add_task(builder->context, symbol, contract, &placement,
                            scratch, 0, &task));
      const uint64_t begin = tile * query->tile_extent;
      const uint64_t available = query->facts.input_count - begin;
      const uint64_t count =
          available < query->tile_extent ? available : query->tile_extent;
      for (uint64_t i = 0; i < count; ++i)
        TRY(builder->read_input(builder->context, task, (uint32_t)(begin + i),
                                0, 0, PS_KERNEL_DOMAIN_HOST_V8));
      TRY(builder->write_output(builder->context, task, request->output,
                                request->member, tile));
    }
  }
  return status(PS_KERNEL_OK_V8);
}
struct KernelState {
  uint32_t count, output_type, input_type[4];
};
static void release(const void* state) {
  free((void*)state);
}
static void invoke(const ps_kernel_invocation_v8* query,
                   ps_kernel_status_v8* result) {
  const struct KernelState* state = query->prepared_context;
  *result = status(PS_KERNEL_OK_V8);
  unsigned char* output = (unsigned char*)query->outputs[0].allocation +
                          query->outputs[0].byte_offset;
  for (uint32_t i = 0; i < state->count; ++i) {
    if (query->cancellation.is_cancelled &&
        query->cancellation.is_cancelled(query->cancellation.context)) {
      *result = status(PS_KERNEL_CANCELLED_V8);
      return;
    }
    const unsigned char* input =
        (const unsigned char*)query->inputs[i].allocation +
        query->inputs[i].byte_offset;
    double value;
    if (state->input_type[i] == 4) {
      float sample;
      memcpy(&sample, input, 4);
      value = sample;
    } else {
      memcpy(&value, input, 8);
    }
    if (state->output_type == 4) {
      const float converted = (float)value;
      memcpy(output + i * 4, &converted, 4);
    } else {
      memcpy(output + i * 8, &value, 8);
    }
  }
}
static ps_kernel_status_v8 prepare_kernel(void* user,
                                          const ps_kernel_preparation_v8* query,
                                          ps_prepared_kernel_v8* kernel) {
  (void)user;
  if (query->placement.backend != PS_BACKEND_CPU_V8 ||
      query->placement.domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      query->symbol.size != 6 || memcmp(query->symbol.data, "gather", 6) ||
      query->contract.size != 15 ||
      memcmp(query->contract.data, "first-values-v1", 15) ||
      query->write_count != 1 || !query->read_count || query->read_count > 4 ||
      query->parameter_bytes || query->scratch[0] || query->scratch[1] ||
      query->scratch[2])
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  const ps_kernel_view_spec_v8* out = &query->writes[0];
  const uint32_t type = out->layout.element_type;
  const uint32_t width = type == 4 ? 4 : 8;
  if ((type != 3 && type != 4) || out->port || out->member ||
      out->domain != PS_KERNEL_DOMAIN_HOST_V8 || out->byte_offset ||
      out->layout.rank != 1 || out->layout.shape[0] != query->read_count ||
      out->layout.strides[0] != width ||
      out->bytes != query->read_count * width ||
      out->layout.origin[0] > query->facts.input_count ||
      query->read_count > query->facts.input_count - out->layout.origin[0])
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  struct KernelState state = {.count = query->read_count, .output_type = type};
  for (uint32_t i = 0; i < query->read_count; ++i) {
    const ps_kernel_view_spec_v8* in = &query->reads[i];
    const uint32_t in_type = in->layout.element_type;
    const uint32_t in_width = in_type == 4 ? 4 : 8;
    if ((in_type != 3 && in_type != 4) ||
        in->port != out->layout.origin[0] + i || in->member || in->tile_index ||
        in->domain != PS_KERNEL_DOMAIN_HOST_V8 || in->byte_offset ||
        in->layout.rank != 1 || in->layout.origin[0] || !in->layout.shape[0] ||
        in->layout.strides[0] != in_width ||
        in->layout.shape[0] > UINT64_MAX / in_width ||
        in->bytes != in->layout.shape[0] * in_width)
      return status(PS_KERNEL_INVALID_ARGUMENT_V8);
    state.input_type[i] = in_type;
  }
  struct KernelState* owned = malloc(sizeof(*owned));
  if (!owned)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  *owned = state;
  *kernel = (ps_prepared_kernel_v8){.struct_size = sizeof(*kernel),
                                    .abi_version = 8,
                                    .context = owned,
                                    .release = release,
                                    .invoke_cpu = invoke};
  return status(PS_KERNEL_OK_V8);
}
static ps_operation_v8 operation = {.struct_size = sizeof(operation),
                                    .key = STR("fixture.contract"),
                                    .flags = PS_OPERATION_CPU_V8 |
                                             PS_OPERATION_DETERMINISTIC_V8 |
                                             PS_OPERATION_PURE_V8,
                                    .outputs = &output,
                                    .output_count = 1,
                                    .parameters = parameters,
                                    .parameter_count = 1,
                                    .prepare = prepare,
                                    .lower = lower,
                                    .prepare_kernel = prepare_kernel,
                                    .repeated_input = &input,
                                    .repeated_minimum = 1,
                                    .repeated_maximum = 4,
                                    .repeated_match = 1};
static void destroy(void* context) {
  (void)context;
}
static const ps_operation_module_v8 api = {.struct_size = sizeof(api),
                                           .abi_version = 8,
                                           .operations = &operation,
                                           .operation_count = 1,
                                           .destroy = destroy};
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
#if PS_BAD_CONTRACT_CASE == 1
  operation.struct_size = 0;
#elif PS_BAD_CONTRACT_CASE == 3
  schema.tensors = NULL;
#elif PS_BAD_CONTRACT_CASE == 5
  operation.repeated_minimum = 0;
#elif PS_BAD_CONTRACT_CASE == 6
  operation.parameter_count = 129;
#elif PS_BAD_CONTRACT_CASE == 7
  tensor.rank = 9;
#elif PS_BAD_CONTRACT_CASE == 8
  input.element_type_mask = UINT32_C(1) << 7;
#elif PS_BAD_CONTRACT_CASE == 9
  input.element_type = 3;
#elif PS_BAD_CONTRACT_CASE == 10
  output.prototype = NULL;
#elif PS_BAD_CONTRACT_CASE == 11
  output.contract.tensor_key.size = 1;
#elif PS_BAD_CONTRACT_CASE == 12
  output.input_count = 1;
#elif PS_BAD_CONTRACT_CASE == 13
  operation.repeated_match = 0;
  operation.prepare = NULL;
#endif
  return &api;
}
