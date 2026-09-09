#include <cstddef>
#include <cstdint>

#include "photospider/plugin/operation_plugin_api.h"

namespace {
ps_operation_port_constraint_v6 port = {
    sizeof(ps_operation_port_constraint_v6), PS_OPERATION_PORT_VALUE_V6, 0,
    0};  // NOLINT(whitespace/indent_namespace)
alignas(ps_operation_port_constraint_v6) unsigned char misaligned[32]{};
int unreachable(void*, const ps_operation_value_view_v6*, std::uint32_t,
                const ps_operation_parameter_value_v6*, std::uint32_t,
                std::uint32_t, ps_operation_cancelled_v6, void*,
                const ps_operation_output_sink_v6*, char*, std::size_t) {
  return PS_OPERATION_RESULT_FAILURE_V6;
}
void destroy(const ps_operation_descriptor_v6*, std::uint32_t) {}
ps_operation_descriptor_v6 descriptors[2]{};
ps_operation_plugin_api_v6 api{};
}  // namespace
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_6;
}
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v6*
ps_operation_plugin_get_api_v6(void) {
  descriptors[0] = {sizeof(ps_operation_descriptor_v6),
                    "valid.prefix",
                    12,
                    0,
                    PS_OPERATION_FLAG_CPU,
                    0,
                    PS_OPERATION_ELEMENT_FLOAT64_V6,
                    0,
                    nullptr,
                    PS_OPERATION_SHAPE_SCALAR_V6,
                    PS_OPERATION_REGION_WHOLE_V6,
                    0,
                    0,
                    0,
                    nullptr,
                    0,
                    nullptr,
                    {sizeof(ps_operation_port_constraint_v6),
                     PS_OPERATION_PORT_VALUE_V6, 0, 0},
                    unreachable,
                    nullptr};
  descriptors[1] = descriptors[0];
  descriptors[1].key = "invalid.port";
  descriptors[1].input_count = 1;
  descriptors[1].input_schema_count = 1;
  descriptors[1].input_schema = &port;
#if PS_BAD_PORT_CASE == 1
  descriptors[1].input_schema_count = 0;
#elif PS_BAD_PORT_CASE == 2
  descriptors[1].input_schema = nullptr;
#elif PS_BAD_PORT_CASE == 3
  descriptors[1].input_schema =
      reinterpret_cast<const ps_operation_port_constraint_v6*>(misaligned + 1);
#elif PS_BAD_PORT_CASE == 4
  port.struct_size -= 1;
#elif PS_BAD_PORT_CASE == 5
  port.kind = 999;
#elif PS_BAD_PORT_CASE == 6
  port.minimum_bits = 0x80000000U;
#elif PS_BAD_PORT_CASE == 7
  port.kind = PS_OPERATION_PORT_FLOAT32_SCALAR_V6;
  port.maximum_bits = 0x7f800000U;
#elif PS_BAD_PORT_CASE == 8
  descriptors[1].output_schema.struct_size -= 1;
#endif
  api = {sizeof(ps_operation_plugin_api_v6), 2, descriptors, destroy};
  return &api;
}
