#include <cstdint>

#include "photospider/plugin/operation_plugin_api.h"

/**
 * @brief Returns an intentionally unsupported operation ABI version.
 * @return Two, rejected before the v3 API lookup.
 * @throws Nothing.
 * @note Version validation must reject before reading an API table.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return 2U;
}

/**
 * @brief Returns no API table for the unsupported-version fixture.
 * @return Null.
 * @throws Nothing.
 * @note A correct host never calls this after the version mismatch.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v3*
ps_operation_plugin_get_api_v3(void) {
  return nullptr;
}
