#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"

static void (*lifetime_event)(void*, uint32_t);
static void* lifetime_user;
#ifdef __cplusplus
extern "C"
#endif
    PS_RESULT_EXPORT void ps_extension_set_lifetime_event(
        void (*event)(void*, uint32_t), void* user) {
  lifetime_event = event;
  lifetime_user = user;
}
static const ps_result_resource_identity_v4 identity = {
    3,
    {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
     0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
     0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad}};
#define ID(KEY)                             \
  {sizeof(ps_result_extension_identity_v4), \
   1,                                       \
   KEY,                                     \
   sizeof(KEY) - 1,                         \
   "fixture/1",                             \
   9}
static const ps_result_extension_identity_v4 validator_id = ID("fixture.bytes");
static const ps_result_extension_identity_v4 report_id = ID("fixture.count");
static int validation_prepare(void* unused,
                              const ps_result_validation_preparation_v4* p,
                              void** state) {
  (void)unused;
  if (!p->output || p->resource_count != 1 ||
      p->resources[0].identity.byte_length != 3 ||
      memcmp(p->resources[0].bytes, "abc", 3))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  *state = malloc(3);
  if (!*state)
    return PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V4;
  memcpy(*state, p->resources[0].bytes, 3);
  return 0;
}
static void validation_destroy(void* unused, void* state) {
  (void)unused;
  free(state);
  if (lifetime_event)
    lifetime_event(lifetime_user, 1);
}
static int validate(void* unused, const void* state,
                    const ps_result_validation_context_v4* c) {
  (void)unused;
#if defined(PS_EXTENSION_FAILURE_FIXTURE) && PS_EXTENSION_CASE == 5
  uint8_t unauthorized = 0;
  ps_result_region_v4 bad_region = {.struct_size = sizeof(bad_region),
                                    .rank = 1,
                                    .extent = {4}};
  c->read_tensor(c->context, 0, &bad_region, &unauthorized, 1);
  throw std::runtime_error("after unauthorized read");
#endif
#if defined(PS_EXTENSION_FAILURE_FIXTURE) && PS_EXTENSION_CASE == 7
  c->fail_domain(c->context, PS_RESULT_STATUS_FAILURE_V4,
                 UINT64_C(0x123456789abcdef0));
  throw std::runtime_error("after domain error");
#endif
  if (lifetime_event)
    lifetime_event(lifetime_user, 0);
  ps_result_region_v4 region;
  uint32_t count = 0;
  uint8_t bytes[3];
  if (c->tensor_regions(c->context, 0, &region, 1, &count) || count != 1 ||
      c->read_tensor(c->context, 0, &region, bytes, 3))
    return 1;
  return memcmp(state, bytes, 3) ? PS_RESULT_STATUS_INVALID_ARGUMENT_V4 : 0;
}
static ps_result_validation_definition_v4 validators[] = {
    {sizeof(ps_result_validation_definition_v4), ID("fixture.bytes"), 4096, 0,
     NULL, validation_prepare, validate, validation_destroy}};
static int validate_report(void* unused, const uint8_t* bytes, uint32_t size) {
  (void)unused;
  (void)bytes;
  return size == 8 ? 0 : 1;
}
static int merge(void* unused, const uint8_t* a, const uint8_t* b, uint8_t* out,
                 uint32_t size) {
  (void)unused;
  uint64_t x, y;
  if (size != 8)
    return 1;
  memcpy(&x, a, 8);
  memcpy(&y, b, 8);
  if (y > UINT64_MAX - x)
    return PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V4;
  x += y;
  memcpy(out, &x, 8);
  return 0;
}
static ps_result_diagnostic_definition_v4 diagnostics[] = {
    {sizeof(ps_result_diagnostic_definition_v4), ID("fixture.count"), 8, 64,
     NULL, validate_report, merge}};
static const ps_result_tensor_spec_v4 tensor = {.struct_size = sizeof(tensor),
                                                .key = "bytes",
                                                .key_size = 5,
                                                .element_type = 1,
                                                .rank = 1,
                                                .shape = {3}};
static const ps_result_schema_v4 schema = {.struct_size = sizeof(schema),
                                           .id = "fixture.bytes",
                                           .id_size = 13,
                                           .version = 1,
                                           .publication = 1,
                                           .tensors = &tensor,
                                           .tensor_count = 1};
static const ps_result_output_v4 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v4),
             .kind = 6,
             .schema = &schema,
             .validators = &validator_id,
             .validator_count = 1},
    .execution = PS_RESULT_WHOLE_V4,
    .failure_delivery = 0};
static int resolve(void* unused, const ps_result_port_v4* in, uint32_t ni,
                   const ps_result_parameter_value_v4* p, uint32_t np,
                   const ps_result_port_v4* out, uint32_t no,
                   const ps_result_metadata_sink_v4* sink) {
  (void)unused;
  (void)in;
  (void)ni;
  (void)p;
  (void)np;
  (void)no;
#if defined(PS_EXTENSION_FAILURE_FIXTURE) && PS_EXTENSION_CASE == 3
  sink->set_resources(sink->context, &identity, 1025);
  throw std::runtime_error("after service failure");
#elif defined(PS_EXTENSION_FAILURE_FIXTURE) && PS_EXTENSION_CASE == 4
  throw std::runtime_error("resolver failure");
#endif
  return sink->set_output(sink->context, 0, out) ||
         sink->set_resources(sink->context, &identity, 1);
}
static int start(void* u, void* state, const ps_result_query_v4* q,
                 const ps_result_services_v4* s) {
  (void)u;
  (void)state;
  (void)q;
  (void)s;
  return 0;
}
static int poll(void* u, void* state, const ps_result_query_v4* q,
                const ps_result_services_v4* s) {
  (void)u;
  (void)state;
  ps_result_region_v4 region = {.struct_size = sizeof(region),
                                .rank = 1,
                                .extent = {3}};
  const uint64_t work = 3;
  if (q->resource_count != 1 ||
      s->report_diagnostic(s->context, 0, (const uint8_t*)&work, 8) ||
      s->begin_result(s->context) ||
      s->publish_tensor(s->context, 0, &region, q->resources[0].bytes, 3) ||
      s->publish_result(s->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V4;
}
static void destroy(void* u, void* state) {
  (void)u;
  (void)state;
}
static void destroy_api(void* u) {
  (void)u;
  if (lifetime_event)
    lifetime_event(lifetime_user, 2);
}
static const ps_result_operation_v4 operation = {
    .struct_size = sizeof(operation),
    .key = "fixture.c_extensions",
    .key_size = 20,
    .flags = PS_RESULT_FLAG_CPU_V4 | PS_RESULT_FLAG_DETERMINISTIC_V4 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V4,
    .outputs = &output,
    .output_count = 1,
    .maximum_stages = 2,
    .resolve_metadata = resolve,
    .start = start,
    .poll = poll,
    .destroy = destroy,
    .diagnostics = &report_id,
    .diagnostic_count = 1};
static const ps_result_operation_plugin_api_v4 api = {
    .struct_size = sizeof(api),
    .abi_version = PS_RESULT_OPERATION_ABI_VERSION_4,
    .operations = &operation,
    .operation_count = 1,
    .destroy = destroy_api,
    .validators = validators,
    .validator_count = 1,
    .diagnostics = diagnostics,
    .diagnostic_count = 1};
#ifdef __cplusplus
extern "C"
#endif
    PS_RESULT_EXPORT const ps_result_operation_plugin_api_v4*
    ps_result_operation_plugin_get_api_v4(void) {
#if defined(PS_EXTENSION_FAILURE_FIXTURE) && PS_EXTENSION_CASE == 1
  validators[0].struct_size = 0;
  validators[0].identity.key = (const char*)(uintptr_t)1;
#elif defined(PS_EXTENSION_FAILURE_FIXTURE) && PS_EXTENSION_CASE == 2
  diagnostics[0].struct_size = 0;
  diagnostics[0].identity.key = (const char*)(uintptr_t)1;
#elif defined(PS_EXTENSION_FAILURE_FIXTURE) && PS_EXTENSION_CASE == 6
  validators[0].identity.version = 2;
#endif
  return &api;
}
