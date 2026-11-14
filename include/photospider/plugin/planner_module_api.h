#pragma once
#include "photospider/plugin/operation_module_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Borrowed seeded IR call and its physical input/output value IDs. */
typedef struct ps_plan_call_v1 {
  uint64_t source_call;
  uint32_t block, ordinal, input_count, output_count;
  ps_string_v8 operation;
  const uint32_t *inputs, *outputs;
} ps_plan_call_v1;
typedef struct ps_plan_access_v1 {
  uint32_t port, value, member, domain;
  uint64_t tile;
} ps_plan_access_v1;
typedef struct ps_plan_task_v1 {
  uint32_t struct_size, call, kind, invocation_abi;
  ps_string_v8 symbol, contract;
  ps_placement_v8 placement;
  uint64_t scratch[3];
  const ps_plan_access_v1 *reads, *writes;
  uint32_t read_count, write_count;
  const uint32_t *schema_reads, *predecessors;
  uint32_t schema_read_count, predecessor_count;
  const uint8_t* parameters;
  uint32_t parameter_bytes;
} ps_plan_task_v1;
/** @brief Synchronous planning session. Seeded CFG, operations and schemas
 * belong to the host. append_task creates canonical tile layouts. finalize
 * and validation always run in the host before immutable publication.
 */
typedef struct ps_plan_bytes_v1 {
  const char* data;
  uint64_t size;
  void* owner;
  void (*release)(void*);
} ps_plan_bytes_v1;
typedef ps_kernel_status_v8 (*ps_plan_interval_fn_v1)(void*, ps_string_v8 name,
                                                      uint32_t output,
                                                      uint32_t member,
                                                      uint64_t begin,
                                                      uint64_t end);
typedef struct ps_planner_host_v1 {
  uint32_t struct_size, abi_version, ir_form, execution_mode;
  uint64_t tile_extent, maximum_tasks, maximum_records, maximum_edges,
      maximum_work;
  uint32_t call_count, value_count, block_count;
  void* context;
  ps_kernel_status_v8 (*call)(void*, uint32_t, ps_plan_call_v1*);
  ps_kernel_status_v8 (*grid)(void*, uint32_t value, uint32_t member,
                              ps_tile_grid_v8*);
  ps_kernel_status_v8 (*request)(void*, uint32_t block, uint32_t output,
                                 uint32_t member, uint64_t tile,
                                 uint32_t* selected);
  ps_kernel_status_v8 (*append_task)(void*, const ps_plan_task_v1*,
                                     uint32_t* id);
  /* Callback-local owner for physical table text. The representation is the
   * ordered physical block of format 5, without a file or artifact header. */
  ps_kernel_status_v8 (*seed)(void*, ps_plan_bytes_v1*);
  ps_kernel_status_v8 (*selection)(void*, uint32_t block, void*,
                                   ps_plan_interval_fn_v1);
  ps_kernel_status_v8 (*input_domain)(void*, uint32_t declaration,
                                      uint32_t* domain);
  ps_kernel_status_v8 (*traits)(void*, uint32_t call, uint32_t* pure,
                                uint32_t* gpu);
  ps_kernel_status_v8 (*lower)(void*, uint32_t call, const ps_tile_request_v8*,
                               uint32_t count, uint32_t backend,
                               ps_plan_bytes_v1*);
  ps_kernel_status_v8 (*publish)(void*, const char* physical, uint64_t bytes);
  ps_kernel_status_v8 (*charge)(void*, uint64_t work);
  ps_kernel_status_v8 (*export_value)(void*, uint32_t call, uint32_t value);
} ps_planner_host_v1;
typedef struct ps_planner_module_v1 {
  uint32_t struct_size, abi_version, ir_forms;
  ps_string_v8 name, version, description;
  void* context;
  ps_kernel_status_v8 (*plan)(void*, const ps_planner_host_v1*);
  void (*destroy)(void*);
} ps_planner_module_v1;
PS_OPERATION_MODULE_EXPORT const ps_planner_module_v1*
ps_planner_module_get_api_v1(void);
#ifdef __cplusplus
}
#endif
