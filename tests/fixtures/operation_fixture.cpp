#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "photospider/plugin/operation_plugin_api.h"

namespace {

/** @brief Number of exact destroy callbacks observed by this fixture image. */
std::atomic<std::uint32_t> destroy_count{0U};

/** @brief Fixture mode that requests recoverable GPU backend fallback. */
constexpr std::uint32_t kGpuBackendUnavailable = 1U;
/** @brief Fixture mode that reports an ordinary nonrecoverable GPU failure. */
constexpr std::uint32_t kGpuOrdinaryFailure = 2U;
/** @brief Fixture mode that reports an unknown nonzero callback result. */
constexpr std::uint32_t kGpuUnknownResult = 3U;
/** @brief Fixture mode that publishes valid output before unavailability. */
constexpr std::uint32_t kGpuOutputThenUnavailable = 4U;
/** @brief Fixture mode that publishes malformed output before unavailability.
 */
constexpr std::uint32_t kGpuBadOutputThenUnavailable = 5U;

/**
 * @brief Owns one GPU-result fixture mode and exact invocation counters.
 *
 * @note The descriptor borrows this process-lifetime state. Atomic counters
 * permit the host's CPU and GPU lanes to update independently.
 */
struct GpuResultState final {
  /**
   * @brief Constructs zeroed counters for one immutable fixture mode.
   * @param mode_value Closed test mode in 1..5.
   * @throws Nothing.
   */
  explicit GpuResultState(std::uint32_t mode_value) noexcept
      : mode(mode_value) {}

  /** @brief Immutable callback behavior selector. */
  std::uint32_t mode;
  /** @brief Number of GPU callback entries. */
  std::atomic<std::uint32_t> gpu_invocations{0U};
  /** @brief Number of CPU callback entries. */
  std::atomic<std::uint32_t> cpu_invocations{0U};
};

/** @brief State for ordinary no-output backend unavailability. */
GpuResultState gpu_backend_unavailable{kGpuBackendUnavailable};
/** @brief State for ordinary GPU failure. */
GpuResultState gpu_ordinary_failure{kGpuOrdinaryFailure};
/** @brief State for an unknown GPU callback result. */
GpuResultState gpu_unknown_result{kGpuUnknownResult};
/** @brief State for valid output followed by backend unavailability. */
GpuResultState gpu_output_then_unavailable{kGpuOutputThenUnavailable};
/** @brief State for invalid output followed by backend unavailability. */
GpuResultState gpu_bad_output_then_unavailable{kGpuBadOutputThenUnavailable};

/**
 * @brief Looks up process-lifetime GPU fixture state by exported mode.
 * @param mode Closed test mode in 1..5.
 * @return Matching state or null for an unknown mode.
 * @throws Nothing.
 */
GpuResultState* find_gpu_result_state(std::uint32_t mode) noexcept {
  switch (mode) {
    case kGpuBackendUnavailable:
      return &gpu_backend_unavailable;
    case kGpuOrdinaryFailure:
      return &gpu_ordinary_failure;
    case kGpuUnknownResult:
      return &gpu_unknown_result;
    case kGpuOutputThenUnavailable:
      return &gpu_output_then_unavailable;
    case kGpuBadOutputThenUnavailable:
      return &gpu_bad_output_then_unavailable;
    default:
      return nullptr;
  }
}

/**
 * @brief Doubles one contiguous Float64 scalar input.
 * @param user_data Unused fixture state.
 * @param inputs Exact one-element input array.
 * @param input_count Must equal one.
 * @param parameters Exact required `scale` Float64 parameter.
 * @param parameter_count Must equal one.
 * @param backend Must name CPU for this fixture.
 * @param cancelled Host cancellation observer.
 * @param cancellation_context Host cancellation state.
 * @param sink Required single-publication sink.
 * @param diagnostic Writable failure diagnostic.
 * @param diagnostic_capacity Diagnostic buffer capacity.
 * @return One closed `ps_operation_result_v2` value.
 * @throws Nothing.
 * @note No pointer is retained after return.
 */
int execute_double(void* user_data, const ps_operation_value_view_v2* inputs,
                   std::uint32_t input_count,
                   const ps_operation_parameter_value_v2* parameters,
                   std::uint32_t parameter_count, std::uint32_t backend,
                   ps_operation_cancelled_v2 cancelled,
                   void* cancellation_context,
                   const ps_operation_output_sink_v2* sink, char* diagnostic,
                   std::size_t diagnostic_capacity) {
  static_cast<void>(user_data);
  if (!inputs || input_count != 1U || !parameters || parameter_count != 1U ||
      parameters[0].struct_size != sizeof(ps_operation_parameter_value_v2) ||
      parameters[0].key_size != 5U ||
      std::memcmp(parameters[0].key, "scale", 5U) != 0 ||
      parameters[0].type != PS_OPERATION_PARAMETER_FLOAT64_V2 ||
      backend != 1U || !sink || !sink->publish ||
      inputs[0].element_type != PS_OPERATION_ELEMENT_FLOAT64_V2 ||
      inputs[0].rank != 1U || inputs[0].byte_size != sizeof(double) ||
      !inputs[0].shape || inputs[0].shape[0] != 1U ||
      !inputs[0].demand_offsets || inputs[0].demand_offsets[0] != 0U ||
      !inputs[0].demand_extents || inputs[0].demand_extents[0] != 1U ||
      !inputs[0].data || inputs[0].facet_count != 1U || !inputs[0].facets ||
      inputs[0].facets[0].struct_size != sizeof(ps_operation_facet_view_v2) ||
      inputs[0].facets[0].key_size != 13U ||
      std::memcmp(inputs[0].facets[0].key, "test.semantic", 13U) != 0 ||
      (cancelled && cancelled(cancellation_context))) {
    if (diagnostic && diagnostic_capacity > 0U) {
      diagnostic[0] = '\0';
    }
    return PS_OPERATION_RESULT_FAILURE_V2;
  }
  double value = 0.0;
  std::memcpy(&value, inputs[0].data, sizeof(value));
  value *= parameters[0].float64_value;
  const std::uint64_t shape[] = {1U};
  return sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT64_V2, shape,
                       1U, inputs[0].facets, inputs[0].facet_count,
                       reinterpret_cast<const std::uint8_t*>(&value),
                       sizeof(value))
             ? PS_OPERATION_RESULT_SUCCESS_V2
             : PS_OPERATION_RESULT_FAILURE_V2;
}

/**
 * @brief Attempts one output with an invalid zero-version facet.
 * @param user_data Unused fixture state.
 * @param inputs Exact one-element input array.
 * @param input_count Must equal one.
 * @param parameters Empty parameter array.
 * @param parameter_count Must equal zero.
 * @param backend Must name CPU for this fixture.
 * @param cancelled Host cancellation observer.
 * @param cancellation_context Host cancellation state.
 * @param sink Required single-publication sink.
 * @param diagnostic Writable failure diagnostic.
 * @param diagnostic_capacity Diagnostic buffer capacity.
 * @return `PS_OPERATION_RESULT_SUCCESS_V2` after the sink rejects the malformed
 * facet so the host surfaces the sink validation failure.
 * @throws Nothing.
 * @note This fixture proves host-side facet validation, not plugin behavior.
 */
int execute_bad_facet(void* user_data, const ps_operation_value_view_v2* inputs,
                      std::uint32_t input_count,
                      const ps_operation_parameter_value_v2* parameters,
                      std::uint32_t parameter_count, std::uint32_t backend,
                      ps_operation_cancelled_v2 cancelled,
                      void* cancellation_context,
                      const ps_operation_output_sink_v2* sink, char* diagnostic,
                      std::size_t diagnostic_capacity) {
  static_cast<void>(user_data);
  static_cast<void>(parameters);
  static_cast<void>(cancelled);
  static_cast<void>(cancellation_context);
  if (!inputs || input_count != 1U || parameter_count != 0U || backend != 1U ||
      !sink || !sink->publish) {
    return PS_OPERATION_RESULT_FAILURE_V2;
  }
  if (diagnostic && diagnostic_capacity > 0U) {
    diagnostic[0] = '\0';
  }
  const char key[] = "bad.facet";
  const ps_operation_facet_view_v2 facet = {
      sizeof(ps_operation_facet_view_v2),
      key,
      static_cast<std::uint32_t>(sizeof(key) - 1U),
      0U,
      nullptr,
      0U};
  static_cast<void>(sink->publish(sink->context, inputs[0].element_type,
                                  inputs[0].shape, inputs[0].rank, &facet, 1U,
                                  inputs[0].data, inputs[0].byte_size));
  return PS_OPERATION_RESULT_SUCCESS_V2;
}

/**
 * @brief Copies one bounded fixture diagnostic into the host buffer.
 * @param message Null-terminated static fixture text.
 * @param diagnostic Writable host buffer, possibly null.
 * @param diagnostic_capacity Buffer size including the terminator.
 * @throws Nothing.
 * @note The helper always terminates a nonempty destination buffer.
 */
void write_diagnostic(const char* message, char* diagnostic,
                      std::size_t diagnostic_capacity) noexcept {
  if (!diagnostic || diagnostic_capacity == 0U) {
    return;
  }
  const std::size_t message_size = std::strlen(message);
  const std::size_t copy_size = message_size < diagnostic_capacity - 1U
                                    ? message_size
                                    : diagnostic_capacity - 1U;
  std::memcpy(diagnostic, message, copy_size);
  diagnostic[copy_size] = '\0';
}

/**
 * @brief Exercises recoverable and ordinary GPU callback result categories.
 * @param user_data Pointer to one static fixture state.
 * @param inputs Exact one-element Float64 input array.
 * @param input_count Must equal one.
 * @param parameters Empty parameter array.
 * @param parameter_count Must equal zero.
 * @param backend One for CPU or two for optional local GPU.
 * @param cancelled Host cancellation observer.
 * @param cancellation_context Host cancellation state.
 * @param sink Required single-publication sink.
 * @param diagnostic Writable failure diagnostic.
 * @param diagnostic_capacity Diagnostic buffer capacity.
 * @return One closed `ps_operation_result_v2` value, or intentional `99` for
 * the host's unknown-result fail-closed regression.
 * @throws Nothing.
 * @note Modes four and five intentionally violate the no-output backend-
 * unavailable contract; CPU always publishes the copied input if reached.
 */
int execute_gpu_result(void* user_data,
                       const ps_operation_value_view_v2* inputs,
                       std::uint32_t input_count,
                       const ps_operation_parameter_value_v2* parameters,
                       std::uint32_t parameter_count, std::uint32_t backend,
                       ps_operation_cancelled_v2 cancelled,
                       void* cancellation_context,
                       const ps_operation_output_sink_v2* sink,
                       char* diagnostic,
                       std::size_t diagnostic_capacity) noexcept {
  static_cast<void>(parameters);
  auto* state = static_cast<GpuResultState*>(user_data);
  if (!state || !inputs || input_count != 1U || parameter_count != 0U ||
      (backend != 1U && backend != 2U) || !sink || !sink->publish ||
      inputs[0].element_type != PS_OPERATION_ELEMENT_FLOAT64_V2 ||
      inputs[0].rank != 1U || inputs[0].byte_size != sizeof(double) ||
      !inputs[0].shape || inputs[0].shape[0] != 1U || !inputs[0].data) {
    write_diagnostic("fixture invocation is malformed", diagnostic,
                     diagnostic_capacity);
    return PS_OPERATION_RESULT_FAILURE_V2;
  }
  if (cancelled && cancelled(cancellation_context)) {
    write_diagnostic("fixture invocation was cancelled", diagnostic,
                     diagnostic_capacity);
    return PS_OPERATION_RESULT_CANCELLED_V2;
  }
  if (backend == 2U) {
    state->gpu_invocations.fetch_add(1U, std::memory_order_relaxed);
    if (state->mode == kGpuBackendUnavailable) {
      write_diagnostic("fixture GPU backend is unavailable", diagnostic,
                       diagnostic_capacity);
      return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V2;
    }
    if (state->mode == kGpuOrdinaryFailure) {
      write_diagnostic("fixture GPU operation failed", diagnostic,
                       diagnostic_capacity);
      return PS_OPERATION_RESULT_FAILURE_V2;
    }
    if (state->mode == kGpuOutputThenUnavailable) {
      static_cast<void>(sink->publish(sink->context, inputs[0].element_type,
                                      inputs[0].shape, inputs[0].rank,
                                      inputs[0].facets, inputs[0].facet_count,
                                      inputs[0].data, inputs[0].byte_size));
      write_diagnostic("fixture published output before unavailability",
                       diagnostic, diagnostic_capacity);
      return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V2;
    }
    if (state->mode == kGpuBadOutputThenUnavailable) {
      const char key[] = "bad.facet";
      const ps_operation_facet_view_v2 facet = {
          sizeof(ps_operation_facet_view_v2),
          key,
          static_cast<std::uint32_t>(sizeof(key) - 1U),
          0U,
          nullptr,
          0U};
      static_cast<void>(sink->publish(sink->context, inputs[0].element_type,
                                      inputs[0].shape, inputs[0].rank, &facet,
                                      1U, inputs[0].data, inputs[0].byte_size));
      write_diagnostic("fixture published invalid output before unavailability",
                       diagnostic, diagnostic_capacity);
      return PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V2;
    }
    write_diagnostic("fixture GPU returned an unknown result", diagnostic,
                     diagnostic_capacity);
    return 99;
  }
  state->cpu_invocations.fetch_add(1U, std::memory_order_relaxed);
  return sink->publish(sink->context, inputs[0].element_type, inputs[0].shape,
                       inputs[0].rank, inputs[0].facets, inputs[0].facet_count,
                       inputs[0].data, inputs[0].byte_size)
             ? PS_OPERATION_RESULT_SUCCESS_V2
             : PS_OPERATION_RESULT_FAILURE_V2;
}

/**
 * @brief Records exact release of the static descriptor table.
 * @param operations Original fixture descriptor pointer.
 * @param operation_count Original fixture descriptor count.
 * @throws Nothing.
 * @note The records are static and require no allocation release.
 */
void destroy_fixture(const ps_operation_descriptor_v2* operations,
                     std::uint32_t operation_count) {
  if (operations && operation_count == 7U) {
    destroy_count.fetch_add(1U, std::memory_order_relaxed);
  }
}

/**
 * @brief Builds the required Float64 `scale` declaration.
 * @return Valid required parameter descriptor.
 * @throws Nothing.
 */
ps_operation_parameter_descriptor_v2 make_double_parameter() noexcept {
  return {sizeof(ps_operation_parameter_descriptor_v2), "scale", 5U,
          PS_OPERATION_PARAMETER_FLOAT64_V2, 1U};
}

/** @brief Required Float64 `scale` schema for `fixture.double`. */
const auto double_parameter = make_double_parameter();

/**
 * @brief Builds all valid fixture operation descriptors.
 * @return Contiguous double, bad-facet, and GPU-result descriptor table.
 * @throws Nothing.
 * @note Every referenced callback and parameter record has static lifetime.
 */
std::array<ps_operation_descriptor_v2, 7U> make_descriptors() noexcept {
  return {
      {{sizeof(ps_operation_descriptor_v2), "fixture.double", 14U, 1U,
        PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
            PS_OPERATION_FLAG_CPU,
        sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V2, 0U, nullptr,
        PS_OPERATION_SHAPE_PRESERVE_FIRST_V2,
        PS_OPERATION_REGION_ELEMENTWISE_V2, 0U, 1U, 1U, &double_parameter,
        execute_double, nullptr},
       {sizeof(ps_operation_descriptor_v2), "fixture.bad_facet", 17U, 1U,
        PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
            PS_OPERATION_FLAG_CPU,
        sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V2, 0U, nullptr,
        PS_OPERATION_SHAPE_PRESERVE_FIRST_V2,
        PS_OPERATION_REGION_ELEMENTWISE_V2, 0U, 1U, 0U, nullptr,
        execute_bad_facet, nullptr},
       {sizeof(ps_operation_descriptor_v2), "fixture.gpu_fallback", 20U, 1U,
        PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
            PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
            PS_OPERATION_FLAG_CPU_FALLBACK,
        sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V2, 0U, nullptr,
        PS_OPERATION_SHAPE_PRESERVE_FIRST_V2,
        PS_OPERATION_REGION_ELEMENTWISE_V2, 0U, 1U, 0U, nullptr,
        execute_gpu_result, &gpu_backend_unavailable},
       {sizeof(ps_operation_descriptor_v2), "fixture.gpu_failure", 19U, 1U,
        PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
            PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
            PS_OPERATION_FLAG_CPU_FALLBACK,
        sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V2, 0U, nullptr,
        PS_OPERATION_SHAPE_PRESERVE_FIRST_V2,
        PS_OPERATION_REGION_ELEMENTWISE_V2, 0U, 1U, 0U, nullptr,
        execute_gpu_result, &gpu_ordinary_failure},
       {sizeof(ps_operation_descriptor_v2), "fixture.gpu_unknown", 19U, 1U,
        PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
            PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
            PS_OPERATION_FLAG_CPU_FALLBACK,
        sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V2, 0U, nullptr,
        PS_OPERATION_SHAPE_PRESERVE_FIRST_V2,
        PS_OPERATION_REGION_ELEMENTWISE_V2, 0U, 1U, 0U, nullptr,
        execute_gpu_result, &gpu_unknown_result},
       {sizeof(ps_operation_descriptor_v2), "fixture.gpu_output_unavailable",
        sizeof("fixture.gpu_output_unavailable") - 1U, 1U,
        PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
            PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
            PS_OPERATION_FLAG_CPU_FALLBACK,
        sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V2, 0U, nullptr,
        PS_OPERATION_SHAPE_PRESERVE_FIRST_V2,
        PS_OPERATION_REGION_ELEMENTWISE_V2, 0U, 1U, 0U, nullptr,
        execute_gpu_result, &gpu_output_then_unavailable},
       {sizeof(ps_operation_descriptor_v2),
        "fixture.gpu_bad_output_unavailable",
        sizeof("fixture.gpu_bad_output_unavailable") - 1U, 1U,
        PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
            PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU |
            PS_OPERATION_FLAG_CPU_FALLBACK,
        sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V2, 0U, nullptr,
        PS_OPERATION_SHAPE_PRESERVE_FIRST_V2,
        PS_OPERATION_REGION_ELEMENTWISE_V2, 0U, 1U, 0U, nullptr,
        execute_gpu_result, &gpu_bad_output_then_unavailable}}};
}

/** @brief Static operation descriptor table with valid and bad-output cases. */
const auto descriptors = make_descriptors();

/**
 * @brief Builds the valid operation fixture API table.
 * @return API table referencing `descriptors` and lifecycle callback.
 * @throws Nothing.
 */
ps_operation_plugin_api_v2 make_api() noexcept {
  return {sizeof(ps_operation_plugin_api_v2), 7U, descriptors.data(),
          destroy_fixture};
}

/** @brief Static valid plugin API table. */
const ps_operation_plugin_api_v2 api = make_api();

}  // namespace

/**
 * @brief Returns the valid fixture operation ABI version.
 * @return `PS_OPERATION_ABI_VERSION_2`.
 * @throws Nothing.
 * @note The function has no side effect.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_2;
}

/**
 * @brief Returns the valid fixture operation table.
 * @return Process-lifetime immutable static table.
 * @throws Nothing.
 * @note The host must invoke its destroy callback exactly once.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v2*
ps_operation_plugin_get_api_v2(void) {
  return &api;
}

/**
 * @brief Returns the fixture destroy callback count for lifecycle validation.
 * @return Monotonic count within the loaded fixture image.
 * @throws Nothing.
 * @note Used only by the lifecycle integration test.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t ps_operation_fixture_destroy_count(
    void) {
  return destroy_count.load(std::memory_order_relaxed);
}

/**
 * @brief Returns the exact GPU callback count for one fixture mode.
 * @param mode Closed fixture mode in 1..5.
 * @return Monotonic invocation count, or zero for an unknown mode.
 * @throws Nothing.
 * @note The symbol is test-only and is not part of the operation ABI table.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_fixture_gpu_invocation_count(std::uint32_t mode) {
  const GpuResultState* state = find_gpu_result_state(mode);
  return state ? state->gpu_invocations.load(std::memory_order_relaxed) : 0U;
}

/**
 * @brief Returns the exact CPU callback count for one fixture mode.
 * @param mode Closed fixture mode in 1..5.
 * @return Monotonic invocation count, or zero for an unknown mode.
 * @throws Nothing.
 * @note The symbol is test-only and is not part of the operation ABI table.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_fixture_cpu_invocation_count(std::uint32_t mode) {
  const GpuResultState* state = find_gpu_result_state(mode);
  return state ? state->cpu_invocations.load(std::memory_order_relaxed) : 0U;
}
