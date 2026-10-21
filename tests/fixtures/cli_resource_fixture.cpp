#include <cstdint>
#include <cstring>
#include <new>

#include "photospider/plugin/operation_module_api.h"

namespace {
ps_kernel_status_v8 status(std::uint32_t code) {
  ps_kernel_status_v8 result{};
  result.code = code;
  return result;
}
const auto tensor = [] {
  ps_tensor_spec_v8 result{};
  result.struct_size = sizeof(result);
  result.key = {"samples", 7};
  result.element_type = 3;  // Float64.
  result.rank = 1;
  result.shape[0] = 4;
  result.channel_axis = UINT32_MAX;
  return result;
}();
const ps_schema_v8 schema = {
    sizeof(schema), {"fixture.vector", 14}, 1, 1, &tensor, 0,
    nullptr};  // NOLINT(whitespace/indent_namespace)
const auto port = [] {
  ps_port_contract_v8 result{};
  result.struct_size = sizeof(result);
  result.schema_id = {"fixture.vector", 14};
  result.schema_version = 1;
  result.tensor_key = {"samples", 7};
  result.element_type = 3;
  result.rank = 1;
  return result;
}();
const ps_operation_output_v8 output = {
    sizeof(output), {"value", 5}, port, &schema,
    UINT32_MAX,     nullptr};  // NOLINT(whitespace/indent_namespace)
const ps_parameter_spec_v8 parameter = {
    sizeof(parameter),
    {"sha256", 6},
    PS_PARAMETER_STRING_V8,
    1,
    0,
    0,
    0};  // NOLINT(whitespace/indent_namespace)
int nibble(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
ps_kernel_status_v8 prepare(void*, const ps_static_query_v8* query,
                            const ps_preparation_sink_v8* sink,
                            ps_operation_state_v8*) {
  if (query->input_count != 1 || query->parameter_count != 1 ||
      query->inputs[0].tensor_count != 1 ||
      query->inputs[0].tensors[0].rank != 1 ||
      query->inputs[0].tensors[0].batch_rank != 0 ||
      query->inputs[0].tensors[0].shape[0] != 4 ||
      query->parameters[0].type != PS_PARAMETER_STRING_V8 ||
      query->parameters[0].string_value.size != 64)
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  ps_resource_id_v8 id{};
  id.byte_length = 1;
  const auto text = query->parameters[0].string_value;
  for (unsigned i = 0; i < 32; ++i) {
    int a = nibble(text.data[2 * i]), b = nibble(text.data[2 * i + 1]);
    if (a < 0 || b < 0)
      return status(PS_KERNEL_INVALID_ARGUMENT_V8);
    id.sha256[i] = static_cast<std::uint8_t>(a * 16 + b);
  }
  return sink->require_resource(sink->context, &id);
}
ps_kernel_status_v8 validate_resources(void*, const void*,
                                       const ps_resource_view_v8* resources,
                                       std::uint32_t count) {
  return status(count == 1 && resources[0].identity.byte_length == 1
                    ? PS_KERNEL_OK_V8
                    : PS_KERNEL_TYPE_MISMATCH_V8);
}
ps_kernel_status_v8 lower(void*, const ps_lowering_query_v8* query,
                          const ps_operation_builder_v8* builder) {
  const ps_placement_v8 placement{PS_BACKEND_CPU_V8,
                                  PS_KERNEL_DOMAIN_HOST_V8,
                                  {"cpu", 3},
                                  {nullptr, 0}};
  const std::uint64_t scratch[3] = {};
  for (unsigned i = 0; i < query->request_count; ++i) {
    const auto& request = query->requests[i];
    for (auto tile = request.begin; tile < request.end; ++tile) {
      std::uint32_t task = PS_ID_NONE_V8;
      auto result = builder->add_task(builder->context, {"scale", 5},
                                      {"scale-f64-v1", 12}, &placement, scratch,
                                      0, &task);
      if (result.code)
        return result;
      result = builder->read_input(builder->context, task, 0, request.member,
                                   tile, PS_KERNEL_DOMAIN_HOST_V8);
      if (result.code)
        return result;
      result = builder->write_output(builder->context, task, request.output,
                                     request.member, tile);
      if (result.code)
        return result;
    }
  }
  return status(PS_KERNEL_OK_V8);
}
struct KernelState {
  const std::uint8_t* factor;
};
void invoke(const ps_kernel_invocation_v8* query, ps_kernel_status_v8* result) {
  if (query->cancellation.is_cancelled &&
      query->cancellation.is_cancelled(query->cancellation.context)) {
    *result = status(PS_KERNEL_CANCELLED_V8);
    return;
  }
  const auto* state = static_cast<const KernelState*>(query->prepared_context);
  const auto& input = query->inputs[0];
  const auto& output = query->outputs[0];
  const auto* source =
      static_cast<const std::uint8_t*>(input.allocation) + input.byte_offset;
  auto* destination =
      static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
  for (std::uint64_t i = 0; i < output.layout->shape[0]; ++i) {
    double value;
    std::memcpy(&value, source + i * sizeof(value), sizeof(value));
    value *= *state->factor;
    std::memcpy(destination + i * sizeof(value), &value, sizeof(value));
  }
  *result = status(PS_KERNEL_OK_V8);
}
ps_kernel_status_v8 prepare_kernel(void*, const ps_kernel_preparation_v8* query,
                                   ps_prepared_kernel_v8* result) {
  if (query->symbol.size != 5 || std::memcmp(query->symbol.data, "scale", 5) ||
      query->contract.size != 12 ||
      std::memcmp(query->contract.data, "scale-f64-v1", 12) ||
      query->placement.backend != PS_BACKEND_CPU_V8 ||
      query->resource_count != 1 ||
      query->resources[0].identity.byte_length != 1 || query->read_count != 1 ||
      query->write_count != 1 || query->parameter_bytes || query->scratch[0] ||
      query->scratch[1] || query->scratch[2])
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  const auto& read = query->reads[0];
  const auto& write = query->writes[0];
  if (read.port || write.port || read.member || write.member ||
      read.domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      write.domain != PS_KERNEL_DOMAIN_HOST_V8 ||
      read.layout.element_type != 3 || write.layout.element_type != 3 ||
      read.layout.rank != 1 || write.layout.rank != 1 ||
      read.layout.strides[0] != sizeof(double) ||
      write.layout.strides[0] != sizeof(double) ||
      read.layout.shape[0] != write.layout.shape[0] ||
      read.layout.origin[0] != write.layout.origin[0])
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  auto* state = new (std::nothrow)
      KernelState{static_cast<const std::uint8_t*>(query->resources[0].data)};
  if (!state)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  result->struct_size = sizeof(*result);
  result->abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result->context = state;
  result->release = [](const void* state) {
    delete static_cast<const KernelState*>(state);
  };
  result->invoke_cpu = invoke;
  return status(PS_KERNEL_OK_V8);
}
const auto operation = [] {
  ps_operation_v8 result{};
  result.struct_size = sizeof(result);
  result.key = {"fixture.resource_scale", 22};
  result.flags = PS_OPERATION_CPU_V8 | PS_OPERATION_DETERMINISTIC_V8 |
                 PS_OPERATION_PURE_V8;
  result.inputs = &port;
  result.input_count = 1;
  result.outputs = &output;
  result.output_count = 1;
  result.parameters = &parameter;
  result.parameter_count = 1;
  result.prepare = prepare;
  result.validate_resources = validate_resources;
  result.lower = lower;
  result.prepare_kernel = prepare_kernel;
  return result;
}();
void destroy(void*) {}
const ps_operation_module_v8 api = {
    sizeof(api), PS_OPERATION_MODULE_ABI_VERSION_8, &operation, 1, nullptr,
    destroy};  // NOLINT(whitespace/indent_namespace)
}  // namespace
extern "C" PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  return &api;
}
