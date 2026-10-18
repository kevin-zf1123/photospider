/** @file result_operation_plugin_api.h
 * @brief Standalone C declarations for Result operation ABI 7.
 *
 * This header owns the Result scalar, parameter, flag, and callback
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
#define PS_RESULT_OPERATION_ABI_VERSION_7 7U
/** @brief Ordinary Result callback and service status values.
 * Values 0 through 6 represent success, failure, cancellation,
 * backend-unavailable, resource-exhausted, type-mismatch, and invalid-argument
 * outcomes. Unknown nonzero callback results fail closed. Poll callbacks may
 * also yield Need or publication.
 */
typedef enum ps_result_status_v7 {
  PS_RESULT_STATUS_OK_V7 = 0,
  PS_RESULT_STATUS_FAILURE_V7 = 1,
  PS_RESULT_STATUS_CANCELLED_V7 = 2,
  PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V7 = 3,
  PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V7 = 4,
  PS_RESULT_STATUS_TYPE_MISMATCH_V7 = 5,
  PS_RESULT_STATUS_INVALID_ARGUMENT_V7 = 6
} ps_result_status_v7;
/** @brief Operation has deterministic output for equal inputs/parameters. */
#define PS_RESULT_FLAG_DETERMINISTIC_V7 (1U << 0U)
/** @brief Operation has no externally visible side effect. */
#define PS_RESULT_FLAG_SIDE_EFFECT_FREE_V7 (1U << 1U)
/** @brief Operation can execute on CPU; at least one CPU/GPU flag is required.
 */
#define PS_RESULT_FLAG_CPU_V7 (1U << 2U)
/** @brief Operation can execute on the optional local GPU lane. */
#define PS_RESULT_FLAG_GPU_V7 (1U << 3U)
/** @brief Output-free GPU unavailability may use CPU; requires CPU and GPU
 * flags. */
#define PS_RESULT_FLAG_CPU_FALLBACK_V7 (1U << 4U)
/** @brief Opts pure deterministic Result outputs into one internal
 * block-state namespace across outputs. Regional-atomic outputs are ineligible.
 * Block keys still include all resolved outputs, backend, execution mode,
 * static parameters, input metadata, block kind/range/mode, incoming state, and
 * supplied tensor metadata and bytes. Public outputs, Needs/evidence and other
 * caches stay independent. This flag does not promise concurrent producer
 * deduplication and does not change the table layout.
 */
#define PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V7 (1U << 5U)

/** @brief Result C ABI tensor element-type codes.
 * Values match the public C++ element-type model and are declared here without
 * requiring the base operation plugin header.
 */
typedef enum ps_result_element_type_v7 {
  PS_RESULT_ELEMENT_UINT8_V7 = 1,
  PS_RESULT_ELEMENT_INT64_V7 = 2,
  PS_RESULT_ELEMENT_FLOAT64_V7 = 3,
  PS_RESULT_ELEMENT_FLOAT32_V7 = 4,
  PS_RESULT_ELEMENT_INT8_V7 = 5,
  PS_RESULT_ELEMENT_UINT16_V7 = 6,
  PS_RESULT_ELEMENT_INT16_V7 = 7
} ps_result_element_type_v7;

/** @brief Closed source-parameter type values for Result operation ABI 7. */
typedef enum ps_result_parameter_type_v7 {
  PS_RESULT_PARAMETER_INT64_V7 = 1,
  PS_RESULT_PARAMETER_FLOAT64_V7 = 2,
  PS_RESULT_PARAMETER_BOOL_V7 = 3,
  PS_RESULT_PARAMETER_STRING_V7 = 4
} ps_result_parameter_type_v7;

/**
 * @brief One immutable parameter declaration published by an operation.
 *
 * @note Key bytes and declaration records remain plugin-owned until the API
 * destroy callback.
 */
typedef struct ps_result_parameter_descriptor_v7 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Nonempty bounded UTF-8 parameter key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief One `ps_result_parameter_type_v7` value. */
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
} ps_result_parameter_descriptor_v7;

/**
 * @brief One canonical source parameter supplied to a callback.
 *
 * @note Exactly the field selected by `type` is meaningful; every pointer is
 * callback-local and must not be retained.
 */
typedef struct ps_result_parameter_value_v7 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief Nonempty schema-declared UTF-8 parameter key. */
  const char* key;
  /** @brief Exact key byte count excluding any terminator. */
  uint32_t key_size;
  /** @brief One `ps_result_parameter_type_v7` value. */
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
} ps_result_parameter_value_v7;

#define PS_RESULT_NEED_V7 100
#define PS_RESULT_PUBLISH_V7 101
#define PS_RESULT_NO_CHANNEL_V7 UINT32_MAX

typedef struct ps_result_region_v7 {
  uint32_t struct_size, rank;
  uint64_t offset[8], extent[8];
} ps_result_region_v7;
typedef struct ps_result_tensor_spec_v7 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size, element_type, rank;
  uint64_t shape[8], batch_shape[8];
  uint32_t batch_rank, atomic_trailing_axes, spatial;
  uint32_t height_axis, width_axis, channel_axis;
} ps_result_tensor_spec_v7;
/* Fixed member visibility scopes, copied during metadata preparation.
 * Policies: 1 CompleteBundle, 2 IndependentChunks, 3 StablePrefix (one member).
 * Scopes and their disjoint member slots are sorted; omitted members use 2. */
typedef struct ps_result_publication_scope_v7 {
  uint32_t struct_size, policy;
  const uint32_t* members;
  uint32_t member_count;
} ps_result_publication_scope_v7;
typedef struct ps_result_schema_v7 {
  uint32_t struct_size;
  const char* id;
  uint32_t id_size, version;
  const ps_result_tensor_spec_v7* tensors;
  uint32_t tensor_count;
  const ps_result_publication_scope_v7* publication_scopes;
  uint32_t publication_scope_count;
} ps_result_schema_v7;
/** @brief Immutable implementation identity; all strings copied on import. */
typedef struct ps_result_extension_identity_v7 {
  uint32_t struct_size, version;
  const char* key;
  uint32_t key_size;
  const char* implementation;
  uint32_t implementation_size;
} ps_result_extension_identity_v7;
typedef struct ps_result_resource_identity_v7 {
  uint64_t byte_length;
  uint8_t sha256[32];
} ps_result_resource_identity_v7;
/** @brief Bytes borrowed for the enclosing callback; no ambient resource
 * lookup. */
typedef struct ps_result_resource_view_v7 {
  ps_result_resource_identity_v7 identity;
  const uint8_t* bytes;
} ps_result_resource_view_v7;
typedef struct ps_result_port_v7 {
  uint32_t struct_size, element_type, rank;
  const ps_result_schema_v7* schema;
  uint32_t element_type_mask;
  /** @brief Optional named tensor predicate. An input prototype may omit schema
   * with a tensor member predicate. With member predicates an omitted key
   * selects the sole tensor, and multiple tensors require a key. Without member
   * predicates the complete fixed representation is selected. Outputs must
   * supply their schema.
   * dtype/rank constrain this member. Strings are copied
   * on registration and borrowed in metadata/query views.
   */
  const char* tensor_key;
  uint32_t tensor_key_size;
  const ps_result_extension_identity_v7* validators;
  uint32_t validator_count;
} ps_result_port_v7;
/** @brief Enable the logical output's cumulative new-payload capacity bound.
 * An explicit zero bound permits borrowed storage only. Unknown flag bits fail
 * import. Unique physical owners are counted once per logical output activation
 * across its tile instructions and publications; input
 * and shared-stage owners are excluded. Root allocation limits still apply.
 */
#define PS_RESULT_OUTPUT_PAYLOAD_BOUND_V7 (1U << 0U)
/** @brief Immutable named output declaration. Metadata preparation resolves
 * fixed tensor shapes and preserves names/projections/flags. Exact-size ABI 7
 * records are required; older layouts have no compatibility prefix.
 */
typedef struct ps_result_output_v7 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size;
  ps_result_port_v7 port;
  /* UINT32_MAX selects every input; zero selects none. */
  const uint32_t* input_indices;
  uint32_t input_count;
  uint32_t flags;
  uint64_t maximum_output_payload_bytes;
} ps_result_output_v7;
/** @brief Static call-shared continuation contract for one block activation.
 * Strings, projections and
 * schema are copied on registration/preparation. state_bytes excludes host
 * bridge storage. Bounds are cumulative within this stage activation;
 * re-entering its block creates a fresh activation.
 * A stage has no public output identity; start/poll select it with
 * shared_stage.
 */
typedef struct ps_result_shared_stage_v7 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size;
  const ps_result_schema_v7* schema;
  const uint32_t* input_indices;
  uint32_t input_count, backend, mandatory;
  uint64_t state_bytes, workspace_bytes, maximum_work,
      maximum_output_payload_bytes;
  uint32_t maximum_stages;
  const ps_result_extension_identity_v7* validators;
  uint32_t validator_count;
} ps_result_shared_stage_v7;
/** @brief One static member grid, independent of physical storage.
 * shape contains batch and cell axes. H/W index that full shape; rank is the
 * sentinel for a missing spatial axis. batch_axis_mask marks coordinate axes
 * with one observation per tile. All views are borrowed for one callback.
 */
typedef struct ps_result_tile_grid_v7 {
  uint32_t struct_size, rank, batch_rank, batch_axis_mask;
  uint32_t height_axis, width_axis;
  uint64_t tile_extent, tile_rows, tile_columns, tile_count;
  uint64_t shape[8];
} ps_result_tile_grid_v7;
/** @brief Compact half-open member tile intervals. A descriptor-only Need
 * has slot zero and no intervals. Payload Needs have descriptor_only zero.
 * Zero intervals request no payload, descriptor grant, or producer work. The
 * compiler validates and normalizes at most 65536 intervals into fixed grants.
 * Poll-time requests only assert subsets of those grants and never start
 * producers. Bounds are static; no Whole or dynamic End request exists. Errors
 * are sticky and no missing samples are supplied.
 */
typedef struct ps_result_tile_interval_v7 {
  uint64_t begin, end;
} ps_result_tile_interval_v7;
typedef struct ps_result_tile_need_v7 {
  uint32_t struct_size, input, tensor_slot, descriptor_only;
  const ps_result_tile_interval_v7* intervals;
  uint64_t interval_count;
} ps_result_tile_need_v7;
/** @brief Borrowed member grids in schema order, at most 16 members. */
typedef struct ps_result_tile_grids_v7 {
  uint32_t struct_size, count;
  const ps_result_tile_grid_v7* members;
} ps_result_tile_grids_v7;
/** @brief Borrowed member tile intervals in a static publication or read. */
typedef struct ps_result_tile_selection_v7 {
  uint32_t struct_size, tensor_slot;
  const ps_result_tile_interval_v7* intervals;
  uint64_t interval_count;
} ps_result_tile_selection_v7;
/** @brief Pure compilation query. All nested views expire with the callback.
 * No payload, execution service, continuation or I/O is available. A shared
 * target may span multiple members; publication contains the complete target.
 * Resource identities declare required bytes without exposing runtime data.
 */
typedef struct ps_result_static_query_v7 {
  uint32_t struct_size, output_index, backend, tensor_slot, shared_stage;
  uint64_t tile_unit, tile_extent;
  const ps_result_port_v7* inputs;
  uint32_t input_count;
  const ps_result_tile_grids_v7* input_tile_grids;
  const ps_result_output_v7* output;
  const ps_result_tile_grids_v7* output_tile_grids;
  const ps_result_shared_stage_v7* shared_stages;
  uint32_t shared_stage_count;
  const ps_result_parameter_value_v7* parameters;
  uint32_t parameter_count;
  const ps_result_tile_selection_v7* publication;
  uint32_t publication_count;
  const ps_result_resource_identity_v7* resource_ids;
  uint32_t resource_count;
} ps_result_static_query_v7;
/** @brief Synchronous, entry-thread-only mapping sink; errors are sticky.
 * Nested records are copied before each call returns. At most 65536 records
 * and 65536 intervals may be supplied across the entire mapper callback.
 * add_shared with all_members=1 requires NULL/0 and selects the complete
 * stage; all_members=0 with NULL/0 explicitly requests no stage payload.
 * Input descriptors use tensor_slot=0 and NULL/0 intervals. No sink call
 * starts execution. The caller must not retain this sink or its context.
 */
typedef struct ps_result_static_mapping_sink_v7 {
  uint32_t struct_size;
  void* context;
  int (*add_input)(void*, const ps_result_tile_need_v7*);
  int (*add_shared)(void*, uint32_t stage, uint32_t all_members,
                    const ps_result_tile_selection_v7*, uint32_t member_count);
} ps_result_static_mapping_sink_v7;
/** @brief Borrowed immutable query for a complete member tile or shared stage.
 * Tile callbacks use tensor_slot/tile_unit. Stage callbacks use shared_stage
 * and use tensor_slot/tile_unit for the current compiled target. All nested
 * declarations, schemas, grids, parameters and resource views expire with the
 * callback.
 */
typedef struct ps_result_query_v7 {
  uint32_t struct_size, output_index, backend, tensor_slot;
  /** @brief UINT32_MAX for a tile, otherwise a prepared stage ordinal. */
  uint32_t shared_stage;
  const ps_result_shared_stage_v7* shared_stages;
  uint32_t shared_stage_count;
  uint64_t tile_unit, tile_extent;
  const ps_result_port_v7* inputs;
  uint32_t input_count;
  /** @brief One immutable compiled grid entry per input, in the same order. */
  const ps_result_tile_grids_v7* input_tile_grids;
  /** @brief Resolved selected declaration; policy is fixed by registration. */
  const ps_result_output_v7* output;
  const ps_result_tile_grids_v7* output_tile_grids;
  const ps_result_parameter_value_v7* parameters;
  uint32_t parameter_count;
  const ps_result_resource_view_v7* resources;
  uint32_t resource_count;
} ps_result_query_v7;

/** @brief Checkpoint state borrowed for one C Result callback epoch.
 * Set `struct_size` to this structure size and `reserved` to zero. A miss has
 * `handle == 0`, `sequence == 0`, and `byte_size == 0`. A hit identifies the
 * greatest saved sequence not greater than the requested bound and reports
 * its canonical tensor byte size. A nonzero handle is valid only through the
 * current start or poll callback.
 */
typedef struct ps_result_checkpoint_v7 {
  uint32_t struct_size, reserved;
  uint64_t handle, sequence, byte_size;
} ps_result_checkpoint_v7;

/** @brief Captured immutable Result facts. No object identity is content.
 * Compiled descriptor or payload grants authorize facts for the input.
 * Descriptor-only grants permit no payload reads. Counts and shapes are static.
 */
typedef struct ps_result_descriptor_v7 {
  uint32_t struct_size, sealed, tensor_count;
  uint64_t object_id, revision;
} ps_result_descriptor_v7;
#define PS_RESULT_VIEW_UNAVAILABLE_V7 7
/** @brief Authorized immutable tensor samples. data points at the first
 * logical sample. Advance by signed sample_stride_bytes, including negative
 * and zero strides. bytes is the checked physical address span including the
 * final element and any stride gaps. It is not a forward memcpy length from
 * data; negative strides may place other samples before data.
 */
typedef struct ps_result_tensor_row_v7 {
  uint32_t struct_size;
  const uint8_t* data;
  uint64_t samples, bytes;
  int64_t sample_stride_bytes;
} ps_result_tensor_row_v7;
typedef struct ps_result_tensor_rectangle_v7 {
  uint32_t struct_size;
  ps_result_tensor_row_v7 row;
  uint64_t rows;
  int64_t row_stride_bytes;
} ps_result_tensor_rectangle_v7;
/** @brief Owning authorized window exported by acquire_tensor_window.
 * region includes every declared batch and cell axis. row/rectangle may run
 * concurrently on CPU workers without calling execution services; they observe
 * the captured cancellation token and record invalid access as a sticky
 * protocol failure. Borrowed data pointers remain valid until the last matching
 * window handle retires. retain_window/release_window use the current poll's
 * entry thread. Release must follow worker completion. The copied function
 * table/context stays valid until operation destroy; after the last release
 * reads fail. The window retains certified source facts, schema
 * and profiles independently of later polls, producer or context lifetime. No
 * payload is copied, cached or made writable.
 */
typedef struct ps_result_tensor_window_v7 {
  uint32_t struct_size, height_axis, width_axis, channel_axis;
  /* Source identity for a singleton Result. Zero denotes a valid compound
     window with no single Result identity; it grants neither payload reads
     nor result_descriptor access. */
  uint64_t object_id;
  ps_result_region_v7 region;
  void* context;
  int (*row)(void*, const uint64_t*, uint32_t, ps_result_tensor_row_v7*);
  int (*rectangle)(void*, const uint64_t*, uint32_t,
                   ps_result_tensor_rectangle_v7*);
} ps_result_tensor_window_v7;
typedef struct ps_result_view_axis_v7 {
  int32_t output_axis;
  uint64_t source_origin;
  int64_t step;
  uint64_t output_origin;
} ps_result_view_axis_v7;
/** @brief Explicit tensor view transform. With reshape=0, axes contains
 * source_rank anchored point maps, one for each full source axis.
 * With reshape=1, source_rank=0 and axes=NULL; logical row-major shape changes
 * require compatible signed/zero contiguous storage chunks. Borrowed for the
 * service call only; no payload copy occurs. Invalid authorization is sticky.
 */
typedef struct ps_result_tensor_transform_v7 {
  uint32_t struct_size, reshape, source_rank;
  const ps_result_view_axis_v7* axes;
} ps_result_tensor_transform_v7;
/** @brief One explicit affine piece of a zero-copy publication.
 * region uses target coordinates. transform.reshape must be zero.
 * The window handle and axis array are borrowed for the service call.
 */
typedef struct ps_result_tensor_view_piece_v7 {
  uint32_t struct_size;
  uint64_t window_handle;
  ps_result_region_v7 region;
  ps_result_tensor_transform_v7 transform;
} ps_result_tensor_view_piece_v7;
/** @brief Native view of one tensor atlas authorized by a read grant.
 * shape and tile_shape describe the full tensor sample domain; payload and
 * directory sizes are byte counts for the packed atlas. The two tokens name
 * GPU buffers created for the active poll and are valid only for that poll.
 * Repeated acquisition of the same input and slot in one poll returns the
 * same atlas and tokens while neither token has been released. If either
 * token is released, a later acquisition retires the other token from that
 * pair and returns a fresh pair; neither old token becomes valid again. This
 * record grants no additional input reads.
 */
typedef struct ps_result_native_atlas_v7 {
  uint32_t struct_size, rank, element_type, reserved;
  uint64_t shape[8], tile_shape[8];
  uint64_t slot_count, payload_sample_bytes;
  uint64_t payload_byte_size, directory_byte_size;
  uint64_t payload_token, directory_token;
} ps_result_native_atlas_v7;
/** @brief Services available to one Result block compute callback.
 * The incoming handle names an immutable generic single-tensor
 * sealed Result. The callback may read its state and grant-authorized
 * input samples, then return the incoming handle or a newly created state
 * handle. The callback cannot issue another Need. A returned new handle is
 * consumed by the host; the incoming handle remains borrowed. The service
 * table is borrowed until the callback returns, while atlas tokens remain
 * valid until the enclosing poll ends. Scratch and GPU access follow their
 * normal poll ownership. Block callbacks cannot start another block or
 * publish operation outputs.
 */
typedef struct ps_result_block_services_v7 {
  uint32_t struct_size;
  void* context;
  int (*read_tensor)(void*, uint32_t input, uint32_t slot, const uint64_t*,
                     uint32_t rank, void*, uint64_t bytes);
  int (*create_state)(void*, const ps_result_schema_v7*, const uint8_t*,
                      uint64_t bytes, uint64_t* result);
  int (*read_state)(void*, uint64_t result, uint8_t*, uint64_t bytes);
  int (*release_state)(void*, uint64_t result);
  int (*acquire_native_atlas)(void*, uint32_t input, uint32_t slot,
                              ps_result_native_atlas_v7*);
  int (*allocate_scratch)(void*, uint64_t bytes, uint8_t**);
  int (*release_scratch)(void*, uint8_t*);
  int (*consume_work)(void*, uint64_t units);
  int (*cancelled)(void*);
  const ps_gpu_service_v1* gpu;
} ps_result_block_services_v7;
typedef int (*ps_result_block_compute_v7)(const ps_result_block_services_v7*,
                                          uint64_t incoming_result,
                                          uint64_t* outgoing_result, void*);
/** @brief GPU discovery record kinds; Descriptor requires zero slot and bounds.
 */
#define PS_RESULT_DISCOVERY_TILE_V7 0U
#define PS_RESULT_DISCOVERY_DESCRIPTOR_V7 1U
#define PS_RESULT_DISCOVERY_HEADER_BYTES_V7 16U
#define PS_RESULT_DISCOVERY_RECORD_BYTES_V7 32U
/** @brief One normalized input request copied from a GPU discovery receipt.
 * Tile records identify a half-open interval in one compiled member grid.
 * Descriptor records have zero slot and bounds. Overlapping and adjacent
 * intervals of the same input/member are normalized; payload requests include
 * descriptor observation. No source payload is retained by this receipt.
 */
typedef struct ps_result_discovery_request_v7 {
  uint32_t input, kind, tensor_slot, reserved;
  uint64_t begin, end;
} ps_result_discovery_request_v7;
/** @brief Restricted services borrowed during one discovery compute callback.
 * Reads and atlas acquisition are limited to already supplied tensor grants.
 * Scratch, work, cancellation, and synchronous native GPU services are also
 * available. The table pointer and service context expire when the callback
 * returns. The callback cannot request Needs, publish outputs, call block, or
 * recursively start discovery; every failed service is sticky.
 */
typedef struct ps_result_discovery_services_v7 {
  uint32_t struct_size;
  void* context;
  int (*read_tensor)(void*, uint32_t input, uint32_t slot, const uint64_t*,
                     uint32_t rank, void*, uint64_t bytes);
  int (*acquire_native_atlas)(void*, uint32_t input, uint32_t slot,
                              ps_result_native_atlas_v7*);
  int (*allocate_scratch)(void*, uint64_t bytes, uint8_t**);
  int (*release_scratch)(void*, uint8_t*);
  int (*consume_work)(void*, uint64_t units);
  int (*cancelled)(void*);
  const ps_gpu_service_v1* gpu;
} ps_result_discovery_services_v7;
/** @brief Synchronous callback that emits a bounded GPU request table.
 * Return an ordinary Result operation status; this callback does not return
 * `PS_RESULT_NEED_V7`. Submit actual native GPU work before returning success.
 * The table and service pointers are borrowed only for this call.
 */
typedef int (*ps_result_discovery_compute_v7)(
    const ps_result_discovery_services_v7*, uint8_t* table, uint64_t byte_size,
    uint32_t capacity, void* user);
/* Services and query are borrowed for one start/poll. Tensor reads copy samples
 * from compiled read grants; owning windows expose zero-copy runs.
 * Retained handles preserve that coverage until release/destroy. Every failed
 * service is sticky. Tensor sample copies use Root-accounted memory. Entry
 * services use the callback entry thread. cancelled is also callable from CPU
 * range or tile workers, and those workers may write their admitted scratch
 * bytes. Owning window row/rectangle access may run concurrently on CPU workers
 * without reentering execution services.
 */
typedef struct ps_result_services_v7 {
  uint32_t struct_size, abi_version;
  void* context;
  int (*need_tiles)(void*, const ps_result_tile_need_v7*);
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
  int (*publish_tensor)(void*, uint32_t slot, const ps_result_region_v7*,
                        const uint8_t*, uint64_t bytes);
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
  /** @brief Read the captured descriptor for a statically granted Result.
   * The Plan must grant descriptor or payload access for `input`; the grant is
   * available before the first poll. Descriptor-only grants permit no payload
   * read. Without a compiled grant this
   * returns sticky `InvalidArgument` with `UnauthorizedRead` and Protocol/Group
   * failure detail.
   */
  int (*result_descriptor)(void*, uint32_t input, ps_result_descriptor_v7*);
  /** @brief Acquire an exact rectangle inside this phase's compiled grant.
   * Handles survive polls and remain valid until release or operation destroy.
   */
  int (*acquire_tensor_window)(void*, uint32_t input, uint32_t slot,
                               const ps_result_region_v7*,
                               ps_result_tensor_window_v7*, uint64_t* handle);
  int (*acquire_retained_window)(void*, uint64_t image_handle,
                                 const ps_result_region_v7*,
                                 ps_result_tensor_window_v7*, uint64_t* handle);
  int (*retain_window)(void*, uint64_t handle, uint64_t* retained_handle);
  int (*release_window)(void*, uint64_t handle);
  /** @brief Publish one authorized source window without copying payload.
   * transform must be non-NULL and explicit, including for identity maps.
   * Signed/zero strides and compatible single-source reshape are supported.
   * VIEW_UNAVAILABLE leaves the builder usable for explicit Auto copy; all
   * other service failures are sticky. Overlap and source cycles fail.
   * Publication retains the source after its window handle is released.
   */
  int (*publish_tensor_view)(void*, uint32_t slot, const ps_result_region_v7*,
                             uint64_t window_handle,
                             const ps_result_tensor_transform_v7* transform);
  /** @brief Publish 1..16 explicit affine pieces in one transaction.
   * Nonempty, disjoint piece regions must cover the total region exactly.
   * Atomic publication constraints apply to the total region. All logical
   * mappings and source permissions are validated before physical fallback.
   * Sources may belong to distinct Roots; their payload is not copied or
   * charged to the target Root. The target retains all source owners on
   * success and advances coverage/revision once. Failure preserves prior
   * coverage and captured prefixes; only VIEW_UNAVAILABLE is nonsticky.
   * Every envelope requires its exact struct_size. No implicit concatenation
   * or identity transform is inferred.
   */
  int (*publish_tensor_view_pieces)(void*, uint32_t slot,
                                    const ps_result_region_v7*,
                                    const ps_result_tensor_view_piece_v7*,
                                    uint32_t count);
  /** @brief Borrowed bounded bytes for a declared diagnostic type ordinal. */
  int (*report_diagnostic)(void*, uint32_t type_index, const uint8_t* bytes,
                           uint32_t size);
  const ps_cpu_parallel_service_v2* cpu_parallel;
  const ps_cpu_tiles_service_v2* cpu_tiles;
  const ps_gpu_service_v1* gpu;
  /** @brief Acquire an owning window backed for native GPU access.
   * The region must be covered by a compiled payload read grant. A missing
   * GPU lane returns BackendUnavailable; a missing slot or descriptor-only
   * authorization is an InvalidArgument protocol failure, which is sticky.
   * Root work and any required upload/layout packing are charged. Compatible
   * same-device affine backing may be reused; fragmented sources can require
   * both materialization and upload, each counted as a transfer. The returned
   * handle uses the ordinary window retain/release lifecycle, and its window
   * retains source Result ownership beyond the callback phase.
   */
  int (*acquire_native_tensor_window)(void*, uint32_t input, uint32_t slot,
                                      const ps_result_region_v7*,
                                      ps_result_tensor_window_v7*,
                                      uint64_t* handle);
  /** @brief Pack the current payload grant for native atlas lookup.
   * The returned buffer tokens are poll-scoped GPU bindings; repeated calls
   * for one input and slot in the poll reuse the same atlas. This does not
   * authorize source reads beyond the compiled grant.
   */
  int (*acquire_native_atlas)(void*, uint32_t input, uint32_t slot,
                              ps_result_native_atlas_v7*);
  /** @brief Copy and seal a generic one-tensor state.
   * The schema must have no batch axes or spatial layout. The byte count must
   * exactly match the tensor's samples. Use this handle as a block state or
   * pass it to checkpoint_publish. The returned handle remains owned by the
   * active Result operation and must be released with release_block_state
   * unless returned from a block callback. Publishing it as a checkpoint
   * retains a Result reference but does not consume the caller's handle.
   */
  int (*create_block_state)(void*, const ps_result_schema_v7*, const uint8_t*,
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
               ps_result_block_compute_v7, void* user,
               uint64_t* outgoing_result);
  /** @brief Publish bytes from an exact scratch allocation as a tensor region.
   * After validating the envelope and region, the host moves the
   * allocation out of scratch and freezes it for publication. It remains
   * consumed if a later publication step fails. The region origin is expressed
   * in row-major sample coordinates.
   * The former writable scratch token cannot modify the transferred bytes.
   */
  int (*publish_tensor_buffer)(void*, uint32_t slot, const ps_result_region_v7*,
                               uint8_t* scratch, uint64_t bytes);
  /** @brief Run synchronous GPU selection within compiled input grants.
   * Capacity must be positive and is limited by 65536, `maximum_gpu_requests`,
   * and dependency `maximum_boxes`. Candidates must be positive; they bound
   * and charge every emit attempt. Discovery work is also checked against the
   * unit's remaining budget across polls and the active Run/Root budgets. The
   * callback must dispatch native GPU work.
   * Every returned interval must lie within the instruction's compiled grants.
   * All selected inputs are already ready, so the callback may compute and
   * publish in the same poll. Receipts do not enlarge read authorization.
   * The returned receipt handle is owned by this plugin
   * instance until released or operation destruction.
   */
  int (*discover)(void*, uint32_t capacity, uint32_t candidates,
                  ps_result_discovery_compute_v7, void* user,
                  uint64_t* receipt);
  /** @brief Read normalized receipt records or query their count.
   * Set `capacity` to zero and `output` to NULL to retrieve only `count`, then
   * provide initialized records and sufficient capacity to copy them. The
   * receipt handle remains valid across polls until `release_discovery` or
   * operation destruction.
   */
  int (*discovery_requests)(void*, uint64_t receipt,
                            ps_result_discovery_request_v7*, uint32_t capacity,
                            uint32_t* count);
  /** @brief Release a receipt owned by this plugin instance. */
  int (*release_discovery)(void*, uint64_t receipt);
  /** @brief Borrow the greatest saved checkpoint sequence <= `before`.
   * `phase` must be nonzero. Initialize the destination size and reserved
   * fields. A successful miss returns handle zero; a hit returns an opaque
   * handle, sequence, and canonical logical tensor byte size. The state and
   * witness remain owned by the host until this start/poll callback returns.
   * The handle grants read access only
   * through checkpoint_read. Checkpoint services are unavailable during block
   * callbacks and GPU discovery.
   */
  int (*checkpoint_before)(void*, uint32_t phase, uint64_t before,
                           ps_result_checkpoint_v7*);
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
   * one-tensor sealed Result handle owned by this operation instance. If
   * optional retention is admitted, the host keeps the sealed Result and its
   * success witness for later lookup. This call does not consume the
   * block-state handle, which the caller must still release. Block-state
   * handles remain valid across polls until release or callback state
   * destruction. Retention requires a deterministic side-effect-free operation;
   * Block callbacks and GPU discovery cannot use checkpoints.
   */
  int (*checkpoint_publish)(void*, uint32_t phase, uint64_t sequence,
                            uint64_t state);
  /** @brief Record the first operation domain failure; code must be nonzero. */
  int (*fail_domain)(void*, int status_code, uint64_t domain_code);
  /** @brief Assert complete-stage coverage within the compiled shared grants.
   * This never starts a producer. Repeated or unknown ordinals fail.
   */
  int (*need_shared_stage)(void*, uint32_t stage);
  /** @brief Acquire a retained window from an explicitly supplied stage Result.
   * This grants no access to the stage's original inputs. Window handles use
   * the same release/retain lifetime as ordinary authorized tensor windows.
   */
  int (*acquire_shared_window)(void*, uint32_t stage, uint32_t slot,
                               const ps_result_region_v7*,
                               ps_result_tensor_window_v7*, uint64_t* handle);
  /** @brief Assert member intervals within the compiled shared grants.
   * Repeat for distinct members. Empty intervals are a no-op. Assertions never
   * start a stage or add work. Cannot be combined with need_shared_stage for
   * the same stage.
   */
  int (*need_shared_tiles)(void*, uint32_t stage, uint32_t slot,
                           const ps_result_tile_interval_v7*, uint32_t count);
} ps_result_services_v7;

/** @brief Borrowed static validation input. All resource identities are
 * explicit. */
typedef struct ps_result_validation_preparation_v7 {
  uint32_t struct_size, output, port, shared_stage;
  const ps_result_schema_v7* schema;
  const ps_result_parameter_value_v7* parameters;
  uint32_t parameter_count;
  const ps_result_resource_view_v7* resources;
  uint32_t resource_count;
} ps_result_validation_preparation_v7;
/** @brief Entry-thread-only authorized reads, valid until validate returns.
 * Reads copy into caller buffers in logical order, bounded to 65536 bytes.
 * Member regions describe newly authorized coverage only.
 * No method requests computation. A failed call remains sticky even if ignored.
 */
typedef struct ps_result_validation_context_v7 {
  uint32_t struct_size, complete;
  const ps_result_schema_v7* schema;
  void* context;
  int (*consume_work)(void*, uint64_t);
  int (*cancelled)(void*);
  int (*tensor_regions)(void*, uint32_t, ps_result_region_v7*, uint32_t,
                        uint32_t*);
  int (*read_tensor)(void*, uint32_t, const ps_result_region_v7*, uint8_t*,
                     uint32_t);
  /** Record the first domain error; later callback failures cannot replace it.
   */
  int (*fail_domain)(void*, uint32_t status_code, uint64_t domain_code);
} ps_result_validation_context_v7;
/** @brief Reentrant callbacks. Prepared state owns its storage; destroy runs
 * before the module owner retires, including after failed prepare.
 */
typedef struct ps_result_validation_definition_v7 {
  uint32_t struct_size;
  ps_result_extension_identity_v7 identity;
  uint64_t scratch_bytes;
  uint32_t requires_complete;
  void* user_data;
  int (*prepare)(void*, const ps_result_validation_preparation_v7*, void**);
  int (*validate)(void*, const void*, const ps_result_validation_context_v7*);
  void (*destroy_prepared)(void*, void*);
} ps_result_validation_definition_v7;
/** @brief Fixed-size opaque report. Failed merge never changes accumulated
 * data. */
typedef struct ps_result_diagnostic_definition_v7 {
  uint32_t struct_size;
  ps_result_extension_identity_v7 identity;
  uint32_t payload_bytes;
  uint64_t scratch_bytes;
  void* user_data;
  int (*validate)(void*, const uint8_t*, uint32_t);
  int (*merge)(void*, const uint8_t*, const uint8_t*, uint8_t*, uint32_t);
} ps_result_diagnostic_definition_v7;
/** @brief Pure metadata sink borrowed only during resolver entry. Each output
 * must be supplied once. set_output copies the complete nested record before
 * returning, so callback-local schemas/tensors are safe. Errors are
 * sticky. set_output must be called on the resolver entry thread.
 */
typedef struct ps_result_metadata_sink_v7 {
  uint32_t struct_size;
  void* context;
  int (*set_output)(void*, uint32_t, const ps_result_port_v7*);
  int (*set_resources)(void*, const ps_result_resource_identity_v7*, uint32_t);
  /** @brief Replace all stage prototypes once with owned static declarations.
   */
  int (*set_shared_stages)(void*, const ps_result_shared_stage_v7*, uint32_t);
} ps_result_metadata_sink_v7;
typedef struct ps_result_operation_v7 {
  uint32_t struct_size;
  const char* key;
  /** @brief Operation capability and retry-policy flags.
   * `PS_RESULT_FLAG_CPU_FALLBACK_V7` is an explicit opt-in and requires both
   * CPU and GPU capability flags. C `start` and `poll` callbacks both run
   * through the executor's poll-phase fallback gate. A retry requires a
   * recoverable GPU `BackendUnavailable`, deterministic and side-effect-free
   * traits at both start and poll, and a retry-safe attempt with no published
   * output, tensor I/O, or native effect. Sticky operation or service failures
   * are checked before the callback result. Cancellation, stale state, and
   * other errors remain terminal. Successful output publication followed by
   * `BackendUnavailable` becomes `OperationFailed`; `begin_result` alone is
   * not publication.
   * The separate C++ `OperationRegistry::start_result` entry point has its own
   * pre-continuation backend checks.
   * C import derives `cacheable` from the presence of both purity flags. A
   * non-pure operation is uncached and declares at least one mandatory
   * shared stage. Stage computation is never retried after factory entry.
   */
  uint32_t key_size, flags;
  const ps_result_port_v7* inputs;
  uint32_t input_count;
  const ps_result_output_v7* outputs;
  uint32_t output_count;
  const ps_result_parameter_descriptor_v7* parameters;
  uint32_t parameter_count;
  uint64_t state_bytes, workspace_bytes;
  uint32_t maximum_stages, cpu_staged_tiles;
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
  int (*resolve_metadata)(void*, const ps_result_port_v7*, uint32_t,
                          const ps_result_parameter_value_v7*, uint32_t,
                          const ps_result_port_v7*, uint32_t,
                          const ps_result_metadata_sink_v7*);
  int (*start)(void*, void*, const ps_result_query_v7*,
               const ps_result_services_v7*);
  int (*poll)(void*, void*, const ps_result_query_v7*,
              const ps_result_services_v7*);
  void (*destroy)(void*, void*);
  /** @brief Optional prototype for one repeated trailing input group.
   * `input_count` and `inputs` describe only the fixed prefix. When
   * `repeated_maximum` is nonzero, this points to one additional input
   * constraint prototype; the host applies it to each actual input after the
   * prefix during metadata resolution and start/poll query validation. The
   * prototype constrains metadata only and grants no payload access. Each
   * actual input still requires its own compiled tensor grant.
   */
  const ps_result_port_v7* repeated_input;
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
  const ps_result_extension_identity_v7* diagnostics;
  uint32_t diagnostic_count;
  const ps_result_shared_stage_v7* shared_stages;
  uint32_t shared_stage_count;
  /** @brief Optional pure mapping hook, called during physical compilation.
   * NULL selects complete projected original inputs. Shared edges must be
   * declared explicitly. A successful non-NULL hook with no sink calls declares
   * no reads. Imports and execution use saved mappings without this callback.
   * Calls may run concurrently; operation state and external I/O are forbidden.
   */
  int (*map_dependencies)(void*, const ps_result_static_query_v7*,
                          const ps_result_static_mapping_sink_v7*);
} ps_result_operation_v7;
/** @brief Result operation module table and its library-lifetime hook.
 * After successful import, the host retains the table owner while imported
 * definitions can reference plugin-owned declarations. It invokes `destroy`
 * when that retained table lifetime ends.
 */
typedef struct ps_result_operation_plugin_api_v7 {
  uint32_t struct_size, abi_version;
  const ps_result_operation_v7* operations;
  uint32_t operation_count;
  void* context;
  void (*destroy)(void*);
  const ps_result_validation_definition_v7* validators;
  uint32_t validator_count;
  const ps_result_diagnostic_definition_v7* diagnostics;
  uint32_t diagnostic_count;
} ps_result_operation_plugin_api_v7;
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v7*
ps_result_operation_plugin_get_api_v7(void);
#ifdef __cplusplus
}
#endif
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_OPERATION_PLUGIN_API_H_
