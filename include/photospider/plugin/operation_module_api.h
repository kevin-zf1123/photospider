#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_MODULE_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_MODULE_API_H_

#include <stdint.h>

#include "photospider/plugin/fixed_invocation_api.h"

#if defined(_WIN32) && defined(PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
#define PS_OPERATION_MODULE_EXPORT __declspec(dllexport)
#elif !defined(_WIN32)
#define PS_OPERATION_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define PS_OPERATION_MODULE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PS_OPERATION_MODULE_ABI_VERSION_8 8U
#define PS_OPERATION_DETERMINISTIC_V8 1U
#define PS_OPERATION_PURE_V8 2U
#define PS_OPERATION_CPU_V8 4U
#define PS_OPERATION_GPU_V8 8U
#define PS_BACKEND_CPU_V8 0U
#define PS_BACKEND_GPU_V8 1U
#define PS_ID_NONE_V8 UINT32_MAX
#define PS_PARAMETER_INT64_V8 1U
#define PS_PARAMETER_FLOAT64_V8 2U
#define PS_PARAMETER_BOOL_V8 3U
#define PS_PARAMETER_STRING_V8 4U

/** @brief Port dtype sets. Combine named bits with bitwise OR.
 * Each bit is 1u << (ps_element_type_v8 code - 1). Zero adds no dtype
 * constraint. Bits outside PS_ELEMENT_MASK_ALL_V8 are invalid.
 */
#define PS_ELEMENT_MASK_UINT8_V8 UINT32_C(0x01)
#define PS_ELEMENT_MASK_INT64_V8 UINT32_C(0x02)
#define PS_ELEMENT_MASK_FLOAT64_V8 UINT32_C(0x04)
#define PS_ELEMENT_MASK_FLOAT32_V8 UINT32_C(0x08)
#define PS_ELEMENT_MASK_INT8_V8 UINT32_C(0x10)
#define PS_ELEMENT_MASK_UINT16_V8 UINT32_C(0x20)
#define PS_ELEMENT_MASK_INT16_V8 UINT32_C(0x40)
#define PS_ELEMENT_MASK_ALL_V8 UINT32_C(0x7f)

/** @brief Stable schema publication metadata, carried in uint32_t fields.
 * COMPLETE_BUNDLE groups full member coverage; INDEPENDENT_CHUNKS describes
 * independent complete tiles; STABLE_PREFIX describes a single member's tiles
 * in local index order. Fixed execution completes whole tasks and publishes
 * Results at call retirement or RET, irrespective of this schema metadata.
 */
typedef enum ps_schema_policy_v8 {
  PS_SCHEMA_COMPLETE_BUNDLE_V8 = 1,
  PS_SCHEMA_INDEPENDENT_CHUNKS_V8 = 2,
  PS_SCHEMA_STABLE_PREFIX_V8 = 3
} ps_schema_policy_v8;

#define PS_REPEATED_MATCH_NONE_V8 0U
#define PS_REPEATED_MATCH_DTYPE_SHAPE_V8 1U

/** @brief Borrowed byte string. Empty strings use {NULL,0}. Keys are UTF-8;
 * parameter values are arbitrary bytes. No terminating NUL is required.
 * Query records, strings and nested metadata/view arrays expire at callback
 * return. Kernel preparation may retain its static_state pointer and immutable
 * resource data: the host keeps their owners through kernel release.
 * All struct_size fields require the exact sizeof of their named ABI record.
 * Shorter prefixes and larger records are rejected. Changing record layouts,
 * signatures or existing field semantics requires a new ABI version.
 * Arrays have a nonnull aligned pointer exactly when count is nonzero.
 */
typedef struct ps_string_v8 {
  const char* data;
  uint32_t size;
} ps_string_v8;

/** @brief Static tensor member. element_type uses ps_element_type_v8.
 * rank is cell rank; batch_rank + rank <= 8. channel_axis=UINT32_MAX omits it.
 * Only declared axes are used; spatial is 0/1. Shape and batch extents are
 * positive. Atomic trailing axes remain whole in each fixed tile.
 */
typedef struct ps_tensor_spec_v8 {
  uint32_t struct_size;
  ps_string_v8 key;
  uint32_t element_type, rank, batch_rank, atomic_trailing_axes;
  uint64_t shape[8], batch_shape[8];
  uint32_t spatial, height_axis, width_axis, channel_axis;
} ps_tensor_spec_v8;

/** @brief Result schema scope; policy uses ps_schema_policy_v8.
 * Members and scopes are sorted, disjoint and bounded by 16. STABLE_PREFIX
 * lists one member; unlisted members use INDEPENDENT_CHUNKS.
 */
typedef struct ps_schema_scope_v8 {
  uint32_t policy, member_count;
  const uint32_t* members;
} ps_schema_scope_v8;
typedef struct ps_schema_v8 {
  uint32_t struct_size;
  ps_string_v8 id;
  uint32_t version, tensor_count;
  const ps_tensor_spec_v8* tensors;
  uint32_t scope_count;
  const ps_schema_scope_v8* scopes;
} ps_schema_v8;

/** @brief Static port predicates. An empty schema id accepts a metadata-derived
 * identity. Zero dtype/rank/mask adds no corresponding constraint. A nonempty
 * tensor_key selects a named member; otherwise a tensor predicate requires a
 * sole member. element_type uses ps_element_type_v8; a nonzero element_type
 * and a nonzero element_type_mask are mutually exclusive. Masks combine
 * PS_ELEMENT_MASK_*_V8 bits. No sample validator runs in fixed execution.
 */
typedef struct ps_port_contract_v8 {
  uint32_t struct_size;
  ps_string_v8 schema_id, tensor_key;
  uint32_t schema_version, element_type, rank, element_type_mask;
} ps_port_contract_v8;
/** @brief Scalar parameter declaration. type uses PS_PARAMETER_*_V8.
 * required and bounded are 0/1. Numeric bounds are inclusive; bounded Int64
 * endpoints are exact integers in +/-(2^53-1). Bool/String are unbounded.
 */
typedef struct ps_parameter_spec_v8 {
  uint32_t struct_size;
  ps_string_v8 key;
  uint32_t type, required, bounded;
  double minimum, maximum;
} ps_parameter_spec_v8;
/** @brief Typed scalar value. type selects the corresponding value field;
 * bool_value is 0/1. All other value fields are zero-initialized by the host.
 */
typedef struct ps_parameter_value_v8 {
  ps_string_v8 key;
  uint32_t type;
  int64_t int64_value;
  double float64_value;
  uint32_t bool_value;
  ps_string_v8 string_value;
} ps_parameter_value_v8;

/** @brief Fixed output prototype with optional input projection.
 * input_count=UINT32_MAX with NULL indices means all declared inputs;
 * zero means schema-only. Static preparation may narrow the projection.
 */
typedef struct ps_operation_output_v8 {
  uint32_t struct_size;
  ps_string_v8 key;
  ps_port_contract_v8 contract;
  const ps_schema_v8* prototype;
  uint32_t input_count;
  const uint32_t* input_indices;
} ps_operation_output_v8;
typedef struct ps_resource_id_v8 {
  uint64_t byte_length;
  uint8_t sha256[32];
} ps_resource_id_v8;
/** @brief Callback-local resource descriptor. data points to byte_length
 * immutable bytes. A prepared kernel may borrow data until its release
 * callback returns; the host retains the selected ResourceSet for that period.
 */
typedef struct ps_resource_view_v8 {
  ps_resource_id_v8 identity;
  const void* data;
} ps_resource_view_v8;

/** @brief Owning immutable operation state. The host calls release exactly
 * once after all prepared kernels release, including preparation failure.
 * A nonnull context requires release. release must not throw. Static state
 * may be shared concurrently and must own any retained callback input data.
 */
typedef struct ps_operation_state_v8 {
  const void* context;
  void (*release)(const void* context);
} ps_operation_state_v8;
typedef struct ps_static_query_v8 {
  uint32_t struct_size, input_count, parameter_count;
  const ps_schema_v8* inputs;
  const ps_parameter_value_v8* parameters;
} ps_static_query_v8;

/** @brief Synchronous preparation services. Copy inputs before returning.
 * Each output may be set once; omitted outputs retain their prototypes.
 * UINT32_MAX projection preserves the registered projection. require_resource
 * declares immutable bytes for validation/linking; no payload is available.
 * Services retain their first failure even if the callback ignores it.
 * Invoke services only on the callback thread and never retain this table.
 */
typedef struct ps_preparation_sink_v8 {
  uint32_t struct_size;
  void* context;
  ps_kernel_status_v8 (*set_output)(void*, uint32_t, const ps_schema_v8*,
                                    const uint32_t*, uint32_t);
  ps_kernel_status_v8 (*require_resource)(void*, const ps_resource_id_v8*);
} ps_preparation_sink_v8;

typedef struct ps_tile_request_v8 {
  uint32_t output, member;
  uint64_t begin, end;
} ps_tile_request_v8;
typedef struct ps_tile_grid_v8 {
  uint32_t port, member, rank, batch_axis_mask, height_axis, width_axis;
  uint64_t shape[8], tile_count, batch_count, rows, columns;
} ps_tile_grid_v8;
typedef struct ps_placement_v8 {
  uint32_t backend, domain;
  ps_string_v8 profile, queue;
} ps_placement_v8;
typedef struct ps_lowering_query_v8 {
  uint32_t struct_size;
  ps_static_query_v8 facts;
  const void* static_state;
  const ps_schema_v8* outputs;
  uint32_t output_count, backend;
  uint64_t tile_extent, maximum_records, maximum_edges, maximum_work;
  const ps_tile_request_v8* requests;
  uint32_t request_count;
  const ps_tile_grid_v8* input_grids;
  uint32_t input_grid_count;
  const ps_tile_grid_v8* output_grids;
  uint32_t output_grid_count;
} ps_lowering_query_v8;

/** @brief Callback-local checked builder. IDs name this call's private tables.
 * Every function returns a bounded status; the first failure rejects the whole
 * lowering even if ignored. All methods are synchronous on the callback thread.
 * add_task takes Host/Device/Shared scratch bytes and effect=0/1. An effectful
 * call emits one terminal effect. add_copy needs one matching read and write.
 * For GPU COPY, finalization declares one output-sized Shared scratch segment
 * per Host endpoint. Preparation consumes this serialized staging bound.
 * set_parameters copies bytes, at most maximum_work in aggregate per lowering.
 * Outputs/private tiles are full fixed-grid tiles with one exclusive producer.
 */
typedef struct ps_operation_builder_v8 {
  uint32_t struct_size;
  void* context;
  ps_kernel_status_v8 (*add_task)(void*, ps_string_v8, ps_string_v8,
                                  const ps_placement_v8*,
                                  const uint64_t scratch[3], uint32_t,
                                  uint32_t*);
  ps_kernel_status_v8 (*add_copy)(void*, const ps_placement_v8*, uint32_t*);
  ps_kernel_status_v8 (*add_temporary)(void*, const ps_schema_v8*, uint32_t*);
  ps_kernel_status_v8 (*read_input)(void*, uint32_t, uint32_t, uint32_t,
                                    uint64_t, uint32_t);
  ps_kernel_status_v8 (*read_input_schema)(void*, uint32_t, uint32_t);
  ps_kernel_status_v8 (*write_output)(void*, uint32_t, uint32_t, uint32_t,
                                      uint64_t);
  ps_kernel_status_v8 (*write_temporary)(void*, uint32_t, uint32_t, uint32_t,
                                         uint64_t, uint32_t*);
  ps_kernel_status_v8 (*read_tile)(void*, uint32_t, uint32_t, uint32_t);
  ps_kernel_status_v8 (*depends_on)(void*, uint32_t, uint32_t);
  ps_kernel_status_v8 (*set_parameters)(void*, uint32_t, const void*, uint64_t);
} ps_operation_builder_v8;

/** @brief Compiled task view. byte_offset is relative to the compiled tile.
 * bytes bounds that tile. A Device input binding may add a checked allocation
 * base to byte_offset and expose a larger capacity, preserving layout and its
 * declared samples. Output and scratch allocations follow the compiled bounds.
 * port=UINT32_MAX denotes private writes. Reads keep their declared port.
 */
typedef struct ps_kernel_view_spec_v8 {
  uint32_t port, member, domain;
  uint64_t tile_index, bytes, byte_offset;
  ps_kernel_layout_v8 layout;
} ps_kernel_view_spec_v8;
struct ps_native_gpu_service_v2;
typedef struct ps_kernel_preparation_v8 {
  uint32_t struct_size;
  ps_static_query_v8 facts;
  const void* static_state;
  ps_string_v8 symbol, contract;
  ps_placement_v8 placement;
  uint64_t tile_extent;
  const ps_schema_v8* outputs;
  uint32_t output_count;
  const ps_kernel_view_spec_v8* reads;
  uint32_t read_count;
  const ps_kernel_view_spec_v8* writes;
  uint32_t write_count;
  const void* parameters;
  uint64_t parameter_bytes, scratch[3];
  const ps_resource_view_v8* resources;
  uint32_t resource_count;
  const struct ps_native_gpu_service_v2* native_gpu;
} ps_kernel_preparation_v8;

/** @brief Immutable module-owned declaration, valid until module destroy.
 * 0..1024 inputs, 1..64 outputs, 0..128 parameters. Repeated inputs use one
 * trailing homogeneous contract after input_count fixed contracts; bounds are
 * 1..1024 when enabled, with fixed plus repeated inputs at most 1024.
 * repeated_match=PS_REPEATED_MATCH_DTYPE_SHAPE_V8 requires the selected tensor
 * members of repeated inputs to share dtype and full logical shape, including
 * batch axes. PS_REPEATED_MATCH_NONE_V8 permits differing dtype/shape and
 * requires prepare for an active group. A disabled group uses NULL and zero
 * for repeated_input, both bounds and repeated_match. Other match codes fail.
 * A lower callback is mandatory. prepare is optional for
 * fixed prototypes; prepare_kernel is optional for COPY-only/elided calls.
 * Callbacks are reentrant, must not throw across the C ABI, and report statuses
 * from fixed_invocation_api.h. On any prepare/prepare_kernel return, ownership
 * of a populated output state/kernel passes to the host, including failure.
 */
typedef struct ps_operation_v8 {
  uint32_t struct_size;
  ps_string_v8 key;
  uint32_t flags, input_count;
  const ps_port_contract_v8* inputs;
  const ps_port_contract_v8* repeated_input;
  uint32_t repeated_minimum, repeated_maximum, repeated_match;
  const ps_operation_output_v8* outputs;
  uint32_t output_count;
  const ps_parameter_spec_v8* parameters;
  uint32_t parameter_count;
  void* user_data;
  ps_kernel_status_v8 (*prepare)(void*, const ps_static_query_v8*,
                                 const ps_preparation_sink_v8*,
                                 ps_operation_state_v8*);
  ps_kernel_status_v8 (*validate_resources)(void*, const void*,
                                            const ps_resource_view_v8*,
                                            uint32_t);
  ps_kernel_status_v8 (*lower)(void*, const ps_lowering_query_v8*,
                               const ps_operation_builder_v8*);
  ps_kernel_status_v8 (*prepare_kernel)(void*, const ps_kernel_preparation_v8*,
                                        ps_prepared_kernel_v8*);
} ps_operation_v8;

/** @brief Transactionally registered trusted native module. The host retains
 * this table and its code until every definition/static/kernel owner retires,
 * then calls destroy(context) before unloading. destroy is required, noexcept.
 * All tables require naturally aligned pointers and exact ABI/version sizes.
 * Native code is trusted; validation provides no process isolation.
 */
typedef struct ps_operation_module_v8 {
  uint32_t struct_size, abi_version;
  const ps_operation_v8* operations;
  uint32_t operation_count;
  void* context;
  void (*destroy)(void*);
} ps_operation_module_v8;
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void);

#ifdef __cplusplus
}
#endif
#endif /* INCLUDE_PHOTOSPIDER_PLUGIN_OPERATION_MODULE_API_H_ */
