#include <stdint.h>
#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"
#include "photospider/plugin/result_tile_helpers.h"

#ifndef PS_BAD_CONTRACT_CASE
#define PS_BAD_CONTRACT_CASE 0
#endif

static const ps_result_parameter_descriptor_v3 parameters[] = {
    {sizeof(ps_result_parameter_descriptor_v3), "dtype", 5,
     PS_RESULT_PARAMETER_STRING_V3, 1, 0, 0, 0},
    {sizeof(ps_result_parameter_descriptor_v3), "semantic", 8,
     PS_RESULT_PARAMETER_STRING_V3, 1, 0, 0, 0}};
static ps_result_port_v3 input = {.struct_size = sizeof(input),
                                  .kind = PS_RESULT_OBJECT_V3,
                                  .rank = 1,
                                  .element_type_mask = 12};
static ps_result_tensor_spec_v3 tensor = {
    .struct_size = sizeof(tensor),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT32_V3,
    .rank = 1,
    .shape = {1},
    .channel_axis = PS_RESULT_NO_CHANNEL_V3};
static ps_result_schema_v3 schema = {
    .struct_size = sizeof(schema),
    .id = "fixture.contract",
    .id_size = 16,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V3,
    .tensors = &tensor,
    .tensor_count = 1};
static ps_result_output_v3 output = {
    .struct_size = sizeof(output),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v3),
             .kind = PS_RESULT_OBJECT_V3,
             .schema = &schema,
             .rank = 1,
             .element_type_mask = 12},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_WHOLE_V3};
static const ps_result_parameter_value_v3* parameter(
    const ps_result_parameter_value_v3* values, uint32_t count,
    const char* key) {
  for (uint32_t i = 0; i < count; ++i)
    if (values[i].key_size == strlen(key) &&
        !memcmp(values[i].key, key, values[i].key_size))
      return &values[i];
  return NULL;
}
static int hex(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  return -1;
}
static int resolve(void* user, const ps_result_port_v3* inputs, uint32_t count,
                   const ps_result_parameter_value_v3* values,
                   uint32_t value_count, const ps_result_port_v3* outputs,
                   uint32_t output_count,
                   const ps_result_metadata_sink_v3* sink) {
  (void)user;
  const ps_result_parameter_value_v3* dtype =
      parameter(values, value_count, "dtype");
  const ps_result_parameter_value_v3* semantic =
      parameter(values, value_count, "semantic");
  if (!count || count > 4 || output_count != 1 || !dtype || !semantic ||
      dtype->type != PS_RESULT_PARAMETER_STRING_V3 ||
      semantic->type != PS_RESULT_PARAMETER_STRING_V3 ||
      !semantic->string_size || semantic->string_size > 8192 ||
      semantic->string_size % 2)
    return 6;
  for (uint32_t i = 0; i < count; ++i)
    if (!inputs[i].schema || inputs[i].schema->field_count ||
        inputs[i].schema->tensor_count != 1 ||
        inputs[i].schema->tensors[0].batch_rank)
      return 5;
  ps_result_port_v3 resolved = outputs[0];
  ps_result_schema_v3 selected_schema = *resolved.schema;
  ps_result_tensor_spec_v3 selected_tensor = selected_schema.tensors[0];
  if (dtype->string_size == 7 && !memcmp(dtype->string_value, "float32", 7))
    selected_tensor.element_type = PS_RESULT_ELEMENT_FLOAT32_V3;
  else if (dtype->string_size == 7 &&
           !memcmp(dtype->string_value, "float64", 7))
    selected_tensor.element_type = PS_RESULT_ELEMENT_FLOAT64_V3;
  else
    return 6;
  uint8_t payload[4096];
  for (uint32_t i = 0; i < semantic->string_size; i += 2) {
    const int hi = hex(semantic->string_value[i]);
    const int lo = hex(semantic->string_value[i + 1]);
    if (hi < 0 || lo < 0)
      return 6;
    payload[i / 2] = (uint8_t)(hi * 16 + lo);
  }
  const ps_result_facet_view_v3 facet = {
      sizeof(facet), "photospider.semantic",   20, 1,
      payload,       semantic->string_size / 2};
  selected_tensor.shape[0] = count;
  selected_tensor.facets = &facet;
  selected_tensor.facet_count = 1;
  selected_schema.tensors = &selected_tensor;
  resolved.schema = &selected_schema;
  return sink->set_output(sink->context, 0, &resolved);
}
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
  const uint32_t count = query->input_count;
  const int is_fp32 = query->output->port.schema->tensors[0].element_type ==
                      PS_RESULT_ELEMENT_FLOAT32_V3;
  const ps_result_region_v3 source = {.struct_size = sizeof(source),
                                      .rank = 1,
                                      .extent = {1}};
  if (!empty && !(*(uint32_t*)state)++) {
    for (uint32_t i = 0; i < count; ++i)
      if (ps_result_need_regions_v3(services, query, i, &source, 1))
        return 1;
    return PS_RESULT_NEED_V3;
  }
  if (services->begin_result(services->context))
    return 1;
  float fp32[4];
  double fp64[4];
  const uint64_t at = 0;
  for (uint32_t i = 0; !empty && i < count; ++i) {
    if (services->cancelled(services->context))
      return 2;
    if (query->inputs[i].schema->tensors[0].element_type ==
        PS_RESULT_ELEMENT_FLOAT32_V3) {
      if (services->read_tensor(services->context, i, 0, &at, 1, &fp32[i], 4))
        return 1;
      fp64[i] = fp32[i];
    } else {
      if (services->read_tensor(services->context, i, 0, &at, 1, &fp64[i], 8))
        return 1;
      if (is_fp32)
        fp32[i] = (float)fp64[i];
    }
  }
  if (!empty) {
    const ps_result_region_v3 region = {.struct_size = sizeof(region),
                                        .rank = 1,
                                        .extent = {count}};
    if (services->consume_work(services->context, count) ||
        services->publish_tensor(
            services->context, 0, &region,
            is_fp32 ? (const uint8_t*)fp32 : (const uint8_t*)fp64,
            count * (is_fp32 ? 4 : 8)))
      return 1;
  }
  return services->publish_result(services->context, 1) ? 1
                                                        : PS_RESULT_PUBLISH_V3;
}
static void destroy_state(void* user, void* state) {
  (void)user;
  (void)state;
}
static ps_result_operation_v3 operation = {
    .struct_size = sizeof(operation),
    .key = "fixture.contract",
    .key_size = 16,
    .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_DETERMINISTIC_V3 |
             PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
    .outputs = &output,
    .output_count = 1,
    .parameters = parameters,
    .parameter_count = 2,
    .state_bytes = sizeof(uint32_t),
    .workspace_bytes = 4096,
    .maximum_stages = 2,
    .resolve_metadata = resolve,
    .start = start,
    .poll = poll,
    .destroy = destroy_state,
    .repeated_input = &input,
    .repeated_minimum = 1,
    .repeated_maximum = 4,
    .repeated_match = 1};
static void destroy(void* context) {
  (void)context;
}
static const ps_result_operation_plugin_api_v3 api = {
    sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_3, &operation, 1, NULL,
    destroy};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v3*
ps_result_operation_plugin_get_api_v3(void) {
#if PS_BAD_CONTRACT_CASE == 1
  operation.struct_size = 0;
#elif PS_BAD_CONTRACT_CASE == 2
  schema.domain_rank = 9;
#elif PS_BAD_CONTRACT_CASE == 3
  schema.tensors = NULL;
#elif PS_BAD_CONTRACT_CASE == 4
  output.port.kind = 999;
#elif PS_BAD_CONTRACT_CASE == 5
  operation.repeated_minimum = 0;
#elif PS_BAD_CONTRACT_CASE == 6
  operation.parameter_count = 129;
#elif PS_BAD_CONTRACT_CASE == 7
  tensor.rank = 9;
#elif PS_BAD_CONTRACT_CASE == 8
  input.element_type_mask = UINT32_C(1) << 7;
#elif PS_BAD_CONTRACT_CASE == 9
  input.element_type = PS_RESULT_ELEMENT_FLOAT64_V3;
#elif PS_BAD_CONTRACT_CASE == 10
  output.port.schema = NULL;
#elif PS_BAD_CONTRACT_CASE == 11
  output.port.tensor_key_size = 1;
#elif PS_BAD_CONTRACT_CASE == 12
  output.port.requires_semantics = 2;
#endif
  return &api;
}
