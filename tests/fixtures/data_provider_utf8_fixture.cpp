#include <atomic>
#include <cstdint>

#include "photospider/plugin/data_provider_api.h"

#ifndef PS_PROVIDER_UTF8_CASE
#error "PS_PROVIDER_UTF8_CASE must select one UTF-8 contract case"
#endif

#if PS_PROVIDER_UTF8_CASE < 1 || PS_PROVIDER_UTF8_CASE > 2
#error "PS_PROVIDER_UTF8_CASE must be 1 or 2"
#endif

namespace {

/** @brief Number of exact destroy callbacks observed by this fixture image. */
std::atomic<std::uint32_t> destroy_count{0U};

/** @brief Exact pointer/length view over one static schema key. */
struct KeyBytes final {
  /** @brief Static schema key bytes. */
  const char* data = nullptr;
  /** @brief Exact byte count excluding the terminator. */
  std::uint32_t size = 0U;
};

/**
 * @brief Returns the selected provider schema key.
 * @return Case 1 overlong `C0 AF` or case 2 valid non-ASCII UTF-8.
 * @throws Nothing.
 */
KeyBytes schema_key() noexcept {
#if PS_PROVIDER_UTF8_CASE == 1
  static const char key[] = {static_cast<char>(0xc0), static_cast<char>(0xaf),
                             '\0'};
  return {key, 2U};
#else
  static const char key[] = {'f',
                             'i',
                             'x',
                             't',
                             'u',
                             'r',
                             'e',
                             '.',
                             static_cast<char>(0xe5),
                             static_cast<char>(0x9b),
                             static_cast<char>(0xbe),
                             static_cast<char>(0xe5),
                             static_cast<char>(0x83),
                             static_cast<char>(0x8f),
                             '\0'};
  return {key, 14U};
#endif
}

/**
 * @brief Records exact release of this fixture's static table.
 * @param schemas Original schema array.
 * @param schema_count Original schema count.
 * @throws Nothing.
 */
void destroy_fixture(const ps_data_schema_v1* schemas,
                     std::uint32_t schema_count) {
  if (schemas && schema_count == 1U) {
    destroy_count.fetch_add(1U, std::memory_order_relaxed);
  }
}

/**
 * @brief Builds one structurally complete provider schema.
 * @return Schema carrying the selected strict UTF-8 case.
 * @throws Nothing.
 */
ps_data_schema_v1 make_schema() noexcept {
  const KeyBytes key = schema_key();
  return {sizeof(ps_data_schema_v1), key.data, key.size, 3U, 4U};
}

/** @brief Static provider schema carrying the selected UTF-8 case. */
const ps_data_schema_v1 schema = make_schema();

/**
 * @brief Builds the complete provider ABI table.
 * @return Static one-record table with exact destroy ownership.
 * @throws Nothing.
 */
ps_data_provider_api_v1 make_api() noexcept {
  return {sizeof(ps_data_provider_api_v1), 1U, &schema, destroy_fixture};
}

/** @brief Complete provider ABI table for the selected UTF-8 case. */
const ps_data_provider_api_v1 api = make_api();

}  // namespace

/**
 * @brief Returns provider ABI version one.
 * @return `PS_DATA_PROVIDER_ABI_VERSION_1`.
 * @throws Nothing.
 */
extern "C" PS_DATA_PROVIDER_EXPORT std::uint32_t
ps_data_provider_get_abi_version(void) {
  return PS_DATA_PROVIDER_ABI_VERSION_1;
}

/**
 * @brief Returns the selected UTF-8 fixture table.
 * @return Process-lifetime immutable table.
 * @throws Nothing.
 */
extern "C" PS_DATA_PROVIDER_EXPORT const ps_data_provider_api_v1*
ps_data_provider_get_api_v1(void) {
  return &api;
}

/**
 * @brief Returns exact destroy callback count for this fixture image.
 * @return Monotonic destroy count.
 * @throws Nothing.
 */
extern "C" PS_DATA_PROVIDER_EXPORT std::uint32_t
ps_data_provider_utf8_fixture_destroy_count(void) {
  return destroy_count.load(std::memory_order_relaxed);
}
