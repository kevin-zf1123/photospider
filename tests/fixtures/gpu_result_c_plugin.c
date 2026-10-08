#include <stdint.h>
#include <string.h>

#include "photospider/plugin/fragment_atlas_msl.h"
#include "photospider/plugin/result_operation_plugin_api.h"

struct State {
  uint64_t old_token, coordinate, incoming;
  uint32_t stage, mode, box;
  float offset;
};
static const ps_result_tensor_spec_v2 state_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v2),
    .key = "state",
    .key_size = 5,
    .element_type = PS_RESULT_ELEMENT_FLOAT32_V2,
    .rank = 1,
    .shape = {2}};
static const ps_result_schema_v2 state_schema = {
    .struct_size = sizeof(ps_result_schema_v2),
    .id = "c.gpu.state",
    .id_size = 11,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &state_tensor,
    .tensor_count = 1};
static const char shader[] = PS_FRAGMENT_ATLAS_MSL_V11
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

static int start(void* user, void* bytes, const ps_result_query_v2* q,
                 const ps_result_services_v2* host) {
  (void)user;
  (void)host;
  struct State* state = bytes;
  state->mode = (uint32_t)q->parameters[0].int64_value;
  return 0;
}
static int need(const ps_result_services_v2* host, const struct State* state,
                int data) {
  ps_result_region_v2 boxes[65] = {{0}};
  const uint32_t count = data ? 65 : 1;
  for (uint32_t i = 0; i < count; ++i) {
    boxes[i].struct_size = sizeof(boxes[i]);
    boxes[i].rank = 1;
    boxes[i].offset[0] =
        data ? (uint64_t)state->offset + (i + 1) * 64 : state->coordinate;
    boxes[i].extent[0] = 1;
  }
  return host->need_tensor(host->context, 0, 0, data ? 9 : 10, boxes, count)
             ? 1
             : PS_RESULT_NEED_V2;
}
struct ComputeContext {
  const struct State* state;
  const ps_result_services_v2* outer;
};
static int compute(const ps_result_block_services_v2* host, uint64_t incoming,
                   uint64_t* outgoing, void* user) {
  const struct ComputeContext* context = user;
  const struct State* state = context->state;
  if (state->mode == 15) {
    context->outer->need_tensor(context->outer->context, 0, 0, 1, NULL, 0);
    return 1;
  }
  float values[2] = {0};
  if (host->read_state(host->context, incoming, (uint8_t*)values,
                       sizeof(values)))
    return 1;
  if (state->mode == 16) {
    ps_result_tensor_spec_v2 tensor = state_tensor;
    tensor.shape[0] = 1;
    ps_result_schema_v2 schema = state_schema;
    schema.tensors = &tensor;
    return host->create_state(host->context, &schema, (const uint8_t*)values, 4,
                              outgoing);
  }
  ps_result_native_atlas_v2 atlas = {0}, again = {0};
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
  const uint64_t constants[] = {atlas.slot_count, atlas.shape[0],
                                atlas.tile_shape[0], atlas.payload_sample_bytes,
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
static int invalid_token(const ps_result_services_v2* host, uint64_t token) {
  const ps_gpu_buffer_binding_v1 binding = {sizeof(binding), 0, token, 0, 4, 0};
  ps_gpu_dispatch_v1 command = {0};
  command.struct_size = sizeof(command);
  command.buffers = &binding;
  command.buffer_count = 1;
  host->gpu->execute(host->gpu->context, &command, 1);
  return 1;
}
static int poll(void* user, void* bytes, const ps_result_query_v2* q,
                const ps_result_services_v2* host) {
  (void)user;
  ps_result_query_v2 whole_query;
  ps_result_region_v2 whole_region = {0};
  if (q->requested_kind == 0) {
    whole_query = *q;
    whole_region.struct_size = sizeof(whole_region);
    whole_region.rank = 1;
    whole_region.extent[0] = q->output->port.schema->tensors[0].shape[0];
    whole_query.requested = &whole_region;
    whole_query.requested_count = 1;
    q = &whole_query;
  }
  struct State* state = bytes;
  if (!state->stage) {
    const ps_result_relation_row_v2 descriptor = {
        0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
    if (host->begin_result(host->context) ||
        host->bind_descriptor(host->context, &descriptor, 1,
                              PS_RESULT_EXACT_V2))
      return 1;
    if (!q->requested_count)
      return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V2;
    state->coordinate = q->requested[0].offset[0];
    state->stage = 1;
    return need(host, state, 0);
  }
  if (state->stage == 1) {
    if (host->read_tensor(host->context, 0, 0, &state->coordinate, 1,
                          &state->offset, 4) ||
        !(state->offset >= 0 && state->offset < 64) ||
        state->offset != (float)(uint32_t)state->offset)
      return 1;
    if (state->mode == 1) {
      ps_result_native_atlas_v2 atlas = {0};
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
    return need(host, state, 1);
  }
  if (state->mode == 1) {
    ps_result_native_atlas_v2 atlas = {0};
    atlas.struct_size = sizeof(atlas);
    if (host->acquire_native_atlas(host->context, 0, 0, &atlas))
      return 1;
    return invalid_token(host, state->old_token);
  }
  if (state->mode == 2 || state->mode == 8) {
    ps_result_native_atlas_v2 atlas = {0};
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
    const ps_result_region_v2 box = {.struct_size = sizeof(box),
                                     .rank = 1,
                                     .extent = {1}};
    host->need_tensor(host->context, 0, 0, 1, &box, 65537);
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
      const uint64_t at[] = {(uint64_t)state->offset + (i + 1) * 64};
      float sample = 0;
      if (host->read_tensor(host->context, 0, 0, at, 1, &sample, 4))
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
  ps_result_region_v2 region = {.struct_size = sizeof(region),
                                .rank = 1,
                                .offset = {state->coordinate},
                                .extent = {1}};
  ps_result_relation_row_v2 rows[66] = {{0}};
  for (uint32_t i = 0; i < 66; ++i) {
    rows[i].output = state->coordinate;
    rows[i].roles = i == 65 ? 2 : 1;
    rows[i].target = PS_RESULT_TARGET_TENSOR_V2;
    rows[i].first =
        i == 65 ? state->coordinate : (uint64_t)state->offset + (i + 1) * 64;
    rows[i].count = 1;
  }
  if (host->publish_tensor_buffer(host->context, 0, &region, destination, 4,
                                  rows, 66, PS_RESULT_EXACT_V2,
                                  PS_RESULT_FINAL_V2))
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
      q->requested[state->box].offset[0] + q->requested[state->box].extent[0]) {
    if (++state->box < q->requested_count)
      state->coordinate = q->requested[state->box].offset[0];
  }
  if (state->box == q->requested_count)
    return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V2;
  state->stage = 1;
  return need(host, state, 0);
}
static void destroy(void* user, void* bytes) {
  (void)user;
  (void)bytes;
}
static void release(void* context) {
  (void)context;
}
static const ps_result_tensor_spec_v2 tensor = {
    .struct_size = sizeof(tensor),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT32_V2,
    .rank = 1,
    .shape = {8192}};
static const ps_result_schema_v2 tensor_schema = {
    .struct_size = sizeof(tensor_schema),
    .id = "c.gpu.samples",
    .id_size = 13,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_port_v2 input = {
    .struct_size = sizeof(input),
    .kind = PS_RESULT_OBJECT_V2,
    .element_type = PS_RESULT_ELEMENT_FLOAT32_V2,
    .rank = 1,
    .tensor_key = "samples",
    .tensor_key_size = 7};
static const ps_result_output_v2 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &tensor_schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_REGIONAL_V2};
static const ps_result_parameter_descriptor_v2 parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v2), "mode", 4,
     PS_RESULT_PARAMETER_INT64_V2, 1, 1, 0, 17}};
static int resolve(void* user, const ps_result_port_v2* inputs, uint32_t count,
                   const ps_result_parameter_value_v2* values,
                   uint32_t value_count, const ps_result_port_v2* outputs,
                   uint32_t output_count,
                   const ps_result_metadata_sink_v2* sink) {
  (void)user;
  (void)values;
  (void)value_count;
  (void)outputs;
  if (count != 1 || output_count != 1 || !inputs[0].schema ||
      inputs[0].schema->tensor_count != 1 || inputs[0].schema->field_count)
    return 5;
  const ps_result_tensor_spec_v2* source = inputs[0].schema->tensors;
  if (source->rank != 1 || source->batch_rank || source->shape[0] < 4225 ||
      source->element_type != PS_RESULT_ELEMENT_FLOAT32_V2)
    return 5;
  ps_result_tensor_spec_v2 result_tensor = tensor;
  result_tensor.shape[0] = source->shape[0];
  ps_result_schema_v2 result_schema = tensor_schema;
  result_schema.tensors = &result_tensor;
  ps_result_port_v2 result = output.port;
  result.schema = &result_schema;
  return sink->set_output(sink->context, 0, &result);
}
static const ps_result_operation_v2 operation = {
    .struct_size = sizeof(operation),
    .key = "example.c_native_sum",
    .key_size = 20,
    .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_GPU_V2 |
             PS_RESULT_FLAG_DETERMINISTIC_V2 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
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
static const ps_result_operation_plugin_api_v2 api = {
    sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_2, &operation, 1, NULL,
    release};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}
