#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "photospider/plugin/operation_module_api.h"

#define TEXT(s) {s, sizeof(s) - 1}
#define TRY(call)                         \
  do {                                    \
    ps_kernel_status_v8 result_ = (call); \
    if (result_.code)                     \
      return result_;                     \
  } while (0)

static char member_key[] = "target", operation_key[] = "contract.member";
static char output_key[] = "value", schema_id[] = "contract.value";
static char tensor_key[] = "number", scale_key[] = "scale",
            offset_key[] = "offset";
PS_OPERATION_MODULE_EXPORT void ps_contract_mutate(int changed) {
  member_key[0] = changed ? 'T' : 't';
  operation_key[9] = changed ? 'M' : 'm';
  output_key[0] = changed ? 'V' : 'v';
  schema_id[0] = changed ? 'C' : 'c';
  tensor_key[0] = changed ? 'N' : 'n';
  scale_key[0] = changed ? 'S' : 's';
  offset_key[0] = changed ? 'O' : 'o';
}
static ps_kernel_status_v8 status(uint32_t code) {
  const ps_kernel_status_v8 result = {.code = code};
  return result;
}
static const unsigned modes[] = {0, 1, 2, 3};
static const ps_tensor_spec_v8 tensors[] = {
    {.struct_size = sizeof(ps_tensor_spec_v8),
     .key = TEXT(tensor_key),
     .element_type = 3,
     .rank = 1,
     .shape = {1},
     .channel_axis = UINT32_MAX},
    {.struct_size = sizeof(ps_tensor_spec_v8),
     .key = TEXT(tensor_key),
     .element_type = 3,
     .rank = 2,
     .shape = {1, UINT64_C(1) << 61},
     .channel_axis = UINT32_MAX}};
static const ps_schema_v8 schemas[] = {{.struct_size = sizeof(ps_schema_v8),
                                        .id = TEXT(schema_id),
                                        .version = 1,
                                        .tensor_count = 1,
                                        .tensors = &tensors[0]},
                                       {.struct_size = sizeof(ps_schema_v8),
                                        .id = TEXT(schema_id),
                                        .version = 1,
                                        .tensor_count = 1,
                                        .tensors = &tensors[1]}};
static const ps_operation_output_v8 outputs[] = {
    {.struct_size = sizeof(ps_operation_output_v8),
     .key = TEXT(output_key),
     .contract = {.struct_size = sizeof(ps_port_contract_v8),
                  .schema_id = TEXT(schema_id),
                  .schema_version = 1},
     .prototype = &schemas[0],
     .input_count = UINT32_MAX},
    {.struct_size = sizeof(ps_operation_output_v8),
     .key = TEXT(output_key),
     .contract = {.struct_size = sizeof(ps_port_contract_v8),
                  .schema_id = TEXT(schema_id),
                  .schema_version = 1},
     .prototype = &schemas[1],
     .input_count = UINT32_MAX}};
static const ps_port_contract_v8 ports[] = {
    {.struct_size = sizeof(ps_port_contract_v8),
     .tensor_key = TEXT(member_key),
     .element_type = 4,
     .rank = 1},
    {.struct_size = sizeof(ps_port_contract_v8), .element_type = 4, .rank = 1}};
/* Deliberately reverse lexical order; callback values are canonical by key. */
static const ps_parameter_spec_v8 parameters[] = {
    {.struct_size = sizeof(ps_parameter_spec_v8),
     .key = TEXT(scale_key),
     .type = PS_PARAMETER_FLOAT64_V8,
     .bounded = 1,
     .minimum = 0,
     .maximum = 4},
    {.struct_size = sizeof(ps_parameter_spec_v8),
     .key = TEXT(offset_key),
     .type = PS_PARAMETER_INT64_V8,
     .bounded = 1,
     .minimum = -2,
     .maximum = 2}};
static void release(const void* state) {
  free((void*)state);
}
static ps_kernel_status_v8 prepare(void* user, const ps_static_query_v8* query,
                                   const ps_preparation_sink_v8* sink,
                                   ps_operation_state_v8* state) {
  const unsigned mode = *(const unsigned*)user;
  (void)sink;
  double value = 11;
  if (mode == 2) {
    double offset = 0, scale = 1;
    for (uint32_t i = 0; i < query->parameter_count; ++i) {
      const ps_parameter_value_v8* parameter = &query->parameters[i];
      if (i && parameter->key.data[0] < query->parameters[i - 1].key.data[0])
        return status(PS_KERNEL_INVALID_ARGUMENT_V8);
      if (parameter->key.size == 6 &&
          !memcmp(parameter->key.data, "offset", 6) &&
          parameter->type == PS_PARAMETER_INT64_V8)
        offset = (double)parameter->int64_value;
      else if (parameter->key.size == 5 &&
               !memcmp(parameter->key.data, "scale", 5) &&
               parameter->type == PS_PARAMETER_FLOAT64_V8)
        scale = parameter->float64_value;
      else
        return status(PS_KERNEL_INVALID_ARGUMENT_V8);
    }
    value = (30 + offset) * scale;
  }
  double* owned = malloc(sizeof(*owned));
  if (!owned)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  *owned = value;
  state->context = owned;
  state->release = release;
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 lower(void* user, const ps_lowering_query_v8* query,
                                 const ps_operation_builder_v8* builder) {
  (void)user;
  const ps_string_v8 symbol = TEXT("fill"), contract = TEXT("f64-v1");
  const ps_placement_v8 placement = {.backend = PS_BACKEND_CPU_V8,
                                     .domain = PS_KERNEL_DOMAIN_HOST_V8,
                                     .profile = TEXT("cpu")};
  const uint64_t scratch[3] = {0};
  for (uint32_t r = 0; r < query->request_count; ++r) {
    const ps_tile_request_v8* request = &query->requests[r];
    for (uint64_t tile = request->begin; tile < request->end; ++tile) {
      uint32_t task;
      TRY(builder->add_task(builder->context, symbol, contract, &placement,
                            scratch, 0, &task));
      for (uint32_t port = 0; port < query->facts.input_count; ++port)
        TRY(builder->read_input_schema(builder->context, task, port));
      TRY(builder->write_output(builder->context, task, request->output,
                                request->member, tile));
    }
  }
  return status(PS_KERNEL_OK_V8);
}
struct Kernel {
  double value;
  uint64_t samples;
};
static void invoke(const ps_kernel_invocation_v8* call,
                   ps_kernel_status_v8* result) {
  const struct Kernel* kernel = call->prepared_context;
  uint8_t* output =
      (uint8_t*)call->outputs[0].allocation + call->outputs[0].byte_offset;
  for (uint64_t i = 0; i < kernel->samples; ++i)
    memcpy(output + i * 8, &kernel->value, 8);
  *result = status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 prepare_kernel(void* user,
                                          const ps_kernel_preparation_v8* query,
                                          ps_prepared_kernel_v8* result) {
  (void)user;
  if (query->placement.backend != PS_BACKEND_CPU_V8 ||
      query->placement.domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      query->symbol.size != 4 || memcmp(query->symbol.data, "fill", 4) ||
      query->contract.size != 6 || memcmp(query->contract.data, "f64-v1", 6) ||
      query->read_count || query->write_count != 1 || query->parameter_bytes ||
      query->scratch[0] || query->scratch[1] || query->scratch[2])
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  const ps_kernel_view_spec_v8* out = query->writes;
  uint64_t bytes = 8;
  if (out->port || out->member || out->byte_offset ||
      out->domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      out->layout.element_type != 3 || !out->layout.rank ||
      out->layout.rank > 2)
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  for (uint32_t axis = out->layout.rank; axis-- > 0;) {
    if (!out->layout.shape[axis] ||
        out->layout.strides[axis] != (int64_t)bytes ||
        out->layout.shape[axis] > UINT64_MAX / bytes)
      return status(PS_KERNEL_INVALID_ARGUMENT_V8);
    bytes *= out->layout.shape[axis];
  }
  if (bytes != out->bytes)
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  struct Kernel* kernel = malloc(sizeof(*kernel));
  if (!kernel)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  kernel->value = *(const double*)query->static_state;
  kernel->samples = bytes / 8;
  *result = (ps_prepared_kernel_v8){.struct_size = sizeof(*result),
                                    .abi_version = 8,
                                    .context = kernel,
                                    .release = release,
                                    .invoke_cpu = invoke};
  return status(PS_KERNEL_OK_V8);
}
// ClangFormat aligns C designated initializers inside this macro.
// NOLINTBEGIN(whitespace/indent)
#define OP(KEY, MODE, INPUTS, COUNT, OUTPUT, PARAMETERS, PARAM_COUNT) \
  {.struct_size = sizeof(ps_operation_v8),                            \
   .key = TEXT(KEY),                                                  \
   .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_PURE_V8 |              \
            PS_OPERATION_DETERMINISTIC_V8,                            \
   .inputs = INPUTS,                                                  \
   .input_count = COUNT,                                              \
   .outputs = OUTPUT,                                                 \
   .output_count = 1,                                                 \
   .parameters = PARAMETERS,                                          \
   .parameter_count = PARAM_COUNT,                                    \
   .user_data = (void*)&modes[MODE],                                  \
   .prepare = prepare,                                                \
   .lower = lower,                                                    \
   .prepare_kernel = prepare_kernel}
// NOLINTEND
static const ps_operation_v8 operations[] = {
    OP(operation_key, 0, &ports[0], 1, &outputs[0], NULL, 0),
    OP("contract.sole", 1, &ports[1], 1, &outputs[0], NULL, 0),
    OP("contract.parameters", 2, NULL, 0, &outputs[0], parameters, 2),
    OP("contract.sparse", 3, NULL, 0, &outputs[1], NULL, 0)};
static void destroy(void* context) {
  (void)context;
}
static const ps_operation_module_v8 module = {.struct_size = sizeof(module),
                                              .abi_version = 8,
                                              .operations = operations,
                                              .operation_count = 4,
                                              .destroy = destroy};
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  return &module;
}
