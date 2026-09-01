#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_PLUGIN_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_PLUGIN_API_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
#define PS_OPERATION_EXPORT __declspec(dllexport)
#else
#define PS_OPERATION_EXPORT
#endif
#else
#define PS_OPERATION_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Numeric version of the operation DSO ABI. */
#define PS_OPERATION_ABI_VERSION_1 1U
/** @brief Operation has deterministic output for equal inputs/parameters. */
#define PS_OPERATION_FLAG_DETERMINISTIC (1U << 0U)
/** @brief Operation has no externally visible side effect. */
#define PS_OPERATION_FLAG_SIDE_EFFECT_FREE (1U << 1U)
/** @brief Operation can execute on the required CPU backend. */
#define PS_OPERATION_FLAG_CPU (1U << 2U)
/** @brief Operation can execute on the optional local GPU lane. */
#define PS_OPERATION_FLAG_GPU (1U << 3U)
/** @brief GPU failure may use a semantically equivalent CPU implementation. */
#define PS_OPERATION_FLAG_CPU_FALLBACK (1U << 4U)

/** @brief Output shape is one scalar element. */
#define PS_OPERATION_SHAPE_SCALAR_V1 1U
/** @brief Output descriptor preserves the first input descriptor. */
#define PS_OPERATION_SHAPE_PRESERVE_FIRST_V1 2U
/** @brief All input descriptors match and output preserves them. */
#define PS_OPERATION_SHAPE_MATCH_INPUTS_V1 3U

/** @brief Operation consumes and produces whole logical coverage. */
#define PS_OPERATION_REGION_WHOLE_V1 1U
/** @brief Region propagation is elementwise. */
#define PS_OPERATION_REGION_ELEMENTWISE_V1 2U
/** @brief Region propagation uses a symmetric nonzero halo. */
#define PS_OPERATION_REGION_HALO_V1 3U

/** @brief C ABI scalar representation values matching the public C++ model. */
typedef enum ps_operation_element_type_v1 {
  PS_OPERATION_ELEMENT_UINT8_V1 = 1,
  PS_OPERATION_ELEMENT_INT64_V1 = 2,
  PS_OPERATION_ELEMENT_FLOAT64_V1 = 3
} ps_operation_element_type_v1;

/**
 * @brief One bounded immutable semantic facet visible to an operation.
 *
 * @note Key and payload pointers remain valid only for the callback duration.
 */
typedef struct ps_operation_facet_view_v1 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Nonempty printable-ASCII facet key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief Positive facet schema version. */
  uint32_t version;
  /** @brief Immutable payload or null only when payload_size is zero. */
  const uint8_t* payload;
  /** @brief Bounded opaque payload byte count. */
  uint32_t payload_size;
} ps_operation_facet_view_v1;

/**
 * @brief Immutable validated input Value view for one callback invocation.
 *
 * @note Every pointer remains valid only for the duration of the callback.
 */
typedef struct ps_operation_value_view_v1 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief One `ps_operation_element_type_v1` value. */
  uint32_t element_type;
  /** @brief Rank in the inclusive range 1..8. */
  uint32_t rank;
  /** @brief Number of immutable payload bytes. */
  uint64_t byte_size;
  /** @brief Rank-sized nonzero shape array. */
  const uint64_t* shape;
  /** @brief Immutable payload or null only when byte_size is zero. */
  const uint8_t* data;
  /** @brief Number of bounded records in `facets`. */
  uint32_t facet_count;
  /** @brief Immutable facet array or null only when facet_count is zero. */
  const ps_operation_facet_view_v1* facets;
} ps_operation_value_view_v1;

/**
 * @brief Host-owned sink used to publish one complete output Value.
 *
 * @note The callback copies data synchronously and returns nonzero on success.
 */
typedef struct ps_operation_output_sink_v1 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief Opaque host state returned unchanged to `publish`. */
  void* context;
  /**
   * @brief Copies and validates one complete output.
   * @param context Opaque host state.
   * @param element_type One `ps_operation_element_type_v1` value.
   * @param shape Rank-sized nonzero shape array.
   * @param rank Rank in 1..8.
   * @param facets Bounded facet array, null only when facet_count is zero.
   * @param facet_count Number of facet records in 0..64.
   * @param data Payload pointer, nonnull when byte_size is nonzero.
   * @param byte_size Exact payload byte count.
   * @return Nonzero when output was accepted; zero on validation/allocation
   * failure.
   * @note The host copies all bytes before return and invokes this at most
   * once.
   */
  int (*publish)(void* context, uint32_t element_type, const uint64_t* shape,
                 uint32_t rank, const ps_operation_facet_view_v1* facets,
                 uint32_t facet_count, const uint8_t* data, uint64_t byte_size);
} ps_operation_output_sink_v1;

/**
 * @brief Callback that reports whether cooperative cancellation was requested.
 * @param context Invocation-local host cancellation state.
 * @return Nonzero after cancellation is requested; zero otherwise.
 * @note The plugin must not retain the context or throw across the C boundary.
 */
typedef int (*ps_operation_cancelled_v1)(void* context);

/**
 * @brief Synchronous operation execution callback.
 * @param user_data Descriptor-owned opaque state.
 * @param inputs Array of `input_count` immutable Value views.
 * @param input_count Exact descriptor input count.
 * @param backend 1 for CPU or 2 for optional local GPU.
 * @param cancelled Host cancellation callback.
 * @param cancellation_context Opaque cancellation state.
 * @param sink Host-owned single-output sink.
 * @param diagnostic Writable diagnostic buffer.
 * @param diagnostic_capacity Writable buffer size including terminator.
 * @return Zero on success; nonzero on operation failure/cancellation.
 * @note The callback must not throw across the C boundary or retain pointers.
 */
typedef int (*ps_operation_execute_v1)(
    void* user_data, const ps_operation_value_view_v1* inputs,
    uint32_t input_count, uint32_t backend, ps_operation_cancelled_v1 cancelled,
    void* cancellation_context, const ps_operation_output_sink_v1* sink,
    char* diagnostic, size_t diagnostic_capacity);

/**
 * @brief One immutable operation descriptor published by a plugin.
 *
 * @note `key` and user_data remain valid until the plugin API destroy callback.
 */
typedef struct ps_operation_descriptor_v1 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Nonempty UTF-8 operation key. */
  const char* key;
  /** @brief Exact operation-key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief Exact number of ordered input Values. */
  uint32_t input_count;
  /** @brief Bitwise `PS_OPERATION_FLAG_*` semantic traits. */
  uint32_t flags;
  /** @brief Estimated peak invocation bytes for local resource admission. */
  uint64_t estimated_bytes;
  /** @brief Scalar element type used by static output inference. */
  uint32_t output_element_type;
  /** @brief One `PS_OPERATION_SHAPE_*_V1` inference rule. */
  uint32_t shape_rule;
  /** @brief One `PS_OPERATION_REGION_*_V1` propagation rule. */
  uint32_t region_rule;
  /** @brief Symmetric halo, nonzero only for REGION_HALO. */
  uint32_t halo_radius;
  /** @brief Nonzero when derived local result caching is semantically legal. */
  uint32_t cacheable;
  /** @brief Required synchronous callback. */
  ps_operation_execute_v1 execute;
  /** @brief Descriptor-owned opaque callback state, possibly null. */
  void* user_data;
} ps_operation_descriptor_v1;

/**
 * @brief Complete version-one plugin table.
 *
 * @note The host validates and copies all descriptors before publication.
 */
typedef struct ps_operation_plugin_api_v1 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Number of descriptors in `operations`. */
  uint32_t operation_count;
  /** @brief Immutable descriptor array. */
  const ps_operation_descriptor_v1* operations;
  /**
   * @brief Releases plugin-owned descriptor/user state exactly once.
   * @param operations Original descriptor array.
   * @param operation_count Original count.
   * @note The host calls this only after every invocation lease is released.
   */
  void (*destroy)(const ps_operation_descriptor_v1* operations,
                  uint32_t operation_count);
} ps_operation_plugin_api_v1;

/**
 * @brief Returns the plugin ABI version without side effects.
 * @return `PS_OPERATION_ABI_VERSION_1` for this header.
 * @note The function must not throw across the C boundary.
 */
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void);

/**
 * @brief Returns the immutable version-one plugin table.
 * @return Nonnull table whose `struct_size` is exact.
 * @note The table remains valid until the host invokes its destroy callback.
 */
PS_OPERATION_EXPORT const ps_operation_plugin_api_v1*
ps_operation_plugin_get_api_v1(void);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_PLUGIN_API_H_
