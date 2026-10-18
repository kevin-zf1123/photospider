#include <array>
#include <cstdint>
#include <cstring>

#include "photospider/plugin/result_operation_plugin_api.h"
#include "photospider/plugin/result_tile_helpers.h"

namespace {
const auto tensor = [] {
  ps_result_tensor_spec_v7 t{};
  t.struct_size = sizeof(t);
  t.key = "samples";
  t.key_size = 7;
  t.element_type = PS_RESULT_ELEMENT_FLOAT64_V7;
  t.rank = 1;
  t.shape[0] = 4;
  t.channel_axis = PS_RESULT_NO_CHANNEL_V7;
  return t;
}();
const ps_result_schema_v7 schema = {sizeof(schema),
                                    "fixture.vector",
                                    14,
                                    1,
                                    &tensor,
                                    1,
                                    nullptr,
                                    0};  // NOLINT(whitespace/indent_namespace)
const auto port = [] {
  ps_result_port_v7 p{};
  p.struct_size = sizeof(p);
  p.schema = &schema;
  return p;
}();
const auto output = [] {
  ps_result_output_v7 o{};
  o.struct_size = sizeof(o);
  o.key = "value";
  o.key_size = 5;
  o.port = port;
  o.input_count = UINT32_MAX;
  return o;
}();
const ps_result_parameter_descriptor_v7 parameter = {
    sizeof(parameter), "sha256", 6, PS_RESULT_PARAMETER_STRING_V7,
    1};  // NOLINT(whitespace/indent_namespace)
int nibble(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
int resolve(void*, const ps_result_port_v7*, std::uint32_t,
            const ps_result_parameter_value_v7* p, std::uint32_t n,
            const ps_result_port_v7*, std::uint32_t,
            const ps_result_metadata_sink_v7* sink) {
  if (n != 1 || p->string_size != 64)
    return 1;
  ps_result_resource_identity_v7 id{};
  id.byte_length = 1;
  for (unsigned i = 0; i < 32; ++i) {
    int a = nibble(p->string_value[2 * i]),
        b = nibble(p->string_value[2 * i + 1]);
    if (a < 0 || b < 0)
      return 1;
    id.sha256[i] = static_cast<std::uint8_t>(a * 16 + b);
  }
  if (sink->set_resources(sink->context, &id, 1))
    return 1;
  return sink->set_output(sink->context, 0, &port);
}
int start(void*, void* state, const ps_result_query_v7*,
          const ps_result_services_v7*) {
  *static_cast<bool*>(state) = false;
  return 0;
}
int poll(void*, void* state, const ps_result_query_v7* query,
         const ps_result_services_v7* services) {
  if (!*static_cast<bool*>(state)) {
    *static_cast<bool*>(state) = true;
    return ps_result_to_whole_v7(services, query, 0) ? 1 : PS_RESULT_NEED_V7;
  }
  if (query->resource_count != 1 ||
      query->resources[0].identity.byte_length != 1)
    return 1;
  ps_result_region_v7 region{};
  if (ps_result_output_region_v7(query, 0, &region))
    return 1;
  std::array<double, 4> values{};
  for (std::uint64_t i = 0; i < region.extent[0]; ++i) {
    auto coordinate = region.offset[0] + i;
    if (services->read_tensor(services->context, 0, 0, &coordinate, 1,
                              &values[i], sizeof(double)))
      return 1;
    values[i] *= query->resources[0].bytes[0];
  }
  if (services->begin_result(services->context) ||
      services->publish_tensor(
          services->context, 0, &region,
          reinterpret_cast<const std::uint8_t*>(values.data()),
          region.extent[0] * sizeof(double)))
    return 1;
  return services->publish_result(services->context, 1) ? 1
                                                        : PS_RESULT_PUBLISH_V7;
}
void destroy(void*, void*) {}
const auto operation = [] {
  ps_result_operation_v7 o{};
  o.struct_size = sizeof(o);
  o.key = "fixture.resource_scale";
  o.key_size = 22;
  o.flags = PS_RESULT_FLAG_CPU_V7 | PS_RESULT_FLAG_DETERMINISTIC_V7 |
            PS_RESULT_FLAG_SIDE_EFFECT_FREE_V7;
  o.inputs = &port;
  o.input_count = 1;
  o.outputs = &output;
  o.output_count = 1;
  o.parameters = &parameter;
  o.parameter_count = 1;
  o.state_bytes = sizeof(bool);
  o.maximum_stages = 3;
  o.resolve_metadata = resolve;
  o.start = start;
  o.poll = poll;
  o.destroy = destroy;
  return o;
}();
void destroy_plugin(void*) {}
const ps_result_operation_plugin_api_v7 api = {
    sizeof(api),   PS_RESULT_OPERATION_ABI_VERSION_7, &operation, 1, nullptr,
    destroy_plugin};  // NOLINT(whitespace/indent_namespace)
}  // namespace
extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v7*
ps_result_operation_plugin_get_api_v7(void) {
  return &api;
}
