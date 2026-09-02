#include "photospider/plugin/operation_plugin.hpp"

// Keep the self-contained C++ wrapper as this translation unit's first include.
#include "photospider/plugin/data_provider_api.h"

static_assert(
    ps::plugin::element_type_value(PS_OPERATION_ELEMENT_FLOAT64_V2) ==
        PS_OPERATION_ELEMENT_FLOAT64_V2,
    "the installed C++ operation wrapper must preserve ABI enum values");
static_assert(
    noexcept(ps::plugin::element_type_value(PS_OPERATION_ELEMENT_FLOAT64_V2)),
    "the installed C++ operation wrapper conversion must remain noexcept");

/**
 * @brief Returns the ABI-version sum computed by the installed C SDK unit.
 * @return Sum of the operation and data-provider ABI version constants.
 * @throws Nothing.
 * @note The implementation is compiled as C and linked into this C++ process.
 */
extern "C" unsigned int photospider_sdk_version_sum(void);

/**
 * @brief Runs the installed kernel pipeline through the downstream shared
 * bridge.
 * @return Zero when the bridge produces the expected scalar.
 * @throws Nothing across the C linkage boundary.
 * @note The bridge owns all C++ exception fencing and links the installed
 * static or shared kernel product directly.
 */
extern "C" int photospider_consumer_run_pipeline(void);

/**
 * @brief Verifies the installed C SDKs and invokes the shared bridge.
 * @return Zero when the SDK versions and shared-bridge pipeline both pass.
 * @throws Nothing.
 * @note Distinct nonzero results identify SDK mismatch or bridge failure.
 */
int main() {
  if (photospider_sdk_version_sum() !=
      PS_OPERATION_ABI_VERSION_2 + PS_DATA_PROVIDER_ABI_VERSION_1) {
    return 4;
  }
  return photospider_consumer_run_pipeline();
}
