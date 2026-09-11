#include <stdint.h>
#include <string.h>

#include "photospider/plugin/dependency_plugin_api.h"
#include "photospider/plugin/fragment_atlas_msl.h"
#include "photospider/plugin/gpu_discovery_msl.h"

struct State {
  uint64_t coordinate;
  uint32_t stage, capacity, mode;
};
static const char discovery_source[] =
    PS_FRAGMENT_ATLAS_MSL_V9 PS_GPU_DISCOVERY_MSL_V9
    "kernel void discover(device const uchar* controls [[buffer(0)]],"
    " device const ulong* directory [[buffer(1)]], device atomic_uint* table "
    "[[buffer(2)]],"
    " constant ulong* c [[buffer(3)]], uint i [[thread_position_in_grid]]) {"
    " ulong at[8]={c[4]}, address=0, base=0;"
    " if(!ps_atlas_address(directory,c[0],c+1,c+2,1,at,c[3],8,address))"
    " {atomic_store_explicit(table+1,1u,memory_order_relaxed);return;}"
    " for(uint b=0;b<8;++b) base|=ulong(controls[address+b])<<(8*b);"
    " ulong offsets[8]={base+ulong(i)*4096}, extents[8]={1};"
    " ps_discovery_emit(table,uint(c[5]),0,1,1,offsets,extents);}";
static const char value_source[] = PS_FRAGMENT_ATLAS_MSL_V9
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
static int validate(const ps_dependency_metadata_query_v9* q, void* user) {
  (void)user;
  return q->input_count == 2 && q->inputs[0].rank == 1 &&
                 q->inputs[1].rank == 1 && q->inputs[0].shape[0] == 8192 &&
                 q->inputs[1].shape[0] == 8192 &&
                 q->inputs[0].element_type == PS_OPERATION_ELEMENT_FLOAT32_V9 &&
                 q->inputs[1].element_type == PS_OPERATION_ELEMENT_INT64_V9
             ? PS_OPERATION_RESULT_SUCCESS_V9
             : PS_DEPENDENCY_INVALID_ARGUMENT_V9;
}
static int start(const ps_dependency_query_v9* q, void* bytes, uint64_t size,
                 void* user) {
  (void)user;
  if (size != sizeof(struct State))
    return PS_OPERATION_RESULT_FAILURE_V9;
  struct State* s = bytes;
  s->coordinate = q->outputs[0].offsets[0];
  s->capacity = (uint32_t)q->metadata.parameters[0].int64_value;
  s->mode = (uint32_t)q->metadata.parameters[1].int64_value;
  return PS_OPERATION_RESULT_SUCCESS_V9;
}
static ps_gpu_dispatch_v9 command(const char* source, uint32_t source_size,
                                  const char* entry, uint32_t entry_size,
                                  const ps_gpu_buffer_binding_v9* buffers,
                                  const uint64_t* constants, uint64_t threads) {
  ps_gpu_dispatch_v9 result = {0};
  result.struct_size = sizeof(result);
  result.source = source;
  result.source_size = source_size;
  result.entry = entry;
  result.entry_size = entry_size;
  result.buffers = buffers;
  result.buffer_count = 3;
  result.constants = constants;
  result.constant_size = 6 * 8;
  result.constant_index = 3;
  result.grid[0] = threads;
  result.grid[1] = result.grid[2] = 1;
  return result;
}
static int discover(const ps_dependency_discovery_services_v9* host,
                    uint8_t* table, uint64_t size, uint32_t capacity,
                    void* user) {
  const struct State* s = user;
  ps_dependency_atlas_v9 atlas = {0};
  atlas.struct_size = sizeof(atlas);
  uint64_t token = 0;
  if (!host->atlas(host->context, 1, &atlas) ||
      !host->gpu_buffer(host->context, table, size, 1, &token))
    return PS_OPERATION_RESULT_FAILURE_V9;
  const ps_gpu_buffer_binding_v9 buffers[] = {
      {sizeof(ps_gpu_buffer_binding_v9), 0, atlas.payload_token, 0,
       atlas.payload_byte_size, 0},
      {sizeof(ps_gpu_buffer_binding_v9), 1, atlas.directory_token, 0,
       atlas.directory_byte_size, 0},
      {sizeof(ps_gpu_buffer_binding_v9), 2, token, 0, size, 1}};
  const uint64_t constants[] = {atlas.slot_count,    atlas.shape[0],
                                atlas.tile_shape[0], atlas.payload_sample_bytes,
                                s->coordinate,       capacity};
  const ps_gpu_dispatch_v9 work =
      command(discovery_source, sizeof(discovery_source) - 1, "discover", 8,
              buffers, constants, 2);
  return host->gpu_execute(host->context, &work, 1)
             ? PS_OPERATION_RESULT_SUCCESS_V9
             : PS_OPERATION_RESULT_FAILURE_V9;
}
static int needs(const ps_dependency_query_v9* q,
                 const ps_dependency_services_v9* host, uint32_t port,
                 uint64_t first, uint32_t count) {
  ps_dependency_run_v9 runs[2] = {{0}};
  for (uint32_t i = 0; i < count; ++i) {
    runs[i].struct_size = sizeof(runs[i]);
    runs[i].rank = 1;
    runs[i].offsets[0] = first + i * 4096;
    runs[i].extents[0] = 1;
  }
  ps_dependency_association_v9 a = {0};
  a.struct_size = sizeof(a);
  a.output_rank = 1;
  a.output[0] = q->outputs[0].offsets[0];
  a.port = port;
  a.roles = port ? 2 : 1;
  a.runs = runs;
  a.run_count = count;
  return host->associate(host->context, &a) ? PS_DEPENDENCY_NEED_V9
                                            : PS_OPERATION_RESULT_FAILURE_V9;
}
static int finish(const ps_dependency_query_v9* q,
                  const ps_dependency_services_v9* host) {
  if (host->fragment_count(host->context, 0) != 2)
    return PS_OPERATION_RESULT_FAILURE_V9;
  uint64_t coordinates[2] = {0};
  for (uint32_t i = 0; i < 2; ++i) {
    ps_dependency_fragment_v9 fragment = {0};
    fragment.struct_size = sizeof(fragment);
    if (!host->fragment(host->context, 0, i, &fragment))
      return PS_OPERATION_RESULT_FAILURE_V9;
    coordinates[i] = fragment.offsets[0];
  }
  float sum = 0;
  if (q->backend == 2) {
    ps_dependency_atlas_v9 atlas = {0};
    atlas.struct_size = sizeof(atlas);
    uint8_t* scratch = host->allocate_scratch(host->context, 8);
    uint64_t token = 0;
    if (!scratch || !host->atlas(host->context, 0, &atlas) ||
        !host->gpu_buffer(host->context, scratch, 8, 1, &token))
      return PS_OPERATION_RESULT_FAILURE_V9;
    const ps_gpu_buffer_binding_v9 buffers[] = {
        {sizeof(ps_gpu_buffer_binding_v9), 0, atlas.payload_token, 0,
         atlas.payload_byte_size, 0},
        {sizeof(ps_gpu_buffer_binding_v9), 1, atlas.directory_token, 0,
         atlas.directory_byte_size, 0},
        {sizeof(ps_gpu_buffer_binding_v9), 2, token, 0, 8, 1}};
    const uint64_t constants[] = {
        atlas.slot_count,           atlas.shape[0], atlas.tile_shape[0],
        atlas.payload_sample_bytes, coordinates[0], coordinates[1]};
    const ps_gpu_dispatch_v9 work =
        command(value_source, sizeof(value_source) - 1, "value", 5, buffers,
                constants, 1);
    if (!host->gpu_execute(host->context, &work, 1))
      return PS_OPERATION_RESULT_FAILURE_V9;
    uint32_t missing = 0;
    memcpy(&missing, scratch + 4, 4);
    if (missing)
      return PS_OPERATION_RESULT_FAILURE_V9;
    memcpy(&sum, scratch, 4);
  } else {
    for (uint32_t i = 0; i < 2; ++i) {
      float value = 0;
      if (!host->read(host->context, 0, coordinates + i, 1, &value, 4))
        return PS_OPERATION_RESULT_FAILURE_V9;
      sum += value;
    }
  }
  uint64_t token = 0;
  uint8_t* output = host->allocate_output(host->context, q->outputs, &token);
  if (!output)
    return PS_OPERATION_RESULT_FAILURE_V9;
  memcpy(output, &sum, 4);
  return host->publish_output(host->context, token)
             ? PS_OPERATION_RESULT_SUCCESS_V9
             : PS_OPERATION_RESULT_FAILURE_V9;
}
static int poll(const ps_dependency_query_v9* q, void* bytes,
                const ps_dependency_services_v9* host, void* user) {
  (void)user;
  struct State* s = bytes;
  if (s->stage++ == 0)
    return needs(q, host, 1, s->coordinate, 1);
  if (s->stage == 2) {
    if (q->backend == 2) {
      host->discover(host->context, s->capacity, 2, discover, s);
      if (s->mode == 1) {
        uint64_t token = 0;
        uint8_t* output =
            host->allocate_output(host->context, q->outputs, &token);
        if (output) {
          memset(output, 0, 4);
          host->publish_output(host->context, token);
        }
        return PS_OPERATION_RESULT_SUCCESS_V9;
      }
      return PS_DEPENDENCY_NEED_V9;
    }
    int64_t base = 0;
    if (!host->read(host->context, 1, &s->coordinate, 1, &base, 8))
      return PS_OPERATION_RESULT_FAILURE_V9;
    if (base < 0 || base >= 4096)
      return PS_DEPENDENCY_INVALID_ARGUMENT_V9;
    return needs(q, host, 0, (uint64_t)base, 2);
  }
  return finish(q, host);
}
static void destroy(void* bytes, void* user) {
  (void)bytes;
  (void)user;
}
static const ps_dependency_program_v9 program = {
    sizeof(program), 4,     0,    0,      sizeof(struct State),
    validate,        start, poll, destroy};
static const ps_operation_parameter_descriptor_v9 parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v9), "capacity", 8,
     PS_OPERATION_PARAMETER_INT64_V9, 1, 1, 1, 65536},
    {sizeof(ps_operation_parameter_descriptor_v9), "mode", 4,
     PS_OPERATION_PARAMETER_INT64_V9, 1, 1, 0, 1}};
static const ps_operation_port_constraint_v9 ports[] = {
    {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_VALUE_V9, 0, 0,
     NULL},
    {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_VALUE_V9, 0, 0,
     NULL}};
static const ps_operation_descriptor_v9 operations[] = {
    {sizeof(ps_operation_descriptor_v9),
     "example.c_discovery",
     sizeof("example.c_discovery") - 1,
     2,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
         PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     1,
     2,
     parameters,
     2,
     ports,
     0,
     0,
     8,
     0,
     &program,
     1,
     {{sizeof(ps_operation_output_descriptor_v9),
       "value",
       5,
       PS_OPERATION_ELEMENT_FLOAT32_V9,
       0,
       0,
       PS_OPERATION_SHAPE_PRESERVE_FIRST_V9,
       PS_OPERATION_REGION_DEPENDENCY_V9,
       0,
       {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_VALUE_V9, 0,
        0, NULL},
       0,
       0,
       0,
       0,
       0,
       0,
       0,
       0,
       0,
       NULL}}}};
static void release(const ps_operation_descriptor_v9* values, uint32_t count) {
  (void)values;
  (void)count;
}
static const ps_operation_plugin_api_v9 api = {sizeof(api), 1, operations,
                                               release};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_9;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v9*
ps_operation_plugin_get_api_v9(void) {
  return &api;
}
