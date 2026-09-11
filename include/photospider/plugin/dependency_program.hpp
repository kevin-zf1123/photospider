#pragma once

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
#include "photospider/data/dependency.hpp"
#include "photospider/data/value_fragments.hpp"
#include "photospider/plugin/operation_types.hpp"

namespace ps {
struct OperationTraits;
class OperationRegistry;
/** @brief Execution-scoped bounds, separate from compile-time phase limits. */
struct DependencyLimits final {
  FootprintLimits sets;
  std::uint64_t maximum_work = 1048576;
  std::uint64_t maximum_state_bytes = 1048576;
  std::uint32_t maximum_stages = 4096;
};
/** @brief Caller-owned metadata and exact requested sample set for direct
 * start.
 * @note Inputs and parameters are copied before callbacks. snapshot_identity
 * denotes one immutable supplied input bundle; supply must name the same
 * bundle. It is not a persistent content digest or a caller claim authorizing
 * cache hits. It is provenance for routing/validation, not a semantic input:
 * deterministic programs must not derive output values or dependency choices
 * from the spelling of this identity, allocator addresses or invocation timing.
 */
struct DependencyRequest final {
  std::vector<OperationMetadata> inputs;
  std::map<std::string, ParameterValue> parameters;
  Footprint outputs;
  std::string snapshot_identity;
  Backend backend = Backend::Cpu;
  CancellationToken cancellation = {};
  DependencyLimits limits = {};
};
/** @brief Validated borrowed query visible to start/poll, never retained by
 * code.
 * @note outputs uses descriptor coordinates; observations uses HW for image-v2
 * pixels and the full logical shape for generic samples. RequestRecord retains
 * complete original outputs throughout its invocation and never splits it.
 */
struct DependencyQuery final {
  std::vector<OperationMetadata> inputs;
  OperationMetadata output;
  std::map<std::string, ParameterValue> parameters;
  Footprint outputs;
  Footprint observations;
  std::string snapshot_identity;
  ObservationKind kind = ObservationKind::Atomic;
  Backend backend = Backend::Cpu;
  CancellationToken cancellation = {};
};
/** @brief Declared next reads plus their output associations.
 * @note Atomic programs emit associations; terminal RequestRecord programs emit
 * request_needs. The host preserves rows separately from their transport union.
 * A program may repeat/control reads, consuming execution fuel on every stage.
 */
struct DependencyNeedBatch final {
  std::vector<AtomCertificate> associations;
  std::vector<DependencyNeed> request_needs;
};
/** @brief A poll either suspends for declared inputs or completes its exact
 * set.
 * @note Failure is the enclosing Result status. Under RequestFailureOnly it
 * belongs only to this single observation or identical terminal full request.
 */
using DependencyPoll = std::variant<DependencyNeedBatch, ValueFragments>;
/** @brief Completed internal state with the exact input witness that produced
 * it.
 * @note This is not a public output observation or a RequestRecord. A consumer
 * inherits its complete witness; lookup never waits or evaluates a producer.
 * Only the host constructs checkpoints from a successfully supplied history.
 */
class PHOTOSPIDER_API DependencyCheckpoint final {
 public:
  DependencyCheckpoint() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Algorithm phase identifier; throws logic_error if invalid. */
  std::uint32_t phase() const;
  /** @brief Last completed logical position; throws logic_error if invalid. */
  std::uint64_t sequence() const;
  /** @brief Immutable accounted state, borrowed for this handle lifetime.
   * @throws std::logic_error If the handle is invalid.
   */
  const Value& state() const;
  /** @brief Conservative host metadata units; throws logic_error if invalid. */
  std::uint64_t metadata_entries() const;

 private:
  friend class DependencySession;
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};
/** @brief Optional host-owned, completed-only state services for one poll.
 * @note One service scope must name the same operation instance, immutable
 * input bundle and static contract. Never share it across different bindings.
 * find returns the greatest available sequence <= before in the named phase.
 * publish may decline optional retention by returning success. No service may
 * block waiting for computation. State owners remain accounted until
 * retirement. RequestRecord programs cannot use these services. Source/waiter
 * errors are never stored as checkpoints or broadcast through this interface.
 * Service exceptions are fenced and sticky even if plugin code ignores errors.
 */
struct DependencyCheckpointServices final {
  /** @brief Nonempty host operation-instance scope, at most 4096 bytes.
   * @note Included in checkpoint provenance; plugin callbacks cannot inspect
   * it.
   */
  std::string identity;
  std::function<Result<std::optional<DependencyCheckpoint>>(
      std::uint32_t phase, std::uint64_t before)>
      find;
  std::function<Status(const DependencyCheckpoint&)> publish;
};
/** @brief Optional host cache for pure internal block transforms.
 * @note Keys are computed by the session from the static operation contract,
 * phase/range/mode, exact supplied input sets and bits, and actual incoming
 * state bits. Values contain no old source witness. Hosts retain only completed
 * immutable states, account their owners, and never wait for a producer.
 */
struct DependencyBlockServices final {
  /** @brief Charges optional key work before hashing. False bypasses retention.
   */
  std::function<bool(std::uint64_t)> consume_work;
  /** @brief Returns a completed state, or an invalid Value for a cache miss. */
  std::function<Result<Value>(const std::string&)> find;
  /** @brief Optionally retains a completed state; failures are sticky. */
  std::function<Status(const std::string&, const Value&)> publish;
};
/** @brief Services borrowed only for one finite, nonblocking poll.
 * @note inputs contains only this stage's ready authorized fragments. No read
 * starts upstream execution. State must copy needed data through its accounted
 * owner before returning; retaining borrowed phase/query pointers is invalid.
 */
struct PHOTOSPIDER_API DependencyPhase final {
  const DependencyQuery& query;
  const std::vector<ValueFragments>& inputs;
  const BufferAllocator& allocator;
  /** @brief Charges candidates before enumeration/allocation, including
   * repeats. Trusted callbacks must charge non-read discovery work explicitly.
   */
  const std::function<Status(std::uint64_t)>& consume_work;
  /** @brief Makes a service failure sticky even if callback code ignores it. */
  const std::function<Status(Status)>& report_failure;
  /** @brief Effective exact-set limits, including invocation cancellation.
   * @note Apply before copying raw candidates; normalization must not erase
   * their contribution to metadata/work bounds.
   */
  FootprintLimits sets = {};
  /** @brief Imports a completed checkpoint and its full successful witness.
   * Missing optional services return an empty result; this never starts work.
   */
  std::function<Result<std::optional<DependencyCheckpoint>>(std::uint32_t,
                                                            std::uint64_t)>
      checkpoint_before;
  /** @brief Publishes host-allocated immutable state from the supplied history.
   * @note The sequence/phase identifies the algorithm's complete state,
   * including its processed index and numeric mode. A changed incoming state
   * must be computed again; equality of an outgoing accumulator is not a block
   * key.
   */
  std::function<Status(std::uint32_t, std::uint64_t, const Value&)>
      checkpoint_publish;
  /** @brief Evaluates one pure state transition, optionally reusing its result.
   * @note compute must depend only on incoming state, current supplied inputs,
   * static parameters/metadata, phase, [begin,end) and mode. It must encode all
   * carried controls/numeric state in incoming, and must not inspect original
   * Q, earlier unsupplied fragments or external/mutable state. This trusted
   * contract does not infer purity from arbitrary C++ code. Output state must
   * match the incoming descriptor, region and facets and use this stage
   * allocator. Successful hits are copied into the stage allocator and preserve
   * current input evidence; they never import an obsolete prefix relation.
   * Errors remain local to the current Atomic observation. No output batching
   * is authorized.
   */
  std::function<Result<Value>(std::uint32_t phase, std::uint64_t begin,
                              std::uint64_t end, std::uint64_t mode,
                              const Value& incoming,
                              const std::function<Result<Value>()>& compute)>
      block;
  /** @brief Charged, bounds-checked sample read; no missing-page zero fallback.
   */
  Status read(std::uint32_t port, const std::vector<std::uint64_t>& coordinate,
              void* destination, std::size_t size) const;
};
/** @brief Address-stable, move-only state allocated through the host allocator.
 * @note Destruction runs exactly once before its storage lease retires. A state
 * must finish each poll, use supplied allocators for payload/scratch, and avoid
 * hidden external inputs. This trusted in-process contract is not a sandbox.
 */
class PHOTOSPIDER_API DependencyContinuation final {
 public:
  DependencyContinuation() = default;
  ~DependencyContinuation() noexcept;
  DependencyContinuation(DependencyContinuation&& other) noexcept;
  DependencyContinuation& operator=(DependencyContinuation&& other) noexcept;
  DependencyContinuation(const DependencyContinuation&) = delete;
  DependencyContinuation& operator=(const DependencyContinuation&) = delete;
  /** @brief Constructs State in host bytes; State implements poll(phase).
   * @return Owned state or allocator/size failure; construction exceptions
   * propagate to the registry's start fence. Overaligned state is unsupported.
   */
  template <class State, class... Args>
  static Result<DependencyContinuation> make(const BufferAllocator& allocator,
                                             Args&&... args) {
    static_assert(alignof(State) <= alignof(std::max_align_t),
                  "overaligned dependency state");
    static_assert(std::is_nothrow_destructible<State>::value,
                  "state destructor must not throw");
    auto memory = allocator.allocate(sizeof(State));
    if (!memory.ok())
      return Result<DependencyContinuation>(memory.status());
    DependencyContinuation result;
    result.storage_ = memory.take_value();
    new (result.storage_.data()) State(std::forward<Args>(args)...);
    result.destroy_ = [](void* state) noexcept {
      static_cast<State*>(state)->~State();
    };
    result.poll_ = [](void* state, const DependencyPhase& phase) {
      return static_cast<State*>(state)->poll(phase);
    };
    return Result<DependencyContinuation>(std::move(result));
  }
  bool valid() const noexcept { return poll_ != nullptr; }
  std::uint64_t state_bytes() const noexcept { return storage_.size(); }

 private:
  friend class DependencySession;
  void reset() noexcept;
  MutableBuffer storage_;
  using Destroy = void (*)(void*) noexcept;  // NOLINT(readability/casting)
  Destroy destroy_ = nullptr;
  Result<DependencyPoll> (*poll_)(void*, const DependencyPhase&) = nullptr;
};
/** @brief Optional pure static validation, including Empty output requests.
 * @note Called after base parameter/descriptor inference, before any state or
 * source read, and during compilation. May reject but cannot change metadata.
 * Must be deterministic, finite and independent of request coverage, pixels or
 * external state. Borrowed inputs/parameters expire at return; exceptions are
 * fenced. Implementation identity follows its immutable registered definition.
 */
// Wrapped function types are not namespace indentation.
// NOLINTBEGIN(whitespace/indent_namespace)
using DependencyValidator =
    std::function<Status(const std::vector<OperationMetadata>&,
                         const std::map<std::string, ParameterValue>&)>;
// NOLINTEND
/** @brief Creates host-owned continuation without any upstream blocking call.
 */
using DependencyStart = std::function<Result<DependencyContinuation>(
    const DependencyQuery&, const BufferAllocator&)>;
/** @brief Successful complete request, with atomic or terminal evidence.
 * @note Atomic results include a restrictable certificate. RequestRecord
 * results have no atomic certificate and retain the complete request and
 * dependency list.
 */
struct DependencyResult final {
  ValueFragments value;
  Footprint original_outputs;
  ObservationKind kind = ObservationKind::Atomic;
  std::optional<DependencyCertificate> certificate;
  std::vector<DependencyNeed> request_dependencies;
};
/** @brief Poll result delivered by the validated direct protocol driver. */
using DependencyProgress = std::variant<DependencyNeedBatch, DependencyResult>;
/** @brief One owning start/poll/supply/retire lifecycle, with no worker
 * ownership.
 * @note Calls must not race destruction. Concurrent/reentrant poll or supply is
 * rejected, never serialized behind an active callback. Borrowed phase objects
 * expire on return; the owning registry/DSO definition survives state
 * destruction. Accepted-call failures retire state with cancellation priority.
 * A rejected concurrent/reentrant call leaves the active call and its state
 * undisturbed.
 */
class PHOTOSPIDER_API DependencySession final {
 public:
  ~DependencySession() noexcept;
  DependencySession(const DependencySession&) = delete;
  DependencySession& operator=(const DependencySession&) = delete;
  /** @brief Polls only with ready inputs; does not wait or execute upstream
   * work.
   * @return NeedBatch or complete result, or a fenced typed failure. Polling
   * while waiting for supply or after a terminal result fails InvalidArgument.
   * @param allocator Stage-local host output/scratch allocator.
   * @param checkpoints Optional borrowed host services, valid until poll
   * returns.
   * @throws std::bad_alloc For caller-side metadata copying.
   */
  Result<DependencyProgress> poll(
      const BufferAllocator& allocator = BufferAllocator{},
      const DependencyCheckpointServices& checkpoints = {},
      const DependencyBlockServices& blocks = {});
  /** @brief Supplies exactly the pending transport union for every input port.
   * @param inputs Exact matching metadata and authorized sets, including empty
   * entries for unused ports. Owners remain held only until the next poll
   * returns.
   * @param snapshot_identity Must equal the captured immutable bundle identity.
   * @return Success or typed metadata/coverage/numeric/cancellation failure.
   * Invalid supply is terminal and never enters the program callback.
   */
  Status supply(std::vector<ValueFragments> inputs,
                const std::string& snapshot_identity);
  /** @brief Exact grouped port/role projection of the current pending reads. */
  Result<std::vector<DependencyNeed>> pending_reads() const;
  /** @brief Read-only query, valid for the session lifetime; not mutable state.
   */
  const DependencyQuery& query() const noexcept;
  /** @brief Total charged work and number of actual program polls. */
  std::uint64_t consumed_work() const;
  std::uint32_t poll_count() const;

 private:
  friend class OperationRegistry;
  struct Impl;
  explicit DependencySession(std::unique_ptr<Impl> impl);
  static Result<std::shared_ptr<DependencySession>> create(
      const std::string& operation, OperationTraits traits,
      const DependencyStart& start, const DependencyValidator& validate,
      DependencyRequest request, const BufferAllocator& allocator,
      std::shared_ptr<const void> definition);
  static Status validate_static(
      const DependencyValidator& validate,
      const std::vector<OperationMetadata>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      const CancellationToken& cancellation = {});
  std::unique_ptr<Impl> impl_;
};
/** @brief Resolves observation coordinates and complete-pixel closure metadata.
 * @note Pure checked transformations; no pixel reads or storage allocation.
 */
PHOTOSPIDER_API Result<Footprint> operation_observations(
    const OperationMetadata& output, const Footprint& samples,
    const FootprintLimits& limits = {});
PHOTOSPIDER_API Result<Footprint> observation_samples(
    const OperationMetadata& output, const Footprint& observations,
    const FootprintLimits& limits = {});
}  // namespace ps
