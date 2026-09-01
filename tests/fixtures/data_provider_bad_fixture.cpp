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

/** @brief Malformed schema carrying an unknown element type. */
const ps_data_schema_v1 schema = {sizeof(ps_data_schema_v1), "fixture.unknown",
                                  15U, 99U, 1U};

/** @brief Structurally complete table used to test schema validation. */
const ps_data_provider_api_v1 api = {sizeof(ps_data_provider_api_v1), 1U,
                                     &schema, destroy_fixture};

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
