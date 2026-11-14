#include <string.h>

#include "photospider/plugin/operation_module_api.h"

#define TEXT(s) {s, sizeof(s) - 1}
#define TRY(call)                         \
  do {                                    \
    ps_kernel_status_v8 status_ = (call); \
    if (status_.code)                     \
      return status_;                     \
  } while (0)

static ps_kernel_status_v8 status(uint32_t code) {
  const ps_kernel_status_v8 result = {.code = code};
  return result;
}
static const ps_tensor_spec_v8 tensor = {.struct_size = sizeof(tensor),
                                         .key = TEXT("samples"),
                                         .element_type = PS_ELEMENT_FLOAT64_V8,
                                         .rank = 1,
                                         .shape = {1},
                                         .channel_axis = UINT32_MAX};
static const ps_schema_v8 schema = {.struct_size = sizeof(schema),
                                    .id = TEXT("fixture.lowering"),
                                    .version = 1,
                                    .tensors = &tensor,
                                    .tensor_count = 1};
static const ps_operation_output_v8 output = {
    .struct_size = sizeof(output),
    .key = TEXT("value"),
    .contract = {.struct_size = sizeof(ps_port_contract_v8),
                 .schema_id = TEXT("fixture.lowering"),
                 .schema_version = 1},
    .prototype = &schema};
static const ps_placement_v8 cpu = {PS_BACKEND_CPU_V8,
                                    PS_KERNEL_DOMAIN_HOST_V8,
                                    TEXT("cpu"),
                                    {NULL, 0}};
static const unsigned ignore_invalid_abi = 1, ignore_gpu_v9 = 2;
static ps_kernel_status_v8 lower(void* context,
                                 const ps_lowering_query_v9* query,
                                 const ps_operation_builder_v10* builder) {
  const ps_string_v8 symbol = TEXT("fill"), contract = TEXT("scalar-v1");
  const uint64_t scratch[3] = {0};
  uint32_t temporary;
  TRY(builder->add_temporary(builder->context, &schema, &temporary));
  for (unsigned r = 0; r < query->request_count; ++r) {
    const ps_tile_request_v8* request = &query->requests[r];
    for (uint64_t tile = request->begin; tile < request->end; ++tile) {
      uint32_t kernel, copy, private_tile;
      TRY(builder->add_task(builder->context, symbol, contract, &cpu, scratch,
                            0, 9, &kernel));
      TRY(builder->write_temporary(builder->context, kernel, temporary,
                                   request->member, tile, &private_tile));
      TRY(builder->add_copy(builder->context, &cpu, &copy));
      TRY(builder->read_tile(builder->context, copy, private_tile, 0));
      TRY(builder->write_output(builder->context, copy, request->output,
                                request->member, tile));
    }
  }
  if (context) {
    uint32_t ignored, abi = 7;
    ps_placement_v8 where = cpu;
    if (*(const unsigned*)context == ignore_gpu_v9) {
      where.backend = PS_BACKEND_GPU_V8;
      abi = 9;
    }
    /* Even a complete valid program must be rejected after an ignored error. */
    (void)builder->add_task(builder->context, symbol, contract, &where, scratch,
                            0, abi, &ignored);
  }
  return status(PS_KERNEL_OK_V8);
}
static void fill(const ps_kernel_invocation_v8* query,
                 ps_kernel_status_v8* result) {
  const double value = 42;
  memcpy((unsigned char*)query->outputs[0].allocation +
             query->outputs[0].byte_offset,
         &value, sizeof(value));
  *result = status(PS_KERNEL_OK_V8);
}
static void fill_v9(const ps_kernel_invocation_v9* query,
                    ps_kernel_status_v8* result) {
  *result = query->work.consume(query->work.context, 10, 1);
  if (!result->code)
    fill(&query->views, result);
}
static ps_kernel_status_v8 prepare_v9(void* context,
                                      const ps_kernel_preparation_v8* query,
                                      ps_prepared_kernel_v9* kernel) {
  (void)context;
  if (query->placement.backend != PS_BACKEND_CPU_V8 || query->read_count ||
      query->write_count != 1 || query->writes[0].layout.rank != 1 ||
      query->writes[0].layout.shape[0] != 1)
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  *kernel = (ps_prepared_kernel_v9){
      .struct_size = sizeof(*kernel),
      .abi_version = 9,
      .invoke_cpu = fill_v9};  // NOLINT(readability/braces)
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 prepare_v8(void* context,
                                      const ps_kernel_preparation_v8* query,
                                      ps_prepared_kernel_v8* kernel) {
  (void)context;
  if (query->placement.backend != PS_BACKEND_CPU_V8)
    return status(PS_KERNEL_BACKEND_UNAVAILABLE_V8);
  *kernel = (ps_prepared_kernel_v8){
      .struct_size = sizeof(*kernel),
      .abi_version = 8,
      .invoke_cpu = fill};  // NOLINT(readability/braces)
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 configure(void* context,
                                     const ps_static_query_v8* facts,
                                     const void* state, uint32_t backend,
                                     uint64_t words[64], uint32_t* count) {
  (void)context;
  (void)facts;
  (void)state;
  if (backend > PS_BACKEND_GPU_V8)
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  words[0] = backend;
  *count = 1;
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 describe(void* context,
                                    const ps_static_query_v8* facts,
                                    const void* state, uint32_t backend,
                                    const ps_mapping_query_v1* query,
                                    ps_tile_task_v9* task) {
  (void)context;
  (void)facts;
  (void)state;
  if (query->config_count != 1 || query->config[0] != backend ||
      backend > PS_BACKEND_GPU_V8)
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  *task = (ps_tile_task_v9){
      .struct_size = sizeof(*task),
      .invocation_abi = 8,
      .symbol = TEXT("fill"),
      .contract = TEXT("scalar-v1"),
      .placement = {
          .backend = backend,
          .domain = backend == PS_BACKEND_CPU_V8 ? PS_KERNEL_DOMAIN_HOST_V8
                                                 : PS_KERNEL_DOMAIN_DEVICE_V8,
          .profile = TEXT("exact-f64")}};  // NOLINT(readability/braces)
  return status(PS_KERNEL_OK_V8);
}
static const ps_tilewise_v10 tilewise = {.struct_size = sizeof(tilewise),
                                         .describe = describe,
                                         .configure = configure};
static const ps_operation_v10 operations[] = {
    {.struct_size = sizeof(ps_operation_v10),
     .key = TEXT("fixture.custom-v9"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_PURE_V8 |
              PS_OPERATION_DETERMINISTIC_V8,
     .outputs = &output,
     .output_count = 1,
     .lower = lower,
     .prepare_kernel_v9 = prepare_v9},
    {.struct_size = sizeof(ps_operation_v10),
     .key = TEXT("fixture.bad-abi"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_PURE_V8 |
              PS_OPERATION_DETERMINISTIC_V8,
     .outputs = &output,
     .output_count = 1,
     .user_data = (void*)&ignore_invalid_abi,
     .lower = lower,
     .prepare_kernel_v9 = prepare_v9},
    {.struct_size = sizeof(ps_operation_v10),
     .key = TEXT("fixture.bad-gpu-v9"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_GPU_V8 | PS_OPERATION_PURE_V8 |
              PS_OPERATION_DETERMINISTIC_V8,
     .outputs = &output,
     .output_count = 1,
     .user_data = (void*)&ignore_gpu_v9,
     .lower = lower,
     .prepare_kernel_v9 = prepare_v9},
    {.struct_size = sizeof(ps_operation_v10),
     .key = TEXT("fixture.backend"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_GPU_V8 | PS_OPERATION_PURE_V8 |
              PS_OPERATION_DETERMINISTIC_V8,
     .outputs = &output,
     .output_count = 1,
     .prepare_kernel = prepare_v8,
     .tilewise = &tilewise}};
static void destroy(void* context) {
  (void)context;
}
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v10*
ps_operation_module_get_api_v10(void) {
  static const ps_operation_module_v10 api = {
      .struct_size = sizeof(api),
      .abi_version = PS_OPERATION_MODULE_ABI_VERSION_10,
      .operations = operations,
      .operation_count = sizeof(operations) / sizeof(operations[0]),
      .destroy = destroy};
  return &api;
}
