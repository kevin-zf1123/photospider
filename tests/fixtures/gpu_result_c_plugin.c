#include <stdint.h>
#include <string.h>

#include "photospider/plugin/fragment_atlas_msl.h"
#include "photospider/plugin/result_operation_plugin_api.h"
#include "photospider/plugin/result_tile_helpers.h"

struct State {
  uint64_t old_token, coordinate, incoming;
  uint32_t stage, mode, box;
  float offset;
};
static const ps_result_tensor_spec_v3 state_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v3),
    .key = "state",
    .key_size = 5,
    .element_type = PS_RESULT_ELEMENT_FLOAT32_V3,
    .rank = 1,
    .shape = {2}};
static const ps_result_schema_v3 state_schema = {
    .struct_size = sizeof(ps_result_schema_v3),
    .id = "c.gpu.state",
    .id_size = 11,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V3,
    .tensors = &state_tensor,
    .tensor_count = 1};
static const char shader[] = PS_FRAGMENT_ATLAS_MSL_V11
    "kernel void sum65(device const uchar* data [[buffer(0)]],"
    " device const ulong* directory [[buffer(1)]], device uint* output "
    "[[buffer(2)]],"
    " constant ulong* c [[buffer(3)]]) { float sum=0; uint missing=0;"
    " ulong at[8]={0}, address=0; for(uint i=0;i<65;++i) {at[1]=c[6]+(i+1)*64;"
    " if(!ps_atlas_address(directory,c[0],c+1,c+3,2,at,c[5],4,address))"
    " {missing=1;continue;} uint bits=0;"
    " for(uint b=0;b<4;++b)bits|=uint(data[address+b])<<(b*8);"
    " sum+=as_type<float>(bits); } "
    "output[0]=as_type<uint>(sum);output[1]=missing;}";

static int start(void* user, void* bytes, const ps_result_query_v3* q,
                 const ps_result_services_v3* host) {
  (void)user;
  (void)host;
  struct State* state = bytes;
  if (!q->tiled || q->tile_extent != 1)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V3;
  state->mode = (uint32_t)q->parameters[0].int64_value;
  return 0;
}
static int need(const ps_result_services_v3* host, const ps_result_query_v3* q,
                const struct State* state, int data) {
  (void)q;
  uint64_t tiles[65];
  const uint32_t count = data ? 65 : 1;
  for (uint32_t i = 0; i < count; ++i)
    tiles[i] =
        data ? (uint64_t)state->offset + (i + 1) * 64 : state->coordinate;
  const ps_result_tile_need_v3 request = {sizeof(request), 0,    0, 0,
                                          tiles,           count};
  return host->need_tiles(host->context, &request) ? 1 : PS_RESULT_NEED_V3;
}
struct ComputeContext {
  const struct State* state;
  const ps_result_services_v3* outer;
};
static int compute(const ps_result_block_services_v3* host, uint64_t incoming,
                   uint64_t* outgoing, void* user) {
  const struct ComputeContext* context = user;
  const struct State* state = context->state;
  if (state->mode == 15) {
    ps_result_need_whole_v3(context->outer, 0);
    return 1;
  }
  float values[2] = {0};
  if (host->read_state(host->context, incoming, (uint8_t*)values,
                       sizeof(values)))
    return 1;
  if (state->mode == 16) {
    ps_result_tensor_spec_v3 tensor = state_tensor;
    tensor.shape[0] = 1;
    ps_result_schema_v3 schema = state_schema;
    schema.tensors = &tensor;
    return host->create_state(host->context, &schema, (const uint8_t*)values, 4,
                              outgoing);
  }
  ps_result_native_atlas_v3 atlas = {0}, again = {0};
  atlas.struct_size = again.struct_size = sizeof(atlas);
  if (host->acquire_native_atlas(host->context, 0, 0, &atlas) ||
      host->acquire_native_atlas(host->context, 0, 0, &again) ||
      atlas.payload_token != again.payload_token ||
      atlas.directory_token != again.directory_token)
    return 1;
  if (state->mode == 17) {
    if (host->gpu->release(host->gpu->context, atlas.payload_token) ||
        host->acquire_native_atlas(host->context, 0, 0, &again) ||
        again.payload_token == atlas.payload_token)
      return 1;
    atlas = again;
  }
  uint8_t* scratch = NULL;
  uint64_t output = 0;
  if (host->allocate_scratch(host->context, 8, &scratch) ||
      host->gpu->buffer(host->gpu->context, scratch, 8, 1, &output))
    return 1;
  ps_gpu_buffer_binding_v1 buffers[] = {
      {sizeof(ps_gpu_buffer_binding_v1), 0, atlas.payload_token, 0,
       atlas.payload_byte_size, 0},
      {sizeof(ps_gpu_buffer_binding_v1), 1, atlas.directory_token, 0,
       atlas.directory_byte_size, 0},
      {sizeof(ps_gpu_buffer_binding_v1), 2, output, 0, 8, 1}};
  if (state->mode == 11)
    buffers[0].writable = 1;
  const uint64_t constants[] = {atlas.slot_count,
                                atlas.shape[0],
                                atlas.shape[1],
                                atlas.tile_shape[0],
                                atlas.tile_shape[1],
                                atlas.payload_sample_bytes,
                                (uint64_t)values[0] + (state->mode == 7)};
  ps_gpu_dispatch_v1 command = {0};
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
  if (host->gpu->execute(host->gpu->context, &command, 1))
    return 1;
  uint32_t missing = 0;
  memcpy(&missing, scratch + 4, 4);
  if (missing)
    return 1;
  memcpy(values + 1, scratch, 4);
  if (host->create_state(host->context, &state_schema, (const uint8_t*)values,
                         sizeof(values), outgoing) ||
      host->gpu->release(host->gpu->context, output) ||
      host->release_scratch(host->context, scratch))
    return 1;
  return 0;
}
static int invalid_token(const ps_result_services_v3* host, uint64_t token) {
  const ps_gpu_buffer_binding_v1 binding = {sizeof(binding), 0, token, 0, 4, 0};
  ps_gpu_dispatch_v1 command = {0};
  command.struct_size = sizeof(command);
  command.buffers = &binding;
  command.buffer_count = 1;
  host->gpu->execute(host->gpu->context, &command, 1);
  return 1;
}
/* Direct mock-host probes isolate bridge rejection from backend validation.
 * 18: invalid writable; 19/20: forged execute/release token;
 * 21/22: expired execute/release token; 23: valid same-poll translation. */
static int bridge_probe(struct State* state, const ps_result_query_v3* q,
                        const ps_result_services_v3* host) {
  if (state->mode == 18) {
    uint8_t* scratch = NULL;
    uint64_t token = 0;
    if (host->allocate_scratch(host->context, 4, &scratch))
      return 1;
    host->gpu->buffer(host->gpu->context, scratch, 4, 2, &token);
    return 1;
  }
  if (state->mode >= 21 && !state->stage) {
    uint8_t* scratch = NULL;
    if (host->allocate_scratch(host->context, 4, &scratch) ||
        host->gpu->buffer(host->gpu->context, scratch, 4, 1, &state->old_token))
      return 1;
    state->stage = 1;
    if (state->mode != 23)
      return need(host, q, state, 0);
  }
  const uint64_t token = state->mode < 21 ? UINT64_MAX : state->old_token;
  if (state->mode == 20 || state->mode == 22) {
    host->gpu->release(host->gpu->context, token);
    return 1;
  }
  const ps_gpu_buffer_binding_v1 binding = {sizeof(binding), 0, token, 0, 4, 0};
  const char source[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void noop(device const uint* in [[buffer(0)]]) {}";
  ps_gpu_dispatch_v1 command = {0};
  command.struct_size = sizeof(command);
  command.source = source;
  command.source_size = sizeof(source) - 1;
  command.entry = "noop";
  command.entry_size = 4;
  command.buffers = &binding;
  command.buffer_count = 1;
  command.grid[0] = command.grid[1] = command.grid[2] = 1;
  if (host->gpu->execute(host->gpu->context, &command, 1))
    return 1;
  if (state->mode != 23 || host->gpu->release(host->gpu->context, token))
    return 1;
  return need(host, q, state, 0);
}
static int poll(void* user, void* bytes, const ps_result_query_v3* q,
                const ps_result_services_v3* host) {
  (void)user;
  ps_result_region_v3 requested_region;
  if (ps_result_output_region_v3(q, 0, &requested_region))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V3;
  const uint32_t requested_count =
      q->requested.whole || q->requested.tile_count ? 1 : 0;
  struct State* state = bytes;
  if (state->mode >= 18)
    return bridge_probe(state, q, host);
  if (!state->stage) {
    if (host->begin_result(host->context))
      return 1;
    if (!requested_count)
      return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V3;
    state->coordinate = requested_region.offset[1];
    state->stage = 1;
    return need(host, q, state, 0);
  }
  if (state->stage == 1) {
    const uint64_t coordinate[] = {0, state->coordinate};
    if (host->read_tensor(host->context, 0, 0, coordinate, 2, &state->offset,
                          4) ||
        !(state->offset >= 0 && state->offset < 64) ||
        state->offset != (float)(uint32_t)state->offset)
      return 1;
    if (state->mode == 1) {
      ps_result_native_atlas_v3 atlas = {0};
      atlas.struct_size = sizeof(atlas);
      if (host->acquire_native_atlas(host->context, 0, 0, &atlas))
        return 1;
      state->old_token = atlas.payload_token;
    }
    state->stage = 2;
    if (q->backend == 2) {
      const float values[] = {state->offset, 0};
      if (host->create_block_state(host->context, &state_schema,
                                   (const uint8_t*)values, sizeof(values),
                                   &state->incoming))
        return 1;
    }
    return need(host, q, state, 1);
  }
  if (state->mode == 1) {
    ps_result_native_atlas_v3 atlas = {0};
    atlas.struct_size = sizeof(atlas);
    if (host->acquire_native_atlas(host->context, 0, 0, &atlas))
      return 1;
    return invalid_token(host, state->old_token);
  }
  if (state->mode == 2 || state->mode == 8) {
    ps_result_native_atlas_v3 atlas = {0};
    atlas.struct_size = sizeof(atlas);
    atlas.reserved = state->mode == 2;
    host->acquire_native_atlas(host->context, 0, 0, &atlas);
    return 1;
  }
  if (state->mode == 3) {
    uint64_t token = 0;
    uint8_t* valid = NULL;
    if (host->allocate_scratch(host->context, 4, &valid))
      return 1;
    host->gpu->buffer(host->gpu->context, valid, 4, 2, &token);
    return 1;
  }
  if (state->mode == 4) {
    host->gpu->execute(host->gpu->context, NULL, 0);
    return 1;
  }
  if (state->mode == 9 || state->mode == 10) {
    ps_gpu_dispatch_v1 command = {0};
    ps_gpu_buffer_binding_v1 buffer = {0};
    command.struct_size = sizeof(command);
    command.buffers = &buffer;
    command.buffer_count = state->mode == 10 ? 32 : 1;
    host->gpu->execute(host->gpu->context, &command, state->mode == 9 ? 33 : 1);
    return 1;
  }
  if (state->mode == 5)
    return invalid_token(host, UINT64_MAX);
  if (state->mode == 12) {
    const ps_result_region_v3 box = {.struct_size = sizeof(box),
                                     .rank = 1,
                                     .extent = {1}};
    host->need_tiles(host->context,
                     &(ps_result_tile_need_v3){sizeof(ps_result_tile_need_v3),
                                               0, 0, 0, box.offset, 65537});
    return 1;
  }
  if (state->mode == 13) {
    uint8_t bytes[8];
    host->read_block_state(host->context, UINT64_MAX, bytes, sizeof(bytes));
    return 1;
  }
  if (state->mode == 14) {
    if (host->release_block_state(host->context, state->incoming))
      return 1;
    host->release_block_state(host->context, state->incoming);
    return 1;
  }
  float values[] = {state->offset, 0};
  if (q->backend == 2) {
    uint64_t outgoing = 0;
    const struct ComputeContext compute_context = {state, host};
    if (host->block(host->context, 1, 0, 65, 1, state->incoming, compute,
                    (void*)&compute_context, &outgoing) ||
        host->read_block_state(host->context, outgoing, (uint8_t*)values,
                               sizeof(values)) ||
        host->release_block_state(host->context, state->incoming) ||
        host->release_block_state(host->context, outgoing))
      return 1;
  } else {
    for (uint64_t i = 0; i < 65; ++i) {
      const uint64_t at[] = {0, (uint64_t)state->offset + (i + 1) * 64};
      float sample = 0;
      if (host->read_tensor(host->context, 0, 0, at, 2, &sample, 4))
        return 1;
      values[1] += sample;
    }
  }
  uint8_t* destination = NULL;
  if (host->allocate_scratch(host->context, 4, &destination))
    return 1;
  memcpy(destination, values + 1, 4);
  uint64_t token = 0;
  if (state->mode == 6 &&
      host->gpu->buffer(host->gpu->context, destination, 4, 1, &token))
    return 1;
  ps_result_region_v3 region = {.struct_size = sizeof(region),
                                .rank = 2,
                                .offset = {0, state->coordinate},
                                .extent = {1, 1}};
  if (host->publish_tensor_buffer(host->context, 0, &region, destination, 4))
    return 1;
  if (state->mode == 6) {
    const char source[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void write(device uint* out [[buffer(0)]]) {out[0]=0;}";
    const ps_gpu_buffer_binding_v1 buffer = {sizeof(buffer), 0, token, 0, 4, 1};
    ps_gpu_dispatch_v1 command = {0};
    command.struct_size = sizeof(command);
    command.source = source;
    command.source_size = sizeof(source) - 1;
    command.entry = "write";
    command.entry_size = 5;
    command.buffers = &buffer;
    command.buffer_count = 1;
    command.grid[0] = command.grid[1] = command.grid[2] = 1;
    host->gpu->execute(host->gpu->context, &command, 1);
    return 1;
  }
  ++state->coordinate;
  if (state->coordinate ==
      requested_region.offset[1] + requested_region.extent[1]) {
    if (++state->box < requested_count)
      state->coordinate = requested_region.offset[1];
  }
  if (state->box == requested_count)
    return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V3;
  state->stage = 1;
  return need(host, q, state, 0);
}
static void destroy(void* user, void* bytes) {
  (void)user;
  (void)bytes;
}
static void release(void* context) {
  (void)context;
}
static const ps_result_tensor_spec_v3 tensor = {
    .struct_size = sizeof(tensor),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT32_V3,
    .rank = 2,
    .shape = {1, 8192},
    .spatial = 1,
    .height_axis = 0,
    .width_axis = 1,
    .channel_axis = PS_RESULT_NO_CHANNEL_V3};
static const ps_result_schema_v3 tensor_schema = {
    .struct_size = sizeof(tensor_schema),
    .id = "c.gpu.samples",
    .id_size = 13,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V3,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_port_v3 input = {
    .struct_size = sizeof(input),
    .kind = PS_RESULT_OBJECT_V3,
    .element_type = PS_RESULT_ELEMENT_FLOAT32_V3,
    .rank = 2,
    .tensor_key = "samples",
    .tensor_key_size = 7};
static const ps_result_output_v3 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v3),
             .kind = PS_RESULT_OBJECT_V3,
             .schema = &tensor_schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_REGIONAL_V3};
static const ps_result_parameter_descriptor_v3 parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v3), "mode", 4,
     PS_RESULT_PARAMETER_INT64_V3, 1, 1, 0, 23}};
static int resolve(void* user, const ps_result_port_v3* inputs, uint32_t count,
                   const ps_result_parameter_value_v3* values,
                   uint32_t value_count, const ps_result_port_v3* outputs,
                   uint32_t output_count,
                   const ps_result_metadata_sink_v3* sink) {
  (void)user;
  (void)values;
  (void)value_count;
  (void)outputs;
  if (count != 1 || output_count != 1 || !inputs[0].schema ||
      inputs[0].schema->tensor_count != 1 || inputs[0].schema->field_count)
    return 5;
  const ps_result_tensor_spec_v3* source = inputs[0].schema->tensors;
  if (source->rank != 2 || source->batch_rank || source->shape[0] != 1 ||
      source->shape[1] < 4225 ||
      source->element_type != PS_RESULT_ELEMENT_FLOAT32_V3)
    return 5;
  ps_result_tensor_spec_v3 result_tensor = tensor;
  result_tensor.shape[1] = source->shape[1];
  ps_result_schema_v3 result_schema = tensor_schema;
  result_schema.tensors = &result_tensor;
  ps_result_port_v3 result = output.port;
  result.schema = &result_schema;
  return sink->set_output(sink->context, 0, &result);
}
static const ps_result_operation_v3 operation = {
    .struct_size = sizeof(operation),
    .key = "example.c_native_sum",
    .key_size = 20,
    .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_GPU_V3 |
             PS_RESULT_FLAG_DETERMINISTIC_V3 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
    .inputs = &input,
    .input_count = 1,
    .outputs = &output,
    .output_count = 1,
    .parameters = parameters,
    .parameter_count = 1,
    .state_bytes = sizeof(struct State),
    .workspace_bytes = 24,
    .maximum_stages = 65536,
    .resolve_metadata = resolve,
    .start = start,
    .poll = poll,
    .destroy = destroy};
static const ps_result_operation_plugin_api_v3 api = {
    sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_3, &operation, 1, NULL,
    release};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v3*
ps_result_operation_plugin_get_api_v3(void) {
  return &api;
}
