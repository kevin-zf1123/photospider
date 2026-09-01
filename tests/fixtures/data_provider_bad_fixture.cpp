#include <cstdint>

#include "photospider/plugin/data_provider_api.h"

namespace {

/**
 * @brief Performs no-op cleanup for the malformed static table.
 * @param schemas Ignored original static schema pointer.
 * @param schema_count Ignored original schema count.
 * @throws Nothing.
 * @note The fixture owns no dynamic storage.
 */
void destroy_fixture(const ps_data_schema_v1* schemas,
                     std::uint32_t schema_count) {
  static_cast<void>(schemas);
  static_cast<void>(schema_count);
}

/**
 * @brief Builds the malformed schema without namespace-scope continuation.
 * @return Schema carrying an intentionally unknown element type.
 * @throws Nothing.
 * @note The returned string pointer has process-lifetime storage.
 */
ps_data_schema_v1 make_schema() noexcept {
  return {sizeof(ps_data_schema_v1), "fixture.unknown", 15U, 99U, 1U};
}

/** @brief Malformed schema carrying an unknown element type. */
const ps_data_schema_v1 schema = make_schema();

/**
 * @brief Builds the structurally complete malformed provider table.
 * @return API table referencing `schema` and its cleanup callback.
 * @throws Nothing.
 * @note The table itself remains process-lifetime static storage.
 */
ps_data_provider_api_v1 make_api() noexcept {
  return {sizeof(ps_data_provider_api_v1), 1U, &schema, destroy_fixture};
}

/** @brief Structurally complete table used to test schema validation. */
const ps_data_provider_api_v1 api = make_api();

}  // namespace

/**
 * @brief Returns the supported ABI version for the malformed-record fixture.
 * @return `PS_DATA_PROVIDER_ABI_VERSION_1`.
 * @throws Nothing.
 * @note Record validation, rather than version validation, must reject it.
 */
extern "C" PS_DATA_PROVIDER_EXPORT std::uint32_t
ps_data_provider_get_abi_version(void) {
  return PS_DATA_PROVIDER_ABI_VERSION_1;
}

/**
 * @brief Returns the malformed provider table.
 * @return Process-lifetime static table.
 * @throws Nothing.
 * @note The element type is intentionally unknown.
 */
extern "C" PS_DATA_PROVIDER_EXPORT const ps_data_provider_api_v1*
ps_data_provider_get_api_v1(void) {
  return &api;
}
