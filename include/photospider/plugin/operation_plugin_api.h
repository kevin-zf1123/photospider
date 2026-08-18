#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_PLUGIN_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_PLUGIN_API_H_

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file operation_plugin_api.h
 * @brief Frozen separately versioned pure-C operation-plugin ABI v1.
 *
 * The ABI exchanges only exact fixed-width records, bounded borrowed views,
 * opaque round-trip contexts, Host-owned sinks, and synchronous callbacks.
 * It is an in-process compatibility boundary, not a sandbox. No C++ object,
 * allocator owner, exception, RTTI value, Graph, Run, scheduler, cache,
 * executor, native device handle, or asynchronous completion crosses it.
 *
 * @note The supported profile has 8-bit bytes, 8-byte data/function pointers,
 *       natural 8-byte pointer alignment, Host endianness, and the platform C
 *       calling convention. Every v1 record has one exact accepted size.
 */

#ifdef __cplusplus
extern "C" {
#define PS_OPERATION_NOEXCEPT noexcept
#define PS_OPERATION_STATIC_ASSERT(condition, message) \
  static_assert((condition), message)
#define PS_OPERATION_ALIGNOF(type) alignof(type)
#else
#define PS_OPERATION_NOEXCEPT
#define PS_OPERATION_STATIC_ASSERT(condition, message) \
  _Static_assert((condition), message)
#define PS_OPERATION_ALIGNOF(type) _Alignof(type)
#endif

#if defined(_WIN32)
#define PS_OPERATION_CALL __cdecl
#if defined(PS_OPERATION_PLUGIN_BUILD)
#define PS_OPERATION_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PS_OPERATION_PLUGIN_EXPORT
#endif
#else
#define PS_OPERATION_CALL
#define PS_OPERATION_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/** @brief Exact separately versioned operation-plugin ABI generation. */
#define PS_OPERATION_PLUGIN_ABI_VERSION 1U

/** @brief Required numeric discovery symbol exported by every ABI-v1 DSO. */
#define PS_OPERATION_PLUGIN_GET_ABI_VERSION_SYMBOL \
  "ps_operation_plugin_get_abi_version"
/** @brief Required exact root discovery symbol exported by every ABI-v1 DSO. */
#define PS_OPERATION_PLUGIN_GET_API_V1_SYMBOL "ps_operation_plugin_get_api_v1"

/** @brief Exact root and suite sizes accepted by ABI v1. */
#define PS_OPERATION_PLUGIN_API_V1_SIZE 96U
#define PS_OPERATION_SUITE_V1_SIZE 64U

/** @brief Exact helper sizes accepted by ABI v1. */
#define PS_OPERATION_RECORD_HEADER_V1_SIZE 16U
#define PS_OPERATION_SUITE_HEADER_V1_SIZE 16U
#define PS_OPERATION_IDENTITY_V1_SIZE 16U
#define PS_OPERATION_GENERATION_HANDLE_V1_SIZE 16U
#define PS_OPERATION_INVOCATION_HANDLE_V1_SIZE 16U
#define PS_OPERATION_BYTES_V1_SIZE 16U
#define PS_OPERATION_MUTABLE_BYTES_V1_SIZE 16U
#define PS_OPERATION_ARRAY_REF_V1_SIZE 16U
#define PS_OPERATION_CONFIGURATION_VALUE_V1_SIZE 16U
#define PS_OPERATION_AXIS_RANGE_V1_SIZE 16U
#define PS_OPERATION_IMAGE_BOUNDS_V1_SIZE 32U
#define PS_OPERATION_SAMPLE_DOMAIN_V1_SIZE 24U
#define PS_OPERATION_SHA256_DIGEST_V1_SIZE 32U

/** @brief Exact semantic-record sizes accepted by ABI v1. */
#define PS_OPERATION_DIAGNOSTIC_V1_SIZE 48U
#define PS_OPERATION_OUTPUT_SINK_V1_SIZE 48U
#define PS_OPERATION_CONFIGURATION_NODE_V1_SIZE 64U
#define PS_OPERATION_CONFIGURATION_VIEW_V1_SIZE 48U
#define PS_OPERATION_DESCRIPTOR_V1_SIZE 128U
#define PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE 192U
#define PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE 112U
#define PS_OPERATION_VALUE_DESCRIPTOR_V1_SIZE 224U
#define PS_OPERATION_FACET_VIEW_V1_SIZE 64U
#define PS_OPERATION_BUFFER_VIEW_V1_SIZE 80U
#define PS_OPERATION_VALUE_VIEW_V1_SIZE 128U
#define PS_OPERATION_INPUT_BINDING_V1_SIZE 96U
#define PS_OPERATION_OUTPUT_PLAN_V1_SIZE 112U
#define PS_OPERATION_MUTABLE_OUTPUT_BINDING_V1_SIZE 128U
#define PS_OPERATION_INVOCATION_V1_SIZE 96U
#define PS_OPERATION_REGION_ATOM_V1_SIZE 96U
#define PS_OPERATION_REGION_SET_VIEW_V1_SIZE 48U
#define PS_OPERATION_REGION_BINDING_V1_SIZE 80U
#define PS_OPERATION_DEPENDENCY_RECORD_V1_SIZE 96U
#define PS_OPERATION_TILE_V1_SIZE 64U
#define PS_OPERATION_DENSE_TENSOR_DESCRIPTOR_V1_SIZE 96U
#define PS_OPERATION_STRIDED_LAYOUT_V1_SIZE 64U
#define PS_OPERATION_IMAGE_FACET_V1_SIZE 160U
#define PS_OPERATION_CHANNEL_V1_SIZE 48U
#define PS_OPERATION_CHANNEL_GROUP_V1_SIZE 64U
#define PS_OPERATION_CHANNEL_SAMPLE_DOMAIN_V1_SIZE 64U
#define PS_OPERATION_SAMPLE_DOMAIN_FACET_V1_SIZE 80U
#define PS_OPERATION_COLOR_FACET_V1_SIZE 64U
#define PS_OPERATION_OUTPUT_BUFFER_PLAN_V1_SIZE 64U
#define PS_OPERATION_OUTPUT_GRANT_SPAN_V1_SIZE 64U

/** @brief Frozen structural bounds for every ABI-v1 callback. */
#define PS_OPERATION_MAX_NAME_BYTES_V1 128U
#define PS_OPERATION_MAX_IMPLEMENTATION_VERSION_BYTES_V1 4096U
#define PS_OPERATION_MAX_DIAGNOSTIC_BYTES_V1 4096U
#define PS_OPERATION_MAX_OPERATIONS_V1 4096U
#define PS_OPERATION_MAX_IMPLEMENTATIONS_V1 256U
#define PS_OPERATION_MAX_PORTS_V1 256U
#define PS_OPERATION_MAX_RANK_V1 16U
#define PS_OPERATION_MAX_FACETS_V1 64U
#define PS_OPERATION_MAX_BUFFERS_V1 64U
#define PS_OPERATION_MAX_CONFIGURATION_NODES_V1 4096U
#define PS_OPERATION_MAX_CONFIGURATION_DEPTH_V1 64U
#define PS_OPERATION_MAX_CONFIGURATION_BYTES_V1 1048576U
#define PS_OPERATION_MAX_REGION_ATOMS_V1 64U
#define PS_OPERATION_MAX_DEPENDENCY_RECORDS_V1 4096U
#define PS_OPERATION_MAX_CHANNELS_V1 4096U
#define PS_OPERATION_MAX_CHANNEL_GROUPS_V1 4096U
#define PS_OPERATION_MAX_CHANNEL_GROUP_MEMBERS_V1 4096U
#define PS_OPERATION_MAX_CHANNEL_GROUP_MEMBERSHIPS_V1 65536U
#define PS_OPERATION_MAX_OUTPUT_GRANT_SPANS_V1 1048576U

/** @brief Stable callback status scalar. */
typedef uint32_t ps_operation_status_v1;
/** @brief Callback completed with a complete valid result. */
#define PS_OPERATION_STATUS_OK_V1 0U
/** @brief Caller input or configuration is invalid. */
#define PS_OPERATION_STATUS_INVALID_ARGUMENT_V1 1U
/** @brief Host or plugin bounded staging exhausted memory. */
#define PS_OPERATION_STATUS_OUT_OF_MEMORY_V1 2U
/** @brief Requested suite, operation, representation, or route is unsupported.
 */
#define PS_OPERATION_STATUS_UNSUPPORTED_V1 3U
/** @brief One exact ABI record or relationship is malformed. */
#define PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1 4U
/** @brief A bounded result exceeds an explicit v1 limit. */
#define PS_OPERATION_STATUS_TOO_COMPLEX_V1 5U
/** @brief Host cancellation was observed through a sink. */
#define PS_OPERATION_STATUS_CANCELLED_V1 6U
/** @brief Current immutable invocation state cannot satisfy the request. */
#define PS_OPERATION_STATUS_FAILED_PRECONDITION_V1 7U
/** @brief Callback encountered a non-resource internal failure. */
#define PS_OPERATION_STATUS_INTERNAL_ERROR_V1 8U

/** @brief Stable suite identity scalar. */
typedef uint32_t ps_operation_suite_id_v1;
#define PS_OPERATION_SUITE_DEFINITION_V1 1U
#define PS_OPERATION_SUITE_CONFIGURATION_V1 2U
#define PS_OPERATION_SUITE_INFERENCE_V1 3U
#define PS_OPERATION_SUITE_REGION_V1 4U
#define PS_OPERATION_SUITE_DEPENDENCY_V1 5U
#define PS_OPERATION_SUITE_EXECUTION_V1 6U

/** @brief Stable semantic-record identity scalar. */
typedef uint32_t ps_operation_record_kind_v1;
#define PS_OPERATION_RECORD_DIAGNOSTIC_V1 1U
#define PS_OPERATION_RECORD_OUTPUT_SINK_V1 2U
#define PS_OPERATION_RECORD_CONFIGURATION_NODE_V1 3U
#define PS_OPERATION_RECORD_CONFIGURATION_VIEW_V1 4U
#define PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1 5U
#define PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1 6U
#define PS_OPERATION_RECORD_PORT_DESCRIPTOR_V1 7U
#define PS_OPERATION_RECORD_VALUE_DESCRIPTOR_V1 8U
#define PS_OPERATION_RECORD_FACET_VIEW_V1 9U
#define PS_OPERATION_RECORD_BUFFER_VIEW_V1 10U
#define PS_OPERATION_RECORD_VALUE_VIEW_V1 11U
#define PS_OPERATION_RECORD_INPUT_BINDING_V1 12U
#define PS_OPERATION_RECORD_OUTPUT_PLAN_V1 13U
#define PS_OPERATION_RECORD_MUTABLE_OUTPUT_BINDING_V1 14U
#define PS_OPERATION_RECORD_INVOCATION_V1 15U
#define PS_OPERATION_RECORD_REGION_ATOM_V1 16U
#define PS_OPERATION_RECORD_REGION_SET_VIEW_V1 17U
#define PS_OPERATION_RECORD_REGION_BINDING_V1 18U
#define PS_OPERATION_RECORD_DEPENDENCY_RECORD_V1 19U
#define PS_OPERATION_RECORD_TILE_V1 20U
#define PS_OPERATION_RECORD_DENSE_TENSOR_DESCRIPTOR_V1 21U
#define PS_OPERATION_RECORD_STRIDED_LAYOUT_V1 22U
#define PS_OPERATION_RECORD_IMAGE_FACET_V1 23U
#define PS_OPERATION_RECORD_CHANNEL_V1 24U
#define PS_OPERATION_RECORD_CHANNEL_GROUP_V1 25U
#define PS_OPERATION_RECORD_CHANNEL_SAMPLE_DOMAIN_V1 26U
#define PS_OPERATION_RECORD_SAMPLE_DOMAIN_FACET_V1 27U
#define PS_OPERATION_RECORD_COLOR_FACET_V1 28U
#define PS_OPERATION_RECORD_OUTPUT_BUFFER_PLAN_V1 29U
#define PS_OPERATION_RECORD_OUTPUT_GRANT_SPAN_V1 30U

/** @brief Stable configuration-node kind scalar. */
typedef uint32_t ps_operation_configuration_kind_v1;
#define PS_OPERATION_CONFIGURATION_NULL_V1 1U
#define PS_OPERATION_CONFIGURATION_BOOLEAN_V1 2U
#define PS_OPERATION_CONFIGURATION_SIGNED_I64_V1 3U
#define PS_OPERATION_CONFIGURATION_BINARY64_V1 4U
#define PS_OPERATION_CONFIGURATION_UTF8_V1 5U
#define PS_OPERATION_CONFIGURATION_BYTES_V1 6U
#define PS_OPERATION_CONFIGURATION_ARRAY_V1 7U
#define PS_OPERATION_CONFIGURATION_OBJECT_V1 8U

/** @brief Stable port-direction scalar. */
typedef uint32_t ps_operation_port_direction_v1;
#define PS_OPERATION_PORT_INPUT_V1 1U
#define PS_OPERATION_PORT_OUTPUT_V1 2U

/** @brief Stable intent-mask scalar and bits. */
typedef uint32_t ps_operation_intent_mask_v1;
#define PS_OPERATION_INTENT_HP_V1 1U
#define PS_OPERATION_INTENT_RT_V1 2U

/** @brief Stable execution-shape mask scalar and bits. */
typedef uint32_t ps_operation_execution_shape_mask_v1;
#define PS_OPERATION_EXECUTION_MONOLITHIC_V1 1U
#define PS_OPERATION_EXECUTION_TILED_V1 2U

/** @brief Stable device-kind scalar. V1 supports CPU only. */
typedef uint32_t ps_operation_device_kind_v1;
#define PS_OPERATION_DEVICE_CPU_V1 1U

/** @brief Stable buffer-access mask scalar and bits. */
typedef uint32_t ps_operation_access_mask_v1;
#define PS_OPERATION_ACCESS_READ_V1 1U
#define PS_OPERATION_ACCESS_WRITE_V1 2U

/** @brief Stable implementation-behavior mask scalar and bits. */
typedef uint32_t ps_operation_behavior_mask_v1;
#define PS_OPERATION_BEHAVIOR_SIDE_EFFECT_V1 1U
#define PS_OPERATION_BEHAVIOR_DATA_DEPENDENT_V1 2U

/** @brief Stable CPU execution-mode scalar. */
typedef uint32_t ps_operation_execution_mode_v1;
#define PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1 1U
#define PS_OPERATION_EXECUTION_SUPERVISED_PROCESS_V1 2U

/** @brief Stable Region result scalar. */
typedef uint32_t ps_operation_region_outcome_v1;
#define PS_OPERATION_REGION_EXACT_V1 1U
#define PS_OPERATION_REGION_UNKNOWN_V1 2U
#define PS_OPERATION_REGION_UNSUPPORTED_V1 3U
#define PS_OPERATION_REGION_TOO_COMPLEX_V1 4U

/** @brief Stable Region-set representation scalar. */
typedef uint32_t ps_operation_region_set_kind_v1;
#define PS_OPERATION_REGION_SET_EMPTY_V1 1U
#define PS_OPERATION_REGION_SET_WHOLE_V1 2U
#define PS_OPERATION_REGION_SET_CLAUSE_V1 3U

/** @brief Stable Region atom-kind scalar. */
typedef uint32_t ps_operation_region_atom_kind_v1;
#define PS_OPERATION_REGION_ATOM_IMAGE_RECT_V1 1U
#define PS_OPERATION_REGION_ATOM_TENSOR_SLICE_V1 2U

/** @brief Stable Host output-sink channel scalar. */
typedef uint32_t ps_operation_output_channel_v1;
#define PS_OPERATION_OUTPUT_DIAGNOSTIC_V1 1U
#define PS_OPERATION_OUTPUT_PLAN_V1 2U
#define PS_OPERATION_OUTPUT_REGION_BINDING_V1 3U
#define PS_OPERATION_OUTPUT_DEPENDENCY_RECORD_V1 4U

/** @brief Stable ValueView flag scalar and bits. */
typedef uint32_t ps_operation_value_flags_v1;
#define PS_OPERATION_VALUE_PAYLOAD_AVAILABLE_V1 1U

/** @brief Stable InputBinding flag scalar and bits. */
typedef uint32_t ps_operation_input_binding_flags_v1;
#define PS_OPERATION_INPUT_CONNECTED_V1 1U
#define PS_OPERATION_INPUT_REGION_AVAILABLE_V1 2U

/** @brief Stable ImageFacet presence-mask scalar and bits. */
typedef uint32_t ps_operation_image_facet_presence_v1;
#define PS_OPERATION_IMAGE_HAS_CHANNEL_AXIS_V1 1U
#define PS_OPERATION_IMAGE_HAS_DISPLAY_WINDOW_V1 2U
#define PS_OPERATION_IMAGE_HAS_CHANNEL_SCHEMA_V1 4U
#define PS_OPERATION_IMAGE_HAS_SAMPLE_DOMAIN_V1 8U
#define PS_OPERATION_IMAGE_HAS_COLOR_V1 16U

/** @brief Stable element-semantics scalar. */
typedef uint32_t ps_operation_element_semantics_v1;
#define PS_OPERATION_ELEMENT_UNSIGNED_INTEGER_V1 1U
#define PS_OPERATION_ELEMENT_SIGNED_INTEGER_V1 2U
#define PS_OPERATION_ELEMENT_FLOATING_POINT_V1 3U

/** @brief Stable storage-encoding scalar. */
typedef uint32_t ps_operation_storage_encoding_v1;
#define PS_OPERATION_STORAGE_NATIVE_SCALAR_V1 1U
#define PS_OPERATION_STORAGE_FP4_E2M1_V1 2U

/** @brief Stable sample-encoding scalar. */
typedef uint32_t ps_operation_sample_encoding_v1;
#define PS_OPERATION_SAMPLE_ENCODING_VALUE_V1 1U
#define PS_OPERATION_SAMPLE_ENCODING_NORMALIZED_V1 2U
#define PS_OPERATION_SAMPLE_ENCODING_CODE_VALUE_V1 3U

/** @brief Stable sample-domain kind scalar. */
typedef uint32_t ps_operation_sample_domain_kind_v1;
#define PS_OPERATION_SAMPLE_DOMAIN_NORMALIZED_V1 1U
#define PS_OPERATION_SAMPLE_DOMAIN_LEGAL_V1 2U
#define PS_OPERATION_SAMPLE_DOMAIN_CODE_VALUE_V1 3U

/** @brief Stable color-transfer scalar. */
typedef uint32_t ps_operation_color_transfer_v1;
#define PS_OPERATION_COLOR_TRANSFER_SCENE_LINEAR_V1 1U
#define PS_OPERATION_COLOR_TRANSFER_SRGB_V1 2U
#define PS_OPERATION_COLOR_TRANSFER_REC709_V1 3U
#define PS_OPERATION_COLOR_TRANSFER_PQ_V1 4U
#define PS_OPERATION_COLOR_TRANSFER_HLG_V1 5U

/** @brief Stable color-primaries scalar. */
typedef uint32_t ps_operation_color_primaries_v1;
#define PS_OPERATION_COLOR_PRIMARIES_REC709_V1 1U
#define PS_OPERATION_COLOR_PRIMARIES_DISPLAY_P3_D65_V1 2U
#define PS_OPERATION_COLOR_PRIMARIES_REC2020_V1 3U
#define PS_OPERATION_COLOR_PRIMARIES_ACES_AP0_V1 4U
#define PS_OPERATION_COLOR_PRIMARIES_ACES_AP1_V1 5U

/**
 * @brief Exact semantic-record prefix.
 * @note `flags` is zero unless the containing record documents closed bits.
 */
typedef struct ps_operation_record_header_v1 {
  /** @brief Exact complete record size, never a minimum prefix size. */
  uint32_t struct_size;
  /** @brief One exact `PS_OPERATION_RECORD_*_V1` value. */
  ps_operation_record_kind_v1 struct_kind;
  /** @brief Exact record structural version; v1 requires one. */
  uint32_t struct_version;
  /** @brief Closed per-record flags, otherwise zero. */
  uint32_t flags;
} ps_operation_record_header_v1;

/** @brief Exact Host-prepared suite-table prefix. */
typedef struct ps_operation_suite_header_v1 {
  /** @brief Must equal `PS_OPERATION_SUITE_V1_SIZE`. */
  uint32_t struct_size;
  /** @brief Exact requested suite identity. */
  ps_operation_suite_id_v1 suite_id;
  /** @brief Exact requested suite version; v1 requires one. */
  uint32_t suite_version;
  /** @brief Must be zero for every v1 suite. */
  uint32_t flags;
} ps_operation_suite_header_v1;

/** @brief Permanent or Host-scoped opaque 128-bit identity. */
typedef struct ps_operation_identity_v1 {
  /** @brief Most-significant opaque identity word. */
  uint64_t word0;
  /** @brief Least-significant opaque identity word. */
  uint64_t word1;
} ps_operation_identity_v1;

/** @brief Unpredictable Host-minted operation-generation handle. */
typedef struct ps_operation_generation_handle_v1 {
  /** @brief First opaque generation word. */
  uint64_t word0;
  /** @brief Second opaque generation word. */
  uint64_t word1;
} ps_operation_generation_handle_v1;

/** @brief Unpredictable Host-minted invocation handle. */
typedef struct ps_operation_invocation_handle_v1 {
  /** @brief First opaque invocation word. */
  uint64_t word0;
  /** @brief Second opaque invocation word. */
  uint64_t word1;
} ps_operation_invocation_handle_v1;

/** @brief Bounded callback-local immutable byte view. */
typedef struct ps_operation_bytes_v1 {
  /** @brief Borrowed first byte, null exactly when size is zero. */
  const uint8_t* data;
  /** @brief Exact bounded byte count. */
  uint64_t size;
} ps_operation_bytes_v1;

/** @brief Callback-local Host-owned mutable byte view. */
typedef struct ps_operation_mutable_bytes_v1 {
  /** @brief Borrowed first writable byte, null exactly when size is zero. */
  uint8_t* data;
  /** @brief Exact checked writable byte count. */
  uint64_t size;
} ps_operation_mutable_bytes_v1;

/** @brief Borrowed exact-stride array reference. */
typedef struct ps_operation_array_ref_v1 {
  /** @brief First element, null exactly when count is zero. */
  const void* data;
  /** @brief Bounded element count. */
  uint32_t count;
  /** @brief Exact documented element stride, zero only when count is zero. */
  uint32_t stride;
} ps_operation_array_ref_v1;

/** @brief Exact storage for one scalar/configuration byte-view value. */
typedef union ps_operation_configuration_value_v1 {
  /** @brief Fixed scalar storage interpreted by node kind. */
  uint64_t words[2];
  /** @brief Borrowed UTF-8 or arbitrary-byte value interpreted by node kind. */
  ps_operation_bytes_v1 bytes;
} ps_operation_configuration_value_v1;

/** @brief Signed origin plus nonnegative extent for one logical axis. */
typedef struct ps_operation_axis_range_v1 {
  /** @brief Signed logical half-open origin. */
  int64_t origin;
  /** @brief Logical half-open extent. */
  uint64_t extent;
} ps_operation_axis_range_v1;

/** @brief Signed immutable half-open ordinary-image bounds. */
typedef struct ps_operation_image_bounds_v1 {
  /** @brief Inclusive x endpoint. */
  int64_t x_begin;
  /** @brief Inclusive y endpoint. */
  int64_t y_begin;
  /** @brief Exclusive x endpoint. */
  int64_t x_end;
  /** @brief Exclusive y endpoint. */
  int64_t y_end;
} ps_operation_image_bounds_v1;

/** @brief One finite inclusive declared sample interval. */
typedef struct ps_operation_sample_domain_v1 {
  /** @brief One `PS_OPERATION_SAMPLE_DOMAIN_*_V1` value. */
  ps_operation_sample_domain_kind_v1 kind;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief IEEE-754 binary64 bits of the finite inclusive minimum. */
  uint64_t minimum_binary64_bits;
  /** @brief IEEE-754 binary64 bits of the finite inclusive maximum. */
  uint64_t maximum_binary64_bits;
} ps_operation_sample_domain_v1;

/** @brief Exact SHA-256 digest words in canonical byte order. */
typedef struct ps_operation_sha256_digest_v1 {
  /** @brief Four fixed digest words; all zero denotes an absent digest. */
  uint64_t words[4];
} ps_operation_sha256_digest_v1;

/* Forward declarations keep callback signatures fully prototyped in C11. */
typedef struct ps_operation_diagnostic_v1 ps_operation_diagnostic_v1;
typedef struct ps_operation_output_sink_v1 ps_operation_output_sink_v1;
typedef struct ps_operation_configuration_node_v1
    ps_operation_configuration_node_v1;
typedef struct ps_operation_configuration_view_v1
    ps_operation_configuration_view_v1;
typedef struct ps_operation_descriptor_v1 ps_operation_descriptor_v1;
typedef struct ps_operation_implementation_descriptor_v1
    ps_operation_implementation_descriptor_v1;
typedef struct ps_operation_port_descriptor_v1 ps_operation_port_descriptor_v1;
typedef struct ps_operation_value_descriptor_v1
    ps_operation_value_descriptor_v1;
typedef struct ps_operation_facet_view_v1 ps_operation_facet_view_v1;
typedef struct ps_operation_buffer_view_v1 ps_operation_buffer_view_v1;
typedef struct ps_operation_value_view_v1 ps_operation_value_view_v1;
typedef struct ps_operation_input_binding_v1 ps_operation_input_binding_v1;
typedef struct ps_operation_output_plan_v1 ps_operation_output_plan_v1;
typedef struct ps_operation_mutable_output_binding_v1
    ps_operation_mutable_output_binding_v1;
typedef struct ps_operation_invocation_v1 ps_operation_invocation_v1;
typedef struct ps_operation_region_atom_v1 ps_operation_region_atom_v1;
typedef struct ps_operation_region_set_view_v1 ps_operation_region_set_view_v1;
typedef struct ps_operation_region_binding_v1 ps_operation_region_binding_v1;
typedef struct ps_operation_dependency_record_v1
    ps_operation_dependency_record_v1;
typedef struct ps_operation_tile_v1 ps_operation_tile_v1;
typedef struct ps_operation_dense_tensor_descriptor_v1
    ps_operation_dense_tensor_descriptor_v1;
typedef struct ps_operation_strided_layout_v1 ps_operation_strided_layout_v1;
typedef struct ps_operation_image_facet_v1 ps_operation_image_facet_v1;
typedef struct ps_operation_channel_v1 ps_operation_channel_v1;
typedef struct ps_operation_channel_group_v1 ps_operation_channel_group_v1;
typedef struct ps_operation_channel_sample_domain_v1
    ps_operation_channel_sample_domain_v1;
typedef struct ps_operation_sample_domain_facet_v1
    ps_operation_sample_domain_facet_v1;
typedef struct ps_operation_color_facet_v1 ps_operation_color_facet_v1;
typedef struct ps_operation_output_buffer_plan_v1
    ps_operation_output_buffer_plan_v1;
typedef struct ps_operation_output_grant_span_v1
    ps_operation_output_grant_span_v1;
typedef struct ps_operation_plugin_api_v1 ps_operation_plugin_api_v1;

/**
 * @brief Returns the exact operation ABI revision exported by one DSO.
 * @return `PS_OPERATION_PLUGIN_ABI_VERSION` for an ABI-v1 producer.
 * @note This is the only callback the Host may invoke before the root-table
 *       handshake. It has no object-lifetime dependency and must not unwind.
 */
/* NOLINTNEXTLINE(readability/casting) */
typedef uint32_t(PS_OPERATION_CALL* ps_operation_plugin_get_abi_version_fn_v1)(
    void) PS_OPERATION_NOEXCEPT;
/**
 * @brief Fills one Host-prepared exact root table.
 * @param api_out Nonnull Host-owned table with the exact v1 prefix prefilled.
 * @return `OK` after a complete fill, otherwise a stable failure status.
 * @note On failure the producer must leave the table unusable, retain no Host
 *       pointer, and allow no exception to cross the DSO boundary.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_plugin_get_api_fn_v1)(
    ps_operation_plugin_api_v1* api_out) PS_OPERATION_NOEXCEPT;
/**
 * @brief Copies one exact-stride batch into a Host-owned output channel.
 * @param host_context Opaque callback state supplied by the Host.
 * @param channel Closed destination channel for this record batch.
 * @param records First immutable record, or null only when `count` is zero.
 * @param count Bounded number of records in this emission.
 * @param stride Exact frozen byte stride for the selected channel record.
 * @return `OK` when the Host accepted the whole batch, otherwise the sticky
 *         sink failure that the producer must propagate unchanged.
 * @note The call is synchronous: the Host copies or rejects the batch before
 *       return, and the Host never retains plugin storage.
 */
typedef ps_operation_status_v1(PS_OPERATION_CALL* ps_operation_emit_fn_v1)(
    void* host_context, ps_operation_output_channel_v1 channel,
    const void* records, uint32_t count, uint32_t stride) PS_OPERATION_NOEXCEPT;
/**
 * @brief Fills one Host-prepared exact suite table.
 * @param plugin_context Opaque generation context from the root table.
 * @param suite_id Closed identity of the requested v1 suite.
 * @param requested_version Exact requested suite version; must be one.
 * @param suite_out Nonnull Host-owned exact 64-byte suite table.
 * @return `OK` after a complete exact fill, otherwise a stable failure status.
 * @note Short tables, long tables, missing tails, and compatible-version
 *       fallbacks are invalid; no pointer may be retained or unwound through.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_query_suite_fn_v1)(
    void* plugin_context, ps_operation_suite_id_v1 suite_id,
    uint32_t requested_version,
    ps_operation_suite_header_v1* suite_out) PS_OPERATION_NOEXCEPT;
/**
 * @brief Destroys one successfully published plugin generation exactly once.
 * @param plugin_context Opaque generation context from the root table.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` after destruction or a stable diagnostic failure status.
 * @note The Host calls this only after publication is withdrawn and every
 *       callback/output lease for the generation has drained. It must not
 *       retain `sink`, throw, or destroy the generation more than once.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_destroy_plugin_fn_v1)(
    void* plugin_context,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Returns the bounded number of operation definitions in a generation.
 * @param plugin_context Opaque generation context from the root table.
 * @param operation_count_out Nonnull destination for the complete count.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` with a count no greater than the frozen maximum, otherwise a
 *         stable failure status.
 * @note The returned dense index space is immutable for the generation.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_get_operation_count_fn_v1)(
    void* plugin_context, uint32_t* operation_count_out,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Fills one Host-prepared operation descriptor by dense index.
 * @param plugin_context Opaque generation context from the root table.
 * @param operation_index Index below the previously returned operation count.
 * @param operation_out Nonnull Host-owned exact descriptor destination.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` with a complete immutable descriptor, otherwise a stable
 *         failure status.
 * @note Every nested byte/array view remains plugin-owned and immutable until
 *       generation destruction; the Host validates and copies it immediately.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_get_operation_fn_v1)(
    void* plugin_context, uint32_t operation_index,
    ps_operation_descriptor_v1* operation_out,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Returns the bounded implementation count for one operation.
 * @param plugin_context Opaque generation context from the root table.
 * @param operation_identity Nonnull permanent operation identity.
 * @param implementation_count_out Nonnull destination for the complete count.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` with a bounded count, otherwise a stable failure status.
 * @note The returned dense index space is immutable for the generation.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_get_implementation_count_fn_v1)(
    void* plugin_context, const ps_operation_identity_v1* operation_identity,
    uint32_t* implementation_count_out,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Fills one Host-prepared implementation descriptor by dense index.
 * @param plugin_context Opaque generation context from the root table.
 * @param operation_identity Nonnull permanent operation identity.
 * @param implementation_index Index below the returned implementation count.
 * @param implementation_out Nonnull Host-owned exact descriptor destination.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` with a complete immutable descriptor, otherwise a stable
 *         failure status.
 * @note The Host validates and copies all nested views before publication.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_get_implementation_fn_v1)(
    void* plugin_context, const ps_operation_identity_v1* operation_identity,
    uint32_t implementation_index,
    ps_operation_implementation_descriptor_v1* implementation_out,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Validates one immutable configuration without creating state.
 * @param plugin_context Opaque generation context from the root table.
 * @param operation_identity Nonnull permanent operation identity.
 * @param configuration Nonnull callback-scoped flattened configuration tree.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` when the configuration is supported, otherwise a stable
 *         validation or sink failure status.
 * @note The producer must not retain any configuration or sink pointer.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_validate_configuration_fn_v1)(
    void* plugin_context, const ps_operation_identity_v1* operation_identity,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Creates one plugin-owned configured context, which may be null.
 * @param plugin_context Opaque generation context from the root table.
 * @param operation_identity Nonnull permanent operation identity.
 * @param implementation_identity Nonnull permanent implementation identity.
 * @param configuration Nonnull callback-scoped flattened configuration tree.
 * @param configured_context_out Nonnull destination written exactly once on
 *        success; a null context is a valid successful result.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` after creating the context, otherwise a stable failure status.
 * @note Each successful call is paired with exactly one destroy callback after
 *       all invocations using the context have drained.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_create_configured_context_fn_v1)(
    void* plugin_context, const ps_operation_identity_v1* operation_identity,
    const ps_operation_identity_v1* implementation_identity,
    const ps_operation_configuration_view_v1* configuration,
    void** configured_context_out,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Destroys one successfully created configured context exactly once.
 * @param plugin_context Opaque generation context from the root table.
 * @param operation_identity Permanent operation identity used at creation.
 * @param implementation_identity Permanent implementation identity used at
 *        creation.
 * @param configured_context Opaque created context, including a valid null.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` after destruction or a stable diagnostic failure status.
 * @note The Host invokes this only after in-flight context users have drained;
 *       the callback must not retain `sink` or unwind through the C boundary.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_destroy_configured_context_fn_v1)(
    void* plugin_context, const ps_operation_identity_v1* operation_identity,
    const ps_operation_identity_v1* implementation_identity,
    void* configured_context,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Emits complete immutable output plans before Host allocation.
 * @param plugin_context Opaque generation context from the root table.
 * @param invocation Nonnull invocation identity and configured-context view.
 * @param configuration Nonnull immutable flattened configuration tree.
 * @param input_bindings Nonnull exact-stride array of immutable input bindings.
 * @param sink Host sink accepting only bounded output-plan/diagnostic records.
 * @return `OK` after emitting the complete plan set, otherwise a stable failure
 *         or sticky sink status.
 * @note No output storage exists during this phase. All arguments are borrowed
 *       for this synchronous call and must not be retained.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_infer_output_plans_fn_v1)(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* input_bindings,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Maps demanded output Regions to exact upstream input bindings.
 * @param plugin_context Opaque generation context from the root table.
 * @param invocation Nonnull immutable invocation identity.
 * @param configuration Nonnull immutable flattened configuration tree.
 * @param input_bindings Exact-stride immutable input-binding array.
 * @param demanded_output_region_bindings Exact-stride demanded output Regions.
 * @param sink Host sink accepting bounded Region-binding/diagnostic records.
 * @return `OK` after complete emission, otherwise a stable failure status.
 * @note The mapping is pure for the supplied immutable snapshot and all
 *       callback-scoped records must be consumed synchronously.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_propagate_region_backward_fn_v1)(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* input_bindings,
    const ps_operation_array_ref_v1* demanded_output_region_bindings,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Maps one active input-edge Region to affected output Regions.
 * @param plugin_context Opaque generation context from the root table.
 * @param invocation Nonnull immutable invocation identity.
 * @param configuration Nonnull immutable flattened configuration tree.
 * @param input_bindings Exact-stride immutable input-binding array.
 * @param active_input_edge_identity Nonnull invocation-local input edge.
 * @param changed_input_regions Nonnull exact changed Region set for that edge.
 * @param sink Host sink accepting bounded Region-binding/diagnostic records.
 * @return `OK` after complete emission, otherwise a stable failure status.
 * @note The callback must not retain Host pointers or emit out-of-scope edges.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_propagate_region_forward_fn_v1)(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* input_bindings,
    const ps_operation_identity_v1* active_input_edge_identity,
    const ps_operation_region_set_view_v1* changed_input_regions,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Emits bounded dependency rows for one immutable invocation.
 * @param plugin_context Opaque generation context from the root table.
 * @param invocation Nonnull immutable invocation identity.
 * @param configuration Nonnull immutable flattened configuration tree.
 * @param input_bindings Exact-stride immutable input-binding array.
 * @param demanded_output_region_bindings Exact-stride demanded output Regions.
 * @param sink Host sink accepting bounded dependency/diagnostic records.
 * @return `OK` after complete emission, otherwise a stable failure status.
 * @note Dependencies use identities and Regions only; no process pointer may
 *       escape or be serialized by an isolation transport.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_build_dependencies_fn_v1)(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* input_bindings,
    const ps_operation_array_ref_v1* demanded_output_region_bindings,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Executes one complete synchronous Host-planned invocation.
 * @param plugin_context Opaque generation context from the root table.
 * @param invocation Nonnull immutable invocation identity and context view.
 * @param configuration Nonnull immutable flattened configuration tree.
 * @param input_bindings Exact-stride immutable input-binding array.
 * @param mutable_output_bindings Exact-stride Host-owned grant array.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` only after all authorized output writes are complete, otherwise
 *         a stable failure or sticky sink status.
 * @note Writes must stay inside every granted span and declared output Region;
 *       native async work and retention beyond return are forbidden in v1.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_execute_monolithic_fn_v1)(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* input_bindings,
    const ps_operation_array_ref_v1* mutable_output_bindings,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Executes one checked synchronous tile of a Host-planned invocation.
 * @param plugin_context Opaque generation context from the root table.
 * @param invocation Nonnull immutable invocation identity and context view.
 * @param configuration Nonnull immutable flattened configuration tree.
 * @param input_bindings Exact-stride immutable input-binding array.
 * @param mutable_output_bindings Exact-stride Host-owned grant array.
 * @param tile Nonnull immutable tile identity and signed requested Region.
 * @param sink Callback-scoped Host diagnostic sink.
 * @return `OK` only after all authorized tile writes are complete, otherwise a
 *         stable failure or sticky sink status.
 * @note Writes must stay inside both the tile and every granted span; native
 *       async work and retention beyond return are forbidden in v1.
 */
typedef ps_operation_status_v1(
    PS_OPERATION_CALL* ps_operation_execute_tiled_fn_v1)(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* input_bindings,
    const ps_operation_array_ref_v1* mutable_output_bindings,
    const ps_operation_tile_v1* tile,
    const ps_operation_output_sink_v1* sink) PS_OPERATION_NOEXCEPT;
/**
 * @brief Gives every reserved suite slot one portable function-pointer type.
 * @note Every reserved slot must be null in ABI v1 and is never invoked. Its
 *       typed form avoids representing a function pointer as a data pointer.
 */
typedef void(PS_OPERATION_CALL* ps_operation_reserved_callback_fn_v1)(void)
    PS_OPERATION_NOEXCEPT;

/** @brief One bounded Host-owned callback diagnostic record. */
struct ps_operation_diagnostic_v1 {
  /** @brief Exact Diagnostic record header. */
  ps_operation_record_header_v1 header;
  /** @brief Stable status associated with this message. */
  ps_operation_status_v1 status;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Borrowed UTF-8 message copied synchronously by the Host. */
  ps_operation_bytes_v1 message;
  /** @brief Must be zero. */
  uint64_t reserved1;
};

/** @brief Host-owned synchronous sticky-failure output sink. */
struct ps_operation_output_sink_v1 {
  /** @brief Exact OutputSink record header. */
  ps_operation_record_header_v1 header;
  /** @brief Opaque Host callback state, never retained by the plugin. */
  void* host_context;
  /** @brief Required synchronous Host emission callback. */
  ps_operation_emit_fn_v1 emit;
  /** @brief Must be all zero. */
  uint64_t reserved[2];
};

/** @brief One node in a bounded immutable configuration tree. */
struct ps_operation_configuration_node_v1 {
  /** @brief Exact ConfigurationNode record header. */
  ps_operation_record_header_v1 header;
  /** @brief One closed configuration kind. */
  ps_operation_configuration_kind_v1 node_kind;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Object key bytes; empty for non-object children/root as specified.
   */
  ps_operation_bytes_v1 key;
  /** @brief Scalar or byte-view storage interpreted by `node_kind`. */
  ps_operation_configuration_value_v1 value;
  /** @brief Dense first child index for Array/Object, otherwise zero. */
  uint32_t first_child;
  /** @brief Exact contiguous child count. */
  uint32_t child_count;
};

/** @brief Complete bounded immutable configuration-tree view. */
struct ps_operation_configuration_view_v1 {
  /** @brief Exact ConfigurationView record header. */
  ps_operation_record_header_v1 header;
  /** @brief Dense root-node index. */
  uint32_t root_index;
  /** @brief Exact node count. */
  uint32_t node_count;
  /** @brief Borrowed node array, null exactly when count is zero. */
  const ps_operation_configuration_node_v1* nodes;
  /** @brief Exact ConfigurationNode stride, zero only for an empty tree. */
  uint32_t node_stride;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Checked aggregate borrowed string/byte count. */
  uint64_t total_borrowed_bytes;
};

/** @brief One complete immutable operation definition. */
struct ps_operation_descriptor_v1 {
  /** @brief Exact OperationDescriptor record header. */
  ps_operation_record_header_v1 header;
  /** @brief Permanent nonzero operation identity. */
  ps_operation_identity_v1 operation_identity;
  /** @brief Canonical nonempty operation type segment. */
  ps_operation_bytes_v1 type;
  /** @brief Canonical nonempty operation subtype segment. */
  ps_operation_bytes_v1 subtype;
  /** @brief Optional bounded diagnostic display name. */
  ps_operation_bytes_v1 display_name;
  /** @brief Permanent nonzero configuration-Schema identity. */
  ps_operation_identity_v1 configuration_schema_identity;
  /** @brief Exact-stride input PortDescriptor array. */
  ps_operation_array_ref_v1 input_ports;
  /** @brief Exact-stride output PortDescriptor array. */
  ps_operation_array_ref_v1 output_ports;
};

/** @brief One immutable schedulable implementation definition. */
struct ps_operation_implementation_descriptor_v1 {
  /** @brief Exact ImplementationDescriptor record header. */
  ps_operation_record_header_v1 header;
  /** @brief Permanent nonzero implementation identity. */
  ps_operation_identity_v1 implementation_identity;
  /** @brief Exact parent operation identity. */
  ps_operation_identity_v1 operation_identity;
  /** @brief Canonical nonempty bounded implementation name. */
  ps_operation_bytes_v1 name;
  /** @brief Checked HP/RT intent-mask bits. */
  ps_operation_intent_mask_v1 intent_mask;
  /** @brief Checked Monolithic/Tiled shape-mask bits. */
  ps_operation_execution_shape_mask_v1 execution_shape_mask;
  /** @brief Exact device profile; v1 accepts CPU only. */
  ps_operation_device_kind_v1 device_kind;
  /** @brief Checked SideEffect/DataDependent behavior bits. */
  ps_operation_behavior_mask_v1 behavior_mask;
  /** @brief Aggregate declared input access bits. */
  ps_operation_access_mask_v1 input_access_mask;
  /** @brief Aggregate declared output access bits. */
  ps_operation_access_mask_v1 output_access_mask;
  /** @brief Boolean reentrancy declaration, exactly zero or one. */
  uint32_t reentrant;
  /** @brief Exact-identity callback cap; zero means no additional cap. */
  uint32_t maximum_parallelism;
  /** @brief Empty or rank-sized stride-8 `uint64_t` preferred tile extents. */
  ps_operation_array_ref_v1 tile_extents;
  /** @brief Additional retained bytes per entered callback. */
  uint64_t retained_memory_bytes;
  /** @brief Additional scratch bytes per entered callback. */
  uint64_t scratch_bytes;
  /** @brief Finite positive IEEE-754 binary64 relative-cost bits. */
  uint64_t relative_cost_binary64_bits;
  /** @brief Optional bounded process-exclusive key bytes. */
  ps_operation_bytes_v1 exclusive_key;
  /** @brief Trusted-in-process or supervised-process route. */
  ps_operation_execution_mode_v1 execution_mode;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Nonzero only for a supervised signed runtime package. */
  ps_operation_identity_v1 runtime_package_identity;
  /** @brief Must be all zero. */
  uint64_t reserved[2];
};

/** @brief One immutable operation input or output port definition. */
struct ps_operation_port_descriptor_v1 {
  /** @brief Exact PortDescriptor record header. */
  ps_operation_record_header_v1 header;
  /** @brief Permanent nonzero port identity. */
  ps_operation_identity_v1 port_identity;
  /** @brief Dense zero-based index in its direction. */
  uint32_t index;
  /** @brief Input or Output direction. */
  ps_operation_port_direction_v1 direction;
  /** @brief Canonical nonempty bounded port name. */
  ps_operation_bytes_v1 name;
  /** @brief Required representation-Schema identity. */
  ps_operation_identity_v1 schema_identity;
  /** @brief Required Facet identity, or zero when the port has none. */
  ps_operation_identity_v1 facet_identity;
  /** @brief Required Layout identity, or zero when unconstrained. */
  ps_operation_identity_v1 layout_identity;
  /** @brief Must be zero. */
  uint64_t reserved0;
};

/** @brief Complete immutable logical/physical descriptor projection. */
struct ps_operation_value_descriptor_v1 {
  /** @brief Exact ValueDescriptor record header. */
  ps_operation_record_header_v1 header;
  /** @brief Permanent representation-Schema identity. */
  ps_operation_identity_v1 schema_identity;
  /** @brief Permanent primary Facet identity or zero when absent. */
  ps_operation_identity_v1 facet_identity;
  /** @brief Permanent Layout identity. */
  ps_operation_identity_v1 layout_identity;
  /** @brief Nonzero descriptor structural version. */
  uint64_t descriptor_version;
  /** @brief Nonzero layout structural version. */
  uint64_t layout_version;
  /** @brief Exact canonical descriptor digest or all zero when unavailable. */
  ps_operation_sha256_digest_v1 descriptor_digest;
  /** @brief Exact canonical content digest or all zero when unavailable. */
  ps_operation_sha256_digest_v1 content_digest;
  /** @brief Exact canonical layout digest or all zero when unavailable. */
  ps_operation_sha256_digest_v1 layout_digest;
  /** @brief Complete DenseTensor projection, required for DenseTensor Schema.
   */
  const ps_operation_dense_tensor_descriptor_v1* dense_tensor;
  /** @brief Complete ImageFacet projection, required for ordinary DenseImage.
   */
  const ps_operation_image_facet_v1* image_facet;
  /** @brief Complete Strided projection, required for Strided Layout. */
  const ps_operation_strided_layout_v1* strided_layout;
  /** @brief Must be all zero. */
  uint64_t reserved[3];
};

/** @brief One byte-preserving additional Facet projection. */
struct ps_operation_facet_view_v1 {
  /** @brief Exact FacetView record header. */
  ps_operation_record_header_v1 header;
  /** @brief Permanent nonzero Facet identity. */
  ps_operation_identity_v1 facet_identity;
  /** @brief Nonzero Facet structural version. */
  uint64_t facet_version;
  /** @brief Borrowed exact canonical payload bytes. */
  ps_operation_bytes_v1 canonical_payload;
  /** @brief Must be zero. */
  uint64_t reserved0;
};

/** @brief One checked callback-local storage binding view. */
struct ps_operation_buffer_view_v1 {
  /** @brief Exact BufferView record header. */
  ps_operation_record_header_v1 header;
  /** @brief Host-scoped nonzero allocation identity. */
  ps_operation_identity_v1 allocation_identity;
  /** @brief Host-scoped nonzero binding identity. */
  ps_operation_identity_v1 binding_identity;
  /** @brief Byte offset within the checked binding envelope. */
  uint64_t offset;
  /** @brief Exact checked byte size. */
  uint64_t size;
  /** @brief Checked read/write bits for this callback. */
  ps_operation_access_mask_v1 access_mask;
  /** @brief CPU in v1. */
  ps_operation_device_kind_v1 device_kind;
  /** @brief Borrowed CPU base only when payload-available is set. */
  uint8_t* cpu_data;
};

/** @brief One complete callback-local immutable Value observation. */
struct ps_operation_value_view_v1 {
  /** @brief Exact ValueView header; PayloadAvailable is the only v1 flag. */
  ps_operation_record_header_v1 header;
  /** @brief Nonnull complete descriptor projection. */
  const ps_operation_value_descriptor_v1* descriptor;
  /** @brief Host-scoped nonzero logical Value identity. */
  ps_operation_identity_v1 value_identity;
  /** @brief Nonzero Host process Value revision. */
  uint64_t revision;
  /** @brief Exact logical rank. */
  uint32_t rank;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Rank-sized exact-stride `uint64_t` extents. */
  ps_operation_array_ref_v1 extents;
  /** @brief Exact-stride additional FacetView array. */
  ps_operation_array_ref_v1 facets;
  /** @brief Exact-stride BufferView array. */
  ps_operation_array_ref_v1 buffers;
  /** @brief Must be all zero. */
  uint64_t reserved[3];
};

/** @brief One destination-indexed immutable operation input binding. */
struct ps_operation_input_binding_v1 {
  /** @brief Exact InputBinding header with closed connected/Region flags. */
  ps_operation_record_header_v1 header;
  /** @brief Exact destination port identity. */
  ps_operation_identity_v1 port_identity;
  /** @brief Host-scoped edge identity, zero only when disconnected. */
  ps_operation_identity_v1 edge_identity;
  /** @brief Dense destination input index. */
  uint32_t port_index;
  /** @brief Checked connected/Region flags. */
  ps_operation_input_binding_flags_v1 binding_flags;
  /** @brief Complete value, or null exactly when disconnected. */
  const ps_operation_value_view_v1* value;
  /** @brief Optional exact logical validity Region. */
  const ps_operation_region_set_view_v1* region;
  /** @brief Must be all zero. */
  uint64_t reserved[3];
};

/** @brief One complete immutable Host allocation plan for an output port. */
struct ps_operation_output_plan_v1 {
  /** @brief Exact OutputPlan record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact output port identity. */
  ps_operation_identity_v1 port_identity;
  /** @brief Dense output port index. */
  uint32_t port_index;
  /** @brief Exact number of OutputBufferPlan rows. */
  uint32_t buffer_count;
  /** @brief Nonnull complete output descriptor. */
  const ps_operation_value_descriptor_v1* descriptor;
  /** @brief Exact-stride OutputBufferPlan rows. */
  ps_operation_array_ref_v1 buffers;
  /** @brief Nonnull exact complete logical output Region. */
  const ps_operation_region_set_view_v1* full_region;
  /**
   * @brief Plugin writes zero during inference; Host grants a nonzero identity.
   */
  ps_operation_identity_v1 plan_identity;
  /** @brief Checked aggregate output access bits. */
  ps_operation_access_mask_v1 access_mask;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Must be all zero. */
  uint64_t reserved[2];
};

/** @brief One callback-scoped Host-owned mutable output grant. */
struct ps_operation_mutable_output_binding_v1 {
  /** @brief Exact MutableOutputBinding record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact output port identity. */
  ps_operation_identity_v1 port_identity;
  /** @brief Host-scoped allocation binding identity. */
  ps_operation_identity_v1 binding_identity;
  /** @brief Callback-scoped nonzero write-grant identity. */
  ps_operation_identity_v1 grant_identity;
  /** @brief Dense output port index. */
  uint32_t port_index;
  /** @brief Must be zero for v1. */
  uint32_t binding_flags;
  /** @brief Nonnull exact immutable plan being fulfilled. */
  const ps_operation_output_plan_v1* plan;
  /** @brief Nonnull complete descriptor echoed from the plan. */
  const ps_operation_value_descriptor_v1* descriptor;
  /** @brief Exact-stride Host-created OutputGrantSpan rows. */
  ps_operation_array_ref_v1 spans;
  /** @brief Nonnull exact logical Region covered by this grant. */
  const ps_operation_region_set_view_v1* region;
  /** @brief Must be all zero. */
  uint64_t reserved[2];
};

/** @brief One immutable Host-minted callback invocation identity. */
struct ps_operation_invocation_v1 {
  /** @brief Exact Invocation record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact published plugin generation handle. */
  ps_operation_generation_handle_v1 generation_handle;
  /** @brief Exact callback invocation handle. */
  ps_operation_invocation_handle_v1 invocation_handle;
  /** @brief Exact operation identity. */
  ps_operation_identity_v1 operation_identity;
  /** @brief Exact selected implementation identity. */
  ps_operation_identity_v1 implementation_identity;
  /** @brief Opaque configured context returned for these identities. */
  void* configured_context;
  /** @brief Exactly one supported active intent bit. */
  ps_operation_intent_mask_v1 intent_mask;
  /** @brief Must be zero. */
  uint32_t reserved0;
};

/** @brief One exact logical Region atom with an explicit domain. */
struct ps_operation_region_atom_v1 {
  /** @brief Exact RegionAtom record header. */
  ps_operation_record_header_v1 header;
  /** @brief ImageRect or TensorSlice. */
  ps_operation_region_atom_kind_v1 atom_kind;
  /** @brief Two for ImageRect, exact tensor rank for TensorSlice. */
  uint32_t rank;
  /** @brief Explicit nonzero logical coordinate-domain identity. */
  ps_operation_identity_v1 domain_identity;
  /** @brief Rank-sized exact-stride AxisRange array. */
  ps_operation_array_ref_v1 axis_ranges;
  /** @brief Must be all zero. */
  uint64_t reserved[5];
};

/** @brief Canonical Empty, Whole, or one bounded Region clause. */
struct ps_operation_region_set_view_v1 {
  /** @brief Exact RegionSetView record header. */
  ps_operation_record_header_v1 header;
  /** @brief Empty, Whole, or Clause representation. */
  ps_operation_region_set_kind_v1 set_kind;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Exact-stride RegionAtom rows for Clause, empty otherwise. */
  ps_operation_array_ref_v1 atoms;
  /** @brief Must be zero. */
  uint64_t reserved1;
};

/** @brief One Region result bound to an operation port and optional edge. */
struct ps_operation_region_binding_v1 {
  /** @brief Exact RegionBinding record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact bound port identity. */
  ps_operation_identity_v1 port_identity;
  /** @brief Exact edge identity or zero for an output-only binding. */
  ps_operation_identity_v1 edge_identity;
  /** @brief Region value for Exact, otherwise null. */
  const ps_operation_region_set_view_v1* region;
  /** @brief Exact, Unknown, Unsupported, or TooComplex. */
  ps_operation_region_outcome_v1 outcome;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Must be all zero. */
  uint64_t reserved[2];
};

/** @brief One bounded output-site to upstream-edge dependency row. */
struct ps_operation_dependency_record_v1 {
  /** @brief Exact DependencyRecord record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact output port identity. */
  ps_operation_identity_v1 output_port_identity;
  /** @brief Host-scoped output-site identity. */
  ps_operation_identity_v1 output_site_identity;
  /** @brief Host-scoped output-Region identity. */
  ps_operation_identity_v1 output_region_identity;
  /** @brief Exact upstream input-edge identity. */
  ps_operation_identity_v1 input_edge_identity;
  /** @brief Nonnull exact required upstream Region. */
  const ps_operation_region_set_view_v1* input_region;
  /** @brief Must be zero. */
  uint64_t reserved0;
};

/** @brief One checked rank-general execution tile. */
struct ps_operation_tile_v1 {
  /** @brief Exact Tile record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact tile rank. */
  uint32_t rank;
  /** @brief Dense invocation-local tile index. */
  uint32_t tile_index;
  /** @brief Rank-sized exact-stride AxisRange rows. */
  ps_operation_array_ref_v1 axis_ranges;
  /** @brief Must be all zero. */
  uint64_t reserved[3];
};

/** @brief Complete bounded DenseTensor logical descriptor. */
struct ps_operation_dense_tensor_descriptor_v1 {
  /** @brief Exact DenseTensorDescriptor record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact positive logical rank. */
  uint32_t rank;
  /** @brief Unsigned, signed, or floating-point semantics. */
  ps_operation_element_semantics_v1 element_semantics;
  /** @brief NativeScalar or FP4 E2M1 encoding. */
  ps_operation_storage_encoding_v1 storage_encoding;
  /** @brief Exact positive physical bit width. */
  uint32_t bit_width;
  /** @brief Rank-sized exact-stride `uint64_t` positive extents. */
  ps_operation_array_ref_v1 extents;
  /** @brief Optional rank-sized stride-8 quantization block shape. */
  ps_operation_array_ref_v1 quantization_block_shape;
  /** @brief Optional stride-4 binary32 scale bits. */
  ps_operation_array_ref_v1 quantization_scales_binary32;
  /** @brief Boolean quantization presence, exactly zero or one. */
  uint32_t quantization_present;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Must be zero. */
  uint64_t reserved1;
};

/** @brief Complete checked Strided physical layout projection. */
struct ps_operation_strided_layout_v1 {
  /** @brief Exact StridedLayout record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact stride rank. */
  uint32_t rank;
  /** @brief Dense referenced BufferView/plan-buffer index. */
  uint32_t buffer_index;
  /** @brief Byte offset of logical coordinate zero. */
  uint64_t byte_offset;
  /** @brief Rank-sized exact-stride `int64_t` byte strides. */
  ps_operation_array_ref_v1 byte_strides;
  /** @brief Exact containing checked storage span. */
  uint64_t storage_size;
  /** @brief Must be zero. */
  uint64_t reserved0;
};

/** @brief Complete bounded ordinary DenseImage interpretation. */
struct ps_operation_image_facet_v1 {
  /** @brief Exact ImageFacet record header. */
  ps_operation_record_header_v1 header;
  /** @brief Explicit in-rank x axis. */
  uint32_t x_axis;
  /** @brief Explicit distinct in-rank y axis. */
  uint32_t y_axis;
  /** @brief Explicit channel axis; zero when its presence bit is clear. */
  uint32_t channel_axis;
  /** @brief Closed optional-field presence bits. */
  ps_operation_image_facet_presence_v1 presence_mask;
  /** @brief Required signed nonempty payload data window. */
  ps_operation_image_bounds_v1 data_window;
  /** @brief Optional signed display window; zero when absent. */
  ps_operation_image_bounds_v1 display_window;
  /** @brief Exact-stride Channel rows, empty when schema absent. */
  ps_operation_array_ref_v1 channels;
  /** @brief Exact-stride ChannelGroup rows, empty when schema absent. */
  ps_operation_array_ref_v1 channel_groups;
  /** @brief Optional complete SampleDomainFacet. */
  const ps_operation_sample_domain_facet_v1* sample_domain;
  /** @brief Optional complete ColorFacet. */
  const ps_operation_color_facet_v1* color;
  /** @brief Must be all zero. */
  uint64_t reserved[2];
};

/** @brief One stable channel identity and diagnostic name. */
struct ps_operation_channel_v1 {
  /** @brief Exact Channel record header. */
  ps_operation_record_header_v1 header;
  /** @brief Nonzero stable channel identity. */
  uint64_t channel_id;
  /** @brief Optional bounded diagnostic name. */
  ps_operation_bytes_v1 diagnostic_name;
  /** @brief Must be zero. */
  uint64_t reserved0;
};

/** @brief One stable bounded channel group. */
struct ps_operation_channel_group_v1 {
  /** @brief Exact ChannelGroup record header. */
  ps_operation_record_header_v1 header;
  /** @brief Nonzero stable group identity. */
  uint64_t channel_group_id;
  /** @brief Optional bounded diagnostic group name. */
  ps_operation_bytes_v1 diagnostic_name;
  /** @brief Sorted unique stride-8 `uint64_t` channel identities. */
  ps_operation_array_ref_v1 member_channel_ids;
  /** @brief Must be zero. */
  uint64_t reserved0;
};

/** @brief One per-channel replacement sample-domain record. */
struct ps_operation_channel_sample_domain_v1 {
  /** @brief Exact ChannelSampleDomain record header. */
  ps_operation_record_header_v1 header;
  /** @brief Existing nonzero stable channel identity. */
  uint64_t channel_id;
  /** @brief Complete finite replacement interval. */
  ps_operation_sample_domain_v1 domain;
  /** @brief Must be all zero. */
  uint64_t reserved[2];
};

/** @brief Complete bounded declared sample interpretation. */
struct ps_operation_sample_domain_facet_v1 {
  /** @brief Exact SampleDomainFacet record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact facet structural version; v1 requires one. */
  uint32_t structural_version;
  /** @brief Exact SampleEncoding structural version; v1 requires one. */
  uint32_t encoding_structural_version;
  /** @brief Value, Normalized, or CodeValue encoding. */
  ps_operation_sample_encoding_v1 encoding_kind;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Default finite sample interval. */
  ps_operation_sample_domain_v1 default_domain;
  /** @brief Exact-stride sorted ChannelSampleDomain rows. */
  ps_operation_array_ref_v1 per_channel;
  /** @brief Must be zero. */
  uint64_t reserved1;
};

/** @brief Complete explicit color interpretation bound to one group. */
struct ps_operation_color_facet_v1 {
  /** @brief Exact ColorFacet record header. */
  ps_operation_record_header_v1 header;
  /** @brief Exact structural version; v1 requires one. */
  uint32_t structural_version;
  /** @brief Explicit transfer-function classification. */
  ps_operation_color_transfer_v1 transfer;
  /** @brief Explicit color-primary classification. */
  ps_operation_color_primaries_v1 primaries;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Existing nonzero stable channel-group identity. */
  uint64_t channel_group_id;
  /** @brief Must be all zero. */
  uint64_t reserved[3];
};

/** @brief One immutable Host allocation row in an OutputPlan. */
struct ps_operation_output_buffer_plan_v1 {
  /** @brief Exact OutputBufferPlan record header. */
  ps_operation_record_header_v1 header;
  /** @brief Dense output buffer index. */
  uint32_t buffer_index;
  /** @brief Checked required access bits. */
  ps_operation_access_mask_v1 access_mask;
  /** @brief Byte offset within the planned binding. */
  uint64_t byte_offset;
  /** @brief Exact positive planned byte size. */
  uint64_t byte_size;
  /** @brief Exact positive power-of-two base alignment. */
  uint64_t alignment;
  /** @brief Must be all zero. */
  uint64_t reserved[2];
};

/** @brief One checked callback-scoped Host-owned writable span. */
struct ps_operation_output_grant_span_v1 {
  /** @brief Exact OutputGrantSpan record header. */
  ps_operation_record_header_v1 header;
  /** @brief Checked byte offset from the Host allocation base. */
  uint64_t allocation_offset;
  /** @brief Exact positive writable byte size. */
  uint64_t byte_size;
  /** @brief Borrowed mutable bytes; its size must equal `byte_size`. */
  ps_operation_mutable_bytes_v1 bytes;
  /** @brief Proven positive power-of-two address alignment. */
  uint64_t alignment;
  /** @brief Must be zero. */
  uint64_t reserved0;
};

/** @brief Definition-suite v1 callback table. */
typedef struct ps_operation_definition_suite_v1 {
  /** @brief Exact Host-prepared Definition suite prefix. */
  ps_operation_suite_header_v1 header;
  /** @brief Required operation-count callback. */
  ps_operation_get_operation_count_fn_v1 get_operation_count;
  /** @brief Required operation-descriptor callback. */
  ps_operation_get_operation_fn_v1 get_operation;
  /** @brief Required implementation-count callback. */
  ps_operation_get_implementation_count_fn_v1 get_implementation_count;
  /** @brief Required implementation-descriptor callback. */
  ps_operation_get_implementation_fn_v1 get_implementation;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved0;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved1;
} ps_operation_definition_suite_v1;

/** @brief Configuration-suite v1 callback table. */
typedef struct ps_operation_configuration_suite_v1 {
  /** @brief Exact Host-prepared Configuration suite prefix. */
  ps_operation_suite_header_v1 header;
  /** @brief Required pure validation callback. */
  ps_operation_validate_configuration_fn_v1 validate_configuration;
  /** @brief Required configured-context create callback. */
  ps_operation_create_configured_context_fn_v1 create_configured_context;
  /** @brief Required configured-context destroy callback. */
  ps_operation_destroy_configured_context_fn_v1 destroy_configured_context;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved0;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved1;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved2;
} ps_operation_configuration_suite_v1;

/** @brief Inference-suite v1 callback table. */
typedef struct ps_operation_inference_suite_v1 {
  /** @brief Exact Host-prepared Inference suite prefix. */
  ps_operation_suite_header_v1 header;
  /** @brief Required complete output-plan inference callback. */
  ps_operation_infer_output_plans_fn_v1 infer_output_plans;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved0;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved1;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved2;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved3;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved4;
} ps_operation_inference_suite_v1;

/** @brief Region-suite v1 callback table. */
typedef struct ps_operation_region_suite_v1 {
  /** @brief Exact Host-prepared Region suite prefix. */
  ps_operation_suite_header_v1 header;
  /** @brief Required backward propagation callback. */
  ps_operation_propagate_region_backward_fn_v1 propagate_backward;
  /** @brief Required forward propagation callback. */
  ps_operation_propagate_region_forward_fn_v1 propagate_forward;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved0;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved1;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved2;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved3;
} ps_operation_region_suite_v1;

/** @brief Dependency-suite v1 callback table. */
typedef struct ps_operation_dependency_suite_v1 {
  /** @brief Exact Host-prepared Dependency suite prefix. */
  ps_operation_suite_header_v1 header;
  /** @brief Required bounded dependency-builder callback. */
  ps_operation_build_dependencies_fn_v1 build_dependencies;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved0;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved1;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved2;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved3;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved4;
} ps_operation_dependency_suite_v1;

/** @brief Execution-suite v1 callback table. */
typedef struct ps_operation_execution_suite_v1 {
  /** @brief Exact Host-prepared Execution suite prefix. */
  ps_operation_suite_header_v1 header;
  /** @brief Required synchronous monolithic callback. */
  ps_operation_execute_monolithic_fn_v1 execute_monolithic;
  /** @brief Required synchronous tiled callback. */
  ps_operation_execute_tiled_fn_v1 execute_tiled;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved0;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved1;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved2;
  /** @brief Must be null. */
  ps_operation_reserved_callback_fn_v1 reserved3;
} ps_operation_execution_suite_v1;

/** @brief Exact operation-plugin v1 root API table. */
struct ps_operation_plugin_api_v1 {
  /** @brief Must equal `PS_OPERATION_PLUGIN_API_V1_SIZE`. */
  uint32_t struct_size;
  /** @brief Must equal `PS_OPERATION_PLUGIN_ABI_VERSION`. */
  uint32_t abi_version;
  /** @brief Must be zero. */
  uint32_t flags;
  /** @brief Must be zero. */
  uint32_t reserved0;
  /** @brief Permanent nonzero plugin identity. */
  ps_operation_identity_v1 plugin_identity;
  /** @brief Bounded immutable implementation-version UTF-8 bytes. */
  ps_operation_bytes_v1 implementation_version;
  /** @brief Opaque plugin generation context, optionally null. */
  void* plugin_context;
  /** @brief Required exact suite-query callback. */
  ps_operation_query_suite_fn_v1 query_suite;
  /** @brief Required exactly-once generation destroy callback. */
  ps_operation_destroy_plugin_fn_v1 destroy_plugin;
  /** @brief Must be all zero. */
  uint64_t reserved[3];
};

/**
 * @brief Returns the exact ABI generation implemented by this DSO.
 * @return `PS_OPERATION_PLUGIN_ABI_VERSION` for an operation-v1 candidate.
 * @throws Nothing; declared `noexcept` in C++.
 * @note The Host resolves and calls this symbol before any other candidate
 *       function. It transfers no ownership and accepts no input.
 */
PS_OPERATION_PLUGIN_EXPORT uint32_t PS_OPERATION_CALL
ps_operation_plugin_get_abi_version(void) PS_OPERATION_NOEXCEPT;

/**
 * @brief Fills one Host-prepared exact operation-v1 root table.
 * @param api_out Nonnull 96-byte Host-owned root initialized with the exact
 *        size/version/zero prefix.
 * @return Stable v1 status; `OK` requires every root field to be valid.
 * @throws Nothing; declared `noexcept` in C++.
 * @note The plugin preserves the Host-authored prefix, fills only declared
 *       fields, and transfers no allocator or C++ ownership across the call.
 */
PS_OPERATION_PLUGIN_EXPORT ps_operation_status_v1 PS_OPERATION_CALL
ps_operation_plugin_get_api_v1(ps_operation_plugin_api_v1* api_out)
    PS_OPERATION_NOEXCEPT;

/* The header is also the first independent conformance oracle. */
PS_OPERATION_STATIC_ASSERT(CHAR_BIT == 8, "operation ABI requires 8-bit bytes");
PS_OPERATION_STATIC_ASSERT(sizeof(uint32_t) == 4U,
                           "operation ABI requires 32-bit uint32_t");
PS_OPERATION_STATIC_ASSERT(sizeof(uint64_t) == 8U,
                           "operation ABI requires 64-bit uint64_t");
PS_OPERATION_STATIC_ASSERT(sizeof(void*) == 8U,
                           "operation ABI requires 64-bit pointers");
PS_OPERATION_STATIC_ASSERT(sizeof(ps_operation_query_suite_fn_v1) == 8U,
                           "operation ABI requires 64-bit function pointers");

#define PS_OPERATION_ASSERT_LAYOUT(type, size_value)                       \
  PS_OPERATION_STATIC_ASSERT(sizeof(type) == (size_value), #type " size"); \
  PS_OPERATION_STATIC_ASSERT(PS_OPERATION_ALIGNOF(type) == 8U,             \
                             #type " alignment")

PS_OPERATION_STATIC_ASSERT(sizeof(ps_operation_record_header_v1) == 16U,
                           "record header size");
PS_OPERATION_STATIC_ASSERT(
    PS_OPERATION_ALIGNOF(ps_operation_record_header_v1) == 4U,
    "record header alignment");
PS_OPERATION_STATIC_ASSERT(sizeof(ps_operation_suite_header_v1) == 16U,
                           "suite header size");
PS_OPERATION_STATIC_ASSERT(PS_OPERATION_ALIGNOF(ps_operation_suite_header_v1) ==
                               4U,
                           "suite header alignment");
PS_OPERATION_ASSERT_LAYOUT(ps_operation_identity_v1, 16U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_generation_handle_v1, 16U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_invocation_handle_v1, 16U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_bytes_v1, 16U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_mutable_bytes_v1, 16U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_array_ref_v1, 16U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_configuration_value_v1, 16U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_axis_range_v1, 16U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_image_bounds_v1, 32U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_sample_domain_v1, 24U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_sha256_digest_v1, 32U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_diagnostic_v1, 48U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_output_sink_v1, 48U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_configuration_node_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_configuration_view_v1, 48U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_descriptor_v1, 128U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_implementation_descriptor_v1, 192U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_port_descriptor_v1, 112U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_value_descriptor_v1, 224U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_facet_view_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_buffer_view_v1, 80U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_value_view_v1, 128U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_input_binding_v1, 96U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_output_plan_v1, 112U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_mutable_output_binding_v1, 128U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_invocation_v1, 96U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_region_atom_v1, 96U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_region_set_view_v1, 48U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_region_binding_v1, 80U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_dependency_record_v1, 96U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_tile_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_dense_tensor_descriptor_v1, 96U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_strided_layout_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_image_facet_v1, 160U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_channel_v1, 48U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_channel_group_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_channel_sample_domain_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_sample_domain_facet_v1, 80U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_color_facet_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_output_buffer_plan_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_output_grant_span_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_definition_suite_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_configuration_suite_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_inference_suite_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_region_suite_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_dependency_suite_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_execution_suite_v1, 64U);
PS_OPERATION_ASSERT_LAYOUT(ps_operation_plugin_api_v1, 96U);

PS_OPERATION_STATIC_ASSERT(offsetof(ps_operation_plugin_api_v1, query_suite) ==
                               56U,
                           "root query_suite offset");
PS_OPERATION_STATIC_ASSERT(offsetof(ps_operation_value_descriptor_v1,
                                    dense_tensor) == 176U,
                           "ValueDescriptor dense projection offset");
PS_OPERATION_STATIC_ASSERT(offsetof(ps_operation_image_facet_v1, data_window) ==
                               32U,
                           "ImageFacet data-window offset");
PS_OPERATION_STATIC_ASSERT(offsetof(ps_operation_mutable_output_binding_v1,
                                    spans) == 88U,
                           "MutableOutputBinding span offset");

#undef PS_OPERATION_ASSERT_LAYOUT

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_PLUGIN_API_H_ */
