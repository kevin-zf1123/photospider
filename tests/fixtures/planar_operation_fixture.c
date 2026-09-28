#include <string.h>

#include "photospider/plugin/planar_operation_plugin_api.h"
static int infer(void* user, const ps_planar_metadata_v3* in, uint32_t count,
                 const ps_operation_parameter_value_v11* p, uint32_t pc,
                 ps_planar_metadata_v3* out, char* d, size_t dc) {
  (void)user;
  (void)count;
  (void)pc;
  (void)d;
  (void)dc;
  *out = in[0];
  out->facets = NULL;
  out->facet_count = 0;
  if (p[0].int64_value == 2)
    out->rank = 1;
  if (p[0].int64_value == 3) {
    static uint64_t unaligned[32];
    out->facets =
        (const ps_operation_facet_view_v11*)((const char*)unaligned + 1);
    out->facet_count = 1;
  }
  if (p[0].int64_value == 4) {
    static const uint8_t byte = 0;
    static const ps_operation_facet_view_v11 facet = {
        sizeof(ps_operation_facet_view_v11), "fixture", 7, 1, &byte, 65537};
    out->facets = &facet;
    out->facet_count = 1;
  }
  return 0;
}
struct RangeWork {
  const ps_planar_services_v3* services;
  const uint8_t* source;
  uint8_t* scratch;
  int64_t mode;
};
static int range_block(void* raw, uint64_t begin, uint64_t end, uint32_t slot) {
  struct RangeWork* work = (struct RangeWork*)raw;
  (void)begin;
  (void)end;
  (void)slot;
  if (work->mode == 10)
    return 4;
  if (work->mode == 11) {
    (void)work->services->allocate_scratch(work->services->context, 1);
    return 0; /* Ignored forbidden service must be sticky. */
  }
  memcpy(work->scratch, work->source, 4);
  return 0;
}
static int execute(void* user, const ps_planar_metadata_v3* in, uint32_t count,
                   const ps_operation_parameter_value_v11* p, uint32_t pc,
                   const ps_planar_metadata_v3* out,
                   const ps_planar_services_v3* s, char* d, size_t dc) {
  (void)user;
  (void)in;
  (void)count;
  (void)pc;
  (void)d;
  (void)dc;
  uint64_t at[3] = {0, 0, 0}, samples = 0;
  const uint8_t* source = NULL;
  uint8_t* target = NULL;
  if (out->rank != 3 || out->shape[0] != 1)
    return 1;
  if (p[0].int64_value == 5) {
    (void)s->allocate_scratch(s->context, 33);
    return 0;
  }
  if (p[0].int64_value == 6) {
    at[0] = 99;
    (void)s->write_row(s->context, at, 3, &target, &samples);
    return 0;
  }
  if (p[0].int64_value == 7)
    return 2;
  uint8_t* scratch = s->allocate_scratch(s->context, 32);
  if (!scratch)
    return 4;
  if (p[0].int64_value == 8) {
    (void)s->release_scratch(s->context, scratch);
    (void)s->release_scratch(s->context, scratch);
    return 0;
  }
  if (!s->read_row(s->context, 0, at, 3, &source, &samples))
    return 1;
  if (p[0].int64_value >= 9) {
    const ps_cpu_parallel_service_v1* parallel = s->cpu_parallel;
    if (!parallel || parallel->struct_size != sizeof(*parallel) ||
        parallel->abi_version != PS_CPU_PARALLEL_ABI_VERSION_1)
      return 1;
    struct RangeWork work = {s, source, scratch, p[0].int64_value};
    (void)parallel->run(parallel->context, 1, 1, 1, range_block, &work);
  } else {
    memcpy(scratch, source, 4);
  }
  if (!s->write_row(s->context, at, 3, &target, &samples))
    return 1;
  memcpy(target, scratch, 4);
  return s->release_scratch(s->context, scratch) ? 0 : 1;
}
static int unused(void* u, const ps_operation_value_view_v11* v, uint32_t n,
                  const ps_operation_parameter_value_v11* p, uint32_t pc,
                  uint32_t backend, ps_operation_cancelled_v11 c, void* cc,
                  const ps_operation_output_sink_v11* s, char* d, size_t dc) {
  (void)u;
  (void)v;
  (void)n;
  (void)p;
  (void)pc;
  (void)backend;
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
    11};
static const ps_operation_port_constraint_v11 input = {
    sizeof(input), PS_OPERATION_PORT_VALUE_V11, 0, 0, NULL};
static const ps_operation_descriptor_v11 operation = {
    .struct_size = sizeof(operation),
    .key = "test.planar_plugin",
    .key_size = 18,
    .input_count = 1,
    .flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
             PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
    .parameter_count = 1,
    .parameters = &parameter,
    .input_schema_count = 1,
    .input_schema = &input,
    .execute = unused,
    .workspace_bytes = 32,
    .output_count = 1,
    .outputs = {{.struct_size = sizeof(ps_operation_output_descriptor_v11),
                 .key = "values",
                 .key_size = 6,
                 .output_element_type = 4,
                 .shape_rule = PS_OPERATION_SHAPE_PRESERVE_FIRST_V11,
                 .region_rule = PS_OPERATION_REGION_WHOLE_V11,
#ifdef PS_PROJECT_PLANAR
                 .project_inputs = 1,
#endif
                 .output_schema = {sizeof(ps_operation_port_constraint_v11),
                                   PS_OPERATION_PORT_VALUE_V11, 0, 0, NULL}}}};
static void destroy(const ps_operation_descriptor_v11* p, uint32_t n) {
  (void)p;
  (void)n;
}
static const ps_operation_plugin_api_v11 api = {sizeof(api), 1, &operation,
                                                destroy};
static const ps_planar_operation_v3 entry = {sizeof(entry), infer, execute};
#ifdef PS_BAD_PLANAR
#define EXT_VERSION 77
#else
#define EXT_VERSION PS_PLANAR_OPERATION_ABI_VERSION_3
#endif
static const ps_planar_operation_plugin_api_v3 extension = {
    sizeof(extension), EXT_VERSION, 1, &entry};
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_11;
}
PS_OPERATION_EXPORT const ps_operation_plugin_api_v11*
ps_operation_plugin_get_api_v11(void) {
  return &api;
}
PS_OPERATION_EXPORT const ps_planar_operation_plugin_api_v3*
#ifdef PS_OLD_PLANAR
ps_operation_plugin_get_planar_api_v1(void) {
#else
ps_operation_plugin_get_planar_api_v3(void) {
#endif
  return &extension;
}
