#include "./expression_contract_fixture.h"

#include <stdlib.h>

static const ps_result_parameter_descriptor_v2 parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v2), "expression", 10,
     PS_RESULT_PARAMETER_STRING_V2, 1, 0, 0, 0},
    {sizeof(ps_result_parameter_descriptor_v2), "count", 5,
     PS_RESULT_PARAMETER_INT64_V2, 1, 0, 0, 0},
    {sizeof(ps_result_parameter_descriptor_v2), "start", 5,
     PS_RESULT_PARAMETER_FLOAT64_V2, 1, 0, 0, 0},
    {sizeof(ps_result_parameter_descriptor_v2), "step", 4,
     PS_RESULT_PARAMETER_FLOAT64_V2, 1, 0, 0, 0}};
static const ps_result_port_v2 input = {
    .struct_size = sizeof(input),
    .kind = PS_RESULT_OBJECT_V2,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1};
static int start(void* user, void* state, const ps_result_query_v2* query,
                 const ps_result_services_v2* services) {
  (void)user;
  (void)query;
  *(uint32_t*)state = 0;
  return services->cancelled(services->context) ? 2 : 0;
}
static int poll(void* user, void* state, const ps_result_query_v2* query,
                const ps_result_services_v2* services) {
  (void)user;
  const int empty = query->requested_kind == 2 && query->requested_count == 0;
  const uint64_t coefficients = query->inputs[0].schema->tensors[0].shape[0];
  if (!empty && !(*(uint32_t*)state)++) {
    const ps_result_region_v2 source = {.struct_size = sizeof(source),
                                        .rank = 1,
                                        .extent = {coefficients}};
    return services->need_tensor(services->context, 0, 0, 1, &source, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  }
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, NULL, 0, PS_RESULT_EXACT_V2))
    return 1;
  if (!empty) {
    uint64_t relation = 0;
    if (services->make_tensor_cartesian(services->context, 0, 0, 0, 1, 0,
                                        coefficients, PS_RESULT_CONSERVATIVE_V2,
                                        &relation))
      return 1;
    const float zeros[64] = {0};
    const uint64_t count = query->output->port.schema->tensors[0].shape[0];
    for (uint64_t begin = 0; begin < count; begin += 64) {
      if (services->cancelled(services->context))
        return 2;
      const uint64_t size = count - begin < 64 ? count - begin : 64;
      ps_result_region_v2 box = {.struct_size = sizeof(box),
                                 .rank = 1,
                                 .offset = {begin},
                                 .extent = {size}};
      if (services->consume_work(services->context, size) ||
          services->publish_tensor_with_relation(
              services->context, 0, &box, (const uint8_t*)zeros,
              size * sizeof(float), relation, PS_RESULT_FINAL_V2))
        return 1;
    }
    if (services->release_relation(services->context, relation))
      return 1;
  }
  return services->publish_result(services->context, 1) ? 1
                                                        : PS_RESULT_PUBLISH_V2;
}
static void destroy_state(void* user, void* state) {
  (void)user;
  (void)state;
}
struct Bundle {
  ps_result_operation_plugin_api_v2 api;
  ps_result_operation_v2 operation;
  ps_result_output_v2 output;
};
static void destroy(void* context) {
  free(context);
}
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  const ps_result_port_v2* prototype = fixture_expression_prototype();
  if (!prototype)
    return NULL;
  struct Bundle* bundle = (struct Bundle*)calloc(1, sizeof(struct Bundle));
  if (!bundle)
    return NULL;
  bundle->output = (ps_result_output_v2){.struct_size = sizeof(bundle->output),
                                         .key = "value",
                                         .key_size = 5,
                                         .port = *prototype,
                                         .input_count = UINT32_MAX,
                                         .execution = PS_RESULT_WHOLE_V2};
  bundle->operation = (ps_result_operation_v2){
      .struct_size = sizeof(bundle->operation),
      .key = "fixture.expression_contract",
      .key_size = 27,
      .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
               PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,
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
  bundle->api = (ps_result_operation_plugin_api_v2){
      .struct_size = sizeof(bundle->api),
      .abi_version = PS_RESULT_OPERATION_ABI_VERSION_2,
      .operations = &bundle->operation,
      .operation_count = 1,
      .context = bundle,
      .destroy = destroy};
  return &bundle->api;
}
