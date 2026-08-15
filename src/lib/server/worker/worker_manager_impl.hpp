#pragma once

/**
 * @file worker_manager_impl.hpp
 * @brief Declares the source-private WorkerManager record registry.
 *
 * Method bodies are separated by lifecycle, process, and monitor ownership.
 * This header is not installed and does not change WorkerManager ABI.
 */

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "server/worker/worker_manager_posix.hpp"

namespace ps::server {

/**
 * @brief Complete private WorkerManager implementation and record registry.
 * @throws Constructor behavior is documented by `WorkerManager`.
 */
class WorkerManager::Impl final {
 public:
  /**
   * @brief One move-only exact assignment/supervision ownership record.
   * @throws Construction may allocate while retaining assignment values.
   */
  struct Record final {
    /**
     * @brief Retains one exact immutable assignment before thread creation.
     * @param value Assignment moved into sole record ownership.
     * @param fail_construction_for_test Optional one-shot identity-retention
     * allocation fault used only by source-private tests.
     * @throws std::bad_alloc when identity retention or its test seam fails.
     */
    explicit Record(
        JobAssignment value,
        const std::shared_ptr<std::atomic<bool>>& fail_construction_for_test)
        : identity(retain_identity(value.identity, fail_construction_for_test)),
          assignment(std::move(value)) {}

    /**
     * @brief Copies the exact identity at the retained-record boundary.
     * @param value Valid assignment identity to retain.
     * @param fail_construction_for_test Optional one-shot test fault.
     * @return Independent retained identity.
     * @throws std::bad_alloc when the test fault is consumed or copying fails.
     */
    static AttemptIdentity retain_identity(
        const AttemptIdentity& value,
        const std::shared_ptr<std::atomic<bool>>& fail_construction_for_test) {
      if (fail_construction_for_test != nullptr &&
          fail_construction_for_test->exchange(false,
                                               std::memory_order_acq_rel)) {
        throw std::bad_alloc();
      }
      return value;
    }

    /** @brief Complete immutable attempt identity used for every action. */
    AttemptIdentity identity;
    /** @brief Exact immutable assignment retained through supervision. */
    JobAssignment assignment;
    /** @brief Supervisor-created attempt-local stream data-plane ownership. */
    WorkerArtifactDataPlane data_plane;
    /** @brief Sole joinable supervision-thread handle. */
    std::thread supervisor;
    /** @brief Monotonic external cancellation request. */
    bool cancellation_requested = false;
    /** @brief Monotonic manager shutdown request. */
    bool shutdown_requested = false;
    /** @brief Whether callback/terminal processing reached its final tail. */
    bool completed = false;
    /** @brief Exact live child PID, or -1 before spawn/after reaping. */
    pid_t pid = -1;
  };

  /**
   * @brief Validates configuration and starts one internal handle reaper.
   * @param factory Non-null worker factory.
   * @param callbacks Complete service callback set.
   * @param options Bounded manager policy.
   * @param in_process_test_mode Explicit non-installed test execution mode.
   * @throws As `WorkerManager::WorkerManager`.
   */
  Impl(std::shared_ptr<JobAttemptWorkerFactory> factory,
       WorkerManagerCallbacks callbacks, WorkerManagerOptions options,
       bool in_process_test_mode);

  /**
   * @brief Drains all records before member teardown.
   * @throws Nothing.
   */
  ~Impl() noexcept;

  /** @copydoc WorkerManager::start */
  void start(JobAssignment assignment);

  /** @copydoc WorkerManager::request_cancel */
  bool request_cancel(const AttemptIdentity& identity) noexcept;

  /** @copydoc WorkerManager::owns_attempt */
  bool owns_attempt(const AttemptIdentity& identity) const noexcept;

  /** @copydoc WorkerManager::shutdown */
  void shutdown() noexcept;

  /** @copydoc WorkerManager::ownership_snapshot */
  WorkerManagerOwnershipSnapshot ownership_snapshot() const noexcept;

  /** @copydoc WorkerManager::wait_for_owned_count_at_most */
  bool wait_for_owned_count_at_most(std::size_t maximum_count,
                                    std::chrono::milliseconds timeout) const;

 private:
  /**
   * @brief Validates every callback, duration, factory, and product executable.
   * @return Nothing after every applicable invariant is validated.
   * @throws std::invalid_argument for any fail-closed configuration error.
   * @throws std::bad_alloc when a field-specific duration diagnostic cannot be
   * retained.
   * @throws std::system_error when the product `SIGCHLD` action cannot be
   * queried.
   * @note Explicit in-process test mode validates common bounds but neither
   * creates a child nor claims process-global reaping authority.
   */
  void validate_configuration() const;

  /**
   * @brief Runs one exact record and contains every exception at thread scope.
   * @param record Shared stable record retained by the registry and thread.
   * @return Nothing after optional completion callback and completed marking.
   * @throws Nothing; first or fallback completion construction failure and
   * completion-callback failure allocation-free fail-stop the authority before
   * completed-record marking or deletion.
   * @note `begin_assignment()` returning false is the sole no-completion
   * retirement path. Once begin succeeds or raises, supervision must deliver
   * one typed terminal fact or fail-stop while retaining the record.
   */
  void supervise(const std::shared_ptr<Record>& record) noexcept;

  /**
   * @brief Executes the explicit deterministic in-process test marker path.
   * @param record Exact assignment record.
   * @return One report completion without an OS-isolation claim.
   * @throws Worker creation/execution fact preparation failures may propagate
   * to the thread classifier. First `Report` completion construction never
   * propagates: any failure allocation-free fail-stops before callback or
   * record retirement.
   */
  WorkerManagerCompletion run_in_process(const std::shared_ptr<Record>& record);

  /**
   * @brief Forks and immediately execs one fresh exact worker process.
   * @param record Exact assignment record whose PID registry is updated.
   * @return Live child and private parent socket.
   * @throws ManagerFailure for setup, fork, resource-limit, or exec failure.
   * @throws std::system_error for pre-fork descriptor, platform-closure, or
   * address-space-limit setup failure.
   * @throws std::overflow_error if the captured monotonic base cannot
   * represent the validated startup deadline.
   * @throws std::bad_alloc when retaining the executable, bootstrap arguments,
   * or a setup diagnostic exhausts memory.
   * @note The fork child performs only async-signal-safe descriptor, limit,
   * status-write, and exec operations using storage prepared before fork.
   * Darwin closes fd 7 through the kernel `kern.maxfilesperproc` ceiling;
   * Linux uses raw `close_range(7, UINT_MAX, 0)` and reports any unavailable or
   * rejected syscall through close-on-exec fd 4. The parent revalidates
   * waitable `SIGCHLD` policy immediately before `fork` and fail-stops if
   * process-global exact-reaping authority changed. A finite inherited hard
   * `RLIMIT_FSIZE` below the accepted output-stage maximum fails as
   * `WorkerStartup` before `fork`; it is never used to narrow that maximum.
   */
  ChildProcess spawn_process(const std::shared_ptr<Record>& record);

  /**
   * @brief Pumps checkpoint bytes while the exact startup child is killable.
   * @param record Exact retained assignment and cancellation owner.
   * @param process Non-null live child and checkpoint-stream owner.
   * @param deadline Absolute assignment-startup deadline shared with accept.
   * @return Empty after exact bytes plus EOF commit; otherwise one terminal
   * cancellation/failure completion after exact process reaping.
   * @throws std::invalid_argument when `process` is null.
   * @throws ManagerFailure for timeout, poll, or premature stream closure.
   * @throws std::overflow_error through bounded termination deadlines.
   * @note The manager endpoint is nonblocking. Every transfer slice revisits
   * shutdown/cancellation and the absolute deadline; only the worker performs
   * blocking data-plane receive and can therefore be terminated and reaped.
   */
  std::optional<WorkerManagerCompletion> transfer_checkpoint(
      const std::shared_ptr<Record>& record, ChildProcess* process,
      std::chrono::steady_clock::time_point deadline);

  /**
   * @brief Performs one fixed nonblocking receive into final output storage.
   * @param process Non-null child/data-plane owner.
   * @param image Optional exact final image owner for a successful Report.
   * @param expected_bytes Metadata-declared payload length, possibly zero.
   * @param received_bytes Non-null exact direct-receive offset.
   * @param hasher Non-null allocation-free incremental integrity owner.
   * @param output_eof Non-null monotonic exact-EOF observation.
   * @param output_digest Non-null finalized digest destination.
   * @return Progress, WouldBlock, or EndOfStream from exactly one receive.
   * @throws Data-plane validation or socket failures unchanged.
   * @note The call never loops, waits, allocates, or copies prior bytes. Its
   * caller returns to cancellation/shutdown/runtime/heartbeat arbitration
   * before any second bulk operation.
   */
  WorkerDataPlaneIoStatus drain_output_slice(
      ChildProcess* process, const std::optional<ImageBuffer>& image,
      std::size_t expected_bytes, std::size_t* received_bytes,
      ArtifactContentHasher* hasher, bool* output_eof,
      std::optional<ArtifactContentDigest>* output_digest);

  /**
   * @brief Runs the complete assignment/heartbeat/report/exit state machine.
   * @param record Exact immutable attempt record.
   * @return One report, failure, or forced-cancellation completion after reap.
   * @throws ManagerFailure for spawn or exact-cleanup failures that the outer
   * supervision-thread classifier must publish.
   * @throws std::system_error when pre-fork platform setup fails.
   * @throws std::overflow_error when spawn or exact cleanup cannot represent a
   * validated monotonic deadline.
   * @throws std::bad_alloc when pre-fork path/bootstrap storage cannot be
   * retained. First completion construction never propagates and instead
   * allocation-free fail-stops.
   * @note The exact child PID is registered before prepared graph material is
   * copied. The non-virtual catalog lookup performs no trusted I/O; every later
   * failure therefore has one signalable and reapable process owner. After
   * spawn, standard exceptions from the ordinary protocol/monitor path are
   * converted to typed failure only after exact reaping; test-only observation
   * or cleanup exceptions still propagate to the thread-scope classifier.
   * Assignment acceptance becomes authoritative only after exact identity
   * decoding while still strictly before the captured startup deadline.
   */
  WorkerManagerCompletion run_external_process(
      const std::shared_ptr<Record>& record);

  /**
   * @brief Monitors one accepted worker until exact exit/reap classification.
   * @param record Exact assignment record.
   * @param process Non-null live/reap-tracked child owner.
   * @return One completion only after `process` is reaped.
   * @throws Worker protocol exceptions and channel failures unrelated to an
   * accepted cancellation for the outer classifier. Accepted-cancel channel
   * failures remain inside this bounded process/wait-status state machine.
   * @throws std::invalid_argument when `process` is null.
   * @throws ManagerFailure when a deterministic test-only exit observation
   * expires or observes an invalid zero-exit state.
   * @throws std::overflow_error if a captured monotonic base cannot represent
   * one of the validated lifecycle deadlines.
   * @throws std::bad_alloc when bounded frame/report reconstruction exhausts
   * memory.
   * @note Short read slices share one stateful frame decoder. Partial bytes and
   * a complete transport frame remain decoder-owned across readiness or
   * semantic-acceptance timeout; identity/report interpretation commits the
   * frame only at a fresh strict-before sample, before any lifecycle mutation.
   * Cancellation-send failure starts the same cooperative deadline and keeps
   * draining worker report/EOF/exit truth. Every ordinary EOF or
   * candidate-Report deadline is subordinated through one shared arbitration
   * to the still-active cooperative deadline, so it cannot terminate/reap
   * first and exact wait status can still outrank channel or protocol loss. If
   * the deadline-side exact observation reaps a
   * natural exit before channel revocation, the monitor keeps that descriptor
   * open and drains the stateful decoder through one bounded post-reap window;
   * only matching delivered TERM/KILL escalation yields
   * `ForcedCancellation`.
   * Source-private observations may record the first exact external heartbeat
   * and the latest one-based liveness-eligible Heartbeat ordinal accepted
   * while candidate output remains pending. They change no deadline and grant
   * no process, channel, payload, cancellation, completion, artifact, or quota
   * authority.
   * Socket-read poll budgets and semantic lifecycle acceptance deadlines are
   * separate: pending bulk uses a due poll budget for one nonblocking control
   * probe, while a ready frame remains acceptable only before the earliest
   * runtime/heartbeat/cancel/report/post-reap lifecycle deadline. A probe
   * timeout retains decoder state and yields exactly one bounded bulk slice.
   * A worker waits for one identity-only `CompletionReady`
   * acknowledgement. The manager sends it only after nonblocking EOF, bounded
   * hashing, reference/descriptor/resource joins, and image reconstruction;
   * an already unavailable cancellation channel skips the impossible reply but
   * does not erase a completely joined worker fact. Authenticated heartbeats
   * queued before readiness are accepted without renewing an inactive
   * heartbeat deadline. Once exact reap is observed, this monitor never reads
   * the bulk lane and never creates a successful candidate mapping.
   * Every first `Report`, `Failure`, or `ForcedCancellation` construction is
   * locally fail-stop protected after exact reaping and cannot be reclassified
   * by the outer catch boundary.
   */
  WorkerManagerCompletion monitor_process(const std::shared_ptr<Record>& record,
                                          ChildProcess* process);

  /**
   * @brief Observes manager/service monotonic cancellation without lock
   * nesting.
   * @param record Exact retained record.
   * @return True for manager request, shutdown, or service fencing intent.
   * @throws Nothing; callback failures conservatively request cancellation.
   */
  bool cancellation_requested(
      const std::shared_ptr<Record>& record) const noexcept;

  /**
   * @brief Builds one first manager-owned failure completion.
   * @param identity Exact retained assignment identity.
   * @param failure Closed manager failure category.
   * @param message Trusted supervisor diagnostic.
   * @return Complete failure completion.
   * @throws Nothing; any construction failure allocation-free fail-stops.
   */
  WorkerManagerCompletion failure_completion(
      const AttemptIdentity& identity, JobAttemptFailure failure,
      const char* message) const noexcept;

  /**
   * @brief Builds one prefixed first failure completion within the fail-stop
   * construction boundary.
   * @param identity Exact retained assignment identity.
   * @param failure Closed manager failure category.
   * @param prefix Trusted diagnostic prefix, or null for none.
   * @param detail Trusted diagnostic suffix, or null for none.
   * @return Complete failure completion.
   * @throws Nothing; any construction failure allocation-free fail-stops.
   */
  WorkerManagerCompletion prefixed_failure_completion(
      const AttemptIdentity& identity, JobAttemptFailure failure,
      const char* prefix, const char* detail) const noexcept;

  /**
   * @brief Builds one wait-status first failure completion within the fail-stop
   * construction boundary.
   * @param identity Exact retained assignment identity.
   * @param failure Closed manager failure category.
   * @param status Exact status returned by `waitpid`.
   * @return Complete failure completion.
   * @throws Nothing; formatting or value-retention failure allocation-free
   * fail-stops.
   */
  WorkerManagerCompletion wait_status_failure_completion(
      const AttemptIdentity& identity, JobAttemptFailure failure,
      int status) const noexcept;

  /**
   * @brief Builds one first forced-cancellation completion after exact reaping.
   * @param identity Exact retained assignment identity.
   * @param message Trusted escalation diagnostic.
   * @return Complete forced-cancellation fact.
   * @throws Nothing; any construction failure allocation-free fail-stops.
   */
  WorkerManagerCompletion forced_cancellation_completion(
      const AttemptIdentity& identity, const char* message) const noexcept;

  /**
   * @brief Builds one first report completion after in-process or exact-reap
   * execution.
   * @param identity Exact retained assignment identity.
   * @param report Candidate worker report to transfer.
   * @return Complete report fact.
   * @throws Nothing; any construction failure allocation-free fail-stops.
   */
  WorkerManagerCompletion report_completion(
      const AttemptIdentity& identity,
      JobAttemptReport&& report) const noexcept;

  /**
   * @brief Injects one deterministic allocation failure at a selected first
   * terminal-completion constructor.
   * @param point Constructor boundary currently entered.
   * @return Nothing when the source-private selection is absent or different.
   * @throws std::bad_alloc after atomically consuming an exact selected point.
   */
  void inject_initial_completion_construction_failure_for_test(
      WorkerManagerCompletionConstructionPointForTest point) const;

  /**
   * @brief Injects one deterministic allocation failure during completion
   * reconstruction.
   * @return Nothing when the source-private gate is absent or disarmed.
   * @throws std::bad_alloc after atomically consuming one armed test request.
   * @note Product configuration leaves the gate null. The exception is raised
   * at the real supervisor reconstruction boundary without replacing the
   * process allocator or exposing an installed fault-control surface.
   */
  void inject_completion_construction_failure_for_test() const;

  /**
   * @brief Publishes one exact live PID into its immutable record.
   * @param record Exact retained record.
   * @param pid Positive freshly forked child PID.
   * @throws Nothing.
   * @note An impossible post-fork publication transition fail-stops before the
   * child can escape the sole lifecycle authority.
   */
  void set_live_pid(const std::shared_ptr<Record>& record, pid_t pid) noexcept;

  /**
   * @brief Clears one PID immediately after exact successful waitpid reaping.
   * @param record Exact retained record.
   * @param pid Exact reaped PID.
   * @throws Nothing.
   * @note An ownership mismatch after exact `waitpid` fail-stops rather than
   * leaving a stale live-PID record eligible for ordinary completion.
   */
  void clear_reaped_pid(const std::shared_ptr<Record>& record,
                        pid_t pid) noexcept;

  /**
   * @brief Signals only the PID retained by one exact current record.
   * @param record Exact retained record.
   * @param pid Candidate child PID, which must equal retained ownership.
   * @param signal_number `SIGTERM` or `SIGKILL`.
   * @return Whether delivery occurred, the process vanished, or signaling was
   * rejected.
   * @throws Nothing.
   * @note A retained record/PID mismatch or synchronization failure fail-stops
   * because the caller can no longer prove exact lifecycle authority.
   */
  OwnedSignalResult signal_owned(const std::shared_ptr<Record>& record,
                                 pid_t pid, int signal_number) noexcept;

  /**
   * @brief Performs one nonblocking exact waitpid observation.
   * @param record Exact retained record.
   * @param process Non-null child state.
   * @return True when this call or an earlier call has reaped the child.
   * @throws std::invalid_argument when `process` is null.
   * @note The source-private test gate may suppress the syscall to exercise
   * final deadline handling; product configuration leaves that gate null. A
   * non-interruption error, including `ECHILD`, means exact status authority
   * was lost and fail-stops without a completion callback or record deletion.
   */
  bool observe_exit(const std::shared_ptr<Record>& record,
                    ChildProcess* process);

  /**
   * @brief Waits nonblocking for one exact child until a local deadline.
   * @param record Exact retained record.
   * @param process Non-null child state.
   * @param deadline Absolute monotonic deadline.
   * @return True when reaped before deadline.
   * @throws std::invalid_argument when `process` is null.
   * @throws std::overflow_error if the monotonic clock cannot represent the
   * next bounded poll slice.
   * @note Reaping authority loss fail-stops rather than throwing.
   */
  bool wait_for_exit_until(const std::shared_ptr<Record>& record,
                           ChildProcess* process,
                           std::chrono::steady_clock::time_point deadline);

  /**
   * @brief Observes natural exit or revokes, escalates, and reaps one child.
   * @param record Exact retained assignment record.
   * @param process Non-null process owner.
   * @return Whether exact reaping preceded channel revocation, followed
   * revocation without matching signal death, or matched delivered owned
   * escalation.
   * @throws std::invalid_argument when `process` is null.
   * @throws ManagerFailure when the test-only pre-signal observation expires or
   * observes an abnormal exit.
   * @throws std::overflow_error if a captured monotonic base cannot represent
   * a validated termination or reap deadline.
   * @note Exact natural exit observed before revocation leaves `control` open
   * for the caller's bounded report/EOF drain. Every signal revalidates
   * complete record/PID ownership under mutex. Missing the final reap deadline
   * or losing exact wait authority fail-stops the authority process; this
   * function never falls back to an unbounded `waitpid` or a recoverable
   * manager completion.
   */
  TerminateAndReapResult terminate_and_reap(
      const std::shared_ptr<Record>& record, ChildProcess* process);

  /**
   * @brief Marks one supervision record complete and wakes the handle reaper.
   * @param record Exact retained record whose thread is returning.
   * @throws Nothing.
   * @note A retained live PID or synchronization failure fail-stops before the
   * record becomes eligible for callback-independent deletion.
   */
  void mark_completed(const std::shared_ptr<Record>& record) noexcept;

  /**
   * @brief Reports whether any completed supervision handle can be transferred.
   * @return True when one map record is completed.
   * @throws Nothing.
   * @note Caller holds `mutex_`.
   */
  bool has_completed_record_locked() const noexcept;

  /**
   * @brief Joins completed supervision threads outside the manager mutex.
   * @return Nothing after shutdown and complete record drainage.
   * @throws Nothing.
   * @note A completed record that still retains a live PID fail-stops before
   * map erasure or supervisor-handle transfer.
   */
  void reap_supervisors() noexcept;

  /** @brief Shared factory retained through every supervision thread. */
  std::shared_ptr<JobAttemptWorkerFactory> factory_;
  /** @brief Complete callback set invoked without the manager mutex. */
  WorkerManagerCallbacks callbacks_;
  /** @brief Immutable bounded process lifecycle configuration. */
  WorkerManagerOptions options_;
  /** @brief Explicit non-installed deterministic test-only mode. */
  bool in_process_test_mode_ = false;
  /** @brief Serializes exact record/PID/cancel/handle ownership. */
  mutable std::mutex mutex_;
  /** @brief Wakes supervisors, reaper, shutdown, and test observers. */
  mutable std::condition_variable condition_;
  /** @brief JobAttemptId text to exact active/completed record. */
  std::map<std::string, std::shared_ptr<Record>> records_;
  /** @brief Sole record currently joining outside `mutex_`, if any. */
  std::shared_ptr<Record> joining_record_;
  /** @brief Monotonic shutdown and no-new-assignment state. */
  bool shutting_down_ = false;
  /** @brief Sole infrastructure thread that joins supervision handles. */
  std::thread reaper_;
};

}  // namespace ps::server
