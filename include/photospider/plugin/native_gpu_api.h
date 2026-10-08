/** @file native_gpu_api.h
 * @brief Standalone C ABI for invocation-scoped native GPU services.
 *
 * This header is independently versioned from the operation plugin ABI. It
 * declares buffer and synchronous dispatch services without depending on the
 * Result operation plugin header.
 */
#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_NATIVE_GPU_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_NATIVE_GPU_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS_GPU_ABI_VERSION_1 1U
/** @brief Native service outcomes; failures also enter the host failure latch.
 */
typedef enum ps_gpu_result_v1 {
  PS_GPU_RESULT_SUCCESS_V1 = 0,
  PS_GPU_RESULT_FAILURE_V1 = 1,
  PS_GPU_RESULT_CANCELLED_V1 = 2,
  PS_GPU_RESULT_BACKEND_UNAVAILABLE_V1 = 3
} ps_gpu_result_v1;
/** @brief Borrowed invocation cancellation probe; nonzero means cancelled. */
typedef int (*ps_gpu_cancelled_v1)(void* context);

/** @brief Native backend identifiers reported by the GPU service. */
#define PS_GPU_BACKEND_METAL_V1 1U
#define PS_GPU_BACKEND_VULKAN_V1 2U
/** @brief Trusted module encoding; MSL is the zero-initialized default. */
#define PS_GPU_CODE_MSL_V1 0U
#define PS_GPU_CODE_SPIRV_V1 1U

/** @brief One invocation-local native buffer binding; no native handle escapes.
 * @note Token comes from buffer(), offset/size address only that view. Index is
 * 0..30. writable is 0 or 1 and cannot promote an immutable input to writable.
 */
typedef struct ps_gpu_buffer_binding_v1 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Metal argument index or Vulkan set-0 binding index, in 0..30.
   * SPIR-V resources at these indexes use storage-buffer descriptors.
   */
  uint32_t index;
  /** @brief Nonzero token returned by this invocation's buffer service. */
  uint64_t token;
  /** @brief Byte offset relative to the acquired view.
   * @note The resulting offset into the underlying buffer must be divisible
   * by the service's minimum_buffer_offset_alignment. The shader may address
   * a finer logical subregion using its own constants and integer indexing.
   */
  uint64_t offset;
  /** @brief Positive accessible byte count within the acquired view. */
  uint64_t byte_size;
  /** @brief Zero for read-only access, one for permitted mutable access. */
  uint32_t writable;
} ps_gpu_buffer_binding_v1;

/** @brief One bounded trusted native compute dispatch, borrowed during execute.
 * @note Source is 1..262144 bytes, entry 1..128 bytes, bindings <=31. Constants
 * are at most 4096 bytes at a distinct index. Grid dimensions
 * are 1..UINT32_MAX. MSL compiles with safe math and no contraction. SPIR-V
 * modules carry their declared numerical execution modes and must be compiled
 * for their operation's profile. Code is trusted process code.
 * Shader access must remain inside declared views;
 * binding validation is not a shader sandbox.
 */
typedef struct ps_gpu_dispatch_v1 {
  /** @brief Exact structure byte size. */
  uint32_t struct_size;
  /** @brief Borrowed UTF-8 MSL or naturally uint32-aligned SPIR-V bytes.
   * SPIR-V requires a valid little-endian module header and word-sized length.
   */
  const char* source;
  /** @brief Exact source byte count. */
  uint32_t source_size;
  /** @brief UTF-8 entry name, borrowed without requiring a terminator. */
  const char* entry;
  /** @brief Exact entry-name byte count. */
  uint32_t entry_size;
  /** @brief Naturally aligned binding array, nullable when count is zero. */
  const ps_gpu_buffer_binding_v1* buffers;
  /** @brief Binding count, with unique indexes distinct from constants. */
  uint32_t buffer_count;
  /** @brief Borrowed constant bytes, nullable when constant_size is zero. */
  const void* constants;
  /** @brief Constant byte count; copied into command metadata by the host. */
  uint32_t constant_size;
  /** @brief Buffer index for constants, in 0..30.
   * Vulkan uses one set-0 uniform-buffer descriptor at this index; its layout
   * must match the supplied bytes. SPIR-V push constants are outside this ABI.
   */
  uint32_t constant_index;
  /** @brief Positive total thread counts along x, y and z. */
  uint64_t grid[3];
  /** @brief Explicit complete threadgroup shape, or {0,0,0} for host choice.
   * Mixed zero/nonzero shapes reject. Explicit shapes dispatch ceil(grid/group)
   * complete groups; the shader must guard padded threads. Group dimensions
   * and product must fit the selected device/pipeline limits. Use explicit
   * shapes for group barriers, shared memory and fixed reduction topology.
   */
  uint32_t group[3];
  /** @brief PS_GPU_CODE_MSL_V1 or PS_GPU_CODE_SPIRV_V1.
   * A backend accepts its matching encoding. SPIR-V declares a fixed LocalSize;
   * a zero group selects that size, and an explicit group must match it.
   */
  uint32_t code_format;
} ps_gpu_dispatch_v1;

/** @brief Synchronous host GPU services; null for CPU invocations.
 * @note All tokens and pointers expire at callback return. Host buffers alone
 * are eligible. Calls never throw; failures are sticky and override callback
 * success. execute accepts 1..32 dispatches and drains submitted native work
 * before returning, including cancellation/failure. Use only on the invoking
 * callback thread; do not access mutable buffer bytes concurrently with
 * execute. The service owns queue and pipelines. Output and scratch allocations
 * use the enclosing sink services and count against the host's live buffer
 * budget.
 */
typedef struct ps_gpu_service_v1 {
  /** @brief Exact structure byte size supplied by the host. */
  uint32_t struct_size;
  /** @brief PS_GPU_ABI_VERSION_1; check before using the table. */
  uint32_t abi_version;
  /** @brief Borrowed host state passed unchanged to service functions. */
  void* context;
  /** @brief Acquires a bounded buffer token; writes token only on success.
   * @param context This service's host state.
   * @param bytes Start of a live host native input, output or scratch view.
   * @param byte_size Positive view size within the allocation payload.
   * @param writable Zero or one; immutable inputs cannot become writable.
   * @param token Nonnull destination for the invocation-local token.
   * @return A PS_GPU_RESULT_*_V1 code; failure becomes sticky.
   * @note At most 1024 simultaneously live views; publication revokes write
   * access. Release a token after its final synchronous dispatch to release
   * its retained owner. Reacquisition never makes an old token valid again.
   */
  int (*buffer)(void* context, const uint8_t* bytes, uint64_t byte_size,
                uint32_t writable, uint64_t* token);
  /** @brief Executes trusted dispatch records synchronously.
   * @param context This service's host state.
   * @param commands Naturally aligned borrowed dispatch array.
   * @param command_count Array size in 1..32.
   * @return A PS_GPU_RESULT_*_V1 code; submitted device failure ends
   * Run.
   * @note Successful return permits CPU access to the completed shared bytes.
   * Retained pipelines are internal; callers must not retain service pointers.
   */
  int (*execute)(void* context, const ps_gpu_dispatch_v1* commands,
                 uint32_t command_count);
  /** @brief Releases one live token after synchronous work has drained.
   * @return A PS_GPU_RESULT_*_V1 code; invalid/double release is sticky.
   * @note Releases the view owner, not the enclosing allocation owner's
   * reference. Stale tokens reject even when their slot has been reused.
   * Cleanup remains available after an earlier service error.
   */
  int (*release)(void* context, uint64_t token);
  /** @brief PS_GPU_BACKEND_METAL_V1 or PS_GPU_BACKEND_VULKAN_V1. */
  uint32_t backend;
  /** @brief Positive alignment for the total native buffer binding offset. */
  uint64_t minimum_buffer_offset_alignment;
} ps_gpu_service_v1;

#ifdef __cplusplus
}
#endif
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_NATIVE_GPU_API_H_
