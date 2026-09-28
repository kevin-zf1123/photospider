#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_PLANAR_OPERATION_PLUGIN_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_PLANAR_OPERATION_PLUGIN_API_H_
#include "photospider/plugin/cpu_parallel_api.h"
#include "photospider/plugin/cpu_tiles_api.h"
#include "photospider/plugin/operation_plugin_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Optional independently versioned structural-image extension to
 * ABI 11. Tables correspond one-to-one to the base operation table. Every
 * record in an extended DSO is planar and replaces execute; mixed Value/planar
 * modules, dependency programs and multiple results are excluded. GPU Whole
 * operations use native scratch and explicit GPU services, with no CPU
 * fallback. The base API destroy owns both tables. All functions must be
 * noexcept across this boundary. Native modules are trusted code, not sandboxed
 * programs.
 */
#define PS_PLANAR_OPERATION_ABI_VERSION_3 3U
#define PS_PLANAR_EXECUTION_WHOLE_V3 0U
#define PS_PLANAR_EXECUTION_CPU_STAGES_V3 1U
/** @brief Borrowed metadata. Shape/axes are logical; order 0 continuous, 1
 * tiled. channel_axis is UINT32_MAX for a single-plane rank-two image. Facets
 * expire when the infer/execute call returns. Groups are deliberately
 * unspecified; semantic groups are described by facets, and outputs use no
 * structural groups.
 */
typedef struct ps_planar_metadata_v3 {
  uint32_t struct_size, element_type, rank, facet_count;
  uint64_t shape[8];
  const ps_operation_facet_view_v11* facets;
  uint32_t order, height_axis, width_axis, channel_axis;
  uint64_t row_pitch_bytes;
} ps_planar_metadata_v3;
/** @brief Invocation-local services. cancelled is thread-safe. Row and
 * scratch services run only on the owning callback thread, outside a parallel
 * range block. Plugins may use cpu_parallel for synchronous host range work;
 * they must not create workers or submit to external pools. Preallocate buffers
 * before run(), and release them only after its barrier returns.
 * Read/write rows stop at tile/ROI edges.
 * Scratch is host-owned, zero-initialized, 8-byte aligned, live-budgeted and
 * released by exact returned pointer or automatically at callback return.
 * The workspace bound counts aggregate live requested bytes. Native backing
 * alignment and padding are additionally charged at actual capacity to the
 * context's resource budget. GPU tokens retain both charges until retirement.
 * Failed services are sticky, overriding success; output commits only after a
 * successful callback and final cancellation check. Never retain pointers.
 */
typedef struct ps_planar_services_v3 {
  uint32_t struct_size;
  void* context;
  int (*read_row)(void*, uint32_t input, const uint64_t* coordinate,
                  uint32_t rank, const uint8_t** bytes, uint64_t* samples);
  int (*write_row)(void*, const uint64_t* coordinate, uint32_t rank,
                   uint8_t** bytes, uint64_t* samples);
  uint8_t* (*allocate_scratch)(void*, uint64_t bytes);
  int (*release_scratch)(void*, uint8_t* bytes);
  int (*cancelled)(void*);
  /** @brief Nonnull on CPU Whole calls; borrowed until execute returns.
   * Exact version/size must match cpu_parallel_api.h before use. */
  const ps_cpu_parallel_service_v1* cpu_parallel;
  /** @brief Actual backend: 1 CPU, 2 native GPU. */
  uint32_t backend;
  /** @brief Nonnull only for GPU Whole; null cpu_parallel in that case.
   * Scratch buffers are native-bindable. Row pointers are host views and must
   * be copied at explicit device boundaries. Tokens retain allocation owners
   * until release() or callback return, independently of release_scratch().
   */
  const ps_gpu_service_v11* gpu;
  /** @brief Precharges algorithm work and observes cancellation/currentness.
   * Returns 1 on success, 0 on sticky failure. Serial callback thread only.
   * Passing zero checks external stop without consuming work.
   */
  int (*consume_work)(void*, uint64_t units);
  /** @brief Present exclusively for CPU stage coordinators. Each call waits
   * for its tile callbacks on the shared pool; cpu_parallel and gpu are null.
   * The execute coordinator runs on the caller, outside kernel workers.
   */
  const ps_cpu_tiles_service_v1* cpu_tiles;
} ps_planar_services_v3;
/** @brief One planar operation. infer is pure and may run concurrently.
 * It returns complete output metadata; pointer fields must refer to input or
 * descriptor-owned storage (never callback stack storage). execute receives
 * complete Whole input windows; output shape is already inferred/validated.
 * Results reuse PS_OPERATION_RESULT values; 4=ResourceExhausted,
 * 5=TypeMismatch, 6=InvalidArgument. Unknown codes fail as OperationFailed.
 */
typedef struct ps_planar_operation_v3 {
  uint32_t struct_size;
  int (*infer)(void* user, const ps_planar_metadata_v3*, uint32_t inputs,
               const ps_operation_parameter_value_v11*, uint32_t parameters,
               ps_planar_metadata_v3* output, char* diagnostic,
               size_t capacity);
  int (*execute)(void* user, const ps_planar_metadata_v3*, uint32_t inputs,
                 const ps_operation_parameter_value_v11*, uint32_t parameters,
                 const ps_planar_metadata_v3* output,
                 const ps_planar_services_v3*, char* diagnostic,
                 size_t capacity);
  /** @brief WHOLE uses its selected backend callback lane. CPU_STAGES uses
   * caller orchestration with cpu_tiles, and requires a CPU-only operation.
   * Both models retain the operation's declared Whole dependency/ROI contract.
   */
  uint32_t execution_model;
} ps_planar_operation_v3;
/** @brief Exact-size extension table, retained until base destroy. */
typedef struct ps_planar_operation_plugin_api_v3 {
  uint32_t struct_size, abi_version, operation_count;
  const ps_planar_operation_v3* operations;
} ps_planar_operation_plugin_api_v3;
/** @brief Optional exported entry point; no host state is changed by lookup. */
PS_OPERATION_EXPORT const ps_planar_operation_plugin_api_v3*
ps_operation_plugin_get_planar_api_v3(void);
#ifdef __cplusplus
}
#endif
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_PLANAR_OPERATION_PLUGIN_API_H_
