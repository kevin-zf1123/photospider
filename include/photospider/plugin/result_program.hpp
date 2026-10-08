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
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/fragment_atlas.hpp"
#include "photospider/data/quality.hpp"
#include "photospider/data/result.hpp"
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
  /** @brief Result-discovery work budget per Actor, accumulated across polls.
   */
  std::uint64_t maximum_work = 1048576;
  /** @brief Maximum state allocation for a staged Result continuation. */
  std::uint64_t maximum_state_bytes = 1048576;
  /** @brief Maximum staged Result transitions admitted for one Run. */
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
        parameters(static_parameters) {}
  const std::vector<OperationMetadata>& inputs;
  const OperationMetadata& output;
  const std::map<std::string, ParameterValue>& parameters;
  /** @brief Captured demand for the selected named output tensor slot.
   * `tensor_slot` selects the member; an absent footprint requests its full
   * sample domain. This footprint participates in producer identity. Result
   * checkpoint scope is separate and excludes the requested footprint. Tile
   * dimensions are physical scheduling choices. For a C++ RequestRecord
   * output, this footprint is the complete terminal query Q; the executor does
   * not enlarge a supplied Q merely because the output region rule is Whole.
   */
  std::optional<Footprint> tensor_outputs;
  std::uint32_t tensor_slot = 0;
  std::uint64_t tile_height = 128, tile_width = 128;
  Backend backend = Backend::Cpu;
  std::string_view semantic_key;
  /** @brief Routing identity for the captured execution snapshot.
   * Result joint admission requires a shared nonempty identity across members;
   * it identifies provenance and does not supply numeric input semantics.
   */
  std::string_view snapshot_identity;
  std::uint32_t output_index = 0;
  std::uint64_t page_bytes = 4096;
  CancellationToken cancellation;
  /** @brief Immutable resource owners selected for the Result input/output
   * schemas. The continuation retains the admitted set independently of this
   * query.
   */
  ResourceBindings resources = {};
  /** @brief Owning immutable metadata/static-parameter preparation. Compiled
   * starts preserve and validate this handle; direct starts prepare once when
   * absent. Continuations retain it for all polls and concurrent plan reuse.
   */
  std::shared_ptr<const PreparedOperation> prepared;
};
/** @brief Derive the logical atom named by a Result query.
 * The key uses the declaration-order output index and the selected tensor
 * query. Tuple-channel and atomic trailing axes are closed into one
 * observation and omitted from the coordinate; every other sample axis,
 * including batch axes, remains in order and must have extent one. The query
 * must close to one nonempty box. A collection-only output maps to the
 * canonical singleton key `{output_index, rank 1, coordinate 0}`. This helper
 * validates query geometry, not the operation's joint-contract declaration.
 */
PHOTOSPIDER_API Result<AtomKey> result_atom_key(
    const ResultProgramQuery& query);
/** @brief Return the fixed observation domain for the selected Result slot.
 * The domain omits tuple-channel and atomic trailing axes while retaining the
 * remaining batch and cell axes with their complete extents. If no coordinate
 * axes remain, it is the singleton rank-one domain. For a collection-only
 * output, the domain is also the singleton key domain. This describes semantic
 * observation scope; it does not expand tensor payload or authorize reads.
 */
PHOTOSPIDER_API Result<AtomDomain> result_observation_domain(
    const ResultProgramQuery& query);
/** @brief Need for one tensor member of an input Result.
 * `samples` uses the member's full sample shape, including batch axes before
 * cell axes. When one poll requests several roles for the same input and slot,
 * the executor supplies the union of payload-readable samples once. It still
 * validates and records each role's own footprint separately. For a computed
 * contract-2 producer, the coordinator expands a wide tensor Need into
 * singleton-atom requests before supplying it; this does not limit wider
 * tensor Needs on other paths. When the union of a computed contract-2
 * tensor Need is Empty and the producer schema has no fields, the coordinator
 * can supply a sealed Empty Result through its metadata-only object path. This
 * creates no atom and grants no sample-read permission. A producer with fields
 * does not use this shortcut.
 */
struct ResultTensorNeed final {
  std::uint32_t input = 0, slot = 0;
  Footprint samples;
  std::uint32_t roles = 1;
};
/** @brief Owning capability restricted to the explicit tensor Need.
 * A capability can contain one Result or private pieces backed by several
 * original Results when a computed contract-2 Need is expanded into
 * singleton-atom requests. Each piece retains its original Result, captured
 * descriptor and granted samples; the coordinator does not create an
 * aggregate Result or validation domain. Reads and acquired windows remain
 * restricted to those grants. `object_id()` is the source ID for a singleton
 * capability and zero when there is no single Result identity; zero grants no
 * read authority. An Empty metadata-only grant may retain a sealed Result,
 * but its empty coverage does not authorize `read()` or `acquire()`. The phase
 * association still lists the actual input ports and their source object IDs.
 * For compiled structured CPU Whole view execution, the coordinator may prepare
 * each authorized Need box as an affine view before the next computation poll.
 * Auto may instead retain an immutable, Root-accounted private backing when a
 * source box has no compatible view. Copies of the capability share that
 * backing and its metadata lease. This preparation does not change the Result
 * schema or grant access beyond the Need; direct phases do not synthesize it.
 */
class PHOTOSPIDER_API ResultTensorInput final {
 public:
  ResultTensorInput() = default;
  std::uint64_t object_id() const noexcept {
    return pieces_.size() > 1 ? 0 : result_.object_id();
  }
  const ResultTensorSpec& spec() const {
    return result_.schema().tensors.at(slot_);
  }
  const Footprint& coverage() const noexcept { return samples_; }
  Status read(const std::vector<std::uint64_t>& coordinate, void* destination,
              std::size_t bytes,
              const CancellationToken& cancellation = {}) const;

  /** @brief Acquire an exact Need-authorized frame/layer rectangle.
   * Acquisition does not copy samples; a compiled Whole coordinator may have
   * prepared private affine backing earlier when its Auto view proof failed.
   * Unauthorized access records a sticky protocol failure. The returned window
   * owns immutable backing independently of this phase.
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
  struct Piece {
    ResultRef result;
    ResultDescriptor descriptor;
    Footprint samples;
  };
  struct InputBacking {
    ResourceLease metadata_owner;
    Value value;
    InputBacking(ResourceLease metadata, Value backing)
        : metadata_owner(std::move(metadata)), value(std::move(backing)) {}
  };
  Status read_granted(const std::vector<std::uint64_t>& coordinate,
                      void* destination, std::size_t bytes,
                      const CancellationToken& cancellation) const;
  Status prepare_whole_view(
      bool require_view, const CancellationToken& cancellation,
      const std::function<Status(std::uint64_t)>& consume_work);
  ResultRef result_;
  ResultDescriptor descriptor_;
  ResourceVector<Piece> pieces_;
  ResourceVector<std::shared_ptr<const InputBacking>> input_backing_;
  std::optional<ResourceBudget> resources_;
  std::uint32_t slot_ = 0;
  bool payload_authorized_ = false;
  Footprint samples_;
  std::shared_ptr<std::atomic<ErrorCode>> failure_;
  std::shared_ptr<plugin_internal::FailureLatch> observer_;
};
/** @brief Requests complete associated data or a monotone minimum field prefix.
 * Complete requests wait for seal. A prefix request returns a sealed shorter
 * collection when discovery finished; the consumer checks actual count.
 */
struct ResultObjectNeed final {
  std::uint32_t input = 0, field = 0;
  bool complete = true;
  std::uint64_t minimum_rows = 0;
};
struct ResultCreateTemporary final {};
struct ResultReadTemporary final {
  TemporaryStorage storage;
  std::uint64_t offset = 0, bytes = 0;
};
struct ResultWriteTemporary final {
  TemporaryStorage storage;
  std::uint64_t offset = 0;
  std::shared_ptr<const CpuStorage> bytes;
};
struct ResultExtendTemporary final {
  TemporaryStorage storage;
  std::uint64_t bytes = 0;
};
/** @brief Closed mandatory I/O actions, executed after a callback yields. */
// NOLINTBEGIN(whitespace/indent_namespace)
using ResultIoRequest =
    std::variant<ResultReadPlan, ResultWritePlan, ResultCreateTemporary,
                 ResultReadTemporary, ResultWriteTemporary,
                 ResultExtendTemporary>;
/** @brief Read owner, new temporary owner, append offset or write success. */
using ResultIoReply =
    std::variant<std::shared_ptr<const CpuStorage>, TemporaryStorage,
                 std::uint64_t, std::monostate>;
// NOLINTEND
/** @brief Requests bounded Result input access or mandatory I/O for one stage.
 * The combined Result-object, tensor, and I/O entry count is limited by the
 * smaller of 65536 and the dependency set's `maximum_boxes`. An ordinary
 * empty Need is invalid. Tensor Needs for the same input and slot can carry
 * separate roles; the executor unions their payload reads while preserving
 * per-role evidence.
 */
struct ResultProgramNeed final {
  ResourceVector<ResultObjectNeed> results;
  ResourceVector<ResultIoRequest> io;
  ResourceVector<ResultTensorNeed> tensors = {};
};
/** @brief Mutable table borrowed only during a synchronous GPU discovery call.
 * `bytes` points to a zero-initialized Root allocation with 16 header bytes
 * and `capacity` 144-byte records. The host freezes and decodes it after the
 * callback and submitted GPU work have drained; it is separate from operation
 * workspace.
 */
struct ResultGpuRequestTable final {
  std::uint8_t* bytes = nullptr;
  std::uint64_t byte_size = 0;
  std::uint32_t capacity = 0;
};
/** @brief Immutable grouped tensor-footprint metadata returned by discovery.
 * The receipt retains no source payload. Its tensor Needs describe typed input
 * regions and roles; the executor supplies those Needs before dependent
 * computation continues.
 */
struct ResultDiscoveryReceipt final {
  ResourceVector<ResultTensorNeed> tensors;
};
/** @brief New certified prefix or complete object, published by one producer.
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
 * Root resource operations are explicit admission; mandatory I/O is returned as
 * Need actions. Calling TemporaryStorage I/O inside poll is a sticky protocol
 * failure. `read_tensor()` and `consume_work()` failures cannot be ignored
 * into success.
 */
struct PHOTOSPIDER_API ResultProgramPhase final {
  const ResultProgramQuery& query;
  const ResultObjectInputs& results;
  const ResourceVector<ResultIoReply>& io;
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
  const ResultTensorInputs* tensors = nullptr;
  const ps_cpu_parallel_service_v1* cpu_parallel = nullptr;
  const ps_cpu_tiles_service_v1* cpu_tiles = nullptr;
  const ps_gpu_service_v1* gpu = nullptr;
  /** @brief Borrowed host status reader for the current native invocation.
   * Read on the entry thread after a GPU service call to preserve specific
   * errors that the numeric GPU ABI reports through its general failure code.
   * Must not be retained beyond this phase. */
  std::function<Status()> gpu_status = {};
  /** @brief Root-accounted summary of consumed Result identities.
   * Entries preserve input-port/ObjectId order, including repeated ids across
   * ports, and include earlier Need stages. The list retains no payload and
   * grants no read permission. Borrowed only until this poll returns; copy it
   * into ResultBuilder before returning publication. ResultObjectInputs,
   * ResultTensorInput capabilities, and retained windows have independent
   * owner lifetimes. A successful checkpoint restore refreshes this list;
   * references and iterators to its prior elements may be invalidated, while
   * the borrowed vector object remains valid through the poll.
   */
  const ResourceVector<std::uint64_t>* association = nullptr;
  /** @brief Reports actual numeric/copy/view work for this poll. Reports are
   * validated and merged by the host; ignored failures remain sticky. Borrowed
   * until poll returns and only callable on its entry thread.
   */
  std::function<Status(const NumericDiagnostics&)> report_numeric = {};
  /** @brief Find the greatest saved sequence <= `before` in one checkpoint
   * phase. A miss, including optional cache/capacity exhaustion, returns an
   * empty optional. A successful restore also refreshes `association` before
   * this callback returns; references and iterators to its previous elements
   * may be invalidated. Restoring a checkpoint restores dependency history,
   * not `results`, tensor capabilities, read windows, or Need authorization;
   * the callback must request current input access separately. RequestRecord
   * outputs cannot use checkpoint services; such use is a sticky
   * InvalidArgument.
   */
  std::function<Result<std::optional<ResultCheckpoint>>(std::uint32_t,
                                                        std::uint64_t)>
      checkpoint_before = {};
  /** @brief Retain a sealed state Result under a nonzero phase and an ordered
   * sequence, which may be zero.
   * Retention requires a pure deterministic operation and pure selected-input
   * producer closure. Optional cache or capacity refusal declines retention
   * without failing an otherwise successful operation. Protocol, cancellation,
   * and admitted Run-work failures remain execution failures. RequestRecord
   * outputs cannot use checkpoint services; such use is a sticky
   * InvalidArgument.
   */
  std::function<Status(std::uint32_t, std::uint64_t, const ResultRef&)>
      checkpoint_publish = {};
  /** @brief Evaluate a Result block from an explicit incoming state.
   * `kind`, `[begin,end)`, and `mode` identify the operation phase and range.
   * The incoming and computed states must share a schema and Root, and each
   * must be a sealed CompleteBundle with one fully covered tensor and no
   * fields. Carry every control needed by a later block in that tensor.
   * Content caching is optional and applies only to cacheable operations with
   * a pure selected-input producer closure. Its key uses the operation
   * contract, range, mode, incoming state schema/coverage/raw bits, and
   * currently supplied Tensor coverage, metadata, and raw bits; it excludes
   * output demand, execution snapshot, and ObjectIds. When the operation opts
   * into cross-output sharing, its namespace also includes all resolved output
   * contracts, backend, execution mode, static parameters and actual input
   * metadata. The selected output index and output-specific metadata are not
   * added separately, so a shared computation must be output independent or
   * encode the difference in state or mode.
   * Optional content caching skips resource-bearing incoming states, supplied
   * Tensor inputs, and computed states. Those states remain valid for
   * computation; computed states with resources are not packed into the
   * internal Value-backed block-state LRU.
   * The compute callback may depend only on key-covered logical schemas,
   * coverage, raw bits, and static metadata/parameters plus phase, range, and
   * mode. It must not depend on Result ObjectIds, `semantic_key`, association,
   * the original output demand, or earlier windows not supplied to this call;
   * those provenance facts are deliberately outside the content key. A cache
   * hit publishes a fresh state Result without old source association or
   * dependency evidence. Need evidence from the current actor remains in
   * effect. Public output publication, checkpoints and completed-result caches
   * remain independent of block-state reuse. Optional cache-work exhaustion
   * skips caching and computes normally; Object-Need contents and I/O are not
   * cached. Pure block work is
   * available inside a RequestRecord query, but a terminal Result cannot be an
   * incoming or computed block state.
   */
  std::function<Result<ResultRef>(std::uint32_t, std::uint64_t, std::uint64_t,
                                  std::uint64_t, const ResultRef&,
                                  const std::function<Result<ResultRef>()>&)>
      block = {};
  /** @brief Run bounded synchronous GPU discovery for additional tensor Needs.
   * Capacity and candidates must be positive. Capacity bounds emitted records;
   * candidates bounds emit attempts. The
   * callback receives a temporary zeroed Root table and must submit actual
   * native GPU work. The host freezes, validates, groups, and returns its
   * immutable footprint receipt. Capacity is limited by the hard 65536 record
   * ceiling, `maximum_gpu_requests`, and dependency `maximum_boxes` bound.
   * Discovery work is also charged against the actor's remaining discovery
   * budget across polls, the Run budget, and Root resources.
   */
  std::function<Result<std::shared_ptr<const ResultDiscoveryReceipt>>(
      std::uint32_t, std::uint32_t,
      const std::function<Status(const ResultGpuRequestTable&)>&)>
      discover = {};
  /** @brief Acquire an owning window backed for native GPU access.
   * The requested Region must be covered by this phase's tensor Need. The
   * active callback must have a GPU lane; otherwise acquisition returns
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
  /** @brief Acquire an immutable packed atlas for this tensor Need.
   * @details Uses only the input port and slot already authorized by this
   * phase's payload Need. The atlas contains the exact Need coverage; it fills
   * no holes and grants no further input access. Repeated calls for the same
   * input and slot in one poll return the same shared atlas. An Empty payload
   * Need yields a valid atlas with one inert payload byte and a two-slot
   * directory; it reads no source samples and performs no dispatch. Root-
   * accounted atlas buffers are independent of operation workspace, source
   * Result lifetime, and this phase; the atlas retains no source Result.
   * @param input Need input-port index.
   * @param slot Need tensor-slot index.
   * @return Immutable atlas, or a typed failure. Calls from a CPU phase, a
   * missing Need, or a Descriptor-only Need fail with sticky InvalidArgument.
   * The returned atlas may outlive the poll and execution context.
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
   * For RequestRecord outputs, the host checks each publication for the
   * captured schema, scope, Root, completeness, and exact selected tensor
   * coverage Q. Partial or mismatched Result publications fail with a sticky
   * protocol error; a successful Result is marked terminal for later
   * input/view/state checks.
   * If the output declares `maximum_output_payload_bytes`, the host compares
   * the publication's unique physical backing against that cap. It excludes
   * owners still live through exposed Result inputs or authorized tensor
   * grants, tracked with weak references across polls. Each prefix is checked
   * as a whole Result. Workspace, metadata and ICC/OCIO resource admission are
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
  ResourceBindings resources_;
  std::shared_ptr<plugin_internal::ResultPayloadBound> payload_bound_;
  std::optional<std::uint64_t> payload_limit_;
  MutableBuffer storage_;
  using Destroy = void (*)(void*) noexcept;  // NOLINT(readability/casting)
  Destroy destroy_ = nullptr;
  Result<ResultProgramPoll> (*poll_)(void*,
                                     const ResultProgramPhase&) = nullptr;
  std::atomic_flag active_ = ATOMIC_FLAG_INIT;
};
/** @brief One local result from a Result joint callback.
 * The callback returns one Root-owned outcome for each member supplied to that
 * round, keyed by its distinct AtomKey. Contract 1 uses distinct output keys;
 * contract 2 can use different coordinates of the same output. The outcome is
 * that member's Result Need, complete publication, or typed local failure.
 * Contract 2 may attach QualityReport evidence, which the host validates
 * against the published estimate or typed failure before delivery. It must
 * be owned by `phase.resources.allocator()` and by either the member phase
 * allocator or the shared workspace allocator.
 */
struct ResultJointOutcome final {
  AtomKey key;
  Result<ResultProgramPoll> outcome;
  std::optional<QualityReport> quality = {};
};
/** @brief Borrowed resources for one bounded Result joint callback round.
 * `members` contains the currently ready, non-cancelled subset; after
 * cancellation this may contain one member even when start admitted more.
 * Their phase objects and services are borrowed for the callback. Each query
 * carries the prepared tensor Q closed over tuple and atomic trailing axes.
 * `allocator` provides the shared workspace limit; `consume_work` charges
 * round work against the active Run/root. Need admission validates each input
 * against the member's selected input projection. All member ResourceBudgets,
 * member allocators and the shared `allocator` must share the continuation
 * Root's nonempty accounting domain. `BufferAllocator::same_owner` checks only
 * that domain identity; it does not compare quotas or grant payload access.
 * The coordinator validates selected ports and owns Need storage; the callback
 * returns a Root-owned outcome vector, which the host validates before use.
 * The host counts Need entries across the whole round, caps their sum at
 * 65,536, and charges each Need envelope before walking it.
 */
struct ResultJointPhase final {
  const ResourceVector<const ResultProgramPhase*>& members;
  const BufferAllocator& allocator;
  std::function<Status(std::uint64_t)> consume_work;
};
/** @brief Move-only shared continuation state for CPU Result joints.
 * Contract 1 keys members by distinct output indices and requires
 * RequestFailureOnly delivery. Contract 2 keys members by distinct AtomKeys
 * and requires PerAtomOutcome delivery; coordinates of one output may repeat.
 * In both contracts, each tensor query closes over tuple-channel and atomic
 * trailing axes into one nonempty Atomic observation. The registry captures
 * raw queries and validates them on later polls before lending closed queries
 * to the callback. Each poll sees the non-cancelled ready subset, which may
 * contain one member, and returns a Root-owned outcome vector keyed by AtomKey.
 * The host validates member identity, Need ownership and projection,
 * publications and optional contract-2 quality before exposing outcomes.
 * Contract-2 ValidationDomain failures must match the output's full fixed
 * observation domain and propagate across covered Ready/Waiting members. A
 * later domain failure cannot revoke a previously semantic-terminal member.
 * Quality reports must be owned by `phase.resources.allocator()` and by
 * either the member phase allocator or the shared workspace allocator.
 * Estimate-read errors
 * retain their original status; cancellation clears the report, while a
 * nonfinite estimate or certificate mismatch yields InvalidQuality.
 * Per-round Need entries are limited to 65,536 across all members. Each Need
 * contains Result-object or tensor requests, or mandatory I/O actions. Callback
 * exceptions and shared allocation, work, or I/O errors
 * retain their first sticky cause. Foreign phase/member accounting domains or
 * foreign outcome storage are rejected. A group poll failure is latched;
 * concurrent
 * or reentrant polls leave the active poll unchanged. The host retains the
 * definition and Root until retirement. Direct registry use does not fulfill
 * Needs or record dependency evidence. Structured execution schedules eligible
 * CPU contract-2 Result queries as well as contract 1. Contract 2 remains a
 * joint path when joint grouping is disabled: the coordinator runs a one-member
 * group rather than a scalar callback, and it does not retry a failed group as
 * singletons. The public `ExecutionContext::execute_atoms` collector uses the
 * structured coordinator to collect managed CPU Result atoms. Each
 * contract-2 member represents one output observation; its input Tensor Needs
 * may span multiple observations. When a Need targets a computed contract-2
 * producer, the coordinator expands its requested samples into independent
 * atom queries and supplies a piecewise `ResultTensorInput` backed by the
 * original Results. Wider computed Tensor Needs remain supported on other
 * paths. There is no GPU joint path.
 */
class PHOTOSPIDER_API ResultJointContinuation final {
 public:
  ResultJointContinuation() = default;
  ~ResultJointContinuation() noexcept;
  ResultJointContinuation(ResultJointContinuation&&) noexcept;
  ResultJointContinuation& operator=(ResultJointContinuation&&) noexcept;
  ResultJointContinuation(const ResultJointContinuation&) = delete;
  ResultJointContinuation& operator=(const ResultJointContinuation&) = delete;
  template <class State, class... Args>
  static Result<ResultJointContinuation> make(const BufferAllocator& allocator,
                                              Args&&... args) {
    static_assert(alignof(State) <= alignof(std::max_align_t),
                  "overaligned Result joint state");
    static_assert(std::is_nothrow_destructible<State>::value,
                  "Result joint state destructor must not throw");
    auto memory = allocator.allocate(sizeof(State));
    if (!memory.ok())
      return Result<ResultJointContinuation>(memory.status());
    ResultJointContinuation continuation;
    continuation.storage_ = memory.take_value();
    new (continuation.storage_.data()) State(std::forward<Args>(args)...);
    continuation.destroy_ = [](void* state) noexcept {
      static_cast<State*>(state)->~State();
    };
    continuation.poll_ = [](void* state, const ResultJointPhase& phase) {
      return static_cast<State*>(state)->poll(phase);
    };
    return Result<ResultJointContinuation>(std::move(continuation));
  }
  bool valid() const noexcept { return poll_ != nullptr; }
  /** @brief Runs one synchronous callback round for a ready member subset.
   * Concurrent or reentrant poll attempts are rejected without changing the
   * active poll. The host validates the Root-owned outcome vector, member
   * identities, Need containers, selected-port constraints and publications
   * before returning any member outcome. Need entries are capped cumulatively
   * at 65,536 for the round, and each envelope is precharged before its
   * entries are traversed. A malformed round latches a sticky group error;
   * a valid local error remains attached to its member. Cancelled
   * members receive a local Cancelled outcome and are omitted from the
   * callback. For payload-role tensor Needs, the host unions transport only
   * within one member, input port and tensor slot. A ColorArray tuple must be
   * complete in that union before any member outcome is exposed; split Needs
   * from different members, ports or slots cannot complete one another.
   * Descriptor-only Needs do not contribute payload transport.
   */
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase);

 private:
  friend class OperationRegistry;
  struct Member {
    AtomKey key;
    std::uint32_t tensor_slot = 0;
    ResourceString semantic_key, snapshot_identity;
    std::optional<Footprint> outputs, requested_outputs;
    std::shared_ptr<const PreparedOperation> prepared;
    ResourceBindings resources;
    std::shared_ptr<plugin_internal::ResultPayloadBound> payload_bound;
    std::optional<std::uint64_t> payload_limit;
    CancellationToken cancellation;
    bool terminal = false, semantic_terminal = false;
  };
  void reset() noexcept;
  Result<ResourceVector<ResultJointOutcome>> poll_ready(
      const ResultJointPhase& phase);
  std::optional<Status> terminal_failure_;
  std::shared_ptr<const void> definition_;
  std::optional<ResourceBudget> root_;
  ResourceVector<Member> members_;
  std::uint64_t workspace_bytes_ = 0;
  std::uint32_t contract_ = 1;
  MutableBuffer storage_;
  using Destroy = void (*)(void*) noexcept;  // NOLINT(readability/casting)
  Destroy destroy_ = nullptr;
  Result<ResourceVector<ResultJointOutcome>> (*poll_)(
      void*, const ResultJointPhase&) = nullptr;
  std::atomic<bool> active_{false};
};
/** @brief Starts shared state for a validated CPU Result joint invocation.
 * Contract 1 accepts 2..64 distinct Atomic output indices; contract 2 accepts
 * 1..64 distinct AtomKeys and may repeat an output index at different
 * coordinates. Every declared output is Atomic, with RequestFailureOnly for
 * contract 1 or PerAtomOutcome for contract 2. Members share one prepared
 * static invocation and nonempty routing `snapshot_identity`. Each tensor
 * query closes to one Atomic observation. The callback receives closed queries
 * for non-cancelled members, which may be a one-member subset. The registry
 * retains raw queries for later comparison and limits state allocation.
 */
using ResultJointStart = std::function<Result<ResultJointContinuation>(
    const ResourceVector<ResultProgramQuery>&, const BufferAllocator&)>;
using ResultProgramStart = std::function<Result<ResultContinuation>(
    const ResultProgramQuery&, const BufferAllocator&)>;
}  // namespace ps
