#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ipc/session_registry.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/host/compute_request.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Private daemon lifecycle state for one accepted compute request.
 *
 * @throws Nothing.
 * @note Values advance only from Queued to Running and then to one terminal
 *       Succeeded or Failed state.
 */
enum class ComputeRequestState {
  /** @brief Accepted work waiting for the sole registry worker. */
  Queued,
  /** @brief Work dequeued by the worker and entering its Host executor. */
  Running,
  /** @brief Immutable terminal work with a successful nested status. */
  Succeeded,
  /** @brief Immutable terminal work with a failed nested status. */
  Failed,
};

/**
 * @brief Matching synchronous Host operation selected for one job.
 *
 * @throws Nothing.
 * @note Status invokes `Host::compute`; Image invokes exactly one
 *       `Host::compute_and_get_image` operation before output publication.
 */
enum class ComputeResultMode {
  /** @brief Retain only the exact Host compute status. */
  Status,
  /** @brief Publish the exact Host image result through the output callback. */
  Image,
};

/**
 * @brief Opaque daemon-global identity for one private compute record.
 *
 * @throws std::bad_alloc when copied storage cannot be allocated.
 * @note Version 2 values use the same 32-lowercase-hex shape as other opaque
 *       identities, but this type is not interchangeable with a session id.
 */
struct ComputeRequestId {
  /** @brief Opaque value generated and reserved before queue publication. */
  std::string value;
};

/**
 * @brief Move-only ownership of one private OutputStore job reference.
 *
 * The registry retains only a stable private output id and exact-once cleanup;
 * filesystem metadata and delivery leases remain owned by OutputStore.
 *
 * @throws std::bad_alloc when reference or callback storage is constructed.
 * @note Cleanup executes outside the compute-registry mutex on optional
 *       lease-aware release, eviction, TTL expiry, failed publication cleanup,
 *       or shutdown. Worker-side eviction cleanup precedes its replacement
 *       terminal snapshot, while failed-publication cleanup precedes that
 *       request's failed terminal snapshot.
 */
class ComputeOutputOwnership {
 public:
  /**
   * @brief Cleanup callback for one successfully published output.
   *
   * @param delivery_id Optional stable lease identity to release atomically
   *        with job ownership.
   * @throws Nothing; implementations must contain filesystem failures.
   * @note The registry never holds its mutex while invoking an active callback,
   *       so nonblocking registry lookups or releases may be re-entered.
   *       Worker-thread callbacks must neither synchronously join that worker
   *       nor wait for the current request's session admission release.
   */
  using Cleanup =
      std::function<void(const std::optional<std::string>& delivery_id)>;

  /**
   * @brief Creates inactive ownership for a successful empty image.
   * @throws Nothing.
   */
  ComputeOutputOwnership() = default;

  /**
   * @brief Creates active ownership for one stable output reference.
   *
   * @param reference Private reference copied into terminal snapshots.
   * @param cleanup Exact-once cleanup callback.
   * @throws std::bad_alloc if owned storage cannot be allocated.
   * @throws std::invalid_argument if either input is empty.
   */
  ComputeOutputOwnership(std::string reference, Cleanup cleanup);

  /**
   * @brief Invokes pending cleanup while containing callback failures.
   * @throws Nothing.
   */
  ~ComputeOutputOwnership() noexcept;

  /**
   * @brief Prevents duplicate cleanup ownership.
   * @throws Nothing because construction is unavailable.
   */
  ComputeOutputOwnership(const ComputeOutputOwnership&) = delete;

  /**
   * @brief Prevents duplicate cleanup ownership by assignment.
   * @return No value because copying is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ComputeOutputOwnership& operator=(const ComputeOutputOwnership&) = delete;

  /**
   * @brief Transfers one output cleanup obligation.
   * @param other Ownership that becomes inactive.
   * @throws Nothing.
   */
  ComputeOutputOwnership(ComputeOutputOwnership&& other) noexcept;

  /**
   * @brief Cleans any current output and transfers another obligation.
   * @param other Ownership that becomes inactive.
   * @return This ownership after transfer.
   * @throws Nothing.
   */
  ComputeOutputOwnership& operator=(ComputeOutputOwnership&& other) noexcept;

  /**
   * @brief Reports whether this object owns one output cleanup.
   * @return True only for a nonempty reference with a pending callback.
   * @throws Nothing.
   */
  bool active() const noexcept;

  /**
   * @brief Returns the stable reference for active ownership.
   * @return Owned reference, or empty text for an inactive value.
   * @throws Nothing.
   */
  const std::string& reference() const noexcept;

  /**
   * @brief Runs pending cleanup and makes this value inactive.
   * @param delivery_id Optional matching delivery lease to release in the same
   *        output-store critical section as job ownership.
   * @return Nothing.
   * @throws Nothing; callback failures are contained.
   * @note Invocation follows the `Cleanup` re-entry and worker-lifecycle
   *       constraints.
   */
  void reset(
      const std::optional<std::string>& delivery_id = std::nullopt) noexcept;

 private:
  /** @brief Stable output reference, empty while inactive. */
  std::string reference_;

  /** @brief Exact-once callback, empty after reset or move. */
  Cleanup cleanup_;
};

/**
 * @brief Result of post-Host image validation and output publication.
 *
 * @throws Whatever owned status or output operations throw.
 * @note A successful empty image uses canonical success and inactive output;
 *       a failure status must not publish ownership into a terminal record.
 */
struct ComputeOutputPublication {
  /** @brief Exact publication status retained by the accepted job. */
  OperationStatus status;

  /** @brief Optional stable output ownership on publication success. */
  ComputeOutputOwnership output;
};

/**
 * @brief Owned immutable view of one compute record at one lookup instant.
 *
 * @throws std::bad_alloc when copied identifiers, status, or reference cannot
 *         be allocated.
 * @note `terminal_status` is populated only for Succeeded or Failed. Polling
 *       copies this value and never refreshes terminal retention age.
 */
struct ComputeRequestSnapshot {
  /** @brief Opaque job identity. */
  ComputeRequestId compute_id;

  /** @brief Opaque session identity captured at accepted submission. */
  IpcSessionId session_id;

  /** @brief Current forward-only lifecycle state. */
  ComputeRequestState state = ComputeRequestState::Queued;

  /** @brief Version 2 jobs cannot be cancelled. */
  bool cancellable = false;

  /** @brief Exact immutable nested status after terminal publication. */
  std::optional<OperationStatus> terminal_status;

  /** @brief Optional private output reference after successful publication. */
  std::optional<std::string> output_reference;
};

/**
 * @brief Injectable global capacity and terminal-retention policy.
 *
 * @throws Nothing.
 * @note Production keeps at most 64 queued/running and 256 terminal records,
 *       with a 15-minute TTL measured from terminal publication.
 */
struct ComputeRequestRegistryLimits {
  /** @brief Maximum queued plus running records. */
  std::size_t active = 64;

  /** @brief Maximum retained terminal records. */
  std::size_t terminal = 256;

  /** @brief Monotonic age after which a terminal record expires. */
  std::chrono::steady_clock::duration terminal_ttl = std::chrono::minutes(15);
};

/**
 * @brief Private bounded compute registry with one explicitly joined worker.
 *
 * Submission first obtains a session job admission, atomically reserves all
 * record/queue storage, and only then publishes a Queued result. The worker
 * advances one record at a time, invokes exactly one matching typed executor,
 * detaches and cleans any capacity eviction before publishing the replacement
 * terminal snapshot, and releases session admission outside this registry's
 * mutex. Active work is never evicted.
 *
 * @throws std::bad_alloc when constructor storage or callbacks cannot be
 *         allocated.
 * @throws std::invalid_argument for zero limits, nonpositive TTL, or missing
 *         executor/publisher callbacks.
 * @note Executors own the existing daemon Host mutex boundary. They must not
 *       call back into this registry. The injected clock must be monotonic and
 *       non-throwing. No detached thread is created.
 */
class ComputeRequestRegistry {
 public:
  /** @brief Monotonic timestamp used for deterministic TTL tests. */
  using TimePoint = std::chrono::steady_clock::time_point;

  /** @brief Injectable non-throwing monotonic clock. */
  using Clock = std::function<TimePoint()>;

  /** @brief Injectable opaque candidate generator. */
  using IdGenerator = std::function<std::string()>;

  /** @brief Exact synchronous status-mode Host executor. */
  using StatusExecutor =
      std::function<OperationStatus(const HostComputeRequest&)>;

  /** @brief Exact synchronous image-mode Host executor. */
  using ImageExecutor =
      std::function<Result<ImageBuffer>(const HostComputeRequest&)>;

  /** @brief Post-Host image validation/materialization callback. */
  using OutputPublisher = std::function<ComputeOutputPublication(
      const ComputeRequestId&, ImageBuffer)>;

  /**
   * @brief Creates a stopped registry with injected lifecycle dependencies.
   *
   * @param sessions Session admission registry that outlives this object.
   * @param status_executor Matching status-mode Host boundary.
   * @param image_executor Matching image-mode Host boundary.
   * @param output_publisher Post-Host image publication boundary.
   * @param limits Global active/terminal limits and terminal TTL.
   * @param clock Monotonic clock; empty selects `steady_clock::now`.
   * @param id_generator Candidate source; empty selects OS entropy.
   * @throws std::bad_alloc if callback, queue, or policy storage fails.
   * @throws std::invalid_argument for invalid limits or callbacks.
   * @note Call `start()` explicitly before accepting submissions.
   */
  ComputeRequestRegistry(SessionRegistry& sessions,
                         StatusExecutor status_executor,
                         ImageExecutor image_executor,
                         OutputPublisher output_publisher,
                         ComputeRequestRegistryLimits limits = {},
                         Clock clock = {}, IdGenerator id_generator = {});

  /**
   * @brief Drains, joins, and releases all terminal output ownership.
   * @throws Nothing; executor and cleanup failures are contained.
   */
  ~ComputeRequestRegistry() noexcept;

  /**
   * @brief Prevents copying mutex, records, and worker ownership.
   * @throws Nothing because construction is unavailable.
   */
  ComputeRequestRegistry(const ComputeRequestRegistry&) = delete;

  /**
   * @brief Prevents replacing worker ownership by copy assignment.
   * @return No value because copying is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ComputeRequestRegistry& operator=(const ComputeRequestRegistry&) = delete;

  /**
   * @brief Starts one joinable worker and enables submissions.
   * @return Success, or daemon internal error if a prior worker is still being
   *         joined.
   * @throws std::system_error if the worker thread cannot be created.
   * @note Restart is valid only after the previous worker was drained/joined.
   */
  OperationStatus start();

  /**
   * @brief Atomically disables new submissions and requests queue draining.
   * @return Nothing.
   * @throws Nothing.
   * @note Already accepted queued work remains scheduled and becomes terminal.
   */
  void stop_admission() noexcept;

  /**
   * @brief Stops admission, drains every accepted job, and joins the worker.
   * @return Nothing.
   * @throws Nothing; this function never holds the registry mutex while
   *         joining.
   * @note Concurrent lifecycle callers wait for the same sole join to finish;
   *       no caller returns while that worker is still running.
   */
  void drain_and_join() noexcept;

  /**
   * @brief Submits one typed Host request after session and capacity admission.
   *
   * @param session_id Existing opaque session id.
   * @param request Host compute value whose private session is overwritten by
   *        the admitted mapping.
   * @param mode Matching status or image executor selection.
   * @return Queued noncancellable snapshot, or top-level admission failure.
   * @throws std::bad_alloc if pre-publication ownership cannot be allocated.
   * @throws std::runtime_error if the opaque source fails.
   * @note A successful return is the commit point. Every later failure becomes
   *       an immutable terminal nested status.
   */
  IpcResult<ComputeRequestSnapshot> submit(const IpcSessionId& session_id,
                                           HostComputeRequest request,
                                           ComputeResultMode mode);

  /**
   * @brief Reads one queued, running, or terminal snapshot non-destructively.
   * @param compute_id Well-formed opaque compute identity.
   * @return Snapshot or daemon `job_not_found` after absence/expiry.
   * @throws std::bad_alloc if copied snapshot storage cannot be allocated.
   */
  IpcResult<ComputeRequestSnapshot> status(const ComputeRequestId& compute_id);

  /**
   * @brief Reads one terminal snapshot non-destructively.
   * @param compute_id Well-formed opaque compute identity.
   * @return Terminal snapshot, `job_not_ready`, or `job_not_found`.
   * @throws std::bad_alloc if copied snapshot storage cannot be allocated.
   */
  IpcResult<ComputeRequestSnapshot> result(const ComputeRequestId& compute_id);

  /**
   * @brief Removes one terminal record and its output ownership.
   * @param compute_id Well-formed opaque compute identity.
   * @param delivery_id Optional stable output lease identity released with
   *        output job ownership when it matches.
   * @return Success, `job_not_ready`, or `job_not_found`.
   * @throws std::bad_alloc if failure diagnostic construction cannot allocate;
   *         output cleanup callback failures are contained.
   */
  OperationStatus release(
      const ComputeRequestId& compute_id,
      const std::optional<std::string>& delivery_id = std::nullopt);

  /**
   * @brief Removes all records whose terminal age reached the injected TTL.
   * @return Number of terminal records removed during this call.
   * @throws Nothing; clock and output cleanup failures are contained.
   * @note Active records are never considered, and lookup does not refresh age.
   */
  std::size_t cleanup_expired() noexcept;

  /**
   * @brief Releases every retained terminal record and output.
   * @return Nothing.
   * @throws Nothing; active records are preserved for worker draining.
   */
  void release_all_terminal() noexcept;

  /**
   * @brief Performs complete registry shutdown in lifecycle order.
   * @return Nothing.
   * @throws Nothing.
   * @note Stops admission, drains/joins the worker, then releases terminal
   *       output ownership. The borrowed session registry remains intact.
   */
  void shutdown() noexcept;

 private:
  /**
   * @brief Complete mutable state for one accepted compute record defined in
   *        the source file.
   *
   * @throws Nothing for this incomplete declaration.
   * @note `records_` owns every complete instance. Detachment transfers sole
   *       ownership outside `mutex_` so admission and output cleanup destruct
   *       only after the protected record mutation has linearized.
   */
  struct Record;

  /**
   * @brief Runs the sole FIFO executor until stop is requested and drained.
   *
   * Each accepted record advances to Running under the registry mutex, invokes
   * its typed Host/output boundary without that mutex, and prepares an exact
   * outcome. Before the terminal commit, the worker detaches any required
   * capacity eviction, completes its output cleanup without the mutex, and
   * cleans abandoned local output after failures. It then captures terminal
   * time, atomically publishes the complete replacement snapshot, and releases
   * session admission after unlocking.
   *
   * @return Nothing.
   * @throws Nothing; every accepted execution failure is contained as terminal.
   * @note An eviction cleanup callback observes the evicted identity as absent
   *       and its replacement as Running. Cleanup callbacks must obey the
   *       nonblocking re-entry constraints documented by `Cleanup`.
   */
  void worker_loop() noexcept;

  /**
   * @brief Copies a complete record while the registry mutex is held.
   * @param record Stable record to snapshot.
   * @return Owned immutable snapshot.
   * @throws std::bad_alloc if copied storage cannot be allocated.
   */
  ComputeRequestSnapshot snapshot_locked(const Record& record) const;

  /**
   * @brief Detaches one terminal record while the registry mutex is held.
   * @param iterator Existing record-map iterator.
   * @return Exclusive record ownership for destruction after unlocking.
   * @throws Nothing.
   */
  std::unique_ptr<Record> detach_terminal_locked(
      std::map<std::string, std::unique_ptr<Record>>::iterator
          iterator) noexcept;

  /** @brief Session registry borrowed through complete worker destruction. */
  SessionRegistry& sessions_;

  /** @brief Matching status-mode executor, normally locking daemon Host. */
  StatusExecutor status_executor_;

  /** @brief Matching image-mode executor, normally locking daemon Host. */
  ImageExecutor image_executor_;

  /** @brief Output validator/publisher invoked after Host mutex release. */
  OutputPublisher output_publisher_;

  /** @brief Immutable capacity and terminal-retention policy. */
  ComputeRequestRegistryLimits limits_;

  /** @brief Injectable monotonic clock. */
  Clock clock_;

  /** @brief Injectable opaque candidate source. */
  IdGenerator id_generator_;

  /** @brief Serializes record, queue, and worker lifecycle state. */
  mutable std::mutex mutex_;

  /** @brief Wakes the worker for accepted work or drain shutdown. */
  std::condition_variable work_cv_;

  /** @brief Wakes lifecycle callers after the sole worker join completes. */
  std::condition_variable join_cv_;

  /** @brief All active and retained terminal records by opaque identity. */
  std::map<std::string, std::unique_ptr<Record>> records_;

  /** @brief Allocation-reserved terminal nodes for active jobs. */
  std::list<std::string> reserved_terminal_order_;

  /** @brief Terminal identities ordered by publication, oldest first. */
  std::list<std::string> terminal_order_;

  /** @brief FIFO active pointers with constructor-reserved capacity. */
  std::vector<Record*> queue_;

  /** @brief Current queued plus running record count. */
  std::size_t active_count_ = 0;

  /** @brief Whether new submissions may publish queue entries. */
  bool accepting_ = false;

  /** @brief Whether the worker exits after the accepted queue drains. */
  bool stop_requested_ = false;

  /** @brief Whether a lifecycle caller currently joins a moved-out worker. */
  bool joining_ = false;

  /** @brief Sole joinable FIFO execution thread. */
  std::thread worker_;
};

}  // namespace ps::ipc::internal
