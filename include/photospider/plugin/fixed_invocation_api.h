#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_FIXED_INVOCATION_API_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_FIXED_INVOCATION_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS_FIXED_INVOCATION_ABI_VERSION_8 8U
#define PS_KERNEL_MESSAGE_CAPACITY_V8 256U

/** @brief Operational status categories. Samples remain unchecked. */
typedef enum ps_kernel_code_v8 {
  PS_KERNEL_OK_V8 = 0,
  PS_KERNEL_INVALID_ARGUMENT_V8 = 1,
  PS_KERNEL_NOT_FOUND_V8 = 2,
  PS_KERNEL_CYCLE_V8 = 3,
  PS_KERNEL_TYPE_MISMATCH_V8 = 4,
  PS_KERNEL_RESOURCE_EXHAUSTED_V8 = 5,
  PS_KERNEL_CANCELLED_V8 = 6,
  PS_KERNEL_STALE_V8 = 7,
  PS_KERNEL_BACKEND_UNAVAILABLE_V8 = 8,
  PS_KERNEL_OPERATION_FAILED_V8 = 9,
  PS_KERNEL_INTERNAL_V8 = 10
} ps_kernel_code_v8;

/** @brief Stable optional failure details. NONE leaves code sufficient.
 * Values are carried in uint32_t fields; the C enum layout is not part of ABI.
 * Unknown codes or reasons produce INTERNAL with MALFORMED_ENVELOPE.
 * Operation-specific details belong in the opaque domain_code field.
 */
typedef enum ps_kernel_reason_v8 {
  PS_KERNEL_REASON_NONE_V8 = 0,
  PS_KERNEL_REASON_ARITHMETIC_OVERFLOW_V8 = 1,
  PS_KERNEL_REASON_INVALID_DOMAIN_V8 = 2,
  PS_KERNEL_REASON_MALFORMED_ENVELOPE_V8 = 3,
  PS_KERNEL_REASON_UNAUTHORIZED_READ_V8 = 4,
  PS_KERNEL_REASON_STALE_HANDLE_V8 = 5,
  PS_KERNEL_REASON_HOST_EXCEPTION_V8 = 6,
  PS_KERNEL_REASON_SHORT_IO_V8 = 7,
  PS_KERNEL_REASON_WORK_LIMIT_V8 = 8,
  PS_KERNEL_REASON_CAPACITY_LIMIT_V8 = 9,
  PS_KERNEL_REASON_STAGE_LIMIT_V8 = 10,
  PS_KERNEL_REASON_CANCELLED_V8 = 11,
  PS_KERNEL_REASON_STALE_VERSION_V8 = 12,
  PS_KERNEL_REASON_VIEW_UNAVAILABLE_V8 = 13
} ps_kernel_reason_v8;

/** @brief Stable tensor element codes, carried in uint32_t fields.
 * Integer names specify signedness and bit width; FLOAT32/FLOAT64 are IEEE-754
 * binary32/binary64. Zero is only a wildcard in a port predicate. Other codes
 * are rejected. Existing values retain their meanings within ABI 8.
 */
typedef enum ps_element_type_v8 {
  PS_ELEMENT_UINT8_V8 = 1,
  PS_ELEMENT_INT64_V8 = 2,
  PS_ELEMENT_FLOAT64_V8 = 3,
  PS_ELEMENT_FLOAT32_V8 = 4,
  PS_ELEMENT_INT8_V8 = 5,
  PS_ELEMENT_UINT16_V8 = 6,
  PS_ELEMENT_INT16_V8 = 7
} ps_element_type_v8;

/** @brief Fixed-size terminal record, copied into reserved completion storage.
 * message is UTF-8 with a terminating NUL; truncation is permitted. reason and
 * domain_code are optional machine-readable details. code uses
 * ps_kernel_code_v8 and reason uses ps_kernel_reason_v8; zero initializes both
 * to success/NONE. domain_code is opaque to the host. No field owns memory.
 */
typedef struct ps_kernel_status_v8 {
  uint32_t code, reason;
  uint64_t domain_code;
  char message[PS_KERNEL_MESSAGE_CAPACITY_V8];
} ps_kernel_status_v8;

#define PS_KERNEL_DOMAIN_HOST_V8 0U
#define PS_KERNEL_DOMAIN_DEVICE_V8 1U
#define PS_KERNEL_DOMAIN_SHARED_V8 2U

/** @brief Prepared affine layout, borrowed through terminal completion.
 * rank is 1..8; element_type uses ps_element_type_v8.
 * shape/origin are logical coordinates and strides are signed bytes. Unused
 * axes are zero. The host checks every reachable address before submission.
 */
typedef struct ps_kernel_layout_v8 {
  uint32_t rank, element_type;
  uint64_t shape[8], origin[8];
  int64_t strides[8];
} ps_kernel_layout_v8;

/** @brief Immutable input allocation or opaque native allocation handle.
 * bytes is accessible allocation capacity, byte_offset locates coordinate
 * origin, and port/member/tile_index are fixed by the Plan. All fields and the
 * allocation are borrowed until synchronous return or asynchronous completion.
 * A Device input may occupy a subview of a larger retained allocation: bytes
 * and byte_offset then include that allocation's base. Only samples in layout
 * are declared reads; extra capacity does not enlarge the read footprint.
 */
typedef struct ps_kernel_const_view_v8 {
  const void* allocation;
  uint64_t bytes, byte_offset;
  const ps_kernel_layout_v8* layout;
  uint32_t domain, port, member, reserved;
  uint64_t tile_index;
} ps_kernel_const_view_v8;

/** @brief Exclusive preallocated output. Success initializes every logical
 * sample of every declared output view. Distinct output tiles have distinct
 * allocations. The kernel neither releases nor replaces these handles.
 */
typedef struct ps_kernel_mutable_view_v8 {
  void* allocation;
  uint64_t bytes, byte_offset;
  const ps_kernel_layout_v8* layout;
  uint32_t domain, port, member, reserved;
  uint64_t tile_index;
} ps_kernel_mutable_view_v8;

typedef struct ps_kernel_scratch_v8 {
  void* allocation;
  uint64_t bytes;
  uint32_t domain, reserved;
} ps_kernel_scratch_v8;

/** @brief Borrowed thread-safe cancellation query. Querying allocates nothing.
 * A null query denotes an inert token. The context is owned by the Run.
 */
typedef struct ps_kernel_cancellation_v8 {
  const void* context;
  uint32_t (*is_cancelled)(const void* context);
} ps_kernel_cancellation_v8;

/** @brief Complete fixed invocation; the host retains every referenced owner.
 * Sizes and versions are checked before issue, not by numerical loops. A zero
 * count permits a null array. Parameters contain the Plan's immutable bytes.
 * prepared_context is read-only and shared concurrently by multiple Runs.
 */
typedef struct ps_kernel_invocation_v8 {
  uint32_t struct_size, abi_version;
  const void* prepared_context;
  const ps_kernel_const_view_v8* inputs;
  uint32_t input_count, output_count;
  const ps_kernel_mutable_view_v8* outputs;
  const ps_kernel_scratch_v8* scratch;
  uint32_t scratch_count, reserved;
  const void* parameters;
  uint64_t parameter_bytes;
  ps_kernel_cancellation_v8 cancellation;
  /** @brief Run-local logical Result ObjectId for each input view.
   * Entries correspond one-for-one to inputs and are nonzero. Tiles/members
   * of the same dynamic Result share an identity. CFG aliases and external
   * representation imports preserve it; newly produced Results receive fresh
   * identities. Imported views carry the external source ObjectId; exporting
   * their COPY storage creates a distinct Result ObjectId. Partial projections
   * also create new Result objects while internal aliases retain their source
   * association. Borrowed until terminal completion, never a content digest.
   * This separate array leaves the affine view record layout unchanged.
   */
  const uint64_t* input_object_ids;
} ps_kernel_invocation_v8;

typedef struct ps_kernel_completion_key_v8 {
  uint64_t run, activation, submission;
} ps_kernel_completion_key_v8;

#define PS_KERNEL_CLOCK_UNAVAILABLE_V8 0U
#define PS_KERNEL_CLOCK_METAL_MACH_NS_V8 1U
#define PS_KERNEL_CLOCK_VULKAN_TICKS_V8 2U

/** @brief Optional device command interval, never a host callback duration.
 * Zero clock_domain means unavailable. Metal values are system mach time in
 * nanoseconds. Vulkan values are raw queue-family timestamps with valid_bits
 * low bits; their modular difference is (end_tick - begin_tick) modulo
 * 2^valid_bits. A raw pair cannot exclude multiple wraps or establish unique
 * elapsed time. Spans shorter than the wrap period have an unambiguous delta.
 * tick_period_ns converts one tick to nanoseconds. clock_instance identifies
 * a process-local device clock. Vulkan guarantees ordering only within this
 * pair: matching instances do not establish an epoch across submissions.
 * Metal intervals share system mach time. Neither domain is calibrated to
 * the Run's steady clock.
 * Command intervals include command-buffer barriers, not CPU staging copies.
 * All fields are values and own no memory.
 */
typedef struct ps_kernel_device_timing_v8 {
  uint32_t clock_domain, valid_bits;
  uint64_t clock_instance, begin_tick, end_tick;
  double tick_period_ns;
} ps_kernel_device_timing_v8;

/** @brief One armed completion slot. The producer calls finish exactly once
 * after all accesses end and writes are visible, then stops using this slot.
 * finish copies the bounded status synchronously and allocates nothing. Inline
 * completion during submit is legal. It never invokes user observers.
 */
typedef struct ps_kernel_completion_v8 {
  void* context;
  ps_kernel_completion_key_v8 key;
  void (*finish)(void* context, ps_kernel_completion_key_v8 key,
                 const ps_kernel_status_v8* status);
  /** @brief Optional request and receiver for device timing.
   * Null disables collection. A provider may omit unavailable timing, or call
   * once with a valid record before finish, serially on the completion path.
   * Rejected submissions must not report timing.
   * The receiver copies synchronously without allocation or user callbacks.
   * Reporting does not complete work or release borrowing. A failed timing
   * query must not change an otherwise successful task's outcome.
   */
  void (*report_timing)(void* context, ps_kernel_completion_key_v8 key,
                        const ps_kernel_device_timing_v8* timing);
} ps_kernel_completion_v8;

/** @brief accepted=1 transfers borrowing until one terminal completion.
 * Rejection has accepted=0, non-OK status and no completion; the host retains
 * all owners. ticket is backend-local and independent of completion identity.
 * Accepted status is OK even if an inline completion reports task failure.
 */
typedef struct ps_kernel_submission_v8 {
  uint32_t accepted, reserved;
  uint64_t ticket;
  ps_kernel_status_v8 status;
} ps_kernel_submission_v8;

/** @brief CPU return establishes quiescence. Exceptions must not cross C ABI.
 */
typedef void (*ps_kernel_cpu_invoke_v8)(
    const ps_kernel_invocation_v8* invocation, ps_kernel_status_v8* status);
/** @brief GPU acceptance alone does not establish completion or visibility.
 * Both invocation and completion remain borrowed through finish. On uncertain
 * device quiescence, retain borrowing until the backend establishes it.
 */
typedef ps_kernel_submission_v8 (*ps_kernel_gpu_submit_v8)(
    const ps_kernel_invocation_v8* invocation,
    const ps_kernel_completion_v8* completion);

/** @brief One immutable preparation owner transferred to the host.
 * Exact struct_size/abi_version and at least one entry point are required.
 * context may be null. release is required for a nonnull context, runs exactly
 * once after all Runs/submissions drain, and must not throw. The host retains
 * the defining module until after release returns; callers share this owner
 * through host handles instead of copying and releasing this record twice.
 */
typedef struct ps_prepared_kernel_v8 {
  uint32_t struct_size, abi_version;
  const void* context;
  void (*release)(const void* context);
  ps_kernel_cpu_invoke_v8 invoke_cpu;
  ps_kernel_gpu_submit_v8 submit_gpu;
} ps_prepared_kernel_v8;

#ifdef __cplusplus
}
#endif
#endif /* INCLUDE_PHOTOSPIDER_PLUGIN_FIXED_INVOCATION_API_H_ */
