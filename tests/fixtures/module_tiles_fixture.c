#include <stdatomic.h>
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

static const uint64_t shape[5] = {2, 2, 2, 3, 5};
static const ps_tensor_spec_v8 tensor = {.struct_size = sizeof(tensor),
                                         .key = TEXT("image"),
                                         .element_type = 3,
                                         .rank = 3,
                                         .batch_rank = 2,
                                         .shape = {2, 3, 5},
                                         .batch_shape = {2, 2},
                                         .spatial = 1,
                                         .height_axis = 1,
                                         .width_axis = 2,
                                         .channel_axis = 0};
static const ps_schema_v8 schema = {.struct_size = sizeof(schema),
                                    .id = TEXT("test.c_tiles"),
                                    .version = 1,
                                    .tensor_count = 1,
                                    .tensors = &tensor};
static const ps_port_contract_v8 input = {.struct_size = sizeof(input),
                                          .schema_id = TEXT("test.c_tiles"),
                                          .schema_version = 1,
                                          .element_type = 3,
                                          .rank = 3};
static const ps_operation_output_v8 output = {
    .struct_size = sizeof(output),
    .key = TEXT("tiled"),
    .contract = {.struct_size = sizeof(ps_port_contract_v8),
                 .schema_id = TEXT("test.c_tiles"),
                 .schema_version = 1},
    .prototype = &schema,
    .input_count = UINT32_MAX};
static const ps_operation_output_v8 schema_output = {
    .struct_size = sizeof(schema_output),
    .key = TEXT("tiled"),
    .contract = {.struct_size = sizeof(ps_port_contract_v8),
                 .schema_id = TEXT("test.c_tiles"),
                 .schema_version = 1},
    .prototype = &schema};
static const unsigned modes[] = {0, 1, 2};
static atomic_uint_fast64_t invocations[3];

static ps_kernel_status_v8 status(uint32_t code) {
  const ps_kernel_status_v8 result = {.code = code};
  return result;
}
static int valid_grid(const ps_tile_grid_v8* grid, uint64_t extent) {
  const uint64_t rows = 1 + 2 / extent, columns = 1 + 4 / extent;
  if (grid->port || grid->member || grid->rank != 5 ||
      grid->batch_axis_mask != 3 || grid->height_axis != 3 ||
      grid->width_axis != 4 || grid->batch_count != 4 || grid->rows != rows ||
      grid->columns != columns || grid->tile_count != 4 * rows * columns)
    return 0;
  for (unsigned axis = 0; axis < 5; ++axis)
    if (grid->shape[axis] != shape[axis])
      return 0;
  return 1;
}
static ps_kernel_status_v8 lower(void* user, const ps_lowering_query_v8* query,
                                 const ps_operation_builder_v8* builder) {
  const unsigned mode = *(const unsigned*)user;
  if (!query->tile_extent || query->output_count != 1 ||
      query->output_grid_count != 1 ||
      query->facts.input_count != (mode ? 1U : 0U) ||
      query->input_grid_count != (mode ? 1U : 0U) ||
      !valid_grid(query->output_grids, query->tile_extent) ||
      (mode && !valid_grid(query->input_grids, query->tile_extent)))
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  const ps_string_v8 symbol = TEXT("tile");
  const ps_string_v8 contract = TEXT("batched-chw-v1");
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
      if (mode == 1)
        TRY(builder->read_input(builder->context, task, 0, 0, tile,
                                PS_KERNEL_DOMAIN_HOST_V8));
      if (mode == 2)
        TRY(builder->read_input_schema(builder->context, task, 0));
      TRY(builder->write_output(builder->context, task, request->output,
                                request->member, tile));
    }
  }
  return status(PS_KERNEL_OK_V8);
}

struct KernelState {
  unsigned mode;
  uint64_t count, origin[5], extent[5], tensor_count;
};
static void release(const void* state) {
  free((void*)state);
}
static void invoke(const ps_kernel_invocation_v8* query,
                   ps_kernel_status_v8* result) {
  const struct KernelState* state = query->prepared_context;
  *result = status(PS_KERNEL_OK_V8);
  if (query->cancellation.is_cancelled &&
      query->cancellation.is_cancelled(query->cancellation.context)) {
    *result = status(PS_KERNEL_CANCELLED_V8);
    return;
  }
  atomic_fetch_add(&invocations[state->mode], 1);
  unsigned char* output = (unsigned char*)query->outputs[0].allocation +
                          query->outputs[0].byte_offset;
  if (state->mode == 1) {
    memcpy(output,
           (const unsigned char*)query->inputs[0].allocation +
               query->inputs[0].byte_offset,
           state->count * sizeof(double));
    return;
  }
  for (uint64_t index = 0; index < state->count; ++index) {
    uint64_t remainder = index, at[5], flat = 0;
    for (unsigned axis = 5; axis-- > 0;) {
      at[axis] = state->origin[axis] + remainder % state->extent[axis];
      remainder /= state->extent[axis];
    }
    for (unsigned axis = 0; axis < 5; ++axis)
      flat = flat * shape[axis] + at[axis];
    const double value =
        (double)(state->mode == 2 ? state->tensor_count : flat + 1);
    memcpy(output + index * sizeof(value), &value, sizeof(value));
  }
}
/* Validate the actual fixed view independently from the query's grid table. */
static int valid_view(const ps_kernel_view_spec_v8* view, uint64_t extent) {
  const uint64_t rows = 1 + 2 / extent, columns = 1 + 4 / extent;
  if (view->port || view->member || view->byte_offset ||
      view->domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      view->layout.element_type != 3 || view->layout.rank != 5 ||
      view->tile_index >= 4 * rows * columns)
    return 0;
  const uint64_t batch = view->tile_index / (rows * columns);
  const uint64_t y = (view->tile_index / columns) % rows * extent;
  const uint64_t x = view->tile_index % columns * extent;
  const uint64_t origin[5] = {batch / 2, batch % 2, 0, y, x};
  const uint64_t sizes[5] = {1, 1, 2, 3 - y < extent ? 3 - y : extent,
                             5 - x < extent ? 5 - x : extent};
  uint64_t bytes = sizeof(double);
  for (unsigned axis = 5; axis-- > 0;) {
    if (view->layout.origin[axis] != origin[axis] ||
        view->layout.shape[axis] != sizes[axis] ||
        view->layout.strides[axis] != (int64_t)bytes)
      return 0;
    bytes *= sizes[axis];
  }
  return view->bytes == bytes;
}
static ps_kernel_status_v8 prepare_kernel(void* user,
                                          const ps_kernel_preparation_v8* query,
                                          ps_prepared_kernel_v8* kernel) {
  const unsigned mode = *(const unsigned*)user;
  if (!query->tile_extent || query->placement.backend != PS_BACKEND_CPU_V8 ||
      query->placement.domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      query->symbol.size != 4 || memcmp(query->symbol.data, "tile", 4) ||
      query->contract.size != 14 ||
      memcmp(query->contract.data, "batched-chw-v1", 14) ||
      query->read_count != (mode == 1 ? 1U : 0U) || query->write_count != 1 ||
      query->facts.input_count != (mode ? 1U : 0U) || query->parameter_bytes ||
      query->scratch[0] || query->scratch[1] || query->scratch[2] ||
      !valid_view(&query->writes[0], query->tile_extent))
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  if (mode == 1 && (!valid_view(&query->reads[0], query->tile_extent) ||
                    query->reads[0].tile_index != query->writes[0].tile_index))
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  struct KernelState* state = malloc(sizeof(*state));
  if (!state)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  state->mode = mode;
  state->count = query->writes[0].bytes / sizeof(double);
  state->tensor_count = mode ? query->facts.inputs[0].tensor_count : 0;
  memcpy(state->origin, query->writes[0].layout.origin, sizeof(state->origin));
  memcpy(state->extent, query->writes[0].layout.shape, sizeof(state->extent));
  *kernel = (ps_prepared_kernel_v8){.struct_size = sizeof(*kernel),
                                    .abi_version = 8,
                                    .context = state,
                                    .release = release,
                                    .invoke_cpu = invoke};
  return status(PS_KERNEL_OK_V8);
}
static const ps_operation_v8 operations[] = {
    {.struct_size = sizeof(ps_operation_v8),
     .key = TEXT("test.c_tiles.generate"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
              PS_OPERATION_PURE_V8,
     .outputs = &output,
     .output_count = 1,
     .user_data = (void*)&modes[0],
     .lower = lower,
     .prepare_kernel = prepare_kernel},
    {.struct_size = sizeof(ps_operation_v8),
     .key = TEXT("test.c_tiles.copy"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
              PS_OPERATION_PURE_V8,
     .inputs = &input,
     .input_count = 1,
     .outputs = &output,
     .output_count = 1,
     .user_data = (void*)&modes[1],
     .lower = lower,
     .prepare_kernel = prepare_kernel},
    {.struct_size = sizeof(ps_operation_v8),
     .key = TEXT("test.c_tiles.schema"),
     .flags = PS_OPERATION_CPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
              PS_OPERATION_PURE_V8,
     .inputs = &input,
     .input_count = 1,
     .outputs = &schema_output,
     .output_count = 1,
     .user_data = (void*)&modes[2],
     .lower = lower,
     .prepare_kernel = prepare_kernel}};
static void destroy(void* context) {
  (void)context;
}
static const ps_operation_module_v8 api = {.struct_size = sizeof(api),
                                           .abi_version = 8,
                                           .operations = operations,
                                           .operation_count = 3,
                                           .destroy = destroy};
PS_OPERATION_MODULE_EXPORT uint64_t ps_module_tiles_invocations(unsigned mode) {
  return mode < 3 ? atomic_load(&invocations[mode]) : UINT64_MAX;
}
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  return &api;
}
