/**
 * @file test_worker_supervisor.cpp
 * @brief Verifies Issue #100 real-process crash isolation and bounded
 * lifecycle.
 */
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)
#include "server/single_tenant_job_service_test_access.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_TEST_WORKER_FIXTURE_PATH
#error "PS_TEST_WORKER_FIXTURE_PATH must name the real-process fixture"
#endif

namespace ps::server {
namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;

/**
 * @brief Owns one unique durable root through all service restarts in a test.
 * @throws Filesystem failures when creation fails.
 */
class ScopedSupervisorRoot final {
 public:
  /** @brief Creates one unique empty directory. */
  ScopedSupervisorRoot() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("photospider-issue100-supervisor-" + std::to_string(ticks) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create supervisor test root");
    }
  }

  /** @brief Best-effort removes the exact root after all services close. */
  ~ScopedSupervisorRoot() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  /** @brief Prevents duplicate root ownership. */
  ScopedSupervisorRoot(const ScopedSupervisorRoot& other) = delete;
  /** @brief Prevents duplicate root assignment. */
  ScopedSupervisorRoot& operator=(const ScopedSupervisorRoot& other) = delete;

  /**
   * @brief Returns the exact existing root path.
   * @return Borrowed path valid for this owner lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Sole recursively cleaned durable root. */
  std::filesystem::path path_;
};

/**
 * @brief Externalizable fixture factory that cannot execute in control plane.
 * @throws Nothing for construction.
 */
class FixtureWorkerFactory final : public JobAttemptWorkerFactory {
 public:
  /**
   * @brief Fails if the product path ever invokes an in-process worker.
   * @param assignment Exact assignment, intentionally unused.
   * @return Never returns.
   * @throws std::logic_error unconditionally.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) override {
    static_cast<void>(assignment);
    throw std::logic_error(
        "fixture factory must not execute in the control-plane process");
  }

  /**
   * @brief Advertises exact external assignment support.
   * @return True.
   * @throws Nothing.
   */
  bool supports_external_assignment() const noexcept override { return true; }

  /**
   * @brief Prepares deterministic graph-id-selected fixture material.
   * @param assignment Exact current assignment.
   * @return Bounded trusted placeholder paths; the fixture keys off JobSpec.
   * @throws std::invalid_argument when the assignment has no JobSpec.
   */
  ResolvedGraphArtifact prepare_external_graph(
      const JobAssignment& assignment) const override {
    if (assignment.spec == nullptr) {
      throw std::invalid_argument("fixture assignment has no JobSpec");
    }
    ResolvedGraphArtifact graph;
    graph.ok = true;
    graph.root_dir = "/fixture";
    graph.yaml_path =
        "/fixture/" + assignment.spec->graph_artifact_id().value() + ".yaml";
    return graph;
  }
};

/**
 * @brief Unmarked in-process-only factory used to prove product rejection.
 * @throws Nothing for construction.
 */
class UnmarkedWorkerFactory final : public JobAttemptWorkerFactory {
 public:
  /**
   * @brief Returns null if reached; construction must reject first.
   * @param assignment Exact assignment, unused.
   * @return Null worker.
   * @throws Nothing.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) override {
    static_cast<void>(assignment);
    return nullptr;
  }
};

/**
 * @brief Builds complete finite resources realistic for a freshly execed image.
 * @return 512-GiB Darwin-compatible address-space envelope and one-MiB
 * artifact bounds.
 * @throws Nothing.
 */
JobResourceRequest supervisor_resources() {
  JobResourceRequest request;
  request.cpu_slots = 1U;
  request.host_memory_bytes = 512ULL << 30U;
  request.output_bytes = 1U << 20U;
  request.staging_bytes = 1U << 20U;
  request.retention_bytes = 1U << 20U;
  return request;
}

/**
 * @brief Builds permissive finite capacity for concurrent process tests.
 * @return Capacity for eight active 512-GiB attempts and retained outputs.
 * @throws Nothing.
 */
TenantQuotaLimits supervisor_quota() {
  TenantQuotaLimits limits;
  limits.maximum_active_attempts = 8U;
  limits.capacity.cpu_slots = 8U;
  limits.capacity.host_memory_bytes = 4ULL << 40U;
  limits.capacity.output_bytes = 8U << 20U;
  limits.capacity.staging_bytes = 8U << 20U;
  limits.capacity.retention_bytes = 8U << 20U;
  return limits;
}

/**
 * @brief Builds short deterministic manager bounds for the fixture.
 * @return Valid process options using the configured fixture executable.
 * @throws Path allocation failures unchanged.
 */
WorkerManagerOptions supervisor_options() {
  WorkerManagerOptions options;
  options.worker_executable = PS_TEST_WORKER_FIXTURE_PATH;
  options.startup_timeout = 2s;
  options.heartbeat_interval = 25ms;
  options.heartbeat_timeout = 180ms;
  options.attempt_runtime_timeout = 3s;
  options.post_report_timeout = 150ms;
  options.cooperative_cancel_timeout = 100ms;
  options.terminate_timeout = 100ms;
  options.kill_reap_timeout = 500ms;
  options.io_timeout = 500ms;
  return options;
}

/**
 * @brief Builds one fixture-selected immutable JobSpec.
 * @param mode Exact graph id interpreted only by the process fixture.
 * @param checkpoint Optional durable checkpoint identity.
 * @return Complete supported JobSpec.
 * @throws Contract/allocation failures unchanged.
 */
JobSpec fixture_spec(std::string mode,
                     std::optional<ArtifactId> checkpoint = std::nullopt) {
  return JobSpec(GraphArtifactId(std::move(mode)), 0,
                 OutputSlotId("image.final"), supervisor_resources(),
                 std::move(checkpoint));
}

/**
 * @brief Creates one real product-mode service using the fixture executable.
 * @param root Existing durable root.
 * @param options Valid manager options.
 * @return Unique service owner.
 * @throws Service construction failures unchanged.
 */
std::unique_ptr<SingleTenantJobService> make_service(
    const std::filesystem::path& root, WorkerManagerOptions options) {
  return std::make_unique<SingleTenantJobService>(
      TenantId("tenant.supervisor"), supervisor_quota(), root,
      std::make_shared<FixtureWorkerFactory>(), DurableServerStateOptions{},
      TenantQuotaAuthorityOptions{}, std::move(options));
}

/**
 * @brief Waits for one terminal Job and asserts presence.
 * @param service Live service.
 * @param job_id Exact accepted Job identity.
 * @return Copied terminal snapshot.
 * @throws Test assertion failure as runtime error when unexpectedly absent.
 */
JobSnapshot wait_terminal(SingleTenantJobService& service,
                          const JobId& job_id) {
  std::optional<JobSnapshot> snapshot = service.wait_for(job_id, 5s);
  if (!snapshot.has_value()) {
    throw std::runtime_error("supervisor Job did not become terminal");
  }
  return *snapshot;
}

/**
 * @brief Polls a predicate within one short deterministic observer bound.
 * @param predicate Nonempty read-only predicate.
 * @param timeout Maximum observer duration.
 * @return True once predicate succeeds, otherwise false at timeout.
 * @throws Predicate exceptions unchanged.
 */
bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

/**
 * @brief Reads the PID encoded in one fixture artifact's four tight bytes.
 * @param service Live artifact authority.
 * @param snapshot Successful Job snapshot with receipt.
 * @return Encoded positive child PID.
 * @throws std::runtime_error for absent or malformed artifact truth.
 */
std::uint32_t artifact_pid(const SingleTenantJobService& service,
                           const JobSnapshot& snapshot) {
  if (!snapshot.output_receipt.has_value()) {
    throw std::runtime_error("successful fixture Job has no receipt");
  }
  const std::shared_ptr<const ArtifactRecord> artifact =
      service.find_artifact(snapshot.output_receipt->artifact_id);
  if (artifact == nullptr ||
      artifact->payload.size() != sizeof(std::uint32_t)) {
    throw std::runtime_error("fixture artifact has no encoded PID");
  }
  std::uint32_t pid = 0U;
  std::memcpy(&pid, artifact->payload.data(), sizeof(pid));
  return pid;
}

TEST(WorkerSupervisor, ConcurrentAttemptsUseFreshProcessesAndReapCompletely) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  const JobSubmission first =
      service->submit(fixture_spec("fixture.slow.success"));
  const JobSubmission second =
      service->submit(fixture_spec("fixture.slow.success"));

  ASSERT_TRUE(wait_until(
      [&] {
        return SingleTenantJobServiceTestAccess::live_worker_process_count(
                   *service) == 2U;
      },
      2s));
  const JobSnapshot first_terminal = wait_terminal(*service, first.job_id);
  const JobSnapshot second_terminal = wait_terminal(*service, second.job_id);
  ASSERT_EQ(first_terminal.state, JobState::Succeeded);
  ASSERT_EQ(second_terminal.state, JobState::Succeeded);
  EXPECT_NE(artifact_pid(*service, first_terminal),
            artifact_pid(*service, second_terminal));
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
}

TEST(WorkerSupervisor, CrashAndProtocolFaultsFailOnlyOwningAttempt) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  const JobSubmission unrelated =
      service->submit(fixture_spec("fixture.slow.success"));
  const std::vector<std::pair<std::string, JobAttemptFailure>> cases{
      {"fixture.preaccept.nonzero", JobAttemptFailure::WorkerExit},
      {"fixture.nonzero", JobAttemptFailure::WorkerExit},
      {"fixture.signal", JobAttemptFailure::WorkerSignal},
      {"fixture.channel", JobAttemptFailure::WorkerChannel},
      {"fixture.malformed", JobAttemptFailure::WorkerProtocol},
      {"fixture.stall", JobAttemptFailure::WorkerHeartbeatTimeout},
      {"fixture.report.hang", JobAttemptFailure::WorkerProtocol}};
  for (const auto& test_case : cases) {
    const JobSubmission submitted =
        service->submit(fixture_spec(test_case.first));
    const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
    EXPECT_EQ(terminal.state, JobState::Failed) << test_case.first;
    EXPECT_EQ(terminal.failure, test_case.second)
        << test_case.first << ": " << terminal.message;
    EXPECT_TRUE(terminal.attempt_settled) << test_case.first;
    EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed)
        << test_case.first;
  }
  EXPECT_EQ(wait_terminal(*service, unrelated.job_id).state,
            JobState::Succeeded);
}

TEST(WorkerSupervisor, ReassemblesReportAcrossMultiplePollSlices) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.fragmented.report"));

  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Succeeded) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Succeeded);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::None);
}

TEST(WorkerSupervisor, ReassemblesCancelAcrossMultiplePollSlices) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.cooperative_cancel_timeout = 300ms;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.fragmented.cancel"));
  ASSERT_TRUE(wait_until(
      [&] {
        const auto snapshot = service->query(submitted.job_id);
        return snapshot.has_value() && snapshot->state == JobState::Running;
      },
      2s));

  ASSERT_TRUE(service->cancel(submitted.job_id));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Cancelled) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Cancelled);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::CancellationObserved);
}

TEST(WorkerSupervisor, RuntimeTimeoutTerminatesHeartbeatingWorker) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.attempt_runtime_timeout = 160ms;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.runtime"));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
  EXPECT_EQ(terminal.state, JobState::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerRuntimeTimeout)
      << terminal.message;
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
}

TEST(WorkerSupervisor, ReapDeadlineFailStopsWithoutBlockingFallback) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.cooperative_cancel_timeout = 50ms;
  options.terminate_timeout = 50ms;
  options.kill_reap_timeout = 50ms;
  options.io_timeout = 50ms;
  options.defer_reap_observation_for_test =
      std::make_shared<std::atomic<bool>>(true);

  EXPECT_DEATH(
      {
        auto service = make_service(root.path(), options);
        const JobSubmission submitted =
            service->submit(fixture_spec("fixture.ignore"));
        static_cast<void>(submitted);
        if (!wait_until(
                [&] {
                  return SingleTenantJobServiceTestAccess::
                             live_worker_process_count(*service) == 1U;
                },
                2s)) {
          throw std::runtime_error("death-test worker did not become live");
        }
        service.reset();
      },
      "exact worker was not reaped before the SIGKILL deadline");
}

TEST(WorkerSupervisor, CooperativeAndForcedCancellationRemainDistinct) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  const JobSubmission cooperative =
      service->submit(fixture_spec("fixture.cooperative"));
  ASSERT_TRUE(wait_until(
      [&] {
        const auto snapshot = service->query(cooperative.job_id);
        return snapshot.has_value() && snapshot->state == JobState::Running;
      },
      2s));
  ASSERT_TRUE(service->cancel(cooperative.job_id));
  const JobSnapshot cooperative_terminal =
      wait_terminal(*service, cooperative.job_id);
  EXPECT_EQ(cooperative_terminal.state, JobState::Cancelled);
  EXPECT_EQ(cooperative_terminal.failure,
            JobAttemptFailure::CancellationObserved);

  const JobSubmission ignored = service->submit(fixture_spec("fixture.ignore"));
  ASSERT_TRUE(wait_until(
      [&] {
        const auto snapshot = service->query(ignored.job_id);
        return snapshot.has_value() && snapshot->state == JobState::Running;
      },
      2s));
  const auto started = std::chrono::steady_clock::now();
  ASSERT_TRUE(service->cancel(ignored.job_id));
  const JobSnapshot ignored_terminal = wait_terminal(*service, ignored.job_id);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_EQ(ignored_terminal.state, JobState::Cancelled);
  EXPECT_EQ(ignored_terminal.failure,
            JobAttemptFailure::WorkerCancellationForced);
  EXPECT_LT(elapsed, 2s);
}

TEST(WorkerSupervisor, CancelSendFailurePreservesWorkerFailureAndExit) {
  /**
   * @brief Maps one fixture cancel-channel fault to its exact terminal truth.
   * @throws Nothing for aggregate initialization and value operations.
   */
  struct CancelRaceCase final {
    /** @brief Fixture behavior selected after closing its cancel read side. */
    const char* mode;
    /** @brief Exact expected worker-owned or wait-status failure. */
    JobAttemptFailure failure;
  };
  const std::array<CancelRaceCase, 4U> cases{{
      {"fixture.cancel-race.failed-report", JobAttemptFailure::Compute},
      {"fixture.cancel-race.nonzero", JobAttemptFailure::WorkerExit},
      {"fixture.cancel-race.signal", JobAttemptFailure::WorkerSignal},
      {"fixture.cancel-race.channel-close", JobAttemptFailure::WorkerChannel},
  }};

  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  for (std::size_t repetition = 0U; repetition < 3U; ++repetition) {
    for (const CancelRaceCase& test_case : cases) {
      const JobSubmission submitted =
          service->submit(fixture_spec(test_case.mode));
      ASSERT_TRUE(service->cancel(submitted.job_id))
          << test_case.mode << " repetition " << repetition;
      const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
      EXPECT_EQ(terminal.state, JobState::Failed)
          << test_case.mode << " repetition " << repetition << ": "
          << terminal.message;
      EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed)
          << test_case.mode << " repetition " << repetition;
      EXPECT_EQ(terminal.failure, test_case.failure)
          << test_case.mode << " repetition " << repetition << ": "
          << terminal.message;
      EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerCancellationForced);
    }
  }
}

TEST(WorkerSupervisor, StaleLeaseCannotCancelFreshRetryProcess) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  const JobSubmission first = service->submit(fixture_spec("fixture.retry"));
  const JobSnapshot failed = wait_terminal(*service, first.job_id);
  ASSERT_EQ(failed.failure, JobAttemptFailure::WorkerExit);
  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  const TenantQuotaSnapshot released = service->quota_snapshot();
  EXPECT_EQ(released.active_attempts, 0U);

  const std::optional<JobSubmission> retry = service->retry(first.job_id);
  ASSERT_TRUE(retry.has_value());
  ASSERT_TRUE(wait_until(
      [&] {
        return SingleTenantJobServiceTestAccess::live_worker_process_count(
                   *service) == 1U;
      },
      2s));
  EXPECT_FALSE(SingleTenantJobServiceTestAccess::request_exact_worker_cancel(
      *service, first.assignment));
  const JobSnapshot succeeded = wait_terminal(*service, retry->job_id);
  EXPECT_EQ(succeeded.state, JobState::Succeeded);
  EXPECT_NE(retry->assignment.worker_instance_id,
            first.assignment.worker_instance_id);
  EXPECT_NE(retry->assignment.worker_lease_generation,
            first.assignment.worker_lease_generation);
  EXPECT_EQ(succeeded.output_artifact_id, failed.output_artifact_id);
  EXPECT_EQ(succeeded.output_commit_id, failed.output_commit_id);
}

TEST(WorkerSupervisor, RetryCheckpointAndRestartPreserveDurableAuthority) {
  ScopedSupervisorRoot root;
  JobId completed_job_id;
  ArtifactId checkpoint_id;
  {
    auto service = make_service(root.path(), supervisor_options());
    const JobSubmission first = service->submit(fixture_spec("fixture.retry"));
    completed_job_id = first.job_id;
    ASSERT_EQ(wait_terminal(*service, first.job_id).state, JobState::Failed);
    ASSERT_TRUE(
        SingleTenantJobServiceTestAccess::
            wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
    const std::optional<JobSubmission> retry = service->retry(first.job_id);
    ASSERT_TRUE(retry.has_value());
    const JobSnapshot succeeded = wait_terminal(*service, first.job_id);
    ASSERT_EQ(succeeded.state, JobState::Succeeded);
    ASSERT_TRUE(succeeded.output_receipt.has_value());
    checkpoint_id = succeeded.output_receipt->artifact_id;
  }
  {
    auto recovered = make_service(root.path(), supervisor_options());
    const std::optional<JobSnapshot> prior = recovered->query(completed_job_id);
    ASSERT_TRUE(prior.has_value());
    ASSERT_EQ(prior->state, JobState::Succeeded);
    ASSERT_TRUE(prior->output_receipt.has_value());
    EXPECT_EQ(prior->output_receipt->artifact_id, checkpoint_id);
    const JobSubmission checkpoint =
        recovered->submit(fixture_spec("fixture.checkpoint", checkpoint_id));
    const JobSnapshot terminal = wait_terminal(*recovered, checkpoint.job_id);
    EXPECT_EQ(terminal.state, JobState::Succeeded);
    EXPECT_EQ(recovered->quota_snapshot().active_attempts, 0U);
  }
}

TEST(WorkerSupervisor, ShutdownDrainsIgnoringWorkersWithinConcurrentBound) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  static_cast<void>(service->submit(fixture_spec("fixture.ignore")));
  static_cast<void>(service->submit(fixture_spec("fixture.ignore")));
  ASSERT_TRUE(wait_until(
      [&] {
        return SingleTenantJobServiceTestAccess::live_worker_process_count(
                   *service) == 2U;
      },
      2s));
  const auto started = std::chrono::steady_clock::now();
  service.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}

TEST(WorkerSupervisor, ProductConstructionRejectsUnmarkedOrMissingExecutable) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions valid = supervisor_options();
  const std::filesystem::path untouched_root =
      root.path() / "invalid-service-root";
  EXPECT_THROW(SingleTenantJobService(TenantId("tenant.supervisor"),
                                      supervisor_quota(), untouched_root,
                                      std::make_shared<UnmarkedWorkerFactory>(),
                                      DurableServerStateOptions{},
                                      TenantQuotaAuthorityOptions{}, valid),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(untouched_root));

  WorkerManagerOptions missing = supervisor_options();
  missing.worker_executable = root.path() / "missing-worker";
  EXPECT_THROW(SingleTenantJobService(TenantId("tenant.supervisor"),
                                      supervisor_quota(), root.path(),
                                      std::make_shared<FixtureWorkerFactory>(),
                                      DurableServerStateOptions{},
                                      TenantQuotaAuthorityOptions{}, missing),
               std::invalid_argument);
}

}  // namespace
}  // namespace ps::server
