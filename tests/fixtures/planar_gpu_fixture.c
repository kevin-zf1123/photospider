#include <string.h>

#include "native_scale_spirv.h"
#include "photospider/plugin/planar_operation_plugin_api.h"

static int infer(void* user, const ps_planar_metadata_v3* in, uint32_t n,
                 const ps_operation_parameter_value_v11* p, uint32_t pc,
                 ps_planar_metadata_v3* out, char* d, size_t dc) {
  (void)user;
  (void)p;
  (void)pc;
  (void)d;
  (void)dc;
  if (n != 1)
    return 6;
  *out = in[0];
  return 0;
}
static int execute(void* user, const ps_planar_metadata_v3* in, uint32_t n,
                   const ps_operation_parameter_value_v11* p, uint32_t pc,
                   const ps_planar_metadata_v3* out,
                   const ps_planar_services_v3* s, char* d, size_t dc) {
  (void)user;
  (void)in;
  (void)n;
  (void)pc;
  (void)out;
  (void)d;
  (void)dc;
  if (s->struct_size != sizeof(*s) || s->backend != 2 || s->cpu_parallel ||
      !s->gpu || s->gpu->struct_size != sizeof(*s->gpu) || !s->consume_work)
    return 6;
  const uint64_t at[3] = {0, 0, 0};
  uint64_t count = 0, a = 0, b = 0;
  const uint8_t* source = NULL;
  uint8_t* result = NULL;
  if (!s->read_row(s->context, 0, at, 3, &source, &count) || count != 1 ||
      !s->write_row(s->context, at, 3, &result, &count))
    return 1;
  memcpy(result, source, 4);
  if (p[0].int64_value >= 5 && p[0].int64_value <= 7)
    return p[0].int64_value == 7 ? 3 : (int)p[0].int64_value;
  if (p[0].int64_value == 1) {
    (void)s->gpu->buffer(s->gpu->context, source, 4, 0, &a);
    return 0; /* Host row is not native; ignored failure must reject commit. */
  }
  if (p[0].int64_value == 2)
    return 0; /* GPU success without dispatch. */
  if (p[0].int64_value == 3) {
    (void)s->consume_work(s->context, UINT64_MAX);
    return 0;
  }
  if (!s->consume_work(s->context, 65536))
    return 4;
  /* Each 16385-byte request is charged at the device backing capacity. */
  uint8_t* input = s->allocate_scratch(s->context, 16385);
  uint8_t* output = s->allocate_scratch(s->context, 16385);
  if (!input || !output)
    return 4;
  memcpy(input, source, 4);
  if (s->gpu->buffer(s->gpu->context, input, 16385, 0, &a) ||
      s->gpu->buffer(s->gpu->context, output, 16385, 1, &b))
    return 1;
  if (p[0].int64_value == 8) {
    /* Tokens retain the requested quota after scratch pointers are released. */
    (void)s->release_scratch(s->context, input);
    (void)s->release_scratch(s->context, output);
    (void)s->allocate_scratch(s->context, 1);
    return 0;
  }
  const char shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void scale(device const float* a [[buffer(0)]],"
      "device float* b [[buffer(1)]], uint i [[thread_position_in_grid]])"
      "{if(i==0)b[0]=a[0]*.5f;}";
  const ps_gpu_buffer_binding_v11 bindings[] = {
      {sizeof(ps_gpu_buffer_binding_v11), 0, a, 0, 4, 0},
      {sizeof(ps_gpu_buffer_binding_v11), 1, b, 0, 4, 1}};
  ps_gpu_dispatch_v11 command = {0};
  command.struct_size = sizeof(command);
  uint32_t padded_words[16384];
  char* padded = (char*)padded_words;
  command.source = shader;
  command.source_size = sizeof(shader) - 1;
  if (p[0].int64_value == 10) {
    memset(padded, ' ', sizeof(padded_words));
    memcpy(padded, shader, sizeof(shader) - 1);
    command.source = padded;
    command.source_size = sizeof(padded_words);
  }
  if (s->gpu->backend == PS_GPU_BACKEND_VULKAN_V11) {
    command.source = (const char*)kNativeScaleSpirv;
    command.source_size = sizeof(kNativeScaleSpirv);
    command.code_format = PS_GPU_CODE_SPIRV_V11;
    if (p[0].int64_value == 10) {
      /* Valid OpNop instructions in the first basic block enlarge only the
         retained source key, while preserving native scale computation. */
      const uint32_t words = sizeof(kNativeScaleSpirv) / 4;
      uint32_t insert = 5;
      while (insert < words && (kNativeScaleSpirv[insert] & 65535) != 248)
        insert += kNativeScaleSpirv[insert] >> 16;
      if (insert >= words)
        return 6;
      insert += kNativeScaleSpirv[insert] >> 16;
      const uint32_t padding = 16384 - words;
      memcpy(padded_words, kNativeScaleSpirv, insert * 4);
      for (uint32_t i = 0; i < padding; ++i)
        padded_words[insert + i] = 0x00010000;
      memcpy(padded_words + insert + padding, kNativeScaleSpirv + insert,
             (words - insert) * 4);
      command.source = padded;
      command.source_size = sizeof(padded_words);
    }
  }
  command.entry = "scale";
  command.entry_size = 5;
  command.buffers = bindings;
  command.buffer_count = 2;
  command.grid[0] = command.grid[1] = command.grid[2] = 1;
  if (p[0].int64_value == 4)
    command.group[0] = 4;
  const int code = s->gpu->execute(s->gpu->context, &command, 1);
  if (!code)
    memcpy(result, output, 4);
  (void)s->gpu->release(s->gpu->context, a);
  (void)s->gpu->release(s->gpu->context, b);
  (void)s->release_scratch(s->context, input);
  (void)s->release_scratch(s->context, output);
  if (p[0].int64_value == 9) {
    uint8_t* reused = s->allocate_scratch(s->context, 32770);
    if (!reused)
      return 4;
    (void)s->release_scratch(s->context, reused);
  }
  // Deliberately ignore native failure; host remains authoritative.
  return 0;
}
static int unused(void* u, const ps_operation_value_view_v11* v, uint32_t n,
                  const ps_operation_parameter_value_v11* p, uint32_t pc,
                  uint32_t b, ps_operation_cancelled_v11 c, void* cc,
                  const ps_operation_output_sink_v11* s, char* d, size_t dc) {
  (void)u;
  (void)v;
  (void)n;
  (void)p;
  (void)pc;
  (void)b;
  (void)c;
  (void)cc;
  (void)s;
  (void)d;
  (void)dc;
  return 1;
}
static const ps_operation_parameter_descriptor_v11 parameter = {
    sizeof(parameter),
    "case",
    4,
    PS_OPERATION_PARAMETER_INT64_V11,
    1,
    1,
    0,
    10};
static const ps_operation_port_constraint_v11 input = {
    sizeof(input), PS_OPERATION_PORT_VALUE_V11, 0, 0, NULL};
static const ps_operation_descriptor_v11 operation = {
    .struct_size = sizeof(operation),
    .key = "test.planar_gpu",
    .key_size = 15,
    .input_count = 1,
    .flags = PS_OPERATION_FLAG_GPU | PS_OPERATION_FLAG_DETERMINISTIC |
             PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
    .parameter_count = 1,
    .parameters = &parameter,
    .input_schema_count = 1,
    .input_schema = &input,
    .execute = unused,
    .workspace_bytes = 32770,
    .output_count = 1,
    .outputs = {{.struct_size = sizeof(ps_operation_output_descriptor_v11),
                 .key = "values",
                 .key_size = 6,
                 .output_element_type = 4,
                 .shape_rule = PS_OPERATION_SHAPE_PRESERVE_FIRST_V11,
                 .region_rule = PS_OPERATION_REGION_WHOLE_V11,
                 .output_schema = {sizeof(ps_operation_port_constraint_v11),
                                   PS_OPERATION_PORT_VALUE_V11, 0, 0, NULL}}}};
static void destroy(const ps_operation_descriptor_v11* p, uint32_t n) {
  (void)p;
  (void)n;
}
static const ps_operation_plugin_api_v11 api = {sizeof(api), 1, &operation,
                                                destroy};
static const ps_planar_operation_v3 entry = {sizeof(entry), infer, execute};
static const ps_planar_operation_plugin_api_v3 extension = {
    sizeof(extension), PS_PLANAR_OPERATION_ABI_VERSION_3, 1, &entry};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_11;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v11*
ps_operation_plugin_get_api_v11(void) {
  return &api;
}
PS_OPERATION_EXPORT const ps_planar_operation_plugin_api_v3*
ps_operation_plugin_get_planar_api_v3(void) {
  return &extension;
}
