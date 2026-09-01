#include <cstddef>
#include <cstdint>

#include "photospider/plugin/operation_plugin_api.h"

#ifndef PS_BAD_PARAMETER_CASE
#error "PS_BAD_PARAMETER_CASE must select one malformed parameter contract"
#endif

namespace {

/**
 * @brief Never-entered callback for malformed descriptor fixtures.
 * @param user_data Unused descriptor state.
 * @param inputs Unused input array.
 * @param input_count Unused input count.
 * @param parameters Unused parameter array.
 * @param parameter_count Unused parameter count.
 * @param backend Unused backend.
 * @param cancelled Unused cancellation callback.
 * @param cancellation_context Unused cancellation state.
 * @param sink Unused output sink.
 * @param diagnostic Unused diagnostic buffer.
 * @param diagnostic_capacity Unused diagnostic capacity.
 * @return Nonzero because execution is never legal for this fixture.
 * @throws Nothing.
 * @note Host validation must reject before retaining or invoking this callback.
 */
int execute_never(void* user_data, const ps_operation_value_view_v2* inputs,
                  std::uint32_t input_count,
                  const ps_operation_parameter_value_v2* parameters,
                  std::uint32_t parameter_count, std::uint32_t backend,
                  ps_operation_cancelled_v2 cancelled,
                  void* cancellation_context,
                  const ps_operation_output_sink_v2* sink, char* diagnostic,
                  std::size_t diagnostic_capacity) {
  static_cast<void>(user_data);
  static_cast<void>(inputs);
  static_cast<void>(input_count);
  static_cast<void>(parameters);
  static_cast<void>(parameter_count);
  static_cast<void>(backend);
  static_cast<void>(cancelled);
  static_cast<void>(cancellation_context);
  static_cast<void>(sink);
  static_cast<void>(diagnostic);
  static_cast<void>(diagnostic_capacity);
  return 1;
}

/**
 * @brief Accepts exact destroy ownership for one rejected static table.
 * @param operations Original descriptor array.
 * @param operation_count Original descriptor count.
 * @throws Nothing.
 * @note Static fixture memory requires no release.
 */
void destroy_fixture(const ps_operation_descriptor_v2* operations,
                     std::uint32_t operation_count) {
  static_cast<void>(operations);
  static_cast<void>(operation_count);
}

#if PS_BAD_PARAMETER_CASE == 2
/** @brief Parameter record with an intentionally wrong exact structure size. */
const ps_operation_parameter_descriptor_v2 parameter = {
    sizeof(ps_operation_parameter_descriptor_v2) - 1U, "value", 5U,
    PS_OPERATION_PARAMETER_FLOAT64_V2, 1U};
#elif PS_BAD_PARAMETER_CASE == 4
/** @brief Parameter record whose declared key length exceeds the ABI bound. */
const ps_operation_parameter_descriptor_v2 parameter = {
    sizeof(ps_operation_parameter_descriptor_v2), "x", 1025U,
    PS_OPERATION_PARAMETER_FLOAT64_V2, 1U};
#else
/** @brief Structurally valid record used to isolate another malformed field. */
const ps_operation_parameter_descriptor_v2 parameter = {
    sizeof(ps_operation_parameter_descriptor_v2), "value", 5U,
    PS_OPERATION_PARAMETER_FLOAT64_V2, 1U};
#endif

#if PS_BAD_PARAMETER_CASE == 5
/** @brief Byte storage used to provide a deliberately misaligned pointer. */
unsigned char
    misaligned_storage[sizeof(ps_operation_parameter_descriptor_v2) + 1U]{};
#endif

/**
 * @brief Returns the case-specific malformed parameter table pointer.
 * @return Null, aligned valid, or deliberately misaligned pointer.
 * @throws Nothing.
 * @note The host must validate pointer/count/alignment before dereference.
 */
const ps_operation_parameter_descriptor_v2* parameter_pointer() noexcept {
#if PS_BAD_PARAMETER_CASE == 1
  return nullptr;
#elif PS_BAD_PARAMETER_CASE == 5
  return reinterpret_cast<const ps_operation_parameter_descriptor_v2*>(
      misaligned_storage + 1U);
#else
  return &parameter;
#endif
}

/**
 * @brief Returns the case-specific parameter count.
 * @return One except for the count-overflow case, which returns 129.
 * @throws Nothing.
 */
std::uint32_t parameter_count() noexcept {
#if PS_BAD_PARAMETER_CASE == 3
  return 129U;
#else
  return 1U;
#endif
}

/** @brief Malformed operation descriptor selected by the compile definition. */
const ps_operation_descriptor_v2 descriptors[] = {
    {sizeof(ps_operation_descriptor_v2), "fixture.bad.parameter", 21U, 0U,
     PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
         PS_OPERATION_FLAG_CPU,
     sizeof(double), PS_OPERATION_ELEMENT_FLOAT64_V2, 0U, nullptr,
     PS_OPERATION_SHAPE_SCALAR_V2, PS_OPERATION_REGION_WHOLE_V2, 0U, 1U,
     parameter_count(), parameter_pointer(), execute_never, nullptr}};

/** @brief Complete API table whose descriptor must fail atomically. */
const ps_operation_plugin_api_v2 api = {sizeof(ps_operation_plugin_api_v2), 1U,
                                        descriptors, destroy_fixture};

}  // namespace

/**
 * @brief Returns the supported ABI version so descriptor validation runs.
 * @return `PS_OPERATION_ABI_VERSION_2`.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_2;
}

/**
 * @brief Returns one case-specific malformed API table.
 * @return Process-lifetime static table.
 * @throws Nothing.
 * @note The host must reject it without partial registry publication.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v2*
ps_operation_plugin_get_api_v2(void) {
  return &api;
}
