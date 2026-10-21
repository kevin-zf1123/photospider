#include <stdint.h>
#include <string.h>

#include "photospider/plugin/native_kernel_api.h"

#define TEXT(s) {s, sizeof(s) - 1}
#define TRY(call)                        \
  do {                                   \
    ps_kernel_status_v8 value_ = (call); \
    if (value_.code)                     \
      return value_;                     \
  } while (0)
static const unsigned modes[] = {0, 1, 2, 3};
static const ps_tensor_spec_v8 tensor = {.struct_size = sizeof(tensor),
                                         .key = TEXT("samples"),
                                         .element_type = 1,
                                         .rank = 1,
                                         .shape = {129},
                                         .channel_axis = UINT32_MAX};
static const ps_schema_v8 schema = {.struct_size = sizeof(schema),
                                    .id = TEXT("native.tiles"),
                                    .version = 1,
                                    .tensors = &tensor,
                                    .tensor_count = 1};
static const ps_port_contract_v8 input = {.struct_size = sizeof(input),
                                          .schema_id = TEXT("native.tiles"),
                                          .schema_version = 1};
static const ps_operation_output_v8 output = {
    .struct_size = sizeof(output),
    .key = TEXT("value"),
    .contract = {.struct_size = sizeof(ps_port_contract_v8),
                 .schema_id = TEXT("native.tiles"),
                 .schema_version = 1},
    .prototype = &schema,
    .input_count = UINT32_MAX};
static ps_kernel_status_v8 status(uint32_t code) {
  const ps_kernel_status_v8 result = {.code = code};
  return result;
}
static ps_kernel_status_v8 lower(void* context,
                                 const ps_lowering_query_v8* query,
                                 const ps_operation_builder_v8* builder) {
  const unsigned mode = *(const unsigned*)context;
  const ps_string_v8 symbol = TEXT("native-u8"), contract = TEXT("tiles-v1");
  const ps_placement_v8 placement = {
      .backend = mode >= 2 ? PS_BACKEND_CPU_V8 : PS_BACKEND_GPU_V8,
      .domain =
          mode == 2 ? PS_KERNEL_DOMAIN_HOST_V8 : PS_KERNEL_DOMAIN_SHARED_V8,
      .profile = TEXT("exact-u8")};
  const uint64_t scratch[3] = {0};
  for (unsigned r = 0; r < query->request_count; ++r)
    for (uint64_t tile = query->requests[r].begin;
         tile < query->requests[r].end; ++tile) {
      uint32_t task;
      if (mode >= 2)
        TRY(builder->add_copy(builder->context, &placement, &task));
      else
        TRY(builder->add_task(builder->context, symbol, contract, &placement,
                              scratch, 0, &task));
      if (mode)
        TRY(builder->read_input(
            builder->context, task, 0, 0, tile,
            mode == 3 ? PS_KERNEL_DOMAIN_HOST_V8 : PS_KERNEL_DOMAIN_SHARED_V8));
      TRY(builder->write_output(builder->context, task,
                                query->requests[r].output,
                                query->requests[r].member, tile));
    }
  return status(PS_KERNEL_OK_V8);
}
static const char fill_source[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void run(device uchar* out [[buffer(0)]],"
    "constant uint2& cfg [[buffer(1)]], uint i [[thread_position_in_grid]]) {"
    "out[i] = uchar(cfg.x + i + cfg.y); }\n";
static const char add_source[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void run(device const uchar* in [[buffer(0)]],"
    "device uchar* out [[buffer(1)]], constant uint2& cfg [[buffer(2)]],"
    "uint i [[thread_position_in_grid]]) { out[i] = uchar(in[i] + cfg.y); }\n";
static ps_kernel_status_v8 prepare(void* context,
                                   const ps_kernel_preparation_v8* query,
                                   ps_prepared_kernel_v8* kernel) {
  const unsigned mode = *(const unsigned*)context;
  const ps_native_gpu_service_v2* service = query->native_gpu;
  if (!service || service->struct_size != sizeof(*service) ||
      service->abi_version != PS_NATIVE_GPU_ABI_VERSION_2 ||
      service->backend != PS_NATIVE_METAL_V2)
    return status(PS_KERNEL_BACKEND_UNAVAILABLE_V8);
  if (mode > 1 || query->read_count != mode || query->write_count != 1 ||
      query->parameter_bytes || query->scratch[0] || query->scratch[1] ||
      query->scratch[2] || query->symbol.size != 9 ||
      memcmp(query->symbol.data, "native-u8", 9) || query->contract.size != 8 ||
      memcmp(query->contract.data, "tiles-v1", 8))
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  const ps_kernel_view_spec_v8* out = &query->writes[0];
  if (out->layout.element_type != 1 || out->layout.rank != 1 ||
      out->domain != PS_KERNEL_DOMAIN_SHARED_V8 || out->byte_offset ||
      out->port || out->member || out->layout.strides[0] != 1 || !out->bytes ||
      out->bytes != out->layout.shape[0] || out->layout.origin[0] >= 129 ||
      out->bytes > 129 - out->layout.origin[0])
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  if (mode && (query->reads[0].bytes != out->bytes ||
               query->reads[0].layout.origin[0] != out->layout.origin[0] ||
               query->reads[0].layout.rank != 1 ||
               query->reads[0].layout.element_type != 1 ||
               query->reads[0].layout.strides[0] != 1 ||
               query->reads[0].layout.shape[0] != out->layout.shape[0]))
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  uint32_t constants[2] = {(uint32_t)out->layout.origin[0], mode ? 2 : 41};
  ps_native_binding_v2 bindings[2] = {
      {0, mode ? PS_NATIVE_INPUT_V2 : PS_NATIVE_OUTPUT_V2, 0, 0, out->bytes},
      {1, PS_NATIVE_OUTPUT_V2, 0, 0, out->bytes}};
  const ps_native_program_v2 program = {
      .struct_size = sizeof(program),
      .code_format = PS_NATIVE_MSL_V2,
      .source = {mode ? add_source : fill_source,
                 mode ? sizeof(add_source) - 1 : sizeof(fill_source) - 1},
      .entry = TEXT("run"),
      .bindings = bindings,
      .binding_count = mode ? 2 : 1,
      .constants = constants,
      .constant_bytes = sizeof(constants),
      .constant_index = mode ? 2 : 1,
      .grid = {out->bytes, 1, 1}};
  const ps_kernel_status_v8 result =
      service->prepare(service->context, &program, query, kernel);
  memset(constants, 0xff, sizeof(constants));
  memset(bindings, 0xff, sizeof(bindings));
  return result;
}
static const ps_operation_v8 operations[] = {
    {.struct_size = sizeof(ps_operation_v8),
     .key = TEXT("native.fill"),
     .flags = PS_OPERATION_GPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
              PS_OPERATION_PURE_V8,
     .outputs = &output,
     .output_count = 1,
     .user_data = (void*)&modes[0],
     .lower = lower,
     .prepare_kernel = prepare},
    {.struct_size = sizeof(ps_operation_v8),
     .key = TEXT("native.add"),
     .flags = PS_OPERATION_GPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
              PS_OPERATION_PURE_V8,
     .inputs = &input,
     .input_count = 1,
     .outputs = &output,
     .output_count = 1,
     .user_data = (void*)&modes[1],
     .lower = lower,
     .prepare_kernel = prepare},
    {.struct_size = sizeof(ps_operation_v8),
     .key = TEXT("native.download"),
     .flags = PS_OPERATION_GPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
              PS_OPERATION_PURE_V8,
     .inputs = &input,
     .input_count = 1,
     .outputs = &output,
     .output_count = 1,
     .user_data = (void*)&modes[2],
     .lower = lower},
    {.struct_size = sizeof(ps_operation_v8),
     .key = TEXT("native.upload"),
     .flags = PS_OPERATION_GPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
              PS_OPERATION_PURE_V8,
     .inputs = &input,
     .input_count = 1,
     .outputs = &output,
     .output_count = 1,
     .user_data = (void*)&modes[3],
     .lower = lower}};
static void destroy(void* context) {
  (void)context;
}
static const ps_operation_module_v8 api = {.struct_size = sizeof(api),
                                           .abi_version = 8,
                                           .operations = operations,
                                           .operation_count = 4,
                                           .destroy = destroy};
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  return &api;
}
