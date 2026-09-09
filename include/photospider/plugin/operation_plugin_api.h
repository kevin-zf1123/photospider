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
#define PS_OPERATION_ABI_VERSION_7 7U
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
#define PS_OPERATION_SHAPE_SCALAR_V7 1U
/** @brief Output descriptor preserves the first input descriptor. */
#define PS_OPERATION_SHAPE_PRESERVE_FIRST_V7 2U
/** @brief All input descriptors match and output preserves them. */
#define PS_OPERATION_SHAPE_MATCH_INPUTS_V7 3U
/**
 * @brief Output uses an explicit descriptor-owned fixed dense shape.
 * @note ABI v7 carries no output strides, so the host rejects fixed shapes
 * whose contiguous strides or byte count are not representable.
 */
#define PS_OPERATION_SHAPE_FIXED_V7 4U
/** @brief Ceil-divided spatial first input. */
#define PS_OPERATION_SHAPE_SHRINK_V7 5U

/** @brief Operation consumes and produces whole logical coverage. */
#define PS_OPERATION_REGION_WHOLE_V7 1U
/** @brief Region propagation is elementwise. */
#define PS_OPERATION_REGION_ELEMENTWISE_V7 2U
/** @brief Region propagation uses a symmetric nonzero halo. */
#define PS_OPERATION_REGION_HALO_V7 3U
/** @brief Clipped integer box footprint. */
#define PS_OPERATION_REGION_SHRINK_V7 4U

/** @brief C ABI scalar representation values matching the public C++ model. */
typedef enum ps_operation_element_type_v7 {
  PS_OPERATION_ELEMENT_UINT8_V7 = 1,
  PS_OPERATION_ELEMENT_INT64_V7 = 2,
  PS_OPERATION_ELEMENT_FLOAT64_V7 = 3,
  PS_OPERATION_ELEMENT_FLOAT32_V7 = 4
} ps_operation_element_type_v7;

/** @brief Closed source-parameter type values for operation ABI v7. */
typedef enum ps_operation_parameter_type_v7 {
  PS_OPERATION_PARAMETER_INT64_V7 = 1,
  PS_OPERATION_PARAMETER_FLOAT64_V7 = 2,
  PS_OPERATION_PARAMETER_BOOL_V7 = 3,
  PS_OPERATION_PARAMETER_STRING_V7 = 4
} ps_operation_parameter_type_v7;

/**
 * @brief Closed synchronous callback result values for operation ABI v7.
 *
 * @note Unknown nonzero integers fail closed as ordinary operation failures.
 * This enum does not change the `int` callback signature or descriptor layout.
 */
typedef enum ps_operation_result_v7 {
  /** @brief Callback completed and the output sink accepted one Value. */
  PS_OPERATION_RESULT_SUCCESS_V7 = 0,
  /** @brief Ordinary nonrecoverable operation failure. */
  PS_OPERATION_RESULT_FAILURE_V7 = 1,
  /** @brief Cooperative cancellation was observed by the callback. */
  PS_OPERATION_RESULT_CANCELLED_V7 = 2,
  /**
   * @brief Selected local backend cannot execute this invocation.
   * @note Valid only when the callback has not invoked the output sink.
   */
  PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7 = 3
} ps_operation_result_v7;

/**
 * @brief One immutable parameter declaration published by an operation.
 *
 * @note Key bytes remain plugin-owned until the API destroy callback.
 */
typedef struct ps_operation_parameter_descriptor_v7 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Nonempty bounded UTF-8 parameter key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief One `ps_operation_parameter_type_v7` value. */
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
} ps_operation_parameter_descriptor_v7;

/**
 * @brief One canonical source parameter supplied to a callback.
 *
 * @note Exactly the field selected by `type` is meaningful; every pointer is
 * callback-local and must not be retained.
 */
typedef struct ps_operation_parameter_value_v7 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief Nonempty schema-declared UTF-8 parameter key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief One `ps_operation_parameter_type_v7` value. */
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
} ps_operation_parameter_value_v7;

/**
 * @brief One bounded immutable semantic facet visible to an operation.
 *
 * @note Key and payload pointers remain valid only for the callback duration.
 */
typedef struct ps_operation_facet_view_v7 {
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
} ps_operation_facet_view_v7;

/**
 * @brief Immutable validated input Value view for one callback invocation.
 *
 * @note Every pointer remains valid only for the duration of the callback.
 */
typedef struct ps_operation_value_view_v7 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief One `ps_operation_element_type_v7` value. */
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
  const ps_operation_facet_view_v7* facets;
  /** @brief Rank-sized logical storage origin and signed byte strides. */
  const uint64_t* storage_origin;
  const int64_t* byte_strides;
  /** @brief Rank-sized valid coverage; accesses outside it are forbidden. */
  const uint64_t* region_offsets;
  const uint64_t* region_extents;
  /** @brief Byte offset of storage_origin from data (allocation start). */
  uint64_t byte_offset;
} ps_operation_value_view_v7;

/** @brief One invocation-local native buffer binding; no native handle escapes.
 * @note Token comes from buffer(), offset/size address only that view. Index is
 * 0..30. writable is 0 or 1 and cannot promote an immutable input to writable.
 */
typedef struct ps_gpu_buffer_binding_v7 {
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
} ps_gpu_buffer_binding_v7;

/** @brief One bounded trusted Metal compute dispatch, borrowed during execute.
 * @note Source is 1..262144 bytes, entry 1..128 bytes, bindings <=31. Constants
 * are at most 4096 bytes at a distinct index. Grid dimensions
 * are 1..UINT32_MAX. Safe math and no contraction are host policy; source is
 * trusted process code. Shader access must remain inside declared views;
 * binding validation is not a shader sandbox.
 */
typedef struct ps_gpu_dispatch_v7 {
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
  const ps_gpu_buffer_binding_v7* buffers;
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
} ps_gpu_dispatch_v7;

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
typedef struct ps_gpu_service_v7 {
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
   * @return A PS_OPERATION_RESULT_*_V7 code; failure becomes sticky.
   * @note At most 1024 views per invocation; publication revokes write access.
   */
  int (*buffer)(void* context, const uint8_t* bytes, uint64_t byte_size,
                uint32_t writable, uint64_t* token);
  /** @brief Executes trusted dispatch records synchronously.
   * @param context This service's host state.
   * @param commands Naturally aligned borrowed dispatch array.
   * @param command_count Array size in 1..32.
   * @return A PS_OPERATION_RESULT_*_V7 code; submitted device failure ends Run.
   * @note Successful return permits CPU access to the completed shared bytes.
   * Retained pipelines are internal; callers must not retain service pointers.
   */
  int (*execute)(void* context, const ps_gpu_dispatch_v7* commands,
                 uint32_t command_count);
} ps_gpu_service_v7;

/**
 * @brief Host-owned sink used to publish the requested output Region.
 *
 * @note Publication freezes host output or copies external data synchronously.
 */
typedef struct ps_operation_output_sink_v7 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief Opaque host state returned unchanged to `publish`. */
  void* context;
  /**
   * @brief Validates and freezes/copies the requested output Region.
   * @param context Opaque host state.
   * @param element_type One `ps_operation_element_type_v7` value.
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
                 uint32_t rank, const ps_operation_facet_view_v7* facets,
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
  const ps_gpu_service_v7* gpu;
  /** @brief Resolved output metadata, borrowed for this callback only.
   * @note Pass these canonical facets to publish for declared semantic output.
   */
  uint32_t output_element_type;
  uint32_t output_facet_count;
  const ps_operation_facet_view_v7* output_facets;
} ps_operation_output_sink_v7;

/**
 * @brief Callback that reports whether cooperative cancellation was requested.
 * @param context Invocation-local host cancellation state.
 * @return Nonzero after cancellation is requested; zero otherwise.
 * @note The plugin must not retain the context or throw across the C boundary.
 */
typedef int (*ps_operation_cancelled_v7)(void* context);

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
 * @return One closed `ps_operation_result_v7` value. Unknown nonzero values
 * are treated as `PS_OPERATION_RESULT_FAILURE_V7` by the host.
 * @throws Nothing; plugins must not let exceptions cross this pure-C ABI
 * callback boundary.
 * @note The callback must not throw across the C boundary or retain pointers.
 * `PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7` requests CPU fallback only for
 * a GPU attempt whose copied traits permit it; the output sink must not be
 * invoked for that result. If it was invoked, a rejected output keeps its
 * exact sink failure and an accepted output becomes a terminal ordinary
 * contract failure, so neither case can request fallback. Invoking the sink
 * more than once is always a terminal ordinary contract failure regardless of
 * the callback result and never requests fallback. Host cancellation remains
 * authoritative over the callback result and sink state.
 */
typedef int (*ps_operation_execute_v7)(
    void* user_data, const ps_operation_value_view_v7* inputs,
    uint32_t input_count, const ps_operation_parameter_value_v7* parameters,
    uint32_t parameter_count, uint32_t backend,
    ps_operation_cancelled_v7 cancelled, void* cancellation_context,
    const ps_operation_output_sink_v7* sink, char* diagnostic,
    size_t diagnostic_capacity);

/** @brief Generic Value port with existing Region rules. */
#define PS_OPERATION_PORT_VALUE_V7 1U
/** @brief Complete Float32 {1} constrained by a finite interval.
 * @note Accepts generic or dimensionless scalar/single-sample signal semantics.
 * Computed views may be padded/unaligned/strided; use the logical sample
 * address.
 */
#define PS_OPERATION_PORT_FLOAT32_SCALAR_V7 2U
/** @brief Dense HWC Float32 RGBA with exact linear premultiplied profile. */
#define PS_OPERATION_PORT_RGBA_FLOAT32_V7 3U
/** @brief Float32 {H,W} with canonical typed coverage and finite [0,1]. */
#define PS_OPERATION_PORT_FLOAT32_MASK_V7 4U

/** @brief Typed metadata constraint; all storage remains plugin-owned until
 * destroy. */
typedef struct ps_operation_semantic_constraint_v7 {
  uint32_t struct_size;
  uint32_t semantic_kind;
  uint32_t element_type;
  uint32_t rank;
  uint32_t facet_count;
  const ps_operation_facet_view_v7* facets;
  /** @brief Allowed dtype bits (element code - 1), low four bits only.
   * Zero adds no restriction; nonzero element_type and mask are mutually
   * exclusive.
   */
  uint32_t element_type_mask;
} ps_operation_semantic_constraint_v7;

/** @brief Typed semantic port, conservatively Whole in ABI 7. */
#define PS_OPERATION_PORT_TYPED_V7 5U
/** @brief Independent statically resolved output axes. */
#define PS_OPERATION_SHAPE_AXES_V7 6U
/** @brief Dtype selection: declared, input, or static String parameter. */
#define PS_OPERATION_DTYPE_DECLARED_V7 0U
#define PS_OPERATION_DTYPE_INPUT_V7 1U
#define PS_OPERATION_DTYPE_PARAMETER_V7 2U
/** @brief Extent source: constant, positive Int64 parameter, input axis, count.
 */
#define PS_OPERATION_EXTENT_CONSTANT_V7 0U
#define PS_OPERATION_EXTENT_PARAMETER_V7 1U
#define PS_OPERATION_EXTENT_INPUT_AXIS_V7 2U
#define PS_OPERATION_EXTENT_INPUT_COUNT_V7 3U
#define PS_OPERATION_EXTENT_INDEX_LIST_COUNT_V7 4U
/** @brief Semantic inference: drop, preserve input, establish facets,
 * parameter. */
#define PS_OPERATION_SEMANTIC_DROP_V7 0U
#define PS_OPERATION_SEMANTIC_PRESERVE_V7 1U
#define PS_OPERATION_SEMANTIC_ESTABLISH_V7 2U
#define PS_OPERATION_SEMANTIC_PARAMETER_V7 3U
#define PS_OPERATION_SEMANTIC_EXTRACT_CHANNEL_V7 4U
#define PS_OPERATION_SEMANTIC_SWIZZLE_CHANNELS_V7 5U
#define PS_OPERATION_SEMANTIC_MERGE_CHANNELS_PARAMETER_V7 6U
#define PS_OPERATION_SEMANTIC_ASSOCIATE_ALPHA_V7 7U
#define PS_OPERATION_SEMANTIC_UNASSOCIATE_ALPHA_V7 8U
#define PS_OPERATION_SEMANTIC_RGB_TO_XYZ_V7 9U
#define PS_OPERATION_SEMANTIC_XYZ_TO_RGB_V7 10U
#define PS_OPERATION_SEMANTIC_XYZ_TO_LAB_V7 11U
#define PS_OPERATION_SEMANTIC_LAB_TO_XYZ_V7 12U
/** @brief Requires start/step Float64 and an expression String parameter;
 * generic Float64 coefficient input [K], K=1..256; Float32 output [1..1048576].
 */
#define PS_OPERATION_SEMANTIC_SAMPLE_EXPRESSION_V7 13U
/** @brief Float32 Signal query + Signal/Lut [N>=2] table; parameter
 * reject/clip. Query sample units match table axis units; output semantics are
 * dropped.
 */
#define PS_OPERATION_SEMANTIC_APPLY_LUT_1D_V7 14U

/** @brief Checked static extent; parameter pointer/count is null/zero when
 * unused. */
typedef struct ps_operation_extent_v7 {
  uint32_t struct_size;
  uint32_t source;
  uint64_t constant;
  const char* parameter;
  uint32_t parameter_size;
  uint32_t input;
  uint32_t axis;
  uint64_t offset;
} ps_operation_extent_v7;

/** @brief Optional declarative output and repeated-input contract.
 * @note Exact size/alignment and pointer/count bounds are checked before copy.
 * Dtype/semantic parameters are required String schema entries, except
 * extract's required Int64 index. Swizzle and INDEX_LIST_COUNT use canonical
 * comma-separated decimal indices (1..64 entries, each 0..63, no spaces/leading
 * zeroes). Alpha/color transforms have no semantic parameter. New transforms
 * are Whole; their metadata checks and output facets are shared with C++
 * inference. axes has 1..8 records only for AXES. A repeated group follows
 * input_count fixed inputs; input_schema then has prefix+one template and
 * actual inputs are <=1024. All members of a homogeneous group must share dtype
 * and shape.
 */
typedef struct ps_operation_contract_v7 {
  uint32_t struct_size;
  uint32_t dtype_rule;
  uint32_t dtype_input;
  const char* dtype_parameter;
  uint32_t dtype_parameter_size;
  uint32_t axis_count;
  const ps_operation_extent_v7* axes;
  uint32_t repeated_minimum;
  uint32_t repeated_maximum;
  uint32_t repeated_match;
  uint32_t semantic_rule;
  uint32_t semantic_input;
  uint32_t output_facet_count;
  const ps_operation_facet_view_v7* output_facets;
  const char* semantic_parameter;
  uint32_t semantic_parameter_size;
} ps_operation_contract_v7;

/**
 * @brief Immutable copied port schema record, never retained by the compiler.
 * @note Unknown kinds or invalid combinations reject the complete plugin.
 * Scalars require finite inclusive endpoints; other kinds require zero bits.
 */
typedef struct ps_operation_port_constraint_v7 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief One PS_OPERATION_PORT_*_V7 kind. */
  uint32_t kind;
  /** @brief Numeric uint32 binary32 lower-bound bits, decoded with memcpy. */
  uint32_t minimum_bits;
  /** @brief Numeric uint32 binary32 upper-bound bits, decoded with memcpy. */
  uint32_t maximum_bits;
  /** @brief Optional typed/rank/dtype constraint, copied before publication. */
#ifdef __cplusplus
  const ps_operation_semantic_constraint_v7* semantic = nullptr;
#else
  const ps_operation_semantic_constraint_v7* semantic;
#endif
} ps_operation_port_constraint_v7;

/**
 * @brief One immutable operation descriptor published by a plugin.
 *
 * @note `key` and user_data remain valid until the plugin API destroy callback.
 */
typedef struct ps_operation_descriptor_v7 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Nonempty UTF-8 operation key. */
  const char* key;
  /** @brief Exact operation-key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief Exact count, or fixed prefix before a repeated contract group. */
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
   * @note Since ABI v7 carries no strides, the loader derives a contiguous
   * stride chain with checked uint64 products. Every stored stride must fit
   * int64, and complete byte count B must satisfy `B > 0`,
   * `B - 1 <= INT64_MAX`, and `B <= SIZE_MAX` before publication.
   */
  const uint64_t* output_shape;
  /** @brief One `PS_OPERATION_SHAPE_*_V7` inference rule. */
  uint32_t shape_rule;
  /** @brief One `PS_OPERATION_REGION_*_V7` propagation rule. */
  uint32_t region_rule;
  /** @brief Symmetric halo, nonzero only for REGION_HALO. */
  uint32_t halo_radius;
  /** @brief Nonzero when derived local result caching is semantically legal. */
  uint32_t cacheable;
  /** @brief Number of records in `parameters`, bounded by 128. */
  uint32_t parameter_count;
  /** @brief Parameter declarations, null only when count is zero. */
  const ps_operation_parameter_descriptor_v7* parameters;
  /** @brief Exact count, or input_count+one template for a repeated group. */
  uint32_t input_schema_count;
  /**
   * @brief Naturally aligned array, null exactly when count is zero.
   * @note Records and array remain valid until destroy. Host copies every
   * constraint before atomic registry publication; no callback may mutate it.
   */
  const ps_operation_port_constraint_v7* input_schema;
  /** @brief Exact-sized Value or image output constraint; scalar forbidden. */
  ps_operation_port_constraint_v7 output_schema;
  /** @brief Required synchronous callback. */
  ps_operation_execute_v7 execute;
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
  /** @brief Optional declarative contract, owned until plugin destroy. */
#ifdef __cplusplus
  const ps_operation_contract_v7* contract = nullptr;
#else
  const ps_operation_contract_v7* contract;
#endif
} ps_operation_descriptor_v7;

/**
 * @brief Complete version-seven plugin table.
 *
 * @note The host validates and copies all descriptors before publication.
 */
typedef struct ps_operation_plugin_api_v7 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Number of descriptors in `operations`. */
  uint32_t operation_count;
  /** @brief Immutable descriptor array. */
  const ps_operation_descriptor_v7* operations;
  /**
   * @brief Releases plugin-owned descriptor/user state exactly once.
   * @param operations Original descriptor array.
   * @param operation_count Original count.
   * @note The host calls this only after every invocation lease is released.
   */
  void (*destroy)(const ps_operation_descriptor_v7* operations,
                  uint32_t operation_count);
} ps_operation_plugin_api_v7;

/**
 * @brief Returns the plugin ABI version without side effects.
 * @return `PS_OPERATION_ABI_VERSION_7` for this header.
 * @note The function must not throw across the C boundary.
 */
PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version(void);

/**
 * @brief Returns the immutable version-seven plugin table.
 * @return Nonnull table whose `struct_size` is exact.
 * @note The table remains valid until the host invokes its destroy callback.
 */
PS_OPERATION_EXPORT const ps_operation_plugin_api_v7*
ps_operation_plugin_get_api_v7(void);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_PLUGIN_API_H_
