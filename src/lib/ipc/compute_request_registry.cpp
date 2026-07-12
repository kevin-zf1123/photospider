#include "ipc/compute_request_registry.hpp"

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "ipc/codec.hpp"

namespace ps::ipc::internal {
namespace {

/**
 * @brief Builds the stable absent-job lookup failure.
 * @return Daemon-domain `job_not_found` status.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 */
OperationStatus job_not_found_status() {
  return failure_status(OperationErrorDomain::Daemon, kJobNotFoundCode,
                        "job_not_found", "compute job was not found");
}

/**
 * @brief Builds the stable premature-result/release failure.
 * @return Daemon-domain `job_not_ready` status.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 */
OperationStatus job_not_ready_status() {
  return failure_status(OperationErrorDomain::Daemon, kJobNotReadyCode,
                        "job_not_ready", "compute job is not terminal");
}

/**
 * @brief Builds the stable active-registry capacity failure.
 * @return Daemon-domain `capacity_exceeded` status.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 */
OperationStatus active_capacity_status() {
  return failure_status(OperationErrorDomain::Daemon, kCapacityExceededCode,
                        "capacity_exceeded",
                        "compute active-job capacity is exhausted");
}

/**
 * @brief Builds a preallocated fallback for any accepted worker exception.
 * @return Daemon-domain `internal_error` nested terminal status.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 * @note Every record owns this value before its queue commit point, allowing
 *       `std::bad_alloc` itself to become terminal without new allocation.
 */
OperationStatus worker_fallback_status() {
  return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                        "internal_error",
                        "unexpected compute worker or publication failure");
}

/**
 * @brief Reports whether a state is immutable terminal.
 * @param state Current record state.
 * @return True for Succeeded or Failed.
 * @throws Nothing.
 */
bool terminal_state(ComputeRequestState state) noexcept {
  return state == ComputeRequestState::Succeeded ||
         state == ComputeRequestState::Failed;
}

}  // namespace

/**
 * @brief Complete storage for one active or retained compute request.
 *
 * @throws std::bad_alloc when owned request/status storage cannot be allocated.
 * @note `terminal_slot` starts in `reserved_terminal_order_` and is spliced
 *       without allocation into publication order at the terminal commit.
 */
struct ComputeRequestRegistry::Record {
  /** @brief Opaque compute identity. */
  ComputeRequestId compute_id;

  /** @brief Opaque session identity retained independently of session close. */
  IpcSessionId session_id;

  /** @brief Exact Host request with its admitted private session id. */
  HostComputeRequest request;

  /** @brief Matching typed executor selection. */
  ComputeResultMode mode = ComputeResultMode::Status;

  /** @brief Forward-only lifecycle state. */
  ComputeRequestState state = ComputeRequestState::Queued;

  /** @brief Preallocated fallback, replaced by a normal exact terminal status.
   */
  std::optional<OperationStatus> terminal_status;

  /** @brief Optional stable output cleanup ownership. */
  ComputeOutputOwnership output;

  /** @brief Injected monotonic time of complete terminal publication. */
  TimePoint terminal_since{};

  /** @brief Allocation-reserved ordering node for terminal publication. */
  std::list<std::string>::iterator terminal_slot;

  /** @brief Session job count retained through terminal publication. */
  SessionRegistry::JobAdmission admission;
};

/** @copydoc ComputeOutputOwnership::ComputeOutputOwnership */
ComputeOutputOwnership::ComputeOutputOwnership(std::string reference,
                                               Cleanup cleanup)
    : reference_(std::move(reference)), cleanup_(std::move(cleanup)) {
  if (reference_.empty() || !cleanup_) {
    throw std::invalid_argument(
        "active compute output requires reference and cleanup");
  }
}

/** @copydoc ComputeOutputOwnership::~ComputeOutputOwnership */
ComputeOutputOwnership::~ComputeOutputOwnership() noexcept {
  reset();
}

/** @copydoc ComputeOutputOwnership::ComputeOutputOwnership */
ComputeOutputOwnership::ComputeOutputOwnership(
    ComputeOutputOwnership&& other) noexcept
    : reference_(std::move(other.reference_)),
      cleanup_(std::move(other.cleanup_)) {
  other.reference_.clear();
  other.cleanup_ = nullptr;
}

/** @copydoc ComputeOutputOwnership::operator= */
ComputeOutputOwnership& ComputeOutputOwnership::operator=(
    ComputeOutputOwnership&& other) noexcept {
  if (this != &other) {
    reset();
    reference_ = std::move(other.reference_);
    cleanup_ = std::move(other.cleanup_);
    other.reference_.clear();
    other.cleanup_ = nullptr;
  }
  return *this;
}

/** @copydoc ComputeOutputOwnership::active */
bool ComputeOutputOwnership::active() const noexcept {
  return !reference_.empty() && static_cast<bool>(cleanup_);
}

/** @copydoc ComputeOutputOwnership::reference */
const std::string& ComputeOutputOwnership::reference() const noexcept {
  return reference_;
}

/** @copydoc ComputeOutputOwnership::reset */
void ComputeOutputOwnership::reset(
    const std::optional<std::string>& delivery_id) noexcept {
  Cleanup cleanup = std::move(cleanup_);
  cleanup_ = nullptr;
  reference_.clear();
  if (cleanup) {
    try {
      cleanup(delivery_id);
    } catch (...) {
    }
  }
}

/** @copydoc ComputeRequestRegistry::ComputeRequestRegistry */
ComputeRequestRegistry::ComputeRequestRegistry(
    SessionRegistry& sessions, StatusExecutor status_executor,
    ImageExecutor image_executor, OutputPublisher output_publisher,
    ComputeRequestRegistryLimits limits, Clock clock, IdGenerator id_generator)
    : sessions_(sessions),
      status_executor_(std::move(status_executor)),
      image_executor_(std::move(image_executor)),
      output_publisher_(std::move(output_publisher)),
      limits_(limits),
      clock_(std::move(clock)),
      id_generator_(std::move(id_generator)) {
  if (limits_.active == 0 || limits_.terminal == 0 ||
      limits_.terminal_ttl <= std::chrono::steady_clock::duration::zero()) {
    throw std::invalid_argument(
        "compute registry limits and terminal TTL must be positive");
  }
  if (!status_executor_ || !image_executor_ || !output_publisher_) {
    throw std::invalid_argument(
        "compute registry requires status, image, and output callbacks");
  }
  if (!clock_) {
    clock_ = [] { return std::chrono::steady_clock::now(); };
  }
  if (!id_generator_) {
    id_generator_ = generate_opaque_id;
  }
  queue_.reserve(limits_.active);
}

/** @copydoc ComputeRequestRegistry::~ComputeRequestRegistry */
ComputeRequestRegistry::~ComputeRequestRegistry() noexcept {
  shutdown();
}

/** @copydoc ComputeRequestRegistry::start */
OperationStatus ComputeRequestRegistry::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (joining_) {
    return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                          "internal_error",
                          "compute worker is still being joined");
  }
  if (worker_.joinable()) {
    if (accepting_ && !stop_requested_) {
      return ok_status();
    }
    return failure_status(
        OperationErrorDomain::Daemon, kInternalErrorCode, "internal_error",
        "stopped compute worker must be joined before restart");
  }
  if (active_count_ != 0 || !queue_.empty()) {
    return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                          "internal_error",
                          "compute registry restart found active work");
  }
  stop_requested_ = false;
  std::thread candidate(&ComputeRequestRegistry::worker_loop, this);
  worker_ = std::move(candidate);
  accepting_ = true;
  return ok_status();
}

/** @copydoc ComputeRequestRegistry::stop_admission */
void ComputeRequestRegistry::stop_admission() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    stop_requested_ = true;
  }
  work_cv_.notify_all();
}

/** @copydoc ComputeRequestRegistry::drain_and_join */
void ComputeRequestRegistry::drain_and_join() noexcept {
  stop_admission();
  std::thread joining_worker;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (joining_) {
      join_cv_.wait(lock, [this] { return !joining_; });
      return;
    }
    if (!worker_.joinable()) {
      return;
    }
    joining_ = true;
    joining_worker = std::move(worker_);
  }
  joining_worker.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    joining_ = false;
  }
  join_cv_.notify_all();
}

/** @copydoc ComputeRequestRegistry::submit */
IpcResult<ComputeRequestSnapshot> ComputeRequestRegistry::submit(
    const IpcSessionId& session_id, HostComputeRequest request,
    ComputeResultMode mode) {
  (void)cleanup_expired();
  IpcResult<SessionRegistry::JobAdmission> admitted =
      sessions_.admit_job(session_id);
  if (!admitted.status.ok) {
    return {std::move(admitted.status), {}};
  }
  request.session = admitted.value.host_session();
  OperationStatus fallback = worker_fallback_status();

  ComputeRequestSnapshot submitted;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || stop_requested_ || !worker_.joinable()) {
      return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                             "internal_error",
                             "compute registry is not accepting submissions"),
              {}};
    }
    if (active_count_ >= limits_.active) {
      return {active_capacity_status(), {}};
    }

    std::string candidate;
    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
      candidate = id_generator_();
      if (!valid_opaque_id(candidate)) {
        return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                               "internal_error",
                               "compute id generator returned invalid data"),
                {}};
      }
      if (records_.count(candidate) == 0) {
        break;
      }
      candidate.clear();
    }
    if (candidate.empty()) {
      return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                             "internal_error",
                             "opaque compute id collision limit reached"),
              {}};
    }

    submitted.compute_id.value = candidate;
    submitted.session_id = session_id;
    submitted.state = ComputeRequestState::Queued;
    submitted.cancellable = false;

    reserved_terminal_order_.push_back(candidate);
    const auto terminal_slot = std::prev(reserved_terminal_order_.end());
    std::unique_ptr<Record> record;
    try {
      record = std::make_unique<Record>(
          Record{{candidate},
                 session_id,
                 std::move(request),
                 mode,
                 ComputeRequestState::Queued,
                 std::optional<OperationStatus>(std::move(fallback)),
                 {},
                 {},
                 terminal_slot,
                 {}});
    } catch (...) {
      reserved_terminal_order_.erase(terminal_slot);
      throw;
    }
    Record* record_pointer = record.get();
    queue_.push_back(record_pointer);
    try {
      const auto inserted = records_.emplace(candidate, std::move(record));
      if (!inserted.second) {
        queue_.pop_back();
        reserved_terminal_order_.erase(terminal_slot);
        return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                               "internal_error",
                               "compute id reservation invariant failed"),
                {}};
      }
      inserted.first->second->admission = std::move(admitted.value);
    } catch (...) {
      queue_.pop_back();
      reserved_terminal_order_.erase(terminal_slot);
      throw;
    }
    ++active_count_;
  }
  work_cv_.notify_one();
  return {ok_status(), std::move(submitted)};
}

/** @copydoc ComputeRequestRegistry::status */
IpcResult<ComputeRequestSnapshot> ComputeRequestRegistry::status(
    const ComputeRequestId& compute_id) {
  (void)cleanup_expired();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(compute_id.value);
  if (found == records_.end()) {
    return {job_not_found_status(), {}};
  }
  return {ok_status(), snapshot_locked(*found->second)};
}

/** @copydoc ComputeRequestRegistry::result */
IpcResult<ComputeRequestSnapshot> ComputeRequestRegistry::result(
    const ComputeRequestId& compute_id) {
  (void)cleanup_expired();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(compute_id.value);
  if (found == records_.end()) {
    return {job_not_found_status(), {}};
  }
  if (!terminal_state(found->second->state)) {
    return {job_not_ready_status(), {}};
  }
  return {ok_status(), snapshot_locked(*found->second)};
}

/** @copydoc ComputeRequestRegistry::release */
OperationStatus ComputeRequestRegistry::release(
    const ComputeRequestId& compute_id,
    const std::optional<std::string>& delivery_id) {
  (void)cleanup_expired();
  std::unique_ptr<Record> removed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(compute_id.value);
    if (found == records_.end()) {
      return job_not_found_status();
    }
    if (!terminal_state(found->second->state)) {
      return job_not_ready_status();
    }
    removed = detach_terminal_locked(found);
  }
  removed->output.reset(delivery_id);
  removed.reset();
  return ok_status();
}

/** @copydoc ComputeRequestRegistry::cleanup_expired */
std::size_t ComputeRequestRegistry::cleanup_expired() noexcept {
  TimePoint now;
  try {
    now = clock_();
  } catch (...) {
    return 0;
  }
  std::size_t removed_count = 0;
  while (true) {
    std::unique_ptr<Record> removed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (terminal_order_.empty()) {
        break;
      }
      const auto found = records_.find(terminal_order_.front());
      if (found == records_.end()) {
        terminal_order_.pop_front();
        continue;
      }
      const Record& record = *found->second;
      if (now < record.terminal_since ||
          now - record.terminal_since < limits_.terminal_ttl) {
        break;
      }
      removed = detach_terminal_locked(found);
    }
    removed.reset();
    ++removed_count;
  }
  return removed_count;
}

/** @copydoc ComputeRequestRegistry::release_all_terminal */
void ComputeRequestRegistry::release_all_terminal() noexcept {
  while (true) {
    std::unique_ptr<Record> removed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (terminal_order_.empty()) {
        return;
      }
      const auto found = records_.find(terminal_order_.front());
      if (found == records_.end()) {
        terminal_order_.pop_front();
        continue;
      }
      removed = detach_terminal_locked(found);
    }
    removed.reset();
  }
}

/** @copydoc ComputeRequestRegistry::shutdown */
void ComputeRequestRegistry::shutdown() noexcept {
  drain_and_join();
  release_all_terminal();
}

/** @copydoc ComputeRequestRegistry::worker_loop */
void ComputeRequestRegistry::worker_loop() noexcept {
  while (true) {
    Record* record = nullptr;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_cv_.wait(lock,
                    [this] { return stop_requested_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stop_requested_) {
          return;
        }
        continue;
      }
      record = queue_.front();
      queue_.erase(queue_.begin());
      record->state = ComputeRequestState::Running;
    }

    bool normal_outcome = false;
    OperationStatus outcome;
    ComputeOutputOwnership output;
    try {
      if (record->mode == ComputeResultMode::Status) {
        outcome = status_executor_(record->request);
      } else {
        Result<ImageBuffer> image = image_executor_(record->request);
        if (!image.status.ok) {
          outcome = std::move(image.status);
        } else {
          ComputeOutputPublication publication =
              output_publisher_(record->compute_id, std::move(image.value));
          outcome = std::move(publication.status);
          if (outcome.ok) {
            output = std::move(publication.output);
          }
        }
      }
      normal_outcome = true;
    } catch (...) {
    }

    TimePoint terminal_time{};
    try {
      terminal_time = clock_();
    } catch (...) {
      normal_outcome = false;
    }

    std::unique_ptr<Record> evicted;
    SessionRegistry::JobAdmission completed_admission;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (normal_outcome) {
        *record->terminal_status = std::move(outcome);
        if (record->terminal_status->ok && output.active()) {
          record->output = std::move(output);
        }
      }
      record->state = record->terminal_status->ok
                          ? ComputeRequestState::Succeeded
                          : ComputeRequestState::Failed;
      record->terminal_since = terminal_time;
      if (terminal_order_.size() >= limits_.terminal) {
        const auto oldest = records_.find(terminal_order_.front());
        if (oldest != records_.end()) {
          evicted = detach_terminal_locked(oldest);
        } else {
          terminal_order_.pop_front();
        }
      }
      terminal_order_.splice(terminal_order_.end(), reserved_terminal_order_,
                             record->terminal_slot);
      --active_count_;
      completed_admission = std::move(record->admission);
    }
    completed_admission = {};
    evicted.reset();
  }
}

/** @copydoc ComputeRequestRegistry::snapshot_locked */
ComputeRequestSnapshot ComputeRequestRegistry::snapshot_locked(
    const Record& record) const {
  ComputeRequestSnapshot snapshot;
  snapshot.compute_id = record.compute_id;
  snapshot.session_id = record.session_id;
  snapshot.state = record.state;
  snapshot.cancellable = false;
  if (terminal_state(record.state)) {
    snapshot.terminal_status = record.terminal_status;
    if (record.output.active()) {
      snapshot.output_reference = record.output.reference();
    }
  }
  return snapshot;
}

/** @copydoc ComputeRequestRegistry::detach_terminal_locked */
std::unique_ptr<ComputeRequestRegistry::Record>
ComputeRequestRegistry::detach_terminal_locked(
    std::map<std::string, std::unique_ptr<Record>>::iterator
        iterator) noexcept {
  std::unique_ptr<Record> removed = std::move(iterator->second);
  terminal_order_.erase(removed->terminal_slot);
  records_.erase(iterator);
  return removed;
}

}  // namespace ps::ipc::internal
