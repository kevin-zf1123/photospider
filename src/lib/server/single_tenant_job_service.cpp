/**
 * @file single_tenant_job_service.cpp
 * @brief Implements Issue #99 durable Job, quota, retry, and artifact truth.
 */
#include "server/single_tenant_job_service.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "server/single_tenant_job_service_test_access.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_manager.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/** @brief Process-wide uniqueness input for service identity namespaces. */
std::atomic<std::uint64_t> g_service_namespace_sequence{0U};

/**
 * @brief Reserves the next nonzero value from one identity sequence.
 * @tparam AfterInitialObservation Callable invoked after the first atomic
 * observation and before the saturation decision.
 * @param sequence Non-null caller-owned monotonic sequence.
 * @param after_initial_observation Test observation callback; production uses
 * an inline no-op.
 * @return Fresh sequence value, including `UINT64_MAX` for the final
 * reservation.
 * @throws std::invalid_argument when `sequence` is null.
 * @throws std::overflow_error when the sequence is already saturated.
 * @throws Any exception raised by `after_initial_observation` unchanged.
 * @note The successful relaxed compare/exchange is the uniqueness
 * linearization point. A saturated observation performs no write.
 */
template <typename AfterInitialObservation>
std::uint64_t reserve_next_identity_sequence_value_impl(
    std::atomic<std::uint64_t>* sequence,
    AfterInitialObservation&& after_initial_observation) {
  if (sequence == nullptr) {
    throw std::invalid_argument("identity sequence is null");
  }
  std::uint64_t observed = sequence->load(std::memory_order_relaxed);
  std::forward<AfterInitialObservation>(after_initial_observation)();
  for (;;) {
    if (observed == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("process identity sequence exhausted");
    }
    const std::uint64_t reserved = observed + 1U;
    if (sequence->compare_exchange_weak(observed, reserved,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
      return reserved;
    }
  }
}

/**
 * @brief Reserves one production identity value without test observation.
 * @param sequence Non-null caller-owned monotonic sequence.
 * @return Fresh sequence value.
 * @throws std::invalid_argument when `sequence` is null.
 * @throws std::overflow_error when the sequence is already saturated.
 */
std::uint64_t reserve_next_identity_sequence_value(
    std::atomic<std::uint64_t>* sequence) {
  return reserve_next_identity_sequence_value_impl(sequence, []() noexcept {});
}

/**
 * @brief Creates a collision-resistant opaque namespace for one service run.
 * @param tenant_id Configured tenant identity.
 * @param root Canonical locked state root.
 * @return Twenty-four lowercase hexadecimal characters.
 * @throws Hashing/allocation/sequence failures unchanged.
 * @note Durable identities retain this namespace in records. Restart creates a
 * different namespace and therefore cannot reuse a prior Job token even though
 * active identity counters are process-local.
 */
std::string make_service_namespace(const TenantId& tenant_id,
                                   const std::filesystem::path& root) {
  const auto system_ticks =
      std::chrono::system_clock::now().time_since_epoch().count();
  const auto steady_ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::uint64_t sequence =
      reserve_next_identity_sequence_value(&g_service_namespace_sequence);
  const std::string seed =
      tenant_id.value() + "|" + root.string() + "|" +
      std::to_string(static_cast<std::int64_t>(system_ticks)) + "|" +
      std::to_string(static_cast<std::int64_t>(steady_ticks)) + "|" +
      std::to_string(static_cast<std::int64_t>(::getpid())) + "|" +
      std::to_string(sequence);
  const JobSpecDigest digest = hash_job_spec_bytes(
      reinterpret_cast<const std::byte*>(seed.data()), seed.size());
  return digest.hex().substr(0U, 24U);
}

/**
 * @brief Builds one stable initial Job-scoped identity suffix.
 * @param service_namespace Collision-resistant current service namespace.
 * @param job_sequence Checked nonzero service-local Job sequence.
 * @return `<namespace>-<sequence>` opaque suffix.
 * @throws std::bad_alloc when construction fails.
 */
std::string job_suffix(std::string_view service_namespace,
                       std::uint64_t job_sequence) {
  return std::string(service_namespace) + "-" + std::to_string(job_sequence);
}

/**
 * @brief Mints one Job identity from a durable non-reused namespace/suffix.
 * @param suffix Exact current service Job suffix.
 * @return Fresh JobId.
 * @throws Identity/allocation failures unchanged.
 */
JobId mint_job_id(std::string_view suffix) {
  return JobId("job-v2-" + std::string(suffix));
}

/**
 * @brief Mints one attempt identity for an exact Job lease generation.
 * @param job_id Durable Job owner.
 * @param generation Positive current lease generation.
 * @return Fresh generation-qualified JobAttemptId.
 * @throws Identity/allocation failures unchanged.
 */
JobAttemptId mint_attempt_id(const JobId& job_id, std::uint64_t generation) {
  return JobAttemptId("attempt-v2-" + job_id.value() + "-" +
                      std::to_string(generation));
}

/**
 * @brief Mints one worker identity for an exact Job lease generation.
 * @param job_id Durable Job owner.
 * @param generation Positive current lease generation.
 * @return Fresh generation-qualified WorkerInstanceId.
 * @throws Identity/allocation failures unchanged.
 */
WorkerInstanceId mint_worker_id(const JobId& job_id, std::uint64_t generation) {
  return WorkerInstanceId("worker-v2-" + job_id.value() + "-" +
                          std::to_string(generation));
}

/**
 * @brief Mints one stable artifact identity at initial Job acceptance.
 * @param suffix Exact current service Job suffix.
 * @return Stable retry-preserved ArtifactId.
 * @throws Identity/allocation failures unchanged.
 */
ArtifactId mint_artifact_id(std::string_view suffix) {
  return ArtifactId("artifact-v2-" + std::string(suffix));
}

/**
 * @brief Mints one stable idempotent output transaction at Job acceptance.
 * @param suffix Exact current service Job suffix.
 * @return Stable retry-preserved OutputCommitId.
 * @throws Identity/allocation failures unchanged.
 */
OutputCommitId mint_commit_id(std::string_view suffix) {
  return OutputCommitId("commit-v2-" + std::string(suffix));
}

/**
 * @brief Reports whether a failure category may originate from a worker.
 * @param failure Candidate closed or invalid representation.
 * @return True only for one worker-owned failed-report category.
 * @throws Nothing.
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
    case JobAttemptFailure::RecoveryInterrupted:
    case JobAttemptFailure::WorkerStartup:
    case JobAttemptFailure::WorkerExit:
    case JobAttemptFailure::WorkerSignal:
    case JobAttemptFailure::WorkerChannel:
    case JobAttemptFailure::WorkerProtocol:
    case JobAttemptFailure::WorkerHeartbeatTimeout:
    case JobAttemptFailure::WorkerRuntimeTimeout:
    case JobAttemptFailure::WorkerCancellationForced:
      return false;
  }
  return false;
}

/**
 * @brief Validates the closed outcome/settlement/failure/image report shape.
 * @param report Identity-fenced candidate report.
 * @return True only for one complete supported worker report shape.
 * @throws Nothing.
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
    case JobAttemptOutcome::None:
      return false;
  }
  return false;
}

/**
 * @brief Publishes one fail-closed report rejection without trusting facts.
 * @param job Mutable current Job snapshot held under the service mutex.
 * @param message Stable control-plane diagnostic.
 * @return Nothing.
 * @throws std::bad_alloc when storing the diagnostic exhausts memory.
 */
void reject_report(JobSnapshot& job, std::string_view message) {
  job.state = JobState::Failed;
  job.attempt_outcome.reset();
  job.attempt_settled = false;
  job.failure = JobAttemptFailure::ReportRejected;
  job.message.assign(message);
  job.output_receipt.reset();
}

/**
 * @brief Checks whether a recovered artifact fulfills one stable Job output.
 * @param job Durable Job record.
 * @param artifact Candidate recovered immutable artifact.
 * @return True only when every stable identity and durability field matches.
 * @throws Nothing.
 */
bool artifact_fulfills_job(const DurableJobRecord& job,
                           const ArtifactRecord& artifact) noexcept {
  const OutputCommitReceipt& receipt = artifact.receipt;
  return receipt.attempt.tenant_id == job.tenant_id &&
         receipt.attempt.job_id == job.job_id && job.spec != nullptr &&
         receipt.attempt.job_spec_digest == job.spec->digest() &&
         receipt.output_slot_id == job.spec->output_slot_id() &&
         receipt.artifact_id == job.output_artifact_id &&
         receipt.output_commit_id == job.output_commit_id &&
         receipt.achieved_durability == ArtifactDurability::CrashDurable;
}

/**
 * @brief Revalidates one checkpoint record against exact admission authority.
 * @param tenant_id Configured tenant accepting the checkpoint.
 * @param artifact_id Exact checkpoint identity declared by the frozen JobSpec.
 * @param artifact Immutable durable record returned by the rooted store.
 * @return True only when tenant, identity, durability, size, and content digest
 * all agree.
 * @throws std::overflow_error when content hashing cannot represent its input.
 * @note Durable lookup already validates these facts during recovery. This
 * defensive join keeps submit and retry authorization identical even if a
 * future store implementation changes its in-memory indexing strategy.
 */
bool checkpoint_is_authorized(const TenantId& tenant_id,
                              const ArtifactId& artifact_id,
                              const ArtifactRecord& artifact) {
  const OutputCommitReceipt& receipt = artifact.receipt;
  return receipt.attempt.tenant_id == tenant_id &&
         receipt.artifact_id == artifact_id &&
         receipt.achieved_durability == ArtifactDurability::CrashDurable &&
         receipt.descriptor.payload_bytes == artifact.payload.size() &&
         receipt.content_digest ==
             hash_artifact_content(artifact.payload.data(),
                                   artifact.payload.size());
}

}  // namespace

/** @copydoc
 * ps::server::SingleTenantJobServiceTestAccess::reserve_identity_sequence_value
 */
std::uint64_t SingleTenantJobServiceTestAccess::reserve_identity_sequence_value(
    std::atomic<std::uint64_t>* sequence) {
  return reserve_next_identity_sequence_value(sequence);
}

/** @copydoc
 * ps::server::SingleTenantJobServiceTestAccess::reserve_identity_with_observer
 */
std::uint64_t SingleTenantJobServiceTestAccess::reserve_identity_with_observer(
    std::atomic<std::uint64_t>* sequence,
    const std::function<void()>& after_initial_observation) {
  if (!after_initial_observation) {
    throw std::invalid_argument("identity observation callback is empty");
  }
  return reserve_next_identity_sequence_value_impl(sequence,
                                                   after_initial_observation);
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

/** @copydoc ps::server::JobAttemptWorkerFactory::prepare_external_graph */
ResolvedGraphArtifact JobAttemptWorkerFactory::prepare_external_graph(
    const JobAssignment& assignment) const {
  static_cast<void>(assignment);
  throw std::logic_error(
      "worker factory does not support external assignment preparation");
}

/** @copydoc ps::server::SingleTenantJobService::SingleTenantJobService */
SingleTenantJobService::SingleTenantJobService(
    TenantId tenant_id, TenantQuotaLimits quota_limits,
    std::filesystem::path state_root,
    std::shared_ptr<JobAttemptWorkerFactory> worker_factory,
    DurableServerStateOptions state_options,
    TenantQuotaAuthorityOptions quota_options,
    WorkerManagerOptions worker_options)
    : tenant_id_(std::move(tenant_id)),
      worker_factory_(std::move(worker_factory)) {
  if (!tenant_id_.valid()) {
    throw std::invalid_argument("single-tenant service tenant is invalid");
  }
  if (worker_factory_ == nullptr) {
    throw std::invalid_argument("single-tenant worker factory is null");
  }
  const bool in_process_test_mode =
      dynamic_cast<InProcessJobAttemptWorkerFactoryForTest*>(
          worker_factory_.get()) != nullptr;
  WorkerManagerCallbacks callbacks;
  callbacks.begin_assignment = [this](const AttemptIdentity& identity) {
    return begin_managed_assignment(identity);
  };
  callbacks.cancellation_requested = [this](const AttemptIdentity& identity) {
    return cancellation_requested_for(identity);
  };
  callbacks.complete_assignment = [this](WorkerManagerCompletion completion) {
    apply_worker_completion(std::move(completion));
  };
  worker_manager_ = std::make_unique<WorkerManager>(
      worker_factory_, std::move(callbacks), std::move(worker_options),
      in_process_test_mode);
  durable_state_ = std::make_unique<DurableServerState>(
      std::move(state_root), tenant_id_, std::move(state_options));
  quota_authority_ = std::make_unique<TenantQuotaAuthority>(
      tenant_id_, std::move(quota_limits), std::move(quota_options));
  identity_namespace_ =
      make_service_namespace(tenant_id_, durable_state_->root());

  for (const auto& artifact : durable_state_->recovered_artifacts()) {
    quota_authority_->recover_retained_artifact(
        artifact->receipt.artifact_id,
        static_cast<std::uint64_t>(artifact->receipt.descriptor.payload_bytes));
  }
  for (DurableJobRecord recovered : durable_state_->recovered_jobs()) {
    JobSnapshot snapshot;
    snapshot.tenant_id = recovered.tenant_id;
    snapshot.job_id = recovered.job_id;
    snapshot.spec = recovered.spec;
    snapshot.assignment = recovered.assignment;
    snapshot.output_artifact_id = recovered.output_artifact_id;
    snapshot.output_commit_id = recovered.output_commit_id;
    snapshot.state = recovered.state;
    snapshot.cancellation_requested = recovered.cancellation_requested;
    snapshot.attempt_settled = recovered.attempt_settled;
    if (recovered.attempt_outcome != JobAttemptOutcome::None) {
      snapshot.attempt_outcome = recovered.attempt_outcome;
    }
    snapshot.failure = recovered.failure;
    snapshot.message = recovered.message;
    snapshot.output_receipt = recovered.output_receipt;

    const auto artifact =
        durable_state_->find_artifact(recovered.output_artifact_id);
    const bool artifact_matches =
        artifact != nullptr && artifact_fulfills_job(recovered, *artifact);
    if (artifact != nullptr && !artifact_matches) {
      throw DurableCorruptionError(
          "durable Job output identity conflicts with its stable artifact");
    }
    if (artifact_matches && recovered.state != JobState::Cancelled) {
      snapshot.state = JobState::Succeeded;
      snapshot.cancellation_requested = false;
      snapshot.attempt_settled = true;
      snapshot.attempt_outcome = JobAttemptOutcome::Succeeded;
      snapshot.failure = JobAttemptFailure::None;
      snapshot.message.clear();
      snapshot.output_receipt = artifact->receipt;
      persist_snapshot_locked(snapshot);
    } else if (!is_terminal_job_state(recovered.state)) {
      snapshot.state = JobState::Failed;
      snapshot.attempt_settled = true;
      snapshot.attempt_outcome = JobAttemptOutcome::Failed;
      snapshot.failure = JobAttemptFailure::RecoveryInterrupted;
      snapshot.message =
          "service restart interrupted the process-local current attempt";
      snapshot.output_receipt.reset();
      persist_snapshot_locked(snapshot);
    } else if (recovered.state == JobState::Cancelled && artifact_matches) {
      throw DurableCorruptionError(
          "cancelled durable Job conflicts with a committed stable artifact");
    }
    const std::string job_key = snapshot.job_id.value();
    const bool inserted =
        jobs_
            .emplace(job_key,
                     JobControlRecord{std::move(snapshot), std::nullopt})
            .second;
    if (!inserted) {
      throw DurableCorruptionError(
          "recovered durable Job identity is duplicated");
    }
  }
}

/** @copydoc ps::server::SingleTenantJobService::~SingleTenantJobService */
SingleTenantJobService::~SingleTenantJobService() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    for (auto& entry : jobs_) {
      if (!durable_mutation_faulted_locked() &&
          !is_terminal_job_state(entry.second.snapshot.state)) {
        JobSnapshot candidate = entry.second.snapshot;
        candidate.cancellation_requested = true;
        candidate.state = JobState::Cancelling;
        try {
          publish_snapshot_locked(entry.second, std::move(candidate));
        } catch (...) {
          // A pre-publication failure leaves prior truth; a post-publication
          // failure already aligned memory and entered fail-stop. Restart
          // classifies either surviving nonterminal record conservatively.
        }
      }
    }
  }
  condition_.notify_all();
  if (worker_manager_ != nullptr) {
    worker_manager_->shutdown();
  }
}

/** @copydoc ps::server::SingleTenantJobService::submit */
JobSubmission SingleTenantJobService::submit(JobSpec spec) {
  validate_job_spec(spec);
  auto immutable_spec = std::make_shared<const JobSpec>(std::move(spec));
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutting_down_) {
    throw std::invalid_argument("single-tenant service is shutting down");
  }
  require_durable_mutation_available_locked();
  std::shared_ptr<const ArtifactRecord> checkpoint;
  if (immutable_spec->checkpoint_artifact_id().has_value()) {
    checkpoint = durable_state_->find_artifact(
        *immutable_spec->checkpoint_artifact_id());
    if (checkpoint == nullptr ||
        !checkpoint_is_authorized(tenant_id_,
                                  *immutable_spec->checkpoint_artifact_id(),
                                  *checkpoint)) {
      throw std::invalid_argument(
          "Job checkpoint does not resolve to this tenant's durable artifact");
    }
  }
  if (next_job_sequence_ == 0U) {
    throw std::overflow_error("service Job identity sequence exhausted");
  }
  const std::uint64_t sequence = next_job_sequence_++;
  const std::string suffix = job_suffix(identity_namespace_, sequence);
  JobAssignment assignment;
  assignment.identity.tenant_id = tenant_id_;
  assignment.identity.job_id = mint_job_id(suffix);
  assignment.identity.job_spec_digest = immutable_spec->digest();
  assignment.identity.worker_lease_generation = WorkerLeaseGeneration{1U};
  assignment.identity.attempt_id =
      mint_attempt_id(assignment.identity.job_id, 1U);
  assignment.identity.worker_instance_id =
      mint_worker_id(assignment.identity.job_id, 1U);
  assignment.spec = immutable_spec;
  assignment.checkpoint = std::move(checkpoint);
  validate_attempt_identity(assignment.identity);

  JobSnapshot snapshot;
  snapshot.tenant_id = tenant_id_;
  snapshot.job_id = assignment.identity.job_id;
  snapshot.spec = immutable_spec;
  snapshot.assignment = assignment.identity;
  snapshot.output_artifact_id = mint_artifact_id(suffix);
  snapshot.output_commit_id = mint_commit_id(suffix);
  snapshot.state = JobState::Queued;

  JobSubmission submission{snapshot.job_id, immutable_spec->digest(),
                           assignment.identity};
  const DurableJobRecord durable_candidate = durable_record(snapshot);
  const std::string job_key = snapshot.job_id.value();
  std::optional<TenantQuotaReservation> rollback_reservation(
      quota_authority_->reserve(snapshot.job_id,
                                immutable_spec->resource_request()));
  bool job_inserted = false;
  decltype(jobs_)::iterator job_record = jobs_.end();
  try {
    const auto job = jobs_.emplace(
        job_key, JobControlRecord{snapshot, rollback_reservation});
    job_record = job.first;
    job_inserted = job.second;
    if (!job_inserted) {
      throw std::logic_error("fresh durable JobId collided in service state");
    }
    rollback_reservation.reset();
    worker_manager_->start(std::move(assignment));
  } catch (...) {
    std::exception_ptr failure = std::current_exception();
    std::optional<TenantQuotaReservation>& reservation =
        job_inserted ? job_record->second.reservation : rollback_reservation;
    if (!try_release_attempt_locked(reservation)) {
      retain_stranded_reservation_locked(reservation);
    }
    if (job_inserted) {
      jobs_.erase(job_record);
    }
    condition_.notify_all();
    std::rethrow_exception(failure);
  }

  const DurableJobCommitResult commit =
      durable_state_->persist_job(durable_candidate);
  if (!commit.succeeded()) {
    if (!commit.published()) {
      if (!try_release_attempt_locked(job_record->second.reservation)) {
        retain_stranded_reservation_locked(job_record->second.reservation);
      }
      jobs_.erase(job_record);
      condition_.notify_all();
      commit.rethrow_failure();
      throw std::logic_error(
          "unpublished Job journal failure omitted its exception");
    }
    journal_faulted_ = true;
    condition_.notify_all();
    throw DurableJobCommitError(snapshot.job_id, commit.state, commit.failure);
  }
  return submission;
}

/** @copydoc ps::server::SingleTenantJobService::retry */
std::optional<JobSubmission> SingleTenantJobService::retry(
    const JobId& job_id) {
  if (!job_id.valid()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  require_durable_mutation_available_locked();
  const auto found = jobs_.find(job_id.value());
  if (found == jobs_.end() || shutting_down_ ||
      found->second.snapshot.state != JobState::Failed ||
      !found->second.snapshot.attempt_settled ||
      found->second.reservation.has_value() ||
      worker_manager_->owns_attempt(found->second.snapshot.assignment)) {
    return std::nullopt;
  }
  const JobSnapshot previous = found->second.snapshot;
  if (previous.assignment.worker_lease_generation.value ==
      std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("Job retry lease generation exhausted");
  }
  std::shared_ptr<const ArtifactRecord> checkpoint;
  if (previous.spec->checkpoint_artifact_id().has_value()) {
    checkpoint =
        durable_state_->find_artifact(*previous.spec->checkpoint_artifact_id());
    if (checkpoint == nullptr ||
        !checkpoint_is_authorized(tenant_id_,
                                  *previous.spec->checkpoint_artifact_id(),
                                  *checkpoint)) {
      throw std::invalid_argument(
          "Job retry checkpoint is no longer a durable tenant artifact");
    }
  }
  const std::uint64_t generation =
      previous.assignment.worker_lease_generation.value + 1U;
  JobAssignment assignment;
  assignment.identity.tenant_id = tenant_id_;
  assignment.identity.job_id = job_id;
  assignment.identity.job_spec_digest = previous.spec->digest();
  assignment.identity.attempt_id = mint_attempt_id(job_id, generation);
  assignment.identity.worker_instance_id = mint_worker_id(job_id, generation);
  assignment.identity.worker_lease_generation =
      WorkerLeaseGeneration{generation};
  assignment.spec = previous.spec;
  assignment.checkpoint = std::move(checkpoint);

  JobSnapshot replacement = previous;
  replacement.assignment = assignment.identity;
  replacement.state = JobState::Queued;
  replacement.cancellation_requested = false;
  replacement.attempt_settled = false;
  replacement.attempt_outcome.reset();
  replacement.failure = JobAttemptFailure::None;
  replacement.message.clear();
  replacement.output_receipt.reset();
  JobSubmission submission{job_id, previous.spec->digest(),
                           assignment.identity};
  const DurableJobRecord durable_candidate = durable_record(replacement);
  TenantQuotaReservation reservation =
      quota_authority_->reserve(job_id, previous.spec->resource_request());
  JobControlRecord replacement_control{
      std::move(replacement),
      std::optional<TenantQuotaReservation>(std::move(reservation))};
  try {
    worker_manager_->start(std::move(assignment));
  } catch (...) {
    std::exception_ptr failure = std::current_exception();
    if (replacement_control.reservation.has_value()) {
      if (!try_release_attempt_locked(replacement_control.reservation)) {
        retain_stranded_reservation_locked(replacement_control.reservation);
      }
    }
    condition_.notify_all();
    std::rethrow_exception(failure);
  }

  const DurableJobCommitResult commit =
      durable_state_->persist_job(durable_candidate);
  if (!commit.published() && !commit.succeeded()) {
    if (!try_release_attempt_locked(replacement_control.reservation)) {
      retain_stranded_reservation_locked(replacement_control.reservation);
    }
    condition_.notify_all();
    commit.rethrow_failure();
    throw std::logic_error(
        "unpublished retry journal failure omitted its exception");
  }
  std::swap(found->second, replacement_control);
  if (!commit.succeeded()) {
    journal_faulted_ = true;
    condition_.notify_all();
    throw DurableJobCommitError(job_id, commit.state, commit.failure);
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
  return found == jobs_.end()
             ? std::nullopt
             : std::optional<JobSnapshot>(found->second.snapshot);
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
    return found == jobs_.end() ||
           is_terminal_job_state(found->second.snapshot.state);
  });
  if (!completed) {
    return std::nullopt;
  }
  const auto found = jobs_.find(job_id.value());
  return found == jobs_.end()
             ? std::nullopt
             : std::optional<JobSnapshot>(found->second.snapshot);
}

/** @copydoc ps::server::SingleTenantJobService::cancel */
bool SingleTenantJobService::cancel(const JobId& job_id) {
  if (!job_id.valid()) {
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  require_durable_mutation_available_locked();
  const auto found = jobs_.find(job_id.value());
  if (found == jobs_.end() ||
      is_terminal_job_state(found->second.snapshot.state) ||
      found->second.snapshot.cancellation_requested) {
    return false;
  }
  const AttemptIdentity assignment = found->second.snapshot.assignment;
  JobSnapshot replacement = found->second.snapshot;
  replacement.cancellation_requested = true;
  replacement.state = JobState::Cancelling;
  publish_snapshot_locked(found->second, std::move(replacement));
  lock.unlock();
  condition_.notify_all();
  static_cast<void>(worker_manager_->request_cancel(assignment));
  return true;
}

/** @copydoc ps::server::SingleTenantJobService::find_artifact */
std::shared_ptr<const ArtifactRecord> SingleTenantJobService::find_artifact(
    const ArtifactId& artifact_id) const {
  return durable_state_->find_artifact(artifact_id);
}

/** @copydoc ps::server::SingleTenantJobService::delete_artifact */
bool SingleTenantJobService::delete_artifact(const ArtifactId& artifact_id) {
  if (!artifact_id.valid()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  require_durable_mutation_available_locked();
  for (const auto& entry : jobs_) {
    const JobSnapshot& snapshot = entry.second.snapshot;
    if (!is_terminal_job_state(snapshot.state) &&
        snapshot.spec->checkpoint_artifact_id().has_value() &&
        *snapshot.spec->checkpoint_artifact_id() == artifact_id) {
      return false;
    }
  }
  const DurableArtifactEraseResult erased =
      durable_state_->erase_artifact(artifact_id);
  if (!erased.visibility_removed()) {
    erased.rethrow_failure();
    return false;
  }

  std::uint64_t released_bytes = 0U;
  std::exception_ptr failure = erased.failure;
  if (erased.visibility_removal_confirmed()) {
    try {
      released_bytes = quota_authority_->release_retained_artifact(artifact_id);
      if (erased.payload_bytes != 0U &&
          released_bytes != erased.payload_bytes) {
        throw std::logic_error(
            "durable artifact removal disagrees with retained quota charge");
      }
    } catch (...) {
      failure = std::current_exception();
    }
  }
  if (failure == nullptr && !erased.succeeded()) {
    try {
      throw std::logic_error(
          "durable artifact removal stopped without a captured failure");
    } catch (...) {
      failure = std::current_exception();
    }
  }
  if (failure != nullptr) {
    artifact_erase_faulted_ = true;
    condition_.notify_all();
    throw DurableArtifactEraseError(artifact_id, erased.state,
                                    erased.payload_bytes, failure);
  }
  return erased.payload_bytes != 0U || released_bytes != 0U;
}

/** @copydoc ps::server::SingleTenantJobService::quota_snapshot */
TenantQuotaSnapshot SingleTenantJobService::quota_snapshot() const {
  return quota_authority_->snapshot();
}

/** @copydoc ps::server::SingleTenantJobService::begin_managed_assignment */
bool SingleTenantJobService::begin_managed_assignment(
    const AttemptIdentity& expected) {
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(expected.job_id.value());
    if (!durable_mutation_faulted_locked() && found != jobs_.end() &&
        found->second.snapshot.assignment == expected &&
        !is_terminal_job_state(found->second.snapshot.state)) {
      accepted = true;
      if (!found->second.snapshot.cancellation_requested) {
        JobSnapshot running = found->second.snapshot;
        running.state = JobState::Running;
        publish_snapshot_locked(found->second, std::move(running));
      }
    }
  }
  condition_.notify_all();
  return accepted;
}

/** @copydoc ps::server::SingleTenantJobService::apply_worker_completion */
void SingleTenantJobService::apply_worker_completion(
    WorkerManagerCompletion completion) noexcept {
  if (completion.kind == WorkerManagerCompletionKind::Report &&
      completion.report.has_value()) {
    apply_report(completion.identity, std::move(*completion.report));
    return;
  }
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(completion.identity.job_id.value());
    if (durable_mutation_faulted_locked() || found == jobs_.end() ||
        found->second.snapshot.assignment != completion.identity ||
        is_terminal_job_state(found->second.snapshot.state)) {
      return;
    }
    JobControlRecord& control = found->second;
    JobSnapshot candidate = control.snapshot;
    candidate.attempt_settled = true;
    candidate.output_receipt.reset();
    if (completion.kind == WorkerManagerCompletionKind::ForcedCancellation &&
        candidate.cancellation_requested &&
        completion.failure == JobAttemptFailure::WorkerCancellationForced &&
        !completion.report.has_value()) {
      candidate.state = JobState::Cancelled;
      candidate.attempt_outcome = JobAttemptOutcome::Cancelled;
      candidate.failure = JobAttemptFailure::WorkerCancellationForced;
      candidate.message = std::move(completion.message);
      if (candidate.message.empty()) {
        candidate.message = "worker was forcibly cancelled after exact reaping";
      }
    } else {
      candidate.state = JobState::Failed;
      candidate.attempt_outcome = JobAttemptOutcome::Failed;
      candidate.failure = completion.failure;
      candidate.message = std::move(completion.message);
      if (completion.kind == WorkerManagerCompletionKind::Report ||
          completion.report.has_value() ||
          candidate.failure < JobAttemptFailure::WorkerStartup ||
          candidate.failure > JobAttemptFailure::WorkerRuntimeTimeout) {
        candidate.failure = JobAttemptFailure::WorkerProtocol;
        candidate.message =
            "WorkerManager delivered an invalid completion shape";
      } else if (candidate.message.empty()) {
        candidate.message = "worker process failed without a diagnostic";
      }
    }
    publish_snapshot_locked(control, std::move(candidate));
    static_cast<void>(try_release_attempt_locked(control.reservation));
    condition_.notify_all();
  } catch (...) {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = jobs_.find(completion.identity.job_id.value());
      if (!durable_mutation_faulted_locked() && found != jobs_.end() &&
          found->second.snapshot.assignment == completion.identity &&
          !is_terminal_job_state(found->second.snapshot.state)) {
        JobSnapshot failure = found->second.snapshot;
        failure.state = JobState::Failed;
        failure.attempt_settled = true;
        failure.attempt_outcome = JobAttemptOutcome::Failed;
        failure.failure = JobAttemptFailure::ArtifactCommit;
        failure.message = "durable manager-completion publication failed";
        failure.output_receipt.reset();
        try {
          publish_snapshot_locked(found->second, std::move(failure));
          static_cast<void>(
              try_release_attempt_locked(found->second.reservation));
        } catch (...) {
          journal_faulted_ = true;
        }
        condition_.notify_all();
      }
    } catch (...) {
      std::terminate();
    }
  }
}

/** @copydoc ps::server::SingleTenantJobService::cancellation_requested_for */
bool SingleTenantJobService::cancellation_requested_for(
    const AttemptIdentity& expected) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = jobs_.find(expected.job_id.value());
  return shutting_down_ || durable_mutation_faulted_locked() ||
         found == jobs_.end() ||
         found->second.snapshot.assignment != expected ||
         found->second.snapshot.cancellation_requested;
}

/** @copydoc ps::server::SingleTenantJobService::apply_report */
void SingleTenantJobService::apply_report(const AttemptIdentity& expected,
                                          JobAttemptReport report) noexcept {
  bool report_facts_accepted = false;
  JobAttemptOutcome accepted_outcome = JobAttemptOutcome::None;
  bool accepted_settled = false;
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(expected.job_id.value());
    if (durable_mutation_faulted_locked() || found == jobs_.end() ||
        is_terminal_job_state(found->second.snapshot.state)) {
      return;
    }
    JobControlRecord& control = found->second;
    if (expected != control.snapshot.assignment) {
      return;
    }
    JobSnapshot candidate = control.snapshot;
    const bool identity_valid = [&] {
      try {
        validate_attempt_identity(report.identity);
        return report.identity == expected;
      } catch (...) {
        return false;
      }
    }();
    if (!identity_valid) {
      reject_report(candidate,
                    "worker report identity does not match current assignment");
      publish_snapshot_locked(control, candidate);
      static_cast<void>(try_release_attempt_locked(control.reservation));
      condition_.notify_all();
      return;
    }
    if (!has_valid_worker_report_shape(report)) {
      reject_report(candidate, "worker report has an invalid semantic shape");
      publish_snapshot_locked(control, candidate);
      static_cast<void>(try_release_attempt_locked(control.reservation));
      condition_.notify_all();
      return;
    }
    if (report.outcome == JobAttemptOutcome::Cancelled &&
        !candidate.cancellation_requested) {
      reject_report(
          candidate,
          "worker reported cancellation without control-plane intent");
      publish_snapshot_locked(control, candidate);
      static_cast<void>(try_release_attempt_locked(control.reservation));
      condition_.notify_all();
      return;
    }

    report_facts_accepted = true;
    accepted_outcome = report.outcome;
    accepted_settled = report.settled;
    candidate.attempt_settled = report.settled;
    candidate.attempt_outcome = report.outcome;
    candidate.failure = report.failure;
    candidate.message = std::move(report.message);
    if (report.outcome == JobAttemptOutcome::Failed) {
      candidate.output_receipt.reset();
      candidate.state = JobState::Failed;
      if (candidate.message.empty()) {
        candidate.message = "worker attempt failed without a diagnostic";
      }
      publish_snapshot_locked(control, candidate);
      static_cast<void>(try_release_attempt_locked(control.reservation));
      condition_.notify_all();
      return;
    }
    if (candidate.cancellation_requested) {
      candidate.output_receipt.reset();
      candidate.state = JobState::Cancelled;
      candidate.failure = JobAttemptFailure::CancellationObserved;
      if (candidate.message.empty()) {
        candidate.message = "cancellation observed before artifact commit";
      }
      publish_snapshot_locked(control, candidate);
      static_cast<void>(try_release_attempt_locked(control.reservation));
      condition_.notify_all();
      return;
    }
    if (!control.reservation.has_value()) {
      throw std::logic_error(
          "successful current attempt has no quota reservation");
    }

    const OutputCommitReceipt receipt = durable_state_->commit_artifact(
        DurableArtifactCommitRequest{
            candidate.assignment, candidate.spec->output_slot_id(),
            candidate.output_artifact_id, candidate.output_commit_id,
            candidate.spec->resource_request()},
        *report.image);
    if (receipt.attempt.tenant_id != candidate.tenant_id ||
        receipt.attempt.job_id != candidate.job_id ||
        receipt.attempt.job_spec_digest != candidate.spec->digest() ||
        receipt.output_slot_id != candidate.spec->output_slot_id() ||
        receipt.artifact_id != candidate.output_artifact_id ||
        receipt.output_commit_id != candidate.output_commit_id ||
        receipt.achieved_durability != ArtifactDurability::CrashDurable) {
      throw std::logic_error(
          "durable artifact receipt failed stable identity join");
    }
    quota_authority_->commit_retained_artifact(
        control.reservation->id, receipt.artifact_id,
        static_cast<std::uint64_t>(receipt.descriptor.payload_bytes));
    control.reservation.reset();
    candidate.output_receipt = receipt;
    candidate.state = JobState::Succeeded;
    candidate.failure = JobAttemptFailure::None;
    candidate.message.clear();
    publish_snapshot_locked(control, candidate);
    condition_.notify_all();
  } catch (const std::exception& error) {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = jobs_.find(expected.job_id.value());
      if (durable_mutation_faulted_locked() || found == jobs_.end() ||
          found->second.snapshot.assignment != expected ||
          is_terminal_job_state(found->second.snapshot.state)) {
        return;
      }
      JobControlRecord& control = found->second;
      try {
        if (reconcile_published_artifact_locked(control)) {
          condition_.notify_all();
          return;
        }
      } catch (...) {
        if (durable_mutation_faulted_locked()) {
          condition_.notify_all();
          return;
        }
        // The durable occurrence remains restart-reconcilable; publish the
        // strongest process-local failure state possible below.
      }
      JobSnapshot failure = control.snapshot;
      failure.state = JobState::Failed;
      failure.attempt_outcome =
          report_facts_accepted
              ? std::optional<JobAttemptOutcome>(accepted_outcome)
              : std::nullopt;
      failure.attempt_settled = report_facts_accepted && accepted_settled;
      failure.failure = JobAttemptFailure::ArtifactCommit;
      failure.output_receipt.reset();
      failure.message =
          std::string("durable terminal publication failed: ") + error.what();
      try {
        publish_snapshot_locked(control, failure);
      } catch (...) {
        journal_faulted_ = true;
        condition_.notify_all();
        return;
      }
      static_cast<void>(try_release_attempt_locked(control.reservation));
      condition_.notify_all();
    } catch (...) {
      std::terminate();
    }
  } catch (...) {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = jobs_.find(expected.job_id.value());
      if (!durable_mutation_faulted_locked() && found != jobs_.end() &&
          found->second.snapshot.assignment == expected &&
          !is_terminal_job_state(found->second.snapshot.state)) {
        try {
          if (reconcile_published_artifact_locked(found->second)) {
            condition_.notify_all();
            return;
          }
        } catch (...) {
          if (durable_mutation_faulted_locked()) {
            condition_.notify_all();
            return;
          }
          // The durable occurrence remains restart-reconcilable; publish the
          // strongest process-local failure state possible below.
        }
        JobSnapshot failure = found->second.snapshot;
        failure.state = JobState::Failed;
        failure.attempt_outcome =
            report_facts_accepted
                ? std::optional<JobAttemptOutcome>(accepted_outcome)
                : std::nullopt;
        failure.attempt_settled = report_facts_accepted && accepted_settled;
        failure.failure = JobAttemptFailure::ArtifactCommit;
        failure.output_receipt.reset();
        failure.message =
            "durable terminal publication raised a non-standard exception";
        try {
          publish_snapshot_locked(found->second, failure);
        } catch (...) {
          journal_faulted_ = true;
          condition_.notify_all();
          return;
        }
        static_cast<void>(
            try_release_attempt_locked(found->second.reservation));
        condition_.notify_all();
      }
    } catch (...) {
      std::terminate();
    }
  }
}

/** @copydoc
 * ps::server::SingleTenantJobService::reconcile_published_artifact_locked */
bool SingleTenantJobService::reconcile_published_artifact_locked(
    JobControlRecord& control) {
  std::shared_ptr<const ArtifactRecord> artifact;
  try {
    artifact =
        durable_state_->find_artifact(control.snapshot.output_artifact_id);
  } catch (...) {
    fail_stop_artifact_reconciliation_locked();
    throw;
  }
  if (artifact == nullptr) {
    return false;
  }
  try {
    if (!artifact_fulfills_job(durable_record(control.snapshot), *artifact)) {
      throw DurableCorruptionError(
          "visible stable artifact conflicts during Job reconciliation");
    }
    if (control.reservation.has_value()) {
      quota_authority_->commit_retained_artifact(
          control.reservation->id, artifact->receipt.artifact_id,
          static_cast<std::uint64_t>(
              artifact->receipt.descriptor.payload_bytes));
      control.reservation.reset();
    }
    JobSnapshot succeeded = control.snapshot;
    succeeded.state = JobState::Succeeded;
    succeeded.cancellation_requested = false;
    succeeded.attempt_settled = true;
    succeeded.attempt_outcome = JobAttemptOutcome::Succeeded;
    succeeded.failure = JobAttemptFailure::None;
    succeeded.message.clear();
    succeeded.output_receipt = artifact->receipt;
    publish_snapshot_locked(control, std::move(succeeded));
  } catch (...) {
    fail_stop_artifact_reconciliation_locked();
    throw;
  }
  return true;
}

/** @copydoc ps::server::SingleTenantJobService::durable_record */
DurableJobRecord SingleTenantJobService::durable_record(
    const JobSnapshot& snapshot) const {
  DurableJobRecord record;
  record.tenant_id = snapshot.tenant_id;
  record.job_id = snapshot.job_id;
  record.spec = snapshot.spec;
  record.assignment = snapshot.assignment;
  record.output_artifact_id = snapshot.output_artifact_id;
  record.output_commit_id = snapshot.output_commit_id;
  record.state = snapshot.state;
  record.cancellation_requested = snapshot.cancellation_requested;
  record.attempt_settled = snapshot.attempt_settled;
  record.attempt_outcome =
      snapshot.attempt_outcome.value_or(JobAttemptOutcome::None);
  record.failure = snapshot.failure;
  record.message = snapshot.message;
  record.output_receipt = snapshot.output_receipt;
  return record;
}

/** @copydoc ps::server::SingleTenantJobService::persist_snapshot_locked */
void SingleTenantJobService::persist_snapshot_locked(
    const JobSnapshot& snapshot) {
  const DurableJobCommitResult commit =
      durable_state_->persist_job(durable_record(snapshot));
  if (commit.succeeded()) {
    return;
  }
  if (!commit.published()) {
    commit.rethrow_failure();
    throw std::logic_error(
        "unpublished Job journal failure omitted its exception");
  }
  journal_faulted_ = true;
  throw DurableJobCommitError(snapshot.job_id, commit.state, commit.failure);
}

/** @copydoc ps::server::SingleTenantJobService::publish_snapshot_locked */
void SingleTenantJobService::publish_snapshot_locked(JobControlRecord& control,
                                                     JobSnapshot candidate) {
  const DurableJobCommitResult commit =
      durable_state_->persist_job(durable_record(candidate));
  if (commit.published()) {
    std::swap(control.snapshot, candidate);
  }
  if (commit.succeeded()) {
    return;
  }
  if (!commit.published()) {
    commit.rethrow_failure();
    throw std::logic_error(
        "unpublished Job journal failure omitted its exception");
  }
  journal_faulted_ = true;
  condition_.notify_all();
  throw DurableJobCommitError(control.snapshot.job_id, commit.state,
                              commit.failure);
}

/** @copydoc
 * ps::server::SingleTenantJobService::fail_stop_artifact_reconciliation_locked
 */
void SingleTenantJobService::
    fail_stop_artifact_reconciliation_locked() noexcept {
  artifact_reconciliation_faulted_ = true;
  condition_.notify_all();
}

/** @copydoc ps::server::SingleTenantJobService::try_release_attempt_locked */
bool SingleTenantJobService::try_release_attempt_locked(
    std::optional<TenantQuotaReservation>& reservation) noexcept {
  if (!reservation.has_value()) {
    return true;
  }
  try {
    quota_authority_->release_attempt(reservation->id);
    reservation.reset();
    return true;
  } catch (...) {
    quota_release_faulted_ = true;
    condition_.notify_all();
    return false;
  }
}

/** @copydoc
 * ps::server::SingleTenantJobService::retain_stranded_reservation_locked
 */
void SingleTenantJobService::retain_stranded_reservation_locked(
    std::optional<TenantQuotaReservation>& reservation) noexcept {
  if (!quota_release_faulted_ || !reservation.has_value() ||
      stranded_reservation_.has_value()) {
    std::terminate();
  }
  stranded_reservation_ = std::move(reservation);
  reservation.reset();
}

/** @copydoc
 * ps::server::SingleTenantJobService::require_durable_mutation_available_locked
 */
void SingleTenantJobService::require_durable_mutation_available_locked() const {
  if (durable_mutation_faulted_locked()) {
    if (quota_release_faulted_) {
      throw DurableStateError(
          "tenant quota release is fail-stopped; restart required");
    }
    throw DurableStateError(
        "durable server mutation is fail-stopped; restart required");
  }
}

}  // namespace ps::server
