/** @file result_operation_plugin_api.h
 * @brief Standalone C declarations for Result operation ABI 2.
 *
 * This header owns the Result scalar, parameter, facet, flag, and callback
 * status declarations. It does not require the base operation plugin header;
 * native GPU services use their separately versioned public header.
 */
#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_OPERATION_PLUGIN_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_OPERATION_PLUGIN_API_H_

#include <stdint.h>

#include "photospider/plugin/cpu_parallel_api.h"
#include "photospider/plugin/cpu_tiles_api.h"
#include "photospider/plugin/native_gpu_api.h"

/** @brief Export decoration for the Result module entry point.
 * `PHOTOSPIDER_OPERATION_PLUGIN_BUILD` is the shared producer-build switch.
 */
#if defined(_WIN32)
#if defined(PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
#define PS_RESULT_EXPORT __declspec(dllexport)
#else
#define PS_RESULT_EXPORT
#endif
#else
#define PS_RESULT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif
#define PS_RESULT_OPERATION_ABI_VERSION_2 2U
/** @brief Ordinary Result callback and service status values.
 * Values 0 through 6 represent success, failure, cancellation,
 * backend-unavailable, resource-exhausted, type-mismatch, and invalid-argument
 * outcomes. These codes are distinct from `ps_result_error_code_v2`, which
 * describes typed atom failure details; the two code sets are not
 * interchangeable. Unknown nonzero callback results fail closed. Poll
 * callbacks may also yield Need, publication, or an explicitly declared joint
 * Atom failure outcome.
 */
typedef enum ps_result_status_v2 {
  PS_RESULT_STATUS_OK_V2 = 0,
  PS_RESULT_STATUS_FAILURE_V2 = 1,
  PS_RESULT_STATUS_CANCELLED_V2 = 2,
  PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V2 = 3,
  PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V2 = 4,
  PS_RESULT_STATUS_TYPE_MISMATCH_V2 = 5,
  PS_RESULT_STATUS_INVALID_ARGUMENT_V2 = 6
} ps_result_status_v2;
/** @brief Operation has deterministic output for equal inputs/parameters. */
#define PS_RESULT_FLAG_DETERMINISTIC_V2 (1U << 0U)
/** @brief Operation has no externally visible side effect. */
#define PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2 (1U << 1U)
/** @brief Operation can execute on CPU; at least one CPU/GPU flag is required.
 */
#define PS_RESULT_FLAG_CPU_V2 (1U << 2U)
/** @brief Operation can execute on the optional local GPU lane. */
#define PS_RESULT_FLAG_GPU_V2 (1U << 3U)
/** @brief Output-free GPU unavailability may use CPU; requires CPU and GPU
 * flags. */
#define PS_RESULT_FLAG_CPU_FALLBACK_V2 (1U << 4U)
/** @brief Opts pure deterministic Atomic dependency-v1 or Result-v2 outputs
 * into one internal block-state namespace across outputs. Regional-atomic
 * outputs and static dependency pieces are not eligible. Block keys still
 * include all resolved outputs, backend, execution mode, static parameters,
 * input metadata, block kind/range/mode, incoming state, and supplied tensor
 * metadata and bytes. Public outputs, Needs/evidence and other caches stay
 * independent. This flag does not promise concurrent producer deduplication
 * and does not change the table layout.
 */
#define PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V2 (1U << 5U)

/** @brief Result C ABI tensor and field element-type codes.
 * Values match the public C++ element-type model and are declared here without
 * requiring the base operation plugin header.
 */
typedef enum ps_result_element_type_v2 {
  PS_RESULT_ELEMENT_UINT8_V2 = 1,
  PS_RESULT_ELEMENT_INT64_V2 = 2,
  PS_RESULT_ELEMENT_FLOAT64_V2 = 3,
  PS_RESULT_ELEMENT_FLOAT32_V2 = 4,
  PS_RESULT_ELEMENT_INT8_V2 = 5,
  PS_RESULT_ELEMENT_UINT16_V2 = 6,
  PS_RESULT_ELEMENT_INT16_V2 = 7
} ps_result_element_type_v2;

/** @brief Closed source-parameter type values for Result operation ABI 2. */
typedef enum ps_result_parameter_type_v2 {
  PS_RESULT_PARAMETER_INT64_V2 = 1,
  PS_RESULT_PARAMETER_FLOAT64_V2 = 2,
  PS_RESULT_PARAMETER_BOOL_V2 = 3,
  PS_RESULT_PARAMETER_STRING_V2 = 4
} ps_result_parameter_type_v2;

/**
 * @brief One immutable parameter declaration published by an operation.
 *
 * @note Key bytes and declaration records remain plugin-owned until the API
 * destroy callback.
 */
typedef struct ps_result_parameter_descriptor_v2 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Nonempty bounded UTF-8 parameter key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief One `ps_result_parameter_type_v2` value. */
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
} ps_result_parameter_descriptor_v2;

/**
 * @brief One canonical source parameter supplied to a callback.
 *
 * @note Exactly the field selected by `type` is meaningful; every pointer is
 * callback-local and must not be retained.
 */
typedef struct ps_result_parameter_value_v2 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief Nonempty schema-declared UTF-8 parameter key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief One `ps_result_parameter_type_v2` value. */
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
} ps_result_parameter_value_v2;

/**
 * @brief One bounded immutable semantic facet visible to an operation.
 *
 * @note Key and payload pointers remain valid only for the callback duration.
 */
typedef struct ps_result_facet_view_v2 {
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
} ps_result_facet_view_v2;

#define PS_RESULT_NEED_V2 100
#define PS_RESULT_PUBLISH_V2 101
#define PS_RESULT_ATOM_FAILURE_V2 102
/** @brief Deliver an independent outcome for each Atomic observation member. */
#define PS_RESULT_PER_ATOM_OUTCOME_V2 1U
#define PS_RESULT_OBJECT_V2 6U
#define PS_RESULT_WHOLE_V2 1U
#define PS_RESULT_REGIONAL_V2 2U
/** @brief Per-output sampling unit: one Atomic observation. */
#define PS_RESULT_ATOMIC_V2 0U
/** @brief Per-output terminal sampling unit: one complete captured query. */
#define PS_RESULT_REQUEST_RECORD_V2 1U
/** @brief Deliver one failure for a RequestFailureOnly output request. */
#define PS_RESULT_REQUEST_FAILURE_ONLY_V2 0U
#define PS_RESULT_TARGET_FIELD_V2 1U
#define PS_RESULT_TARGET_TENSOR_V2 2U
#define PS_RESULT_TARGET_DESCRIPTOR_V2 3U
#define PS_RESULT_EXACT_V2 1U
#define PS_RESULT_CONSERVATIVE_V2 2U
#define PS_RESULT_UNKNOWN_V2 3U
#define PS_RESULT_FINAL_V2 15U
#define PS_RESULT_NO_CHANNEL_V2 UINT32_MAX
#define PS_RESULT_EXTENT_FIXED_V2 1U
#define PS_RESULT_EXTENT_INPUT_AXIS_V2 2U
#define PS_RESULT_EXTENT_INPUT_ELEMENTS_V2 3U
#define PS_RESULT_EXTENT_FIELD_ROWS_V2 4U
#define PS_RESULT_EXTENT_RUNTIME_COUNT_V2 5U
#define PS_RESULT_COMPLETE_BUNDLE_V2 1U
#define PS_RESULT_INDEPENDENT_CHUNKS_V2 2U
#define PS_RESULT_STABLE_PREFIX_V2 3U

typedef struct ps_result_region_v2 {
  uint32_t struct_size, rank;
  uint64_t offset[8], extent[8];
} ps_result_region_v2;
typedef struct ps_result_group_v2 {
  uint32_t struct_size;
  const char* role;
  uint32_t role_size;
  uint64_t first_channel, channel_count;
} ps_result_group_v2;
typedef struct ps_result_tensor_spec_v2 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size, element_type, rank;
  uint64_t shape[8], batch_shape[8];
  uint32_t batch_rank, atomic_trailing_axes, spatial;
  uint32_t height_axis, width_axis, channel_axis;
  uint32_t storage_order; /* 0=continuous, 1=tiled. */
  uint64_t row_pitch_bytes;
  const ps_result_group_v2* groups;
  uint32_t group_count;
  const ps_result_facet_view_v2* facets;
  uint32_t facet_count;
} ps_result_tensor_spec_v2;
typedef struct ps_result_extent_v2 {
  uint32_t kind, input, axis, field;
  uint64_t value, divisor, offset;
} ps_result_extent_v2;
typedef struct ps_result_field_spec_v2 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size, element_type, record_rank;
  uint64_t record_shape[7];
  ps_result_extent_v2 rows;
} ps_result_field_spec_v2;
typedef struct ps_result_schema_v2 {
  uint32_t struct_size;
  const char* id;
  uint32_t id_size, version, publication;
  const ps_result_field_spec_v2* fields;
  uint32_t field_count;
  const ps_result_tensor_spec_v2* tensors;
  uint32_t tensor_count;
  const ps_result_extent_v2* domain;
  uint32_t domain_rank;
  const ps_result_facet_view_v2* metadata;
  uint32_t metadata_count;
} ps_result_schema_v2;
typedef struct ps_result_port_v2 {
  uint32_t struct_size, kind, element_type, rank;
  const ps_result_schema_v2* schema;
  const ps_result_facet_view_v2* facets;
  uint32_t facet_count;
  float minimum, maximum;
  uint32_t semantic_kind, element_type_mask;
  /** @brief Optional named tensor predicate. An input prototype may omit schema
   * with a tensor member predicate. With member predicates an omitted key
   * selects the sole tensor, and multiple tensors require a key. Without member
   * predicates the complete fixed representation is selected. Outputs must
   * supply their schema.
   * dtype/rank/facets constrain this member; requires_semantics accepts
   * recognized typed facets including ColorArray. scalar_bounds requires full
   * Float32 sample_shape {1}, dimensionless scalar semantics and finite
   * inclusive [minimum,maximum] values before consumption. Strings are copied
   * on registration and borrowed in metadata/query views.
   */
  const char* tensor_key;
  uint32_t tensor_key_size, requires_semantics, scalar_bounds;
} ps_result_port_v2;
/** @brief Policy bits carried by `ps_result_output_v2::flags`.
 * PRESERVE_VIEWS permits a callback to publish a compatible immutable input
 * view. REQUIRE_INPUT_VIEWS selects strict view admission and requires
 * PRESERVE_VIEWS; it is valid only for CPU Whole output. PAYLOAD_BOUND enables
 * `maximum_output_payload_bytes`, including an explicit zero-byte bound. If
 * PAYLOAD_BOUND is clear, the byte field must be zero. Unknown bits are
 * rejected during import. View preservation is limited to non-joint CPU
 * Atomic outputs; the compiled structured Whole path performs input view
 * preparation after Need validation and before the computation callback.
 * Payload bounds are valid for CPU Whole or staged output and are checked at
 * publication under the Result Root.
 */
#define PS_RESULT_OUTPUT_PRESERVE_VIEWS_V2 (1U << 0U)
#define PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V2 (1U << 1U)
#define PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2 (1U << 2U)
/** @brief Immutable declaration of one Result operation output.
 * `observation_kind` classifies this named output independently of `execution`.
 * Atomic outputs are observed in the normal per-observation manner.
 * RequestRecord outputs execute once for the captured semantic-closure query
 * Q. A Whole execution setting does not widen a supplied Q. The callback must
 * publish one complete Result whose selected tensor covers exactly Q; a
 * partial publication is rejected. The resolved output view passed through
 * `ps_result_query_v2::output` reflects this declaration, and metadata
 * resolution cannot change its observation or failure classification.
 *
 * RequestRecord Results are terminal: the host does not admit them as later
 * operation inputs, tensor-view sources, checkpoint state, or block state.
 * An Empty tensor Q skips the operation callback only for a RequestRecord
 * output with no fields; static validation, backend selection and resource
 * admission still apply. Atomic Empty outputs and RequestRecord outputs with
 * fields still execute their callback.
 *
 * `flags` carries the output view and payload policies documented by the
 * `PS_RESULT_OUTPUT_*_V2` constants. `REQUIRE_INPUT_VIEWS` requires
 * `PRESERVE_VIEWS` and CPU Whole execution. `PAYLOAD_BOUND` distinguishes an
 * explicit zero-byte maximum from an unset maximum. Metadata resolution may
 * refine the output port/schema, but cannot change these policies. The selected
 * query's `output` view exposes the immutable declaration to `start` and
 * `poll`.
 *
 * `struct_size` must equal `sizeof(ps_result_output_v2)`. The Result ABI 2
 * importer rejects older or extended layouts; there is no compatibility
 * prefix negotiation for this output declaration.
 */
typedef struct ps_result_output_v2 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size;
  ps_result_port_v2 port;
  /* UINT32_MAX selects every input; zero selects none. */
  const uint32_t* input_indices;
  uint32_t input_count, execution;
  /** @brief `PS_RESULT_ATOMIC_V2` or `PS_RESULT_REQUEST_RECORD_V2`. */
  uint32_t observation_kind;
  /** @brief RequestFailureOnly, or PerAtomOutcome for joint contract 2. */
  uint32_t failure_delivery;
  uint32_t flags;
  uint64_t maximum_output_payload_bytes;
} ps_result_output_v2;
typedef struct ps_result_relation_row_v2 {
  uint64_t output;
  uint32_t input, roles, target, slot;
  uint64_t first, count;
} ps_result_relation_row_v2;
/** @brief Borrowed view of one Result operation metadata resolution or stage.
 * `output` reflects the selected output declaration, including its immutable
 * observation, failure-delivery, view-policy, and payload-bound fields.
 * `requested_kind` selects the complete Result object or the tensor query
 * supplied for that output.
 * For a RequestRecord output, the tensor query is the complete terminal Q;
 * the Whole execution region does not expand it. The query and its input,
 * output, parameter, and nested facet views are borrowed for the enclosing
 * start or poll callback only. Metadata-resolver inputs, parameters, and output
 * prototypes are borrowed for that resolver call only.
 */
typedef struct ps_result_query_v2 {
  uint32_t struct_size, output_index, backend, tensor_slot;
  /** @brief 0 requests the complete Result object; 2 requests tensor coverage.
   * A kind-2 query with zero requested boxes describes Empty coverage, never
   * Whole. The Result port carries all tensor and field members. */
  uint32_t requested_kind;
  const ps_result_port_v2* inputs;
  uint32_t input_count;
  /** @brief Resolved view of the selected declared output. Classification is
   * fixed by registration and cannot be changed by metadata resolution.
   */
  const ps_result_output_v2* output;
  const ps_result_region_v2* requested;
  uint32_t requested_count;
  uint64_t tile_height, tile_width;
  const ps_result_parameter_value_v2* parameters;
  uint32_t parameter_count;
} ps_result_query_v2;

/** @brief Checkpoint state borrowed for one C Result callback epoch.
 * Set `struct_size` to this structure size and `reserved` to zero. A miss has
 * `handle == 0`, `sequence == 0`, and `byte_size == 0`. A hit identifies the
 * greatest saved sequence not greater than the requested bound and reports
 * its canonical tensor byte size. A nonzero handle is valid only through the
 * current start or poll callback; joint handles are member-scoped.
 */
typedef struct ps_result_checkpoint_v2 {
  uint32_t struct_size, reserved;
  uint64_t handle, sequence, byte_size;
} ps_result_checkpoint_v2;

/** @brief Captured immutable Result facts. No object identity is content.
 * A Result Need authorizes this descriptor; tensor Needs authorize only their
 * tensor read capabilities. rows are certified field prefixes, including zero.
 */
typedef struct ps_result_descriptor_v2 {
  uint32_t struct_size, sealed, field_count, tensor_count;
  uint64_t object_id, revision, rows[16];
} ps_result_descriptor_v2;
#define PS_RESULT_VIEW_UNAVAILABLE_V2 7
/** @brief Authorized immutable tensor samples. data points at the first
 * logical sample. Advance by signed sample_stride_bytes, including negative
 * and zero strides. bytes is the checked physical address span including the
 * final element and any stride gaps. It is not a forward memcpy length from
 * data; negative strides may place other samples before data.
 */
typedef struct ps_result_tensor_row_v2 {
  uint32_t struct_size;
  const uint8_t* data;
  uint64_t samples, bytes;
  int64_t sample_stride_bytes;
} ps_result_tensor_row_v2;
typedef struct ps_result_tensor_rectangle_v2 {
  uint32_t struct_size;
  ps_result_tensor_row_v2 row;
  uint64_t rows;
  int64_t row_stride_bytes;
} ps_result_tensor_rectangle_v2;
/** @brief Owning authorized window exported by acquire_tensor_window.
 * region includes every declared batch and cell axis. row/rectangle may run
 * concurrently on CPU workers without calling execution services; they observe
 * the captured cancellation token and record invalid access as a sticky
 * protocol failure. Borrowed data pointers remain valid until the last matching
 * window handle retires. retain_window/release_window use the current poll's
 * entry thread. Release must follow worker completion. The copied function
 * table/context stays valid until operation destroy; after the last release
 * reads fail. The window retains certified source facts, schema, associations
 * and profiles independently of later polls, producer or context lifetime. No
 * payload is copied, cached or made writable.
 */
typedef struct ps_result_tensor_window_v2 {
  uint32_t struct_size, height_axis, width_axis, channel_axis;
  /* Source identity for a singleton Result. Zero denotes a valid compound
     window with no single Result identity; it grants neither payload reads
     nor result_descriptor access. */
  uint64_t object_id;
  ps_result_region_v2 region;
  void* context;
  int (*row)(void*, const uint64_t*, uint32_t, ps_result_tensor_row_v2*);
  int (*rectangle)(void*, const uint64_t*, uint32_t,
                   ps_result_tensor_rectangle_v2*);
} ps_result_tensor_window_v2;
/** @brief Owning fixed numeric report; no borrowed identity strings. */
typedef struct ps_result_numeric_report_v2 {
  uint32_t struct_size, profile;
  char implementation[256];
  uint64_t evaluated_values, strict_fallbacks, view_elements, copied_elements;
  uint64_t fallback_reasons[4], strict_math_calls, function_fallbacks[8][4];
} ps_result_numeric_report_v2;
typedef struct ps_result_mapped_axis_v2 {
  int32_t output_axis;
  uint64_t source_origin;
  int64_t step;
  uint64_t extent, output_origin;
} ps_result_mapped_axis_v2;
/** @brief Optional affine tensor view transform. With reshape=0, axes contains
 * source_rank anchored point maps (extent=1), one for each full source axis.
 * With reshape=1, source_rank=0 and axes=NULL; logical row-major shape changes
 * require compatible signed/zero contiguous storage chunks. Borrowed for the
 * service call only; no payload copy occurs. Invalid authorization is sticky.
 */
typedef struct ps_result_tensor_transform_v2 {
  uint32_t struct_size, reshape, source_rank;
  const ps_result_mapped_axis_v2* axes;
} ps_result_tensor_transform_v2;
/** @brief Native view of one Need-authorized tensor atlas.
 * shape and tile_shape describe the full tensor sample domain; payload and
 * directory sizes are byte counts for the packed atlas. The two tokens name
 * GPU buffers created for the active poll and are valid only for that poll.
 * Repeated acquisition of the same input and slot in one poll returns the
 * same atlas and tokens while neither token has been released. If either
 * token is released, a later acquisition retires the other token from that
 * pair and returns a fresh pair; neither old token becomes valid again. This
 * record grants no additional input reads.
 */
typedef struct ps_result_native_atlas_v2 {
  uint32_t struct_size, rank, element_type, reserved;
  uint64_t shape[8], tile_shape[8];
  uint64_t slot_count, payload_sample_bytes;
  uint64_t payload_byte_size, directory_byte_size;
  uint64_t payload_token, directory_token;
} ps_result_native_atlas_v2;
/** @brief Services available to one Result block compute callback.
 * The incoming handle names an immutable generic single-tensor
 * CompleteBundle Result. The callback may read its state and Need-authorized
 * input samples, then return the incoming handle or a newly created state
 * handle. The callback cannot issue another Need. A returned new handle is
 * consumed by the host; the incoming handle remains borrowed. The service
 * table is borrowed until the callback returns, while atlas tokens remain
 * valid until the enclosing poll ends. Scratch and GPU access follow their
 * normal poll ownership. Block callbacks cannot start another block or
 * publish operation outputs.
 */
typedef struct ps_result_block_services_v2 {
  uint32_t struct_size;
  void* context;
  int (*read_tensor)(void*, uint32_t input, uint32_t slot, const uint64_t*,
                     uint32_t rank, void*, uint64_t bytes);
  int (*create_state)(void*, const ps_result_schema_v2*, const uint8_t*,
                      uint64_t bytes, uint64_t* result);
  int (*read_state)(void*, uint64_t result, uint8_t*, uint64_t bytes);
  int (*release_state)(void*, uint64_t result);
  int (*acquire_native_atlas)(void*, uint32_t input, uint32_t slot,
                              ps_result_native_atlas_v2*);
  int (*allocate_scratch)(void*, uint64_t bytes, uint8_t**);
  int (*release_scratch)(void*, uint8_t*);
  int (*consume_work)(void*, uint64_t units);
  int (*cancelled)(void*);
  const ps_gpu_service_v1* gpu;
} ps_result_block_services_v2;
typedef int (*ps_result_block_compute_v2)(const ps_result_block_services_v2*,
                                          uint64_t incoming_result,
                                          uint64_t* outgoing_result, void*);
/** @brief One normalized rectangle in a Result GPU discovery receipt.
 * `input` selects the operation input Result and `slot` selects its tensor
 * member. `region` uses that tensor's complete sample shape, batch axes first
 * and cell axes after them. `roles` is a nonempty mask of Data (1), Control
 * (2), and Validation (4), with values 1 through 7. Records with distinct
 * role masks remain distinct evidence.
 */
typedef struct ps_result_discovery_request_v2 {
  uint32_t struct_size, input, slot, roles;
  ps_result_region_v2 region;
} ps_result_discovery_request_v2;
/** @brief Restricted services borrowed during one discovery compute callback.
 * Reads and atlas acquisition are limited to already supplied tensor Needs.
 * Scratch, work, cancellation, and synchronous native GPU services are also
 * available. The table pointer and service context expire when the callback
 * returns. The callback cannot request Needs, publish outputs, call block, or
 * recursively start discovery; every failed service is sticky.
 */
typedef struct ps_result_discovery_services_v2 {
  uint32_t struct_size;
  void* context;
  int (*read_tensor)(void*, uint32_t input, uint32_t slot, const uint64_t*,
                     uint32_t rank, void*, uint64_t bytes);
  int (*acquire_native_atlas)(void*, uint32_t input, uint32_t slot,
                              ps_result_native_atlas_v2*);
  int (*allocate_scratch)(void*, uint64_t bytes, uint8_t**);
  int (*release_scratch)(void*, uint8_t*);
  int (*consume_work)(void*, uint64_t units);
  int (*cancelled)(void*);
  const ps_gpu_service_v1* gpu;
} ps_result_discovery_services_v2;
/** @brief Synchronous callback that emits a bounded GPU request table.
 * Return an ordinary Result operation status; this callback does not return
 * `PS_RESULT_NEED_V2`. Submit actual native GPU work before returning success.
 * The table and service pointers are borrowed only for this call.
 */
typedef int (*ps_result_discovery_compute_v2)(
    const ps_result_discovery_services_v2*, uint8_t* table, uint64_t byte_size,
    uint32_t capacity, void* user);
/* Services and query are borrowed for one start/poll. Tensor reads copy samples
 * from explicitly supplied Need coverage; owning windows expose zero-copy runs.
 * Retained handles preserve that coverage until release/destroy. Every failed
 * service is sticky. Temporary field I/O executes after Need yields; read_io
 * observes its next-poll reply. Entry services use the callback entry thread.
 * cancelled is also callable from CPU range or tile workers, and those workers
 * may write their admitted scratch bytes. Owning window row/rectangle access
 * may run concurrently on CPU workers without reentering execution services.
 */
typedef struct ps_result_services_v2 {
  uint32_t struct_size, abi_version;
  void* context;
  int (*need_tensor)(void*, uint32_t input, uint32_t slot, uint32_t roles,
                     const ps_result_region_v2*, uint32_t count);
  int (*need_result)(void*, uint32_t input, uint32_t field, uint32_t complete,
                     uint64_t minimum_rows);
  int (*read_tensor)(void*, uint32_t input, uint32_t slot, const uint64_t*,
                     uint32_t rank, void*, uint64_t bytes);
  int (*retain_tensor)(void*, uint32_t input, uint32_t slot, uint64_t* handle);
  int (*read_retained_tensor)(void*, uint64_t handle, const uint64_t*,
                              uint32_t rank, void*, uint64_t bytes);
  int (*release_tensor)(void*, uint64_t handle);
  int (*allocate_scratch)(void*, uint64_t bytes, uint8_t** destination);
  int (*release_scratch)(void*, uint8_t*);
  int (*consume_work)(void*, uint64_t units);
  int (*cancelled)(void*);
  int (*begin_result)(void*);
  int (*bind_descriptor)(void*, const ps_result_relation_row_v2*, uint32_t,
                         uint32_t guarantee);
  int (*publish_tensor)(void*, uint32_t slot, const ps_result_region_v2*,
                        const uint8_t*, uint64_t bytes,
                        const ps_result_relation_row_v2*, uint32_t rows,
                        uint32_t guarantee, uint32_t finality);
  int (*append_field)(void*, uint32_t field, uint64_t rows, const uint8_t*,
                      uint64_t bytes);
  int (*publish_field)(void*, uint32_t field, uint64_t end,
                       const ps_result_relation_row_v2*, uint32_t rows,
                       uint32_t guarantee, uint32_t finality);
  int (*need_field_read)(void*, uint32_t input, uint32_t field, uint64_t first,
                         uint64_t rows);
  int (*read_io)(void*, uint32_t reply, uint64_t offset, uint8_t*,
                 uint64_t bytes);
  /** @brief Publish this poll's current Result prefix or complete Result.
   * `complete` must be 0 or 1; larger values return `InvalidArgument`. A poll
   * may publish at most once: repeating a successful publication returns
   * non-Protocol `OperationFailed`, not protocol `InvalidArgument`. An invalid
   * first publication remains the sticky failure even if the callback later
   * attempts a valid publication or returns success. The host checks sticky
   * service failures before the callback outcome; only an actual host
   * cancellation can supersede a valid duplicate-publication failure at the
   * execution boundary. Callback-returned Cancelled does not. Publication
   * state resets at the next poll, so a later poll may publish another prefix.
   */
  int (*publish_result)(void*, uint32_t complete);
  /** @brief Read the captured descriptor for an explicitly requested Result.
   * The active phase must include a Result Object Need for `input`; tensor or
   * field Needs alone authorize only their own members. Without the Object
   * Need this returns sticky `InvalidArgument` with `UnauthorizedRead` and
   * Protocol/Group failure detail.
   */
  int (*result_descriptor)(void*, uint32_t input, ps_result_descriptor_v2*);
  /** @brief Acquire an exact rectangle from this phase's explicit tensor Need.
   * Handles survive polls and remain valid until release or operation destroy.
   */
  int (*acquire_tensor_window)(void*, uint32_t input, uint32_t slot,
                               const ps_result_region_v2*,
                               ps_result_tensor_window_v2*, uint64_t* handle);
  int (*acquire_retained_window)(void*, uint64_t image_handle,
                                 const ps_result_region_v2*,
                                 ps_result_tensor_window_v2*, uint64_t* handle);
  int (*retain_window)(void*, uint64_t handle, uint64_t* retained_handle);
  int (*release_window)(void*, uint64_t handle);
  /** @brief Construct exact compact support for one output/input tensor pair.
   * output_slot and input_slot select tensor members of their Result ports;
   * target must select the input tensor. Coordinates cover each member's full
   * sample shape, including batch axes. Handles retain immutable mapped
   * metadata across polls; release_relation retires them.
   */
  int (*make_mapping)(void*, uint32_t output_slot, uint32_t input,
                      uint32_t target, uint32_t input_slot, uint32_t roles,
                      const ps_result_region_v2* outputs,
                      const ps_result_mapped_axis_v2*, uint32_t axis_count,
                      uint64_t* relation);
  /** @brief Exact row-major repartition from one authorized source window.
   * Shapes are taken from the selected tensor members. Cardinality equality
   * is factorized, including domains larger than uint64. The output witness
   * does not change the full output shape's ordinal basis. Returns a retained
   * relation handle; this declaration grants no payload read authorization.
   */
  int (*make_reshape)(void*, uint32_t output_slot, uint32_t input,
                      uint32_t input_slot, uint32_t roles,
                      const ps_result_region_v2* outputs,
                      const ps_result_region_v2* source_window,
                      uint64_t* relation);
  int (*release_relation)(void*, uint64_t relation);
  int (*publish_tensor_with_relation)(void*, uint32_t slot,
                                      const ps_result_region_v2*,
                                      const uint8_t*, uint64_t bytes,
                                      uint64_t relation, uint32_t finality);
  /** @brief Publish source windows without copying payload. A NULL transform
   * assembles ordered planes with a common root and spatial map. A non-NULL
   * transform requires exactly one affine window and maps its authorized
   * samples, including negative/zero strides or compatible reshape chunks.
   * Return VIEW_UNAVAILABLE leaves the builder usable for explicit Auto copy;
   * all other service failures are sticky. Overlap and source cycles fail.
   * Publication retains sources even after every window handle is released.
   */
  int (*publish_tensor_view)(void*, uint32_t slot, const ps_result_region_v2*,
                             const uint64_t* window_handles, uint32_t count,
                             const ps_result_tensor_transform_v2* transform,
                             uint64_t relation, uint32_t finality);
  int (*report_numeric)(void*, const ps_result_numeric_report_v2*);
  /** @brief Create a retained exact inclusive-prefix relation for tensor slots.
   * `output_slot` and `input_slot` select the output and input Result tensors;
   * their sample shapes must be equal rank-one domains. `roles` is a nonempty
   * mask of Data, Control, and Validation roles and cannot include Descriptor.
   * On success, `relation` receives
   * a handle for use by `publish_tensor_with_relation`; release it with
   * `release_relation`. This relation handle does not grant payload-read
   * access; the callback must request input data through `need_tensor`
   * separately.
   */
  int (*make_prefix)(void*, uint32_t output_slot, uint32_t input,
                     uint32_t input_slot, uint32_t roles, uint64_t* relation);
  /** @brief Create a retained exact tensor-neighborhood relation.
   * output_slot selects the output tensor; input and input_slot select a tensor
   * from the operation's input Results. Their complete sample shapes must
   * match, have rank 1..8, and contain positive extents. radii points to rank
   * uint64 radii, one per axis; every uint64 radius is accepted. The generated
   * support targets that Tensor member with first=count=0. periodic is 0 for
   * clipped bounds or 1 for wrapping on every axis. roles is a nonempty subset
   * of Data, Control, and Validation (mask 1..7).
   *
   * The relation describes exact symmetric rectangular support, but grants no
   * read authorization; request samples separately through need_tensor.
   * On success, relation receives a handle for publish_tensor_with_relation.
   * Release it with release_relation after publication. The radii array is
   * borrowed only for this call. Call this service on the active poll's entry
   * thread; service failures are sticky for the poll.
   */
  int (*make_neighborhood)(void*, uint32_t output_slot, uint32_t input,
                           uint32_t input_slot, uint32_t roles,
                           const uint64_t* radii, uint32_t rank,
                           uint32_t periodic, uint64_t* relation);
  const ps_cpu_parallel_service_v1* cpu_parallel;
  const ps_cpu_tiles_service_v1* cpu_tiles;
  const ps_gpu_service_v1* gpu;
  /** @brief Create a relation from every output tensor sample to one input
   * tensor sample span.
   *
   * `output_slot`, `input`, and `input_slot` select the output tensor and the
   * input Result tensor. The input domain is flattened in batch-axis order
   * followed by descriptor-axis order; `[first, first + count)` must fit that
   * domain. Each sample in the complete output tensor maps to this same span.
   * `roles` is a nonempty subset of Data (1), Control (2), and Validation (4),
   * represented by a mask from 1 through 7. `guarantee` is Exact or
   * Conservative. The service validates slots and source bounds before
   * returning a relation handle. Cardinality overflow returns
   * ResourceExhausted.
   *
   * A relation records support for dependency propagation; it does not grant
   * permission to read samples. Request reads separately with `need_tensor`.
   * Use the returned handle when publishing the output relation, then release
   * it with `release_relation`. Call this on the active poll entry thread;
   * service errors remain sticky for that poll.
   */
  int (*make_tensor_cartesian)(void*, uint32_t output_slot, uint32_t input,
                               uint32_t input_slot, uint32_t roles,
                               uint64_t first, uint64_t count,
                               uint32_t guarantee, uint64_t* relation);
  /** @brief Acquire an owning window backed for native GPU access.
   * The region must be covered by a payload-readable tensor Need. A missing
   * GPU lane returns BackendUnavailable; a missing slot or descriptor-only
   * authorization is an InvalidArgument protocol failure, which is sticky.
   * Root work and any required upload/layout packing are charged. Compatible
   * same-device affine backing may be reused; fragmented sources can require
   * both materialization and upload, each counted as a transfer. The returned
   * handle uses the ordinary window retain/release lifecycle, and its window
   * retains source Result ownership beyond the callback phase.
   */
  int (*acquire_native_tensor_window)(void*, uint32_t input, uint32_t slot,
                                      const ps_result_region_v2*,
                                      ps_result_tensor_window_v2*,
                                      uint64_t* handle);
  /** @brief Pack the current Need coverage for native atlas lookup.
   * The returned buffer tokens are poll-scoped GPU bindings; repeated calls
   * for one input and slot in the poll reuse the same atlas. This does not
   * authorize source reads beyond the tensor Need.
   */
  int (*acquire_native_atlas)(void*, uint32_t input, uint32_t slot,
                              ps_result_native_atlas_v2*);
  /** @brief Copy and seal a generic one-tensor CompleteBundle state.
   * The schema must have no fields, metadata, facets, groups, batch axes or
   * spatial layout. The byte count must exactly match the tensor's samples.
   * Use this handle as a block state or pass it to checkpoint_publish. The
   * returned handle remains owned by the active Result operation and must be
   * released with release_block_state unless returned from a block callback.
   * Publishing it as a checkpoint retains a Result reference but does not
   * consume the caller's handle.
   */
  int (*create_block_state)(void*, const ps_result_schema_v2*, const uint8_t*,
                            uint64_t bytes, uint64_t* result);
  /** @brief Copy all samples from a block-state handle to caller storage. */
  int (*read_block_state)(void*, uint64_t result, uint8_t*, uint64_t bytes);
  /** @brief Release an operation-owned block-state handle. */
  int (*release_block_state)(void*, uint64_t result);
  /** @brief Run one Result block callback against an incoming state handle.
   * The callback may return the borrowed incoming handle or a newly created
   * state handle. On success, the host consumes a newly returned handle and
   * writes a distinct outgoing handle. The callback's service table is
   * borrowed until it returns; GPU tokens remain valid through the enclosing
   * poll.
   */
  int (*block)(void*, uint32_t kind, uint64_t begin, uint64_t end,
               uint64_t mode, uint64_t incoming_result,
               ps_result_block_compute_v2, void* user,
               uint64_t* outgoing_result);
  /** @brief Publish bytes from an exact scratch allocation as a tensor region.
   * After validating the envelope, region, and relation, the host moves the
   * allocation out of scratch and freezes it for publication. It remains
   * consumed if a later publication step fails. The region origin is expressed
   * in row-major sample coordinates; relation rows describe output support.
   * The former writable scratch token cannot modify the transferred bytes.
   */
  int (*publish_tensor_buffer)(void*, uint32_t slot, const ps_result_region_v2*,
                               uint8_t* scratch, uint64_t bytes,
                               const ps_result_relation_row_v2*, uint32_t rows,
                               uint32_t guarantee, uint32_t finality);
  /** @brief Run synchronous, bounded GPU discovery for more tensor Needs.
   * Capacity must be positive and is limited by 65536, `maximum_gpu_requests`,
   * and dependency `maximum_boxes`. Candidates must be positive; they bound
   * and charge every emit attempt. Discovery work is also checked against the
   * actor's remaining budget across polls and the active Run/Root budgets. The
   * callback must dispatch native GPU work.
   * After nonempty discovery, the outer callback must yield
   * `PS_RESULT_NEED_V2`; publication while discovered Needs remain pending is
   * rejected. The returned receipt handle is owned by this plugin
   * instance until released or operation destruction.
   */
  int (*discover)(void*, uint32_t capacity, uint32_t candidates,
                  ps_result_discovery_compute_v2, void* user,
                  uint64_t* receipt);
  /** @brief Read normalized receipt records or query their count.
   * Set `capacity` to zero and `output` to NULL to retrieve only `count`, then
   * provide initialized records and sufficient capacity to copy them. The
   * receipt handle remains valid across polls until `release_discovery` or
   * operation destruction.
   */
  int (*discovery_requests)(void*, uint64_t receipt,
                            ps_result_discovery_request_v2*, uint32_t capacity,
                            uint32_t* count);
  /** @brief Release a receipt owned by this plugin instance. */
  int (*release_discovery)(void*, uint64_t receipt);
  /** @brief Borrow the greatest saved checkpoint sequence <= `before`.
   * `phase` must be nonzero. Initialize the destination size and reserved
   * fields. A successful miss returns handle zero; a hit returns an opaque
   * handle, sequence, and canonical logical tensor byte size. The state and
   * witness remain owned by the host until this start/poll callback returns.
   * The handle is member-scoped in a joint poll and grants read access only
   * through checkpoint_read. Checkpoint services are unavailable during block
   * callbacks and GPU discovery.
   */
  int (*checkpoint_before)(void*, uint32_t phase, uint64_t before,
                           ps_result_checkpoint_v2*);
  /** @brief Copy a positive byte interval from a borrowed checkpoint state.
   * `offset` addresses canonical row-major logical sample bytes. The interval
   * may start or end inside a sample's element and may cross sample boundaries;
   * bounds are checked against `byte_size`. Work, capacity, and cancellation
   * are enforced by the active Run/Root. The handle is valid only in the
   * current callback epoch, and this read grants no current-input access.
   */
  int (*checkpoint_read)(void*, uint64_t handle, uint64_t offset, void*,
                         uint64_t bytes);
  /** @brief Retain a block-state Result as checkpoint state.
   * `phase` must be nonzero. `state` must identify a caller-owned generic
   * one-tensor CompleteBundle handle owned by this operation instance (and
   * member in a joint poll). If optional retention is admitted, the host keeps
   * the sealed Result and its success witness for later lookup. This call does
   * not consume the block-state handle, which the caller must still release.
   * Block-state handles remain valid across polls until release or callback
   * state destruction. Retention requires a deterministic side-effect-free
   * operation; RequestRecord, block callbacks, and GPU discovery cannot use
   * checkpoints.
   */
  int (*checkpoint_publish)(void*, uint32_t phase, uint64_t sequence,
                            uint64_t state);
} ps_result_services_v2;

/** @brief Stable key for one Result observation in a joint callback.
 * `output_index` is the declaration-order output index. `rank` is 1..8 and
 * `coordinate` contains the remaining logical observation coordinates after
 * tuple and atomic trailing axes have been closed into the observation. Batch
 * and other ungrouped sample axes remain in order. Unused coordinate slots
 * must be zero. Several keys may name one output only when their coordinates
 * differ; all keys for that output use the same tensor slot.
 */
typedef struct ps_result_atom_key_v2 {
  uint32_t output_index, rank;
  uint64_t coordinate[8];
} ps_result_atom_key_v2;
/** @brief Fixed logical observation domain used by a ValidationDomain failure.
 * `first` identifies its origin and `extent` gives the range on each key axis.
 * The host checks the complete domain against the output's fixed observation
 * domain; a callback's current member subset does not define the domain.
 */
typedef struct ps_result_atom_domain_v2 {
  ps_result_atom_key_v2 first;
  uint64_t extent[8];
} ps_result_atom_domain_v2;
typedef enum ps_result_error_code_v2 {
  PS_RESULT_ERROR_OK_V2 = 0,
  PS_RESULT_ERROR_INVALID_ARGUMENT_V2 = 1,
  PS_RESULT_ERROR_NOT_FOUND_V2 = 2,
  PS_RESULT_ERROR_CYCLE_V2 = 3,
  PS_RESULT_ERROR_TYPE_MISMATCH_V2 = 4,
  PS_RESULT_ERROR_RESOURCE_EXHAUSTED_V2 = 5,
  PS_RESULT_ERROR_CANCELLED_V2 = 6,
  PS_RESULT_ERROR_STALE_V2 = 7,
  PS_RESULT_ERROR_BACKEND_UNAVAILABLE_V2 = 8,
  PS_RESULT_ERROR_OPERATION_FAILED_V2 = 9,
  PS_RESULT_ERROR_INTERNAL_V2 = 10
} ps_result_error_code_v2;
typedef enum ps_result_failure_reason_v2 {
  PS_RESULT_REASON_NONE_V2 = 0,
  PS_RESULT_REASON_INVALID_ASSOCIATION_V2 = 1,
  PS_RESULT_REASON_ASSOCIATION_UNDERFLOW_V2 = 2,
  PS_RESULT_REASON_ARITHMETIC_OVERFLOW_V2 = 3,
  PS_RESULT_REASON_EMPTY_WEIGHTED_RESULT_V2 = 4,
  PS_RESULT_REASON_DIVIDE_BY_ZERO_V2 = 5,
  PS_RESULT_REASON_INVALID_DOMAIN_V2 = 6,
  PS_RESULT_REASON_NOT_CONVERGED_V2 = 7,
  PS_RESULT_REASON_NO_SOLUTION_V2 = 8,
  PS_RESULT_REASON_MULTIPLE_SOLUTIONS_V2 = 9,
  PS_RESULT_REASON_ILL_CONDITIONED_V2 = 10,
  PS_RESULT_REASON_MALFORMED_ENVELOPE_V2 = 11,
  PS_RESULT_REASON_UNAUTHORIZED_READ_V2 = 12,
  PS_RESULT_REASON_STALE_HANDLE_V2 = 13,
  PS_RESULT_REASON_HOST_EXCEPTION_V2 = 14,
  PS_RESULT_REASON_SHORT_IO_V2 = 15,
  PS_RESULT_REASON_WORK_LIMIT_V2 = 16,
  PS_RESULT_REASON_CAPACITY_LIMIT_V2 = 17,
  PS_RESULT_REASON_STAGE_LIMIT_V2 = 18,
  PS_RESULT_REASON_CANCELLED_V2 = 19,
  PS_RESULT_REASON_STALE_VERSION_V2 = 20,
  PS_RESULT_REASON_INVALID_QUALITY_V2 = 21
} ps_result_failure_reason_v2;
typedef enum ps_result_failure_origin_v2 {
  PS_RESULT_ORIGIN_UNSPECIFIED_V2 = 0,
  PS_RESULT_ORIGIN_SCHEMA_V2 = 1,
  PS_RESULT_ORIGIN_DOMAIN_V2 = 2,
  PS_RESULT_ORIGIN_RESOURCE_V2 = 3,
  PS_RESULT_ORIGIN_IO_V2 = 4,
  PS_RESULT_ORIGIN_BACKEND_V2 = 5,
  PS_RESULT_ORIGIN_CANCELLATION_V2 = 6,
  PS_RESULT_ORIGIN_PROTOCOL_V2 = 7
} ps_result_failure_origin_v2;
typedef enum ps_result_failure_scope_v2 {
  PS_RESULT_SCOPE_UNSPECIFIED_V2 = 0,
  PS_RESULT_SCOPE_ATOM_V2 = 1,
  PS_RESULT_SCOPE_VALIDATION_DOMAIN_V2 = 2,
  PS_RESULT_SCOPE_ASSOCIATION_V2 = 3,
  PS_RESULT_SCOPE_GROUP_V2 = 4,
  PS_RESULT_SCOPE_RUN_V2 = 5,
  PS_RESULT_SCOPE_WAITER_V2 = 6
} ps_result_failure_scope_v2;
/** @brief Typed failure detail attached to one contract-2 outcome.
 * The host copies this record before returning from the poll. `scope` must be
 * Atom, with `atom` equal to the outcome key, or ValidationDomain, with a
 * matching complete fixed domain and Domain or Schema origin. Other scopes
 * are rejected. The bounded diagnostic is at most 4096 bytes. Producer node
 * and input identities are assigned by the host and cannot be supplied here.
 */
typedef struct ps_result_atom_failure_v2 {
  uint32_t struct_size, code, reason, origin, scope, reserved;
  ps_result_atom_key_v2 atom;
  ps_result_atom_domain_v2 domain;
  uint32_t message_size;
  char message[4096];
} ps_result_atom_failure_v2;
/** @brief Query borrowed for one member of the joint start callback.
 * The record and its nested query are valid only until `start` returns. The
 * host filters cancelled members before this callback, so the active subset
 * can be smaller than the original request.
 */
typedef struct ps_result_joint_query_v2 {
  ps_result_atom_key_v2 key;
  const ps_result_query_v2* query;
} ps_result_joint_query_v2;
/** @brief Ready member borrowed for one joint poll callback.
 * The record and nested query are valid only during `poll`. `services` grants
 * the member's scoped Result reads, Needs, publication, and sticky error
 * reporting; member handles remain isolated.
 */
typedef struct ps_result_joint_member_v2 {
  ps_result_atom_key_v2 key;
  const ps_result_query_v2* query;
  const ps_result_services_v2* services;
} ps_result_joint_member_v2;
/** @brief One result returned for a borrowed joint member.
 * `key` must match exactly one input member and every input member must occur
 * once. `result` may be a Result Need or complete publication. Contract 1 may
 * also return an ordinary member error code. Contract 2 reports a typed member
 * failure with `PS_RESULT_ATOM_FAILURE_V2`; an ordinary nonzero poll callback
 * return fails the whole group. Contract 2 may attach an opaque quality handle
 * to a successful publication or a Domain-origin Atom/ValidationDomain
 * failure carrying Measured evidence. The host copies the owning report before
 * the callback lease retires. Need outcomes require a zero quality handle.
 * Contract 1 requires `failure` and `quality` to be empty.
 */
typedef struct ps_result_joint_outcome_v2 {
  uint32_t struct_size;
  ps_result_atom_key_v2 key;
  int result;
  ps_result_atom_failure_v2 failure;
  uint64_t quality;
} ps_result_joint_outcome_v2;
/** @brief Shared scratch, work, and contract-2 quality services for one poll.
 * This table is valid only on the callback entry thread and during that poll.
 * Scratch is charged against the shared workspace limit and released when the
 * callback returns. Work is charged to the active Run and Root. Service errors
 * are sticky for the shared continuation. Contract 2 can create Measured or
 * IntegerDiagonal quality reports. Their opaque handles are shared across
 * members in this callback epoch; attach a handle to a publication or a
 * Domain-origin Atom/ValidationDomain failure with Measured evidence before
 * return and release unused handles before the callback lease retires. Need
 * outcomes require a zero quality handle. Quality
 * handles are distinct from member-service handles and cannot be reused across
 * joint groups. Quality factories are unavailable to contract 1.
 */
typedef struct ps_result_joint_services_v2 {
  uint32_t struct_size, reserved;
  void* context;
  int (*scratch)(void*, uint64_t bytes, uint8_t**);
  int (*consume_work)(void*, uint64_t work);
  int (*quality_measured)(void*, const char*, uint32_t snapshot_size,
                          uint64_t dimension, double residual, uint64_t*);
  int (*quality_integer_diagonal)(void*, const char*, uint32_t snapshot_size,
                                  const int64_t* diagonal,
                                  const int64_t* estimate, const int64_t* rhs,
                                  uint64_t count, uint64_t*);
  int (*release_quality)(void*, uint64_t);
} ps_result_joint_services_v2;
/** @brief Optional CPU joint program for direct Result joint continuations.
 * Contract 1 groups distinct outputs with RequestFailureOnly delivery;
 * contract 2 groups distinct observation keys on Atomic outputs with
 * PerAtomOutcome delivery. The operation must also provide its singleton
 * start, poll, and destroy callbacks. Both contracts require CPU capability.
 * Contract 1 registers at least two outputs and accepts 2..64 distinct output
 * members. Contract 2 accepts requests with 1..64 distinct keys; multiple
 * coordinates may name the same output, which has one fixed tensor slot.
 * Cancelled members are filtered before joint start, so its borrowed query
 * subset can contain one member. Query records are valid only until start
 * returns; start receives no payload-read service.
 *
 * The host supplies one zeroed state allocation of `state_bytes`. Once start
 * is entered, destroy runs exactly once, including when start fails or throws.
 * State bytes must be nonzero and at most 1 MiB. Each poll receives the ready
 * subset (1..64 members), member-scoped Result services, and shared
 * scratch/work services. Contract 2 can also create shared Measured or
 * IntegerDiagonal quality reports and attach copied evidence to successful
 * publications or eligible ValidationDomain failures. The
 * callback returns zero for a valid round or an ordinary C error for a group
 * failure. On success, `outcome_count` must equal `count`, and the outcomes
 * must cover each input key exactly once. `workspace_bytes` bounds shared
 * scratch per poll. The program and all nested size fields must equal their
 * current complete structure sizes; unsupported contract numbers are rejected.
 */
typedef struct ps_result_joint_program_v2 {
  uint32_t struct_size, contract;
  uint64_t state_bytes, workspace_bytes;
  int (*start)(const ps_result_joint_query_v2*, uint32_t count, void* state,
               uint64_t state_bytes, void* user);
  int (*poll)(const ps_result_joint_member_v2*, uint32_t count, void* state,
              const ps_result_joint_services_v2*, ps_result_joint_outcome_v2*,
              uint32_t* outcome_count, void* user);
  void (*destroy)(void* state, void* user);
  uint32_t query_size, member_size, outcome_size, services_size;
} ps_result_joint_program_v2;

/** @brief Pure metadata sink borrowed only during resolver entry. Each output
 * must be supplied once. set_output copies the complete nested record before
 * returning, so callback-local schemas/tensors/facets are safe. Errors are
 * sticky. set_output must be called on the resolver entry thread.
 */
typedef struct ps_result_metadata_sink_v2 {
  uint32_t struct_size;
  void* context;
  int (*set_output)(void*, uint32_t, const ps_result_port_v2*);
} ps_result_metadata_sink_v2;
typedef struct ps_result_operation_v2 {
  uint32_t struct_size;
  const char* key;
  /** @brief Operation capability and retry-policy flags.
   * `PS_RESULT_FLAG_CPU_FALLBACK_V2` is an explicit opt-in and requires both
   * CPU and GPU capability flags. C `start` and `poll` callbacks both run
   * through the executor's poll-phase fallback gate. A retry requires a
   * recoverable GPU `BackendUnavailable`, deterministic and side-effect-free
   * traits at both start and poll, and a retry-safe attempt with no published
   * output, field I/O, or native effect. Sticky operation or service failures
   * are checked before the callback result. Cancellation, stale state, and
   * other errors remain terminal. Successful output publication followed by
   * `BackendUnavailable` becomes `OperationFailed`; `begin_result` alone is
   * not publication.
   * The separate C++ `OperationRegistry::start_result` entry point has its own
   * pre-continuation backend checks.
   * C import derives `cacheable` from the presence of both purity flags. A
   * non-pure Dependency-v2 Whole, non-joint Atomic operation with
   * RequestFailureOnly delivery is accepted as uncached; other non-pure
   * dependency operations are rejected.

   */
  uint32_t key_size, flags;
  const ps_result_port_v2* inputs;
  uint32_t input_count;
  const ps_result_output_v2* outputs;
  uint32_t output_count;
  const ps_result_parameter_descriptor_v2* parameters;
  uint32_t parameter_count;
  uint64_t state_bytes, workspace_bytes;
  uint32_t maximum_stages, cpu_staged_tiles;
  /** @brief 0 requires native arithmetic when selected for GPU; 1 declares
   * host-proved bitwise mapped views/copies with no arithmetic dispatch duty.
   * This is a registered semantic obligation; it cannot disguise computation.
   */
  uint32_t data_movement;
  void* user_data;
  /** @brief Optional pure metadata inference. Inputs/parameters and output
   * prototypes are borrowed for this call. Supply one record per output through
   * the synchronous sink, which copies and validates all nested pointers before
   * set_output returns. Nested data only needs to live through that call. No
   * payload, execution services or mutable shared state may be accessed; calls
   * may run concurrently. Schema id/version, output kind and the operation's
   * names/projections remain fixed. With this resolver input shape/schema
   * bodies are prototypes; input kind, dtype/rank, bounds, semantic constraints
   * and Result id/version still constrain them.
   */
  int (*resolve_metadata)(void*, const ps_result_port_v2*, uint32_t,
                          const ps_result_parameter_value_v2*, uint32_t,
                          const ps_result_port_v2*, uint32_t,
                          const ps_result_metadata_sink_v2*);
  int (*start)(void*, void*, const ps_result_query_v2*,
               const ps_result_services_v2*);
  int (*poll)(void*, void*, const ps_result_query_v2*,
              const ps_result_services_v2*);
  void (*destroy)(void*, void*);
  /** @brief Optional prototype for one repeated trailing input group.
   * `input_count` and `inputs` describe only the fixed prefix. When
   * `repeated_maximum` is nonzero, this points to one additional input
   * constraint prototype; the host applies it to each actual input after the
   * prefix during metadata resolution and start/poll query validation. The
   * prototype constrains metadata only and grants no payload access. Each
   * actual input still requires its own Tensor Need.
   */
  const ps_result_port_v2* repeated_input;
  /** @brief Minimum and maximum number of actual trailing inputs.
   * A nonzero maximum enables the group and requires
   * `1 <= repeated_minimum <= repeated_maximum` and
   * `input_count + repeated_maximum <= 1024`. With the group disabled, all
   * repeated fields, including `repeated_input` and `repeated_match`, are
   * zero/null. The fixed-prefix `input_count` is unchanged by resolution.
   * `repeated_match` is 0 or 1. When 1, each repeated Result tensor must match
   * the first repeated tensor's dtype and complete logical `sample_shape()`.
   * When 0, the operation supplies a pure metadata resolver for the expanded
   * input list.
   */
  uint32_t repeated_minimum, repeated_maximum, repeated_match;
  /** @brief Optional direct CPU joint callback program.
   * Contract 1 requires at least two Atomic outputs with RequestFailureOnly
   * delivery. Contract 2 requires Atomic outputs with PerAtomOutcome delivery
   * and adds keyed failures and quality attachment. Singleton callbacks remain
   * required. This pointer extends the ABI 2 operation record: `struct_size`
   * must equal the current full structure size; prefix-sized records are
   * rejected. Structured CPU workflow execution supports contracts 1 and 2;
   * the coordinator supplies Result Needs and dependency evidence. Contract 2
   * remains CPU-only and does not imply native GPU joint execution.
   */
  const ps_result_joint_program_v2* joint;
} ps_result_operation_v2;
/** @brief Result operation module table and its library-lifetime hook.
 * After successful import, the host retains the table owner while imported
 * definitions can reference plugin-owned declarations. It invokes `destroy`
 * when that retained table lifetime ends.
 */
typedef struct ps_result_operation_plugin_api_v2 {
  uint32_t struct_size, abi_version;
  const ps_result_operation_v2* operations;
  uint32_t operation_count;
  void* context;
  void (*destroy)(void*);
} ps_result_operation_plugin_api_v2;
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void);
#ifdef __cplusplus
}
#endif
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_OPERATION_PLUGIN_API_H_
