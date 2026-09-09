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
#define PS_OPERATION_ABI_VERSION_6 6U
/** @brief Operation has deterministic output for equal inputs/parameters. */
#define PS_OPERATION_FLAG_DETERMINISTIC (1U << 0U)
/** @brief Operation has no externally visible side effect. */
#define PS_OPERATION_FLAG_SIDE_EFFECT_FREE (1U << 1U)
/** @brief Operation can execute on the required CPU backend. */
#define PS_OPERATION_FLAG_CPU (1U << 2U)
/** @brief Operation can execute on the optional local GPU lane. */
#define PS_OPERATION_FLAG_GPU (1U << 3U)
/** @brief Output-free GPU unavailability may use an equivalent CPU path. */
#define PS_OPERATION_FLAG_CPU_FALLBACK (1U << 4U)

/** @brief Output shape is one scalar element. */
#define PS_OPERATION_SHAPE_SCALAR_V6 1U
/** @brief Output descriptor preserves the first input descriptor. */
#define PS_OPERATION_SHAPE_PRESERVE_FIRST_V6 2U
/** @brief All input descriptors match and output preserves them. */
#define PS_OPERATION_SHAPE_MATCH_INPUTS_V6 3U
/**
 * @brief Output uses an explicit descriptor-owned fixed dense shape.
 * @note ABI v6 carries no output strides, so the host rejects fixed shapes
 * whose contiguous strides or byte count are not representable.
 */
#define PS_OPERATION_SHAPE_FIXED_V6 4U
/** @brief Ceil-divided spatial first input. */
#define PS_OPERATION_SHAPE_SHRINK_V6 5U

/** @brief Operation consumes and produces whole logical coverage. */
#define PS_OPERATION_REGION_WHOLE_V6 1U
/** @brief Region propagation is elementwise. */
#define PS_OPERATION_REGION_ELEMENTWISE_V6 2U
/** @brief Region propagation uses a symmetric nonzero halo. */
#define PS_OPERATION_REGION_HALO_V6 3U
/** @brief Clipped integer box footprint. */
#define PS_OPERATION_REGION_SHRINK_V6 4U

/** @brief C ABI scalar representation values matching the public C++ model. */
typedef enum ps_operation_element_type_v6 {
  PS_OPERATION_ELEMENT_UINT8_V6 = 1,
  PS_OPERATION_ELEMENT_INT64_V6 = 2,
  PS_OPERATION_ELEMENT_FLOAT64_V6 = 3,
  PS_OPERATION_ELEMENT_FLOAT32_V6 = 4
} ps_operation_element_type_v6;

/** @brief Closed source-parameter type values for operation ABI v6. */
typedef enum ps_operation_parameter_type_v6 {
  PS_OPERATION_PARAMETER_INT64_V6 = 1,
  PS_OPERATION_PARAMETER_FLOAT64_V6 = 2,
  PS_OPERATION_PARAMETER_BOOL_V6 = 3,
  PS_OPERATION_PARAMETER_STRING_V6 = 4
} ps_operation_parameter_type_v6;

/**
 * @brief Closed synchronous callback result values for operation ABI v6.
 *
 * @note Unknown nonzero integers fail closed as ordinary operation failures.
 * This enum does not change the `int` callback signature or descriptor layout.
 */
typedef enum ps_operation_result_v6 {
  /** @brief Callback completed and the output sink accepted one Value. */
  PS_OPERATION_RESULT_SUCCESS_V6 = 0,
  /** @brief Ordinary nonrecoverable operation failure. */
  PS_OPERATION_RESULT_FAILURE_V6 = 1,
  /** @brief Cooperative cancellation was observed by the callback. */
  PS_OPERATION_RESULT_CANCELLED_V6 = 2,
  /**
   * @brief Selected local backend cannot execute this invocation.
   * @note Valid only when the callback has not invoked the output sink.
   */
  PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V6 = 3
} ps_operation_result_v6;

/**
 * @brief One immutable parameter declaration published by an operation.
 *
 * @note Key bytes remain plugin-owned until the API destroy callback.
 */
typedef struct ps_operation_parameter_descriptor_v6 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Nonempty bounded UTF-8 parameter key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief One `ps_operation_parameter_type_v6` value. */
  uint32_t type;
  /** @brief Zero for optional or one for required. */
  uint32_t required;
  /** @brief Optional finite numeric interval; zero fields when unbounded. */
#ifdef __cplusplus
  uint32_t bounded = 0;
  double minimum = 0;
  double maximum = 0;
#else
  uint32_t bounded;
  double minimum;
  double maximum;
#endif
} ps_operation_parameter_descriptor_v6;

/**
 * @brief One canonical source parameter supplied to a callback.
 *
 * @note Exactly the field selected by `type` is meaningful; every pointer is
 * callback-local and must not be retained.
 */
typedef struct ps_operation_parameter_value_v6 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief Nonempty schema-declared UTF-8 parameter key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief One `ps_operation_parameter_type_v6` value. */
  uint32_t type;
  /** @brief Value when type is `INT64`. */
  int64_t int64_value;
  /** @brief Value when type is `FLOAT64`. */
  double float64_value;
  /** @brief Zero or one when type is `BOOL`. */
  uint32_t bool_value;
  /** @brief UTF-8-like bytes when type is `STRING`, otherwise null. */
  const char* string_value;
  /** @brief Exact string byte count, otherwise zero. */
  uint32_t string_size;
} ps_operation_parameter_value_v6;

/**
 * @brief One bounded immutable semantic facet visible to an operation.
 *
 * @note Key and payload pointers remain valid only for the callback duration.
 */
typedef struct ps_operation_facet_view_v6 {
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
} ps_operation_facet_view_v6;

/**
 * @brief Immutable validated input Value view for one callback invocation.
 *
 * @note Every pointer remains valid only for the duration of the callback.
 */
typedef struct ps_operation_value_view_v6 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief One `ps_operation_element_type_v6` value. */
  uint32_t element_type;
  /** @brief Rank in the inclusive range 1..8. */
  uint32_t rank;
  /** @brief Number of immutable payload bytes. */
  uint64_t byte_size;
  /** @brief Rank-sized nonzero shape array. */
  const uint64_t* shape;
  /** @brief Rank-sized planned input-demand offsets. */
  const uint64_t* demand_offsets;
  /** @brief Rank-sized planned input-demand extents. */
  const uint64_t* demand_extents;
  /** @brief Immutable payload or null only when byte_size is zero. */
  const uint8_t* data;
  /** @brief Number of bounded records in `facets`. */
  uint32_t facet_count;
  /** @brief Immutable facet array or null only when facet_count is zero. */
  const ps_operation_facet_view_v6* facets;
  /** @brief Rank-sized logical storage origin and signed byte strides. */
  const uint64_t* storage_origin;
  const int64_t* byte_strides;
  /** @brief Rank-sized valid coverage; accesses outside it are forbidden. */
  const uint64_t* region_offsets;
  const uint64_t* region_extents;
  /** @brief Byte offset of storage_origin from data (allocation start). */
  uint64_t byte_offset;
} ps_operation_value_view_v6;

/** @brief One invocation-local native buffer binding; no native handle escapes.
 * @note Token comes from buffer(), offset/size address only that view. Index is
 * 0..30. writable is 0 or 1 and cannot promote an immutable input to writable.
 */
typedef struct ps_gpu_buffer_binding_v6 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Unique Metal buffer argument index in 0..30. */
  uint32_t index;
  /** @brief Nonzero token returned by this invocation's buffer service. */
  uint64_t token;
  /** @brief Byte offset relative to the acquired view.
   * @note The resulting offset into the underlying buffer must align to 4
   * bytes.
   */
  uint64_t offset;
  /** @brief Positive accessible byte count within the acquired view. */
  uint64_t byte_size;
  /** @brief Zero for read-only access, one for permitted mutable access. */
  uint32_t writable;
} ps_gpu_buffer_binding_v6;

/** @brief One bounded trusted Metal compute dispatch, borrowed during execute.
 * @note Source is 1..262144 bytes, entry 1..128 bytes, bindings <=31. Constants
 * are at most 4096 bytes at a distinct index. Grid dimensions
 * are 1..UINT32_MAX. Safe math and no contraction are host policy; source is
 * trusted process code. Shader access must remain inside declared views;
 * binding validation is not a shader sandbox.
 */
typedef struct ps_gpu_dispatch_v6 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief UTF-8 MSL source, borrowed without requiring a terminator. */
  const char* source;
  /** @brief Exact source byte count. */
  uint32_t source_size;
  /** @brief UTF-8 entry name, borrowed without requiring a terminator. */
  const char* entry;
  /** @brief Exact entry-name byte count. */
  uint32_t entry_size;
  /** @brief Naturally aligned binding array, nullable when count is zero. */
  const ps_gpu_buffer_binding_v6* buffers;
  /** @brief Binding count, with unique indexes distinct from constants. */
  uint32_t buffer_count;
  /** @brief Borrowed constant bytes, nullable when constant_size is zero. */
  const void* constants;
  /** @brief Constant byte count; copied into command metadata by the host. */
  uint32_t constant_size;
  /** @brief Buffer argument index for nonempty constants, in 0..30. */
  uint32_t constant_index;
  /** @brief Positive total thread counts along x, y and z. */
  uint64_t grid[3];
} ps_gpu_dispatch_v6;

/** @brief Synchronous host GPU services; null for CPU invocations.
 * @note All tokens and pointers expire at callback return. Host buffers alone
 * are eligible. Calls never throw; failures are sticky and override callback
 * success. execute accepts 1..32 dispatches and drains submitted native work
 * before returning, including cancellation/failure. Use only on the invoking
 * callback thread; do not access mutable buffer bytes concurrently with
 * execute. The service owns queue and pipelines. Output and scratch allocations
 * use the enclosing sink services and count against the host's live buffer
 * budget.
 */
typedef struct ps_gpu_service_v6 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief Borrowed host state passed unchanged to both functions. */
  void* context;
  /** @brief Acquires a bounded buffer token; writes token only on success.
   * @param context This service's host state.
   * @param bytes Start of a live host native input, output or scratch view.
   * @param byte_size Positive view size within the allocation payload.
   * @param writable Zero or one; immutable inputs cannot become writable.
   * @param token Nonnull destination for the invocation-local token.
   * @return A PS_OPERATION_RESULT_*_V6 code; failure becomes sticky.
   * @note At most 1024 views per invocation; publication revokes write access.
   */
  int (*buffer)(void* context, const uint8_t* bytes, uint64_t byte_size,
                uint32_t writable, uint64_t* token);
  /** @brief Executes trusted dispatch records synchronously.
   * @param context This service's host state.
   * @param commands Naturally aligned borrowed dispatch array.
   * @param command_count Array size in 1..32.
   * @return A PS_OPERATION_RESULT_*_V6 code; submitted device failure ends Run.
   * @note Successful return permits CPU access to the completed shared bytes.
   * Retained pipelines are internal; callers must not retain service pointers.
   */
  int (*execute)(void* context, const ps_gpu_dispatch_v6* commands,
                 uint32_t command_count);
} ps_gpu_service_v6;

/**
 * @brief Host-owned sink used to publish the requested output Region.
 *
 * @note Publication freezes host output or copies external data synchronously.
 */
typedef struct ps_operation_output_sink_v6 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief Opaque host state returned unchanged to `publish`. */
  void* context;
  /**
   * @brief Validates and freezes/copies the requested output Region.
   * @param context Opaque host state.
   * @param element_type One `ps_operation_element_type_v6` value.
   * @param shape Rank-sized nonzero shape array.
   * @param rank Rank in 1..8.
   * @param facets Bounded facet array, null only when facet_count is zero.
   * @param facet_count Number of facet records in 0..64.
   * @param data Payload pointer, nonnull when byte_size is nonzero.
   * @param byte_size Exact payload byte count.
   * @return Nonzero when the first output was accepted; zero for null context,
   * first-call validation/allocation failure, or any second invocation.
   * @throws Nothing; exceptions never cross this pure-C ABI callback boundary.
   * @note The host freezes allocate_output storage or copies external bytes
   * before return. The first invocation claims the sink even when validation
   * rejects it. Any second invocation violates the callback contract and makes
   * the complete operation fail closed; it never replaces the first copied or
   * rejected result.
   */
  int (*publish)(void* context, uint32_t element_type, const uint64_t* shape,
                 uint32_t rank, const ps_operation_facet_view_v6* facets,
                 uint32_t facet_count, const uint8_t* data, uint64_t byte_size);
  /** @brief Requested output descriptor and coverage; borrowed until return. */
  uint32_t output_rank;
  const uint64_t* output_shape;
  const uint64_t* output_offsets;
  const uint64_t* output_extents;
  uint64_t output_byte_size;
  /**
   * @brief Returns host-owned packed output bytes, or null on resource failure.
   * @note Repeated calls before publication return the same buffer. Publish
   * with this pointer freezes it without copying. Never free or retain it.
   */
  uint8_t* (*allocate_output)(void* context);
  /** @brief Allocates invocation-local scratch; null on failure, never free. */
  uint8_t* (*allocate_scratch)(void* context, uint64_t byte_size);
  /** @brief Invocation-local host Metal service, null on the CPU backend. */
  const ps_gpu_service_v6* gpu;
} ps_operation_output_sink_v6;

/**
 * @brief Callback that reports whether cooperative cancellation was requested.
 * @param context Invocation-local host cancellation state.
 * @return Nonzero after cancellation is requested; zero otherwise.
 * @note The plugin must not retain the context or throw across the C boundary.
 */
typedef int (*ps_operation_cancelled_v6)(void* context);

/**
 * @brief Synchronous operation execution callback.
 * @param user_data Descriptor-owned opaque state.
 * @param inputs Array of `input_count` immutable Value views.
 * @param input_count Exact descriptor input count.
 * @param parameters Canonically key-ordered validated parameter array.
 * @param parameter_count Exact number of supplied source parameters.
 * @param backend 1 for CPU or 2 for optional local GPU.
 * @param cancelled Host cancellation callback.
 * @param cancellation_context Opaque cancellation state.
 * @param sink Host-owned single-output sink.
 * @param diagnostic Writable diagnostic buffer.
 * @param diagnostic_capacity Writable buffer size including terminator.
 * @return One closed `ps_operation_result_v6` value. Unknown nonzero values
 * are treated as `PS_OPERATION_RESULT_FAILURE_V6` by the host.
 * @throws Nothing; plugins must not let exceptions cross this pure-C ABI
 * callback boundary.
 * @note The callback must not throw across the C boundary or retain pointers.
 * `PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V6` requests CPU fallback only for
 * a GPU attempt whose copied traits permit it; the output sink must not be
 * invoked for that result. If it was invoked, a rejected output keeps its
 * exact sink failure and an accepted output becomes a terminal ordinary
 * contract failure, so neither case can request fallback. Invoking the sink
 * more than once is always a terminal ordinary contract failure regardless of
 * the callback result and never requests fallback. Host cancellation remains
 * authoritative over the callback result and sink state.
 */
typedef int (*ps_operation_execute_v6)(
    void* user_data, const ps_operation_value_view_v6* inputs,
    uint32_t input_count, const ps_operation_parameter_value_v6* parameters,
    uint32_t parameter_count, uint32_t backend,
    ps_operation_cancelled_v6 cancelled, void* cancellation_context,
    const ps_operation_output_sink_v6* sink, char* diagnostic,
    size_t diagnostic_capacity);

/** @brief Generic Value port with existing Region rules. */
#define PS_OPERATION_PORT_VALUE_V6 1U
/** @brief Direct workflow Float32 scalar constrained by a finite interval. */
#define PS_OPERATION_PORT_FLOAT32_SCALAR_V6 2U
/** @brief Dense HWC Float32 RGBA with exact linear premultiplied profile. */
#define PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V6 3U
/** @brief Float32 {H,W} mask with no facets and finite [0,1] samples. */
#define PS_OPERATION_PORT_FLOAT32_MASK_V6 4U

/**
 * @brief Immutable copied port schema record, never retained by the compiler.
 * @note Unknown kinds or invalid combinations reject the complete plugin.
 * Scalars require finite inclusive endpoints; other kinds require zero bits.
 */
typedef struct ps_operation_port_constraint_v6 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief One PS_OPERATION_PORT_*_V6 kind. */
  uint32_t kind;
  /** @brief Numeric uint32 binary32 lower-bound bits, decoded with memcpy. */
  uint32_t minimum_bits;
  /** @brief Numeric uint32 binary32 upper-bound bits, decoded with memcpy. */
  uint32_t maximum_bits;
} ps_operation_port_constraint_v6;

/**
 * @brief One immutable operation descriptor published by a plugin.
 *
 * @note `key` and user_data remain valid until the plugin API destroy callback.
 */
typedef struct ps_operation_descriptor_v6 {
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
  /**
   * @brief Estimated peak invocation bytes for local resource admission.
   * @note This modeled estimate does not replace fixed dense representability
   * validation at DSO load.
   */
  uint64_t estimated_bytes;
  /** @brief Scalar element type used by static output inference. */
  uint32_t output_element_type;
  /** @brief Rank in 1..8 for FIXED and zero for every other shape rule. */
  uint32_t output_rank;
  /**
   * @brief Rank-sized dense fixed shape, null when output_rank is zero.
   * @note Since ABI v6 carries no strides, the loader derives a contiguous
   * stride chain with checked uint64 products. Every stored stride must fit
   * int64, and complete byte count B must satisfy `B > 0`,
   * `B - 1 <= INT64_MAX`, and `B <= SIZE_MAX` before publication.
   */
  const uint64_t* output_shape;
  /** @brief One `PS_OPERATION_SHAPE_*_V6` inference rule. */
  uint32_t shape_rule;
  /** @brief One `PS_OPERATION_REGION_*_V6` propagation rule. */
  uint32_t region_rule;
  /** @brief Symmetric halo, nonzero only for REGION_HALO. */
  uint32_t halo_radius;
  /** @brief Nonzero when derived local result caching is semantically legal. */
  uint32_t cacheable;
  /** @brief Number of records in `parameters`, bounded by 128. */
  uint32_t parameter_count;
  /** @brief Parameter declarations, null only when count is zero. */
  const ps_operation_parameter_descriptor_v6* parameters;
  /** @brief Must equal input_count, at most 1024. */
  uint32_t input_schema_count;
  /**
   * @brief Naturally aligned array, null exactly when count is zero.
   * @note Records and array remain valid until destroy. Host copies every
   * constraint before atomic registry publication; no callback may mutate it.
   */
  const ps_operation_port_constraint_v6* input_schema;
  /** @brief Exact-sized Value or image output constraint; scalar forbidden. */
  ps_operation_port_constraint_v6 output_schema;
  /** @brief Required synchronous callback. */
  ps_operation_execute_v6 execute;
  /** @brief Descriptor-owned opaque callback state, possibly null. */
  void* user_data;
  /** @brief Fixed scratch bound and additional bytes per demanded input byte.
   */
#ifdef __cplusplus
  uint64_t workspace_bytes = 0;
  uint32_t workspace_input_multiplier = 0;
  const char* halo_radius_parameter = nullptr;
  uint32_t halo_radius_parameter_size = 0;
  const char* spatial_factor_parameter = nullptr;
  uint32_t spatial_factor_parameter_size = 0;
#else
  uint64_t workspace_bytes;
  uint32_t workspace_input_multiplier;
  const char* halo_radius_parameter;
  uint32_t halo_radius_parameter_size;
  const char* spatial_factor_parameter;
  uint32_t spatial_factor_parameter_size;
#endif
} ps_operation_descriptor_v6;

/**
 * @brief Complete version-six plugin table.
 *
 * @note The host validates and copies all descriptors before publication.
 */
typedef struct ps_operation_plugin_api_v6 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Number of descriptors in `operations`. */
  uint32_t operation_count;
  /** @brief Immutable descriptor array. */
  const ps_operation_descriptor_v6* operations;
  /**
   * @brief Releases plugin-owned descriptor/user state exactly once.
   * @param operations Original descriptor array.
   * @param operation_count Original count.
   * @note The host calls this only after every invocation lease is released.
   */
  void (*destroy)(const ps_operation_descriptor_v6* operations,
                  uint32_t operation_count);
} ps_operation_plugin_api_v6;

/**
 * @brief Returns the plugin ABI version without side effects.
 * @return `PS_OPERATION_ABI_VERSION_6` for this header.
 * @note The function must not throw across the C boundary.
 */
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void);

/**
 * @brief Returns the immutable version-six plugin table.
 * @return Nonnull table whose `struct_size` is exact.
 * @note The table remains valid until the host invokes its destroy callback.
 */
PS_OPERATION_EXPORT const ps_operation_plugin_api_v6*
ps_operation_plugin_get_api_v6(void);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_PLUGIN_API_H_
