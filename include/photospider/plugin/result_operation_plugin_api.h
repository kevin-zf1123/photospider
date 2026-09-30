#ifndef PHOTOSPIDER_RESULT_OPERATION_PLUGIN_API_H
#define PHOTOSPIDER_RESULT_OPERATION_PLUGIN_API_H

#include <stdint.h>

#include "photospider/plugin/cpu_parallel_api.h"
#include "photospider/plugin/cpu_tiles_api.h"
#include "photospider/plugin/operation_plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif
#define PS_RESULT_OPERATION_ABI_VERSION_1 1U
#define PS_RESULT_NEED_V1 100
#define PS_RESULT_PUBLISH_V1 101
#define PS_RESULT_VALUE_V1 1U
#define PS_RESULT_SCALAR_V1 2U
#define PS_RESULT_TYPED_V1 5U
#define PS_RESULT_OBJECT_V1 6U
#define PS_RESULT_WHOLE_V1 1U
#define PS_RESULT_REGIONAL_V1 2U
#define PS_RESULT_TARGET_VALUE_V1 0U
#define PS_RESULT_TARGET_FIELD_V1 1U
#define PS_RESULT_TARGET_IMAGE_V1 2U
#define PS_RESULT_TARGET_DESCRIPTOR_V1 3U
#define PS_RESULT_EXACT_V1 1U
#define PS_RESULT_CONSERVATIVE_V1 2U
#define PS_RESULT_UNKNOWN_V1 3U
#define PS_RESULT_FINAL_V1 15U
#define PS_RESULT_NO_CHANNEL_V1 UINT32_MAX
#define PS_RESULT_EXTENT_FIXED_V1 1U
#define PS_RESULT_EXTENT_INPUT_AXIS_V1 2U
#define PS_RESULT_EXTENT_INPUT_ELEMENTS_V1 3U
#define PS_RESULT_EXTENT_FIELD_ROWS_V1 4U
#define PS_RESULT_EXTENT_RUNTIME_COUNT_V1 5U
#define PS_RESULT_COMPLETE_BUNDLE_V1 1U
#define PS_RESULT_INDEPENDENT_CHUNKS_V1 2U
#define PS_RESULT_STABLE_PREFIX_V1 3U

typedef struct ps_result_region_v1 {
  uint32_t struct_size, rank;
  uint64_t offset[8], extent[8];
} ps_result_region_v1;
typedef struct ps_result_group_v1 {
  uint32_t struct_size;
  const char* role;
  uint32_t role_size;
  uint64_t first_channel, channel_count;
} ps_result_group_v1;
typedef struct ps_result_image_spec_v1 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size, element_type, rank;
  uint64_t shape[3], frames, layers;
  uint32_t height_axis, width_axis, channel_axis;
  uint32_t storage_order; /* 0=continuous, 1=tiled. */
  uint64_t row_pitch_bytes;
  const ps_result_group_v1* groups;
  uint32_t group_count;
  const ps_operation_facet_view_v11* facets;
  uint32_t facet_count;
} ps_result_image_spec_v1;
typedef struct ps_result_extent_v1 {
  uint32_t kind, input, axis, field;
  uint64_t value, divisor, offset;
} ps_result_extent_v1;
typedef struct ps_result_field_spec_v1 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size, element_type, record_rank;
  uint64_t record_shape[7];
  ps_result_extent_v1 rows;
} ps_result_field_spec_v1;
typedef struct ps_result_schema_v1 {
  uint32_t struct_size;
  const char* id;
  uint32_t id_size, version, publication;
  const ps_result_field_spec_v1* fields;
  uint32_t field_count;
  const ps_result_image_spec_v1* images;
  uint32_t image_count;
  const ps_result_extent_v1* domain;
  uint32_t domain_rank;
  const ps_operation_facet_view_v11* metadata;
  uint32_t metadata_count;
} ps_result_schema_v1;
typedef struct ps_result_port_v1 {
  uint32_t struct_size, kind, element_type, rank;
  uint64_t shape[8];
  const ps_result_schema_v1* schema;
  const ps_operation_facet_view_v11* facets;
  uint32_t facet_count;
  float minimum, maximum;
  uint32_t semantic_kind, element_type_mask;
} ps_result_port_v1;
typedef struct ps_result_output_v1 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size;
  ps_result_port_v1 port;
  /* UINT32_MAX selects every input; zero selects none. */
  const uint32_t* input_indices;
  uint32_t input_count, execution;
} ps_result_output_v1;
typedef struct ps_result_relation_row_v1 {
  uint64_t output;
  uint32_t input, roles, target, slot;
  uint64_t first, count;
} ps_result_relation_row_v1;
typedef struct ps_result_query_v1 {
  uint32_t struct_size, output_index, backend, image_slot;
  /** @brief 0=complete Result object, 1=Value footprint, 2=image footprint.
   * Zero requested boxes with kind 1/2 means Empty, never Whole. */
  uint32_t requested_kind;
  const ps_result_port_v1* inputs;
  uint32_t input_count;
  const ps_result_output_v1* output;
  const ps_result_region_v1* requested;
  uint32_t requested_count;
  uint64_t tile_height, tile_width;
  const ps_operation_parameter_value_v11* parameters;
  uint32_t parameter_count;
} ps_result_query_v1;

/** @brief Captured immutable Result facts. No object identity is content.
 * A field Need authorizes this descriptor; image Needs authorize only their
 * image read capabilities. rows are certified prefixes, including zero.
 */
typedef struct ps_result_descriptor_v1 {
  uint32_t struct_size, sealed, field_count, image_count;
  uint64_t object_id, revision, rows[16];
} ps_result_descriptor_v1;
/* Services and query are borrowed for one start/poll. Image reads are copies
 * from explicitly supplied Need coverage. Retained handles preserve that
 * coverage until release/destroy. Every failed service is sticky. Temporary
 * field I/O executes after Need yields; read_io observes its next-poll reply.
 * Calls must use the entry thread. Only cancelled is callable from CPU range
 * or tile workers; those workers may also write their admitted scratch bytes.
 */
typedef struct ps_result_services_v1 {
  uint32_t struct_size, abi_version;
  void* context;
  int (*need_value)(void*, uint32_t input, uint32_t roles,
                    const ps_result_region_v1*, uint32_t count);
  int (*need_image)(void*, uint32_t input, uint32_t slot, uint32_t roles,
                    const ps_result_region_v1*, uint32_t count);
  int (*need_result)(void*, uint32_t input, uint32_t field, uint32_t complete,
                     uint64_t minimum_rows);
  int (*read_value)(void*, uint32_t input, const uint64_t*, uint32_t rank,
                    void*, uint64_t bytes);
  int (*read_image)(void*, uint32_t input, uint32_t slot, const uint64_t*,
                    uint32_t rank, void*, uint64_t bytes);
  int (*retain_image)(void*, uint32_t input, uint32_t slot, uint64_t* handle);
  int (*read_retained_image)(void*, uint64_t handle, const uint64_t*,
                             uint32_t rank, void*, uint64_t bytes);
  int (*release_image)(void*, uint64_t handle);
  int (*allocate_scratch)(void*, uint64_t bytes, uint8_t** destination);
  int (*release_scratch)(void*, uint8_t*);
  int (*consume_work)(void*, uint64_t units);
  int (*cancelled)(void*);
  int (*begin_result)(void*);
  int (*bind_descriptor)(void*, const ps_result_relation_row_v1*, uint32_t,
                         uint32_t guarantee);
  int (*publish_image)(void*, uint32_t slot, const ps_result_region_v1*,
                       const uint8_t*, uint64_t bytes,
                       const ps_result_relation_row_v1*, uint32_t rows,
                       uint32_t guarantee, uint32_t finality);
  int (*append_field)(void*, uint32_t field, uint64_t rows, const uint8_t*,
                      uint64_t bytes);
  int (*publish_field)(void*, uint32_t field, uint64_t end,
                       const ps_result_relation_row_v1*, uint32_t rows,
                       uint32_t guarantee, uint32_t finality);
  int (*need_field_read)(void*, uint32_t input, uint32_t field, uint64_t first,
                         uint64_t rows);
  int (*read_io)(void*, uint32_t reply, uint64_t offset, uint8_t*,
                 uint64_t bytes);
  int (*publish_result)(void*, uint32_t complete);
  int (*result_descriptor)(void*, uint32_t input, ps_result_descriptor_v1*);
  int (*publish_value)(void*, const ps_result_region_v1*, const uint8_t*,
                       uint64_t bytes, const ps_result_relation_row_v1*,
                       uint32_t rows, uint32_t guarantee);
  const ps_cpu_parallel_service_v1* cpu_parallel;
  const ps_cpu_tiles_service_v1* cpu_tiles;
  const ps_gpu_service_v11* gpu;
} ps_result_services_v1;

/** @brief Pure metadata sink borrowed only during resolver entry. Each output
 * must be supplied once. set_output copies the complete nested record before
 * returning, so callback-local schemas/images/facets are safe. Errors are
 * sticky. set_output must be called on the resolver entry thread.
 */
typedef struct ps_result_metadata_sink_v1 {
  uint32_t struct_size;
  void* context;
  int (*set_output)(void*, uint32_t, const ps_result_port_v1*);
} ps_result_metadata_sink_v1;
typedef struct ps_result_operation_v1 {
  uint32_t struct_size;
  const char* key;
  uint32_t key_size, flags;
  const ps_result_port_v1* inputs;
  uint32_t input_count;
  const ps_result_output_v1* outputs;
  uint32_t output_count;
  const ps_operation_parameter_descriptor_v11* parameters;
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
  int (*resolve_metadata)(void*, const ps_result_port_v1*, uint32_t,
                          const ps_operation_parameter_value_v11*, uint32_t,
                          const ps_result_port_v1*, uint32_t,
                          const ps_result_metadata_sink_v1*);
  int (*start)(void*, void*, const ps_result_query_v1*,
               const ps_result_services_v1*);
  int (*poll)(void*, void*, const ps_result_query_v1*,
              const ps_result_services_v1*);
  void (*destroy)(void*, void*);
} ps_result_operation_v1;
typedef struct ps_result_operation_plugin_api_v1 {
  uint32_t struct_size, abi_version;
  const ps_result_operation_v1* operations;
  uint32_t operation_count;
  void* context;
  void (*destroy)(void*);
} ps_result_operation_plugin_api_v1;
PS_OPERATION_EXPORT const ps_result_operation_plugin_api_v1*
ps_result_operation_plugin_get_api_v1(void);
#ifdef __cplusplus
}
#endif
#endif
