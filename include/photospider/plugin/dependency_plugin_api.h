#pragma once

#include "photospider/plugin/operation_plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS_DEPENDENCY_NEED_V8 16
#define PS_DEPENDENCY_RESOURCE_EXHAUSTED_V8 4
#define PS_DEPENDENCY_TYPE_MISMATCH_V8 5
#define PS_DEPENDENCY_INVALID_ARGUMENT_V8 6

/** @brief Exact rectangular sample subset in a rank-1..8 descriptor domain.
 * @note Unused array entries must be zero. Empty needs use run_count zero.
 */
typedef struct ps_dependency_run_v8 {
  uint32_t struct_size, rank;
  uint64_t offsets[8], extents[8];
} ps_dependency_run_v8;
/** @brief Non-spatial contract evidence; kind must be nonzero. */
typedef struct ps_dependency_tag_v8 {
  uint32_t struct_size, kind;
  uint64_t id;
} ps_dependency_tag_v8;
/** @brief Immutable callback-borrowed descriptor; no payload access. */
typedef struct ps_dependency_metadata_v8 {
  uint32_t struct_size, element_type, rank, facet_count;
  uint64_t shape[8];
  const ps_operation_facet_view_v8* facets;
} ps_dependency_metadata_v8;
/** @brief Pure static validation inputs, independent of Q and sample bytes. */
typedef struct ps_dependency_metadata_query_v8 {
  uint32_t struct_size, input_count, parameter_count, reserved;
  const ps_dependency_metadata_v8* inputs;
  const ps_operation_parameter_value_v8* parameters;
} ps_dependency_metadata_query_v8;
/** @brief Exact request, valid only for this callback.
 * @note Atomic contains one sample or full image pixel. Terminal requests
 * retain complete original Q; no pointer in this record may be retained.
 */
typedef struct ps_dependency_query_v8 {
  uint32_t struct_size, observation_kind, backend, output_count;
  ps_dependency_metadata_query_v8 metadata;
  ps_dependency_metadata_v8 output;
  const ps_dependency_run_v8* outputs;
} ps_dependency_query_v8;
/** @brief One exact per-output/port/role association, never a mere fetch union.
 * @note Atomic output_rank/coordinate must name the current observation (HW
 * for images). Terminal output_rank and all coordinates must be zero. Roles
 * use Data=1, Control=2, Validation=4, Descriptor=8. Runs and tags are copied.
 */
typedef struct ps_dependency_association_v8 {
  uint32_t struct_size, output_rank, port, roles;
  uint64_t output[8];
  uint32_t run_count, tag_count;
  const ps_dependency_run_v8* runs;
  const ps_dependency_tag_v8* tags;
} ps_dependency_association_v8;
/** @brief One borrowed authorized fragment with checked signed-stride layout.
 * @note data covers the retained allocation; access is authorized only within
 * region. A raw pointer conveys no observed-read proof. Use read for checked
 * sample access; retain_input is required for ownership across polls.
 */
typedef struct ps_dependency_fragment_v8 {
  uint32_t struct_size, rank, element_type, reserved;
  uint64_t shape[8], origin[8], offsets[8], extents[8];
  int64_t strides[8];
  uint64_t byte_offset, byte_size;
  const uint8_t* data;
} ps_dependency_fragment_v8;
/** @brief Borrowed completed state found in one checkpoint phase.
 * @note Initialize struct_size and zero reserved before lookup. A successful
 * miss sets handle, sequence and byte_size to zero. Nonzero handles expire at
 * poll return and are never reused within the invocation. Copy needed state
 * through checkpoint_read into accounted continuation/scratch bytes.
 */
typedef struct ps_dependency_checkpoint_v8 {
  uint32_t struct_size, reserved;
  uint64_t handle, sequence, byte_size;
} ps_dependency_checkpoint_v8;
/** @brief Read-only input and accounted scratch services for a pure block.
 * @note Borrowed until compute returns. There are no association, checkpoint,
 * retained-owner or output-publication services: discovery precedes the block.
 */
typedef struct ps_dependency_block_services_v8 {
  uint32_t struct_size, reserved;
  void* context;
  int (*read)(void*, uint32_t, const uint64_t*, uint32_t, void*, uint64_t);
  uint8_t* (*allocate_scratch)(void*, uint64_t);
  int (*consume_work)(void*, uint64_t);
  int (*is_cancelled)(void*);
} ps_dependency_block_services_v8;
/** @brief Computes complete outgoing state from copied incoming bytes.
 * @note Finite, nonblocking and pure: use only supplied inputs, incoming state,
 * phase/range/mode and registered static parameters/metadata. user may carry
 * only information already determined by those inputs or fixed implementation
 * constants; its address and extra configuration are not automatically keyed.
 * Each phase/mode identifies one fixed transition within the registered
 * implementation; a different algorithm needs a distinct phase/mode. All
 * carried controls and numeric state belong in incoming. Never depend on
 * original Q, mutable user state, timing or earlier unsupplied inputs. Write
 * every outgoing byte; no pointer/handle or uninitialized padding belongs in
 * either state. Pointers expire at return; host owns both buffers. Return
 * SUCCESS or an error, not NEED.
 */
typedef int (*ps_dependency_block_compute_v8)(
    const ps_dependency_block_services_v8*, const uint8_t* incoming,
    uint8_t* outgoing, uint64_t state_bytes, void* user);
/** @brief Finite, nonblocking phase services; every failure is sticky.
 * @note Service/context/input/scratch pointers expire at poll return. Output
 * pointers expire immediately on successful publish_output, or at poll return
 * if unpublished; never access an output through its pointer after publishing.
 * Handles are invocation-local, monotonic and never reused. Retained input
 * handles keep exact authorization and original ownership until release or
 * state destruction. There is no upstream execute or missing-sample fallback.
 * Boolean services return 1 on success, 0 on error. Allocators/retainers return
 * nonzero pointers/handles on success. fragment_count=0 may mean a valid empty
 * port; is_cancelled is an observation only. Every service error is sticky.
 */
typedef struct ps_dependency_services_v8 {
  uint32_t struct_size, reserved;
  void* context;
  int (*associate)(void*, const ps_dependency_association_v8*);
  int (*read)(void*, uint32_t, const uint64_t*, uint32_t, void*, uint64_t);
  uint32_t (*fragment_count)(void*, uint32_t);
  int (*fragment)(void*, uint32_t, uint32_t, ps_dependency_fragment_v8*);
  uint64_t (*retain_input)(void*, uint32_t, uint32_t);
  int (*read_owner)(void*, uint64_t, const uint64_t*, uint32_t, void*,
                    uint64_t);
  int (*release_owner)(void*, uint64_t);
  uint8_t* (*allocate_output)(void*, const ps_dependency_run_v8*, uint64_t*);
  int (*publish_output)(void*, uint64_t);
  uint8_t* (*allocate_scratch)(void*, uint64_t);
  int (*consume_work)(void*, uint64_t);
  int (*is_cancelled)(void*);
  /** @brief Finds the greatest completed sequence <= before in an algorithm
   * phase, importing its full successful witness. Never waits for computation.
   * @note Boolean success includes a miss with result.handle=0. Pure Atomic
   * programs only; terminal use or ignored service errors remain sticky.
   */
  int (*checkpoint_before)(void*, uint32_t phase, uint64_t before,
                           ps_dependency_checkpoint_v8* result);
  /** @brief Copies an exact byte interval of a phase-local checkpoint handle.
   * @note Requires nonnull destination, positive size and an in-bounds
   * interval. Reads are charged before copying; no raw borrowed state pointer
   * is exposed.
   */
  int (*checkpoint_read)(void*, uint64_t handle, uint64_t offset, void*,
                         uint64_t);
  /** @brief Copies and publishes complete algorithm state under phase/sequence.
   * @note State must be nonnull with positive byte_size and contain only value
   * bytes, never pointers/handles or uninitialized padding. It includes all
   * numeric mode/control state required to continue the deterministic
   * algorithm. The copy uses the current stage allocator/workspace; publish may
   * decline optional retention while returning success. The caller retains its
   * source bytes; no later mutation can change the host copy. No failures are
   * stored.
   */
  int (*checkpoint_publish)(void*, uint32_t phase, uint64_t sequence,
                            const uint8_t* state, uint64_t byte_size);
  /** @brief Evaluates a pure Atomic block using the host's optional state LRU.
   * @note Copies incoming before compute, so incoming/outgoing caller buffers
   * may overlap. They must be nonnull and state_bytes positive. Output is
   * copied only on success. The operation must reserve workspace for two state
   * copies plus any scratch. Host keys include actual incoming/input bytes and
   * static implementation identity; failures are sticky and never cached. A hit
   * skips compute while retaining current input evidence. No output batching
   * occurs.
   */
  int (*block)(void*, uint32_t phase, uint64_t begin, uint64_t end,
               uint64_t mode, const uint8_t* incoming, uint64_t state_bytes,
               uint8_t* outgoing, ps_dependency_block_compute_v8 compute,
               void* user);
} ps_dependency_services_v8;
/** @brief Copied staged callbacks for trusted in-process C implementations.
 * @note Exactly one synchronous execute or dependency_program is supplied.
 * state_bytes are zero-initialized host bytes, stable through destroy; the host
 * adds its adapter overhead to the published continuation bound. Never free
 * state. destroy runs once whenever start was entered, including failed starts,
 * and must tolerate partially initialized zeroed state. No callback may throw.
 * validate is optional and runs for Empty requests and compilation; it must be
 * pure and inspect only metadata/parameters. Empty skips start/poll/destroy.
 * start/validate return SUCCESS or an error code. poll returns NEED (with only
 * associations), SUCCESS (with complete published fragments), or an error.
 * Ordinary error codes reuse ps_operation_result_v8 plus the codes above.
 * Unknown callback result codes map to OperationFailed. reserved must be zero;
 * state_bytes and maximum_stages are 1..1048576, maximum_retained_owners is
 * 0..65536. Caller execution limits may impose smaller bounds.
 * All inputs/outputs preserve image-v2 full-channel closure.
 */
typedef struct ps_dependency_program_v8 {
  uint32_t struct_size, maximum_stages, maximum_retained_owners, reserved;
  uint64_t state_bytes;
  int (*validate)(const ps_dependency_metadata_query_v8*, void*);
  int (*start)(const ps_dependency_query_v8*, void*, uint64_t, void*);
  int (*poll)(const ps_dependency_query_v8*, void*,
              const ps_dependency_services_v8*, void*);
  void (*destroy)(void*, void*);
} ps_dependency_program_v8;

#ifdef __cplusplus
}
#endif
