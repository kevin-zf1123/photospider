#include "./expression_contract_fixture.h"

#include <stdlib.h>

#include "photospider/plugin/result_tile_helpers.h"

static const ps_result_parameter_descriptor_v3 parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v3), "expression", 10,
     PS_RESULT_PARAMETER_STRING_V3, 1, 0, 0, 0},
    {sizeof(ps_result_parameter_descriptor_v3), "count", 5,
     PS_RESULT_PARAMETER_INT64_V3, 1, 0, 0, 0},
    {sizeof(ps_result_parameter_descriptor_v3), "start", 5,
     PS_RESULT_PARAMETER_FLOAT64_V3, 1, 0, 0, 0},
    {sizeof(ps_result_parameter_descriptor_v3), "step", 4,
     PS_RESULT_PARAMETER_FLOAT64_V3, 1, 0, 0, 0}};
static const ps_result_port_v3 input = {
    .struct_size = sizeof(input),
    .kind = PS_RESULT_OBJECT_V3,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V3,
    .rank = 1};
static int start(void* user, void* state, const ps_result_query_v3* query,
                 const ps_result_services_v3* services) {
  (void)user;
  (void)query;
  *(uint32_t*)state = 0;
  return services->cancelled(services->context) ? 2 : 0;
}
static int poll(void* user, void* state, const ps_result_query_v3* query,
                const ps_result_services_v3* services) {
  (void)user;
  const int empty = !query->requested.whole && !query->requested.tile_count;
  const uint64_t coefficients = query->inputs[0].schema->tensors[0].shape[0];
  if (!empty && !(*(uint32_t*)state)++) {
    const ps_result_region_v3 source = {.struct_size = sizeof(source),
                                        .rank = 1,
                                        .extent = {coefficients}};
    return ps_result_need_regions_v3(services, query, 0, &source, 1)
               ? 1
               : PS_RESULT_NEED_V3;
  }
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, NULL, 0, PS_RESULT_EXACT_V3))
    return 1;
  if (!empty) {
    uint64_t relation = 0;
    if (services->make_tensor_cartesian(services->context, 0, 0, 0, 1, 0,
                                        coefficients, PS_RESULT_CONSERVATIVE_V3,
                                        &relation))
      return 1;
    const float zeros[64] = {0};
    const uint64_t count = query->output->port.schema->tensors[0].shape[0];
    for (uint64_t begin = 0; begin < count; begin += 64) {
      if (services->cancelled(services->context))
        return 2;
      const uint64_t size = count - begin < 64 ? count - begin : 64;
      ps_result_region_v3 box = {.struct_size = sizeof(box),
                                 .rank = 1,
                                 .offset = {begin},
                                 .extent = {size}};
      if (services->consume_work(services->context, size) ||
          services->publish_tensor_with_relation(
              services->context, 0, &box, (const uint8_t*)zeros,
              size * sizeof(float), relation, PS_RESULT_FINAL_V3))
        return 1;
    }
    if (services->release_relation(services->context, relation))
      return 1;
  }
  return services->publish_result(services->context, 1) ? 1
                                                        : PS_RESULT_PUBLISH_V3;
}
static void destroy_state(void* user, void* state) {
  (void)user;
  (void)state;
}
struct Bundle {
  ps_result_operation_plugin_api_v3 api;
  ps_result_operation_v3 operation;
  ps_result_output_v3 output;
};
static void destroy(void* context) {
  free(context);
}
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v3*
ps_result_operation_plugin_get_api_v3(void) {
  const ps_result_port_v3* prototype = fixture_expression_prototype();
  if (!prototype)
    return NULL;
  struct Bundle* bundle = (struct Bundle*)calloc(1, sizeof(struct Bundle));
  if (!bundle)
    return NULL;
  bundle->output = (ps_result_output_v3){.struct_size = sizeof(bundle->output),
                                         .key = "value",
                                         .key_size = 5,
                                         .port = *prototype,
                                         .input_count = UINT32_MAX,
                                         .execution = PS_RESULT_WHOLE_V3};
  bundle->operation = (ps_result_operation_v3){
      .struct_size = sizeof(bundle->operation),
      .key = "fixture.expression_contract",
      .key_size = 27,
      .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_DETERMINISTIC_V3 |
               PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
      .inputs = &input,
      .input_count = 1,
      .outputs = &bundle->output,
      .output_count = 1,
      .parameters = parameters,
      .parameter_count = 4,
      .state_bytes = sizeof(uint32_t),
      .maximum_stages = 2,
      .resolve_metadata = fixture_expression_metadata,
      .start = start,
      .poll = poll,
      .destroy = destroy_state};
  bundle->api = (ps_result_operation_plugin_api_v3){
      .struct_size = sizeof(bundle->api),
      .abi_version = PS_RESULT_OPERATION_ABI_VERSION_3,
      .operations = &bundle->operation,
      .operation_count = 1,
      .context = bundle,
      .destroy = destroy};
  return &bundle->api;
}
