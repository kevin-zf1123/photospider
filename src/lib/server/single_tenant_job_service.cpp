/**
 * @file single_tenant_job_service.cpp
 * @brief Implements Issue #98 Job truth and artifact commit authority.
 */
#include "server/single_tenant_job_service.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "server/single_tenant_job_service_test_access.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/** @brief Process-wide non-reused Job sequence. */
std::atomic<std::uint64_t> g_job_sequence{0U};
/** @brief Process-wide non-reused attempt sequence. */
std::atomic<std::uint64_t> g_attempt_sequence{0U};
/** @brief Process-wide non-reused worker sequence. */
std::atomic<std::uint64_t> g_worker_sequence{0U};
/** @brief Process-wide non-reused artifact sequence. */
std::atomic<std::uint64_t> g_artifact_sequence{0U};
/** @brief Process-wide non-reused output commit sequence. */
std::atomic<std::uint64_t> g_commit_sequence{0U};

/**
 * @brief Current-thread state for one source-private worker-start injection.
 * @throws Nothing for value operations.
 * @note The capture pointer is non-null only while its non-movable guard is
 * alive on this thread. Normal production threads leave this state disarmed.
 */
struct AssignmentThreadStartFailureInjectionState final {
  /** @brief Guard-owned destination for the rolled-back JobId, or null. */
  std::optional<JobId>* attempted_job_id = nullptr;
  /** @brief Whether the next assignment-thread start must raise. */
  bool armed = false;
};

/** @brief Guarantees failure capture cannot replace the injected exception. */
static_assert(std::is_nothrow_move_constructible_v<JobId>);

/**
 * @brief Per-calling-thread deterministic assignment-thread start injection.
 * @note Thread-local storage prevents concurrent tests or submitters on other
 * threads from consuming this source-private test arm.
 */
thread_local AssignmentThreadStartFailureInjectionState g_worker_start_failure;

/**
 * @brief Arms one current-thread assignment-thread start failure.
 * @param attempted_job_id Non-null guard-owned capture destination that stays
 * alive until matching disarm.
 * @return Nothing.
 * @throws std::invalid_argument when the capture destination is null.
 * @throws std::logic_error when another guard already owns this thread's seam.
 */
void arm_assignment_thread_start_failure(
    std::optional<JobId>* attempted_job_id) {
  if (attempted_job_id == nullptr) {
    throw std::invalid_argument("worker-start failure capture is null");
  }
  if (g_worker_start_failure.attempted_job_id != nullptr) {
    throw std::logic_error("worker-start failure injection is already armed");
  }
  attempted_job_id->reset();
  g_worker_start_failure.attempted_job_id = attempted_job_id;
  g_worker_start_failure.armed = true;
}

/**
 * @brief Disarms one matching current-thread worker-start injection guard.
 * @param attempted_job_id Exact guard-owned capture destination supplied when
 * arming.
 * @return Nothing.
 * @throws Nothing.
 * @note A mismatched pointer is ignored so one guard cannot disarm another.
 */
void disarm_assignment_thread_start_failure(
    const std::optional<JobId>* attempted_job_id) noexcept {
  if (g_worker_start_failure.attempted_job_id != attempted_job_id) {
    return;
  }
  g_worker_start_failure.armed = false;
  g_worker_start_failure.attempted_job_id = nullptr;
}

/**
 * @brief Raises the armed current-thread assignment-start failure, if any.
 * @param attempted_job_id Exact pending JobId, moved into the guard capture
 * only when the injection is consumed.
 * @return Nothing when the current thread has no armed injection.
 * @throws std::system_error with resource-unavailable status when armed.
 * @throws std::bad_alloc only if constructing the injected diagnostic fails.
 * @note Consumption occurs before native thread construction. The guard stays
 * alive but cannot inject a second failure, and other threads are unaffected.
 */
void throw_assignment_thread_start_failure_if_armed(JobId& attempted_job_id) {
  if (!g_worker_start_failure.armed) {
    return;
  }
  g_worker_start_failure.armed = false;
  g_worker_start_failure.attempted_job_id->emplace(std::move(attempted_job_id));
  throw std::system_error(
      std::make_error_code(std::errc::resource_unavailable_try_again),
      "injected assignment-thread start failure");
}

/**
 * @brief Mints one process-lifetime identity in an exact strong domain.
 * @tparam Domain Opaque identity domain tag.
 * @param prefix Stable domain-specific textual prefix.
 * @param sequence Non-null process-wide monotonically increasing counter.
 * @return Fresh strong opaque identity.
 * @throws std::overflow_error if the process exhausts the uint64 sequence.
 * @throws std::bad_alloc when constructing identity text exhausts memory.
 * @note Counters intentionally do not persist across restart; Issue #99 owns
 * durable/global allocation.
 */
template <typename Domain>
OpaqueTextId<Domain> mint_identity(std::string_view prefix,
                                   std::atomic<std::uint64_t>* sequence) {
  if (sequence == nullptr) {
    throw std::invalid_argument("identity sequence is null");
  }
  const std::uint64_t previous = sequence->fetch_add(1U);
  if (previous == std::numeric_limits<std::uint64_t>::max()) {
    sequence->fetch_sub(1U);
    throw std::overflow_error("process identity sequence exhausted");
  }
  return OpaqueTextId<Domain>(std::string(prefix) +
                              std::to_string(previous + 1U));
}

/**
 * @brief Mints one fresh accepted Job identity.
 * @return Process-lifetime unique JobId.
 * @throws As `mint_identity`.
 */
JobId mint_job_id() {
  return mint_identity<JobIdDomain>("job-v1-", &g_job_sequence);
}

/**
 * @brief Mints one fresh Job attempt identity.
 * @return Process-lifetime unique JobAttemptId.
 * @throws As `mint_identity`.
 */
JobAttemptId mint_attempt_id() {
  return mint_identity<JobAttemptIdDomain>("attempt-v1-", &g_attempt_sequence);
}

/**
 * @brief Mints one fresh worker instance identity.
 * @return Process-lifetime unique WorkerInstanceId.
 * @throws As `mint_identity`.
 */
WorkerInstanceId mint_worker_id() {
  return mint_identity<WorkerInstanceIdDomain>("worker-v1-",
                                               &g_worker_sequence);
}

/**
 * @brief Mints one fresh immutable artifact identity.
 * @return Process-lifetime unique ArtifactId.
 * @throws As `mint_identity`.
 */
ArtifactId mint_artifact_id() {
  return mint_identity<ArtifactIdDomain>("artifact-v1-", &g_artifact_sequence);
}

/**
 * @brief Mints one fresh output commit identity.
 * @return Process-lifetime unique OutputCommitId.
 * @throws As `mint_identity`.
 */
OutputCommitId mint_commit_id() {
  return mint_identity<OutputCommitIdDomain>("commit-v1-", &g_commit_sequence);
}

/**
 * @brief Returns a bounded diagnostic copied from one exception.
 * @param error Caught standard exception.
 * @return Human-readable message with a stable prefix.
 * @throws std::bad_alloc when message construction exhausts memory.
 */
std::string unexpected_message(const std::exception& error) {
  return std::string("worker execution raised: ") + error.what();
}

/**
 * @brief Reports whether a failure category may originate from a failed
 * worker report.
 * @param failure Candidate failure enum, including potentially invalid
 * underlying representations.
 * @return True only for a worker-owned failure accepted with `Failed`.
 * @throws Nothing.
 * @note `None`, `CancellationObserved`, `ReportRejected`, and
 * `ArtifactCommit` belong to other outcome or control-plane domains.
 */
bool is_worker_owned_failure(JobAttemptFailure failure) noexcept {
  switch (failure) {
    case JobAttemptFailure::InvalidAssignment:
    case JobAttemptFailure::GraphResolution:
    case JobAttemptFailure::HostSetup:
    case JobAttemptFailure::GraphLoad:
    case JobAttemptFailure::Compute:
    case JobAttemptFailure::Settlement:
    case JobAttemptFailure::Unexpected:
      return true;
    case JobAttemptFailure::None:
    case JobAttemptFailure::CancellationObserved:
    case JobAttemptFailure::ReportRejected:
    case JobAttemptFailure::ArtifactCommit:
      return false;
  }
  return false;
}

/**
 * @brief Validates the closed outcome/settlement/failure/image report shape.
 * @param report Identity-fenced candidate report.
 * @return True only for one complete supported worker report shape.
 * @throws Nothing.
 * @note Diagnostics are intentionally unconstrained. Failed reports preserve
 * either settlement value because it records the worker's actual cleanup.
 */
bool has_valid_worker_report_shape(const JobAttemptReport& report) noexcept {
  switch (report.outcome) {
    case JobAttemptOutcome::Succeeded:
      return report.settled && report.failure == JobAttemptFailure::None &&
             report.image.has_value();
    case JobAttemptOutcome::Failed:
      return is_worker_owned_failure(report.failure) &&
             !report.image.has_value();
    case JobAttemptOutcome::Cancelled:
      return report.settled &&
             report.failure == JobAttemptFailure::CancellationObserved &&
             !report.image.has_value();
  }
  return false;
}

/**
 * @brief Publishes one fail-closed report rejection without trusting facts.
 * @param job Mutable current Job record held under the service mutex.
 * @param message Stable control-plane diagnostic.
 * @return Nothing.
 * @throws std::bad_alloc when storing the diagnostic exhausts memory.
 * @note `Failed` is a control-plane state, not a worker outcome. The rejected
 * report leaves no retained outcome, settlement, or receipt fact.
 */
void reject_report(JobSnapshot& job, std::string_view message) {
  job.state = JobState::Failed;
  job.attempt_outcome.reset();
  job.attempt_settled = false;
  job.failure = JobAttemptFailure::ReportRejected;
  job.message.assign(message);
  job.output_receipt.reset();
}

}  // namespace

/** @copydoc ps::server::ScopedWorkerThreadStartFailure */
ScopedWorkerThreadStartFailure::ScopedWorkerThreadStartFailure() {
  arm_assignment_thread_start_failure(&attempted_job_id_);
}

/**
 * @copydoc
 * ps::server::ScopedWorkerThreadStartFailure::~ScopedWorkerThreadStartFailure
 */
ScopedWorkerThreadStartFailure::~ScopedWorkerThreadStartFailure() noexcept {
  disarm_assignment_thread_start_failure(&attempted_job_id_);
}

/** @copydoc ps::server::is_terminal_job_state */
bool is_terminal_job_state(JobState state) {
  switch (state) {
    case JobState::Queued:
    case JobState::Running:
    case JobState::Cancelling:
      return false;
    case JobState::Succeeded:
    case JobState::Failed:
    case JobState::Cancelled:
      return true;
  }
  throw std::invalid_argument("Job state is invalid");
}

/** @copydoc ps::server::ProcessLifetimeArtifactStore::commit */
OutputCommitReceipt ProcessLifetimeArtifactStore::commit(
    const ArtifactCommitRequest& request) {
  validate_attempt_identity(request.attempt);
  if (!request.output_slot_id.valid()) {
    throw std::invalid_argument("artifact output slot is invalid");
  }
  validate_image_buffer(request.image);
  if (request.image.width <= 0 || request.image.height <= 0 ||
      request.image.channels <= 0 || request.image.data == nullptr) {
    throw std::invalid_argument("artifact image is empty");
  }
  if (request.image.device != Device::CPU) {
    throw std::invalid_argument("artifact image is not CPU-resident");
  }

  const std::size_t row_bytes = image_buffer_row_bytes(request.image);
  const std::size_t height = static_cast<std::size_t>(request.image.height);
  if (row_bytes > std::numeric_limits<std::size_t>::max() / height) {
    throw std::overflow_error("artifact payload size overflowed");
  }
  const std::size_t payload_bytes = row_bytes * height;
  std::vector<std::byte> payload(payload_bytes);
  for (int row = 0; row < request.image.height; ++row) {
    const std::byte* source = image_buffer_row_data(request.image, row);
    std::memcpy(payload.data() + static_cast<std::size_t>(row) * row_bytes,
                source, row_bytes);
  }

  OutputCommitReceipt receipt;
  receipt.attempt = request.attempt;
  receipt.output_slot_id = request.output_slot_id;
  receipt.artifact_id = mint_artifact_id();
  receipt.output_commit_id = mint_commit_id();
  receipt.descriptor = ArtifactImageDescriptor{request.image.width,
                                               request.image.height,
                                               request.image.channels,
                                               request.image.type,
                                               row_bytes,
                                               payload_bytes};
  receipt.content_digest =
      hash_artifact_content(payload.data(), payload.size());
  receipt.achieved_durability = ArtifactDurability::ProcessLifetime;

  auto record = std::make_shared<const ArtifactRecord>(
      ArtifactRecord{receipt, std::move(payload)});
  std::lock_guard<std::mutex> lock(mutex_);
  const bool inserted =
      records_.emplace(receipt.artifact_id.value(), std::move(record)).second;
  if (!inserted) {
    throw std::logic_error("fresh ArtifactId collided in process store");
  }
  return receipt;
}

/** @copydoc ps::server::ProcessLifetimeArtifactStore::find */
std::shared_ptr<const ArtifactRecord> ProcessLifetimeArtifactStore::find(
    const ArtifactId& artifact_id) const {
  if (!artifact_id.valid()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(artifact_id.value());
  return found == records_.end() ? nullptr : found->second;
}

/** @copydoc ps::server::ProcessLifetimeArtifactStore::size */
std::size_t ProcessLifetimeArtifactStore::size() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_.size();
}

/** @copydoc ps::server::SingleTenantJobService::SingleTenantJobService */
SingleTenantJobService::SingleTenantJobService(
    TenantId tenant_id, std::shared_ptr<JobAttemptWorkerFactory> worker_factory)
    : tenant_id_(std::move(tenant_id)),
      worker_factory_(std::move(worker_factory)) {
  if (!tenant_id_.valid()) {
    throw std::invalid_argument("single-tenant service tenant is invalid");
  }
  if (worker_factory_ == nullptr) {
    throw std::invalid_argument("single-tenant worker factory is null");
  }
  reaper_ = std::thread(&SingleTenantJobService::reap_workers, this);
}

/**
 * @copydoc
 * ps::server::SingleTenantJobService::WorkerThreadRecord::WorkerThreadRecord
 */
SingleTenantJobService::WorkerThreadRecord::WorkerThreadRecord(
    SingleTenantJobService* service, JobAssignment assignment)
    : thread(start_assignment_thread(service, std::move(assignment))) {}

/**
 * @copydoc
 * ps::server::SingleTenantJobService::WorkerThreadRecord::start_assignment_thread
 */
std::thread SingleTenantJobService::WorkerThreadRecord::start_assignment_thread(
    SingleTenantJobService* service, JobAssignment assignment) {
  throw_assignment_thread_start_failure_if_armed(assignment.identity.job_id);
  return std::thread(&SingleTenantJobService::run_assignment, service,
                     std::move(assignment));
}

/** @copydoc ps::server::SingleTenantJobService::~SingleTenantJobService */
SingleTenantJobService::~SingleTenantJobService() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    for (auto& entry : jobs_) {
      JobSnapshot& job = entry.second;
      if (!is_terminal_job_state(job.state)) {
        job.cancellation_requested = true;
        job.state = JobState::Cancelling;
      }
    }
  }
  condition_.notify_all();
  if (reaper_.joinable()) {
    reaper_.join();
  }
}

/** @copydoc ps::server::SingleTenantJobService::submit */
JobSubmission SingleTenantJobService::submit(JobSpec spec) {
  validate_job_spec(spec);
  auto immutable_spec = std::make_shared<const JobSpec>(std::move(spec));

  JobAssignment assignment;
  assignment.identity.tenant_id = tenant_id_;
  assignment.identity.job_id = mint_job_id();
  assignment.identity.job_spec_digest = immutable_spec->digest();
  assignment.identity.attempt_id = mint_attempt_id();
  assignment.identity.worker_instance_id = mint_worker_id();
  assignment.identity.worker_lease_generation = WorkerLeaseGeneration{1U};
  assignment.spec = immutable_spec;
  validate_attempt_identity(assignment.identity);

  JobSubmission submission{assignment.identity.job_id,
                           assignment.identity.job_spec_digest,
                           assignment.identity};

  JobSnapshot snapshot;
  snapshot.tenant_id = tenant_id_;
  snapshot.job_id = assignment.identity.job_id;
  snapshot.spec = immutable_spec;
  snapshot.assignment = assignment.identity;
  snapshot.state = JobState::Queued;

  std::lock_guard<std::mutex> lock(mutex_);
  if (shutting_down_) {
    throw std::invalid_argument("single-tenant service is shutting down");
  }
  const std::string job_key = assignment.identity.job_id.value();
  const bool inserted = jobs_.emplace(job_key, std::move(snapshot)).second;
  if (!inserted) {
    throw std::logic_error("fresh JobId collided in service state");
  }
  try {
    const bool worker_inserted =
        workers_.try_emplace(job_key, this, assignment).second;
    if (!worker_inserted) {
      throw std::logic_error("fresh JobId collided in worker ownership");
    }
  } catch (...) {
    jobs_.erase(job_key);
    condition_.notify_all();
    throw;
  }

  return submission;
}

/** @copydoc ps::server::SingleTenantJobService::query */
std::optional<JobSnapshot> SingleTenantJobService::query(
    const JobId& job_id) const {
  if (!job_id.valid()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = jobs_.find(job_id.value());
  if (found == jobs_.end()) {
    return std::nullopt;
  }
  return found->second;
}

/** @copydoc ps::server::SingleTenantJobService::wait_for */
std::optional<JobSnapshot> SingleTenantJobService::wait_for(
    const JobId& job_id, std::chrono::milliseconds timeout) const {
  if (timeout.count() < 0) {
    throw std::invalid_argument("Job wait timeout is negative");
  }
  if (!job_id.valid()) {
    return std::nullopt;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (jobs_.find(job_id.value()) == jobs_.end()) {
    return std::nullopt;
  }
  const bool completed = condition_.wait_for(lock, timeout, [&] {
    const auto found = jobs_.find(job_id.value());
    return found == jobs_.end() || is_terminal_job_state(found->second.state);
  });
  if (!completed) {
    return std::nullopt;
  }
  const auto found = jobs_.find(job_id.value());
  return found == jobs_.end() ? std::nullopt
                              : std::optional<JobSnapshot>(found->second);
}

/** @copydoc ps::server::SingleTenantJobService::cancel */
bool SingleTenantJobService::cancel(const JobId& job_id) noexcept {
  if (!job_id.valid()) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(job_id.value());
    if (found == jobs_.end() || is_terminal_job_state(found->second.state) ||
        found->second.cancellation_requested) {
      return false;
    }
    found->second.cancellation_requested = true;
    found->second.state = JobState::Cancelling;
  }
  condition_.notify_all();
  return true;
}

/** @copydoc ps::server::SingleTenantJobService::find_artifact */
std::shared_ptr<const ArtifactRecord> SingleTenantJobService::find_artifact(
    const ArtifactId& artifact_id) const {
  return artifact_store_.find(artifact_id);
}

/** @copydoc ps::server::SingleTenantJobService::run_assignment */
void SingleTenantJobService::run_assignment(JobAssignment assignment) noexcept {
  try {
    bool assignment_present = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = jobs_.find(assignment.identity.job_id.value());
      if (found != jobs_.end()) {
        assignment_present = true;
        if (!found->second.cancellation_requested) {
          found->second.state = JobState::Running;
        }
      }
    }
    condition_.notify_all();
    if (!assignment_present) {
      mark_worker_completed(assignment.identity.job_id);
      return;
    }

    std::unique_ptr<JobAttemptWorker> worker =
        worker_factory_->create(assignment);
    JobAttemptReport report;
    if (worker == nullptr) {
      report.identity = assignment.identity;
      report.outcome = JobAttemptOutcome::Failed;
      report.settled = false;
      report.failure = JobAttemptFailure::HostSetup;
      report.message = "worker factory returned null";
    } else {
      report =
          worker->execute(assignment, [this, job = assignment.identity.job_id] {
            return cancellation_requested_for(job);
          });
    }
    apply_report(assignment.identity, std::move(report));
  } catch (const std::exception& error) {
    JobAttemptReport report;
    report.identity = assignment.identity;
    report.outcome = JobAttemptOutcome::Failed;
    report.settled = false;
    report.failure = JobAttemptFailure::Unexpected;
    try {
      report.message = unexpected_message(error);
      apply_report(assignment.identity, std::move(report));
    } catch (...) {
      std::terminate();
    }
  } catch (...) {
    JobAttemptReport report;
    report.identity = assignment.identity;
    report.outcome = JobAttemptOutcome::Failed;
    report.settled = false;
    report.failure = JobAttemptFailure::Unexpected;
    try {
      report.message = "worker execution raised a non-standard exception";
      apply_report(assignment.identity, std::move(report));
    } catch (...) {
      std::terminate();
    }
  }
  mark_worker_completed(assignment.identity.job_id);
}

/** @copydoc ps::server::SingleTenantJobService::mark_worker_completed */
void SingleTenantJobService::mark_worker_completed(
    const JobId& job_id) noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = workers_.find(job_id.value());
    if (found != workers_.end()) {
      found->second.completed = true;
    }
  }
  condition_.notify_all();
}

/** @copydoc ps::server::SingleTenantJobService::reap_workers */
void SingleTenantJobService::reap_workers() noexcept {
  for (;;) {
    std::thread completed_worker;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] {
        return has_completed_worker_locked() ||
               (shutting_down_ && workers_.empty());
      });

      const auto completed = std::find_if(
          workers_.begin(), workers_.end(),
          [](const auto& entry) { return entry.second.completed; });
      if (completed == workers_.end()) {
        return;
      }
      completed_worker = std::move(completed->second.thread);
      workers_.erase(completed);
      ++workers_joining_;
    }

    completed_worker.join();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      --workers_joining_;
    }
    condition_.notify_all();
  }
}

/** @copydoc ps::server::SingleTenantJobService::has_completed_worker_locked */
bool SingleTenantJobService::has_completed_worker_locked() const noexcept {
  return std::any_of(workers_.begin(), workers_.end(),
                     [](const auto& entry) { return entry.second.completed; });
}

/** @copydoc ps::server::SingleTenantJobService::cancellation_requested_for */
bool SingleTenantJobService::cancellation_requested_for(
    const JobId& job_id) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = jobs_.find(job_id.value());
  return shutting_down_ || found == jobs_.end() ||
         found->second.cancellation_requested;
}

/** @copydoc ps::server::SingleTenantJobService::apply_report */
void SingleTenantJobService::apply_report(const AttemptIdentity& expected,
                                          JobAttemptReport report) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = jobs_.find(expected.job_id.value());
  if (found == jobs_.end()) {
    return;
  }
  JobSnapshot& job = found->second;
  if (is_terminal_job_state(job.state)) {
    return;
  }

  const bool identity_valid = [&] {
    try {
      validate_attempt_identity(report.identity);
      return expected == job.assignment && report.identity == expected;
    } catch (...) {
      return false;
    }
  }();
  if (!identity_valid) {
    reject_report(job,
                  "worker report identity does not match current assignment");
    condition_.notify_all();
    return;
  }
  if (!has_valid_worker_report_shape(report)) {
    reject_report(job, "worker report has an invalid semantic shape");
    condition_.notify_all();
    return;
  }
  if (report.outcome == JobAttemptOutcome::Cancelled &&
      !job.cancellation_requested) {
    reject_report(job,
                  "worker reported cancellation without control-plane intent");
    condition_.notify_all();
    return;
  }

  job.attempt_settled = report.settled;
  job.attempt_outcome = report.outcome;
  job.failure = report.failure;
  job.message = std::move(report.message);

  if (report.outcome == JobAttemptOutcome::Failed) {
    job.output_receipt.reset();
    job.state = JobState::Failed;
    if (job.message.empty()) {
      job.message = "worker attempt failed without a diagnostic";
    }
    condition_.notify_all();
    return;
  }

  if (job.cancellation_requested) {
    job.output_receipt.reset();
    job.state = JobState::Cancelled;
    job.failure = JobAttemptFailure::CancellationObserved;
    if (job.message.empty()) {
      job.message = "cancellation observed before artifact commit";
    }
    condition_.notify_all();
    return;
  }

  try {
    const OutputCommitReceipt receipt =
        artifact_store_.commit(ArtifactCommitRequest{
            job.assignment, job.spec->output_slot_id(), *report.image});
    if (receipt.attempt != job.assignment ||
        receipt.output_slot_id != job.spec->output_slot_id() ||
        !receipt.artifact_id.valid() || !receipt.output_commit_id.valid() ||
        receipt.achieved_durability != job.spec->requested_durability()) {
      throw std::logic_error("artifact receipt failed identity validation");
    }
    job.output_receipt = receipt;
    job.state = JobState::Succeeded;
    job.failure = JobAttemptFailure::None;
    job.message.clear();
  } catch (const std::exception& error) {
    job.output_receipt.reset();
    job.state = JobState::Failed;
    job.failure = JobAttemptFailure::ArtifactCommit;
    job.message = std::string("artifact commit failed: ") + error.what();
  }
  condition_.notify_all();
}

}  // namespace ps::server
