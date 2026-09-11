#include <string.h>

#include "photospider/plugin/operation_plugin_api.h"

#ifndef PS_BAD_CONTRACT_CASE
#define PS_BAD_CONTRACT_CASE 0
#endif

static int execute(void* user, const ps_operation_value_view_v9* inputs,
                   uint32_t count,
                   const ps_operation_parameter_value_v9* parameters,
                   uint32_t parameter_count, uint32_t backend,
                   ps_operation_cancelled_v9 cancelled, void* cancel_context,
                   const ps_operation_output_sink_v9* sink, char* diagnostic,
                   size_t diagnostic_capacity) {
  (void)user;
  (void)parameters;
  (void)parameter_count;
  (void)cancelled;
  (void)cancel_context;
  (void)diagnostic;
  (void)diagnostic_capacity;
  if (!inputs || !count || backend != 1 ||
      sink->output_element_type != PS_OPERATION_ELEMENT_FLOAT32_V9)
    return PS_OPERATION_RESULT_FAILURE_V9;
  uint8_t* bytes = sink->allocate_output(sink->context);
  if (!bytes)
    return PS_OPERATION_RESULT_FAILURE_V9;
  for (uint32_t i = 0; i < count; ++i) {
    float output;
    if (inputs[i].element_type == PS_OPERATION_ELEMENT_FLOAT32_V9) {
      memcpy(&output, inputs[i].data + inputs[i].byte_offset, sizeof(output));
    } else {
      double input;
      memcpy(&input, inputs[i].data + inputs[i].byte_offset, sizeof(input));
      output = (float)input;
    }
    memcpy(bytes + i * sizeof(output), &output, sizeof(output));
  }
  return sink->publish(sink->context, sink->output_element_type,
                       sink->output_shape, sink->output_rank,
                       sink->output_facets, sink->output_facet_count, bytes,
                       sink->output_byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V9
             : PS_OPERATION_RESULT_FAILURE_V9;
}
static const ps_operation_parameter_descriptor_v9 parameters[] = {
    {sizeof(ps_operation_parameter_descriptor_v9), "dtype", 5,
     PS_OPERATION_PARAMETER_STRING_V9, 1, 0, 0, 0},
    {sizeof(ps_operation_parameter_descriptor_v9), "semantic", 8,
     PS_OPERATION_PARAMETER_STRING_V9, 1, 0, 0, 0}};
static ps_operation_semantic_constraint_v9 input_constraint = {
    sizeof(ps_operation_semantic_constraint_v9), 0, 0, 1, 0, NULL, 12};
static const ps_operation_port_constraint_v9 ports[] = {
    {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_VALUE_V9, 0, 0,
     &input_constraint}};
static const ps_operation_extent_v9 axes[] = {
    {sizeof(ps_operation_extent_v9), PS_OPERATION_EXTENT_INPUT_COUNT_V9, 1,
     NULL, 0, 0, 0, 0, NULL, 0, 1, 1}};
static ps_operation_contract_v9 contract = {sizeof(ps_operation_contract_v9),
                                            PS_OPERATION_DTYPE_PARAMETER_V9,
                                            0,
                                            "dtype",
                                            5,
                                            1,
                                            axes,
                                            1,
                                            4,
                                            1,
                                            PS_OPERATION_SEMANTIC_PARAMETER_V9,
                                            0,
                                            0,
                                            NULL,
                                            "semantic",
                                            8};
static ps_operation_descriptor_v9 operations[] = {
    {sizeof(ps_operation_descriptor_v9),
     "fixture.contract",
     16,
     0,
     PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
         PS_OPERATION_FLAG_SIDE_EFFECT_FREE,
     0,
     1,
     2,
     parameters,
     1,
     ports,
     execute,
     0,
     0,
     0,
     0,
     1,
     {{sizeof(ps_operation_output_descriptor_v9),
       "value",
       5,
       PS_OPERATION_ELEMENT_FLOAT32_V9,
       0,
       0,
       PS_OPERATION_SHAPE_AXES_V9,
       PS_OPERATION_REGION_WHOLE_V9,
       0,
       {sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_TYPED_V9, 0,
        0, NULL},
       0,
       0,
       0,
       0,
       &contract,
       0,
       0,
       0,
       0,
       NULL}}}};
static void destroy(const ps_operation_descriptor_v9* values, uint32_t count) {
  (void)values;
  (void)count;
}
static const uint64_t wide_shape[] = {UINT64_C(1) << 61};
static const ps_operation_plugin_api_v9 api = {
    sizeof(ps_operation_plugin_api_v9), 1, operations, destroy};
uint32_t ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_9;
}
const ps_operation_plugin_api_v9* ps_operation_plugin_get_api_v9(void) {
#if PS_BAD_CONTRACT_CASE == 1
  contract.struct_size = 0;
#elif PS_BAD_CONTRACT_CASE == 2
  contract.axis_count = 9;
#elif PS_BAD_CONTRACT_CASE == 3
  contract.axes = NULL;
#elif PS_BAD_CONTRACT_CASE == 4
  contract.dtype_rule = 999;
#elif PS_BAD_CONTRACT_CASE == 5
  contract.repeated_minimum = 0;
#elif PS_BAD_CONTRACT_CASE == 6
  contract.semantic_parameter_size = 1025;
#elif PS_BAD_CONTRACT_CASE == 7
  contract.axis_count = 0;
  contract.axes = NULL;
  contract.repeated_minimum = 0;
  contract.repeated_maximum = 0;
  operations[0].input_schema_count = 0;
  operations[0].input_schema = NULL;
  operations[0].outputs[0].output_rank = 1;
  operations[0].outputs[0].output_shape = wide_shape;
  operations[0].outputs[0].shape_rule = PS_OPERATION_SHAPE_FIXED_V9;
  operations[0].outputs[0].output_element_type = PS_OPERATION_ELEMENT_UINT8_V9;
#elif PS_BAD_CONTRACT_CASE == 8
  input_constraint.element_type_mask = 16;
#elif PS_BAD_CONTRACT_CASE == 9
  input_constraint.element_type = PS_OPERATION_ELEMENT_FLOAT64_V9;
#elif PS_BAD_CONTRACT_CASE >= 10
  /* A typed output cannot recover semantics from Drop or a NULL contract. */
  memset(&contract, 0, sizeof(contract));
  contract.struct_size = sizeof(contract);
  static ps_operation_port_constraint_v9 drop_port = {
      sizeof(ps_operation_port_constraint_v9), PS_OPERATION_PORT_TYPED_V9, 0, 0,
      NULL};
  drop_port.kind =
      PS_BAD_CONTRACT_CASE == 10   ? PS_OPERATION_PORT_RGBA_FLOAT32_V9
      : PS_BAD_CONTRACT_CASE == 11 ? PS_OPERATION_PORT_FLOAT32_MASK_V9
                                   : PS_OPERATION_PORT_TYPED_V9;
  operations[0].input_count = 1;
  operations[0].input_schema_count = 1;
  operations[0].input_schema = &drop_port;
  operations[0].parameter_count = 0;
  operations[0].parameters = NULL;
  operations[0].outputs[0].shape_rule = PS_OPERATION_SHAPE_PRESERVE_FIRST_V9;
  operations[0].outputs[0].output_schema = drop_port;
  if (PS_BAD_CONTRACT_CASE == 11)
    operations[0].outputs[0].contract = NULL;
#endif
  return &api;
}
