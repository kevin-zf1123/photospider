#include <atomic>
#include <cstddef>
#include <cstdint>

#include "./result_registry_fixture.hpp"

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

void destroy_fixture(void*) {
  destroy_count.fetch_add(1U, std::memory_order_relaxed);
}

/**
 * @brief Builds the case-specific parameter declaration.
 * @return Structurally complete parameter record carrying selected key bytes.
 * @throws Nothing.
 */
ps_result_parameter_descriptor_v2 make_parameter() noexcept {
  const KeyBytes key = parameter_key();
  return {sizeof(ps_result_parameter_descriptor_v2), key.data, key.size,
          PS_RESULT_PARAMETER_FLOAT64_V2, 0U};
}

/** @brief Static parameter declaration for the selected UTF-8 case. */
const ps_result_parameter_descriptor_v2 parameter = make_parameter();

ps_result_operation_v2 make_descriptor() noexcept {
  const KeyBytes key = operation_key();
  return result_registry_fixture::make_operation(key.data, key.size, &parameter,
                                                 1);
}
const auto descriptor = make_descriptor();
// NOLINTBEGIN(whitespace/indent_namespace)
const auto api =
    result_registry_fixture::make_api(&descriptor, destroy_fixture);
// NOLINTEND

}  // namespace

extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}

/**
 * @brief Returns exact destroy callback count for this fixture image.
 * @return Monotonic destroy count.
 * @throws Nothing.
 */
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_utf8_fixture_destroy_count(void) {
  return destroy_count.load(std::memory_order_relaxed);
}
