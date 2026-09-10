#include <atomic>
#include <cstdint>

namespace {
std::atomic<std::uint32_t> calls{0};
}

#include "photospider/plugin/operation_plugin_api.h"

/**
 * @brief Returns an intentionally unsupported operation ABI version.
 * @return Seven, rejected before the ABI8 API lookup.
 * @throws Nothing.
 * @note Version validation must reject before reading an API table.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return 7U;
}

/**
 * @brief Returns no API table for the unsupported-version fixture.
 * @return Null.
 * @throws Nothing.
 * @note A correct host never calls this after the version mismatch.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v8*
ps_operation_plugin_get_api_v8(void) {
  ++calls;
  return nullptr;
}

extern "C" PS_OPERATION_EXPORT std::uint32_t ps_bad_operation_api_calls(void) {
  return calls.load();
}
