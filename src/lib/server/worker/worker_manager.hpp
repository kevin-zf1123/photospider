/**
 * @file worker_manager.hpp
 * @brief Declares the source-private Issue #100 worker lifecycle owner.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Closed trusted completion forms delivered after process
 * reconciliation.
 * @throws Nothing for value operations.
 */
enum class WorkerManagerCompletionKind : std::uint8_t {
  /** @brief One valid report joined with clean exit and exact reaping. */
  Report,
  /** @brief One typed manager fault after process revocation and reaping. */
  Failure,
  /**
   * @brief Accepted cancellation required actual owned signal escalation and
   * matching signal death plus exact reaping.
   */
  ForcedCancellation,
};

/**
 * @brief One exact manager completion delivered to control-plane authority.
 * @throws Nothing for default construction; contained values may allocate.
 * @note `Report` alone carries `report`; manager-owned forms carry a closed
 * failure and no image, artifact, quota, or retry claim.
 * Move construction/assignment must remain non-throwing so a completion that
 * left its local fail-stop construction boundary cannot escape into an outer
 * generic classifier during handoff.
 */
struct WorkerManagerCompletion final {
  /** @brief Exact retained assignment identity. */
  AttemptIdentity identity;
  /** @brief Closed completion form. */
  WorkerManagerCompletionKind kind = WorkerManagerCompletionKind::Failure;
  /** @brief Candidate report present only for `Report`. */
  std::optional<JobAttemptReport> report;
  /** @brief Typed manager failure for non-report forms. */
  JobAttemptFailure failure = JobAttemptFailure::WorkerStartup;
  /** @brief Trusted bounded supervisor diagnostic. */
  std::string message;
};

/** @brief Guards allocation-free post-construction completion handoff. */
static_assert(std::is_nothrow_move_constructible_v<WorkerManagerCompletion> &&
                  std::is_nothrow_move_assignable_v<WorkerManagerCompletion>,
              "WorkerManagerCompletion handoff must remain non-throwing");

/**
 * @brief Service callbacks invoked only outside the WorkerManager mutex.
 * @throws Nothing for default construction; function copies may allocate.
 */
struct WorkerManagerCallbacks final {
  /**
   * @brief Fences and publishes Running for one still-current assignment.
   * @note Returning false retires the record without spawning or completion.
   * A true result is the control-plane supervision fence and deliberately
   * precedes external process spawn, `AssignmentAccepted`, and heartbeats; it
   * is not an external-worker readiness observation.
   */
  std::function<bool(const AttemptIdentity&)> begin_assignment;
  /** @brief Observes monotonic cancel/fail-stop/replacement/shutdown intent. */
  std::function<bool(const AttemptIdentity&)> cancellation_requested;
  /**
   * @brief Applies one exact report or trusted post-reap terminal fact.
   * @note The callback must return only after accepting the handoff or entering
   * its own monotonic service fail-stop. Throwing means terminal-fact delivery
   * cannot be proven and allocation-free fail-stops WorkerManager before
   * completed-record marking or deletion.
   */
  std::function<void(WorkerManagerCompletion)> complete_assignment;
};

/**
 * @brief Mutex-consistent observation of manager-owned supervision handles.
 * @throws Nothing for value operations.
 * @note Counts expose no PID, descriptor, signal, cancellation, or lease
 * mutation capability.
 */
struct WorkerManagerOwnershipSnapshot final {
  /** @brief Records whose supervision tail has not completed. */
  std::size_t active = 0U;
  /** @brief Completed records waiting for reaper transfer. */
  std::size_t completed = 0U;
  /** @brief Supervision handles currently joining outside the manager mutex. */
  std::size_t joining = 0U;
  /** @brief Exact live child processes still owned by active records. */
  std::size_t live_processes = 0U;

  /**
   * @brief Returns every retained supervision handle.
   * @return Checked active/completed/joining sum.
   * @throws std::overflow_error on impossible `size_t` overflow.
   */
  std::size_t total() const;
};

/**
 * @brief Sole source-private owner of worker assignment and process lifecycle.
 *
 * One move-only record and supervision thread owns each exact attempt. Product
 * records prepare one assignment, create direction-reduced artifact streams,
 * fork/exec one fresh process, join every message to the complete identity,
 * and pump bulk bytes through nonblocking manager endpoints under the same
 * absolute startup/runtime/cancel/shutdown deadlines. Blocking data-plane I/O
 * remains in the killable worker. The manager escalates cancellation,
 * preserves bounded buffered report/EOF drainage when natural exit wins the
 * deadline-side reap, and prevents either an ordinary EOF or candidate-Report
 * deadline from preempting an active cooperative cancellation deadline. After
 * a final natural-exit observation, forced escalation delivers owned `SIGTERM`
 * while the control channel is still live, then revokes the channel before
 * bounded wait/KILL handling. It classifies exit and clears/reaps the PID
 * before delivering one completion.
 * The explicit test mode executes a marked factory in its supervision thread
 * and makes no process-isolation or bounded-termination claim.
 *
 * @throws Construction validates factory, callbacks, bounds, product
 * executable access, a waitable product `SIGCHLD` disposition, and creates one
 * internal supervision-thread reaper.
 * @note No API accepts a PID. Callbacks are never invoked while the manager
 * mutex is held, and `shutdown()` waits for no service mutex. After an
 * assignment begins, inability to construct or deliver its terminal fact
 * allocation-free fail-stops before completed-record marking or deletion.
 */
class WorkerManager final {
 public:
  /**
   * @brief Creates one empty lifecycle owner and starts its handle reaper.
   * @param factory Non-null externalizable or marked-test worker factory.
   * @param callbacks Complete control-plane callback set.
   * @param options Valid bounded process configuration.
   * @param in_process_test_mode True only for the non-installed test marker.
   * @throws std::invalid_argument for invalid configuration, including product
   * `SIGCHLD=SIG_IGN` or `SA_NOCLDWAIT` auto-reaping.
   * @throws std::system_error when `SIGCHLD` cannot be queried or the internal
   * reaper thread cannot start.
   * @throws std::bad_alloc when retained state allocation fails.
   */
  WorkerManager(std::shared_ptr<JobAttemptWorkerFactory> factory,
                WorkerManagerCallbacks callbacks, WorkerManagerOptions options,
                bool in_process_test_mode);

  /**
   * @brief Cancels, terminates, reaps, and joins all retained workers.
   * @throws Nothing; ordinary worker/signal/protocol/callback failures are
   * contained, while loss of exact reaping authority fail-stops the process.
   */
  ~WorkerManager() noexcept;

  /** @brief Prevents duplicate lifecycle ownership. */
  WorkerManager(const WorkerManager& other) = delete;
  /** @brief Prevents duplicate lifecycle assignment. */
  WorkerManager& operator=(const WorkerManager& other) = delete;

  /**
   * @brief Publishes and starts supervision for one fresh exact assignment.
   * @param assignment Complete immutable current assignment.
   * @return Nothing after one unique joinable supervision handle is retained.
   * @throws std::invalid_argument for invalid/shutdown input.
   * @throws std::logic_error for an attempt-identity collision.
   * @throws std::system_error when supervision-thread creation fails.
   * @throws std::bad_alloc when record/thread storage allocation fails.
   * @note Record identity retention precedes registry insertion and thread
   * construction. Directional stream creation and all data transfer occur only
   * in that registered supervisor after the service admission lock has been
   * released. Failed insertion retains no record; thread failure erases the
   * exact inserted record before propagating so the caller can roll back
   * admission authority.
   */
  void start(JobAssignment assignment);

  /**
   * @brief Marks monotonic cancellation for one exact retained lease.
   * @param identity Complete current attempt/worker/lease tuple.
   * @return True only when that exact active record exists.
   * @throws Nothing.
   * @note This method stores no PID and sends no signal. The owning supervision
   * thread performs every channel and process action after revalidation.
   */
  bool request_cancel(const AttemptIdentity& identity) noexcept;

  /**
   * @brief Reports whether one exact attempt still has a retained handle.
   * @param identity Complete assignment tuple.
   * @return True for active, completed-not-transferred, or joining ownership.
   * @throws Nothing.
   */
  bool owns_attempt(const AttemptIdentity& identity) const noexcept;

  /**
   * @brief Requests concurrent drainage and joins every worker and reaper.
   * @return Nothing after no live process or supervision handle remains.
   * @throws Nothing; repeated calls are idempotent. Exact reaping authority
   * loss fail-stops rather than returning incomplete ownership.
   */
  void shutdown() noexcept;

  /**
   * @brief Captures current supervision and live-process ownership counts.
   * @return Mutex-consistent non-authorizing observation.
   * @throws Nothing.
   */
  WorkerManagerOwnershipSnapshot ownership_snapshot() const noexcept;

  /**
   * @brief Waits until retained supervision ownership reaches a bound.
   * @param maximum_count Inclusive ownership upper bound.
   * @param timeout Nonnegative observer wait duration.
   * @return True when the bound was reached before timeout.
   * @throws std::invalid_argument for a negative timeout.
   * @throws std::system_error for condition-variable synchronization failure.
   */
  bool wait_for_owned_count_at_most(std::size_t maximum_count,
                                    std::chrono::milliseconds timeout) const;

 private:
  /** @brief Opaque POSIX/thread ownership implementation. */
  class Impl;
  /** @brief Sole implementation owner. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::server
