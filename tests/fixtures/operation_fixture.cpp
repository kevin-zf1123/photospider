#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

#include "photospider/plugin/operation_module_api.h"

namespace {
std::atomic<std::uint32_t> destroy_count{0};
ps_kernel_status_v8 status(std::uint32_t code = PS_KERNEL_OK_V8) {
  ps_kernel_status_v8 value{};
  value.code = code;
  return value;
}
const ps_tensor_spec_v8 tensor = [] {
  ps_tensor_spec_v8 value{};
  value.struct_size = sizeof(value);
  value.key = {"number", 6};
  value.element_type = 3;
  value.rank = 1;
  value.shape[0] = 1;
  value.channel_axis = UINT32_MAX;
  return value;
}();
const ps_schema_v8 schema = [] {
  ps_schema_v8 value{};
  value.struct_size = sizeof(value);
  value.id = {"fixture.scalar", 14};
  value.version = 1;
  value.tensors = &tensor;
  value.tensor_count = 1;
  return value;
}();
const ps_port_contract_v8 port = [] {
  ps_port_contract_v8 value{};
  value.struct_size = sizeof(value);
  value.schema_id = schema.id;
  value.schema_version = 1;
  value.rank = 1;
  value.element_type = 3;
  return value;
}();
const ps_operation_output_v8 output = [] {
  ps_operation_output_v8 value{};
  value.struct_size = sizeof(value);
  value.key = {"value", 5};
  value.contract = port;
  value.prototype = &schema;
  value.input_count = UINT32_MAX;
  return value;
}();
// NOLINTBEGIN(whitespace/indent_namespace)
const ps_parameter_spec_v8 scale = {
    sizeof(scale), {"scale", 5}, PS_PARAMETER_FLOAT64_V8, 1, 0, 0, 0};
// NOLINTEND
ps_kernel_status_v8 prepare(void*, const ps_static_query_v8* query,
                            const ps_preparation_sink_v8* sink,
                            ps_operation_state_v8*) {
  if (query->input_count != 1 || query->parameter_count != 1 ||
      query->inputs[0].tensor_count != 1)
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  const auto& input = query->inputs[0].tensors[0];
  if (input.element_type != 3 || input.rank != 1 || input.shape[0] != 1 ||
      input.batch_rank || input.atomic_trailing_axes != 1)
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  return sink->set_output(sink->context, 0, &query->inputs[0], nullptr,
                          UINT32_MAX);
}
ps_kernel_status_v8 lower(void*, const ps_lowering_query_v8* query,
                          const ps_operation_builder_v8* builder) {
  const ps_placement_v8 placement{PS_BACKEND_CPU_V8,
                                  PS_KERNEL_DOMAIN_HOST_V8,
                                  {"cpu", 3},
                                  {nullptr, 0}};
  const std::uint64_t scratch[3]{};
  for (std::uint32_t i = 0; i < query->request_count; ++i) {
    const auto& request = query->requests[i];
    for (auto tile = request.begin; tile < request.end; ++tile) {
      std::uint32_t task = 0;
      auto result = builder->add_task(builder->context, {"scale", 5},
                                      {"scale-f64-v1", 12}, &placement, scratch,
                                      0, &task);
      if (!result.code)
        result = builder->read_input(builder->context, task, 0, 0, tile,
                                     PS_KERNEL_DOMAIN_HOST_V8);
      if (!result.code)
        result = builder->write_output(builder->context, task, request.output,
                                       request.member, tile);
      if (result.code)
        return result;
    }
  }
  return status();
}
bool scalar(const ps_kernel_view_spec_v8& view) {
  return view.port == 0 && view.member == 0 && view.tile_index == 0 &&
         view.domain == PS_KERNEL_DOMAIN_HOST_V8 && view.bytes == 8 &&
         view.byte_offset == 0 && view.layout.rank == 1 &&
         view.layout.element_type == 3 && view.layout.shape[0] == 1 &&
         view.layout.origin[0] == 0 && view.layout.strides[0] == 8;
}
ps_kernel_status_v8 prepare_kernel(void*, const ps_kernel_preparation_v8* query,
                                   ps_prepared_kernel_v8* result) {
  if (query->placement.backend != PS_BACKEND_CPU_V8 ||
      query->placement.domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      query->symbol.size != 5 || std::memcmp(query->symbol.data, "scale", 5) ||
      query->contract.size != 12 ||
      std::memcmp(query->contract.data, "scale-f64-v1", 12) ||
      query->read_count != 1 || query->write_count != 1 ||
      query->facts.parameter_count != 1 ||
      query->facts.parameters[0].type != PS_PARAMETER_FLOAT64_V8 ||
      !scalar(query->reads[0]) || !scalar(query->writes[0]) ||
      query->parameter_bytes || query->scratch[0] || query->scratch[1] ||
      query->scratch[2])
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  auto* multiplier =
      new (std::nothrow) double{query->facts.parameters[0].float64_value};
  if (!multiplier)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  *result = {};
  result->struct_size = sizeof(*result);
  result->abi_version = 8;
  result->context = multiplier;
  result->release = [](const void* state) {
    delete static_cast<const double*>(state);
  };
  result->invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                          ps_kernel_status_v8* result) {
    *result = status();
    if (invocation->cancellation.is_cancelled &&
        invocation->cancellation.is_cancelled(
            invocation->cancellation.context)) {
      *result = status(PS_KERNEL_CANCELLED_V8);
      return;
    }
    double value;
    std::memcpy(
        &value,
        static_cast<const std::uint8_t*>(invocation->inputs[0].allocation) +
            invocation->inputs[0].byte_offset,
        sizeof(value));
    value *= *static_cast<const double*>(invocation->prepared_context);
    std::memcpy(static_cast<std::uint8_t*>(invocation->outputs[0].allocation) +
                    invocation->outputs[0].byte_offset,
                &value, sizeof(value));
  };
  return status();
}
void destroy(void*) {
  ++destroy_count;
}
const ps_operation_v8 operation = [] {
  ps_operation_v8 value{};
  value.struct_size = sizeof(value);
  value.key = {"fixture.double", 14};
  value.flags = PS_OPERATION_CPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
                PS_OPERATION_PURE_V8;
  value.inputs = &port;
  value.input_count = 1;
  value.outputs = &output;
  value.output_count = 1;
  value.parameters = &scale;
  value.parameter_count = 1;
  value.prepare = prepare;
  value.lower = lower;
  value.prepare_kernel = prepare_kernel;
  return value;
}();
// NOLINTBEGIN(whitespace/indent_namespace)
const ps_operation_module_v8 api{sizeof(api), 8,       &operation,
                                 1,           nullptr, destroy};
// NOLINTEND
}  // namespace
extern "C" PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  return &api;
}
extern "C" PS_OPERATION_MODULE_EXPORT std::uint32_t
ps_operation_fixture_destroy_count() {
  return destroy_count.load();
}
