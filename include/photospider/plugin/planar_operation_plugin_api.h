#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_PLANAR_OPERATION_PLUGIN_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_PLANAR_OPERATION_PLUGIN_API_H_
#include "photospider/plugin/operation_plugin_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Optional independently versioned structural-image extension to ABI 9.
 * Tables correspond one-to-one to the base operation table. Every record in
 * an extended DSO is planar and replaces execute; mixed Value/planar modules,
 * dependency programs, GPU and multiple results are excluded.
 * The base API destroy owns both tables. All functions must be noexcept across
 * this boundary. Native modules are trusted code, not sandboxed programs.
 */
#define PS_PLANAR_OPERATION_ABI_VERSION_1 1U
/** @brief Borrowed metadata. Shape/axes are logical; order 0 continuous, 1
 * tiled. channel_axis is UINT32_MAX for a single-plane rank-two image. Facets
 * expire when the infer/execute call returns. Groups are deliberately
 * unspecified; semantic groups are described by facets, and outputs use no
 * structural groups.
 */
typedef struct ps_planar_metadata_v1 {
  uint32_t struct_size, element_type, rank, facet_count;
  uint64_t shape[8];
  const ps_operation_facet_view_v9* facets;
  uint32_t order, height_axis, width_axis, channel_axis;
  uint64_t row_pitch_bytes;
} ps_planar_metadata_v1;
/** @brief Invocation-local services. Only cancelled is thread-safe; other
 * services run on the callback thread. Plugins execute serially on the host
 * assigned thread and must not create workers or submit to external pools.
 * Read/write rows stop at tile/ROI edges.
 * Scratch is host-owned, zero-initialized, 8-byte aligned, live-budgeted and
 * released by exact returned pointer or automatically at callback return.
 * Failed services are sticky, overriding success; output commits only after a
 * successful callback and final cancellation check. Never retain pointers.
 */
typedef struct ps_planar_services_v1 {
  uint32_t struct_size;
  void* context;
  int (*read_row)(void*, uint32_t input, const uint64_t* coordinate,
                  uint32_t rank, const uint8_t** bytes, uint64_t* samples);
  int (*write_row)(void*, const uint64_t* coordinate, uint32_t rank,
                   uint8_t** bytes, uint64_t* samples);
  uint8_t* (*allocate_scratch)(void*, uint64_t bytes);
  int (*release_scratch)(void*, uint8_t* bytes);
  int (*cancelled)(void*);
} ps_planar_services_v1;
/** @brief One planar operation. infer is pure and may run concurrently.
 * It returns complete output metadata; pointer fields must refer to input or
 * descriptor-owned storage (never callback stack storage). execute receives
 * complete Whole input windows; output shape is already inferred/validated.
 * Results reuse PS_OPERATION_RESULT values; 4=ResourceExhausted,
 * 5=TypeMismatch, 6=InvalidArgument. Unknown codes fail as OperationFailed.
 */
typedef struct ps_planar_operation_v1 {
  uint32_t struct_size;
  int (*infer)(void* user, const ps_planar_metadata_v1*, uint32_t inputs,
               const ps_operation_parameter_value_v9*, uint32_t parameters,
               ps_planar_metadata_v1* output, char* diagnostic,
               size_t capacity);
  int (*execute)(void* user, const ps_planar_metadata_v1*, uint32_t inputs,
                 const ps_operation_parameter_value_v9*, uint32_t parameters,
                 const ps_planar_metadata_v1* output,
                 const ps_planar_services_v1*, char* diagnostic,
                 size_t capacity);
} ps_planar_operation_v1;
/** @brief Exact-size extension table, retained until base destroy. */
typedef struct ps_planar_operation_plugin_api_v1 {
  uint32_t struct_size, abi_version, operation_count;
  const ps_planar_operation_v1* operations;
} ps_planar_operation_plugin_api_v1;
/** @brief Optional exported entry point; no host state is changed by lookup. */
PS_OPERATION_EXPORT const ps_planar_operation_plugin_api_v1*
ps_operation_plugin_get_planar_api_v1(void);
#ifdef __cplusplus
}
#endif
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_PLANAR_OPERATION_PLUGIN_API_H_
