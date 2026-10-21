#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_NATIVE_KERNEL_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_NATIVE_KERNEL_API_H_

#include "photospider/plugin/operation_module_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS_NATIVE_GPU_ABI_VERSION_2 2U
#define PS_NATIVE_METAL_V2 1U
#define PS_NATIVE_VULKAN_V2 2U
#define PS_NATIVE_MSL_V2 0U
#define PS_NATIVE_SPIRV_V2 1U
#define PS_NATIVE_INPUT_V2 0U
#define PS_NATIVE_OUTPUT_V2 1U
#define PS_NATIVE_SCRATCH_V2 2U

/** @brief Fixed shader binding to one invocation view.
 * index is the shader buffer index in 0..30. source selects
 * input/output/scratch. view is the invocation array index for input/output, or
 * the memory domain for scratch. offset and bytes select a positive bounded
 * range relative to the view's logical origin. Inputs remain immutable. Each
 * output is bound exactly once over its complete accessible range; the shader
 * must initialize it. Shared views use host addresses; Device views use opaque
 * allocation identities. Offsets apply to the native buffer, never to an opaque
 * identity. Host views are unsupported. Invocation domains match preparation.
 */
typedef struct ps_native_binding_v2 {
  uint32_t index, source, view;
  uint64_t offset, bytes;
} ps_native_binding_v2;

/** @brief One immutable native dispatch, copied during preparation.
 * Source is 1..262144 bytes; entry is 1..128 UTF-8 bytes. At most 31 distinct
 * buffer indexes may be bound. Constants contain at most 4096 immutable bytes
 * at a separate index. Grid dimensions are 1..UINT32_MAX. Group is either all
 * zero (Metal device choice or SPIR-V LocalSize) or three positive dimensions;
 * explicitly padded groups require bounds guards in the shader. SPIR-V source
 * is aligned to four bytes, has a byte length of 20..262144 divisible by four,
 * and declares a fixed positive LocalSize; explicit group dimensions must
 * match. Trusted shader code obeys declared accesses and numerical semantics;
 * this interface is not a shader sandbox.
 */
typedef struct ps_native_program_v2 {
  uint32_t struct_size, code_format;
  ps_string_v8 source, entry;
  const ps_native_binding_v2* bindings;
  uint32_t binding_count;
  const void* constants;
  uint32_t constant_bytes, constant_index;
  uint64_t grid[3];
  uint32_t group[3];
} ps_native_program_v2;

/** @brief Preparation-only native service, borrowed for prepare_kernel.
 * prepare validates bindings against the actual task signature and compiles
 * the pipeline without running it. On success the output owns one immutable
 * prepared kernel and its device lease; pass it to the enclosing host or call
 * release exactly once. Failure leaves output empty. Returned submit_gpu
 * accepts borrowing until device completion establishes visibility and calls
 * finish exactly once. Cancellation never releases accepted work early.
 * All callbacks are reentrant and nonthrowing. Do not retain the service table.
 */
typedef struct ps_native_gpu_service_v2 {
  uint32_t struct_size, abi_version, backend;
  void* context;
  ps_kernel_status_v8 (*prepare)(void*, const ps_native_program_v2*,
                                 const ps_kernel_preparation_v8*,
                                 ps_prepared_kernel_v8*);
} ps_native_gpu_service_v2;

#ifdef __cplusplus
}
#endif
#endif /* INCLUDE_PHOTOSPIDER_PLUGIN_NATIVE_KERNEL_API_H_ */
