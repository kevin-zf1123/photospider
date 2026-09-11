#include <stdint.h>
#include <string.h>

#include "photospider/plugin/dependency_plugin_api.h"
#include "photospider/plugin/fragment_atlas_msl.h"

struct State {
  uint64_t old_token;
  uint32_t stage, mode;
  float offset;
};
static const char shader[] = PS_FRAGMENT_ATLAS_MSL_V8
    "kernel void sum65(device const uchar* data [[buffer(0)]],"
    " device const ulong* directory [[buffer(1)]], device uint* output "
    "[[buffer(2)]],"
    " constant ulong* c [[buffer(3)]]) { float sum=0; uint missing=0;"
    " ulong at[8]={0}, address=0; for(uint i=0;i<65;++i) {at[0]=c[4]+(i+1)*64;"
    " if(!ps_atlas_address(directory,c[0],c+1,c+2,1,at,c[3],4,address))"
    " {missing=1;continue;} uint bits=0;"
    " for(uint b=0;b<4;++b)bits|=uint(data[address+b])<<(b*8);"
    " sum+=as_type<float>(bits); } "
    "output[0]=as_type<uint>(sum);output[1]=missing;}";
static int validate(const ps_dependency_metadata_query_v8* q, void* user) {
  (void)user;
  return q->input_count == 1 && q->inputs[0].rank == 1 &&
                 q->inputs[0].shape[0] >= 4225 &&
                 q->inputs[0].element_type == PS_OPERATION_ELEMENT_FLOAT32_V8
             ? PS_OPERATION_RESULT_SUCCESS_V8
             : PS_DEPENDENCY_INVALID_ARGUMENT_V8;
}
static int start(const ps_dependency_query_v8* q, void* bytes, uint64_t size,
                 void* user) {
  (void)user;
  if (size != sizeof(struct State))
    return PS_OPERATION_RESULT_FAILURE_V8;
  ((struct State*)bytes)->mode =
      (uint32_t)q->metadata.parameters[0].int64_value;
  return PS_OPERATION_RESULT_SUCCESS_V8;
}
static int need(const ps_dependency_query_v8* q,
                const ps_dependency_services_v8* host, float offset, int data) {
  ps_dependency_run_v8 runs[65] = {{0}};
  const uint32_t count = data ? 65 : 1;
  for (uint32_t i = 0; i < count; ++i) {
    runs[i].struct_size = sizeof(runs[i]);
    runs[i].rank = 1;
    runs[i].offsets[0] =
        data ? (uint64_t)offset + (i + 1) * 64 : q->outputs[0].offsets[0];
    runs[i].extents[0] = 1;
  }
  ps_dependency_association_v8 a = {0};
  a.struct_size = sizeof(a);
  a.output_rank = 1;
  a.output[0] = q->outputs[0].offsets[0];
  a.roles = data ? 1 : 2;
  a.run_count = count;
  a.runs = runs;
  return host->associate(host->context, &a) ? PS_DEPENDENCY_NEED_V8
                                            : PS_OPERATION_RESULT_FAILURE_V8;
}
static int compute(const ps_dependency_block_services_v8* host,
                   const uint8_t* incoming, uint8_t* outgoing, uint64_t size,
                   void* user) {
  const struct State* state = user;
  if (size != 8)
    return PS_OPERATION_RESULT_FAILURE_V8;
  float offset = 0;
  memcpy(&offset, incoming, 4);
  memcpy(outgoing, incoming, 8);
  ps_dependency_atlas_v8 atlas = {0}, again = {0};
  atlas.struct_size = again.struct_size = sizeof(atlas);
  if (!host->atlas(host->context, 0, &atlas) ||
      !host->atlas(host->context, 0, &again) ||
      atlas.payload_token != again.payload_token ||
      atlas.directory_token != again.directory_token)
    return PS_OPERATION_RESULT_FAILURE_V8;
  uint8_t* scratch = host->allocate_scratch(host->context, 8);
  uint64_t output = 0;
  if (!scratch || !host->gpu_buffer(host->context, scratch, 8, 1, &output))
    return PS_OPERATION_RESULT_FAILURE_V8;
  ps_gpu_buffer_binding_v8 buffers[] = {
      {sizeof(ps_gpu_buffer_binding_v8), 0, atlas.payload_token, 0,
       atlas.payload_byte_size, 0},
      {sizeof(ps_gpu_buffer_binding_v8), 1, atlas.directory_token, 0,
       atlas.directory_byte_size, 0},
      {sizeof(ps_gpu_buffer_binding_v8), 2, output, 0, 8, 1}};
  if (state->mode == 11)
    buffers[0].writable = 1;
  const uint64_t constants[] = {atlas.slot_count, atlas.shape[0],
                                atlas.tile_shape[0], atlas.payload_sample_bytes,
                                (uint64_t)offset + (state->mode == 7)};
  ps_gpu_dispatch_v8 command = {0};
  command.struct_size = sizeof(command);
  command.source = shader;
  command.source_size = sizeof(shader) - 1;
  command.entry = "sum65";
  command.entry_size = 5;
  command.buffers = buffers;
  command.buffer_count = 3;
  command.constants = constants;
  command.constant_size = sizeof(constants);
  command.constant_index = 3;
  command.grid[0] = command.grid[1] = command.grid[2] = 1;
  if (!host->gpu_execute(host->context, &command, 1))
    return PS_OPERATION_RESULT_FAILURE_V8;
  uint32_t missing = 0;
  memcpy(&missing, scratch + 4, 4);
  if (missing)
    return PS_OPERATION_RESULT_FAILURE_V8;
  memcpy(outgoing + 4, scratch, 4);
  return PS_OPERATION_RESULT_SUCCESS_V8;
}
static int invalid_token(const ps_dependency_services_v8* host,
                         uint64_t token) {
  const ps_gpu_buffer_binding_v8 binding = {sizeof(binding), 0, token, 0, 4, 0};
  ps_gpu_dispatch_v8 command = {0};
  command.struct_size = sizeof(command);
  command.buffers = &binding;
  command.buffer_count = 1;
  host->gpu_execute(host->context, &command, 1);
  return PS_OPERATION_RESULT_SUCCESS_V8; /* Intentionally ignored sticky error.
                                          */
}
static int poll(const ps_dependency_query_v8* q, void* bytes,
                const ps_dependency_services_v8* host, void* user) {
  (void)user;
  struct State* s = bytes;
  if (s->stage++ == 0)
    return need(q, host, 0, 0);
  if (s->stage == 2) {
    const uint64_t at[] = {q->outputs[0].offsets[0]};
    if (!host->read(host->context, 0, at, 1, &s->offset, 4) ||
        !(s->offset >= 0 && s->offset < 64) ||
        s->offset != (float)(uint32_t)s->offset)
      return PS_OPERATION_RESULT_FAILURE_V8;
    if (s->mode == 1) {
      ps_dependency_atlas_v8 atlas = {0};
      atlas.struct_size = sizeof(atlas);
      if (!host->atlas(host->context, 0, &atlas))
        return PS_OPERATION_RESULT_FAILURE_V8;
      s->old_token = atlas.payload_token;
    }
    return need(q, host, s->offset, 1);
  }
  if (s->mode == 1) {
    ps_dependency_atlas_v8 atlas = {0};
    atlas.struct_size = sizeof(atlas);
    host->atlas(host->context, 0, &atlas);
    return invalid_token(host, s->old_token);
  }
  if (s->mode == 2 || s->mode == 8) {
    ps_dependency_atlas_v8 atlas = {0};
    atlas.struct_size = sizeof(atlas);
    atlas.reserved = s->mode == 2;
    host->atlas(host->context, 0, &atlas);
    return PS_OPERATION_RESULT_SUCCESS_V8;
  }
  if (s->mode == 3) {
    uint64_t token = 0;
    uint8_t* valid = host->allocate_scratch(host->context, 4);
    if (!valid)
      return PS_OPERATION_RESULT_FAILURE_V8;
    host->gpu_buffer(host->context, valid, 4, 2, &token);
    return PS_OPERATION_RESULT_SUCCESS_V8;
  }
  if (s->mode == 4) {
    host->gpu_execute(host->context, NULL, 0);
    return PS_OPERATION_RESULT_SUCCESS_V8;
  }
  if (s->mode == 9 || s->mode == 10) {
    ps_gpu_dispatch_v8 command = {0};
    ps_gpu_buffer_binding_v8 buffer = {0};
    command.struct_size = sizeof(command);
    command.buffers = &buffer;
    command.buffer_count = s->mode == 10 ? 32 : 1;
    host->gpu_execute(host->context, &command, s->mode == 9 ? 33 : 1);
    return PS_OPERATION_RESULT_SUCCESS_V8;
  }
  if (s->mode == 5)
    return invalid_token(host, UINT64_MAX);
  float state[] = {s->offset, 0};
  if (q->backend == 2) {
    if (!host->block(host->context, 1, 0, 65, 1, (const uint8_t*)state, 8,
                     (uint8_t*)state, compute, s))
      return PS_OPERATION_RESULT_FAILURE_V8;
  } else {
    for (uint64_t i = 0; i < 65; ++i) {
      const uint64_t at[] = {(uint64_t)s->offset + (i + 1) * 64};
      float sample = 0;
      if (!host->read(host->context, 0, at, 1, &sample, 4))
        return PS_OPERATION_RESULT_FAILURE_V8;
      state[1] += sample;
    }
  }
  uint64_t output = 0;
  uint8_t* destination =
      host->allocate_output(host->context, q->outputs, &output);
  if (!destination)
    return PS_OPERATION_RESULT_FAILURE_V8;
  memcpy(destination, state + 1, 4);
  uint64_t token = 0;
  if (s->mode == 6)
    host->gpu_buffer(host->context, destination, 4, 1, &token);
  if (!host->publish_output(host->context, output))
    return PS_OPERATION_RESULT_FAILURE_V8;
  if (s->mode == 6) {
    const char source[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void write(device uint* out [[buffer(0)]]) {out[0]=0;}";
    const ps_gpu_buffer_binding_v8 buffer = {sizeof(buffer), 0, token, 0, 4, 1};
    ps_gpu_dispatch_v8 command = {0};
    command.struct_size = sizeof(command);
    command.source = source;
    command.source_size = sizeof(source) - 1;
    command.entry = "write";
    command.entry_size = 5;
    command.buffers = &buffer;
    command.buffer_count = 1;
    command.grid[0] = command.grid[1] = command.grid[2] = 1;
    host->gpu_execute(host->context, &command, 1);
  }
  return PS_OPERATION_RESULT_SUCCESS_V8;
}
static void destroy(void* bytes, void* user) {
  (void)bytes;
  (void)user;
}
static const ps_dependency_program_v8 program = {
    sizeof(program), 4,     0,    0,      sizeof(struct State),
    validate,        start, poll, destroy};
static const ps_operation_parameter_descriptor_v8 parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v8), "mode", 4,
     PS_OPERATION_PARAMETER_INT64_V8, 1, 1, 0, 11}};
static const ps_operation_port_constraint_v8 ports[] = {
    {sizeof(ps_operation_port_constraint_v8), PS_OPERATION_PORT_VALUE_V8, 0, 0,
     NULL}};
static const ps_operation_descriptor_v8 operations[] = {
    {.struct_size = sizeof(ps_operation_descriptor_v8),
     .key = "example.c_native_sum",
     .key_size = sizeof("example.c_native_sum") - 1,
     .input_count = 1,
     .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
              PS_OPERATION_FLAG_DETERMINISTIC |
              PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     .output_element_type = PS_OPERATION_ELEMENT_FLOAT32_V8,
     .shape_rule = PS_OPERATION_SHAPE_PRESERVE_FIRST_V8,
     .region_rule = PS_OPERATION_REGION_DEPENDENCY_V8,
     .cacheable = 1,
     .parameter_count = 1,
     .parameters = parameters,
     .input_schema_count = 1,
     .input_schema = ports,
     .output_schema = {sizeof(ps_operation_port_constraint_v8),
                       PS_OPERATION_PORT_VALUE_V8, 0, 0, NULL},
     .workspace_bytes = 24,
     .dependency_program = &program}};
static void release(const ps_operation_descriptor_v8* descriptors,
                    uint32_t count) {
  (void)descriptors;
  (void)count;
}
static const ps_operation_plugin_api_v8 api = {sizeof(api), 1, operations,
                                               release};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_8;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v8*
ps_operation_plugin_get_api_v8(void) {
  return &api;
}
