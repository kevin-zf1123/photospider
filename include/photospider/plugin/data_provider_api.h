#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_DATA_PROVIDER_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_DATA_PROVIDER_API_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @file data_provider_api.h
 * @brief Frozen dependency-neutral pure-C v3 data-definition provider ABI.
 *
 * The V-14 surface covers Schema, Facet, and Layout definitions plus bounded
 * validation, property, Region, DataSpec, canonical-content, and lifetime
 * callbacks. Access, conversion, execution, asynchronous work, and native
 * device handles are intentionally absent.
 *
 * @note The supported v3 process profile has 8-bit bytes, 8-byte pointers and
 * function pointers, natural 8-byte alignment, Host endianness for in-memory
 * records, and the platform C calling convention.
 */

#ifdef __cplusplus
extern "C" {
#define PS_DATA_NOEXCEPT noexcept
#else
#define PS_DATA_NOEXCEPT
#endif

#if defined(_WIN32)
#define PS_DATA_CALL __cdecl
#if defined(PS_DATA_PROVIDER_BUILD)
#define PS_DATA_PROVIDER_EXPORT __declspec(dllexport)
#else
#define PS_DATA_PROVIDER_EXPORT
#endif
#else
#define PS_DATA_CALL
#define PS_DATA_PROVIDER_EXPORT __attribute__((visibility("default")))
#endif

/** @brief Exact ABI generation implemented by this definition suite. */
#define PS_DATA_PROVIDER_ABI_VERSION 3U

/** @brief Exact frozen record sizes for v3 validation. */
#define PS_DATA_IDENTITY_V3_SIZE 16U
#define PS_DATA_BYTES_V3_SIZE 16U
#define PS_DATA_DEFINITION_V3_SIZE 64U
#define PS_DATA_EXTENSION_V3_SIZE 64U
#define PS_DATA_BUFFER_VIEW_V3_SIZE 56U
#define PS_DATA_BUFFER_ENVELOPE_V3_SIZE 48U
#define PS_DATA_VALUE_VIEW_V3_SIZE 88U
#define PS_DATA_DIAGNOSTIC_V3_SIZE 48U
#define PS_DATA_PROPERTY_QUERY_V3_SIZE 40U
#define PS_DATA_PROPERTY_RESULT_V3_SIZE 56U
#define PS_DATA_REGION_REQUEST_V3_SIZE 72U
#define PS_DATA_REGION_RESULT_V3_SIZE 40U
#define PS_DATA_SPEC_REQUEST_V3_SIZE 64U
#define PS_DATA_SPEC_RESULT_V3_SIZE 40U
#define PS_DATA_BYTE_SINK_V3_SIZE 40U
#define PS_DATA_PROVIDER_API_V3_SIZE 160U

/** @brief Stable provider callback status scalar. */
typedef uint32_t ps_data_status_v3;
/** @brief Callback completed successfully with a canonical output. */
#define PS_DATA_STATUS_OK_V3 0U
/** @brief Input framing or semantics were invalid. */
#define PS_DATA_STATUS_INVALID_ARGUMENT_V3 1U
/** @brief Provider or Host-owned staging exhausted memory. */
#define PS_DATA_STATUS_OUT_OF_MEMORY_V3 2U
/** @brief Requested definition or operation is unsupported. */
#define PS_DATA_STATUS_UNSUPPORTED_V3 3U
/** @brief Matching provider interpretation is unavailable. */
#define PS_DATA_STATUS_MISSING_PROVIDER_V3 4U
/** @brief Requested bounded representation exceeds its explicit budget. */
#define PS_DATA_STATUS_TOO_COMPLEX_V3 5U
/** @brief Provider encountered an internal failure. */
#define PS_DATA_STATUS_INTERNAL_ERROR_V3 6U

/** @brief Stable typed definition-kind scalar. */
typedef uint32_t ps_data_definition_kind_v3;
/** @brief Representation Schema definition namespace. */
#define PS_DATA_DEFINITION_SCHEMA_V3 1U
/** @brief Orthogonal Facet definition namespace. */
#define PS_DATA_DEFINITION_FACET_V3 2U
/** @brief Physical Layout definition namespace. */
#define PS_DATA_DEFINITION_LAYOUT_V3 3U

/** @brief Stable buffer-view flag scalar. */
typedef uint32_t ps_data_buffer_flags_v3;
/** @brief Buffer is host readable for the retained call. */
#define PS_DATA_BUFFER_HOST_VISIBLE_V3 1U
/** @brief `data` exposes payload for this non-pure callback only. */
#define PS_DATA_BUFFER_PAYLOAD_AVAILABLE_V3 2U

/** @brief Stable property-query state scalar. */
typedef uint32_t ps_data_property_state_v3;
/** @brief Property value is present in the result record. */
#define PS_DATA_PROPERTY_AVAILABLE_V3 1U
/** @brief Property has no meaning for this descriptor. */
#define PS_DATA_PROPERTY_NOT_APPLICABLE_V3 2U
/** @brief Property cannot be determined from current metadata. */
#define PS_DATA_PROPERTY_UNKNOWN_V3 3U
/** @brief Property requires explicitly scheduled non-pure work. */
#define PS_DATA_PROPERTY_DEFERRED_V3 4U
/** @brief Required definition provider is not currently available. */
#define PS_DATA_PROPERTY_MISSING_PROVIDER_V3 5U
/** @brief Provider exists but not for the requested Schema version. */
#define PS_DATA_PROPERTY_UNSUPPORTED_SCHEMA_VERSION_V3 6U
/** @brief Descriptor or provider output framing is invalid. */
#define PS_DATA_PROPERTY_INVALID_DESCRIPTOR_V3 7U

/** @brief Stable property-value representation scalar. */
typedef uint32_t ps_data_property_value_kind_v3;
/** @brief Result carries no value bytes or scalar. */
#define PS_DATA_PROPERTY_VALUE_NONE_V3 0U
/** @brief Result carries one unsigned 64-bit scalar. */
#define PS_DATA_PROPERTY_VALUE_UINT64_V3 1U
/** @brief Result carries one bounded borrowed byte sequence. */
#define PS_DATA_PROPERTY_VALUE_BYTES_V3 2U

/** @brief Stable Region request-kind scalar. */
typedef uint32_t ps_data_region_kind_v3;
/** @brief Request selects no logical sites. */
#define PS_DATA_REGION_EMPTY_V3 1U
/** @brief Request selects the complete logical value. */
#define PS_DATA_REGION_WHOLE_V3 2U
/** @brief Request carries one bounded rank-general TensorSlice. */
#define PS_DATA_REGION_TENSOR_SLICE_V3 3U
/** @brief Request cannot be represented by the current ABI generation. */
#define PS_DATA_REGION_UNSUPPORTED_V3 4U

/** @brief Stable provider Region outcome scalar. */
typedef uint32_t ps_data_region_state_v3;
/** @brief Provider evaluated the exact requested Region. */
#define PS_DATA_REGION_EXACT_V3 1U
/** @brief Provider cannot determine the Region outcome from metadata. */
#define PS_DATA_REGION_UNKNOWN_V3 2U
/** @brief Provider does not support the request kind or descriptor. */
#define PS_DATA_REGION_UNSUPPORTED_STATE_V3 3U
/** @brief Exact evaluation exceeded the explicit complexity budget. */
#define PS_DATA_REGION_TOO_COMPLEX_V3 4U

/** @brief Stable DataSpec set-relation scalar. */
typedef uint32_t ps_data_spec_relation_v3;
/** @brief Concrete value set is a subset of the requested DataSpec. */
#define PS_DATA_SPEC_SUBSET_V3 1U
/** @brief Concrete value set is disjoint from the requested DataSpec. */
#define PS_DATA_SPEC_DISJOINT_V3 2U
/** @brief Partial overlap requires an explicit runtime guard. */
#define PS_DATA_SPEC_PARTIAL_RUNTIME_GUARD_V3 3U
/** @brief Provider cannot evaluate the relation from current metadata. */
#define PS_DATA_SPEC_CANNOT_EVALUATE_V3 4U

/**
 * @brief Fixed process-independent 128-bit extension identity.
 * @note The words are numeric values; canonical persistence writes each word
 * in big-endian order independently of Host byte order.
 */
typedef struct ps_data_identity_v3 {
  /** @brief Most-significant identity word. */
  uint64_t high;
  /** @brief Least-significant identity word. */
  uint64_t low;
} ps_data_identity_v3;

/**
 * @brief Borrowed bounded byte view valid for one containing call.
 * @note `data` may be null only when `size` is zero.
 */
typedef struct ps_data_bytes_v3 {
  /** @brief Borrowed first byte, or null for an empty view. */
  const uint8_t* data;
  /** @brief Exact byte count representable by the containing bound. */
  uint64_t size;
} ps_data_bytes_v3;

/**
 * @brief One immutable typed definition advertised by a provider generation.
 * @note Names are diagnostic only and do not participate in identity.
 */
typedef struct ps_data_definition_v3 {
  /** @brief Must equal `PS_DATA_DEFINITION_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief One `PS_DATA_DEFINITION_*_V3` value. */
  ps_data_definition_kind_v3 kind;
  /** @brief Nonzero provider-owned structural version. */
  uint32_t structural_version;
  /** @brief Permanent process-independent definition identity. */
  ps_data_identity_v3 identity;
  /** @brief Bounded borrowed UTF-8 diagnostic name. */
  ps_data_bytes_v3 canonical_name;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_definition_v3;

/**
 * @brief One byte-preserving versioned descriptor or Layout extension.
 * @note The Host preserves payload bytes and never interprets them directly.
 */
typedef struct ps_data_extension_v3 {
  /** @brief Must equal `PS_DATA_EXTENSION_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief One `PS_DATA_DEFINITION_*_V3` value. */
  ps_data_definition_kind_v3 kind;
  /** @brief Nonzero structural version selected by the envelope. */
  uint32_t structural_version;
  /** @brief Permanent definition identity selected by the envelope. */
  ps_data_identity_v3 identity;
  /** @brief Exact borrowed provider payload bytes. */
  ps_data_bytes_v3 payload;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_extension_v3;

/**
 * @brief Borrowed checked storage range visible for one callback.
 * @note Pure query/Region/DataSpec calls clear `data` and the payload flag.
 */
typedef struct ps_data_buffer_view_v3 {
  /** @brief Must equal `PS_DATA_BUFFER_VIEW_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief Borrowed host pointer only when payload-available flag is set. */
  const uint8_t* data;
  /** @brief Positive byte length of this checked BufferHandle range. */
  uint64_t byte_size;
  /** @brief Nonzero process-local diagnostic allocation token. */
  uint64_t allocation_identity;
  /** @brief Dense zero-based index in the containing value view. */
  uint32_t buffer_index;
  /** @brief Checked combination of `PS_DATA_BUFFER_*_V3` flags. */
  ps_data_buffer_flags_v3 flags;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_buffer_view_v3;

/**
 * @brief One checked Layout-declared subrange of a buffer view.
 * @note The Host validates the reference and checked end before callback entry.
 */
typedef struct ps_data_buffer_envelope_v3 {
  /** @brief Must equal `PS_DATA_BUFFER_ENVELOPE_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief Dense zero-based referenced buffer index. */
  uint32_t buffer_index;
  /** @brief Provider-defined nonzero logical role. */
  uint32_t logical_role;
  /** @brief Byte offset relative to the referenced checked range. */
  uint64_t offset;
  /** @brief Positive byte length contained by that range. */
  uint64_t length;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_buffer_envelope_v3;

/**
 * @brief Borrowed complete provider-defined Value metadata for one callback.
 * @note Every pointer/count pair is validated by the Host before entry.
 *       The Host materializes this view only after its owning storage reaches
 *       a stable address; every pointer expires when that callback returns.
 */
typedef struct ps_data_value_view_v3 {
  /** @brief Must equal `PS_DATA_VALUE_VIEW_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief Non-null Schema extension. */
  const ps_data_extension_v3* schema;
  /** @brief Facet array, or null when `facet_count` is zero. */
  const ps_data_extension_v3* facets;
  /** @brief Bounded Facet array length. */
  uint64_t facet_count;
  /** @brief Non-null Layout extension. */
  const ps_data_extension_v3* layout;
  /** @brief Non-null bounded buffer-view array. */
  const ps_data_buffer_view_v3* buffers;
  /** @brief Positive bounded buffer-view count. */
  uint64_t buffer_count;
  /** @brief Non-null bounded Layout-envelope array. */
  const ps_data_buffer_envelope_v3* envelopes;
  /** @brief Positive bounded Layout-envelope count. */
  uint64_t envelope_count;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_value_view_v3;

/**
 * @brief Borrowed bounded provider diagnostic copied before callback return.
 * @note Diagnostic code zero and empty bytes are canonical success output.
 */
typedef struct ps_data_diagnostic_v3 {
  /** @brief Must equal `PS_DATA_DIAGNOSTIC_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief Provider-specific diagnostic code, zero for success. */
  uint32_t code;
  /** @brief Must be zero for v3. */
  uint32_t reserved0;
  /** @brief Borrowed diagnostic UTF-8 bytes. */
  ps_data_bytes_v3 message;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_diagnostic_v3;

/** @brief Fixed metadata-only property request. */
typedef struct ps_data_property_query_v3 {
  /** @brief Must equal `PS_DATA_PROPERTY_QUERY_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief Stable provider-defined property identity. */
  ps_data_identity_v3 property;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_property_query_v3;

/** @brief Fixed bounded property outcome. */
typedef struct ps_data_property_result_v3 {
  /** @brief Must equal `PS_DATA_PROPERTY_RESULT_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief One `PS_DATA_PROPERTY_*_V3` state. */
  ps_data_property_state_v3 state;
  /** @brief One `PS_DATA_PROPERTY_VALUE_*_V3` kind. */
  ps_data_property_value_kind_v3 value_kind;
  /** @brief Scalar result when value kind is UINT64, otherwise zero. */
  uint64_t uint64_value;
  /** @brief Borrowed bounded byte result when value kind is BYTES. */
  ps_data_bytes_v3 bytes_value;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_property_result_v3;

/**
 * @brief Fixed metadata-only Region request.
 * @note Tensor begin/end arrays are borrowed for `rank` entries.
 */
typedef struct ps_data_region_request_v3 {
  /** @brief Must equal `PS_DATA_REGION_REQUEST_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief One `PS_DATA_REGION_*_V3` request kind. */
  ps_data_region_kind_v3 kind;
  /** @brief Tensor rank, zero for Empty/Whole/Unsupported. */
  uint32_t rank;
  /** @brief Explicit logical coordinate-domain identity. */
  ps_data_identity_v3 domain;
  /** @brief Borrowed rank-sized inclusive beginnings, or null. */
  const uint64_t* begin;
  /** @brief Borrowed rank-sized exclusive endings, or null. */
  const uint64_t* end;
  /** @brief Explicit nonzero complexity budget. */
  uint64_t complexity_budget;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_region_request_v3;

/** @brief Fixed provider Region evaluation result. */
typedef struct ps_data_region_result_v3 {
  /** @brief Must equal `PS_DATA_REGION_RESULT_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief One `PS_DATA_REGION_*_V3` outcome state. */
  ps_data_region_state_v3 state;
  /** @brief Must be zero for v3. */
  uint32_t reserved0;
  /** @brief Exact selected logical-site count for Exact, otherwise zero. */
  uint64_t selected_site_count;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_region_result_v3;

/** @brief Fixed metadata-only bounded DataSpec request. */
typedef struct ps_data_spec_request_v3 {
  /** @brief Must equal `PS_DATA_SPEC_REQUEST_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief Required Schema identity. */
  ps_data_identity_v3 schema_identity;
  /** @brief Inclusive nonzero structural-version lower bound. */
  uint32_t minimum_version;
  /** @brief Inclusive structural-version upper bound. */
  uint32_t maximum_version;
  /** @brief Inclusive logical-site lower bound. */
  uint64_t minimum_logical_sites;
  /** @brief Inclusive logical-site upper bound. */
  uint64_t maximum_logical_sites;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_spec_request_v3;

/** @brief Fixed DataSpec set-relation outcome. */
typedef struct ps_data_spec_result_v3 {
  /** @brief Must equal `PS_DATA_SPEC_RESULT_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief One `PS_DATA_SPEC_*_V3` relation. */
  ps_data_spec_relation_v3 relation;
  /** @brief Exactly 0 or 1, with 1 only for partial overlap. */
  uint32_t requires_runtime_guard;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[3];
} ps_data_spec_result_v3;

/**
 * @brief Appends one provider-selected canonical-content byte segment.
 * @param context Host-owned sink context valid for the callback.
 * @param data Borrowed bytes; may be null only when size is zero.
 * @param size Exact byte count.
 * @return Stable status; non-OK aborts traversal.
 * @note The provider never owns or finalizes the Host digest state.
 */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_append_bytes_fn_v3)(
    void* context, const uint8_t* data, uint64_t size) PS_DATA_NOEXCEPT;

/** @brief Host-owned sink table borrowed for one content traversal. */
typedef struct ps_data_byte_sink_v3 {
  /** @brief Must equal `PS_DATA_BYTE_SINK_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief Opaque Host state passed to `append`. */
  void* context;
  /** @brief Non-null Host append callback. */
  ps_data_append_bytes_fn_v3 append;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[2];
} ps_data_byte_sink_v3;

/** @brief Validates complete descriptor, Layout, and buffer semantics. */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_validate_fn_v3)(
    void* provider_context, const ps_data_value_view_v3* value,
    ps_data_diagnostic_v3* diagnostic) PS_DATA_NOEXCEPT;
/** @brief Evaluates one pure metadata-only property query. */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_query_fn_v3)(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_property_query_v3* query, ps_data_property_result_v3* result,
    ps_data_diagnostic_v3* diagnostic) PS_DATA_NOEXCEPT;
/** @brief Evaluates one pure metadata-only bounded Region request. */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_evaluate_region_fn_v3)(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_region_request_v3* request, ps_data_region_result_v3* result,
    ps_data_diagnostic_v3* diagnostic) PS_DATA_NOEXCEPT;
/** @brief Evaluates one pure metadata-only DataSpec set relation. */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_evaluate_spec_fn_v3)(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_spec_request_v3* request, ps_data_spec_result_v3* result,
    ps_data_diagnostic_v3* diagnostic) PS_DATA_NOEXCEPT;
/** @brief Visits logical content in provider-defined canonical order. */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_visit_content_fn_v3)(
    void* provider_context, const ps_data_value_view_v3* value,
    const ps_data_byte_sink_v3* sink,
    ps_data_diagnostic_v3* diagnostic) PS_DATA_NOEXCEPT;
/** @brief Creates one optional provider-owned opaque lifetime object. */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_create_owner_fn_v3)(
    void* provider_context, void** owner,
    ps_data_diagnostic_v3* diagnostic) PS_DATA_NOEXCEPT;
/** @brief Destroys one successfully created opaque owner exactly once. */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_destroy_owner_fn_v3)(
    void* provider_context, void* owner,
    ps_data_diagnostic_v3* diagnostic) PS_DATA_NOEXCEPT;
/** @brief Performs one final generation destroy before module release. */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_destroy_provider_fn_v3)(
    void* provider_context, ps_data_diagnostic_v3* diagnostic) PS_DATA_NOEXCEPT;

/**
 * @brief Complete immutable v3 definition-suite callback table.
 * @note Every pointer is mandatory. Definition metadata remains valid until
 * the final provider destroy returns and the candidate module lease releases.
 */
typedef struct ps_data_provider_api_v3 {
  /** @brief Must equal `PS_DATA_PROVIDER_API_V3_SIZE`. */
  uint64_t struct_size;
  /** @brief Must equal `PS_DATA_PROVIDER_ABI_VERSION`. */
  uint32_t abi_version;
  /** @brief Bounded positive definition array length. */
  uint32_t definition_count;
  /** @brief Stable identity used to replace this provider bundle. */
  ps_data_identity_v3 provider_identity;
  /** @brief Bounded borrowed diagnostic implementation version. */
  ps_data_bytes_v3 implementation_version;
  /** @brief Non-null immutable definition array. */
  const ps_data_definition_v3* definitions;
  /** @brief Opaque provider context passed to every callback. */
  void* provider_context;
  /** @brief Mandatory complete semantic validation callback. */
  ps_data_validate_fn_v3 validate;
  /** @brief Mandatory pure property callback. */
  ps_data_query_fn_v3 query;
  /** @brief Mandatory pure Region callback. */
  ps_data_evaluate_region_fn_v3 evaluate_region;
  /** @brief Mandatory pure DataSpec callback. */
  ps_data_evaluate_spec_fn_v3 evaluate_spec;
  /** @brief Mandatory canonical logical-content visitor. */
  ps_data_visit_content_fn_v3 visit_content;
  /** @brief Mandatory opaque-owner create callback. */
  ps_data_create_owner_fn_v3 create_owner;
  /** @brief Mandatory opaque-owner destroy callback. */
  ps_data_destroy_owner_fn_v3 destroy_owner;
  /** @brief Mandatory final provider-generation destroy callback. */
  ps_data_destroy_provider_fn_v3 destroy_provider;
  /** @brief Must be all zero for v3. */
  uint64_t reserved[4];
} ps_data_provider_api_v3;

/**
 * @brief Candidate numeric handshake function type.
 * @return Exact supported ABI generation.
 * @note The Host calls no other candidate function before equality succeeds.
 */
// NOLINTNEXTLINE(readability/casting)
typedef uint32_t(PS_DATA_CALL* ps_data_provider_get_abi_version_fn_v3)(void)
    PS_DATA_NOEXCEPT;

/**
 * @brief Candidate API-table function type.
 * @param api Host-owned pre-zeroed table with exact `struct_size` set.
 * @return Stable status; only OK permits further validation.
 * @note The candidate fills the complete table without retaining `api`.
 */
typedef ps_data_status_v3(PS_DATA_CALL* ps_data_provider_get_api_fn_v3)(
    ps_data_provider_api_v3* api) PS_DATA_NOEXCEPT;

/**
 * @brief Required provider export returning the numeric ABI generation.
 * @return `PS_DATA_PROVIDER_ABI_VERSION` for this exact header.
 * @note Provider DSOs define this function; the operation runtime only looks
 * it up through the platform loader boundary.
 */
PS_DATA_PROVIDER_EXPORT uint32_t PS_DATA_CALL
ps_data_provider_get_abi_version(void) PS_DATA_NOEXCEPT;

/**
 * @brief Required provider export filling the v3 definition API table.
 * @param api Host-owned exact-size pre-zeroed output table.
 * @return Stable status with no exception or foreign unwinding.
 * @note Provider-owned pointed-to metadata remains immutable until final
 * generation destroy.
 */
PS_DATA_PROVIDER_EXPORT ps_data_status_v3 PS_DATA_CALL
ps_data_provider_get_api_v3(ps_data_provider_api_v3* api) PS_DATA_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#if defined(__cplusplus)
#define PS_DATA_STATIC_ASSERT(condition, message) \
  static_assert(condition, message)
#define PS_DATA_ALIGNOF(type) alignof(type)
#else
#define PS_DATA_STATIC_ASSERT(condition, message) \
  _Static_assert(condition, message)
#define PS_DATA_ALIGNOF(type) _Alignof(type)
#endif

PS_DATA_STATIC_ASSERT(sizeof(void*) == 8U, "v3 requires 8-byte pointers");
PS_DATA_STATIC_ASSERT(sizeof(ps_data_append_bytes_fn_v3) == 8U,
                      "v3 requires 8-byte function pointers");
#define PS_DATA_ASSERT_LAYOUT(type, size_value)                       \
  PS_DATA_STATIC_ASSERT(sizeof(type) == (size_value), #type " size"); \
  PS_DATA_STATIC_ASSERT(PS_DATA_ALIGNOF(type) == 8U, #type " alignment")
PS_DATA_ASSERT_LAYOUT(ps_data_identity_v3, PS_DATA_IDENTITY_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_bytes_v3, PS_DATA_BYTES_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_definition_v3, PS_DATA_DEFINITION_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_extension_v3, PS_DATA_EXTENSION_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_buffer_view_v3, PS_DATA_BUFFER_VIEW_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_buffer_envelope_v3,
                      PS_DATA_BUFFER_ENVELOPE_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_value_view_v3, PS_DATA_VALUE_VIEW_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_diagnostic_v3, PS_DATA_DIAGNOSTIC_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_property_query_v3,
                      PS_DATA_PROPERTY_QUERY_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_property_result_v3,
                      PS_DATA_PROPERTY_RESULT_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_region_request_v3,
                      PS_DATA_REGION_REQUEST_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_region_result_v3, PS_DATA_REGION_RESULT_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_spec_request_v3, PS_DATA_SPEC_REQUEST_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_spec_result_v3, PS_DATA_SPEC_RESULT_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_byte_sink_v3, PS_DATA_BYTE_SINK_V3_SIZE);
PS_DATA_ASSERT_LAYOUT(ps_data_provider_api_v3, PS_DATA_PROVIDER_API_V3_SIZE);

PS_DATA_STATIC_ASSERT(offsetof(ps_data_definition_v3, identity) == 16U,
                      "definition identity offset");
PS_DATA_STATIC_ASSERT(offsetof(ps_data_definition_v3, canonical_name) == 32U,
                      "definition name offset");
PS_DATA_STATIC_ASSERT(offsetof(ps_data_extension_v3, payload) == 32U,
                      "extension payload offset");
PS_DATA_STATIC_ASSERT(offsetof(ps_data_buffer_view_v3, data) == 8U,
                      "buffer data offset");
PS_DATA_STATIC_ASSERT(offsetof(ps_data_value_view_v3, envelopes) == 56U,
                      "value envelopes offset");
PS_DATA_STATIC_ASSERT(offsetof(ps_data_provider_api_v3, definitions) == 48U,
                      "provider definitions offset");
PS_DATA_STATIC_ASSERT(offsetof(ps_data_provider_api_v3, validate) == 64U,
                      "provider validate offset");
PS_DATA_STATIC_ASSERT(offsetof(ps_data_provider_api_v3, destroy_provider) ==
                          120U,
                      "provider destroy offset");
PS_DATA_STATIC_ASSERT(offsetof(ps_data_provider_api_v3, reserved) == 128U,
                      "provider reserved offset");

#undef PS_DATA_ASSERT_LAYOUT
#undef PS_DATA_ALIGNOF
#undef PS_DATA_STATIC_ASSERT

#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_DATA_PROVIDER_API_H_
