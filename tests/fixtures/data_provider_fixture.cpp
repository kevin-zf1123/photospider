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

/**
 * @brief Builds the valid schema without namespace-scope continuation.
 * @return Float64 rank-four fixture schema.
 * @throws Nothing.
 * @note The returned string pointer has process-lifetime storage.
 */
ps_data_schema_v1 make_schema() noexcept {
  return {sizeof(ps_data_schema_v1), "fixture.float64", 15U, 3U, 4U};
}

/** @brief Static valid provider schema. */
const ps_data_schema_v1 schema = make_schema();

/**
 * @brief Builds the valid provider API table.
 * @return API table referencing `schema` and its lifecycle callback.
 * @throws Nothing.
 * @note The table itself remains process-lifetime static storage.
 */
ps_data_provider_api_v1 make_api() noexcept {
  return {sizeof(ps_data_provider_api_v1), 1U, &schema, destroy_fixture};
}

/** @brief Static valid provider API table. */
const ps_data_provider_api_v1 api = make_api();

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
