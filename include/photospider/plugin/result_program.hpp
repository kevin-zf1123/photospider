#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/atom_key.hpp"
#include "photospider/data/fragment_atlas.hpp"
#include "photospider/data/resource_set.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/tiles.hpp"
#include "photospider/plugin/cpu_parallel_api.h"
#include "photospider/plugin/cpu_tiles_api.h"
#include "photospider/plugin/native_gpu_api.h"
#include "photospider/plugin/operation_types.hpp"

namespace ps {
class PreparedOperation;
namespace execution_internal {
class ResultCacheProof;
template <class Producer>
struct StructuredNeedCursor;
}  // namespace execution_internal
/** @brief Execution-scoped limits for structured Result continuations.
 * These bounds are separate from compile-time traits and Root resource limits.
 */
struct DependencyLimits final {
  FootprintLimits sets;
  /** @brief GPU selection work budget per static unit, accumulated across
   * polls.
   */
  std::uint64_t maximum_work = 1048576;
  /** @brief Maximum state allocation for a staged Result continuation. */
  std::uint64_t maximum_state_bytes = 1048576;
  /** @brief Maximum poll transitions for one continuation activation. */
  std::uint32_t maximum_stages = 4096;
  /** @brief Maximum slots in one GPU discovery table, 0 disables discovery.
   * @note Host hard limit is 65536; work and live-byte bounds also apply.
   */
  std::uint32_t maximum_gpu_requests = 65536;
};

namespace plugin_internal {
class FailureLatch;
class ResultPayloadBound;
}  // namespace plugin_internal
/** @brief Compiler-owned immutable input/output metadata for a staged result.
 * Prepared before execution; it contains no runtime allocation or callback.
 */
struct ResultProgramMetadata final {
  std::vector<OperationMetadata> inputs;
  OperationMetadata output;
  /** @brief Compiled member grids in input-port order; empty for direct hosts.
   */
  std::vector<std::vector<TileGrid>> input_tile_grids = {};
  std::vector<TileGrid> output_tile_grids = {};
};
/** @brief Borrowed static query for one structured Result continuation.
 * It identifies the selected Result output and optional tensor-slot demand.
 * `page_bytes` is a physical work-window choice, excluded from semantic_key.
 */
struct ResultProgramQuery final {
  ResultProgramQuery(
      const ResultProgramMetadata& metadata,
      const std::map<std::string, ParameterValue>& static_parameters)
      : inputs(metadata.inputs),
        output(metadata.output),
        parameters(static_parameters),
        input_tile_grids(metadata.input_tile_grids),
        output_tile_grids(metadata.output_tile_grids) {}
  const std::vector<OperationMetadata>& inputs;
  const OperationMetadata& output;
  const std::map<std::string, ParameterValue>& parameters;
  /** @brief Borrowed compiled grids for SDK coordinate conversion. */
  const std::vector<std::vector<TileGrid>>& input_tile_grids;
  const std::vector<TileGrid>& output_tile_grids;
  /** @brief Exact complete selected member tile; absent for a shared stage.
   * The executor supplies the footprint stored in the Plan instruction. It does
   * not authorize input reads. Tile identity includes member and local index.
   */
  std::optional<Footprint> tensor_outputs;
  std::uint32_t tensor_slot = 0;
  std::uint64_t tile_extent = 128;
  /** @brief Selected local tile or shared publication-unit index. */
  TileIndex tile_unit = 0;
  Backend backend = Backend::Cpu;
  std::string_view semantic_key;
  std::uint32_t output_index = 0;
  /** @brief UINT32_MAX for a tile; otherwise the prepared shared stage index.
   * Stage factories identify the contract with this index. Every shared poll
   * receives the current compiled tensor_slot/tile_unit; output_index is
   * routing metadata only for a stage. */
  std::uint32_t shared_stage = UINT32_MAX;
  std::uint64_t page_bytes = 4096;
  CancellationToken cancellation;
  /** @brief Immutable resources explicitly selected by static operation
   * preparation. The continuation retains the admitted set independently of
   * this query.
   */
  ResourceSet resources = {};
  /** @brief Owning immutable metadata/static-parameter preparation. Compiled
   * starts preserve and validate this handle; direct starts prepare once when
   * absent. Continuations retain it for all polls and concurrent plan reuse.
   */
  std::shared_ptr<const PreparedOperation> prepared;
};
/** @brief Owning capability restricted to a compiled tensor grant.
 * The capability retains its source Result, descriptor and granted samples.
 * Reads and acquired windows remain restricted to those grants.
 */
class PHOTOSPIDER_API ResultTensorInput final {
 public:
  ResultTensorInput() = default;
  std::uint64_t object_id() const noexcept { return result_.object_id(); }
  const ResultTensorSpec& spec() const {
    return (static_schema_ ? *static_schema_ : result_.schema())
        .tensors.at(slot_);
  }
  const Footprint& coverage() const noexcept { return samples_; }
  Status read(const std::vector<std::uint64_t>& coordinate, void* destination,
              std::size_t bytes,
              const CancellationToken& cancellation = {}) const;

  /** @brief Acquire an exact rectangle inside the compiled tensor grant.
   * Acquisition retains the authorized immutable fragments without copying
   * samples. Unauthorized access records a sticky protocol failure. The
   * returned window owns immutable backing independently of this phase.
   */
  Result<ResultTensorReadWindow> acquire(
      const Region& region, const CancellationToken& cancellation = {}) const;

 private:
  friend class execution_internal::StructuredExecution;
  friend class execution_internal::ResultCacheProof;
  template <class>
  friend struct execution_internal::StructuredNeedCursor;
  friend class FragmentAtlasPlan;
  friend struct ResultProgramPhase;
  friend class plugin_internal::ResultPayloadBound;
  Status read_granted(const std::vector<std::uint64_t>& coordinate,
                      void* destination, std::size_t bytes,
                      const CancellationToken& cancellation) const;
  ResultRef result_;
  std::shared_ptr<const SchemaTemplate> static_schema_;
  ResultDescriptor descriptor_;
  std::optional<ResourceBudget> resources_;
  std::uint32_t slot_ = 0;
  bool payload_authorized_ = false;
  Footprint samples_;
  std::shared_ptr<std::atomic<ErrorCode>> failure_;
  std::shared_ptr<plugin_internal::FailureLatch> observer_;
};
/** @brief Assertion of already compiled shared-stage read capabilities.
 * An absent selection requests all static members. An explicit empty selection
 * grants no payload. Only compiled tiles are
 * exposed; inherited evidence grants no original-input access.
 */
struct ResultSharedNeed final {
  ResultSharedNeed(std::uint32_t stage = 0,
                   std::optional<TileRequest> tiles = {})
      : stage(stage), tiles(std::move(tiles)) {}  // NOLINT(runtime/explicit)
  std::uint32_t stage;
  std::optional<TileRequest> tiles;
};
/** @brief Optional assertion of static member, stage or descriptor grants.
 * Inputs are ready before the operation starts. Returning this envelope only
 * validates that every request is within the instruction's compiled grants;
 * it never schedules work. Each TileNeed has exactly one request kind and
 * addresses the compiled input grid. An empty payload selection is a no-op.
 */
struct ResultProgramNeed final {
  ResourceVector<TileNeed> tiles = {};
  /** @brief Prepared stages requested by this poll; no blocking invocation. */
  ResourceVector<ResultSharedNeed> shared_stages = {};
};
/** @brief Pure compilation query for one tile or shared publication unit.
 * Contains fixed metadata and preparation only. Shared units use publication
 * and tensor_slot/tile_unit explicitly; runtime sample values are unavailable.
 * Mappers must be deterministic and may not perform I/O or start continuations.
 */
struct ResultStaticQuery final {
  const ResultProgramMetadata& metadata;
  const std::map<std::string, ParameterValue>& parameters;
  const PreparedOperation& prepared;
  Backend backend = Backend::Cpu;
  std::uint64_t tile_extent = 128;
  std::uint32_t output_index = 0;
  std::uint32_t shared_stage = UINT32_MAX;
  std::uint32_t tensor_slot = 0;
  TileIndex tile_unit = 0;
  TileRequest publication;
  std::optional<Footprint> tensor_outputs;
};
/** @brief Owned static reads. The planner resolves and validates every entry.
 * Overlapping entries are merged before scheduling. No callback may extend
 * these grants at execution time. Data-dependent algorithms must declare a
 * finite conservative coverage or express selection using IR control flow.
 */
struct ResultStaticMapping final {
  std::vector<TileNeed> inputs;
  std::vector<ResultSharedNeed> shared_stages;
};
// NOLINTBEGIN(whitespace/indent_namespace)
using OperationDependencyMapper =
    std::function<Result<ResultStaticMapping>(const ResultStaticQuery&)>;
// NOLINTEND
/** @brief Baseline eager mapping for an operation's complete selected inputs.
 * Uses the prepared input projection of the selected output or shared stage.
 * Shared-stage reads must be declared explicitly by a custom mapper; merely
 * declaring a stage never creates an edge to it.
 * This is an explicit conservative contract; supply a custom mapper for local
 * reads, segmented shared dependencies or operations requiring lazy selection.
 */
PHOTOSPIDER_API Result<ResultStaticMapping> complete_input_dependencies(
    const ResultStaticQuery& query);

/** @brief Bounded table borrowed only during synchronous GPU discovery.
 * Layout is a 16-byte header and capacity 32-byte records containing uint32
 * input/kind/tensor_slot/reserved and uint64 begin/end. Kind 0 requests a
 * half-open interval; kind 1 requests a descriptor and has slot/bounds zero.
 * Reserved fields are zero. The host decodes after submitted work drains.
 */
struct ResultGpuRequestTable final {
  std::uint8_t* bytes = nullptr;
  std::uint64_t byte_size = 0;
  std::uint32_t capacity = 0;
};
/** @brief Immutable GPU-selected subset of the instruction's fixed grants.
 * The receipt retains no source payload and grants no additional reads.
 * All selected input tiles are already ready; computation may continue in the
 * same poll. Producer discovery and tile mapping belong to compilation.
 */
struct ResultDiscoveryReceipt final {
  ResourceVector<TileNeed> tiles;
};
/** @brief Candidate prefix or completion returned by a continuation.
 * The host validates publication scopes before exposing them to consumers.
 */
struct ResultPublication final {
  ResultRef result;
  bool complete = false;
};
// NOLINTBEGIN(whitespace/indent_namespace)
using ResultProgramPoll = std::variant<ResultProgramNeed, ResultPublication>;
using ResultObjectInputs =
    std::map<std::uint32_t, ResultRef, std::less<std::uint32_t>,
             ResourceAllocator<std::pair<const std::uint32_t, ResultRef>>>;
/** @brief Metadata snapshots with no payload read capability. */
using ResultDescriptorInputs = std::map<
    std::uint32_t, ResultDescriptor, std::less<std::uint32_t>,
    ResourceAllocator<std::pair<const std::uint32_t, ResultDescriptor>>>;
using ResultTensorInputs = std::map<
    std::pair<std::uint32_t, std::uint32_t>, ResultTensorInput,
    std::less<std::pair<std::uint32_t, std::uint32_t>>,
    ResourceAllocator<std::pair<const std::pair<std::uint32_t, std::uint32_t>,
                                ResultTensorInput>>>;
// NOLINTEND
/** @brief Owning handle to a published state for a pure Result continuation.
 * The handle retains its state Result and dependency witness independently of
 * the checkpoint index. `phase()` and `sequence()` identify the ordering key;
 * `state()` exposes the owning state Result. Accessors throw `logic_error` on
 * an invalid handle.
 */
class PHOTOSPIDER_API ResultCheckpoint final {
 public:
  ResultCheckpoint() = default;
  bool valid() const noexcept { return state_.valid() && witness_ != nullptr; }
  std::uint32_t phase() const;
  std::uint64_t sequence() const;
  const ResultRef& state() const;

 private:
  friend class execution_internal::StructuredExecution;
  ResultRef state_;
  std::uint32_t phase_ = 0;
  std::uint64_t sequence_ = 0;
  std::shared_ptr<const void> witness_;
};
/** @brief Ready inputs/windows borrowed only until this poll returns.
 * Root resource operations are explicit admission. Tensor publication and reads
 * use immutable Root-memory backing. `read_tensor()` and `consume_work()`
 * failures cannot be ignored into success.
 */
struct PHOTOSPIDER_API ResultProgramPhase final {
  const ResultProgramQuery& query;
  /** @brief Direct invocation objects; bounded tensor capabilities authorize
   * reads. Pure tensor payloads use tensors. Descriptor-only requests expose
   * metadata through descriptors and never add a payload capability to this
   * map.
   */
  const ResultObjectInputs& results;
  const BufferAllocator& allocator;
  const ResourceBudget& resources;
  std::function<Status(std::uint64_t)> consume_work;
  std::shared_ptr<std::atomic<ErrorCode>> failure;
  /** @brief Host-provided first-failure observer; callbacks must not retain it.
   */
  std::function<void(const Status&)> failure_observer = {};
  /** @brief Owning synchronized first-failure state for retained capabilities.
   * It retains no phase, actor, services or context pointer. Workers may report
   * complete error provenance through the capability owning this latch.
   */
  std::shared_ptr<plugin_internal::FailureLatch> failure_latch = {};
  /** @brief Records a non-Ok callback failure before further allocation.
   * Preserves first-failure provenance through the host latch and observer.
   * Observer exceptions cannot replace the recorded cause. Call on the entry
   * thread while this borrowed phase is alive; Ok is a no-op.
   */
  void record_failure(const Status& status) const;
  const ResultTensorInputs* tensors = nullptr;
  /** @brief Certified shared publications admitted by current read grants,
   * keyed by prepared stage index.
   * Owning Results authorize their own bytes only. Frozen producer dependencies
   * are inherited by this publication without granting original input reads. */
  const ResultObjectInputs* shared_stages = nullptr;
  /** @brief Captured static facts for descriptor-only inputs.
   * Entries own metadata only. They cannot be used to read tensors;
   * payload access requires an independently supplied payload capability.
   */
  const ResultDescriptorInputs* descriptors = nullptr;
  const ps_cpu_parallel_service_v2* cpu_parallel = nullptr;
  const ps_cpu_tiles_service_v2* cpu_tiles = nullptr;
  const ps_gpu_service_v1* gpu = nullptr;
  /** @brief Borrowed host status reader for the current native invocation.
   * Read on the entry thread after a GPU service call to preserve specific
   * errors that the numeric GPU ABI reports through its general failure code.
   * Must not be retained beyond this phase. */
  std::function<Status()> gpu_status = {};
  /** @brief Report opaque bytes for a type declared in operation diagnostics.
   * Borrowed during this synchronous entry-thread call; errors remain sticky.
   */
  std::function<Status(std::uint32_t, ByteView)> report_diagnostic = {};
  /** @brief Find the greatest saved sequence <= `before` in one checkpoint
   * phase. A miss, including optional cache/capacity exhaustion, returns an
   * empty optional. A checkpoint retains state only. Each invocation uses its
   * current compiled tensor capabilities and current dependency evidence;
   * restoring state cannot enlarge either.
   */
  std::function<Result<std::optional<ResultCheckpoint>>(std::uint32_t,
                                                        std::uint64_t)>
      checkpoint_before = {};
  /** @brief Retain a sealed state Result under a nonzero phase and an ordered
   * sequence, which may be zero.
   * Retention requires a pure deterministic operation and pure selected-input
   * producer closure. Optional cache or capacity refusal declines retention
   * without failing an otherwise successful operation. Protocol, cancellation,
   * and admitted Run-work failures remain execution failures.
   */
  std::function<Status(std::uint32_t, std::uint64_t, const ResultRef&)>
      checkpoint_publish = {};
  /** @brief Evaluate a Result block from an explicit incoming state.
   * `kind`, `[begin,end)`, and `mode` identify the operation phase and range.
   * The incoming and computed states must share a schema and Root, and each
   * must be a sealed Result with one fully covered tensor. Carry every control
   * needed by a later block in that tensor. Content caching is optional and
   * applies only to cacheable operations with a pure selected-input producer
   * closure. Its key uses the operation contract, range, mode, incoming state
   * schema/coverage/raw bits, and currently supplied Tensor coverage, metadata,
   * and raw bits; it excludes output demand and ObjectIds.
   * When the operation opts into cross-output sharing, its namespace also
   * includes all resolved output contracts, backend, execution mode, static
   * parameters and actual input metadata. The selected output index and
   * output-specific metadata are not added separately, so a shared computation
   * must be output independent or encode the difference in state or mode.
   * Optional content caching skips resource-bearing incoming states, supplied
   * Tensor inputs, and computed states. Those states remain valid for
   * computation; computed states with resources are not packed into the
   * internal TensorSpan-backed block-state LRU.
   * The compute callback may depend only on key-covered logical schemas,
   * coverage, raw bits, and static metadata/parameters plus phase, range, and
   * mode. It must not depend on Result ObjectIds, `semantic_key`,
   * the original output demand, or earlier windows not supplied to this call;
   * those provenance facts are deliberately outside the content key. A cache
   * hit publishes a fresh state Result without old dependency evidence.
   * The current instruction's compiled dependency evidence remains in
   * effect. Public output publication, checkpoints and completed-result caches
   * remain independent of block-state reuse. Optional cache-work exhaustion
   * skips caching and computes normally.
   */
  std::function<Result<ResultRef>(std::uint32_t, std::uint64_t, std::uint64_t,
                                  std::uint64_t, const ResultRef&,
                                  const std::function<Result<ResultRef>()>&)>
      block = {};
  /** @brief Run bounded synchronous GPU selection within fixed input grants.
   * Capacity and candidates must be positive. Capacity bounds emitted records;
   * candidates bounds emit attempts. The
   * callback receives a temporary zeroed Root table and must submit actual
   * native GPU work. The host freezes, validates, groups, and returns its
   * immutable footprint receipt. Capacity is limited by the hard 65536 record
   * ceiling, `maximum_gpu_requests`, and dependency `maximum_boxes` bound.
   * The decoded receipt must be a subset of the instruction's compiled grants.
   * It never creates dependencies or changes read authorization. Work is
   * charged against the unit's selection budget across polls, the Run budget,
   * and Root resources. Checkpoint, block and nested selection are forbidden
   * inside the synchronous entry-thread callback.
   */
  std::function<Result<std::shared_ptr<const ResultDiscoveryReceipt>>(
      std::uint32_t, std::uint32_t,
      const std::function<Status(const ResultGpuRequestTable&)>&)>
      discover = {};
  /** @brief Acquire an owning window backed for native GPU access.
   * The requested Region must be covered by this phase's payload read grant.
   * The active callback must have a GPU lane; otherwise acquisition returns
   * BackendUnavailable. Unauthorized or missing tensor coverage is an
   * InvalidArgument protocol failure. The method charges Root work and may
   * pack fragmented backing before uploading it; compatible same-device
   * affine backing can be reused. The returned window retains the source
   * Result, schema, descriptor, cancellation state, and native storage across
   * later phases and owner release. Use it while binding and dispatching GPU
   * work in the current callback; its row/rectangle access follows the owning
   * window contract.
   */
  Result<ResultTensorReadWindow> acquire_native_tensor(
      std::uint32_t input, std::uint32_t slot, const Region& region) const;
  /** @brief Acquire an immutable packed atlas from the current payload grant.
   * @details Uses only the input port and slot already authorized by this
   * phase's payload grant. The atlas contains the exact granted coverage; it
   * fills no holes and grants no further input access. Repeated calls for the
   * same input and slot in one poll return the same shared atlas. An empty Need
   * is a no-op and creates no payload capability. A direct host may explicitly
   * supply an empty payload capability, yielding an inert atlas with no reads
   * or dispatch. Root-
   * accounted atlas buffers are independent of operation workspace, source
   * Result lifetime, and this phase; the atlas retains no source Result.
   * @param input Input-port index.
   * @param slot Tensor-slot index.
   * @return Immutable atlas, or a typed failure. Calls from a CPU phase, a
   * missing payload grant, or descriptor-only grant fail with sticky
   * InvalidArgument. The returned atlas may outlive the poll and execution
   * context.
   */
  Result<std::shared_ptr<const FragmentAtlas>> acquire_native_atlas(
      std::uint32_t input, std::uint32_t slot) const;
  Status read_tensor(std::uint32_t input, std::uint32_t slot,
                     const std::vector<std::uint64_t>& coordinate,
                     void* destination, std::size_t bytes) const;
};
/** @brief Move-only host-owned structured continuation; destructor runs once.
 * A poll is admitted non-blockingly. An overlapping or reentrant call returns
 * InvalidArgument with reason None and Protocol/Group detail. It does not
 * enter the callback, notify the failure observer, record the phase failure
 * latch, or change the active phase or work accounting. Admission covers the
 * callback exception fence and publication checks. Move, reset, and destruction
 * require the continuation to be inactive; its state destructor runs once.
 */
class PHOTOSPIDER_API ResultContinuation final {
 public:
  ResultContinuation() = default;
  ~ResultContinuation() noexcept;
  ResultContinuation(ResultContinuation&&) noexcept;
  ResultContinuation& operator=(ResultContinuation&&) noexcept;
  ResultContinuation(const ResultContinuation&) = delete;
  ResultContinuation& operator=(const ResultContinuation&) = delete;
  template <class State, class... Args>
  static Result<ResultContinuation> make(const BufferAllocator& allocator,
                                         Args&&... args) {
    static_assert(alignof(State) <= alignof(std::max_align_t),
                  "overaligned result state");
    static_assert(std::is_nothrow_destructible<State>::value,
                  "result state destructor must not throw");
    auto memory = allocator.allocate(sizeof(State));
    if (!memory.ok())
      return Result<ResultContinuation>(memory.status());
    ResultContinuation continuation;
    continuation.storage_ = memory.take_value();
    new (continuation.storage_.data()) State(std::forward<Args>(args)...);
    continuation.destroy_ = [](void* state) noexcept {
      static_cast<State*>(state)->~State();
    };
    continuation.poll_ = [](void* state, const ResultProgramPhase& phase) {
      return static_cast<State*>(state)->poll(phase);
    };
    return Result<ResultContinuation>(std::move(continuation));
  }
  /** @brief Create a continuation that calls a compile-time stateless poller.
   * Poll must be a non-null function pointer with signature
   * Result<ResultProgramPoll>(const ResultProgramPhase&). Each host poll calls
   * Poll with its current phase. This factory leaves the continuation's state
   * buffer empty and installs no state destructor; it performs no retained
   * state or payload allocation. OperationRegistry still retains the operation
   * definition, prepared metadata, and resource owners for the continuation's
   * lifetime.
   *
   * The callback may return a failed Result or throw. The host invokes it
   * inside the Result execution failure fence, so Poll is not required to be
   * noexcept. Use make() when a continuation must retain mutable state.
   */
  template <Result<ResultProgramPoll> (*Poll)(const ResultProgramPhase&)>
  static Result<ResultContinuation> stateless() {
    static_assert(Poll != nullptr, "missing stateless Result callback");
    ResultContinuation continuation;
    continuation.poll_ = [](void*, const ResultProgramPhase& phase) {
      return Poll(phase);
    };
    return Result<ResultContinuation>(std::move(continuation));
  }
  bool valid() const noexcept { return poll_ != nullptr; }
  /** @brief Executes one finite stage; the host provides the exception fence.
   * Nonblocking admission rejects an overlapping or reentrant call with
   * InvalidArgument/None/Protocol/Group. The rejected call does not invoke the
   * callback, notify the failure observer, record the phase failure latch, or
   * alter the admitted poll. Callers must keep the continuation alive and
   * inactive for move, reset, or destruction.
   * Standard exceptions return OperationFailed/HostException with a null-safe
   * diagnostic; allocation exceptions return ResourceExhausted. A previously
   * recorded phase failure takes precedence, and failed/cancelled phases do
   * not enter the callback. These rules also apply to direct registry hosts.
   * Direct shared-stage polls expose only certified publication scopes. A
   * hidden intermediate commit returns an empty Need so the host can poll
   * again; complete validators defer visibility until full static coverage.
   * If the output declares `maximum_output_payload_bytes`, the host compares
   * the publication's unique physical backing against that cap. It excludes
   * owners still live through exposed Result inputs or authorized tensor
   * grants, tracked with weak references across polls. Each prefix is checked
   * as a whole Result. Workspace, metadata and immutable resource admission are
   * separate; the callback's earlier Root allocations are not precluded by
   * this publication-time check. On the first poll, the host allocates the
   * bound guard and its shared control block from the Result Root before
   * entering the callback; Metadata admission failure therefore skips the
   * callback. Exceeding the cap is a sticky ResourceExhausted/CapacityLimit
   * failure. Registry-created continuations also retain the operation
   * definition and prepared resources through state destruction. Cancellation
   * is checked around the start factory, so cancellation after factory entry
   * takes precedence over its status or returned state, which is destroyed
   * before start returns.
   */
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase);

 private:
  friend class OperationRegistry;
  void reset() noexcept;
  Result<ResultProgramPoll> poll_guarded(const ResultProgramPhase& phase);
  std::shared_ptr<const void> definition_;
  std::shared_ptr<const PreparedOperation> prepared_;
  std::uint32_t output_index_ = 0, shared_stage_ = UINT32_MAX;
  std::shared_ptr<const SchemaTemplate> output_schema_;
  std::string semantic_key_;
  bool host_validates_ = false;
  std::optional<ResultDescriptor> validated_publication_;
  std::optional<ResultBuilder> visible_publication_;
  TileRequest visible_tiles_;
  ResourceSet resources_;
  std::shared_ptr<plugin_internal::ResultPayloadBound> payload_bound_;
  std::optional<std::uint64_t> payload_limit_;
  MutableBuffer storage_;
  using Destroy = void (*)(void*) noexcept;  // NOLINT(readability/casting)
  Destroy destroy_ = nullptr;
  Result<ResultProgramPoll> (*poll_)(void*,
                                     const ResultProgramPhase&) = nullptr;
  std::atomic_flag active_ = ATOMIC_FLAG_INIT;
};
using ResultProgramStart = std::function<Result<ResultContinuation>(
    const ResultProgramQuery&, const BufferAllocator&)>;
}  // namespace ps
