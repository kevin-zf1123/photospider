#include <atomic>
#include <cstdint>

#include "photospider/plugin/operation_plugin_api.h"

namespace {

/** @brief Number of exact destroy callbacks observed by this fixture image. */
std::atomic<std::uint32_t> destroy_count{0U};

/** @brief Maximum logical extent used by the malformed descriptor. */
constexpr std::uint64_t kMaximumExtent = UINT64_MAX;

/** @brief Fixed shape whose dense Float64 byte product overflows uint64. */
const std::uint64_t overflowing_shape[] = {kMaximumExtent};

/**
 * @brief Provides a callback that must remain unreachable after validation.
 * @param user_data Unused descriptor state.
 * @param inputs Unused input array for this zero-input descriptor.
 * @param input_count Expected zero input count.
 * @param parameters Unused parameter array.
 * @param parameter_count Expected zero parameter count.
 * @param backend Selected local backend.
 * @param cancelled Host cancellation observer.
 * @param cancellation_context Host cancellation state.
 * @param sink Host output sink.
 * @param diagnostic Writable diagnostic buffer.
 * @param diagnostic_capacity Diagnostic buffer capacity.
 * @return Ordinary failure if an invalid descriptor is ever invoked.
 * @throws Nothing.
 * @note Loader validation must reject the descriptor before publication.
 */
int execute_unreachable(void* user_data,
                        const ps_operation_value_view_v2* inputs,
                        std::uint32_t input_count,
                        const ps_operation_parameter_value_v2* parameters,
                        std::uint32_t parameter_count, std::uint32_t backend,
                        ps_operation_cancelled_v2 cancelled,
                        void* cancellation_context,
                        const ps_operation_output_sink_v2* sink,
                        char* diagnostic,
                        std::size_t diagnostic_capacity) noexcept {
  static_cast<void>(user_data);
  static_cast<void>(inputs);
  static_cast<void>(input_count);
  static_cast<void>(parameters);
  static_cast<void>(parameter_count);
  static_cast<void>(backend);
  static_cast<void>(cancelled);
  static_cast<void>(cancellation_context);
  static_cast<void>(sink);
  if (diagnostic && diagnostic_capacity != 0U) {
    diagnostic[0] = '\0';
  }
  return PS_OPERATION_RESULT_FAILURE_V2;
}

/**
 * @brief Builds the immutable dense-overflow operation descriptor.
 * @return Descriptor whose fixed Float64 dense byte product overflows uint64.
 * @throws Nothing.
 */
ps_operation_descriptor_v2 make_descriptor() noexcept {
  return {sizeof(ps_operation_descriptor_v2),
          "fixture.dense_overflow",
          sizeof("fixture.dense_overflow") - 1U,
          0U,
          PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
              PS_OPERATION_FLAG_CPU,
          sizeof(double),
          PS_OPERATION_ELEMENT_FLOAT64_V2,
          1U,
          overflowing_shape,
          PS_OPERATION_SHAPE_FIXED_V2,
          PS_OPERATION_REGION_WHOLE_V2,
          0U,
          1U,
          0U,
          nullptr,
          execute_unreachable,
          nullptr};
}

/** @brief Immutable dense-overflow descriptor rejected by the host loader. */
const ps_operation_descriptor_v2 descriptor = make_descriptor();

/**
 * @brief Records exact release of the static malformed descriptor table.
 * @param operations Original descriptor pointer.
 * @param operation_count Original descriptor count.
 * @throws Nothing.
 * @note Static records need no allocation release.
 */
void destroy_fixture(const ps_operation_descriptor_v2* operations,
                     std::uint32_t operation_count) noexcept {
  if (operations == &descriptor && operation_count == 1U) {
    destroy_count.fetch_add(1U, std::memory_order_relaxed);
  }
}

/**
 * @brief Builds the static API table owning the malformed fixed descriptor.
 * @return Complete version-two API table.
 * @throws Nothing.
 */
ps_operation_plugin_api_v2 make_api() noexcept {
  return {sizeof(ps_operation_plugin_api_v2), 1U, &descriptor, destroy_fixture};
}

/** @brief Static API table owning the malformed fixed descriptor. */
const ps_operation_plugin_api_v2 api = make_api();

}  // namespace

/**
 * @brief Returns the supported operation ABI version.
 * @return `PS_OPERATION_ABI_VERSION_2`.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_2;
}

/**
 * @brief Returns the dense-overflow descriptor table.
 * @return Process-lifetime immutable API table.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v2*
ps_operation_plugin_get_api_v2(void) {
  return &api;
}

/**
 * @brief Returns the observed descriptor-table destroy count.
 * @return Monotonic destroy count within this fixture image.
 * @throws Nothing.
 * @note Test-only symbol outside the operation ABI table.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_dense_overflow_fixture_destroy_count(void) {
  return destroy_count.load(std::memory_order_relaxed);
}
