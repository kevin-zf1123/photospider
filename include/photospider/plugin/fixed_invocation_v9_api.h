#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_FIXED_INVOCATION_V9_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_FIXED_INVOCATION_V9_API_H_

#include "photospider/plugin/fixed_invocation_api.h"

#ifdef __cplusplus
extern "C" {
#endif
#define PS_FIXED_INVOCATION_ABI_VERSION_9 9U
/** @brief Borrowed Run-scoped admission, thread-safe until CPU return.
 * work includes arithmetic and integer limb operations; stages counts explicit
 * refinement attempts. Admission precedes computation, is atomic across both
 * dimensions and never refunds. Zero/zero polls cancellation and sticky
 * failure. Budget rejection returns ResourceExhausted with WorkLimit or
 * StageLimit; cancellation can return Cancelled.
 * The callback never throws. The context must not escape terminal completion or
 * cross Runs.
 */
typedef struct ps_kernel_work_v9 {
  void* context;
  ps_kernel_status_v8 (*consume)(void* context, uint64_t work, uint64_t stages);
} ps_kernel_work_v9;
/** @brief Explicit v9 envelope containing unmodified v8 view vocabulary.
 * header fields are exact sizeof(v9), version 9. views retains its independent
 * exact v8 header. It is not a v8 invocation with an undocumented tail.
 * Providers may forward views to legacy computation only when no v9 charging is
 * claimed.
 */
typedef struct ps_kernel_invocation_v9 {
  uint32_t struct_size, abi_version;
  ps_kernel_invocation_v8 views;
  ps_kernel_work_v9 work;
} ps_kernel_invocation_v9;
typedef void (*ps_kernel_cpu_invoke_v9)(const ps_kernel_invocation_v9*,
                                        ps_kernel_status_v8*);
/** @brief CPU-only v9 preparation; ownership follows prepared_kernel_v8.
 * Native service ABI2 and asynchronous GPU entrypoints remain unchanged.
 */
typedef struct ps_prepared_kernel_v9 {
  uint32_t struct_size, abi_version;
  const void* context;
  void (*release)(const void* context);
  ps_kernel_cpu_invoke_v9 invoke_cpu;
} ps_prepared_kernel_v9;
#ifdef __cplusplus
}
#endif
#endif /* INCLUDE_PHOTOSPIDER_PLUGIN_FIXED_INVOCATION_V9_API_H_ */
