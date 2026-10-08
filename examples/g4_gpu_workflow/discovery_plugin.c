#include <stdint.h>
#include <string.h>

#include "photospider/plugin/fragment_atlas_msl.h"
#include "photospider/plugin/gpu_discovery_msl.h"
#include "photospider/plugin/result_operation_plugin_api.h"

struct State {
  uint64_t coordinate, receipt, base, table_token;
  uint32_t stage, capacity, mode, box;
};
struct DiscoveryContext {
  struct State* state;
  const ps_result_services_v2* outer;
};
static const char discovery_source[] =
    PS_FRAGMENT_ATLAS_MSL_V11 PS_RESULT_GPU_DISCOVERY_MSL_V2
    "kernel void discover(device const uchar* controls [[buffer(0)]],"
    " device const ulong* directory [[buffer(1)]], device atomic_uint* table "
    "[[buffer(2)]],"
    " constant ulong* c [[buffer(3)]], uint i [[thread_position_in_grid]]) {"
    " ulong at[8]={c[4]}, address=0, base=0;"
    " if(!ps_atlas_address(directory,c[0],c+1,c+2,1,at,c[3],8,address))"
    " {atomic_store_explicit(table+1,1u,memory_order_relaxed);return;}"
    " for(uint b=0;b<8;++b) base|=ulong(controls[address+b])<<(8*b);"
    " ulong offsets[8]={base+ulong(i)*4096}, extents[8]={c[6]==5?0ul:1ul};"
    " ps_result_discovery_emit(table,uint(c[5]),0,uint(c[6])==14?15:0,"
    " uint(c[6])==3?0:(uint(c[6])==13 && i==1?4:1),"
    " uint(c[6])==4?0:1,offsets,extents);"
    " if(c[6]==6)atomic_store_explicit(table+2,1u,memory_order_relaxed);}";
static const char value_source[] = PS_FRAGMENT_ATLAS_MSL_V11
    "kernel void value(device const uchar* data [[buffer(0)]],"
    " device const ulong* directory [[buffer(1)]], device uint* output "
    "[[buffer(2)]],"
    " constant ulong* c [[buffer(3)]]) {float sum=0;uint missing=0;"
    " for(uint i=0;i<2;++i) {ulong at[8]={c[4+i]}, address=0;"
    " if(!ps_atlas_address(directory,c[0],c+1,c+2,1,at,c[3],4,address))"
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
  result.constant_size = 7 * 8;
  result.constant_index = 3;
  result.grid[0] = threads;
  result.grid[1] = result.grid[2] = 1;
  return result;
}

static int discover(const ps_result_discovery_services_v2* host, uint8_t* table,
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
  if (state->mode == 16 && host->consume_work(host->context, 4096))
    return 1;
  ps_result_native_atlas_v2 atlas = {0};
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
                                atlas.tile_shape[0], atlas.payload_sample_bytes,
                                state->coordinate,   capacity,
                                state->mode};
  const ps_gpu_dispatch_v1 work =
      command(discovery_source, sizeof(discovery_source) - 1, "discover", 8,
              buffers, constants, 2);
  return host->gpu->execute(host->gpu->context, &work, 1);
}
static int need(const ps_result_services_v2* host, uint32_t input,
                uint64_t first, uint32_t count) {
  ps_result_region_v2 boxes[2] = {{0}};
  for (uint32_t i = 0; i < count; ++i) {
    boxes[i].struct_size = sizeof(boxes[i]);
    boxes[i].rank = 1;
    boxes[i].offset[0] = first + i * 4096;
    boxes[i].extent[0] = 1;
  }
  return host->need_tensor(host->context, input, 0, input ? 10 : 9, boxes,
                           count)
             ? 1
             : PS_RESULT_NEED_V2;
}
static int start(void* user, void* bytes, const ps_result_query_v2* query,
                 const ps_result_services_v2* host) {
  (void)user;
  (void)host;
  struct State* state = bytes;
  state->capacity = (uint32_t)query->parameters[0].int64_value;
  state->mode = (uint32_t)query->parameters[1].int64_value;
  return 0;
}
static int finish(const ps_result_query_v2* query,
                  const ps_result_services_v2* host, struct State* state) {
  uint64_t coordinates[2] = {state->base, state->base + 4096};
  uint32_t roles[2] = {1, 1};
  float sum = 0;
  if (query->backend == 2) {
    ps_result_discovery_request_v2 requests[2] = {{0}};
    requests[0].struct_size = requests[1].struct_size = sizeof(requests[0]);
    uint32_t count = 0;
    if (host->discovery_requests(host->context, state->receipt, requests, 2,
                                 &count) ||
        count != 2)
      return 1;
    for (uint32_t i = 0; i < 2; ++i) {
      if (requests[i].input != 0 || requests[i].slot ||
          requests[i].region.rank != 1 || requests[i].region.extent[0] != 1)
        return 1;
      coordinates[i] = requests[i].region.offset[0];
      roles[i] = requests[i].roles;
    }
    if (host->release_discovery(host->context, state->receipt))
      return 1;
    ps_result_native_atlas_v2 atlas = {0};
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
                                  atlas.tile_shape[0],
                                  atlas.payload_sample_bytes,
                                  coordinates[0],
                                  coordinates[1],
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
      if (host->read_tensor(host->context, 0, 0, coordinates + i, 1, &value, 4))
        return 1;
      sum += value;
    }
  }
  uint8_t* output = NULL;
  if (host->allocate_scratch(host->context, 4, &output))
    return 1;
  memcpy(output, &sum, 4);
  const ps_result_region_v2 region = {.struct_size = sizeof(region),
                                      .rank = 1,
                                      .offset = {state->coordinate},
                                      .extent = {1}};
  const ps_result_relation_row_v2 rows[] = {
      {state->coordinate, 0, roles[0], PS_RESULT_TARGET_TENSOR_V2, 0,
       coordinates[0], 1},
      {state->coordinate, 0, roles[1], PS_RESULT_TARGET_TENSOR_V2, 0,
       coordinates[1], 1},
      {state->coordinate, 1, 2, PS_RESULT_TARGET_TENSOR_V2, 0,
       state->coordinate, 1}};
  return host->publish_tensor_buffer(host->context, 0, &region, output, 4, rows,
                                     3, PS_RESULT_EXACT_V2, PS_RESULT_FINAL_V2);
}
static int poll(void* user, void* bytes, const ps_result_query_v2* query,
                const ps_result_services_v2* host) {
  (void)user;
  ps_result_query_v2 whole_query;
  ps_result_region_v2 whole_region = {0};
  if (query->requested_kind == 0) {
    whole_query = *query;
    whole_region.struct_size = sizeof(whole_region);
    whole_region.rank = 1;
    whole_region.extent[0] = 8192;
    whole_query.requested = &whole_region;
    whole_query.requested_count = 1;
    query = &whole_query;
  }
  struct State* state = bytes;
  if (!state->stage) {
    const ps_result_relation_row_v2 rows[] = {
        {0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1},
        {0, 1, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1}};
    if (host->begin_result(host->context) ||
        host->bind_descriptor(host->context, rows, 2, PS_RESULT_EXACT_V2))
      return 1;
    if (!query->requested_count)
      return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V2;
    state->coordinate = query->requested[0].offset[0];
    state->stage = 1;
    return need(host, 1, state->coordinate, 1);
  }
  if (state->stage == 1) {
    state->stage = 2;
    if (query->backend == 2) {
      struct DiscoveryContext context = {state, host};
      host->discover(host->context, state->capacity, state->mode == 15 ? 0 : 2,
                     discover, &context, &state->receipt);
      if (state->mode == 1)
        return host->publish_result(host->context, 1) ? 1
                                                      : PS_RESULT_PUBLISH_V2;
      if (state->mode == 11) {
        if (host->release_discovery(host->context, state->receipt))
          return 1;
        host->release_discovery(host->context, state->receipt);
        return 1;
      }
      if (state->mode == 12) {
        ps_result_discovery_request_v2 record = {.struct_size = sizeof(record)};
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
      return PS_RESULT_NEED_V2;
    }
    int64_t base = 0;
    if (host->read_tensor(host->context, 1, 0, &state->coordinate, 1, &base, 8))
      return 1;
    if (base < 0 || base >= 4096)
      return 6;
    state->base = (uint64_t)base;
    return need(host, 0, state->base, 2);
  }
  if (finish(query, host, state))
    return 1;
  ++state->coordinate;
  if (state->coordinate == query->requested[state->box].offset[0] +
                               query->requested[state->box].extent[0]) {
    if (++state->box < query->requested_count)
      state->coordinate = query->requested[state->box].offset[0];
  }
  if (state->box == query->requested_count)
    return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V2;
  state->stage = 1;
  return need(host, 1, state->coordinate, 1);
}
static void destroy(void* user, void* bytes) {
  (void)user;
  (void)bytes;
}
static void release(void* user) {
  (void)user;
}
static const ps_result_tensor_spec_v2 tensors[] = {
    {.struct_size = sizeof(ps_result_tensor_spec_v2),
     .key = "samples",
     .key_size = 7,
     .element_type = PS_RESULT_ELEMENT_FLOAT32_V2,
     .rank = 1,
     .shape = {8192}},
    {.struct_size = sizeof(ps_result_tensor_spec_v2),
     .key = "samples",
     .key_size = 7,
     .element_type = PS_RESULT_ELEMENT_INT64_V2,
     .rank = 1,
     .shape = {8192}}};
static const ps_result_schema_v2 schemas[] = {
    {.struct_size = sizeof(ps_result_schema_v2),
     .id = "discovery.data",
     .id_size = 14,
     .version = 1,
     .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
     .tensors = tensors,
     .tensor_count = 1},
    {.struct_size = sizeof(ps_result_schema_v2),
     .id = "discovery.control",
     .id_size = 17,
     .version = 1,
     .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
     .tensors = tensors + 1,
     .tensor_count = 1}};
static const ps_result_port_v2 inputs[] = {
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = schemas},
    {.struct_size = sizeof(ps_result_port_v2),
     .kind = PS_RESULT_OBJECT_V2,
     .schema = schemas + 1}};
static const ps_result_output_v2 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = schemas},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_REGIONAL_V2};
static const ps_result_parameter_descriptor_v2 parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v2), "capacity", 8,
     PS_RESULT_PARAMETER_INT64_V2, 1, 1, 1, 65536},
    {sizeof(ps_result_parameter_descriptor_v2), "mode", 4,
     PS_RESULT_PARAMETER_INT64_V2, 1, 1, 0, 16}};
static const ps_result_operation_v2 operation = {
    .struct_size = sizeof(operation),
    .key = "example.c_discovery",
    .key_size = 19,
    .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_GPU_V2 |
             PS_RESULT_FLAG_DETERMINISTIC_V2 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
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
static const ps_result_operation_plugin_api_v2 api = {
    sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_2, &operation, 1, NULL,
    release};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}
