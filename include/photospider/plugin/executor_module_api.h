#pragma once
#include "photospider/plugin/operation_module_api.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PS_EXECUTOR_MODULE_ABI_VERSION_2 2U
#define PS_EXECUTOR_TASK_NONE_V2 UINT32_MAX
#define PS_EXECUTOR_CALL_SUCCEEDED_V2 0U
#define PS_EXECUTOR_CALL_FAILED_V2 1U
#define PS_EXECUTOR_CALL_ELIDED_V2 2U

/** @brief Opaque execution owner. A successful factory supplies exactly one
 * release. Partial owners on failure also transfer to the caller. */
typedef struct ps_executor_owner_v1 {
  void* object;
  void (*release)(void*);
} ps_executor_owner_v1;
struct ps_executor_module_v2;
typedef struct ps_executor_diagnostics_v1 {
  uint32_t struct_size;
  uint64_t submitted_tasks, completed_tasks, retired_calls, activations;
} ps_executor_diagnostics_v1;
typedef struct ps_executor_task_timing_v1 {
  uint64_t activation, submission, source_call;
  uint32_t block, task, call, backend, status;
  uint64_t ready_ns, submit_ns, cpu_start_ns, completion_ns, consumed_ns,
      copy_bytes, lane_wait_ns, context_wait_ns, coordinator_delay_ns;
  ps_kernel_device_timing_v8 device;
} ps_executor_task_timing_v1;
typedef struct ps_executor_call_timing_v1 {
  uint64_t activation, sequence, source_call, retired_ns;
  uint32_t call, outcome;
} ps_executor_call_timing_v1;
typedef struct ps_executor_result_builder_v1 {
  ps_executor_owner_v1 owner;
  ps_kernel_status_v8 (*publish)(void*, uint32_t member,
                                 const ps_mapping_region_v1*,
                                 const uint8_t* bytes, uint64_t byte_count);
  ps_kernel_status_v8 (*seal)(void*, ps_executor_owner_v1* result);
} ps_executor_result_builder_v1;
/** @brief Borrowed host-bound Plan. A prepared wrapper retains the Plan for
 * the lifetime of the returned preparation. IDs refer to immutable tables. */
typedef struct ps_executor_kernel_v1 {
  ps_executor_owner_v1 owner;
  ps_kernel_status_v8 (*invoke_cpu)(void*, ps_kernel_invocation_v8*);
  ps_kernel_status_v8 (*invoke_cpu_v9)(void*, ps_kernel_invocation_v9*);
  ps_kernel_submission_v8 (*submit_gpu)(void*, ps_kernel_invocation_v8*,
                                        const ps_kernel_completion_v8*);
} ps_executor_kernel_v1;
typedef struct ps_executor_task_v1 {
  uint32_t kind, call, abi, backend, domain, read_count, write_count,
      dependency_count, schema_read_count;
  ps_string_v8 symbol, contract, profile, queue;
  const uint32_t *writes, *dependencies, *schema_reads;
  const uint8_t* parameters;
  uint64_t parameter_bytes, scratch[3];
} ps_executor_task_v1;
typedef struct ps_executor_tile_v1 {
  uint32_t value, member, domain, producer;
  uint64_t index, bytes, offset, alignment;
  ps_kernel_layout_v8 layout;
} ps_executor_tile_v1;
typedef struct ps_executor_plan_v1 {
  uint32_t struct_size, physical_version, execution_mode, tasks, inputs,
      outputs;
  uint64_t tile_extent;
  const void* handle;
  ps_kernel_status_v8 (*input)(const void*, uint32_t, ps_string_v8*);
  ps_kernel_status_v8 (*output)(const void*, uint32_t, ps_string_v8*);
  ps_kernel_status_v8 (*task)(const void*, uint32_t, uint32_t* kind,
                              uint32_t* backend, uint32_t* domain,
                              uint32_t* invocation_abi);
  ps_kernel_status_v8 (*task_info)(const void*, uint32_t, ps_executor_task_v1*);
  ps_kernel_status_v8 (*read)(const void*, uint32_t task, uint32_t slot,
                              uint32_t* port, uint32_t* tile,
                              ps_kernel_layout_v8* layout);
  ps_kernel_status_v8 (*tile)(const void*, uint32_t tile, ps_executor_tile_v1*);
  /* Read the versioned physical tables, including CFG, calls and returns.
   * The host owns the text until the caller releases the returned owner. */
  ps_kernel_status_v8 (*tables)(const void*, ps_string_v8*,
                                ps_executor_owner_v1*);
} ps_executor_plan_v1;
/** @brief Caller-owned sink views. The named result handle is borrowed during
 * emit. retain_result creates an independent reference owned by the host. */
typedef struct ps_executor_failure_v1 {
  uint32_t origin, scope, flags, task, output_index, rank;
  uint64_t node, input, sequence, coordinate[8];
} ps_executor_failure_v1;
typedef struct ps_executor_results_v1 {
  void* context;
  ps_kernel_status_v8 (*emit)(void*, ps_string_v8, const void* result);
  void* diagnostics;
  ps_executor_failure_v1* failure;
  void*
      result_owner; /* Host-private move destination for the linked provider. */
} ps_executor_results_v1;
/** @brief Borrowed Run services; retain extends this record's handle lifetime.
 * retire names a call and source_call from the submitted Plan. Failed calls
 * supply a non-OK status, a task belonging to that call, and no values.
 * Succeeded/Elided calls supply OK and PS_EXECUTOR_TASK_NONE_V2; Elided
 * supplies no values. Success supplies exactly the physical value IDs declared
 * in the call's exports with their sealed schemas and compiled tile coverage.
 * Task ID zero is valid. Invalid envelopes fail even without an observer. The
 * observer receives failed_task and its derived task/node detail. Provider
 * callbacks must finish observer delivery before join/close returns.
 */
typedef struct ps_executor_submit_v2 {
  uint32_t struct_size, input_count, parallelism, trace_capacity;
  uint64_t maximum_work, maximum_stages, maximum_block_activations;
  const void* handle;
  ps_kernel_status_v8 (*input)(const void*, uint32_t, ps_string_v8*,
                               const void** result);
  uint32_t (*cancelled)(const void*);
  ps_kernel_status_v8 (*retain)(const void*, ps_executor_owner_v1*);
  ps_kernel_status_v8 (*retire)(const void*, uint64_t sequence,
                                uint64_t activation, uint64_t source_call,
                                uint32_t call, uint32_t outcome,
                                uint32_t failed_task,
                                ps_kernel_status_v8 status,
                                const uint32_t* values,
                                const void* const* results, uint32_t count);
} ps_executor_submit_v2;
/** @brief Pure C service returned by query_extension for
 * PS_EXECUTOR_VALUE_RESULTS_V1 and revision "1". The table is borrowed for
 * the provider lifetime. make_result creates a builder in the host's resource
 * root using the schema of any value in the bound Plan. This includes call
 * exports with no named final output. An invalid ID returns INVALID_ARGUMENT.
 * Each successful call transfers an independent builder owner to the caller.
 */
#define PS_EXECUTOR_VALUE_RESULTS_V1 "photospider.executor.value-results"
typedef struct ps_executor_value_results_v1 {
  uint32_t struct_size;
  ps_kernel_status_v8 (*make_result)(const void* host,
                                     const ps_executor_plan_v1* plan,
                                     uint32_t value,
                                     ps_executor_result_builder_v1* builder);
} ps_executor_value_results_v1;
typedef struct ps_executor_host_v1 {
  uint32_t struct_size, abi_version;
  const void* handle;
  uint32_t cpu_workers, maximum_queued_tasks, maximum_gpu_submissions,
      maximum_runs;
  uint64_t maximum_live_bytes, tile_cache_bytes;
  ps_kernel_status_v8 (*retain_result)(const void*, ps_executor_owner_v1*);
  ps_kernel_status_v8 (*read_sample)(const void* result, uint32_t member,
                                     const uint64_t* coordinate, uint32_t rank,
                                     void* bytes, uint32_t length);
  ps_kernel_status_v8 (*make_result)(const void* host,
                                     const ps_executor_plan_v1* plan,
                                     ps_string_v8 name,
                                     ps_executor_result_builder_v1* builder);
  /* The returned lease protects a provider release callback through its return
   * into host code. Destroying this owner calls the callback exactly once. */
  ps_kernel_status_v8 (*lease)(const void* host, void* object,
                               void (*release)(void*), ps_executor_owner_v1*);
  void (*report_diagnostics)(void* destination,
                             const ps_executor_diagnostics_v1*);

  ps_kernel_status_v8 (*prepare_kernel)(
      const void* host, const ps_executor_plan_v1*, uint32_t task,
      const struct ps_native_gpu_service_v2* native, ps_executor_kernel_v1*);
  void (*write_task_timing)(void* destination, uint64_t capacity,
                            uint64_t index, const ps_executor_task_timing_v1*);
  void (*write_call_timing)(void* destination, uint64_t capacity,
                            uint64_t index, const ps_executor_call_timing_v1*);
  /** Return a borrowed service for the exact name and version. The physical
   * C++ SDK uses its build ID as the version. An unsupported name or version
   * returns TYPE_MISMATCH with a null service. */
  ps_kernel_status_v8 (*query_extension)(const void* host, ps_string_v8 name,
                                         ps_string_v8 version,
                                         const void** service);
} ps_executor_host_v1;
/** @brief Reentrant provider. close drains context work and observer delivery.
 * prepare and submit transfer owners. observe returns an independent owner.
 * Details of the built-in diagnostic sink are opaque to foreign modules.
 */
typedef struct ps_executor_module_v2 {
  uint32_t struct_size, abi_version, physical_version, backend_mask,
      domain_mask, invocation_mask;
  ps_string_v8 name;
  void* context;
  ps_kernel_status_v8 (*create)(void*, const ps_executor_host_v1*,
                                ps_executor_owner_v1*);
  void (*close)(void*);
  ps_kernel_status_v8 (*prepare)(void*, const ps_executor_plan_v1*,
                                 ps_executor_owner_v1*);
  ps_kernel_status_v8 (*submit)(void*, void*, const ps_executor_submit_v2*,
                                ps_executor_owner_v1*);
  uint32_t (*cancel)(void*);
  ps_kernel_status_v8 (*join)(void*, const ps_executor_results_v1*);
  ps_kernel_status_v8 (*observe)(void*, ps_executor_owner_v1*);
  void (*snapshot)(void*, void* diagnostics);
  uint64_t (*trace)(void*, uint32_t kind, void* destination, uint64_t count);
  uint32_t (*release_cache)(void*);
  uint32_t (*workers)(void*);
  ps_kernel_status_v8 (*description)(void*, void*,
                                     void (*write)(void*, ps_string_v8));
  void (*destroy)(void*);
} ps_executor_module_v2;
PS_OPERATION_MODULE_EXPORT const ps_executor_module_v2*
ps_executor_module_get_api_v2(void);
#ifdef __cplusplus
}
#endif
