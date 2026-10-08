#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include "photospider/plugin/result_operation_plugin_api.h"

namespace {
std::atomic<std::uint64_t> starts{0}, destroys{0}, polls{0}, singles{0};
struct Payload {
  std::int64_t mode;
};
int start_single(void*, void*, const ps_result_query_v2*,
                 const ps_result_services_v2*) {
  ++singles;
  return 0;
}
int poll_single(void*, void*, const ps_result_query_v2* query,
                const ps_result_services_v2* service) {
  ps_result_region_v2 region{};
  region.struct_size = sizeof(region);
  region.rank = 1;
  region.extent[0] = 1;
  const double value = 7 + 4 * query->output_index;
  if (service->begin_result(service->context) ||
      service->bind_descriptor(service->context, nullptr, 0,
                               PS_RESULT_EXACT_V2) ||
      service->publish_tensor(service->context, 0, &region,
                              reinterpret_cast<const std::uint8_t*>(&value),
                              sizeof(value), nullptr, 0, PS_RESULT_EXACT_V2,
                              PS_RESULT_FINAL_V2) ||
      service->publish_result(service->context, 1))
    return PS_RESULT_STATUS_FAILURE_V2;
  return PS_RESULT_PUBLISH_V2;
}
void destroy_single(void*, void*) {}
int start_joint(const ps_result_joint_query_v2* query, std::uint32_t, void* raw,
                std::uint64_t, void*) {
  ++starts;
  static_cast<Payload*>(raw)->mode = query[0].query->parameters[0].int64_value;
  if (static_cast<Payload*>(raw)->mode == 5)
    throw std::runtime_error("entered C start exception");
  return 0;
}
int poll_joint(const ps_result_joint_member_v2* members, std::uint32_t count,
               void* raw, const ps_result_joint_services_v2* service,
               ps_result_joint_outcome_v2* outcomes,
               std::uint32_t* outcome_count, void*) {
  ++polls;
  const auto mode = static_cast<Payload*>(raw)->mode;
  if (mode == 6) {
    for (std::uint32_t i = 0; i < count; ++i) {
      auto result =
          poll_single(nullptr, nullptr, members[i].query, members[i].services);
      outcomes[i] = {};
      outcomes[i].struct_size = sizeof(outcomes[i]);
      outcomes[i].key = members[i].key;
      outcomes[i].result = result;
    }
    const_cast<ps_result_query_v2*>(members[0].query)->output_index = 63;
    outcomes[0].key.output_index = 63;
    *outcome_count = count;
    return 0;
  }
  if (mode <= 1) {
    static_cast<void>(service->scratch(service->context, 1, nullptr));
    if (mode == 1) {
      double ignored = 0;
      const std::uint64_t at = 0;
      static_cast<void>(
          members[0].services->read_tensor(members[0].services->context, 99, 0,
                                           &at, 1, &ignored, sizeof(ignored)));
      return PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V2;
    }
  } else if (mode == 4) {
    std::thread invalid_thread(
        [&] { static_cast<void>(service->consume_work(service->context, 1)); });
    invalid_thread.join();
    return PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V2;
  } else if (mode == 2) {
    static_cast<void>(service->consume_work(service->context, 1000000));
  } else {
    std::uint8_t* ignored = nullptr;
    static_cast<void>(service->scratch(service->context, 128, &ignored));
  }
  throw std::runtime_error("later C callback exception");
}
void destroy_joint(void*, void*) {
  ++destroys;
}
void destroy_plugin(void*) {}
struct Table {
  ps_result_tensor_spec_v2 tensor{};
  ps_result_schema_v2 schema{};
  ps_result_output_v2 outputs[2]{};
  ps_result_parameter_descriptor_v2 mode{};
  ps_result_joint_program_v2 joint{};
  ps_result_operation_v2 operation{};
  ps_result_operation_plugin_api_v2 api{};
  Table() {
    tensor.struct_size = sizeof(tensor);
    tensor.key = "number";
    tensor.key_size = 6;
    tensor.element_type = PS_RESULT_ELEMENT_FLOAT64_V2;
    tensor.rank = 1;
    tensor.shape[0] = 1;
    tensor.channel_axis = PS_RESULT_NO_CHANNEL_V2;
    schema.struct_size = sizeof(schema);
    schema.id = "test.multi_output";
    schema.id_size = 17;
    schema.version = 1;
    schema.publication = PS_RESULT_COMPLETE_BUNDLE_V2;
    schema.tensors = &tensor;
    schema.tensor_count = 1;
    for (unsigned i = 0; i < 2; ++i) {
      outputs[i].struct_size = sizeof(outputs[i]);
      outputs[i].key = i ? "right" : "left";
      outputs[i].key_size = i ? 5 : 4;
      outputs[i].port.struct_size = sizeof(ps_result_port_v2);
      outputs[i].port.kind = PS_RESULT_OBJECT_V2;
      outputs[i].port.schema = &schema;
      outputs[i].execution = PS_RESULT_REGIONAL_V2;
    }
    mode = {sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V2, 0, 1, 0, 6};
    joint = {sizeof(joint),
             1,
             sizeof(Payload),
             64,
             start_joint,
             poll_joint,
             destroy_joint,
             sizeof(ps_result_joint_query_v2),
             sizeof(ps_result_joint_member_v2),
             sizeof(ps_result_joint_outcome_v2),
             sizeof(ps_result_joint_services_v2)};
    operation.struct_size = sizeof(operation);
    operation.key = "fixture.result_joint_throw";
    operation.key_size = 26;
    operation.flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
                      PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2;
    operation.outputs = outputs;
    operation.output_count = 2;
    operation.parameters = &mode;
    operation.parameter_count = 1;
    operation.state_bytes = 1;
    operation.maximum_stages = 2;
    operation.start = start_single;
    operation.poll = poll_single;
    operation.destroy = destroy_single;
    operation.joint = &joint;
    api = {sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_2,
           &operation,  1,
           nullptr,     destroy_plugin};
  }
};
Table table;
}  // namespace
extern "C" PS_RESULT_EXPORT void ps_result_joint_counts(std::uint64_t* counts) {
  counts[0] = starts;
  counts[1] = destroys;
  counts[2] = polls;
  counts[3] = singles;
  counts[4] = counts[5] = 0;
}
extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2() {
  return &table.api;
}
