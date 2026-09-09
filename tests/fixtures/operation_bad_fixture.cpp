#include <cstdint>

#include "photospider/plugin/operation_plugin_api.h"

/**
 * @brief Returns an intentionally unsupported operation ABI version.
 * @return Four, rejected before the v5 API lookup.
 * @throws Nothing.
 * @note Version validation must reject before reading an API table.
 */
extern "C" PS_OPERATION_EXPORT std::uint32_t
ps_operation_plugin_get_abi_version(void) {
  return 4U;
}

/**
 * @brief Returns no API table for the unsupported-version fixture.
 * @return Null.
 * @throws Nothing.
 * @note A correct host never calls this after the version mismatch.
 */
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v7*
ps_operation_plugin_get_api_v7(void) {
  return nullptr;
}
