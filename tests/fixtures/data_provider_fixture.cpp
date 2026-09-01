#include <atomic>
#include <cstdint>

#include "photospider/plugin/data_provider_api.h"

namespace {

/** @brief Number of exact provider releases observed by the fixture image. */
std::atomic<std::uint32_t> destroy_count{0U};

/**
 * @brief Records release of the static schema table.
 * @param schemas Original fixture schema pointer.
 * @param schema_count Original schema count.
 * @throws Nothing.
 * @note Only the exact published table increments the counter.
 */
void destroy_fixture(const ps_data_schema_v1* schemas,
                     std::uint32_t schema_count) {
  if (schemas && schema_count == 1U) {
    destroy_count.fetch_add(1U, std::memory_order_relaxed);
  }
}

/** @brief Static valid provider schema. */
const ps_data_schema_v1 schema = {sizeof(ps_data_schema_v1), "fixture.float64",
                                  15U, 3U, 4U};

/** @brief Static valid provider API table. */
const ps_data_provider_api_v1 api = {sizeof(ps_data_provider_api_v1), 1U,
                                     &schema, destroy_fixture};

}  // namespace

/**
 * @brief Returns the valid fixture provider ABI version.
 * @return `PS_DATA_PROVIDER_ABI_VERSION_1`.
 * @throws Nothing.
 * @note The function has no side effect.
 */
extern "C" PS_DATA_PROVIDER_EXPORT std::uint32_t
ps_data_provider_get_abi_version(void) {
  return PS_DATA_PROVIDER_ABI_VERSION_1;
}

/**
 * @brief Returns the valid fixture provider table.
 * @return Process-lifetime immutable static table.
 * @throws Nothing.
 * @note The host must invoke its destroy callback exactly once.
 */
extern "C" PS_DATA_PROVIDER_EXPORT const ps_data_provider_api_v1*
ps_data_provider_get_api_v1(void) {
  return &api;
}

/**
 * @brief Returns the fixture provider destroy count.
 * @return Monotonic count within the loaded fixture image.
 * @throws Nothing.
 * @note Used only by the lifecycle integration test.
 */
extern "C" PS_DATA_PROVIDER_EXPORT std::uint32_t
ps_data_provider_fixture_destroy_count(void) {
  return destroy_count.load(std::memory_order_relaxed);
}
