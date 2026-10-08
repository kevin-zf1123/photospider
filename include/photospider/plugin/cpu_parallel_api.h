#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_CPU_PARALLEL_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_CPU_PARALLEL_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define PS_CPU_PARALLEL_ABI_VERSION_1 1U
/** @brief One finite host-assigned block [begin,end), with a unique live slot.
 * slot is below the granted maximum_workers. Blocks may execute concurrently
 * and out of order. Write disjoint output ranges and use slot-private scratch.
 * Only immutable inputs and preallocated buffers may be accessed; do not call
 * callback-only allocation/publication/row services or reenter parallel work.
 * Return 0=success, 1=failure, 2=cancelled, 4=resource exhaustion, 6=invalid
 * argument. Exceptions must not cross a C module boundary.
 */
typedef int (*ps_cpu_range_callback_v1)(void* user, uint64_t begin,
                                        uint64_t end, uint32_t slot);
/** @brief Borrowed Whole CPU service, independently versioned and exact-sized.
 * run is synchronous: all active blocks retire before return, including failure
 * and cancellation. Inputs, user and scratch must remain alive until then.
 * Call run only on the owning callback thread. Nested calls reject. Workers
 * use nearest-even/gradual underflow and restore their floating environments.
 * Host errors are sticky and override enclosing callback success. No output is
 * published by this service. CPU tiled callbacks and GPU callbacks lack it.
 * run partitions [0,count) in grain-sized blocks; grain must be positive.
 * workers=0 uses maximum_workers; otherwise require 1..maximum_workers. A
 * smaller grant limits concurrency, not arithmetic order. count=0 does no work.
 * Before a nonempty run, the host reserves its job record in Host/Metadata and
 * one Entries unit, then charges the existing Root `work=count` and
 * `stages=ceil(count/grain)`. Prior Root work counts toward the limit; failure
 * is returned before block callbacks start. A zero count creates no job and
 * charges neither work nor stages. Structured CPU Result Whole polls receive
 * this borrowed service as `ResultProgramPhase::cpu_parallel`; it schedules
 * blocks inside that poll, not an independent Result dependency graph.
 * The host may execute fewer simultaneous blocks than the grant. No private
 * pool is created. Cancellation is cooperative between blocks; each block must
 * remain finite and poll its supplied invocation cancellation for long work.
 */
typedef struct ps_cpu_parallel_service_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t maximum_workers;
  void* context;
  int (*run)(void* context, uint64_t count, uint64_t grain, uint32_t workers,
             ps_cpu_range_callback_v1 callback, void* user);
} ps_cpu_parallel_service_v1;
#ifdef __cplusplus
}
#endif
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_CPU_PARALLEL_API_H_
