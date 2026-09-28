#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_CPU_TILES_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_CPU_TILES_API_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PS_CPU_TILES_ABI_VERSION_1 1U
/** @brief A finite grid of operation-defined work items, partitioned by host.
 * Each extent is a count; each tile extent is positive. A zero grid extent
 * produces zero callbacks. The product of ceil(extent[i]/tile[i]) must fit
 * uint64_t. Grid coordinates name computation work, independently of the
 * operation's input dependency Regions and physical storage tiles.
 * workers=0 selects the Run's granted maximum; a positive value selects at
 * most that maximum. Geometry stays fixed when the worker grant changes.
 */
typedef struct ps_cpu_tile_stage_v1 {
  uint32_t struct_size;
  uint64_t extent[3], tile[3];
  uint32_t workers;
} ps_cpu_tile_stage_v1;
/** @brief One disjoint half-open box [begin,end) within the stage grid.
 * index follows x-fastest tile order. slot is unique among active callbacks
 * of this stage and reusable after retirement. Each callback runs serially on
 * one host CPU worker. The descriptor is borrowed until callback return.
 */
typedef struct ps_cpu_tile_v1 {
  uint32_t struct_size, slot;
  uint64_t index, begin[3], end[3];
} ps_cpu_tile_v1;
/** @brief Returns the planar result code; exceptions stay inside DSO.
 * Codes: 0 success, 1 OperationFailed, 2 Cancelled, 3 BackendUnavailable,
 * 4 ResourceExhausted, 5 TypeMismatch, 6 InvalidArgument. Other codes map to
 * OperationFailed. BackendUnavailable is an error for this CPU-only service.
 * Shared inputs remain immutable; writes occupy disjoint assigned regions or
 * use a separately specified synchronization protocol. Long tiles poll the
 * thread-safe invocation cancellation service. Callback-only row/allocation
 * services belong to the stage coordinator; tile callbacks use buffers that
 * the coordinator retained before calling run().
 */
typedef int (*ps_cpu_tile_callback_v1)(void* user, const ps_cpu_tile_v1* tile);
/** @brief Synchronous stage service, exclusively for staged CPU operations.
 * The coordinator borrows this table during its planar execute invocation.
 * run submits tile callbacks to the context pool; the coordinator waits for
 * every active callback to retire, including its resource/floating scopes.
 * The caller supplies buffers and user state through that barrier. A failed
 * stage stops new claims, drains active callbacks and poisons the invocation.
 * Calls originate on the coordinator thread outside every kernel worker and
 * tile callback. Stages share the context's bounded waiting admission; a
 * stage holds one admission until all its claims retire and it leaves the
 * queue. Quota failure returns ResourceExhausted without automatic retry.
 */
typedef struct ps_cpu_tiles_service_v1 {
  uint32_t struct_size, abi_version, maximum_workers;
  void* context;
  int (*run)(void* context, const ps_cpu_tile_stage_v1* stage,
             ps_cpu_tile_callback_v1 callback, void* user);
} ps_cpu_tiles_service_v1;
#ifdef __cplusplus
}
#endif
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_CPU_TILES_API_H_
