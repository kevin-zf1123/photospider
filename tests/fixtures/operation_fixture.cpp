#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "photospider/plugin/result_operation_plugin_api.h"
#include "photospider/plugin/result_tile_helpers.h"

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
const ps_result_region_v6 scalar_region = [] {
  ps_result_region_v6 value{};
  value.struct_size = sizeof(value);
  value.rank = 1;
  value.extent[0] = 1;
  return value;
}();
const ps_result_tensor_spec_v6 tensor = [] {
  ps_result_tensor_spec_v6 value{};
  value.struct_size = sizeof(value);
  value.key = "number";
  value.key_size = 6;
  value.element_type = PS_RESULT_ELEMENT_FLOAT64_V6;
  value.rank = 1;
  value.shape[0] = 1;
  value.channel_axis = PS_RESULT_NO_CHANNEL_V6;
  return value;
}();
const ps_result_schema_v6 schema = [] {
  ps_result_schema_v6 value{};
  value.struct_size = sizeof(value);
  value.id = "fixture.scalar";
  value.id_size = 14;
  value.version = 1;
  value.tensors = &tensor;
  value.tensor_count = 1;
  return value;
}();
const ps_result_port_v6 port = [] {
  ps_result_port_v6 value{};
  value.struct_size = sizeof(value);
  value.schema = &schema;
  return value;
}();
const ps_result_output_v6 output = [] {
  ps_result_output_v6 value{};
  value.struct_size = sizeof(value);
  value.key = "value";
  value.key_size = 5;
  value.port = port;
  value.input_count = UINT32_MAX;
  return value;
}();
const ps_result_parameter_descriptor_v6 scale = {
    sizeof(scale), "scale", 5, PS_RESULT_PARAMETER_FLOAT64_V6,
    1};  // NOLINT(whitespace/indent_namespace)
int resolve(void* user, const ps_result_port_v6* inputs, std::uint32_t count,
            const ps_result_parameter_value_v6*, std::uint32_t,
            const ps_result_port_v6*, std::uint32_t,
            const ps_result_metadata_sink_v6* sink) {
  if (count != 1 || !inputs[0].schema || inputs[0].schema->tensor_count != 1)
    return 5;
  auto result_schema = *inputs[0].schema;
  auto result_tensor = result_schema.tensors[0];
  if (result_tensor.element_type != PS_RESULT_ELEMENT_FLOAT64_V6 ||
      result_tensor.rank != 1 || result_tensor.shape[0] != 1 ||
      result_tensor.batch_rank)
    return 5;
  if (!user)
    result_tensor.atomic_trailing_axes = 2;
  result_schema.tensors = &result_tensor;
  auto result = inputs[0];
  result.schema = &result_schema;
  return sink->set_output(sink->context, 0, &result);
}
int start(void* user, void* raw, const ps_result_query_v6* query,
          const ps_result_services_v6* services) {
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
int publish(const ps_result_services_v6* services, double value, unsigned bad) {
  // The extra byte exercises the Result publication's exact payload envelope.
  std::array<std::uint8_t, 9> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(value));
  if (services->begin_result(services->context))
    return 1;
  const int written =
      services->publish_tensor(services->context, bad == 1 ? 99 : 0,
                               &scalar_region, bytes.data(), bad == 2 ? 9 : 8);
  return written ? written : services->publish_result(services->context, 1);
}
int poll(void* user, void* raw, const ps_result_query_v6* query,
         const ps_result_services_v6* services) {
  auto& state = *static_cast<State*>(raw);
  auto& observed = *static_cast<Observations*>(user);
  if (!state.requested) {
    state.requested = true;
    return ps_result_to_whole_v6(services, query, 0) ? 1 : PS_RESULT_NEED_V6;
  }
  std::uint64_t coordinate = 0;
  double number = 0;
  if (services->read_tensor(services->context, 0, 0, &coordinate, 1, &number,
                            sizeof(number)))
    return 1;
  if (observed.mode == 0) {
    const auto& spec = query->inputs[0].schema->tensors[0];
    if (spec.atomic_trailing_axes != 1)
      return 1;
    number *= query->parameters[0].float64_value;
  }
  if (observed.mode == 14) {
    static_cast<void>(publish(services, number, 2));
    return PS_RESULT_PUBLISH_V6;
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
    return PS_RESULT_PUBLISH_V6;
  }
  if (query->backend == 2 && (observed.mode == 4 || observed.mode == 5)) {
    static_cast<void>(publish(services, number, observed.mode == 5));
    return 3;
  }
  return publish(services, number, false) ? 1 : PS_RESULT_PUBLISH_V6;
}
void destroy_state(void*, void*) {}
void destroy_plugin(void*) {
  destroy_count.fetch_add(1);
}
const auto operations = [] {
  std::array<ps_result_operation_v6, 16> result{};
  const std::array<const char*, 16> keys = {
      "fixture.double",
      "fixture.bad_atomic_axis",
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
    op.flags = PS_RESULT_FLAG_CPU_V6 | PS_RESULT_FLAG_DETERMINISTIC_V6 |
               PS_RESULT_FLAG_SIDE_EFFECT_FREE_V6;
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
      op.flags |= PS_RESULT_FLAG_GPU_V6 | PS_RESULT_FLAG_CPU_FALLBACK_V6;
    if (!i) {
      op.parameters = &scale;
      op.parameter_count = 1;
    }
  }
  return result;
}();
const ps_result_operation_plugin_api_v6 api = {
    sizeof(api),       PS_RESULT_OPERATION_ABI_VERSION_6,
    operations.data(), operations.size(),
    nullptr,           destroy_plugin};  // NOLINT(whitespace/indent_namespace)
}  // namespace
extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v6*
ps_result_operation_plugin_get_api_v6(void) {
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
