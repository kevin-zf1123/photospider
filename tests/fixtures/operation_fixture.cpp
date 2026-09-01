#include <atomic>
#include <cstdint>
#include <cstring>

#include "photospider/plugin/operation_plugin_api.h"

namespace {

/** @brief Number of exact destroy callbacks observed by this fixture image. */
std::atomic<std::uint32_t> destroy_count{0U};

/**
 * @brief Doubles one contiguous Float64 scalar input.
 * @param user_data Unused fixture state.
 * @param inputs Exact one-element input array.
 * @param input_count Must equal one.
 * @param backend Must name CPU for this fixture.
 * @param cancelled Host cancellation observer.
 * @param cancellation_context Host cancellation state.
 * @param sink Required single-publication sink.
 * @param diagnostic Writable failure diagnostic.
 * @param diagnostic_capacity Diagnostic buffer capacity.
 * @return Zero after accepted publication; nonzero on validation/cancellation.
 * @note No pointer is retained after return.
 */
int execute_double(void* user_data, const ps_operation_value_view_v1* inputs,
                   std::uint32_t input_count, std::uint32_t backend,
                   ps_operation_cancelled_v1 cancelled,
                   void* cancellation_context,
                   const ps_operation_output_sink_v1* sink, char* diagnostic,
                   std::size_t diagnostic_capacity) {
  static_cast<void>(user_data);
  if (!inputs || input_count != 1U || backend != 1U || !sink ||
      !sink->publish ||
      inputs[0].element_type != PS_OPERATION_ELEMENT_FLOAT64_V1 ||
      inputs[0].rank != 1U || inputs[0].byte_size != sizeof(double) ||
      !inputs[0].shape || inputs[0].shape[0] != 1U || !inputs[0].data ||
      inputs[0].facet_count != 1U || !inputs[0].facets ||
      inputs[0].facets[0].struct_size != sizeof(ps_operation_facet_view_v1) ||
      inputs[0].facets[0].key_size != 13U ||
      std::memcmp(inputs[0].facets[0].key, "test.semantic", 13U) != 0 ||
      (cancelled && cancelled(cancellation_context))) {
    if (diagnostic && diagnostic_capacity > 0U) {
      diagnostic[0] = '\0';
    }
    return 1;
  }
  double value = 0.0;
  std::memcpy(&value, inputs[0].data, sizeof(value));
  value *= 2.0;
  const std::uint64_t shape[] = {1U};
  return sink->publish(sink->context, PS_OPERATION_ELEMENT_FLOAT64_V1, shape,
                       1U, inputs[0].facets, inputs[0].facet_count,
                       reinterpret_cast<const std::uint8_t*>(&value),
                       sizeof(value))
             ? 0
             : 1;
}

/**
 * @brief Attempts one output with an invalid zero-version facet.
 * @param user_data Unused fixture state.
 * @param inputs Exact one-element input array.
 * @param input_count Must equal one.
 * @param backend Must name CPU for this fixture.
 * @param cancelled Host cancellation observer.
 * @param cancellation_context Host cancellation state.
 * @param sink Required single-publication sink.
 * @param diagnostic Writable failure diagnostic.
 * @param diagnostic_capacity Diagnostic buffer capacity.
 * @return Zero after the sink rejects the malformed facet.
 * @note This fixture proves host-side facet validation, not plugin behavior.
 */
int execute_bad_facet(void* user_data, const ps_operation_value_view_v1* inputs,
                      std::uint32_t input_count, std::uint32_t backend,
                      ps_operation_cancelled_v1 cancelled,
                      void* cancellation_context,
                      const ps_operation_output_sink_v1* sink, char* diagnostic,
                      std::size_t diagnostic_capacity) {
  static_cast<void>(user_data);
  static_cast<void>(cancelled);
  static_cast<void>(cancellation_context);
  if (!inputs || input_count != 1U || backend != 1U || !sink ||
      !sink->publish) {
    return 1;
  }
  if (diagnostic && diagnostic_capacity > 0U) {
    diagnostic[0] = '\0';
  }
  const char key[] = "bad.facet";
  const ps_operation_facet_view_v1 facet = {
      sizeof(ps_operation_facet_view_v1),
      key,
      static_cast<std::uint32_t>(sizeof(key) - 1U),
      0U,
      nullptr,
      0U};
  static_cast<void>(sink->publish(sink->context, inputs[0].element_type,
                                  inputs[0].shape, inputs[0].rank, &facet, 1U,
                                  inputs[0].data, inputs[0].byte_size));
  return 0;
}

/**
 * @brief Records exact release of the static descriptor table.
 * @param operations Original fixture descriptor pointer.
 * @param operation_count Original fixture descriptor count.
 * @throws Nothing.
 * @note The records are static and require no allocation release.
 */
void destroy_fixture(const ps_operation_descriptor_v1* operations,
                     std::uint32_t operation_count) {
  if (operations && operation_count == 2U) {
    destroy_count.fetch_add(1U, std::memory_order_relaxed);
  }
}

/** @brief Static operation descriptor table with valid and bad-output cases. */
const ps_operation_descriptor_v1 descriptors[] = {
    {sizeof(ps_operation_descriptor_v1), "fixture.double", 14U, 1U,
     PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
         PS_OPERATION_FLAG_CPU,
     sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V1,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V1, PS_OPERATION_REGION_ELEMENTWISE_V1,
     0U, 1U, execute_double, nullptr},
    {sizeof(ps_operation_descriptor_v1), "fixture.bad_facet", 17U, 1U,
     PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
         PS_OPERATION_FLAG_CPU,
     sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V1,
     PS_OPERATION_SHAPE_PRESERVE_FIRST_V1, PS_OPERATION_REGION_ELEMENTWISE_V1,
     0U, 1U, execute_bad_facet, nullptr},
};

/** @brief Static valid plugin API table. */
const ps_operation_plugin_api_v1 api = {sizeof(ps_operation_plugin_api_v1), 2U,
                                        descriptors, destroy_fixture};

}  // namespace

/**
 * @brief Returns the valid fixture operation ABI version.
 * @return `PS_OPERATION_ABI_VERSION_1`.
 * @throws Nothing.
 * @note The function has no side effect.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_1;
}

/**
 * @brief Returns the valid fixture operation table.
 * @return Process-lifetime immutable static table.
 * @throws Nothing.
 * @note The host must invoke its destroy callback exactly once.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v1*
ps_operation_plugin_get_api_v1(void) {
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
