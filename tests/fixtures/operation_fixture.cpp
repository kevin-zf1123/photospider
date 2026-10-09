#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "photospider/plugin/result_operation_plugin_api.h"

namespace {
struct Observations {
  std::uint32_t mode = 0;
  std::atomic<std::uint32_t> gpu{0}, cpu{0}, bits{UINT32_MAX}, waiting{0};
};
std::array<Observations, 15> observations;
std::atomic<std::uint32_t> destroy_count{0};
struct CancellationGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false;
} cancellation_gate;
struct State {
  bool requested = false;
};
const ps_result_region_v2 scalar_region = [] {
  ps_result_region_v2 value{};
  value.struct_size = sizeof(value);
  value.rank = 1;
  value.extent[0] = 1;
  return value;
}();
const ps_result_tensor_spec_v2 tensor = [] {
  ps_result_tensor_spec_v2 value{};
  value.struct_size = sizeof(value);
  value.key = "number";
  value.key_size = 6;
  value.element_type = PS_RESULT_ELEMENT_FLOAT64_V2;
  value.rank = 1;
  value.shape[0] = 1;
  value.channel_axis = PS_RESULT_NO_CHANNEL_V2;
  return value;
}();
const ps_result_schema_v2 schema = [] {
  ps_result_schema_v2 value{};
  value.struct_size = sizeof(value);
  value.id = "fixture.scalar";
  value.id_size = 14;
  value.version = 1;
  value.publication = 1;
  value.tensors = &tensor;
  value.tensor_count = 1;
  return value;
}();
const ps_result_port_v2 port = [] {
  ps_result_port_v2 value{};
  value.struct_size = sizeof(value);
  value.kind = PS_RESULT_OBJECT_V2;
  value.schema = &schema;
  return value;
}();
const ps_result_output_v2 output = [] {
  ps_result_output_v2 value{};
  value.struct_size = sizeof(value);
  value.key = "value";
  value.key_size = 5;
  value.port = port;
  value.input_count = UINT32_MAX;
  value.execution = PS_RESULT_WHOLE_V2;
  return value;
}();
const ps_result_parameter_descriptor_v2 scale = {
    sizeof(scale), "scale", 5, PS_RESULT_PARAMETER_FLOAT64_V2,
    1};  // NOLINT(whitespace/indent_namespace)
int resolve(void* user, const ps_result_port_v2* inputs, std::uint32_t count,
            const ps_result_parameter_value_v2*, std::uint32_t,
            const ps_result_port_v2*, std::uint32_t,
            const ps_result_metadata_sink_v2* sink) {
  if (count != 1 || !inputs[0].schema || inputs[0].schema->tensor_count != 1 ||
      inputs[0].schema->field_count)
    return 5;
  auto result_schema = *inputs[0].schema;
  auto result_tensor = result_schema.tensors[0];
  if (result_tensor.element_type != PS_RESULT_ELEMENT_FLOAT64_V2 ||
      result_tensor.rank != 1 || result_tensor.shape[0] != 1 ||
      result_tensor.batch_rank)
    return 5;
  const ps_result_facet_view_v2 invalid_facet = {
      sizeof(invalid_facet), "bad.facet", 9, 0, nullptr, 0};
  if (!user) {
    result_tensor.facets = &invalid_facet;
    result_tensor.facet_count = 1;
  }
  result_schema.tensors = &result_tensor;
  auto result = inputs[0];
  result.schema = &result_schema;
  return sink->set_output(sink->context, 0, &result);
}
int start(void* user, void* raw, const ps_result_query_v2* query,
          const ps_result_services_v2* services) {
  *static_cast<State*>(raw) = {};
  if (!user)
    return 0;
  auto& observed = *static_cast<Observations*>(user);
  observed.bits.store(UINT32_MAX);
  observed.waiting.store(0);
  if (services->cancelled(services->context))
    return 2;
  (query->backend == 2 ? observed.gpu : observed.cpu).fetch_add(1);
  if (query->backend == 2 && observed.mode >= 1 && observed.mode <= 3)
    return observed.mode == 1 ? 3 : observed.mode == 2 ? 1 : 99;
  return 0;
}
int publish(const ps_result_services_v2* services, double value, unsigned bad) {
  const ps_result_relation_row_v2 descriptor = {
      0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
  const ps_result_relation_row_v2 support = {
      0, 0, 1, PS_RESULT_TARGET_TENSOR_V2, 0, 0, 1};
  // The extra byte exercises the Result publication's exact payload envelope.
  std::array<std::uint8_t, 9> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(value));
  if (services->begin_result(services->context) ||
      services->bind_descriptor(services->context, &descriptor, 1, 1))
    return 1;
  const int written = services->publish_tensor(
      services->context, bad == 1 ? 99 : 0, &scalar_region, bytes.data(),
      bad == 2 ? 9 : 8, &support, 1, 1, PS_RESULT_FINAL_V2);
  return written ? written : services->publish_result(services->context, 1);
}
int poll(void* user, void* raw, const ps_result_query_v2* query,
         const ps_result_services_v2* services) {
  auto& state = *static_cast<State*>(raw);
  auto& observed = *static_cast<Observations*>(user);
  if (!state.requested) {
    state.requested = true;
    return services->need_tensor(services->context, 0, 0, 9, &scalar_region, 1)
               ? 1
               : PS_RESULT_NEED_V2;
  }
  std::uint64_t coordinate = 0;
  double number = 0;
  if (services->read_tensor(services->context, 0, 0, &coordinate, 1, &number,
                            sizeof(number)))
    return 1;
  if (observed.mode == 0) {
    const auto& spec = query->inputs[0].schema->tensors[0];
    if (spec.facet_count != 1 || spec.facets[0].key_size != 13 ||
        std::memcmp(spec.facets[0].key, "test.semantic", 13) != 0)
      return 1;
    number *= query->parameters[0].float64_value;
  }
  if (observed.mode == 14) {
    static_cast<void>(publish(services, number, 2));
    return PS_RESULT_PUBLISH_V2;
  }
  if (observed.mode >= 6) {
    const int first = observed.mode == 11
                          ? services->publish_result(nullptr, 1)
                          : publish(services, number, observed.mode == 7);
    const int second = observed.mode == 11
                           ? publish(services, number, false)
                           : services->publish_result(services->context, 1);
    observed.bits.store((first == 0 ? 2U : 0U) | (second == 0 ? 1U : 0U));
    if (observed.mode == 8)
      return 3;
    if (observed.mode == 9)
      return 1;
    if (observed.mode == 10)
      return 99;
    if (observed.mode == 13)
      return 2;
    if (observed.mode == 12) {
      observed.waiting.store(1, std::memory_order_release);
      std::unique_lock<std::mutex> lock(cancellation_gate.mutex);
      cancellation_gate.entered = true;
      cancellation_gate.changed.notify_all();
      const bool released = cancellation_gate.changed.wait_for(
          lock, std::chrono::seconds(15),
          [&] { return cancellation_gate.released; });
      lock.unlock();
      const bool cancelled = services->cancelled(services->context) != 0;
      if (!released || !cancelled) {
        observed.waiting.store(0, std::memory_order_release);
        return 1;
      }
      observed.waiting.store(0, std::memory_order_release);
    }
    return PS_RESULT_PUBLISH_V2;
  }
  if (query->backend == 2 && (observed.mode == 4 || observed.mode == 5)) {
    static_cast<void>(publish(services, number, observed.mode == 5));
    return 3;
  }
  return publish(services, number, false) ? 1 : PS_RESULT_PUBLISH_V2;
}
void destroy_state(void*, void*) {}
void destroy_plugin(void*) {
  destroy_count.fetch_add(1);
}
const auto operations = [] {
  std::array<ps_result_operation_v2, 16> result{};
  const std::array<const char*, 16> keys = {
      "fixture.double",
      "fixture.bad_facet",
      "fixture.gpu_fallback",
      "fixture.gpu_failure",
      "fixture.gpu_unknown",
      "fixture.gpu_output_unavailable",
      "fixture.gpu_bad_output_unavailable",
      "fixture.duplicate_success",
      "fixture.duplicate_invalid_success",
      "fixture.gpu_duplicate_unavailable",
      "fixture.gpu_duplicate_failure",
      "fixture.gpu_duplicate_unknown",
      "fixture.null_context_then_valid",
      "fixture.duplicate_cancelled",
      "fixture.gpu_duplicate_callback_cancelled",
      "fixture.bad_bytes"};
  for (std::size_t i = 0; i < result.size(); ++i) {
    auto& op = result[i];
    op.struct_size = sizeof(op);
    op.key = keys[i];
    op.key_size = std::strlen(keys[i]);
    op.flags = PS_RESULT_FLAG_CPU_V2 | PS_RESULT_FLAG_DETERMINISTIC_V2 |
               PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2;
    op.inputs = &port;
    op.input_count = 1;
    op.outputs = &output;
    op.output_count = 1;
    op.state_bytes = sizeof(State);
    op.maximum_stages = 3;
    op.resolve_metadata = resolve;
    op.start = start;
    op.poll = poll;
    op.destroy = destroy_state;
    if (i != 1) {
      const auto mode = i ? i - 1 : 0;
      observations[mode].mode = mode;
      op.user_data = &observations[mode];
    }
    if (i >= 2 && i <= 14)
      op.flags |= PS_RESULT_FLAG_GPU_V2 | PS_RESULT_FLAG_CPU_FALLBACK_V2;
    if (!i) {
      op.parameters = &scale;
      op.parameter_count = 1;
    }
  }
  return result;
}();
const ps_result_operation_plugin_api_v2 api = {
    sizeof(api),       PS_RESULT_OPERATION_ABI_VERSION_2,
    operations.data(), operations.size(),
    nullptr,           destroy_plugin};  // NOLINT(whitespace/indent_namespace)
}  // namespace
extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}
extern "C" PS_RESULT_EXPORT std::uint32_t ps_operation_fixture_destroy_count() {
  return destroy_count.load();
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_fixture_gpu_invocation_count(std::uint32_t mode) {
  return mode < observations.size() ? observations[mode].gpu.load() : 0;
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_fixture_cpu_invocation_count(std::uint32_t mode) {
  return mode < observations.size() ? observations[mode].cpu.load() : 0;
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_fixture_publish_result_bits(std::uint32_t mode) {
  return mode < observations.size() ? observations[mode].bits.load()
                                    : UINT32_MAX;
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_fixture_awaiting_cancellation(std::uint32_t mode) {
  return mode < observations.size()
             ? observations[mode].waiting.load(std::memory_order_acquire)
             : 0;
}

// Private fixture synchronization; these symbols are outside the operation ABI.
extern "C" PS_RESULT_EXPORT void ps_operation_fixture_arm_cancellation_gate() {
  std::lock_guard<std::mutex> lock(cancellation_gate.mutex);
  cancellation_gate.entered = false;
  cancellation_gate.released = false;
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_fixture_wait_cancellation_entered(std::uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(cancellation_gate.mutex);
  return cancellation_gate.changed.wait_for(
             lock, std::chrono::milliseconds(timeout_ms),
             [&] { return cancellation_gate.entered; })
             ? 1U
             : 0U;
}
extern "C" PS_RESULT_EXPORT void
ps_operation_fixture_release_cancellation_gate() {
  std::lock_guard<std::mutex> lock(cancellation_gate.mutex);
  cancellation_gate.released = true;
  cancellation_gate.changed.notify_all();
}
