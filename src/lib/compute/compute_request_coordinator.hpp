#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "compute/compute_run.hpp"
#include "compute/compute_supersession.hpp"
#include "graph/graph_state_executor.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Per-live-Graph latest-wins domain and reserved-ticket pump.
 *
 * The coordinator allocates graph-wide generations, owns one bounded lineage
 * row/mailbox per admitted key, and runs at most one materialized request at a
 * time through the existing compute-request lane worker. It owns no OS thread,
 * ExecutionService worker, Run plan, staging Graph, resource grant,
 * graph-lifetime lease, or issue #76 registry.
 *
 * @throws std::bad_alloc when rows, callbacks, or candidate ownership allocate.
 * @throws std::system_error when coordinator/executor synchronization fails.
 * @note Generation publication and currency checks use `graph_state_`; ticket
 * wake is nonblocking and consumes a pre-reserved compute-lane admission.
 */
class ComputeRequestCoordinator final {
 public:
  /**
   * @brief Bounded state snapshot for deterministic private tests.
   * @throws Nothing for value construction and scalar access.
   * @note Counts own no coordinator state and may become stale immediately.
   */
  struct Snapshot {
    /** @brief Live lineage rows, bounded by charged/adopting keys. */
    std::size_t lineage_rows = 0;
    /** @brief Executor reservations adopted by lineage rows. */
    std::size_t reserved_tickets = 0;
    /** @brief Latest unmaterialized mailbox values. */
    std::size_t pending_candidates = 0;
    /** @brief Materialized/draining callbacks; invariant zero or one. */
    std::size_t active_candidates = 0;
    /** @brief Preparations that have not reached publication. */
    std::size_t provisional_adopters = 0;
    /** @brief Total charged compute-lane units. */
    std::size_t lane_admitted_units = 0;
  };

  /**
   * @brief Move-only generation preparation before candidate publication.
   *
   * The value retains one provisional-adopter count. When it created the
   * key's sole parked reservation, that ticket is retained by the lineage row
   * and guarded by this adopter count. Destruction before publication rolls
   * ownership back without changing the current generation.
   *
   * @throws Nothing from movement and destruction.
   * @note The coordinator and both executor lanes must outlive this value.
   */
  class PreparedCandidate final {
   public:
    /** @brief Prevents construction without coordinator-owned preparation. */
    PreparedCandidate() = delete;

    /** @brief Transfers provisional ownership from another value. */
    PreparedCandidate(PreparedCandidate&& other) noexcept;

    /**
     * @brief Replaces this preparation after rolling back its prior ownership.
     * @param other Preparation left inactive.
     * @return Reference to this value.
     * @throws Nothing; invariant failures terminate during rollback.
     */
    PreparedCandidate& operator=(PreparedCandidate&& other) noexcept;

    /** @brief Rolls back unpublished adoption/reservation ownership. */
    ~PreparedCandidate() noexcept;

    /** @brief Prevents duplicate publication ownership. */
    PreparedCandidate(const PreparedCandidate&) = delete;
    /** @brief Prevents duplicate publication assignment. */
    PreparedCandidate& operator=(const PreparedCandidate&) = delete;

    /**
     * @brief Returns the allocated immutable lineage identity.
     * @return Borrowed identity valid for this preparation lifetime.
     * @throws Nothing.
     */
    const SupersessionIdentity& identity() const noexcept { return identity_; }

   private:
    friend class ComputeRequestCoordinator;

    /**
     * @brief Owns one prepared identity and optional provisional ticket.
     * @param coordinator Domain that owns its provisional-adopter count.
     * @param identity Fresh graph-wide identity.
     * @throws Nothing.
     */
    PreparedCandidate(ComputeRequestCoordinator* coordinator,
                      SupersessionIdentity identity) noexcept;

    /** @brief Rolls back this still-active preparation without throwing. */
    void reset() noexcept;

    /** @brief Borrowed domain, null after publish/move/rollback. */
    ComputeRequestCoordinator* coordinator_ = nullptr;
    /** @brief Fresh immutable key and graph-wide generation. */
    SupersessionIdentity identity_;
  };

  /** @brief Materialized request callback executed on the existing lane worker.
   */
  using ExecuteCallback = std::function<void()>;
  /** @brief Exact-once completion for an unmaterialized superseded candidate.
   */
  using SupersededCallback = std::function<void()>;
  /** @brief Fallback completion for an unexpected callback escape. */
  using FailureCallback = std::function<void(std::exception_ptr)>;

  /**
   * @brief Binds one Graph domain to its graph-state and request lanes.
   * @param graph_state Serialized publication/commit authority.
   * @param compute_requests Total-admission reserved-ticket lane.
   * @param first_generation Injectable first graph-wide generation.
   * @throws std::invalid_argument for zero generation or incompatible lane use.
   * @note The lanes and Graph outlive this coordinator.
   */
  ComputeRequestCoordinator(GraphStateExecutor& graph_state,
                            GraphStateExecutor& compute_requests,
                            std::uint64_t first_generation = 1);

  /**
   * @brief Retires all already-drained private row ownership.
   * @throws Nothing; GraphRuntime drains the request lane before destruction.
   */
  ~ComputeRequestCoordinator() noexcept;

  /** @brief Prevents copying one Graph's lineage authority. */
  ComputeRequestCoordinator(const ComputeRequestCoordinator&) = delete;
  /** @brief Prevents replacing one Graph's lineage authority. */
  ComputeRequestCoordinator& operator=(const ComputeRequestCoordinator&) =
      delete;

  /**
   * @brief Allocates identity and acquires/reuses key ticket admission.
   * @param key Canonical target/request-intent lineage.
   * @return Move-only preparation that leaves current publication unchanged.
   * @throws std::overflow_error when graph-wide generation is exhausted.
   * @throws std::runtime_error after coordinator or lane admission stops.
   * @throws std::bad_alloc for row/ticket/callback ownership failure.
   * @throws std::system_error for synchronization failure.
   * @note Capacity waiting occurs only when this key has no reusable ticket,
   * and occurs without graph-state or coordinator mutex ownership.
   */
  PreparedCandidate prepare(const SupersessionKey& key);

  /**
   * @brief Publishes one prepared candidate through graph-state ordering.
   * @param prepared Fresh identity and provisional-adopter ownership.
   * @param cancellation Request-wide source attached to every materialized Run.
   * @param execute Existing possibly blocking Kernel/ComputeService path.
   * @param settle_superseded Exact-once pending/born-stale completion callback.
   * @param settle_failure Unexpected callback-escape completion callback.
   * @return Nothing after the graph-state publication work item is admitted.
   * @throws std::invalid_argument for null/empty ownership callbacks.
   * @throws std::bad_alloc for candidate ownership or graph-state submission.
   * @throws std::runtime_error when close rejects publication.
   * @throws std::system_error for executor/coordinator synchronization.
   * @note The admitted graph-state item later linearizes publication in FIFO
   * order. A newer publication replaces one mailbox value, nonblockingly
   * requests Superseded on the active source, settles displaced pending work,
   * and wakes only the adopted reserved ticket. The caller waits for neither
   * graph-state availability nor execution quiescence after admission.
   */
  void publish(PreparedCandidate prepared,
               std::shared_ptr<ComputeRequestCancellationSource> cancellation,
               ExecuteCallback execute, SupersededCallback settle_superseded,
               FailureCallback settle_failure);

  /**
   * @brief Tests exact current key/generation currency.
   * @param identity Run-captured lineage version.
   * @return True only while the row's current generation matches exactly.
   * @throws std::system_error if coordinator synchronization fails.
   * @note Product commit calls this inside the graph-state transaction after
   * the Run contender is claimed and before persistence/publication.
   */
  bool is_current(const SupersessionIdentity& identity) const;

  /**
   * @brief Rejects new preparations/publications before lane close.
   * @return Nothing.
   * @throws std::system_error if coordinator synchronization fails.
   * @note Published pending/active work remains accepted and is drained by its
   * existing ticket; this method requests no lifecycle cancellation.
   */
  void stop_admission();

  /**
   * @brief Copies the current bounded ownership counts for tests.
   * @return Scalar snapshot captured under coordinator and lane locks.
   * @throws std::system_error if either synchronization primitive fails.
   * @note Counts may change immediately after return and grant no authority.
   */
  Snapshot snapshot() const;

 private:
  /** @brief Fully prepared logical request retained by mailbox/active turn. */
  struct Candidate {
    /** @brief Immutable key/generation shared by its future child Runs. */
    SupersessionIdentity identity;
    /** @brief Request-wide weak-child cancellation fan-out authority. */
    std::shared_ptr<ComputeRequestCancellationSource> cancellation;
    /** @brief Existing blocking compute path executed by the lane worker. */
    ExecuteCallback execute;
    /** @brief Exact-once completion when no Run materializes. */
    SupersededCallback settle_superseded;
    /** @brief Completion when execute violates its containment contract. */
    FailureCallback settle_failure;
  };

  /** @brief Bounded ownership for one canonical supersession key. */
  struct LineageRow {
    /** @brief Latest published generation, absent before first publication. */
    std::optional<SupersessionGeneration> current_generation;
    /** @brief Single executor reservation reused by every key generation. */
    GraphStateExecutor::ContinuationTicket ticket;
    /** @brief Latest unmaterialized published candidate. */
    std::shared_ptr<Candidate> pending;
    /** @brief Currently executing/draining candidate, if this key is active. */
    std::shared_ptr<Candidate> active;
    /** @brief Preparations keeping the row/ticket adoptable before publication.
     */
    std::size_t provisional_adopters = 0;
    /** @brief True while one adopter reserves the sole ticket without locks. */
    bool reservation_in_progress = false;
  };

  /**
   * @brief Executes at most one mailbox candidate for one ticket turn.
   * @param key Lineage whose row owns the running ticket.
   * @param ticket Exact running reservation handle.
   * @return Worker-tail action after physical settlement.
   * @throws Nothing; candidate exceptions are sent to settle_failure.
   */
  GraphStateExecutor::ContinuationAction pump_turn(
      const SupersessionKey& key,
      const GraphStateExecutor::ContinuationTicket& ticket) noexcept;

  /**
   * @brief Rolls back one unpublished preparation and optional ticket.
   * @param identity Prepared key/generation whose adopter count is released.
   * @return Nothing.
   * @throws Synchronization failures; PreparedCandidate converts them to a
   * fatal invariant because destruction is noexcept.
   */
  void abandon_preparation(const SupersessionIdentity& identity);

  /**
   * @brief Requests one reason without letting cleanup failure strand state.
   * @param source Active or displaced request source; null is a no-op.
   * @return Nothing.
   * @throws Nothing; source callback failures are contained.
   */
  static void request_superseded_noexcept(
      const std::shared_ptr<ComputeRequestCancellationSource>& source) noexcept;
  /**
   * @brief Settles one unmaterialized value without escaping the graph lane.
   * @param candidate Candidate whose exact-once owner receives supersession.
   * @return Nothing.
   * @throws Nothing; completion callback failures are contained.
   */
  static void settle_superseded_noexcept(
      const std::shared_ptr<Candidate>& candidate) noexcept;
  /**
   * @brief Settles one callback escape without throwing from lane work.
   * @param candidate Candidate whose completion receives the failure.
   * @param failure Exact callback or infrastructure exception.
   * @return Nothing.
   * @throws Nothing; completion callback failures are contained.
   */
  static void settle_failure_noexcept(
      const std::shared_ptr<Candidate>& candidate,
      std::exception_ptr failure) noexcept;

  /** @brief Serialized visible-generation lane. */
  GraphStateExecutor& graph_state_;
  /** @brief Total-admission lane owning every pump ticket and turn. */
  GraphStateExecutor& compute_requests_;
  /** @brief Protects allocator, rows, admission, and single-active state. */
  mutable std::mutex mutex_;
  /** @brief Releases same-key adopters after sole ticket reservation resolves.
   */
  std::condition_variable reservation_changed_;
  /** @brief Checked graph-wide allocator serialized by `mutex_`. */
  SupersessionGenerationAllocator generation_allocator_;
  /** @brief Canonical key to bounded lineage row. */
  std::map<SupersessionKey, LineageRow> rows_;
  /** @brief True only while new preparation/publication is accepted. */
  bool accepting_ = true;
  /** @brief Graph-wide logical runner owner; at most one row is active. */
  std::optional<SupersessionKey> active_key_;
};

}  // namespace ps::compute
