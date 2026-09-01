#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_DATA_PROVIDER_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_DATA_PROVIDER_API_H_

#include <stdint.h>

#if defined(_WIN32)
#if defined(PHOTOSPIDER_DATA_PROVIDER_BUILD)
#define PS_DATA_PROVIDER_EXPORT __declspec(dllexport)
#else
#define PS_DATA_PROVIDER_EXPORT
#endif
#else
#define PS_DATA_PROVIDER_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Numeric version of the data-definition/provider ABI. */
#define PS_DATA_PROVIDER_ABI_VERSION_1 1U

/**
 * @brief One immutable provider-defined data schema.
 *
 * @note Schemas define operation/Value data facts and grant no execution or
 * storage capability.
 */
typedef struct ps_data_schema_v1 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Nonempty UTF-8 schema key. */
  const char* key;
  /** @brief Exact schema-key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief Scalar element type compatible with operation ABI values. */
  uint32_t element_type;
  /** @brief Maximum supported rank in 1..8. */
  uint32_t maximum_rank;
} ps_data_schema_v1;

/**
 * @brief Complete version-one data-provider table.
 *
 * @note The host validates and copies every schema before registry publication.
 */
typedef struct ps_data_provider_api_v1 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Number of entries in `schemas`. */
  uint32_t schema_count;
  /** @brief Immutable schema array. */
  const ps_data_schema_v1* schemas;
  /**
   * @brief Releases provider-owned records exactly once.
   * @param schemas Original schema array.
   * @param schema_count Original count.
   * @note Called after all copied definitions and provider leases retire.
   */
  void (*destroy)(const ps_data_schema_v1* schemas, uint32_t schema_count);
} ps_data_provider_api_v1;

/**
 * @brief Returns the provider ABI version without side effects.
 * @return `PS_DATA_PROVIDER_ABI_VERSION_1` for this header.
 * @note The function must not throw across the C boundary.
 */
PS_DATA_PROVIDER_EXPORT uint32_t ps_data_provider_get_abi_version(void);

/**
 * @brief Returns the immutable version-one provider table.
 * @return Nonnull table whose `struct_size` and records are exact.
 * @note The table remains valid until the host invokes its destroy callback.
 */
PS_DATA_PROVIDER_EXPORT const ps_data_provider_api_v1*
ps_data_provider_get_api_v1(void);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_DATA_PROVIDER_API_H_
