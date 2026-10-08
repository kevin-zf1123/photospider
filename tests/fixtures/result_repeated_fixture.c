#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "photospider/plugin/result_operation_plugin_api.h"

#define PS_BAD_RESULT_TABLE_COUNT 6U
#include "bad_result_table_support.h"

static _Atomic uint32_t starts, destroys;
PS_RESULT_EXPORT uint32_t fixture_repeated_starts(void) {
  return atomic_load(&starts);
}
PS_RESULT_EXPORT uint32_t fixture_repeated_destroys(void) {
  return atomic_load(&destroys);
}
static const uint32_t no_prefix = 0, one_prefix = 1, heterogeneous_prefix = 1;
static const ps_result_tensor_spec_v2 fixed_input_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v2),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {3},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 fixed_input_schema = {
    .struct_size = sizeof(ps_result_schema_v2),
    .id = "test.multi_output",
    .id_size = 17,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &fixed_input_tensor,
    .tensor_count = 1};
static const ps_result_port_v2 fixed_group = {
    .struct_size = sizeof(ps_result_port_v2),
    .kind = PS_RESULT_OBJECT_V2,
    .schema = &fixed_input_schema};
static const ps_result_port_v2 numeric_group = {
    .struct_size = sizeof(ps_result_port_v2),
    .kind = PS_RESULT_OBJECT_V2,
    .rank = 1,
    .element_type_mask = 15};
static const ps_result_port_v2 prefix = {
    .struct_size = sizeof(ps_result_port_v2),
    .kind = PS_RESULT_OBJECT_V2,
    .rank = 1,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2};
static const ps_result_tensor_spec_v2 output_tensor = {
    .struct_size = sizeof(ps_result_tensor_spec_v2),
    .key = "samples",
    .key_size = 7,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V2,
    .rank = 1,
    .shape = {3},
    .channel_axis = PS_RESULT_NO_CHANNEL_V2};
static const ps_result_schema_v2 output_schema = {
    .struct_size = sizeof(ps_result_schema_v2),
    .id = "fixture.repeated",
    .id_size = 16,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V2,
    .tensors = &output_tensor,
    .tensor_count = 1};
static const ps_result_output_v2 output = {
    .struct_size = sizeof(ps_result_output_v2),
    .key = "value",
    .key_size = 5,
    .port = {.struct_size = sizeof(ps_result_port_v2),
             .kind = PS_RESULT_OBJECT_V2,
             .schema = &output_schema},
    .input_count = UINT32_MAX,
    .execution = PS_RESULT_WHOLE_V2};
static int constraint_view(const ps_result_port_v2* inputs, uint32_t count,
                           uint32_t first) {
  uint32_t i;
  for (i = 0; i < count; ++i) {
    if (inputs[i].struct_size != sizeof(inputs[i]) ||
        inputs[i].kind != PS_RESULT_OBJECT_V2 || !inputs[i].schema)
      return 0;
    if (!first) {
      if (inputs[i].rank || inputs[i].element_type ||
          inputs[i].element_type_mask)
        return 0;
    } else if (i < first) {
      if (inputs[i].rank != 1 ||
          inputs[i].element_type != PS_RESULT_ELEMENT_FLOAT64_V2 ||
          inputs[i].element_type_mask)
        return 0;
    } else if (inputs[i].rank != 1 || inputs[i].element_type ||
               inputs[i].element_type_mask != 15) {
      return 0;
    }
  }
  return 1;
}
static int resolve(void* user, const ps_result_port_v2* inputs, uint32_t count,
                   const ps_result_parameter_value_v2* parameters,
                   uint32_t parameter_count, const ps_result_port_v2* outputs,
                   uint32_t output_count,
                   const ps_result_metadata_sink_v2* sink) {
  uint32_t first = *(const uint32_t*)user;
  ps_result_port_v2 port = outputs[0];
  ps_result_schema_v2 schema = *port.schema;
  ps_result_tensor_spec_v2 tensor = schema.tensors[0];
  (void)parameters;
  if (!constraint_view(inputs, count, first) || parameter_count ||
      output_count != 1 || count <= first || !inputs[first].schema ||
      inputs[first].schema->tensor_count != 1)
    return 6;
  if (first && (inputs[0].schema->tensors[0].shape[0] != 1 ||
                inputs[0].schema->tensors[0].batch_rank))
    return 5;
  if (inputs[first].schema->tensors[0].batch_rank ||
      inputs[first].schema->tensors[0].shape[0] > 8)
    return 5;
  tensor.shape[0] = user == &heterogeneous_prefix
                        ? 1
                        : inputs[first].schema->tensors[0].shape[0];
  schema.tensors = &tensor;
  port.schema = &schema;
  return sink->set_output(sink->context, 0, &port);
}
struct State {
  uint32_t stage, first;
};
static int start(void* user, void* state, const ps_result_query_v2* query,
                 const ps_result_services_v2* services) {
  struct State* s = (struct State*)state;
  s->first = *(const uint32_t*)user;
  atomic_fetch_add(&starts, 1);
  if (!constraint_view(query->inputs, query->input_count, s->first) ||
      services->abi_version != PS_RESULT_OPERATION_ABI_VERSION_2 ||
      query->input_count < s->first + 1 || query->input_count > s->first + 4)
    return 6;
  return 0;
}
static int read_number(const ps_result_services_v2* services,
                       const ps_result_query_v2* query, uint32_t input,
                       uint64_t at, double* out) {
  uint32_t type = query->inputs[input].schema->tensors[0].element_type;
  int rc;
  if (type == PS_RESULT_ELEMENT_UINT8_V2) {
    uint8_t v = 0;
    rc = services->read_tensor(services->context, input, 0, &at, 1, &v, 1);
    *out = v;
  } else if (type == PS_RESULT_ELEMENT_INT64_V2) {
    int64_t v = 0;
    rc = services->read_tensor(services->context, input, 0, &at, 1, &v, 8);
    *out = (double)v;
  } else if (type == PS_RESULT_ELEMENT_FLOAT32_V2) {
    float v = 0;
    rc = services->read_tensor(services->context, input, 0, &at, 1, &v, 4);
    *out = v;
  } else {
    rc = services->read_tensor(services->context, input, 0, &at, 1, out, 8);
  }
  return rc;
}
static int poll(void* user, void* state, const ps_result_query_v2* query,
                const ps_result_services_v2* services) {
  struct State* s = (struct State*)state;
  uint32_t i;
  uint64_t j, count = query->output->port.schema->tensors[0].shape[0];
  ps_result_region_v2 region = {0};
  ps_result_relation_row_v2 descriptor[5], rows[40];
  double values[8], factor = 1;
  uint32_t row_count = 0;
  (void)user;
  if (count > 8 ||
      !constraint_view(query->inputs, query->input_count, s->first))
    return 6;
  if (!s->stage++) {
    for (i = 0; i < query->input_count; ++i) {
      region.struct_size = sizeof(region);
      region.rank = 1;
      region.extent[0] = query->inputs[i].schema->tensors[0].shape[0];
      if (services->need_tensor(services->context, i, 0, 9, &region, 1))
        return 1;
    }
    return PS_RESULT_NEED_V2;
  }
  if (s->first && read_number(services, query, 0, 0, &factor))
    return 1;
  for (j = 0; j < count; ++j) {
    values[j] = 0;
    for (i = s->first; i < query->input_count; ++i) {
      double value;
      if (read_number(services, query, i, j, &value))
        return 1;
      values[j] += value;
    }
    values[j] *= factor;
    for (i = 0; i < query->input_count; ++i)
      rows[row_count++] = (ps_result_relation_row_v2){
          j, i, 1, PS_RESULT_TARGET_TENSOR_V2, 0, i < s->first ? 0 : j, 1};
  }
  for (i = 0; i < query->input_count; ++i)
    descriptor[i] = (ps_result_relation_row_v2){
        0, i, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
  region.struct_size = sizeof(region);
  region.rank = 1;
  region.extent[0] = count;
  if (services->consume_work(services->context, count * query->input_count) ||
      services->begin_result(services->context) ||
      services->bind_descriptor(services->context, descriptor,
                                query->input_count, PS_RESULT_EXACT_V2) ||
      services->publish_tensor(
          services->context, 0, &region, (const uint8_t*)values, count * 8,
          rows, row_count, PS_RESULT_EXACT_V2, PS_RESULT_FINAL_V2) ||
      services->publish_result(services->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V2;
}
static void destroy(void* user, void* state) {
  (void)user;
  (void)state;
  atomic_fetch_add(&destroys, 1);
}
#define OP(KEY, PREFIX, GROUP, RESOLVER, FIRST, MATCH, USER)          \
  {.struct_size = sizeof(ps_result_operation_v2),                     \
   .key = KEY,                                                        \
   .key_size = sizeof(KEY) - 1,                                       \
   .flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 | \
            PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2,                       \
   .inputs = PREFIX,                                                  \
   .input_count = FIRST,                                              \
   .outputs = &output,                                                \
   .output_count = 1,                                                 \
   .state_bytes = sizeof(struct State),                               \
   .workspace_bytes = 2048,                                           \
   .maximum_stages = 2,                                               \
   .user_data = (void*)USER,                                          \
   .resolve_metadata = RESOLVER,                                      \
   .start = start,                                                    \
   .poll = poll,                                                      \
   .destroy = destroy,                                                \
   .repeated_input = GROUP,                                           \
   .repeated_minimum = 1,                                             \
   .repeated_maximum = 4,                                             \
   .repeated_match = MATCH}
static ps_result_operation_v2 operations[] = {
    OP("fixture.repeated.fixed", NULL, &fixed_group, NULL, 0, 1, &no_prefix),
    OP("fixture.repeated.resolved", &prefix, &numeric_group, resolve, 1, 1,
       &one_prefix),
    OP("fixture.repeated.heterogeneous", &prefix, &numeric_group, resolve, 1, 0,
       &heterogeneous_prefix)};
static void destroy_plugin(void* context) {
  (void)context;
  ps_test_bad_result_retired();
}
static const ps_result_operation_plugin_api_v2 api = {
    sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_2,
    operations,  sizeof(operations) / sizeof(operations[0]),
    NULL,        destroy_plugin};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
#ifdef PS_RESULT_BAD_TABLES
  static ps_result_operation_plugin_api_v2 bad_api;
  static ps_result_operation_v2
      bad_operations[sizeof(operations) / sizeof(operations[0])];
  bad_api = api;
  memcpy(bad_operations, operations, sizeof(operations));
  bad_api.operations = bad_operations;
  switch (ps_test_bad_case) {
    case 1:
      bad_operations[1].repeated_minimum = 0;
      break;
    case 2:
      bad_operations[1].repeated_maximum = 1024;
      break;
    case 3:
      bad_operations[1].repeated_match = 2;
      break;
    case 4:
      bad_operations[1].repeated_input = NULL;
      break;
    case 5:
      bad_operations[1].repeated_maximum = 0;
      break;
    case 6:
      bad_operations[0].repeated_match = 0;
      break;
  }
  return &bad_api;
#endif
  return &api;
}
