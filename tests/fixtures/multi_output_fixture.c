#include <stddef.h>
#include <string.h>

#include "photospider/plugin/operation_module_api.h"

#define TEXT(s) {s, sizeof(s) - 1}

static const ps_tensor_spec_v8 tensor = {.struct_size = sizeof(tensor),
                                         .key = TEXT("number"),
                                         .element_type = 3,
                                         .rank = 1,
                                         .shape = {1},
                                         .channel_axis = UINT32_MAX};
static const ps_schema_v8 schema = {.struct_size = sizeof(schema),
                                    .id = TEXT("test.multi_output"),
                                    .version = 1,
                                    .tensor_count = 1,
                                    .tensors = &tensor};
static const ps_operation_output_v8 outputs[] = {
    {.struct_size = sizeof(ps_operation_output_v8),
     .key = TEXT("first"),
     .contract = {.struct_size = sizeof(ps_port_contract_v8),
                  .schema_id = TEXT("test.multi_output"),
                  .schema_version = 1},
     .prototype = &schema},
    {.struct_size = sizeof(ps_operation_output_v8),
     .key = TEXT("second"),
     .contract = {.struct_size = sizeof(ps_port_contract_v8),
                  .schema_id = TEXT("test.multi_output"),
                  .schema_version = 1},
     .prototype = &schema}};
static ps_kernel_status_v8 lower(void* user, const ps_lowering_query_v8* query,
                                 const ps_operation_builder_v8* builder) {
  (void)user;
  const ps_string_v8 symbol = TEXT("value");
  const ps_string_v8 contract = TEXT("value-v1");
  const ps_placement_v8 placement = {.backend = PS_BACKEND_CPU_V8,
                                     .domain = PS_KERNEL_DOMAIN_HOST_V8,
                                     .profile = TEXT("cpu")};
  const uint64_t scratch[3] = {0};
  for (uint32_t i = 0; i < query->request_count; ++i) {
    const ps_tile_request_v8* request = &query->requests[i];
    for (uint64_t tile = request->begin; tile < request->end; ++tile) {
      uint32_t task;
      ps_kernel_status_v8 status = builder->add_task(
          builder->context, symbol, contract, &placement, scratch, 0, &task);
      if (status.code)
        return status;
      status = builder->write_output(builder->context, task, request->output,
                                     request->member, tile);
      if (status.code)
        return status;
    }
  }
  return (ps_kernel_status_v8){0};
}
static void invoke(const ps_kernel_invocation_v8* invocation,
                   ps_kernel_status_v8* status) {
  *status = (ps_kernel_status_v8){0};
  if (invocation->cancellation.is_cancelled &&
      invocation->cancellation.is_cancelled(invocation->cancellation.context)) {
    status->code = PS_KERNEL_CANCELLED_V8;
    return;
  }
  memcpy((unsigned char*)invocation->outputs[0].allocation +
             invocation->outputs[0].byte_offset,
         invocation->prepared_context, sizeof(double));
}
static void release(const void* state) {
  (void)state;
}
static ps_kernel_status_v8 prepare_kernel(void* user,
                                          const ps_kernel_preparation_v8* query,
                                          ps_prepared_kernel_v8* kernel) {
  (void)user;
  static const double values[2] = {10, 11};
  if (query->placement.backend != PS_BACKEND_CPU_V8 ||
      query->placement.domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      query->symbol.size != 5 || memcmp(query->symbol.data, "value", 5) ||
      query->contract.size != 8 ||
      memcmp(query->contract.data, "value-v1", 8) || query->read_count ||
      query->write_count != 1 || query->parameter_bytes || query->scratch[0] ||
      query->scratch[1] || query->scratch[2])
    return (ps_kernel_status_v8){.code = PS_KERNEL_INVALID_ARGUMENT_V8};
  const ps_kernel_view_spec_v8* output = &query->writes[0];
  if (output->port >= 2 || output->member || output->tile_index ||
      output->domain != PS_KERNEL_DOMAIN_HOST_V8 || output->byte_offset ||
      output->bytes != 8 || output->layout.rank != 1 ||
      output->layout.element_type != 3 || output->layout.shape[0] != 1 ||
      output->layout.origin[0] || output->layout.strides[0] != 8)
    return (ps_kernel_status_v8){.code = PS_KERNEL_INVALID_ARGUMENT_V8};
  *kernel = (ps_prepared_kernel_v8){.struct_size = sizeof(*kernel),
                                    .abi_version = 8,
                                    .context = &values[output->port],
                                    .release = release,
                                    .invoke_cpu = invoke};
  return (ps_kernel_status_v8){0};
}
static void destroy(void* context) {
  (void)context;
}
static const ps_operation_v8 operation = {
    .struct_size = sizeof(operation),
    .key = TEXT("test.c_results"),
    .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
             PS_OPERATION_PURE_V8,
    .outputs = outputs,
    .output_count = 2,
    .lower = lower,
    .prepare_kernel = prepare_kernel};
static const ps_operation_module_v8 api = {.struct_size = sizeof(api),
                                           .abi_version = 8,
                                           .operations = &operation,
                                           .operation_count = 1,
                                           .destroy = destroy};
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  return &api;
}
