#include <atomic>
#include <cstddef>
#include <cstdint>

#include "photospider/plugin/operation_plugin_api.h"

#ifndef PS_OPERATION_UTF8_CASE
#error "PS_OPERATION_UTF8_CASE must select one UTF-8 contract case"
#endif

#if PS_OPERATION_UTF8_CASE < 1 || PS_OPERATION_UTF8_CASE > 6
#error "PS_OPERATION_UTF8_CASE must be in the inclusive range 1..6"
#endif

namespace {

/** @brief Number of exact destroy callbacks observed by this fixture image. */
std::atomic<std::uint32_t> destroy_count{0U};

/** @brief Exact pointer/length view over one static key byte sequence. */
struct KeyBytes final {
  /** @brief Static key bytes. */
  const char* data = nullptr;
  /** @brief Exact key byte count excluding the terminator. */
  std::uint32_t size = 0U;
};

/**
 * @brief Returns the case-specific operation key bytes.
 * @return Invalid or valid strict UTF-8 selected by the compile definition.
 * @throws Nothing.
 * @note Case 1 is overlong `C0 AF`, case 4 exceeds U+10FFFF, and case 6 is
 * valid non-ASCII UTF-8; the other cases isolate parameter keys.
 */
KeyBytes operation_key() noexcept {
#if PS_OPERATION_UTF8_CASE == 1
  static const char key[] = {static_cast<char>(0xc0), static_cast<char>(0xaf),
                             '\0'};
  return {key, 2U};
#elif PS_OPERATION_UTF8_CASE == 4
  static const char key[] = {static_cast<char>(0xf4), static_cast<char>(0x90),
                             static_cast<char>(0x80), static_cast<char>(0x80),
                             '\0'};
  return {key, 4U};
#elif PS_OPERATION_UTF8_CASE == 6
  static const char key[] = {'f',
                             'i',
                             'x',
                             't',
                             'u',
                             'r',
                             'e',
                             '.',
                             static_cast<char>(0xe5),
                             static_cast<char>(0x80),
                             static_cast<char>(0x8d),
                             static_cast<char>(0xe7),
                             static_cast<char>(0x8e),
                             static_cast<char>(0x87),
                             '\0'};
  return {key, 14U};
#else
  static const char key[] = "fixture.utf8.parameter";
  return {key, static_cast<std::uint32_t>(sizeof(key) - 1U)};
#endif
}

/**
 * @brief Returns the case-specific parameter-schema key bytes.
 * @return Truncated, surrogate, invalid-continuation, ASCII, or valid
 * non-ASCII UTF-8 bytes selected by the compile definition.
 * @throws Nothing.
 * @note Cases 2, 3, and 5 are negative parameter-schema records.
 */
KeyBytes parameter_key() noexcept {
#if PS_OPERATION_UTF8_CASE == 2
  static const char key[] = {static_cast<char>(0xe2), static_cast<char>(0x82),
                             '\0'};
  return {key, 2U};
#elif PS_OPERATION_UTF8_CASE == 3
  static const char key[] = {static_cast<char>(0xed), static_cast<char>(0xa0),
                             static_cast<char>(0x80), '\0'};
  return {key, 3U};
#elif PS_OPERATION_UTF8_CASE == 5
  static const char key[] = {static_cast<char>(0xe2), '(',
                             static_cast<char>(0xa1), '\0'};
  return {key, 3U};
#elif PS_OPERATION_UTF8_CASE == 6
  static const char key[] = {static_cast<char>(0xe7),
                             static_cast<char>(0xbc),
                             static_cast<char>(0xa9),
                             static_cast<char>(0xe6),
                             static_cast<char>(0x94),
                             static_cast<char>(0xbe),
                             '\0'};
  return {key, 6U};
#else
  static const char key[] = "value";
  return {key, static_cast<std::uint32_t>(sizeof(key) - 1U)};
#endif
}

/**
 * @brief Never publishes output for this registry-validation fixture.
 * @param user_data Unused descriptor state.
 * @param inputs Unused input array.
 * @param input_count Unused input count.
 * @param parameters Unused parameter array.
 * @param parameter_count Unused parameter count.
 * @param backend Unused backend.
 * @param cancelled Unused cancellation callback.
 * @param cancellation_context Unused cancellation state.
 * @param sink Unused output sink.
 * @param diagnostic Unused diagnostic storage.
 * @param diagnostic_capacity Unused diagnostic capacity.
 * @return Nonzero because tests never invoke this fixture.
 * @throws Nothing.
 * @note Negative cases must fail before callback publication; the positive
 * case validates only registry text acceptance.
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
 * @brief Records exact release of this fixture's static table.
 * @param operations Original descriptor array.
 * @param operation_count Original descriptor count.
 * @throws Nothing.
 * @note Every accepted API table ownership path must call this exactly once.
 */
void destroy_fixture(const ps_operation_descriptor_v2* operations,
                     std::uint32_t operation_count) {
  if (operations && operation_count == 1U) {
    destroy_count.fetch_add(1U, std::memory_order_relaxed);
  }
}

/**
 * @brief Builds the case-specific parameter declaration.
 * @return Structurally complete parameter record carrying selected key bytes.
 * @throws Nothing.
 */
ps_operation_parameter_descriptor_v2 make_parameter() noexcept {
  const KeyBytes key = parameter_key();
  return {sizeof(ps_operation_parameter_descriptor_v2), key.data, key.size,
          PS_OPERATION_PARAMETER_FLOAT64_V2, 0U};
}

/** @brief Static parameter declaration for the selected UTF-8 case. */
const ps_operation_parameter_descriptor_v2 parameter = make_parameter();

/**
 * @brief Builds one structurally complete operation descriptor.
 * @return Descriptor carrying the selected operation and parameter keys.
 * @throws Nothing.
 */
ps_operation_descriptor_v2 make_descriptor() noexcept {
  const KeyBytes key = operation_key();
  return {sizeof(ps_operation_descriptor_v2),
          key.data,
          key.size,
          0U,
          PS_OPERATION_FLAG_DETERMINISTIC | PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
              PS_OPERATION_FLAG_CPU,
          sizeof(double),
          PS_OPERATION_ELEMENT_FLOAT64_V2,
          0U,
          nullptr,
          PS_OPERATION_SHAPE_SCALAR_V2,
          PS_OPERATION_REGION_WHOLE_V2,
          0U,
          1U,
          1U,
          &parameter,
          execute_never,
          nullptr};
}

/** @brief Static descriptor carrying the selected UTF-8 contract case. */
const ps_operation_descriptor_v2 descriptor = make_descriptor();

/**
 * @brief Builds the complete operation ABI table.
 * @return Static one-record table with exact destroy ownership.
 * @throws Nothing.
 */
ps_operation_plugin_api_v2 make_api() noexcept {
  return {sizeof(ps_operation_plugin_api_v2), 1U, &descriptor, destroy_fixture};
}

/** @brief Complete operation ABI table for the selected UTF-8 case. */
const ps_operation_plugin_api_v2 api = make_api();

}  // namespace

/**
 * @brief Returns operation ABI version two.
 * @return `PS_OPERATION_ABI_VERSION_2`.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return PS_OPERATION_ABI_VERSION_2;
}

/**
 * @brief Returns the selected UTF-8 fixture table.
 * @return Process-lifetime immutable table.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v2*
ps_operation_plugin_get_api_v2(void) {
  return &api;
}

/**
 * @brief Returns exact destroy callback count for this fixture image.
 * @return Monotonic destroy count.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_utf8_fixture_destroy_count(void) {
  return destroy_count.load(std::memory_order_relaxed);
}
