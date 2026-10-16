#include <stdint.h>
#include <string.h>

#include "photospider/plugin/fragment_atlas_msl.h"
#include "photospider/plugin/gpu_discovery_msl.h"
#include "photospider/plugin/result_operation_plugin_api.h"
#include "photospider/plugin/result_tile_helpers.h"

struct State {
  uint64_t coordinate, receipt, base, table_token;
  uint32_t stage, capacity, mode, box;
};
struct DiscoveryContext {
  struct State* state;
  const ps_result_services_v5* outer;
};
static const char discovery_source[] =
    PS_FRAGMENT_ATLAS_MSL_V11 PS_RESULT_GPU_DISCOVERY_MSL_V5
    "kernel void discover(device const uchar* controls [[buffer(0)]],"
    " device const ulong* directory [[buffer(1)]], device atomic_uint* table "
    "[[buffer(2)]],"
    " constant ulong* c [[buffer(3)]], uint i [[thread_position_in_grid]]) {"
    " ulong at[8]={0,c[6]}, address=0, base=0;"
    " if(!ps_atlas_address(directory,c[0],c+1,c+3,2,at,c[5],8,address))"
    " {atomic_store_explicit(table+1,1u,memory_order_relaxed);return;}"
    " for(uint b=0;b<8;++b) base|=ulong(controls[address+b])<<(8*b);"
    " ulong tile=base+ulong(c[8]==13?1-i:i)*4096;"
    " uint kind=c[8]==3?3:(c[8]==4||c[8]==19?1:(c[8]==5?2:0));"
    " if(c[8]==18||c[8]==19)tile=0;"
    " ps_result_discovery_emit(table,uint(c[7]),c[8]==14?15:0,kind,0,tile,c[8]="
    "=19?0:(c[8]==18?8192:tile+1));"
    " if(c[8]==6)atomic_store_explicit(table+2,1u,memory_order_relaxed);}";
static const char value_source[] = PS_FRAGMENT_ATLAS_MSL_V11
    "kernel void value(device const uchar* data [[buffer(0)]],"
    " device const ulong* directory [[buffer(1)]], device uint* output "
    "[[buffer(2)]],"
    " constant ulong* c [[buffer(3)]]) {float sum=0;uint missing=0;"
    " for(uint i=0;i<2;++i) {ulong at[8]={0,c[6+i]}, address=0;"
    " if(!ps_atlas_address(directory,c[0],c+1,c+3,2,at,c[5],4,address))"
    " {missing=1;continue;}uint bits=0;for(uint "
    "b=0;b<4;++b)bits|=uint(data[address+b])<<(8*b);"
    " sum+=as_type<float>(bits);}output[0]=as_type<uint>(sum);output[1]="
    "missing;}";
static ps_gpu_dispatch_v1 command(const char* source, uint32_t source_size,
                                  const char* entry, uint32_t entry_size,
                                  const ps_gpu_buffer_binding_v1* buffers,
                                  const uint64_t* constants, uint64_t threads) {
  ps_gpu_dispatch_v1 result = {0};
  result.struct_size = sizeof(result);
  result.source = source;
  result.source_size = source_size;
  result.entry = entry;
  result.entry_size = entry_size;
  result.buffers = buffers;
  result.buffer_count = 3;
  result.constants = constants;
  result.constant_size = 10 * 8;
  result.constant_index = 3;
  result.grid[0] = threads;
  result.grid[1] = result.grid[2] = 1;
  return result;
}

static int discover(const ps_result_discovery_services_v5* host, uint8_t* table,
                    uint64_t size, uint32_t capacity, void* user) {
  struct DiscoveryContext* context = user;
  struct State* state = context->state;
  if (state->mode == 9) {
    uint64_t ignored = 0;
    context->outer->discover(context->outer->context, 1, 1, discover, user,
                             &ignored);
    return 1;
  }
  if (state->mode == 10)
    return 0;
  if ((state->mode == 16 || state->mode == 17) &&
      host->consume_work(host->context, 4096))
    return 1;
  ps_result_native_atlas_v5 atlas = {0};
  atlas.struct_size = sizeof(atlas);
  uint64_t token = 0;
  if (host->acquire_native_atlas(host->context, 1, 0, &atlas) ||
      host->gpu->buffer(host->gpu->context, table, size, 1, &token))
    return 1;
  state->table_token = token;
  const ps_gpu_buffer_binding_v1 buffers[] = {
      {sizeof(ps_gpu_buffer_binding_v1), 0, atlas.payload_token, 0,
       atlas.payload_byte_size, 0},
      {sizeof(ps_gpu_buffer_binding_v1), 1, atlas.directory_token, 0,
       atlas.directory_byte_size, 0},
      {sizeof(ps_gpu_buffer_binding_v1), 2, token, 0, size, 1}};
  const uint64_t constants[] = {atlas.slot_count,    atlas.shape[0],
                                atlas.shape[1],      atlas.tile_shape[0],
                                atlas.tile_shape[1], atlas.payload_sample_bytes,
                                state->coordinate,   capacity,
                                state->mode,         0};
  const ps_gpu_dispatch_v1 work =
      command(discovery_source, sizeof(discovery_source) - 1, "discover", 8,
              buffers, constants, 2);
  return host->gpu->execute(host->gpu->context, &work, 1);
}
static int need(const ps_result_services_v5* host,
                const ps_result_query_v5* query, uint32_t input, uint64_t first,
                uint32_t count) {
  (void)query;
  const ps_result_tile_interval_v5 tiles[2] = {{first, first + 1},
                                               {first + 4096, first + 4097}};
  const ps_result_tile_need_v5 request = {.struct_size = sizeof(request),
                                          .input = input,
                                          .intervals = tiles,
                                          .interval_count = count};
  return host->need_tiles(host->context, &request) ? 1 : PS_RESULT_NEED_V5;
}
static int start(void* user, void* bytes, const ps_result_query_v5* query,
                 const ps_result_services_v5* host) {
  (void)user;
  (void)host;
  if (query->tile_extent != 1)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  struct State* state = bytes;
  state->capacity = (uint32_t)query->parameters[0].int64_value;
  state->mode = (uint32_t)query->parameters[1].int64_value;
  return 0;
}
static int finish(const ps_result_query_v5* query,
                  const ps_result_services_v5* host, struct State* state) {
  uint64_t coordinates[2] = {state->base, state->base + 4096};
  float sum = 0;
  if (query->backend == 2) {
    ps_result_discovery_request_v5 requests[2] = {{0}};
    uint32_t count = 0;
    if (host->discovery_requests(host->context, state->receipt, NULL, 0,
                                 &count) ||
        count != (state->mode >= 18 ? 1U : 2U))
      return 1;
    if (host->discovery_requests(host->context, state->receipt, requests, 2,
                                 &count))
      return 1;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t expected = state->mode == 19
                                    ? PS_RESULT_DISCOVERY_DESCRIPTOR_V5
                                    : PS_RESULT_DISCOVERY_TILE_V5;
      if (requests[i].input != 0 || requests[i].reserved ||
          requests[i].kind != expected ||
          (expected && (requests[i].begin || requests[i].end)) ||
          requests[i].tensor_slot)
        return 1;
      if (!expected && state->mode != 18) {
        if (requests[i].end != requests[i].begin + 1)
          return 1;
        coordinates[i] = requests[i].begin;
      }
    }
    if (state->mode == 19) {
      const uint64_t at[2] = {0, 0};
      float ignored = 0;
      host->read_tensor(host->context, 0, 0, at, 2, &ignored, 4);
      return PS_RESULT_STATUS_CANCELLED_V5;
    }
    if (host->release_discovery(host->context, state->receipt))
      return 1;
    ps_result_native_atlas_v5 atlas = {0};
    atlas.struct_size = sizeof(atlas);
    uint8_t* scratch = NULL;
    uint64_t token = 0;
    if (host->allocate_scratch(host->context, 8, &scratch) ||
        host->acquire_native_atlas(host->context, 0, 0, &atlas) ||
        host->gpu->buffer(host->gpu->context, scratch, 8, 1, &token))
      return 1;
    const ps_gpu_buffer_binding_v1 buffers[] = {
        {sizeof(ps_gpu_buffer_binding_v1), 0, atlas.payload_token, 0,
         atlas.payload_byte_size, 0},
        {sizeof(ps_gpu_buffer_binding_v1), 1, atlas.directory_token, 0,
         atlas.directory_byte_size, 0},
        {sizeof(ps_gpu_buffer_binding_v1), 2, token, 0, 8, 1}};
    const uint64_t constants[] = {atlas.slot_count,
                                  atlas.shape[0],
                                  atlas.shape[1],
                                  atlas.tile_shape[0],
                                  atlas.tile_shape[1],
                                  atlas.payload_sample_bytes,
                                  coordinates[0],
                                  coordinates[1],
                                  0,
                                  0};
    const ps_gpu_dispatch_v1 work =
        command(value_source, sizeof(value_source) - 1, "value", 5, buffers,
                constants, 1);
    if (host->gpu->execute(host->gpu->context, &work, 1))
      return 1;
    uint32_t missing = 0;
    memcpy(&missing, scratch + 4, 4);
    if (missing)
      return 1;
    memcpy(&sum, scratch, 4);
    if (host->gpu->release(host->gpu->context, token) ||
        host->release_scratch(host->context, scratch))
      return 1;
  } else {
    for (uint32_t i = 0; i < 2; ++i) {
      float value = 0;
      const uint64_t at[2] = {0, coordinates[i]};
      if (host->read_tensor(host->context, 0, 0, at, 2, &value, 4))
        return 1;
      sum += value;
    }
  }
  uint8_t* output = NULL;
  if (host->allocate_scratch(host->context, 4, &output))
    return 1;
  memcpy(output, &sum, 4);
  const ps_result_region_v5 region = {.struct_size = sizeof(region),
                                      .rank = 2,
                                      .offset = {0, state->coordinate},
                                      .extent = {1, 1}};
  return host->publish_tensor_buffer(host->context, 0, &region, output, 4);
}
static int poll(void* user, void* bytes, const ps_result_query_v5* query,
                const ps_result_services_v5* host) {
  (void)user;
  ps_result_region_v5 requested_region;
  if (ps_result_output_region_v5(query, 0, &requested_region))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V5;
  const uint32_t requested_count = 1;
  struct State* state = bytes;
  if (!state->stage) {
    if (host->begin_result(host->context))
      return 1;
    if (!requested_count)
      return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V5;
    state->coordinate = requested_region.offset[1];
    state->stage = 1;
    return need(host, query, 1, state->coordinate, 1);
  }
  if (state->stage == 1 || state->stage == 3) {
    ++state->stage;
    if (query->backend == 2) {
      struct DiscoveryContext context = {state, host};
      host->discover(host->context, state->capacity, state->mode == 15 ? 0 : 2,
                     discover, &context, &state->receipt);
      if (state->mode == 1)
        return host->publish_result(host->context, 1) ? 1
                                                      : PS_RESULT_PUBLISH_V5;
      if (state->mode == 11) {
        if (host->release_discovery(host->context, state->receipt))
          return 1;
        host->release_discovery(host->context, state->receipt);
        return 1;
      }
      if (state->mode == 12) {
        ps_result_discovery_request_v5 record = {0};
        uint32_t count = 0;
        host->discovery_requests(host->context, state->receipt, &record, 1,
                                 &count);
        return 1;
      }
      if (state->mode == 8) {
        const char source[] =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void put(device uint* out [[buffer(0)]]){out[0]=0;}";
        const ps_gpu_buffer_binding_v1 binding = {
            sizeof(binding), 0, state->table_token, 0, 4, 1};
        ps_gpu_dispatch_v1 command = {0};
        command.struct_size = sizeof(command);
        command.source = source;
        command.source_size = sizeof(source) - 1;
        command.entry = "put";
        command.entry_size = 3;
        command.buffers = &binding;
        command.buffer_count = 1;
        command.grid[0] = command.grid[1] = command.grid[2] = 1;
        host->gpu->execute(host->gpu->context, &command, 1);
        return 1;
      }
      return PS_RESULT_NEED_V5;
    }
    int64_t base = 0;
    const uint64_t at[2] = {0, state->coordinate};
    if (host->read_tensor(host->context, 1, 0, at, 2, &base, 8))
      return 1;
    if (base < 0 || base >= 4096)
      return 6;
    state->base = (uint64_t)base;
    return need(host, query, 0, state->base, 2);
  }
  if (query->backend == 2 && state->mode == 17 && state->stage == 2) {
    if (host->release_discovery(host->context, state->receipt))
      return 1;
    state->stage = 3;
    return need(host, query, 1, state->coordinate, 1);
  }
  if (finish(query, host, state))
    return 1;
  ++state->coordinate;
  if (state->coordinate ==
      requested_region.offset[1] + requested_region.extent[1]) {
    if (++state->box < requested_count)
      state->coordinate = requested_region.offset[1];
  }
  if (state->box == requested_count)
    return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V5;
  state->stage = 1;
  return need(host, query, 1, state->coordinate, 1);
}
static void destroy(void* user, void* bytes) {
  (void)user;
  (void)bytes;
}
static void release(void* user) {
  (void)user;
}
static const ps_result_tensor_spec_v5 tensors[] = {
    {.struct_size = sizeof(ps_result_tensor_spec_v5),
     .key = "samples",
     .key_size = 7,
     .element_type = PS_RESULT_ELEMENT_FLOAT32_V5,
     .rank = 2,
     .shape = {1, 8192},
     .spatial = 1,
     .height_axis = 0,
     .width_axis = 1,
     .channel_axis = PS_RESULT_NO_CHANNEL_V5},
    {.struct_size = sizeof(ps_result_tensor_spec_v5),
     .key = "samples",
     .key_size = 7,
     .element_type = PS_RESULT_ELEMENT_INT64_V5,
     .rank = 2,
     .shape = {1, 8192},
     .spatial = 1,
     .height_axis = 0,
     .width_axis = 1,
     .channel_axis = PS_RESULT_NO_CHANNEL_V5}};
static const ps_result_schema_v5 schemas[] = {
    {.struct_size = sizeof(ps_result_schema_v5),
     .id = "discovery.data",
     .id_size = 14,
     .version = 1,
     .tensors = tensors,
     .tensor_count = 1},
    {.struct_size = sizeof(ps_result_schema_v5),
     .id = "discovery.control",
     .id_size = 17,
     .version = 1,
     .tensors = tensors + 1,
     .tensor_count = 1}};
static const ps_result_port_v5 inputs[] = {
    {.struct_size = sizeof(ps_result_port_v5), .schema = schemas},
    {.struct_size = sizeof(ps_result_port_v5), .schema = schemas + 1}};
static const ps_result_output_v5 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v5), .schema = schemas},
    .input_count = UINT32_MAX};
static const ps_result_parameter_descriptor_v5 parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v5), "capacity", 8,
     PS_RESULT_PARAMETER_INT64_V5, 1, 1, 1, 65536},
    {sizeof(ps_result_parameter_descriptor_v5), "mode", 4,
     PS_RESULT_PARAMETER_INT64_V5, 1, 1, 0, 19}};
static const ps_result_operation_v5 operation = {
    .struct_size = sizeof(operation),
    .key = "example.c_discovery",
    .key_size = 19,
    .flags = PS_RESULT_FLAG_CPU_V5 | PS_RESULT_FLAG_GPU_V5 |
             PS_RESULT_FLAG_DETERMINISTIC_V5 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5,
    .inputs = inputs,
    .input_count = 2,
    .outputs = &output,
    .output_count = 1,
    .parameters = parameters,
    .parameter_count = 2,
    .state_bytes = sizeof(struct State),
    .workspace_bytes = 8,
    .maximum_stages = 65536,
    .start = start,
    .poll = poll,
    .destroy = destroy};
static const ps_result_operation_plugin_api_v5 api = {
    sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_5, &operation, 1, NULL,
    release};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v5*
ps_result_operation_plugin_get_api_v5(void) {
  return &api;
}
