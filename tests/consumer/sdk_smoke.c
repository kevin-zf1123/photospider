#include "photospider/plugin/data_provider_api.h"
#include "photospider/plugin/operation_plugin_api.h"

/**
 * @brief Compiles both installed pure-C SDK surfaces in one consumer unit.
 * @return Sum of the two current ABI version constants.
 * @note The object is compile-only and does not resolve plugin entry points.
 */
unsigned int photospider_sdk_version_sum(void) {
  return PS_OPERATION_ABI_VERSION_1 + PS_DATA_PROVIDER_ABI_VERSION_1;
}
