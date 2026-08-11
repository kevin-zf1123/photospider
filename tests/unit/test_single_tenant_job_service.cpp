/**
 * @file test_single_tenant_job_service.cpp
 * @brief Verifies Issue #99 quota, durable artifacts, retry, and fencing.
 */
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)
#include "server/single_tenant_job_service_test_access.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_manager_test_access.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_protocol.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/**
 * @brief Mutable state for one interrupted-close descriptor-reuse probe.
 * @throws Nothing for value operations.
 */
struct InterruptedCloseReuseProbe final {
  /** @brief Still-open source duplicated onto the released descriptor value. */
  int replacement_source = -1;
  /** @brief Number of close callbacks observed by the production primitive. */
  std::size_t close_calls = 0U;
  /** @brief First unexpected raw close error, or zero. */
  int close_error = 0;
  /** @brief First unexpected descriptor-duplication error, or zero. */
  int duplicate_error = 0;
};

/**
 * @brief Simulates Linux releasing and reusing an fd before reporting EINTR.
 * @param descriptor Former owned numeric descriptor.
 * @param context Non-null `InterruptedCloseReuseProbe`.
 * @return `-1`/`EINTR` after first close and reuse; a second invocation closes
 * the reused descriptor and returns the raw close result.
 * @throws Nothing.
 */
int close_and_reuse_before_eintr(int descriptor, void* context) noexcept {
  auto* probe = static_cast<InterruptedCloseReuseProbe*>(context);
  ++probe->close_calls;
  if (probe->close_calls == 1U) {
    if (::close(descriptor) != 0) {
      probe->close_error = errno;
      return 0;
    }
    if (::dup2(probe->replacement_source, descriptor) != descriptor) {
      probe->duplicate_error = errno;
      return 0;
    }
    errno = EINTR;
    return -1;
  }
  return ::close(descriptor);
}

/**
 * @brief Closes one test-owned descriptor exactly once when valid.
 * @param descriptor Descriptor or negative sentinel.
 * @return Nothing.
 * @throws Nothing; cleanup errors are intentionally ignored.
 */
void close_test_descriptor_once(int descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
  }
}

/**
 * @brief Builds the complete default Job resource request used by unit tests.
 * @param cpu_slots Positive Embedded Host callback and server CPU bound.
 * @return Canonical request with one configured device declaration.
 * @throws std::bad_alloc when device storage allocation fails.
 */
JobResourceRequest test_job_resources(std::uint32_t cpu_slots = 2U) {
  JobResourceRequest request;
  request.cpu_slots = cpu_slots;
  request.host_memory_bytes = 1U << 20U;
  request.output_bytes = 1U << 20U;
  request.staging_bytes = 1U << 20U;
  request.retention_bytes = 1U << 20U;
  request.devices.push_back(DeviceResourceRequest{"gpu.test", 1U << 16U});
  return request;
}

/**
 * @brief Builds resources that permit one raw-frame-sized test image.
 * @return Valid request larger than the reusable-checkpoint payload cap.
 * @throws std::bad_alloc when configured-device storage allocation fails.
 * @note This helper lets the in-process test marker create a legacy-like
 * artifact that bypasses the external worker encoder without bypassing service
 * authorization.
 */
JobResourceRequest checkpoint_transport_resources() {
  JobResourceRequest request = test_job_resources();
  request.output_bytes = kMaximumWorkerFramePayloadBytes;
  request.staging_bytes = kMaximumWorkerFramePayloadBytes;
  request.retention_bytes = kMaximumWorkerFramePayloadBytes;
  return request;
}

/**
 * @brief Builds a strictly sorted configured-device vector of exact size.
 * @param count Number of semantic device rows to create, at most 999.
 * @param bytes Positive byte capacity/request for every row.
 * @return Canonical zero-padded `device.NNN` rows in ascending order.
 * @throws std::invalid_argument when `count` or `bytes` is invalid.
 * @throws std::bad_alloc when result construction exhausts memory.
 */
std::vector<DeviceResourceRequest> configured_devices(std::size_t count,
                                                      std::uint64_t bytes) {
  if (count > 999U || bytes == 0U) {
    throw std::invalid_argument("configured-device test input is invalid");
  }
  std::vector<DeviceResourceRequest> result;
  result.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    const std::string digits = std::to_string(index);
    result.push_back(DeviceResourceRequest{
        "device." + std::string(3U - digits.size(), '0') + digits, bytes});
  }
  return result;
}

/**
 * @brief Builds permissive but finite tenant capacity for service unit tests.
 * @return Capacity covering the maintained concurrency/reaping stress cases.
 * @throws std::bad_alloc when configured-device storage allocation fails.
 */
TenantQuotaLimits test_quota_limits() {
  TenantQuotaLimits limits;
  limits.maximum_active_attempts = 512U;
  limits.capacity.cpu_slots = 4096U;
  limits.capacity.host_memory_bytes = 1ULL << 40U;
  limits.capacity.output_bytes = 1ULL << 40U;
  limits.capacity.staging_bytes = 1ULL << 40U;
  limits.capacity.retention_bytes = 1ULL << 40U;
  limits.capacity.devices.push_back(
      DeviceResourceRequest{"gpu.test", 1ULL << 40U});
  return limits;
}

/**
 * @brief Verifies one complete-envelope quota rejection changes no usage.
 * @param limits Valid tenant capacity with one intentionally smaller bound.
 * @param request Complete valid request expected to exceed that bound.
 * @param expected Exact typed rejecting dimension.
 * @return Nothing after GoogleTest assertions are recorded.
 * @throws Unexpected quota construction/snapshot failures unchanged.
 */
void expect_atomic_quota_rejection(TenantQuotaLimits limits,
                                   const JobResourceRequest& request,
                                   TenantQuotaDimension expected) {
  TenantQuotaAuthority authority(TenantId("tenant.test"), std::move(limits));
  try {
    static_cast<void>(authority.reserve(JobId("job.test.reject"), request));
    ADD_FAILURE() << "quota request unexpectedly succeeded";
  } catch (const TenantQuotaExceeded& error) {
    EXPECT_EQ(error.dimension(), expected);
  }
  const TenantQuotaSnapshot usage = authority.snapshot();
  EXPECT_EQ(usage.active_attempts, 0U);
  EXPECT_EQ(usage.cpu_slots, 0U);
  EXPECT_EQ(usage.host_memory_bytes, 0U);
  EXPECT_EQ(usage.output_bytes, 0U);
  EXPECT_EQ(usage.staging_bytes, 0U);
  EXPECT_EQ(usage.retention_bytes, 0U);
  EXPECT_EQ(usage.retained_artifacts, 0U);
  for (const auto& device : usage.device_bytes) {
    EXPECT_EQ(device.second, 0U);
  }
}

/**
 * @brief Owns one unique test root removed only after its service/store closes.
 * @throws Filesystem/runtime failures when creation fails.
 */
class ScopedTestStateRoot final {
 public:
  /** @brief Creates one process-local unique existing state root. */
  ScopedTestStateRoot() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("photospider-issue99-unit-" + std::to_string(ticks) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    if (!std::filesystem::create_directory(root_)) {
      throw std::runtime_error("failed to create Issue #99 unit-test root");
    }
  }

  /** @brief Best-effort removes the exact owned root after dependents close. */
  ~ScopedTestStateRoot() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /** @brief Prevents duplicate test-root ownership. */
  ScopedTestStateRoot(const ScopedTestStateRoot&) = delete;
  /** @brief Prevents duplicate test-root assignment. */
  ScopedTestStateRoot& operator=(const ScopedTestStateRoot&) = delete;

  /**
   * @brief Returns the exact existing root path.
   * @return Borrowed path valid for this owner's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return root_; }

 private:
  /** @brief Exact recursively cleaned temporary root. */
  std::filesystem::path root_;
};

/**
 * @brief Best-effort cleanup owner for one additional exact test path.
 * @throws Nothing after construction; path moves may allocate beforehand.
 * @note This helper is used when a test deliberately renames a managed root,
 * leaving the original `ScopedTestStateRoot` responsible for the replacement
 * path and this owner responsible for the relocated directory.
 */
class ScopedExtraPathCleanup final {
 public:
  /**
   * @brief Retains one exact path for recursive scope-exit cleanup.
   * @param path Path that may or may not exist by destruction time.
   * @throws Nothing after argument construction.
   */
  explicit ScopedExtraPathCleanup(std::filesystem::path path) noexcept
      : path_(std::move(path)) {}

  /**
   * @brief Best-effort removes the exact retained path.
   * @throws Nothing; filesystem errors are captured locally.
   */
  ~ScopedExtraPathCleanup() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  /**
   * @brief Prevents duplicate ownership of one cleanup path.
   * @param other Owner whose cleanup obligation cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  ScopedExtraPathCleanup(const ScopedExtraPathCleanup& other) = delete;
  /**
   * @brief Prevents duplicate assignment of one cleanup path.
   * @param other Owner whose cleanup obligation cannot be copied.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedExtraPathCleanup& operator=(const ScopedExtraPathCleanup& other) =
      delete;

 private:
  /** @brief Exact additional path removed on scope exit. */
  std::filesystem::path path_;
};

/**
 * @brief Test convenience owner pairing a temporary root with the real service.
 * @throws Construction propagates root, quota, durable-state, or service
 * failures unchanged.
 * @note Field order ensures the real service closes before root cleanup.
 */
class TestJobService final {
 public:
  /**
   * @brief Creates one real service with finite default test quota.
   * @param tenant_id Exact configured tenant.
   * @param factory Non-null worker factory.
   * @param worker_options Optional source-private manager test configuration.
   * @throws As `SingleTenantJobService` construction.
   */
  TestJobService(TenantId tenant_id,
                 std::shared_ptr<JobAttemptWorkerFactory> factory,
                 WorkerManagerOptions worker_options = {})
      : service_(std::move(tenant_id), test_quota_limits(), root_.path(),
                 std::move(factory), DurableServerStateOptions{},
                 TenantQuotaAuthorityOptions{}, std::move(worker_options)) {}

  /** @brief Prevents duplicate service/root ownership. */
  TestJobService(const TestJobService&) = delete;
  /** @brief Prevents duplicate service/root assignment. */
  TestJobService& operator=(const TestJobService&) = delete;

  /**
   * @brief Forwards one immutable Job submission.
   * @param spec Complete immutable JobSpec value.
   * @return Accepted Job identity and first assignment.
   * @throws Product submission failures unchanged.
   */
  JobSubmission submit(JobSpec spec) {
    return service_.submit(std::move(spec));
  }
  /**
   * @brief Forwards one current Job query.
   * @param id Exact Job identity.
   * @return Current snapshot, or empty when unknown.
   * @throws Allocation failures unchanged.
   */
  std::optional<JobSnapshot> query(const JobId& id) const {
    return service_.query(id);
  }
  /**
   * @brief Forwards one bounded terminal wait.
   * @param id Exact Job identity.
   * @param timeout Maximum wait duration.
   * @return Terminal snapshot, or empty on timeout/unknown identity.
   * @throws Allocation and synchronization failures unchanged.
   */
  std::optional<JobSnapshot> wait_for(const JobId& id,
                                      std::chrono::milliseconds timeout) const {
    return service_.wait_for(id, timeout);
  }
  /**
   * @brief Forwards one monotonic cancellation request.
   * @param id Exact Job identity.
   * @return True only when cancellation was newly accepted.
   * @throws Product journal failures unchanged.
   */
  bool cancel(const JobId& id) { return service_.cancel(id); }
  /**
   * @brief Forwards one explicit failed-Job retry request.
   * @param id Exact durable Job identity.
   * @return Fresh assignment submission, or empty when not retryable.
   * @throws Product retry failures unchanged.
   */
  std::optional<JobSubmission> retry(const JobId& id) {
    return service_.retry(id);
  }
  /**
   * @brief Forwards durable artifact deletion and quota release.
   * @param id Exact artifact identity.
   * @return True when one artifact was removed.
   * @throws Product deletion failures unchanged.
   */
  bool delete_artifact(const ArtifactId& id) {
    return service_.delete_artifact(id);
  }
  /**
   * @brief Forwards durable artifact lookup.
   * @param id Exact artifact identity.
   * @return Immutable record, or null when absent.
   * @throws Product lookup failures unchanged.
   */
  std::shared_ptr<const ArtifactRecord> find_artifact(
      const ArtifactId& id) const {
    return service_.find_artifact(id);
  }
  /**
   * @brief Forwards one mutex-consistent tenant quota snapshot.
   * @return Complete current usage.
   * @throws Allocation/synchronization failures unchanged.
   */
  TenantQuotaSnapshot quota_snapshot() const {
    return service_.quota_snapshot();
  }
  /**
   * @brief Returns mutable real service access for test-only observers.
   * @return Borrowed service valid for this wrapper's lifetime.
   * @throws Nothing.
   */
  operator SingleTenantJobService&() noexcept { return service_; }
  /**
   * @brief Returns const real service access for test-only observers.
   * @return Borrowed service valid for this wrapper's lifetime.
   * @throws Nothing.
   */
  operator const SingleTenantJobService&() const noexcept { return service_; }

 private:
  /** @brief Temporary root outliving the real service. */
  ScopedTestStateRoot root_;
  /** @brief Exact product service under test. */
  SingleTenantJobService service_;
};

/**
 * @brief Builds one complete deterministic assignment identity for store tests.
 * @param ordinal Positive suffix used only to avoid accidental text equality.
 * @return Complete valid tuple with a real JobSpec digest.
 * @throws Validation or allocation failures unchanged.
 */
AttemptIdentity make_test_identity(std::uint64_t ordinal) {
  const JobSpec spec(GraphArtifactId("graph.test"), 7,
                     OutputSlotId("image.final"), test_job_resources());
  AttemptIdentity identity;
  identity.tenant_id = TenantId("tenant.test");
  identity.job_id = JobId("job.test." + std::to_string(ordinal));
  identity.job_spec_digest = spec.digest();
  identity.attempt_id = JobAttemptId("attempt.test." + std::to_string(ordinal));
  identity.worker_instance_id =
      WorkerInstanceId("worker.test." + std::to_string(ordinal));
  identity.worker_lease_generation = WorkerLeaseGeneration{ordinal + 1U};
  return identity;
}

/**
 * @brief Builds one valid queued durable Job record for journal tests.
 * @param ordinal Positive suffix for every stable identity.
 * @param resources Complete canonical Job resource envelope.
 * @return Identity-joined queued durable record.
 * @throws Validation or allocation failures unchanged.
 */
DurableJobRecord make_test_durable_job_record(
    std::uint64_t ordinal,
    JobResourceRequest resources = test_job_resources()) {
  auto spec = std::make_shared<const JobSpec>(GraphArtifactId("graph.test"), 7,
                                              OutputSlotId("image.final"),
                                              std::move(resources));
  AttemptIdentity identity = make_test_identity(ordinal);
  identity.job_spec_digest = spec->digest();
  DurableJobRecord record;
  record.tenant_id = identity.tenant_id;
  record.job_id = identity.job_id;
  record.spec = std::move(spec);
  record.assignment = identity;
  record.output_artifact_id =
      ArtifactId("artifact.test.job." + std::to_string(ordinal));
  record.output_commit_id =
      OutputCommitId("commit.test.job." + std::to_string(ordinal));
  record.state = JobState::Queued;
  return record;
}

/**
 * @brief Builds one stable server-owned durable artifact commit request.
 * @param ordinal Positive suffix for attempt/artifact/commit identities.
 * @return Complete request with test quota bounds.
 * @throws Identity, validation, or allocation failures unchanged.
 */
DurableArtifactCommitRequest make_test_commit_request(std::uint64_t ordinal) {
  return DurableArtifactCommitRequest{
      make_test_identity(ordinal), OutputSlotId("image.final"),
      ArtifactId("artifact.test." + std::to_string(ordinal)),
      OutputCommitId("commit.test." + std::to_string(ordinal)),
      test_job_resources()};
}

/**
 * @brief Builds a small valid CPU image with row padding and known active
 * bytes.
 * @return Two-by-two RGB uint8 image using aligned padded rows.
 * @throws Allocation and image-contract failures unchanged.
 */
ImageBuffer make_test_image() {
  ImageBuffer image = make_aligned_cpu_image_buffer(2, 2, 3, DataType::UINT8);
  auto* bytes = static_cast<std::byte*>(image.data.get());
  for (std::size_t index = 0U; index < image.step * 2U; ++index) {
    bytes[index] = std::byte{0xee};
  }
  for (std::size_t index = 0U; index < 6U; ++index) {
    bytes[index] = static_cast<std::byte>(index + 1U);
    bytes[image.step + index] = static_cast<std::byte>(index + 11U);
  }
  return image;
}

/**
 * @brief Single-use worker driven by a test-owned execution function.
 * @throws std::bad_alloc when copying function state exhausts memory.
 */
class FunctionWorker final : public JobAttemptWorker {
 public:
  /** @brief Exact worker execution callback signature. */
  using Function = std::function<JobAttemptReport(
      const JobAssignment&, const std::function<bool()>&)>;

  /**
   * @brief Retains one nonempty execution callback.
   * @param function Test-owned worker behavior.
   * @throws std::invalid_argument when callback is empty.
   */
  explicit FunctionWorker(Function function) : function_(std::move(function)) {
    if (!function_) {
      throw std::invalid_argument("test worker function is empty");
    }
  }

  /**
   * @brief Executes the retained test behavior exactly once.
   * @param assignment Exact service assignment.
   * @param cancellation_requested Monotonic cancellation observer.
   * @return Callback-produced attempt report.
   * @throws Callback failures unchanged.
   */
  JobAttemptReport execute(
      const JobAssignment& assignment,
      const std::function<bool()>& cancellation_requested) override {
    return function_(assignment, cancellation_requested);
  }

 private:
  /** @brief Retained exact test behavior. */
  Function function_;
};

/**
 * @brief Factory that gives every submitted Job one fresh FunctionWorker.
 * @throws Constructor rejects an empty callback; create may allocate.
 */
class FunctionWorkerFactory final
    : public InProcessJobAttemptWorkerFactoryForTest {
 public:
  /**
   * @brief Retains one callback copied into every fresh worker.
   * @param function Exact test behavior.
   * @throws std::invalid_argument when callback is empty.
   */
  explicit FunctionWorkerFactory(FunctionWorker::Function function)
      : function_(std::move(function)) {
    if (!function_) {
      throw std::invalid_argument("test worker factory function is empty");
    }
  }

  /**
   * @brief Creates one fresh callback worker.
   * @param assignment Valid assignment, unused until execute.
   * @return Non-null worker owner.
   * @throws std::bad_alloc when allocation exhausts memory.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) override {
    (void)assignment;
    return std::make_unique<FunctionWorker>(function_);
  }

 private:
  /** @brief Callback copied into every fresh worker. */
  FunctionWorker::Function function_;
};

/**
 * @brief Builds one successful settled worker report with a small image.
 * @param assignment Exact service assignment to echo.
 * @return Complete successful candidate facts.
 * @throws Image allocation failures unchanged.
 */
JobAttemptReport successful_report(const JobAssignment& assignment) {
  JobAttemptReport report;
  report.identity = assignment.identity;
  report.outcome = JobAttemptOutcome::Succeeded;
  report.settled = true;
  report.failure = JobAttemptFailure::None;
  report.image = make_test_image();
  return report;
}

/**
 * @brief Builds one retryable settled compute-failure report.
 * @param assignment Exact current assignment to echo.
 * @return Valid failed attempt facts without an image.
 * @throws Identity/message copies may allocate.
 */
JobAttemptReport settled_failed_report(const JobAssignment& assignment) {
  JobAttemptReport report;
  report.identity = assignment.identity;
  report.outcome = JobAttemptOutcome::Failed;
  report.settled = true;
  report.failure = JobAttemptFailure::Compute;
  report.message = "injected settled compute failure";
  return report;
}

/** @brief Closed malformed report cases exercised at the control boundary. */
enum class MalformedReportShape : std::uint8_t {
  /** @brief Successful outcome without settlement proof. */
  SucceededUnsettled,
  /** @brief Successful outcome with a failure category. */
  SucceededWithFailure,
  /** @brief Failed outcome without a failure category. */
  FailedWithoutFailure,
  /** @brief Failed outcome that improperly carries an image. */
  FailedWithImage,
  /** @brief Failed outcome using the cancellation-only category. */
  FailedWithCancellationFailure,
  /** @brief Failed outcome using the control-plane rejection category. */
  FailedWithReportRejected,
  /** @brief Failed outcome using the control-plane commit category. */
  FailedWithArtifactCommit,
  /** @brief Cancelled outcome without settlement proof. */
  CancelledUnsettled,
  /** @brief Cancelled outcome using a non-cancellation failure. */
  CancelledWithWorkerFailure,
  /** @brief Cancelled outcome that improperly carries an image. */
  CancelledWithImage,
  /** @brief Outcome containing an invalid underlying enum representation. */
  InvalidOutcome,
  /** @brief Failure containing an invalid underlying enum representation. */
  InvalidFailure,
};

/**
 * @brief Builds one identity-correct but semantically malformed worker report.
 * @param assignment Exact current assignment to preserve identity fencing.
 * @param shape Malformed outcome/settlement/failure/image combination.
 * @return Candidate report that the control plane must reject fail closed.
 * @throws std::invalid_argument for an invalid shape enum representation.
 * @throws std::bad_alloc when image or identity construction exhausts memory.
 */
JobAttemptReport malformed_report(const JobAssignment& assignment,
                                  MalformedReportShape shape) {
  JobAttemptReport report = successful_report(assignment);
  switch (shape) {
    case MalformedReportShape::SucceededUnsettled:
      report.settled = false;
      return report;
    case MalformedReportShape::SucceededWithFailure:
      report.failure = JobAttemptFailure::Compute;
      return report;
    case MalformedReportShape::FailedWithoutFailure:
      report.outcome = JobAttemptOutcome::Failed;
      report.image.reset();
      return report;
    case MalformedReportShape::FailedWithImage:
      report.outcome = JobAttemptOutcome::Failed;
      report.failure = JobAttemptFailure::Compute;
      return report;
    case MalformedReportShape::FailedWithCancellationFailure:
      report.outcome = JobAttemptOutcome::Failed;
      report.failure = JobAttemptFailure::CancellationObserved;
      report.image.reset();
      return report;
    case MalformedReportShape::FailedWithReportRejected:
      report.outcome = JobAttemptOutcome::Failed;
      report.failure = JobAttemptFailure::ReportRejected;
      report.image.reset();
      return report;
    case MalformedReportShape::FailedWithArtifactCommit:
      report.outcome = JobAttemptOutcome::Failed;
      report.failure = JobAttemptFailure::ArtifactCommit;
      report.image.reset();
      return report;
    case MalformedReportShape::CancelledUnsettled:
      report.outcome = JobAttemptOutcome::Cancelled;
      report.settled = false;
      report.failure = JobAttemptFailure::CancellationObserved;
      report.image.reset();
      return report;
    case MalformedReportShape::CancelledWithWorkerFailure:
      report.outcome = JobAttemptOutcome::Cancelled;
      report.failure = JobAttemptFailure::Compute;
      report.image.reset();
      return report;
    case MalformedReportShape::CancelledWithImage:
      report.outcome = JobAttemptOutcome::Cancelled;
      report.failure = JobAttemptFailure::CancellationObserved;
      return report;
    case MalformedReportShape::InvalidOutcome:
      report.outcome = static_cast<JobAttemptOutcome>(0xffU);
      return report;
    case MalformedReportShape::InvalidFailure:
      report.failure = static_cast<JobAttemptFailure>(0xffU);
      return report;
  }
  throw std::invalid_argument("malformed report shape is invalid");
}

/**
 * @brief Thread-safe one-shot Job-journal fault controller for service tests.
 * @throws Nothing for construction; `observe` throws only when armed.
 */
class OneShotJobCommitFailure final {
 public:
  /**
   * @brief Selects the exact journal stage that may fail once.
   * @param stage Immutable target transition.
   * @throws Nothing.
   */
  explicit OneShotJobCommitFailure(DurableJobCommitStage stage) noexcept
      : stage_(stage) {}

  /**
   * @brief Arms the next observation of the selected stage.
   * @return Nothing.
   * @throws Nothing.
   */
  void arm() noexcept { armed_.store(true, std::memory_order_release); }

  /**
   * @brief Observes a real journal stage and consumes an armed fault.
   * @param stage Current product transition.
   * @return Nothing when disarmed or at another stage.
   * @throws std::runtime_error exactly once after each `arm()`.
   */
  void observe(DurableJobCommitStage stage) {
    if (stage == stage_ && armed_.exchange(false, std::memory_order_acq_rel)) {
      throw std::runtime_error("injected one-shot Job journal failure");
    }
  }

 private:
  /** @brief Immutable exact transition selected for injection. */
  DurableJobCommitStage stage_;
  /** @brief Whether the next selected transition must throw. */
  std::atomic<bool> armed_{false};
};

/**
 * @brief Builds durable-state options wired to one test fault controller.
 * @param failure Non-null controller retained by the observer.
 * @return Options whose Job observer delegates to `failure`.
 * @throws std::invalid_argument when `failure` is null.
 * @throws std::bad_alloc when callback storage allocation fails.
 */
DurableServerStateOptions job_failure_options(
    std::shared_ptr<OneShotJobCommitFailure> failure) {
  if (failure == nullptr) {
    throw std::invalid_argument("Job journal failure controller is null");
  }
  DurableServerStateOptions options;
  options.job_commit_observer =
      [failure = std::move(failure)](DurableJobCommitStage stage) {
        failure->observe(stage);
      };
  return options;
}

/**
 * @brief Thread-safe one-shot active-attempt release fault controller.
 * @throws Nothing for construction; `observe` throws only when armed.
 */
class OneShotQuotaReleaseFailure final {
 public:
  /**
   * @brief Arms the next active-attempt release observation.
   * @return Nothing.
   * @throws Nothing.
   */
  void arm() noexcept { armed_.store(true, std::memory_order_release); }

  /**
   * @brief Consumes an armed release fault before quota mutation.
   * @return Nothing when disarmed.
   * @throws std::runtime_error exactly once after each `arm()`.
   */
  void observe() {
    if (armed_.exchange(false, std::memory_order_acq_rel)) {
      throw std::runtime_error("injected active-attempt release failure");
    }
  }

 private:
  /** @brief Whether the next active-attempt release must throw. */
  std::atomic<bool> armed_{false};
};

/**
 * @brief Builds quota options wired to one active-release fault controller.
 * @param failure Non-null controller retained by the observer.
 * @return Options whose release observer delegates to `failure`.
 * @throws std::invalid_argument when `failure` is null.
 * @throws std::bad_alloc when callback storage allocation fails.
 */
TenantQuotaAuthorityOptions quota_release_failure_options(
    std::shared_ptr<OneShotQuotaReleaseFailure> failure) {
  if (failure == nullptr) {
    throw std::invalid_argument("quota release failure controller is null");
  }
  TenantQuotaAuthorityOptions options;
  options.release_attempt_observer = [failure = std::move(failure)] {
    failure->observe();
  };
  return options;
}

/**
 * @brief Verifies quota-release fail-stop ownership, reads, and mutation gate.
 * @param service Live service after one injected active-release failure.
 * @param query_job_id Valid Job identity used for read and mutation probes.
 * @param job_expected Whether `query_job_id` must remain durably observable.
 * @param expected_job_owners Number of reservation owners on Job controls.
 * @param expected_stranded_owners Number of rollback-only stranded owners.
 * @return Nothing after GoogleTest assertions are recorded.
 * @throws Unexpected query, synchronization, or snapshot failures unchanged.
 * @note Every mutation probe must fail at the common durable-mutation gate;
 * the helper never retries release or exposes a reservation identity.
 */
void expect_quota_release_fail_stop(SingleTenantJobService& service,
                                    const JobId& query_job_id,
                                    bool job_expected,
                                    std::size_t expected_job_owners,
                                    std::size_t expected_stranded_owners) {
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::quota_release_faulted(service));
  EXPECT_TRUE(
      SingleTenantJobServiceTestAccess::durable_mutation_faulted(service));
  EXPECT_FALSE(SingleTenantJobServiceTestAccess::journal_faulted(service));
  EXPECT_FALSE(
      SingleTenantJobServiceTestAccess::artifact_erase_faulted(service));
  EXPECT_FALSE(
      SingleTenantJobServiceTestAccess::artifact_reconciliation_faulted(
          service));

  const TenantQuotaSnapshot usage = service.quota_snapshot();
  const QuotaReservationOwnershipSnapshot ownership =
      SingleTenantJobServiceTestAccess::quota_reservation_ownership(service);
  EXPECT_EQ(ownership.job_controls, expected_job_owners);
  EXPECT_EQ(ownership.stranded, expected_stranded_owners);
  EXPECT_EQ(ownership.total(), usage.active_attempts);

  const std::optional<JobSnapshot> readable = service.query(query_job_id);
  EXPECT_EQ(readable.has_value(), job_expected);
  if (job_expected) {
    EXPECT_TRUE(service.wait_for(query_job_id, std::chrono::milliseconds(0))
                    .has_value());
  }
  EXPECT_EQ(service.find_artifact(ArtifactId("artifact.test.read-only")),
            nullptr);

  EXPECT_THROW(service.submit(JobSpec(GraphArtifactId("graph.test.fenced"), 8,
                                      OutputSlotId("image.final"),
                                      test_job_resources())),
               DurableStateError);
  EXPECT_THROW(static_cast<void>(service.retry(query_job_id)),
               DurableStateError);
  EXPECT_THROW(static_cast<void>(service.cancel(query_job_id)),
               DurableStateError);
  EXPECT_THROW(static_cast<void>(service.delete_artifact(
                   ArtifactId("artifact.test.read-only"))),
               DurableStateError);
}

/**
 * @brief Thread-safe one-shot artifact-deletion fault controller.
 * @throws Nothing for construction; `observe` throws only when armed.
 */
class OneShotArtifactEraseFailure final {
 public:
  /**
   * @brief Selects the exact deletion stage that may fail once.
   * @param stage Immutable target transition.
   * @throws Nothing.
   */
  explicit OneShotArtifactEraseFailure(DurableArtifactEraseStage stage) noexcept
      : stage_(stage) {}

  /**
   * @brief Arms the next observation of the selected stage.
   * @return Nothing.
   * @throws Nothing.
   */
  void arm() noexcept { armed_.store(true, std::memory_order_release); }

  /**
   * @brief Observes a deletion transition and consumes an armed fault.
   * @param stage Current product transition.
   * @return Nothing when disarmed or at another stage.
   * @throws std::runtime_error exactly once after each `arm()`.
   */
  void observe(DurableArtifactEraseStage stage) {
    if (stage == stage_ && armed_.exchange(false, std::memory_order_acq_rel)) {
      throw std::runtime_error("injected one-shot artifact erase failure");
    }
  }

 private:
  /** @brief Immutable exact deletion transition selected for injection. */
  DurableArtifactEraseStage stage_;
  /** @brief Whether the next selected transition must throw. */
  std::atomic<bool> armed_{false};
};

/**
 * @brief Builds durable-state options wired to one erase fault controller.
 * @param failure Non-null controller retained by the observer.
 * @return Options whose artifact erase observer delegates to `failure`.
 * @throws std::invalid_argument when `failure` is null.
 * @throws std::bad_alloc when callback storage allocation fails.
 */
DurableServerStateOptions artifact_erase_failure_options(
    std::shared_ptr<OneShotArtifactEraseFailure> failure) {
  if (failure == nullptr) {
    throw std::invalid_argument("artifact erase failure controller is null");
  }
  DurableServerStateOptions options;
  options.artifact_erase_observer =
      [failure = std::move(failure)](DurableArtifactEraseStage stage) {
        failure->observe(stage);
      };
  return options;
}

/**
 * @brief Coordinates one deliberately blocked worker with its test thread.
 * @throws Synchronization failures from standard primitives.
 */
class WorkerGate final {
 public:
  /**
   * @brief Marks worker entry and waits until the test releases it.
   * @return Nothing after release.
   * @throws std::system_error on synchronization failure.
   */
  void enter_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
  }

  /**
   * @brief Waits for worker entry with a fixed bounded timeout.
   * @return True when worker entry was observed.
   * @throws std::system_error on synchronization failure.
   */
  bool wait_until_entered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [&] { return entered_; });
  }

  /**
   * @brief Releases the waiting worker monotonically.
   * @return Nothing.
   * @throws std::system_error on synchronization failure.
   */
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  /** @brief Serializes gate state. */
  std::mutex mutex_;
  /** @brief Signals entry and release transitions. */
  std::condition_variable condition_;
  /** @brief Whether the worker reached the gate. */
  bool entered_ = false;
  /** @brief Whether the test released the worker. */
  bool released_ = false;
};

/**
 * @brief Coordinates a counted group of concurrently blocked test workers.
 * @throws Synchronization failures from standard primitives.
 */
class WorkerGroupGate final {
 public:
  /**
   * @brief Records one worker entry and waits for the shared release.
   * @return Nothing after release.
   * @throws std::system_error on synchronization failure.
   */
  void enter_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++entered_;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
  }

  /**
   * @brief Waits for at least the requested number of worker entries.
   * @param expected Minimum counted entries required for success.
   * @return True when the count is reached within the fixed test bound.
   * @throws std::system_error on synchronization failure.
   */
  bool wait_until_entered(std::size_t expected) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [&] { return entered_ >= expected; });
  }

  /**
   * @brief Releases every current or future waiter monotonically.
   * @return Nothing.
   * @throws std::system_error on synchronization failure.
   */
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  /** @brief Serializes counted gate state. */
  std::mutex mutex_;
  /** @brief Signals entry-count and release transitions. */
  std::condition_variable condition_;
  /** @brief Number of workers that have entered the gate. */
  std::size_t entered_ = 0U;
  /** @brief Whether all current and future workers may leave. */
  bool released_ = false;
};

/**
 * @brief Releases one worker gate before later-declared service destruction.
 *
 * Tests construct this guard after `SingleTenantJobService`, so fatal Google
 * Test assertions and exception unwinding release the blocked worker before
 * the service destructor joins it.
 *
 * @throws Nothing; synchronization failure during cleanup terminates because
 * allowing service destruction to join a permanently blocked worker cannot
 * recover test-process progress.
 */
class WorkerGateReleaseGuard final {
 public:
  /**
   * @brief Retains one gate for monotonic scope-exit release.
   * @param gate Gate shared with the blocked worker; null creates a disarmed
   * guard.
   * @throws Nothing.
   */
  explicit WorkerGateReleaseGuard(std::shared_ptr<WorkerGate> gate) noexcept
      : gate_(std::move(gate)) {}

  /**
   * @brief Releases an armed gate before the service join can run.
   * @throws Nothing; terminates on an unexpected synchronization failure.
   */
  ~WorkerGateReleaseGuard() noexcept { release(); }

  /** @brief Prevents duplicate ownership of one cleanup obligation. */
  WorkerGateReleaseGuard(const WorkerGateReleaseGuard&) = delete;
  /** @brief Prevents duplicate assignment of one cleanup obligation. */
  WorkerGateReleaseGuard& operator=(const WorkerGateReleaseGuard&) = delete;

  /**
   * @brief Releases the gate immediately and disarms scope-exit cleanup.
   * @return Nothing.
   * @throws Nothing; terminates on an unexpected synchronization failure.
   */
  void release() noexcept {
    if (gate_ == nullptr) {
      return;
    }
    try {
      gate_->release();
      gate_.reset();
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /** @brief Armed gate owner, or null after successful explicit release. */
  std::shared_ptr<WorkerGate> gate_;
};

/**
 * @brief Releases one counted worker gate before service destruction.
 * @throws Nothing; cleanup synchronization failure terminates the test process.
 */
class WorkerGroupGateReleaseGuard final {
 public:
  /**
   * @brief Retains one counted gate for monotonic scope-exit release.
   * @param gate Gate shared with blocked workers; null disarms the guard.
   * @throws Nothing.
   */
  explicit WorkerGroupGateReleaseGuard(
      std::shared_ptr<WorkerGroupGate> gate) noexcept
      : gate_(std::move(gate)) {}

  /**
   * @brief Releases an armed gate before a service can join its workers.
   * @throws Nothing; delegates to the guarded no-throw cleanup path.
   */
  ~WorkerGroupGateReleaseGuard() noexcept { release(); }

  /** @brief Prevents duplicate ownership of one cleanup obligation. */
  WorkerGroupGateReleaseGuard(const WorkerGroupGateReleaseGuard&) = delete;
  /** @brief Prevents duplicate assignment of one cleanup obligation. */
  WorkerGroupGateReleaseGuard& operator=(const WorkerGroupGateReleaseGuard&) =
      delete;

  /**
   * @brief Releases the gate and disarms scope-exit cleanup.
   * @return Nothing.
   * @throws Nothing; terminates on an unexpected synchronization failure.
   */
  void release() noexcept {
    if (gate_ == nullptr) {
      return;
    }
    try {
      gate_->release();
      gate_.reset();
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /** @brief Armed counted gate owner, or null after release. */
  std::shared_ptr<WorkerGroupGate> gate_;
};

/** @brief Stage at which a test double raises its configured exception. */
enum class ExceptionStage : std::uint8_t {
  /** @brief Raise while the service asks the factory for a fresh worker. */
  Factory,
  /** @brief Raise after the fresh worker enters `execute`. */
  Worker,
};

/** @brief Exception family raised by one test double. */
enum class ExceptionKind : std::uint8_t {
  /** @brief Raise a standard runtime error. */
  Standard,
  /** @brief Raise a non-standard integer value. */
  NonStandard,
};

/**
 * @brief Raises the configured standard or non-standard test exception.
 * @param kind Exact exception family to raise.
 * @return Never returns.
 * @throws std::runtime_error for `Standard`; otherwise throws integer `7`.
 */
[[noreturn]] void throw_test_worker_exception(ExceptionKind kind) {
  switch (kind) {
    case ExceptionKind::Standard:
      throw std::runtime_error("test worker raised a standard exception");
    case ExceptionKind::NonStandard:
      throw 7;
  }
  throw std::runtime_error("test exception kind is invalid");
}

/**
 * @brief Worker that optionally waits at a gate and then raises an exception.
 * @throws Construction only copies shared ownership; execute always throws.
 */
class ThrowingWorker final : public JobAttemptWorker {
 public:
  /**
   * @brief Configures one single-use exceptional worker.
   * @param kind Exact exception family raised by execute.
   * @param gate Optional synchronization gate entered before the exception.
   * @throws Nothing.
   */
  ThrowingWorker(ExceptionKind kind, std::shared_ptr<WorkerGate> gate) noexcept
      : kind_(kind), gate_(std::move(gate)) {}

  /**
   * @brief Waits when configured, then raises without settlement evidence.
   * @param assignment Exact assignment, unused by this exceptional double.
   * @param cancellation_requested Observer unused by this exceptional double.
   * @return Never returns.
   * @throws As `throw_test_worker_exception`.
   */
  JobAttemptReport execute(
      const JobAssignment& assignment,
      const std::function<bool()>& cancellation_requested) override {
    (void)assignment;
    (void)cancellation_requested;
    if (gate_ != nullptr) {
      gate_->enter_and_wait();
    }
    throw_test_worker_exception(kind_);
  }

 private:
  /** @brief Exact exception family raised by execute. */
  ExceptionKind kind_;
  /** @brief Optional test synchronization before raising. */
  std::shared_ptr<WorkerGate> gate_;
};

/**
 * @brief Factory that raises directly or returns one exceptional worker.
 * @throws Construction only retains values; create raises when configured.
 */
class ThrowingWorkerFactory final
    : public InProcessJobAttemptWorkerFactoryForTest {
 public:
  /**
   * @brief Configures the exact exception location, family, and optional gate.
   * @param stage Whether factory creation or worker execution raises.
   * @param kind Exact exception family to raise.
   * @param gate Optional worker-execution synchronization gate.
   * @throws Nothing.
   */
  ThrowingWorkerFactory(ExceptionStage stage, ExceptionKind kind,
                        std::shared_ptr<WorkerGate> gate = nullptr) noexcept
      : stage_(stage), kind_(kind), gate_(std::move(gate)) {}

  /**
   * @brief Raises at the factory stage or creates one exceptional worker.
   * @param assignment Exact assignment, unused by this exceptional double.
   * @return Fresh worker only when configured for worker-stage failure.
   * @throws As `throw_test_worker_exception` at the factory stage.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) override {
    (void)assignment;
    if (stage_ == ExceptionStage::Factory) {
      throw_test_worker_exception(kind_);
    }
    return std::make_unique<ThrowingWorker>(kind_, gate_);
  }

 private:
  /** @brief Exact stage at which the exception is raised. */
  ExceptionStage stage_;
  /** @brief Exact exception family to raise. */
  ExceptionKind kind_;
  /** @brief Optional worker-execution synchronization gate. */
  std::shared_ptr<WorkerGate> gate_;
};

/**
 * @brief Contract-violating factory that returns no worker and no proof.
 * @throws Nothing.
 */
class NullWorkerFactory final : public InProcessJobAttemptWorkerFactoryForTest {
 public:
  /**
   * @brief Returns null instead of one fresh assignment worker.
   * @param assignment Exact assignment, unused by this failing double.
   * @return Null worker owner.
   * @throws Nothing.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) noexcept override {
    (void)assignment;
    return nullptr;
  }
};

TEST(SingleTenantJobContract, CanonicalBytesAndDigestAreStable) {
  const JobSpec first(GraphArtifactId("graph.test"), 7,
                      OutputSlotId("image.final"), test_job_resources());
  const JobSpec second(GraphArtifactId("graph.test"), 7,
                       OutputSlotId("image.final"), test_job_resources());
  EXPECT_EQ(first.canonical_bytes(), second.canonical_bytes());
  EXPECT_EQ(first.digest(), second.digest());
  EXPECT_EQ(first.canonical_bytes(),
            "jobspec-v210:graph.test1:711:image.final15:embedded-cpu-v1"
            "13:crash-durable1:27:10485767:10485767:10485767:1048576"
            "1:18:gpu.test5:655361:00:");
  EXPECT_EQ(first.digest().hex(),
            "620d56721b06b7b7c9a2c910c7a9a9f9042e8430d7f0e123ca1f4adf4b7b71de");

  JobResourceRequest changed_device = test_job_resources();
  ++changed_device.devices.front().bytes;
  const JobSpec device_variant(GraphArtifactId("graph.test"), 7,
                               OutputSlotId("image.final"), changed_device);
  EXPECT_NE(device_variant.canonical_bytes(), first.canonical_bytes());
  EXPECT_NE(device_variant.digest(), first.digest());

  const JobSpec checkpoint_variant(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
      test_job_resources(), ArtifactId("artifact.test.checkpoint"));
  EXPECT_NE(checkpoint_variant.canonical_bytes(), first.canonical_bytes());
  EXPECT_NE(checkpoint_variant.digest(), first.digest());

  constexpr char kKnownInput[] = "abc";
  const JobSpecDigest known =
      hash_job_spec_bytes(reinterpret_cast<const std::byte*>(kKnownInput), 3U);
  EXPECT_EQ(known.hex(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_THROW(GraphArtifactId("/tmp/graph.yaml"), std::invalid_argument);
  EXPECT_THROW(ArtifactId("."), std::invalid_argument);
  EXPECT_THROW(JobId(".."), std::invalid_argument);
  EXPECT_THROW(JobSpec(GraphArtifactId("graph.test"), -1,
                       OutputSlotId("image.final"), test_job_resources()),
               std::invalid_argument);
  JobResourceRequest invalid_resources = test_job_resources();
  invalid_resources.cpu_slots = 0U;
  EXPECT_THROW(JobSpec(GraphArtifactId("graph.test"), 7,
                       OutputSlotId("image.final"), invalid_resources),
               std::invalid_argument);

  JobResourceRequest unsorted_devices = test_job_resources();
  unsorted_devices.devices.push_back(
      DeviceResourceRequest{"accelerator.test", 1U});
  EXPECT_THROW(JobSpec(GraphArtifactId("graph.test"), 7,
                       OutputSlotId("image.final"), unsorted_devices),
               std::invalid_argument);

  JobResourceRequest duplicate_devices = test_job_resources();
  duplicate_devices.devices.push_back(DeviceResourceRequest{"gpu.test", 1U});
  EXPECT_THROW(JobSpec(GraphArtifactId("graph.test"), 7,
                       OutputSlotId("image.final"), duplicate_devices),
               std::invalid_argument);
}

TEST(TenantQuotaAuthority,
     RejectsEveryCompleteEnvelopeDimensionWithoutPartialUsage) {
  const JobResourceRequest request = test_job_resources();

  TenantQuotaLimits concurrency_limits = test_quota_limits();
  concurrency_limits.maximum_active_attempts = 1U;
  TenantQuotaAuthority concurrency(TenantId("tenant.test"),
                                   std::move(concurrency_limits));
  const TenantQuotaReservation active =
      concurrency.reserve(JobId("job.test.active"), request);
  try {
    static_cast<void>(
        concurrency.reserve(JobId("job.test.concurrent"), request));
    ADD_FAILURE() << "concurrent quota request unexpectedly succeeded";
  } catch (const TenantQuotaExceeded& error) {
    EXPECT_EQ(error.dimension(), TenantQuotaDimension::Concurrency);
  }
  const TenantQuotaSnapshot concurrent_usage = concurrency.snapshot();
  EXPECT_EQ(concurrent_usage.active_attempts, 1U);
  EXPECT_EQ(concurrent_usage.cpu_slots, request.cpu_slots);
  concurrency.release_attempt(active.id);

  TenantQuotaLimits cpu = test_quota_limits();
  cpu.capacity.cpu_slots = request.cpu_slots - 1U;
  expect_atomic_quota_rejection(std::move(cpu), request,
                                TenantQuotaDimension::Cpu);

  TenantQuotaLimits host = test_quota_limits();
  host.capacity.host_memory_bytes = request.host_memory_bytes - 1U;
  expect_atomic_quota_rejection(std::move(host), request,
                                TenantQuotaDimension::HostMemory);

  TenantQuotaLimits device = test_quota_limits();
  device.capacity.devices.front().bytes = request.devices.front().bytes - 1U;
  expect_atomic_quota_rejection(std::move(device), request,
                                TenantQuotaDimension::Device);

  JobResourceRequest unknown_device = request;
  unknown_device.devices.push_back(DeviceResourceRequest{"gpu.unknown", 1U});
  expect_atomic_quota_rejection(test_quota_limits(), unknown_device,
                                TenantQuotaDimension::Device);

  TenantQuotaLimits output = test_quota_limits();
  output.capacity.output_bytes = request.output_bytes - 1U;
  expect_atomic_quota_rejection(std::move(output), request,
                                TenantQuotaDimension::Output);

  TenantQuotaLimits staging = test_quota_limits();
  staging.capacity.staging_bytes = request.staging_bytes - 1U;
  expect_atomic_quota_rejection(std::move(staging), request,
                                TenantQuotaDimension::Staging);

  TenantQuotaLimits retention = test_quota_limits();
  retention.capacity.retention_bytes = request.retention_bytes - 1U;
  expect_atomic_quota_rejection(std::move(retention), request,
                                TenantQuotaDimension::Retention);
}

TEST(TenantQuotaAuthority,
     SettlesAttemptsAndRetainedArtifactsExactlyOnceAcrossRecovery) {
  TenantQuotaAuthority authority(TenantId("tenant.test"), test_quota_limits());
  const JobResourceRequest request = test_job_resources();
  const TenantQuotaReservation failed =
      authority.reserve(JobId("job.test.failed"), request);
  TenantQuotaSnapshot usage = authority.snapshot();
  EXPECT_EQ(usage.active_attempts, 1U);
  EXPECT_EQ(usage.cpu_slots, request.cpu_slots);
  EXPECT_EQ(usage.host_memory_bytes, request.host_memory_bytes);
  EXPECT_EQ(usage.device_bytes.at("gpu.test"), request.devices.front().bytes);
  EXPECT_EQ(usage.output_bytes, request.output_bytes);
  EXPECT_EQ(usage.staging_bytes, request.staging_bytes);
  EXPECT_EQ(usage.retention_bytes, request.retention_bytes);

  authority.release_attempt(failed.id);
  EXPECT_THROW(authority.release_attempt(failed.id), std::logic_error);
  usage = authority.snapshot();
  EXPECT_EQ(usage.active_attempts, 0U);
  EXPECT_EQ(usage.retention_bytes, 0U);

  const TenantQuotaReservation succeeded =
      authority.reserve(JobId("job.test.succeeded"), request);
  const ArtifactId artifact_id("artifact.test.retained");
  authority.commit_retained_artifact(succeeded.id, artifact_id, 12U);
  usage = authority.snapshot();
  EXPECT_EQ(usage.active_attempts, 0U);
  EXPECT_EQ(usage.cpu_slots, 0U);
  EXPECT_EQ(usage.host_memory_bytes, 0U);
  EXPECT_EQ(usage.device_bytes.at("gpu.test"), 0U);
  EXPECT_EQ(usage.output_bytes, 0U);
  EXPECT_EQ(usage.staging_bytes, 0U);
  EXPECT_EQ(usage.retention_bytes, 12U);
  EXPECT_EQ(usage.retained_artifacts, 1U);
  EXPECT_THROW(
      authority.commit_retained_artifact(succeeded.id, artifact_id, 12U),
      std::logic_error);

  EXPECT_EQ(authority.release_retained_artifact(artifact_id), 12U);
  EXPECT_EQ(authority.release_retained_artifact(artifact_id), 0U);
  EXPECT_EQ(authority.snapshot().retention_bytes, 0U);

  TenantQuotaLimits recovery_limits = test_quota_limits();
  recovery_limits.capacity.retention_bytes =
      std::numeric_limits<std::uint64_t>::max();
  TenantQuotaAuthority recovered(TenantId("tenant.test"),
                                 std::move(recovery_limits));
  recovered.recover_retained_artifact(artifact_id, 1U);
  recovered.recover_retained_artifact(artifact_id, 1U);
  usage = recovered.snapshot();
  EXPECT_EQ(usage.retention_bytes, 1U);
  EXPECT_EQ(usage.retained_artifacts, 1U);

  JobResourceRequest maximum_request = test_job_resources();
  maximum_request.retention_bytes = std::numeric_limits<std::uint64_t>::max();
  try {
    static_cast<void>(
        recovered.reserve(JobId("job.test.overflow"), maximum_request));
    ADD_FAILURE() << "overflowing retained reservation unexpectedly succeeded";
  } catch (const TenantQuotaExceeded& error) {
    EXPECT_EQ(error.dimension(), TenantQuotaDimension::Retention);
  }
  usage = recovered.snapshot();
  EXPECT_EQ(usage.active_attempts, 0U);
  EXPECT_EQ(usage.retention_bytes, 1U);
  EXPECT_EQ(usage.retained_artifacts, 1U);

  EXPECT_THROW(recovered.recover_retained_artifact(artifact_id, 2U),
               std::logic_error);
  usage = recovered.snapshot();
  EXPECT_EQ(usage.retention_bytes, 1U);
  EXPECT_EQ(usage.retained_artifacts, 1U);

  TenantQuotaLimits insufficient_recovery_limits = test_quota_limits();
  insufficient_recovery_limits.capacity.retention_bytes = 1U;
  TenantQuotaAuthority insufficient_recovery(
      TenantId("tenant.test"), std::move(insufficient_recovery_limits));
  try {
    insufficient_recovery.recover_retained_artifact(
        ArtifactId("artifact.test.too-large"), 2U);
    ADD_FAILURE() << "oversized recovered artifact unexpectedly succeeded";
  } catch (const TenantQuotaExceeded& error) {
    EXPECT_EQ(error.dimension(), TenantQuotaDimension::Retention);
  }
  EXPECT_EQ(insufficient_recovery.snapshot().retention_bytes, 0U);
  EXPECT_EQ(insufficient_recovery.snapshot().retained_artifacts, 0U);
}

TEST(TenantQuotaAuthority,
     ReleaseFaultSeamPreservesReservationBeforeExactOnceSettlement) {
  const auto release_failure = std::make_shared<OneShotQuotaReleaseFailure>();
  TenantQuotaAuthority authority(
      TenantId("tenant.test"), test_quota_limits(),
      quota_release_failure_options(release_failure));
  const JobResourceRequest request = test_job_resources();
  const TenantQuotaReservation reservation =
      authority.reserve(JobId("job.test.release-fault"), request);
  const TenantQuotaSnapshot before = authority.snapshot();

  release_failure->arm();
  EXPECT_THROW(authority.release_attempt(reservation.id), std::runtime_error);
  const TenantQuotaSnapshot retained = authority.snapshot();
  EXPECT_EQ(retained.active_attempts, before.active_attempts);
  EXPECT_EQ(retained.cpu_slots, before.cpu_slots);
  EXPECT_EQ(retained.host_memory_bytes, before.host_memory_bytes);
  EXPECT_EQ(retained.output_bytes, before.output_bytes);
  EXPECT_EQ(retained.staging_bytes, before.staging_bytes);
  EXPECT_EQ(retained.retention_bytes, before.retention_bytes);
  EXPECT_EQ(retained.device_bytes, before.device_bytes);

  authority.release_attempt(reservation.id);
  EXPECT_EQ(authority.snapshot().active_attempts, 0U);
  EXPECT_THROW(authority.release_attempt(reservation.id), std::logic_error);
  EXPECT_EQ(authority.snapshot().active_attempts, 0U);
}

TEST(TenantQuotaAuthority, AccountsMultipleConfiguredDevicesExactly) {
  TenantQuotaLimits limits = test_quota_limits();
  limits.capacity.devices.push_back(
      DeviceResourceRequest{"gpu.test.2", 1ULL << 40U});
  TenantQuotaAuthority authority(TenantId("tenant.test"), std::move(limits));
  JobResourceRequest request = test_job_resources();
  request.devices.push_back(DeviceResourceRequest{"gpu.test.2", 4096U});

  const TenantQuotaReservation reservation =
      authority.reserve(JobId("job.test.multi-device"), request);
  TenantQuotaSnapshot usage = authority.snapshot();
  EXPECT_EQ(usage.device_bytes.at("gpu.test"), request.devices.front().bytes);
  EXPECT_EQ(usage.device_bytes.at("gpu.test.2"), 4096U);

  authority.release_attempt(reservation.id);
  usage = authority.snapshot();
  EXPECT_EQ(usage.device_bytes.at("gpu.test"), 0U);
  EXPECT_EQ(usage.device_bytes.at("gpu.test.2"), 0U);
}

TEST(SingleTenantJobContract,
     EnforcesConfiguredDeviceCountAt128AcrossAdmissionQuotaAndRestart) {
  JobResourceRequest maximum = test_job_resources();
  maximum.devices = configured_devices(kMaximumConfiguredDevicesPerJob, 1U);
  EXPECT_NO_THROW(JobSpec(GraphArtifactId("graph.test.devices"), 7,
                          OutputSlotId("image.final"), maximum));

  JobResourceRequest excessive = maximum;
  excessive.devices =
      configured_devices(kMaximumConfiguredDevicesPerJob + 1U, 1U);
  EXPECT_THROW(JobSpec(GraphArtifactId("graph.test.devices"), 7,
                       OutputSlotId("image.final"), excessive),
               std::invalid_argument);

  TenantQuotaLimits limits = test_quota_limits();
  limits.capacity.devices =
      configured_devices(kMaximumConfiguredDevicesPerJob, 2U);
  TenantQuotaAuthority authority(TenantId("tenant.test"), limits);
  const TenantQuotaReservation reservation =
      authority.reserve(JobId("job.test.128-devices"), maximum);
  EXPECT_EQ(authority.snapshot().device_bytes.size(),
            kMaximumConfiguredDevicesPerJob);
  authority.release_attempt(reservation.id);

  TenantQuotaLimits excessive_limits = limits;
  excessive_limits.capacity.devices =
      configured_devices(kMaximumConfiguredDevicesPerJob + 1U, 2U);
  EXPECT_THROW(TenantQuotaAuthority(TenantId("tenant.test"),
                                    std::move(excessive_limits)),
               std::invalid_argument);

  ScopedTestStateRoot root;
  const DurableJobRecord record = make_test_durable_job_record(81U, maximum);
  {
    DurableServerState store(root.path(), TenantId("tenant.test"));
    EXPECT_TRUE(store.persist_job(record).succeeded());
  }
  {
    DurableServerState recovered(root.path(), TenantId("tenant.test"));
    const std::vector<DurableJobRecord> jobs = recovered.recovered_jobs();
    ASSERT_EQ(jobs.size(), 1U);
    ASSERT_NE(jobs.front().spec, nullptr);
    EXPECT_EQ(jobs.front().spec->resource_request().devices.size(),
              kMaximumConfiguredDevicesPerJob);
  }
}

TEST(DurableServerState,
     CopiesActiveRowsKeepsIdentitySeparateAndRecoversAfterRestart) {
  ScopedTestStateRoot root;
  OutputCommitReceipt first;
  OutputCommitReceipt second;
  {
    DurableServerState store(root.path(), TenantId("tenant.test"));
    ImageBuffer image = make_test_image();
    first = store.commit_artifact(
        DurableArtifactCommitRequest{
            make_test_identity(1U), OutputSlotId("image.final"),
            ArtifactId("artifact.test.1"), OutputCommitId("commit.test.1"),
            test_job_resources()},
        image);

    auto* source = static_cast<std::byte*>(image.data.get());
    source[0] = std::byte{0x7f};
    const std::shared_ptr<const ArtifactRecord> record =
        store.find_artifact(first.artifact_id);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->payload.size(), 12U);
    EXPECT_EQ(record->payload[0], std::byte{1U});
    EXPECT_EQ(record->payload[6], std::byte{11U});
    EXPECT_EQ(record->receipt.descriptor.row_bytes, 6U);
    EXPECT_EQ(record->receipt.descriptor.payload_bytes, 12U);
    EXPECT_EQ(record->receipt.achieved_durability,
              ArtifactDurability::CrashDurable);

    source[0] = std::byte{1U};
    second = store.commit_artifact(
        DurableArtifactCommitRequest{
            make_test_identity(2U), OutputSlotId("image.final"),
            ArtifactId("artifact.test.2"), OutputCommitId("commit.test.2"),
            test_job_resources()},
        image);
    EXPECT_EQ(first.content_digest, second.content_digest);
    EXPECT_NE(first.artifact_id, second.artifact_id);
    EXPECT_NE(first.output_commit_id, second.output_commit_id);
    EXPECT_EQ(store.recovered_artifacts().size(), 2U);
  }

  DurableServerState recovered(root.path(), TenantId("tenant.test"));
  EXPECT_EQ(recovered.recovered_artifacts().size(), 2U);
  const std::shared_ptr<const ArtifactRecord> first_record =
      recovered.find_commit(first.output_commit_id);
  ASSERT_NE(first_record, nullptr);
  EXPECT_EQ(first_record->receipt.artifact_id, first.artifact_id);
  EXPECT_EQ(first_record->receipt.content_digest, first.content_digest);
  EXPECT_EQ(first_record->payload[0], std::byte{1U});
}

TEST(DurableServerState,
     CleansPreManifestFailureAndReconcilesPostPublicationIdempotently) {
  {
    ScopedTestStateRoot root;
    DurableServerStateOptions options;
    options.artifact_commit_observer = [](DurableArtifactCommitStage stage) {
      if (stage == DurableArtifactCommitStage::PayloadSynchronized) {
        throw std::runtime_error("injected pre-manifest failure");
      }
    };
    DurableServerState store(root.path(), TenantId("tenant.test"), options);
    const DurableArtifactCommitRequest request = make_test_commit_request(3U);
    ImageBuffer image = make_test_image();
    EXPECT_THROW(store.commit_artifact(request, image), std::runtime_error);
    EXPECT_EQ(store.find_artifact(request.artifact_id), nullptr);
    EXPECT_TRUE(store.recovered_artifacts().empty());
  }

  ScopedTestStateRoot root;
  DurableArtifactCommitRequest request = make_test_commit_request(4U);
  ImageBuffer image = make_test_image();
  OutputCommitReceipt original;
  {
    DurableServerStateOptions options;
    options.artifact_commit_observer = [](DurableArtifactCommitStage stage) {
      if (stage == DurableArtifactCommitStage::ManifestPublished) {
        throw std::runtime_error("injected post-manifest failure");
      }
    };
    DurableServerState store(root.path(), TenantId("tenant.test"), options);
    EXPECT_THROW(store.commit_artifact(request, image), std::runtime_error);
    const std::shared_ptr<const ArtifactRecord> published =
        store.find_commit(request.output_commit_id);
    ASSERT_NE(published, nullptr);
    original = published->receipt;
    EXPECT_EQ(store.find_artifact(request.artifact_id), published);

    DurableArtifactCommitRequest retry = request;
    retry.attempt.attempt_id = JobAttemptId("attempt.test.retry");
    retry.attempt.worker_instance_id = WorkerInstanceId("worker.test.retry");
    ++retry.attempt.worker_lease_generation.value;
    const OutputCommitReceipt reconciled = store.commit_artifact(retry, image);
    EXPECT_EQ(reconciled.attempt, original.attempt);
    EXPECT_EQ(reconciled.artifact_id, original.artifact_id);
    EXPECT_EQ(reconciled.output_commit_id, original.output_commit_id);

    auto* source = static_cast<std::byte*>(image.data.get());
    source[0] = std::byte{0x7f};
    EXPECT_THROW(store.commit_artifact(retry, image), DurableConflictError);
  }

  DurableServerState recovered(root.path(), TenantId("tenant.test"));
  const std::shared_ptr<const ArtifactRecord> record =
      recovered.find_commit(original.output_commit_id);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->receipt.artifact_id, original.artifact_id);
  EXPECT_EQ(record->receipt.content_digest, original.content_digest);
}

TEST(DurableServerState,
     PendingDurabilityReplaysBarriersBeforeEveryReceiptReturnPath) {
  constexpr std::size_t kReturnPathCount = 3U;
  for (std::size_t return_path = 0U; return_path < kReturnPathCount;
       ++return_path) {
    SCOPED_TRACE(return_path);
    ScopedTestStateRoot root;
    std::atomic<bool> manifest_failure_armed{true};
    std::atomic<bool> replay_failure_armed{true};
    std::atomic<std::size_t> revalidations{0U};
    std::atomic<std::size_t> root_replays{0U};
    DurableServerStateOptions options;
    options.artifact_commit_observer = [&manifest_failure_armed,
                                        &replay_failure_armed, &revalidations,
                                        &root_replays](
                                           DurableArtifactCommitStage stage) {
      if (stage == DurableArtifactCommitStage::ManifestPublished &&
          manifest_failure_armed.exchange(false, std::memory_order_acq_rel)) {
        throw std::runtime_error(
            "injected manifest-visible acknowledgement failure");
      }
      if (stage == DurableArtifactCommitStage::DurabilityRevalidationStarted) {
        revalidations.fetch_add(1U, std::memory_order_relaxed);
      }
      if (stage == DurableArtifactCommitStage::RootDirectoryBarrierReplay) {
        root_replays.fetch_add(1U, std::memory_order_relaxed);
        if (replay_failure_armed.exchange(false, std::memory_order_acq_rel)) {
          throw std::runtime_error(
              "injected pending durability root-barrier failure");
        }
      }
    };
    DurableServerState store(root.path(), TenantId("tenant.test"), options);
    const DurableArtifactCommitRequest request =
        make_test_commit_request(90U + return_path);
    ImageBuffer image = make_test_image();
    EXPECT_THROW(store.commit_artifact(request, image), std::runtime_error);

    DurableArtifactCommitRequest retry = request;
    retry.attempt.attempt_id = JobAttemptId("attempt.test.pending-retry");
    retry.attempt.worker_instance_id =
        WorkerInstanceId("worker.test.pending-retry");
    ++retry.attempt.worker_lease_generation.value;
    const auto exercise_return_path = [&]() -> OutputCommitReceipt {
      if (return_path == 0U) {
        const auto record = store.find_commit(request.output_commit_id);
        if (record == nullptr) {
          throw std::runtime_error("pending commit alias was reported absent");
        }
        return record->receipt;
      }
      if (return_path == 1U) {
        const auto record = store.find_artifact(request.artifact_id);
        if (record == nullptr) {
          throw std::runtime_error(
              "pending artifact alias was reported absent");
        }
        return record->receipt;
      }
      return store.commit_artifact(retry, image);
    };

    EXPECT_THROW(static_cast<void>(exercise_return_path()), std::runtime_error);
    EXPECT_EQ(revalidations.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(root_replays.load(std::memory_order_relaxed), 1U);

    const OutputCommitReceipt confirmed = exercise_return_path();
    EXPECT_EQ(confirmed.artifact_id, request.artifact_id);
    EXPECT_EQ(confirmed.output_commit_id, request.output_commit_id);
    EXPECT_EQ(confirmed.achieved_durability, ArtifactDurability::CrashDurable);
    EXPECT_EQ(revalidations.load(std::memory_order_relaxed), 2U);
    EXPECT_EQ(root_replays.load(std::memory_order_relaxed), 2U);

    static_cast<void>(exercise_return_path());
    EXPECT_EQ(revalidations.load(std::memory_order_relaxed), 2U);
    EXPECT_EQ(root_replays.load(std::memory_order_relaxed), 2U);
  }
}

TEST(DurableServerState,
     CompletedBarrierTruthSurvivesFinalObserverAcknowledgementFailure) {
  ScopedTestStateRoot root;
  std::atomic<bool> completion_failure_armed{true};
  std::atomic<std::size_t> revalidations{0U};
  DurableServerStateOptions options;
  options.artifact_commit_observer = [&completion_failure_armed,
                                      &revalidations](
                                         DurableArtifactCommitStage stage) {
    if (stage == DurableArtifactCommitStage::DurabilityRevalidationStarted) {
      revalidations.fetch_add(1U, std::memory_order_relaxed);
    }
    if (stage == DurableArtifactCommitStage::DirectoryBarriersCompleted &&
        completion_failure_armed.exchange(false, std::memory_order_acq_rel)) {
      throw std::runtime_error(
          "injected completed-barrier acknowledgement failure");
    }
  };
  DurableServerState store(root.path(), TenantId("tenant.test"), options);
  const DurableArtifactCommitRequest request = make_test_commit_request(94U);
  ImageBuffer image = make_test_image();

  EXPECT_THROW(store.commit_artifact(request, image), std::runtime_error);
  const std::shared_ptr<const ArtifactRecord> confirmed =
      store.find_commit(request.output_commit_id);
  ASSERT_NE(confirmed, nullptr);
  EXPECT_EQ(confirmed->receipt.artifact_id, request.artifact_id);
  EXPECT_EQ(revalidations.load(std::memory_order_relaxed), 0U);
}

TEST(DurableServerState, RollsBackBothAliasesWhenPrivateIndexPreparationFails) {
  constexpr std::array<DurableArtifactCommitStage, 2U> stages{
      DurableArtifactCommitStage::ArtifactIndexPrepared,
      DurableArtifactCommitStage::CommitIndexPrepared};
  for (std::size_t index = 0U; index < stages.size(); ++index) {
    SCOPED_TRACE(index);
    ScopedTestStateRoot root;
    std::atomic<bool> armed{true};
    DurableServerStateOptions options;
    options.artifact_commit_observer = [target = stages[index], &armed](
                                           DurableArtifactCommitStage stage) {
      if (stage == target && armed.exchange(false, std::memory_order_acq_rel)) {
        throw std::runtime_error(
            "injected private artifact index preparation failure");
      }
    };
    DurableServerState store(root.path(), TenantId("tenant.test"), options);
    const DurableArtifactCommitRequest request =
        make_test_commit_request(30U + index);
    ImageBuffer image = make_test_image();

    EXPECT_THROW(store.commit_artifact(request, image), std::runtime_error);
    EXPECT_EQ(store.find_artifact(request.artifact_id), nullptr);
    EXPECT_EQ(store.find_commit(request.output_commit_id), nullptr);
    EXPECT_TRUE(store.recovered_artifacts().empty());

    const OutputCommitReceipt receipt = store.commit_artifact(request, image);
    const std::shared_ptr<const ArtifactRecord> by_artifact =
        store.find_artifact(request.artifact_id);
    const std::shared_ptr<const ArtifactRecord> by_commit =
        store.find_commit(request.output_commit_id);
    ASSERT_NE(by_artifact, nullptr);
    EXPECT_EQ(by_commit, by_artifact);
    EXPECT_EQ(receipt.artifact_id, request.artifact_id);
    EXPECT_EQ(receipt.output_commit_id, request.output_commit_id);
  }
}

TEST(DurableServerState,
     EraseFaultMatrixRevokesBothAliasesAndLeavesRetryableResidue) {
  constexpr std::array<DurableArtifactEraseStage, 12U> stages{
      DurableArtifactEraseStage::BeforeManifestRemoval,
      DurableArtifactEraseStage::ManifestRemoved,
      DurableArtifactEraseStage::ArtifactDirectorySynchronized,
      DurableArtifactEraseStage::ArtifactsDirectorySynchronized,
      DurableArtifactEraseStage::VisibilityRemovalConfirmed,
      DurableArtifactEraseStage::PayloadRemoved,
      DurableArtifactEraseStage::PrivateManifestRemoved,
      DurableArtifactEraseStage::ArtifactDirectoryCleanupSynchronized,
      DurableArtifactEraseStage::ArtifactDirectoryClosed,
      DurableArtifactEraseStage::ArtifactDirectoryRemoved,
      DurableArtifactEraseStage::ArtifactsDirectoryCleanupSynchronized,
      DurableArtifactEraseStage::CleanupBarriersCompleted};
  constexpr std::array<DurableArtifactEraseState, 12U> expected_states{
      DurableArtifactEraseState::NotRemoved,
      DurableArtifactEraseState::ManifestRemovedDurabilityUnconfirmed,
      DurableArtifactEraseState::ManifestRemovedDurabilityUnconfirmed,
      DurableArtifactEraseState::ManifestRemovedDurabilityUnconfirmed,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::FullyCleaned};

  for (std::size_t index = 0U; index < stages.size(); ++index) {
    SCOPED_TRACE(index);
    ScopedTestStateRoot root;
    const auto failure =
        std::make_shared<OneShotArtifactEraseFailure>(stages[index]);
    const DurableArtifactCommitRequest request =
        make_test_commit_request(50U + index);
    {
      DurableServerState store(root.path(), TenantId("tenant.test"),
                               artifact_erase_failure_options(failure));
      ImageBuffer image = make_test_image();
      static_cast<void>(store.commit_artifact(request, image));
      failure->arm();

      const DurableArtifactEraseResult erased =
          store.erase_artifact(request.artifact_id);
      EXPECT_EQ(erased.state, expected_states[index]);
      EXPECT_EQ(erased.payload_bytes, 12U);
      EXPECT_NE(erased.failure, nullptr);
      EXPECT_THROW(erased.rethrow_failure(), std::runtime_error);
      if (stages[index] == DurableArtifactEraseStage::BeforeManifestRemoval) {
        EXPECT_NE(store.find_artifact(request.artifact_id), nullptr);
        EXPECT_NE(store.find_commit(request.output_commit_id), nullptr);
      } else {
        EXPECT_EQ(store.find_artifact(request.artifact_id), nullptr);
        EXPECT_EQ(store.find_commit(request.output_commit_id), nullptr);
      }

      const DurableArtifactEraseResult retried =
          store.erase_artifact(request.artifact_id);
      EXPECT_TRUE(retried.succeeded());
      EXPECT_EQ(store.find_artifact(request.artifact_id), nullptr);
      EXPECT_EQ(store.find_commit(request.output_commit_id), nullptr);
      EXPECT_TRUE(store.recovered_artifacts().empty());
    }
    DurableServerState recovered(root.path(), TenantId("tenant.test"));
    EXPECT_TRUE(recovered.recovered_artifacts().empty());
    EXPECT_EQ(recovered.find_artifact(request.artifact_id), nullptr);
    EXPECT_EQ(recovered.find_commit(request.output_commit_id), nullptr);
  }
}

TEST(DurableServerState,
     RecoveryRemovesUnambiguousResidueAndRejectsPayloadCorruption) {
  ScopedTestStateRoot residue_root;
  {
    DurableServerState initialized(residue_root.path(),
                                   TenantId("tenant.test"));
  }
  const std::filesystem::path residue =
      residue_root.path() / "artifacts" / "artifact.test.residue";
  ASSERT_TRUE(std::filesystem::create_directory(residue));
  {
    std::ofstream payload(residue / "payload.bin", std::ios::binary);
    ASSERT_TRUE(payload.is_open());
    payload.put('x');
    ASSERT_TRUE(payload.good());
  }
  {
    DurableServerState recovered(residue_root.path(), TenantId("tenant.test"));
    EXPECT_FALSE(std::filesystem::exists(residue));
  }

  ScopedTestStateRoot corruption_root;
  const DurableArtifactCommitRequest request = make_test_commit_request(5U);
  {
    DurableServerState store(corruption_root.path(), TenantId("tenant.test"));
    ImageBuffer image = make_test_image();
    static_cast<void>(store.commit_artifact(request, image));
  }
  {
    std::fstream payload(corruption_root.path() / "artifacts" /
                             request.artifact_id.value() / "payload.bin",
                         std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(payload.is_open());
    payload.put('\x7f');
    ASSERT_TRUE(payload.good());
  }
  EXPECT_THROW(
      DurableServerState(corruption_root.path(), TenantId("tenant.test")),
      DurableCorruptionError);
}

TEST(DurableServerState, EnforcesExclusiveRootOwnershipAndNoFollowChildren) {
  {
    ScopedTestStateRoot root;
    DurableServerState owner(root.path(), TenantId("tenant.test"));
    EXPECT_THROW(DurableServerState(root.path(), TenantId("tenant.test")),
                 DurableCapabilityError);
  }

  ScopedTestStateRoot root;
  ScopedTestStateRoot link_target;
  std::filesystem::create_directory_symlink(link_target.path(),
                                            root.path() / "artifacts");
  EXPECT_THROW(DurableServerState(root.path(), TenantId("tenant.test")),
               DurableCorruptionError);
}

TEST(DurableServerState, RejectsConfiguredRootIdentityReplacement) {
  ScopedTestStateRoot root;
  const std::filesystem::path relocated =
      std::filesystem::path(root.path().string() + ".relocated");
  ScopedExtraPathCleanup relocated_cleanup(relocated);
  {
    DurableServerState store(root.path(), TenantId("tenant.test"));
    std::filesystem::rename(root.path(), relocated);
    EXPECT_TRUE(std::filesystem::create_directory(root.path()));
    ImageBuffer image = make_test_image();
    EXPECT_THROW(store.commit_artifact(make_test_commit_request(49U), image),
                 DurableCorruptionError);
  }
}

TEST(DurableServerState, RejectsIncoherentJobLifecycleBeforePersistence) {
  ScopedTestStateRoot root;
  DurableServerState store(root.path(), TenantId("tenant.test"));
  auto spec = std::make_shared<const JobSpec>(GraphArtifactId("graph.test"), 7,
                                              OutputSlotId("image.final"),
                                              test_job_resources());
  AttemptIdentity identity = make_test_identity(50U);
  identity.job_spec_digest = spec->digest();
  DurableJobRecord record;
  record.tenant_id = identity.tenant_id;
  record.job_id = identity.job_id;
  record.spec = std::move(spec);
  record.assignment = identity;
  record.output_artifact_id = ArtifactId("artifact.test.job-record");
  record.output_commit_id = OutputCommitId("commit.test.job-record");
  const DurableJobCommitResult accepted = store.persist_job(record);
  EXPECT_TRUE(accepted.succeeded());

  record.state = JobState::Succeeded;
  record.attempt_settled = true;
  record.attempt_outcome = JobAttemptOutcome::Succeeded;
  const DurableJobCommitResult invalid_success = store.persist_job(record);
  EXPECT_EQ(invalid_success.state, DurableJobCommitState::NotPublished);
  EXPECT_THROW(invalid_success.rethrow_failure(), std::invalid_argument);

  record.state = JobState::Failed;
  record.attempt_settled = false;
  record.attempt_outcome = JobAttemptOutcome::None;
  record.failure = JobAttemptFailure::Compute;
  const DurableJobCommitResult invalid_failure = store.persist_job(record);
  EXPECT_EQ(invalid_failure.state, DurableJobCommitState::NotPublished);
  EXPECT_THROW(invalid_failure.rethrow_failure(), std::invalid_argument);
}

TEST(DurableServerState,
     JobJournalReportsPublicationStateAndKeepsCacheRestartAligned) {
  const std::array<DurableJobCommitStage, 6U> stages{
      DurableJobCommitStage::CachePrepared,
      DurableJobCommitStage::PrivateFileSynchronized,
      DurableJobCommitStage::RecordPublished,
      DurableJobCommitStage::JobsDirectorySynchronized,
      DurableJobCommitStage::ControlDirectorySynchronized,
      DurableJobCommitStage::DirectoryBarriersCompleted};
  for (std::size_t index = 0U; index < stages.size(); ++index) {
    SCOPED_TRACE(static_cast<int>(stages[index]));
    ScopedTestStateRoot root;
    DurableServerStateOptions options;
    options.job_commit_observer =
        [target = stages[index]](DurableJobCommitStage stage) {
          if (stage == target) {
            throw std::runtime_error("injected Job journal stage failure");
          }
        };
    const DurableJobRecord record = make_test_durable_job_record(90U + index);
    DurableJobCommitResult commit;
    {
      DurableServerState store(root.path(), TenantId("tenant.test"), options);
      commit = store.persist_job(record);
      EXPECT_FALSE(commit.succeeded());
      ASSERT_NE(commit.failure, nullptr);
      EXPECT_THROW(commit.rethrow_failure(), std::runtime_error);
      const bool expected_published =
          stages[index] >= DurableJobCommitStage::RecordPublished;
      EXPECT_EQ(commit.published(), expected_published);
      if (stages[index] == DurableJobCommitStage::DirectoryBarriersCompleted) {
        EXPECT_EQ(commit.state, DurableJobCommitState::ConfirmedCommitted);
      } else if (expected_published) {
        EXPECT_EQ(commit.state,
                  DurableJobCommitState::RecordPublishedDurabilityUnconfirmed);
      } else {
        EXPECT_EQ(commit.state, DurableJobCommitState::NotPublished);
      }
      EXPECT_EQ(store.recovered_jobs().size(), expected_published ? 1U : 0U);
    }
    {
      DurableServerState recovered(root.path(), TenantId("tenant.test"));
      const std::vector<DurableJobRecord> jobs = recovered.recovered_jobs();
      EXPECT_EQ(jobs.size(), commit.published() ? 1U : 0U);
      if (commit.published()) {
        ASSERT_EQ(jobs.size(), 1U);
        EXPECT_EQ(jobs.front().job_id, record.job_id);
        EXPECT_EQ(jobs.front().assignment, record.assignment);
      }
    }
  }
}

/**
 * @brief Proves concurrent contenders reserve the final value exactly once.
 *
 * Starts 32 callers from one barrier against a local sequence initialized to
 * `UINT64_MAX - 1`, then verifies one successful final reservation, fail-closed
 * overflow for every loser, and stable overflow across repeated retries.
 */
TEST(SingleTenantIdentitySequence,
     ConcurrentFinalReservationSaturatesWithoutReuse) {
  constexpr std::size_t kCallerCount = 32U;
  constexpr std::size_t kSaturatedRetryCount = 128U;
  constexpr std::uint64_t kMaximum = std::numeric_limits<std::uint64_t>::max();
  std::atomic<std::uint64_t> sequence{kMaximum - 1U};
  auto gate = std::make_shared<WorkerGroupGate>();
  std::vector<std::future<std::optional<std::uint64_t>>> attempts;
  attempts.reserve(kCallerCount);
  WorkerGroupGateReleaseGuard gate_release(gate);

  for (std::size_t index = 0U; index < kCallerCount; ++index) {
    attempts.push_back(std::async(std::launch::async, [gate, &sequence] {
      gate->enter_and_wait();
      try {
        return std::optional<std::uint64_t>(
            SingleTenantJobServiceTestAccess::reserve_identity_sequence_value(
                &sequence));
      } catch (const std::overflow_error&) {
        return std::optional<std::uint64_t>();
      }
    }));
  }

  const bool all_callers_ready = gate->wait_until_entered(kCallerCount);
  gate_release.release();

  std::size_t reserved_count = 0U;
  std::size_t overflow_count = 0U;
  for (auto& attempt : attempts) {
    try {
      const std::optional<std::uint64_t> result = attempt.get();
      if (result.has_value()) {
        ++reserved_count;
        EXPECT_EQ(*result, kMaximum);
      } else {
        ++overflow_count;
      }
    } catch (const std::exception& error) {
      ADD_FAILURE() << "identity reservation raised unexpectedly: "
                    << error.what();
    } catch (...) {
      ADD_FAILURE() << "identity reservation raised a non-standard exception";
    }
  }

  EXPECT_TRUE(all_callers_ready);
  EXPECT_EQ(reserved_count, 1U);
  EXPECT_EQ(overflow_count, kCallerCount - 1U);
  EXPECT_EQ(sequence.load(std::memory_order_relaxed), kMaximum);

  for (std::size_t index = 0U; index < kSaturatedRetryCount; ++index) {
    EXPECT_THROW(
        SingleTenantJobServiceTestAccess::reserve_identity_sequence_value(
            &sequence),
        std::overflow_error);
    EXPECT_EQ(sequence.load(std::memory_order_relaxed), kMaximum);
  }
}

/**
 * @brief Proves a saturated reservation never exposes a wrapped value.
 *
 * Pauses the exact production reservation helper after its initial load so a
 * second caller can inspect and contend with the saturated sequence before the
 * first caller completes.
 */
TEST(SingleTenantIdentitySequence,
     SaturatedReservationNeverPublishesWrappedState) {
  constexpr std::uint64_t kMaximum = std::numeric_limits<std::uint64_t>::max();
  std::atomic<std::uint64_t> sequence{kMaximum};
  auto observation_gate = std::make_shared<WorkerGate>();
  std::future<std::optional<std::uint64_t>> observed_attempt =
      std::async(std::launch::async, [observation_gate, &sequence] {
        try {
          return std::optional<std::uint64_t>(
              SingleTenantJobServiceTestAccess::reserve_identity_with_observer(
                  &sequence,
                  [observation_gate] { observation_gate->enter_and_wait(); }));
        } catch (const std::overflow_error&) {
          return std::optional<std::uint64_t>();
        }
      });
  WorkerGateReleaseGuard gate_release(observation_gate);

  const bool observation_reached = observation_gate->wait_until_entered();
  EXPECT_TRUE(observation_reached);
  EXPECT_EQ(sequence.load(std::memory_order_relaxed), kMaximum);
  EXPECT_THROW(
      SingleTenantJobServiceTestAccess::reserve_identity_sequence_value(
          &sequence),
      std::overflow_error);
  EXPECT_EQ(sequence.load(std::memory_order_relaxed), kMaximum);

  gate_release.release();
  try {
    EXPECT_FALSE(observed_attempt.get().has_value());
  } catch (const std::exception& error) {
    ADD_FAILURE() << "observed identity reservation raised unexpectedly: "
                  << error.what();
  } catch (...) {
    ADD_FAILURE()
        << "observed identity reservation raised a non-standard exception";
  }
  EXPECT_EQ(sequence.load(std::memory_order_relaxed), kMaximum);
}

TEST(SingleTenantJobService, SuccessRequiresReceiptAndSupportsArtifactLookup) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Succeeded);
  EXPECT_TRUE(terminal->attempt_settled);
  ASSERT_TRUE(terminal->output_receipt.has_value());
  EXPECT_EQ(terminal->output_receipt->attempt, submission.assignment);
  EXPECT_EQ(terminal->output_receipt->output_slot_id,
            OutputSlotId("image.final"));
  EXPECT_NE(service.find_artifact(terminal->output_receipt->artifact_id),
            nullptr);
  EXPECT_FALSE(service.cancel(submission.job_id));
}

TEST(SingleTenantJobService,
     RejectsQuotaBeforeJobOrWorkerPublicationAndReleasesOnSettlement) {
  auto gate = std::make_shared<WorkerGate>();
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [gate](const JobAssignment& assignment,
             const std::function<bool()>& cancellation_requested) {
        gate->enter_and_wait();
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  ScopedTestStateRoot root;
  TenantQuotaLimits limits = test_quota_limits();
  limits.maximum_active_attempts = 1U;
  limits.capacity = test_job_resources();
  SingleTenantJobService service(TenantId("tenant.test"), std::move(limits),
                                 root.path(), factory);
  WorkerGateReleaseGuard gate_release(gate);

  const JobSubmission accepted = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  ASSERT_TRUE(gate->wait_until_entered());
  try {
    static_cast<void>(service.submit(JobSpec(GraphArtifactId("graph.test"), 8,
                                             OutputSlotId("image.final"),
                                             test_job_resources())));
    ADD_FAILURE() << "over-concurrency submission unexpectedly succeeded";
  } catch (const TenantQuotaExceeded& error) {
    EXPECT_EQ(error.dimension(), TenantQuotaDimension::Concurrency);
  }
  EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service), 1U);
  TenantQuotaSnapshot usage = service.quota_snapshot();
  EXPECT_EQ(usage.active_attempts, 1U);
  EXPECT_EQ(usage.retained_artifacts, 0U);

  gate_release.release();
  const std::optional<JobSnapshot> terminal =
      service.wait_for(accepted.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  ASSERT_EQ(terminal->state, JobState::Succeeded);
  usage = service.quota_snapshot();
  EXPECT_EQ(usage.active_attempts, 0U);
  EXPECT_EQ(usage.retention_bytes, 12U);
  EXPECT_EQ(usage.retained_artifacts, 1U);
}

TEST(SingleTenantJobService,
     ExplicitRetryPreservesStableTruthAndMintsFreshAttemptAuthority) {
  std::atomic<std::size_t> calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&calls](const JobAssignment& assignment,
               const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        if (calls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
          return settled_failed_report(assignment);
        }
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission first = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> failed =
      service.wait_for(first.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(failed.has_value());
  ASSERT_EQ(failed->state, JobState::Failed);
  EXPECT_TRUE(failed->attempt_settled);
  EXPECT_EQ(failed->failure, JobAttemptFailure::Compute);
  EXPECT_EQ(service.quota_snapshot().active_attempts, 0U);
  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));

  const std::optional<JobSubmission> retried = service.retry(first.job_id);
  ASSERT_TRUE(retried.has_value());
  EXPECT_EQ(retried->job_id, first.job_id);
  EXPECT_EQ(retried->job_spec_digest, first.job_spec_digest);
  EXPECT_NE(retried->assignment.attempt_id, first.assignment.attempt_id);
  EXPECT_NE(retried->assignment.worker_instance_id,
            first.assignment.worker_instance_id);
  EXPECT_EQ(retried->assignment.worker_lease_generation.value,
            first.assignment.worker_lease_generation.value + 1U);

  const std::optional<JobSnapshot> succeeded =
      service.wait_for(first.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  ASSERT_EQ(succeeded->state, JobState::Succeeded);
  EXPECT_EQ(succeeded->output_artifact_id, failed->output_artifact_id);
  EXPECT_EQ(succeeded->output_commit_id, failed->output_commit_id);
  ASSERT_TRUE(succeeded->output_receipt.has_value());
  EXPECT_EQ(succeeded->output_receipt->attempt, retried->assignment);
  EXPECT_FALSE(service.retry(first.job_id).has_value());
  EXPECT_EQ(service.quota_snapshot().retention_bytes, 12U);

  EXPECT_TRUE(service.delete_artifact(succeeded->output_receipt->artifact_id));
  EXPECT_FALSE(service.delete_artifact(succeeded->output_receipt->artifact_id));
  EXPECT_EQ(service.quota_snapshot().retention_bytes, 0U);
  const std::size_t accepted_before_checkpoint =
      SingleTenantJobServiceTestAccess::accepted_job_count(service);
  EXPECT_THROW(
      service.submit(JobSpec(GraphArtifactId("graph.test"), 9,
                             OutputSlotId("image.final"), test_job_resources(),
                             succeeded->output_receipt->artifact_id)),
      std::invalid_argument);
  EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service),
            accepted_before_checkpoint);
  const std::optional<JobSnapshot> historical = service.query(first.job_id);
  ASSERT_TRUE(historical.has_value());
  EXPECT_EQ(historical->state, JobState::Succeeded);
  ASSERT_TRUE(historical->output_receipt.has_value());
  EXPECT_EQ(historical->output_receipt->artifact_id,
            succeeded->output_receipt->artifact_id);
  EXPECT_EQ(historical->output_receipt->output_commit_id,
            succeeded->output_receipt->output_commit_id);
}

TEST(SingleTenantJobService,
     QuotaReleaseFailureFailStopsMutationAfterFailedTerminal) {
  ScopedTestStateRoot root;
  const auto release_failure = std::make_shared<OneShotQuotaReleaseFailure>();
  std::atomic<std::size_t> worker_calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&worker_calls](const JobAssignment& assignment,
                      const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        if (worker_calls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
          return settled_failed_report(assignment);
        }
        return successful_report(assignment);
      });
  JobSubmission submission;
  JobSnapshot failed;
  {
    SingleTenantJobService service(
        TenantId("tenant.test"), test_quota_limits(), root.path(), factory, {},
        quota_release_failure_options(release_failure));
    release_failure->arm();

    submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                        OutputSlotId("image.final"),
                                        test_job_resources()));
    const std::optional<JobSnapshot> terminal =
        service.wait_for(submission.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(terminal.has_value());
    failed = *terminal;
    ASSERT_EQ(failed.state, JobState::Failed);
    EXPECT_TRUE(failed.attempt_settled);
    EXPECT_EQ(failed.failure, JobAttemptFailure::Compute);
    expect_quota_release_fail_stop(service, submission.job_id, true, 1U, 0U);
  }

  SingleTenantJobService recovered(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), factory);
  const std::optional<JobSnapshot> durable = recovered.query(submission.job_id);
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(durable->state, failed.state);
  EXPECT_EQ(durable->assignment, failed.assignment);
  EXPECT_EQ(durable->failure, failed.failure);
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::quota_reservation_ownership(recovered)
          .total(),
      0U);
  EXPECT_FALSE(
      SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));

  const std::optional<JobSubmission> retried =
      recovered.retry(submission.job_id);
  ASSERT_TRUE(retried.has_value());
  const std::optional<JobSnapshot> succeeded =
      recovered.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  EXPECT_EQ(succeeded->state, JobState::Succeeded);
  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 2U);
}

TEST(SingleTenantJobService,
     QuotaReleaseFailureRetainsCancelledTruthAcrossRestart) {
  ScopedTestStateRoot root;
  const auto release_failure = std::make_shared<OneShotQuotaReleaseFailure>();
  auto gate = std::make_shared<WorkerGate>();
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [gate](const JobAssignment& assignment,
             const std::function<bool()>& cancellation_requested) {
        gate->enter_and_wait();
        if (!cancellation_requested()) {
          return successful_report(assignment);
        }
        JobAttemptReport report;
        report.identity = assignment.identity;
        report.outcome = JobAttemptOutcome::Cancelled;
        report.settled = true;
        report.failure = JobAttemptFailure::CancellationObserved;
        report.message = "worker observed cancellation";
        return report;
      });
  JobSubmission submission;
  {
    SingleTenantJobService service(
        TenantId("tenant.test"), test_quota_limits(), root.path(), factory, {},
        quota_release_failure_options(release_failure));
    WorkerGateReleaseGuard gate_release(gate);
    submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                        OutputSlotId("image.final"),
                                        test_job_resources()));
    ASSERT_TRUE(gate->wait_until_entered());
    EXPECT_TRUE(service.cancel(submission.job_id));
    release_failure->arm();
    gate_release.release();

    const std::optional<JobSnapshot> cancelled =
        service.wait_for(submission.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->state, JobState::Cancelled);
    EXPECT_TRUE(cancelled->attempt_settled);
    EXPECT_EQ(cancelled->attempt_outcome, JobAttemptOutcome::Cancelled);
    EXPECT_EQ(cancelled->failure, JobAttemptFailure::CancellationObserved);
    expect_quota_release_fail_stop(service, submission.job_id, true, 1U, 0U);
  }

  SingleTenantJobService recovered(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), factory);
  const std::optional<JobSnapshot> durable = recovered.query(submission.job_id);
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(durable->state, JobState::Cancelled);
  EXPECT_TRUE(durable->attempt_settled);
  EXPECT_EQ(durable->attempt_outcome, JobAttemptOutcome::Cancelled);
  EXPECT_EQ(durable->failure, JobAttemptFailure::CancellationObserved);
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::quota_reservation_ownership(recovered)
          .total(),
      0U);
  EXPECT_FALSE(
      SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));
  EXPECT_FALSE(recovered.retry(submission.job_id).has_value());

  const JobSubmission accepted = recovered.submit(
      JobSpec(GraphArtifactId("graph.test"), 8, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> succeeded =
      recovered.wait_for(accepted.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  EXPECT_EQ(succeeded->state, JobState::Succeeded);
}

TEST(SingleTenantJobService,
     QuotaReleaseFailureRetainsRejectedReportTruthAcrossRestart) {
  constexpr std::array<bool, 2U> kMismatchedLeaseModes{false, true};
  for (const bool mismatched_lease : kMismatchedLeaseModes) {
    SCOPED_TRACE(::testing::Message()
                 << "mismatched_lease=" << mismatched_lease);
    ScopedTestStateRoot root;
    const auto release_failure = std::make_shared<OneShotQuotaReleaseFailure>();
    auto rejected_factory = std::make_shared<FunctionWorkerFactory>(
        [mismatched_lease](
            const JobAssignment& assignment,
            const std::function<bool()>& cancellation_requested) {
          EXPECT_FALSE(cancellation_requested());
          if (!mismatched_lease) {
            return malformed_report(assignment,
                                    MalformedReportShape::FailedWithoutFailure);
          }
          JobAttemptReport report = successful_report(assignment);
          ++report.identity.worker_lease_generation.value;
          return report;
        });
    JobSubmission submission;
    JobSnapshot rejected;
    {
      SingleTenantJobService service(
          TenantId("tenant.test"), test_quota_limits(), root.path(),
          rejected_factory, {}, quota_release_failure_options(release_failure));
      release_failure->arm();
      submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                          OutputSlotId("image.final"),
                                          test_job_resources()));
      const std::optional<JobSnapshot> terminal =
          service.wait_for(submission.job_id, std::chrono::seconds(2));
      ASSERT_TRUE(terminal.has_value());
      rejected = *terminal;
      EXPECT_EQ(rejected.state, JobState::Failed);
      EXPECT_FALSE(rejected.attempt_outcome.has_value());
      EXPECT_FALSE(rejected.attempt_settled);
      EXPECT_EQ(rejected.failure, JobAttemptFailure::ReportRejected);
      EXPECT_FALSE(rejected.output_receipt.has_value());
      expect_quota_release_fail_stop(service, submission.job_id, true, 1U, 0U);
    }

    auto recovery_factory = std::make_shared<FunctionWorkerFactory>(
        [](const JobAssignment& assignment,
           const std::function<bool()>& cancellation_requested) {
          EXPECT_FALSE(cancellation_requested());
          return successful_report(assignment);
        });
    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(),
                                     recovery_factory);
    const std::optional<JobSnapshot> durable =
        recovered.query(submission.job_id);
    ASSERT_TRUE(durable.has_value());
    EXPECT_EQ(durable->state, rejected.state);
    EXPECT_EQ(durable->assignment, rejected.assignment);
    EXPECT_EQ(durable->attempt_outcome, rejected.attempt_outcome);
    EXPECT_EQ(durable->attempt_settled, rejected.attempt_settled);
    EXPECT_EQ(durable->failure, rejected.failure);
    EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
    EXPECT_FALSE(
        SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));
    EXPECT_FALSE(recovered.retry(submission.job_id).has_value());

    const JobSubmission accepted = recovered.submit(
        JobSpec(GraphArtifactId("graph.test"), 8, OutputSlotId("image.final"),
                test_job_resources()));
    const std::optional<JobSnapshot> succeeded =
        recovered.wait_for(accepted.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(succeeded.has_value());
    EXPECT_EQ(succeeded->state, JobState::Succeeded);
  }
}

TEST(SingleTenantJobService,
     QuotaReleaseFailureRetainsPreManifestArtifactFailureAcrossRestart) {
  ScopedTestStateRoot root;
  const auto release_failure = std::make_shared<OneShotQuotaReleaseFailure>();
  std::atomic<std::size_t> worker_calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&worker_calls](const JobAssignment& assignment,
                      const std::function<bool()>& cancellation_requested) {
        worker_calls.fetch_add(1U, std::memory_order_relaxed);
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  JobSubmission submission;
  JobSnapshot failed;
  {
    DurableServerStateOptions options;
    options.artifact_commit_observer = [](DurableArtifactCommitStage stage) {
      if (stage == DurableArtifactCommitStage::PayloadSynchronized) {
        throw std::runtime_error("injected pre-manifest commit failure");
      }
    };
    SingleTenantJobService service(
        TenantId("tenant.test"), test_quota_limits(), root.path(), factory,
        std::move(options), quota_release_failure_options(release_failure));
    release_failure->arm();
    submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                        OutputSlotId("image.final"),
                                        test_job_resources()));
    const std::optional<JobSnapshot> terminal =
        service.wait_for(submission.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(terminal.has_value());
    failed = *terminal;
    EXPECT_EQ(failed.state, JobState::Failed);
    EXPECT_EQ(failed.attempt_outcome, JobAttemptOutcome::Succeeded);
    EXPECT_TRUE(failed.attempt_settled);
    EXPECT_EQ(failed.failure, JobAttemptFailure::ArtifactCommit);
    EXPECT_FALSE(failed.output_receipt.has_value());
    EXPECT_EQ(service.find_artifact(failed.output_artifact_id), nullptr);
    expect_quota_release_fail_stop(service, submission.job_id, true, 1U, 0U);
  }

  SingleTenantJobService recovered(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), factory);
  const std::optional<JobSnapshot> durable = recovered.query(submission.job_id);
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(durable->state, failed.state);
  EXPECT_EQ(durable->assignment, failed.assignment);
  EXPECT_EQ(durable->attempt_outcome, failed.attempt_outcome);
  EXPECT_EQ(durable->attempt_settled, failed.attempt_settled);
  EXPECT_EQ(durable->failure, failed.failure);
  EXPECT_FALSE(durable->output_receipt.has_value());
  EXPECT_EQ(recovered.find_artifact(failed.output_artifact_id), nullptr);
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  EXPECT_FALSE(
      SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));

  const std::optional<JobSubmission> retried =
      recovered.retry(submission.job_id);
  ASSERT_TRUE(retried.has_value());
  const std::optional<JobSnapshot> succeeded =
      recovered.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  EXPECT_EQ(succeeded->state, JobState::Succeeded);
  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 2U);
}

TEST(SingleTenantJobService,
     QuotaReleaseFailStopFencesConcurrentAttemptReportUntilRestart) {
  ScopedTestStateRoot root;
  const auto release_failure = std::make_shared<OneShotQuotaReleaseFailure>();
  auto failing_gate = std::make_shared<WorkerGate>();
  auto active_gate = std::make_shared<WorkerGate>();
  std::atomic<std::size_t> worker_calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [failing_gate, active_gate, &worker_calls](
          const JobAssignment& assignment,
          const std::function<bool()>& cancellation_requested) {
        const std::size_t call =
            worker_calls.fetch_add(1U, std::memory_order_relaxed);
        if (call == 0U) {
          failing_gate->enter_and_wait();
          EXPECT_FALSE(cancellation_requested());
          return settled_failed_report(assignment);
        }
        active_gate->enter_and_wait();
        return successful_report(assignment);
      });
  JobSubmission failed_submission;
  JobSubmission interrupted_submission;
  {
    SingleTenantJobService service(
        TenantId("tenant.test"), test_quota_limits(), root.path(), factory, {},
        quota_release_failure_options(release_failure));
    WorkerGateReleaseGuard failing_release(failing_gate);
    WorkerGateReleaseGuard active_release(active_gate);
    failed_submission = service.submit(
        JobSpec(GraphArtifactId("graph.test.failure"), 7,
                OutputSlotId("image.final"), test_job_resources()));
    ASSERT_TRUE(failing_gate->wait_until_entered());
    interrupted_submission = service.submit(
        JobSpec(GraphArtifactId("graph.test.active"), 8,
                OutputSlotId("image.final"), test_job_resources()));
    ASSERT_TRUE(active_gate->wait_until_entered());

    release_failure->arm();
    failing_release.release();
    const std::optional<JobSnapshot> failed =
        service.wait_for(failed_submission.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(failed.has_value());
    ASSERT_EQ(failed->state, JobState::Failed);
    expect_quota_release_fail_stop(service, failed_submission.job_id, true, 2U,
                                   0U);

    const std::optional<JobSnapshot> before_report =
        service.query(interrupted_submission.job_id);
    ASSERT_TRUE(before_report.has_value());
    ASSERT_EQ(before_report->state, JobState::Running);
    JobAssignment assignment{before_report->assignment, before_report->spec,
                             nullptr};
    SingleTenantJobServiceTestAccess::inject_attempt_report(
        service, before_report->assignment, successful_report(assignment));
    const std::optional<JobSnapshot> after_report =
        service.query(interrupted_submission.job_id);
    ASSERT_TRUE(after_report.has_value());
    EXPECT_EQ(after_report->state, before_report->state);
    EXPECT_EQ(after_report->assignment, before_report->assignment);
    EXPECT_EQ(after_report->attempt_outcome, before_report->attempt_outcome);
    EXPECT_EQ(after_report->attempt_settled, before_report->attempt_settled);
    EXPECT_EQ(after_report->failure, before_report->failure);
    EXPECT_FALSE(after_report->output_receipt.has_value());

    active_release.release();
    EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                    wait_for_owned_worker_thread_count_at_most(
                        service, 0U, std::chrono::seconds(2)));
    const std::optional<JobSnapshot> still_running =
        service.query(interrupted_submission.job_id);
    ASSERT_TRUE(still_running.has_value());
    EXPECT_EQ(still_running->state, JobState::Running);
  }

  auto recovery_factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  SingleTenantJobService recovered(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), recovery_factory);
  const std::optional<JobSnapshot> durable_failure =
      recovered.query(failed_submission.job_id);
  ASSERT_TRUE(durable_failure.has_value());
  EXPECT_EQ(durable_failure->state, JobState::Failed);
  EXPECT_EQ(durable_failure->failure, JobAttemptFailure::Compute);
  const std::optional<JobSnapshot> interrupted =
      recovered.query(interrupted_submission.job_id);
  ASSERT_TRUE(interrupted.has_value());
  EXPECT_EQ(interrupted->state, JobState::Failed);
  EXPECT_TRUE(interrupted->attempt_settled);
  EXPECT_EQ(interrupted->attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(interrupted->failure, JobAttemptFailure::RecoveryInterrupted);
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::quota_reservation_ownership(recovered)
          .total(),
      0U);
  EXPECT_FALSE(
      SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));

  const JobSubmission accepted = recovered.submit(
      JobSpec(GraphArtifactId("graph.test.recovered"), 9,
              OutputSlotId("image.final"), test_job_resources()));
  const std::optional<JobSnapshot> succeeded =
      recovered.wait_for(accepted.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  EXPECT_EQ(succeeded->state, JobState::Succeeded);
}

TEST(SingleTenantJobService, StaleAttemptReportCannotFailCurrentRetry) {
  std::atomic<std::size_t> calls{0U};
  auto retry_gate = std::make_shared<WorkerGate>();
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&calls, retry_gate](
          const JobAssignment& assignment,
          const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        if (calls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
          return settled_failed_report(assignment);
        }
        retry_gate->enter_and_wait();
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  WorkerGateReleaseGuard retry_release(retry_gate);
  const JobSubmission first = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> failed =
      service.wait_for(first.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(failed.has_value());
  ASSERT_EQ(failed->state, JobState::Failed);
  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));

  const std::optional<JobSubmission> retried = service.retry(first.job_id);
  ASSERT_TRUE(retried.has_value());
  ASSERT_TRUE(retry_gate->wait_until_entered());
  JobAssignment stale_assignment;
  stale_assignment.identity = first.assignment;
  JobAttemptReport stale_report = successful_report(stale_assignment);
  SingleTenantJobServiceTestAccess::inject_attempt_report(
      service, first.assignment, std::move(stale_report));

  const std::optional<JobSnapshot> still_running = service.query(first.job_id);
  ASSERT_TRUE(still_running.has_value());
  EXPECT_EQ(still_running->state, JobState::Running);
  EXPECT_EQ(still_running->assignment, retried->assignment);
  EXPECT_FALSE(still_running->attempt_settled);
  EXPECT_EQ(still_running->failure, JobAttemptFailure::None);
  EXPECT_FALSE(still_running->output_receipt.has_value());
  EXPECT_EQ(service.quota_snapshot().active_attempts, 1U);

  retry_release.release();
  const std::optional<JobSnapshot> succeeded =
      service.wait_for(first.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  EXPECT_EQ(succeeded->state, JobState::Succeeded);
  ASSERT_TRUE(succeeded->output_receipt.has_value());
  EXPECT_EQ(succeeded->output_receipt->attempt, retried->assignment);
}

TEST(SingleTenantJobService,
     AuthorizesDurableCheckpointBeforeAdmissionAndBindsWorkerView) {
  std::atomic<std::size_t> calls{0U};
  std::optional<ArtifactId> expected_checkpoint;
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&calls, &expected_checkpoint](
          const JobAssignment& assignment,
          const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        const std::size_t call = calls.fetch_add(1U);
        if (call == 0U) {
          EXPECT_EQ(assignment.checkpoint, nullptr);
        } else {
          if (!expected_checkpoint.has_value() ||
              assignment.checkpoint == nullptr) {
            ADD_FAILURE() << "authorized checkpoint was not assigned";
            return settled_failed_report(assignment);
          }
          EXPECT_EQ(assignment.checkpoint->receipt.artifact_id,
                    *expected_checkpoint);
          EXPECT_EQ(assignment.spec->checkpoint_artifact_id(),
                    expected_checkpoint);
        }
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission producer = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> produced =
      service.wait_for(producer.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(produced.has_value());
  ASSERT_TRUE(produced->output_receipt.has_value());
  expected_checkpoint = produced->output_receipt->artifact_id;

  const std::size_t accepted_before =
      SingleTenantJobServiceTestAccess::accepted_job_count(service);
  EXPECT_THROW(
      service.submit(JobSpec(GraphArtifactId("graph.test"), 8,
                             OutputSlotId("image.final"), test_job_resources(),
                             ArtifactId("artifact.test.missing"))),
      std::invalid_argument);
  EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service),
            accepted_before);

  const JobSubmission consumer = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 8, OutputSlotId("image.final"),
              test_job_resources(), expected_checkpoint));
  const std::optional<JobSnapshot> consumed =
      service.wait_for(consumer.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(consumed.has_value());
  EXPECT_EQ(consumed->state, JobState::Succeeded);
  EXPECT_EQ(calls.load(), 2U);
}

TEST(SingleTenantJobService,
     RejectsLegacyOversizedCheckpointBeforeQuotaOrWorkerPublication) {
  const std::size_t payload_bytes =
      maximum_worker_checkpoint_payload_bytes() + 1U;
  ASSERT_LE(payload_bytes,
            static_cast<std::size_t>(std::numeric_limits<int>::max()));
  std::atomic<std::size_t> calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&calls, payload_bytes](
          const JobAssignment& assignment,
          const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        calls.fetch_add(1U, std::memory_order_relaxed);
        JobAttemptReport report;
        report.identity = assignment.identity;
        report.outcome = JobAttemptOutcome::Succeeded;
        report.settled = true;
        report.failure = JobAttemptFailure::None;
        report.image = make_aligned_cpu_image_buffer(
            static_cast<int>(payload_bytes), 1, 1, DataType::UINT8, 64U);
        return report;
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobResourceRequest resources = checkpoint_transport_resources();
  const JobSubmission producer = service.submit(
      JobSpec(GraphArtifactId("graph.test.checkpoint-bound-producer"), 7,
              OutputSlotId("image.final"), resources));
  const std::optional<JobSnapshot> produced =
      service.wait_for(producer.job_id, std::chrono::seconds(3));
  ASSERT_TRUE(produced.has_value());
  ASSERT_EQ(produced->state, JobState::Succeeded);
  ASSERT_TRUE(produced->output_receipt.has_value());
  const ArtifactId checkpoint = produced->output_receipt->artifact_id;
  const std::shared_ptr<const ArtifactRecord> artifact =
      service.find_artifact(checkpoint);
  ASSERT_NE(artifact, nullptr);
  EXPECT_EQ(artifact->payload.size(), payload_bytes);
  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));

  const std::size_t accepted_before =
      SingleTenantJobServiceTestAccess::accepted_job_count(service);
  const TenantQuotaSnapshot quota_before = service.quota_snapshot();
  ASSERT_EQ(quota_before.active_attempts, 0U);
  ASSERT_EQ(quota_before.retained_artifacts, 1U);
  ASSERT_EQ(quota_before.retention_bytes, payload_bytes);
  EXPECT_THROW(service.submit(
                   JobSpec(GraphArtifactId("graph.test.checkpoint-bound"), 8,
                           OutputSlotId("image.final"), resources, checkpoint)),
               std::invalid_argument);

  EXPECT_EQ(calls.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service),
            accepted_before);
  EXPECT_EQ(service.find_artifact(checkpoint), artifact);
  const TenantQuotaSnapshot quota_after = service.quota_snapshot();
  EXPECT_EQ(quota_after.active_attempts, 0U);
  EXPECT_EQ(quota_after.cpu_slots, 0U);
  EXPECT_EQ(quota_after.host_memory_bytes, 0U);
  EXPECT_EQ(quota_after.output_bytes, 0U);
  EXPECT_EQ(quota_after.staging_bytes, 0U);
  EXPECT_EQ(quota_after.retained_artifacts, quota_before.retained_artifacts);
  EXPECT_EQ(quota_after.retention_bytes, quota_before.retention_bytes);
  EXPECT_EQ(quota_after.device_bytes, quota_before.device_bytes);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::owned_worker_thread_count(service), 0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(service), 0U);
}

TEST(SingleTenantJobService,
     RetryReauthorizesCheckpointBeforeQuotaOrWorkerPublication) {
  std::atomic<std::size_t> calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&calls](const JobAssignment& assignment,
               const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        const std::size_t call = calls.fetch_add(1U);
        if (call == 1U) {
          return settled_failed_report(assignment);
        }
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission producer = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> produced =
      service.wait_for(producer.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(produced.has_value());
  ASSERT_TRUE(produced->output_receipt.has_value());
  const ArtifactId checkpoint = produced->output_receipt->artifact_id;

  const JobSubmission consumer = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 8, OutputSlotId("image.final"),
              test_job_resources(), checkpoint));
  const std::optional<JobSnapshot> failed =
      service.wait_for(consumer.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(failed.has_value());
  ASSERT_EQ(failed->state, JobState::Failed);
  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));
  ASSERT_TRUE(service.delete_artifact(checkpoint));

  EXPECT_THROW(static_cast<void>(service.retry(consumer.job_id)),
               std::invalid_argument);
  const std::optional<JobSnapshot> unchanged = service.query(consumer.job_id);
  ASSERT_TRUE(unchanged.has_value());
  EXPECT_EQ(unchanged->state, JobState::Failed);
  EXPECT_EQ(unchanged->assignment, failed->assignment);
  EXPECT_EQ(unchanged->failure, failed->failure);
  EXPECT_EQ(service.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::owned_worker_thread_count(service), 0U);
  EXPECT_EQ(calls.load(), 2U);
}

TEST(SingleTenantJobService,
     ReconcilesPostManifestExceptionAndRetainsQuotaExactlyOnce) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  ScopedTestStateRoot root;
  DurableServerStateOptions options;
  options.artifact_commit_observer = [](DurableArtifactCommitStage stage) {
    if (stage == DurableArtifactCommitStage::ManifestPublished) {
      throw std::runtime_error("injected lost artifact acknowledgement");
    }
  };
  SingleTenantJobService service(TenantId("tenant.test"), test_quota_limits(),
                                 root.path(), factory, std::move(options));
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  ASSERT_EQ(terminal->state, JobState::Succeeded);
  ASSERT_TRUE(terminal->output_receipt.has_value());
  EXPECT_NE(service.find_artifact(terminal->output_receipt->artifact_id),
            nullptr);
  const TenantQuotaSnapshot usage = service.quota_snapshot();
  EXPECT_EQ(usage.active_attempts, 0U);
  EXPECT_EQ(usage.retention_bytes, 12U);
  EXPECT_EQ(usage.retained_artifacts, 1U);
}

TEST(SingleTenantJobService,
     PendingBarrierReplayFailureFailStopsAndRestartReconcilesSuccess) {
  ScopedTestStateRoot root;
  std::atomic<std::size_t> worker_calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&worker_calls](const JobAssignment& assignment,
                      const std::function<bool()>& cancellation_requested) {
        worker_calls.fetch_add(1U, std::memory_order_relaxed);
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  std::atomic<bool> manifest_failure_armed{true};
  std::atomic<std::size_t> replay_attempts{0U};
  JobSubmission submission;
  {
    DurableServerStateOptions options;
    options.artifact_commit_observer = [&manifest_failure_armed,
                                        &replay_attempts](
                                           DurableArtifactCommitStage stage) {
      if (stage == DurableArtifactCommitStage::ManifestPublished &&
          manifest_failure_armed.exchange(false, std::memory_order_acq_rel)) {
        throw std::runtime_error(
            "injected manifest-visible acknowledgement failure");
      }
      if (stage == DurableArtifactCommitStage::RootDirectoryBarrierReplay) {
        replay_attempts.fetch_add(1U, std::memory_order_relaxed);
        throw std::runtime_error("injected repeated durability replay failure");
      }
    };
    SingleTenantJobService service(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), factory, std::move(options));
    submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                        OutputSlotId("image.final"),
                                        test_job_resources()));
    ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                    wait_for_owned_worker_thread_count_at_most(
                        service, 0U, std::chrono::seconds(2)));

    const std::optional<JobSnapshot> pending = service.query(submission.job_id);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->state, JobState::Running);
    EXPECT_FALSE(pending->output_receipt.has_value());
    EXPECT_TRUE(
        SingleTenantJobServiceTestAccess::artifact_reconciliation_faulted(
            service));
    EXPECT_TRUE(
        SingleTenantJobServiceTestAccess::durable_mutation_faulted(service));
    EXPECT_FALSE(SingleTenantJobServiceTestAccess::journal_faulted(service));
    EXPECT_GE(replay_attempts.load(std::memory_order_relaxed), 1U);
    EXPECT_THROW(service.find_artifact(pending->output_artifact_id),
                 std::runtime_error);

    const TenantQuotaSnapshot usage = service.quota_snapshot();
    EXPECT_EQ(usage.active_attempts, 1U);
    EXPECT_EQ(usage.retention_bytes,
              pending->spec->resource_request().retention_bytes);
    EXPECT_EQ(usage.retained_artifacts, 0U);

    JobAssignment assignment{pending->assignment, pending->spec, nullptr};
    SingleTenantJobServiceTestAccess::inject_attempt_report(
        service, pending->assignment, successful_report(assignment));
    const std::optional<JobSnapshot> fenced = service.query(submission.job_id);
    ASSERT_TRUE(fenced.has_value());
    EXPECT_EQ(fenced->state, JobState::Running);
    EXPECT_EQ(service.quota_snapshot().active_attempts, 1U);
    EXPECT_THROW(service.submit(JobSpec(GraphArtifactId("graph.test"), 8,
                                        OutputSlotId("image.final"),
                                        test_job_resources())),
                 DurableStateError);
  }

  SingleTenantJobService recovered(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), factory);
  const std::optional<JobSnapshot> succeeded =
      recovered.query(submission.job_id);
  ASSERT_TRUE(succeeded.has_value());
  ASSERT_EQ(succeeded->state, JobState::Succeeded);
  ASSERT_TRUE(succeeded->output_receipt.has_value());
  EXPECT_FALSE(
      SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(recovered.quota_snapshot().retention_bytes, 12U);
  EXPECT_EQ(recovered.quota_snapshot().retained_artifacts, 1U);
  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 1U);
}

TEST(SingleTenantJobService,
     QuotaConversionFailureKeepsReservationAndRestartReconcilesSuccess) {
  ScopedTestStateRoot root;
  std::atomic<std::size_t> worker_calls{0U};
  std::atomic<std::size_t> conversion_attempts{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&worker_calls](const JobAssignment& assignment,
                      const std::function<bool()>& cancellation_requested) {
        worker_calls.fetch_add(1U, std::memory_order_relaxed);
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  JobSubmission submission;
  {
    TenantQuotaAuthorityOptions quota_options;
    quota_options.retained_artifact_commit_observer = [&conversion_attempts] {
      conversion_attempts.fetch_add(1U, std::memory_order_relaxed);
      throw std::runtime_error("injected quota conversion failure");
    };
    SingleTenantJobService service(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), factory, {},
                                   std::move(quota_options));
    submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                        OutputSlotId("image.final"),
                                        test_job_resources()));
    ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                    wait_for_owned_worker_thread_count_at_most(
                        service, 0U, std::chrono::seconds(2)));

    const std::optional<JobSnapshot> pending = service.query(submission.job_id);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->state, JobState::Running);
    EXPECT_FALSE(pending->output_receipt.has_value());
    EXPECT_EQ(conversion_attempts.load(std::memory_order_relaxed), 2U);
    EXPECT_TRUE(
        SingleTenantJobServiceTestAccess::artifact_reconciliation_faulted(
            service));
    EXPECT_FALSE(SingleTenantJobServiceTestAccess::journal_faulted(service));
    EXPECT_NE(service.find_artifact(pending->output_artifact_id), nullptr);

    const TenantQuotaSnapshot usage = service.quota_snapshot();
    EXPECT_EQ(usage.active_attempts, 1U);
    EXPECT_EQ(usage.retention_bytes,
              pending->spec->resource_request().retention_bytes);
    EXPECT_EQ(usage.retained_artifacts, 0U);

    JobAssignment assignment{pending->assignment, pending->spec, nullptr};
    SingleTenantJobServiceTestAccess::inject_attempt_report(
        service, pending->assignment, successful_report(assignment));
    EXPECT_EQ(conversion_attempts.load(std::memory_order_relaxed), 2U);
    EXPECT_EQ(service.query(submission.job_id)->state, JobState::Running);
    EXPECT_EQ(service.quota_snapshot().active_attempts, 1U);
  }

  SingleTenantJobService recovered(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), factory);
  const std::optional<JobSnapshot> succeeded =
      recovered.query(submission.job_id);
  ASSERT_TRUE(succeeded.has_value());
  ASSERT_EQ(succeeded->state, JobState::Succeeded);
  ASSERT_TRUE(succeeded->output_receipt.has_value());
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(recovered.quota_snapshot().retention_bytes, 12U);
  EXPECT_EQ(recovered.quota_snapshot().retained_artifacts, 1U);
  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 1U);
}

TEST(SingleTenantJobService,
     TwoSucceededJournalPrePublicationFailuresPreserveRetainedTruth) {
  ScopedTestStateRoot root;
  auto gate = std::make_shared<WorkerGate>();
  std::atomic<std::size_t> worker_calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [gate, &worker_calls](
          const JobAssignment& assignment,
          const std::function<bool()>& cancellation_requested) {
        worker_calls.fetch_add(1U, std::memory_order_relaxed);
        gate->enter_and_wait();
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  std::atomic<std::size_t> remaining_failures{0U};
  std::atomic<std::size_t> observed_failures{0U};
  JobSubmission submission;
  {
    DurableServerStateOptions options;
    options.job_commit_observer =
        [&remaining_failures, &observed_failures](DurableJobCommitStage stage) {
          if (stage != DurableJobCommitStage::PrivateFileSynchronized) {
            return;
          }
          std::size_t remaining =
              remaining_failures.load(std::memory_order_acquire);
          while (remaining != 0U &&
                 !remaining_failures.compare_exchange_weak(
                     remaining, remaining - 1U, std::memory_order_acq_rel,
                     std::memory_order_acquire)) {
          }
          if (remaining != 0U) {
            observed_failures.fetch_add(1U, std::memory_order_relaxed);
            throw std::runtime_error(
                "injected Succeeded journal pre-publication failure");
          }
        };
    SingleTenantJobService service(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), factory, std::move(options));
    submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                        OutputSlotId("image.final"),
                                        test_job_resources()));
    ASSERT_TRUE(gate->wait_until_entered());
    remaining_failures.store(2U, std::memory_order_release);
    gate->release();
    ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                    wait_for_owned_worker_thread_count_at_most(
                        service, 0U, std::chrono::seconds(2)));

    EXPECT_EQ(observed_failures.load(std::memory_order_relaxed), 2U);
    EXPECT_EQ(remaining_failures.load(std::memory_order_relaxed), 0U);
    const std::optional<JobSnapshot> pending = service.query(submission.job_id);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->state, JobState::Running);
    EXPECT_FALSE(pending->output_receipt.has_value());
    EXPECT_TRUE(
        SingleTenantJobServiceTestAccess::artifact_reconciliation_faulted(
            service));
    EXPECT_FALSE(SingleTenantJobServiceTestAccess::journal_faulted(service));

    const TenantQuotaSnapshot usage = service.quota_snapshot();
    EXPECT_EQ(usage.active_attempts, 0U);
    EXPECT_EQ(usage.retention_bytes, 12U);
    EXPECT_EQ(usage.retained_artifacts, 1U);

    JobAssignment assignment{pending->assignment, pending->spec, nullptr};
    SingleTenantJobServiceTestAccess::inject_attempt_report(
        service, pending->assignment, successful_report(assignment));
    EXPECT_EQ(observed_failures.load(std::memory_order_relaxed), 2U);
    EXPECT_EQ(service.query(submission.job_id)->state, JobState::Running);
    EXPECT_EQ(service.quota_snapshot().retention_bytes, 12U);
  }

  auto unused_factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  SingleTenantJobService recovered(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), unused_factory);
  const std::optional<JobSnapshot> succeeded =
      recovered.query(submission.job_id);
  ASSERT_TRUE(succeeded.has_value());
  ASSERT_EQ(succeeded->state, JobState::Succeeded);
  ASSERT_TRUE(succeeded->output_receipt.has_value());
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(recovered.quota_snapshot().retention_bytes, 12U);
  EXPECT_EQ(recovered.quota_snapshot().retained_artifacts, 1U);
  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 1U);
}

TEST(SingleTenantJobService,
     PreservesSettlementAndRetriesAfterPreManifestCommitFailure) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  ScopedTestStateRoot root;
  std::atomic<std::size_t> payload_observations{0U};
  DurableServerStateOptions options;
  options.artifact_commit_observer = [&payload_observations](
                                         DurableArtifactCommitStage stage) {
    if (stage == DurableArtifactCommitStage::PayloadSynchronized &&
        payload_observations.fetch_add(1U, std::memory_order_relaxed) == 0U) {
      throw std::runtime_error("injected pre-manifest commit failure");
    }
  };
  SingleTenantJobService service(TenantId("tenant.test"), test_quota_limits(),
                                 root.path(), factory, std::move(options));
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> failed =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(failed.has_value());
  ASSERT_EQ(failed->state, JobState::Failed);
  EXPECT_EQ(failed->attempt_outcome, JobAttemptOutcome::Succeeded);
  EXPECT_TRUE(failed->attempt_settled);
  EXPECT_EQ(failed->failure, JobAttemptFailure::ArtifactCommit);
  EXPECT_FALSE(failed->output_receipt.has_value());
  EXPECT_EQ(service.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(service.quota_snapshot().retention_bytes, 0U);
  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));

  const std::optional<JobSubmission> retry = service.retry(submission.job_id);
  ASSERT_TRUE(retry.has_value());
  const std::optional<JobSnapshot> succeeded =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  ASSERT_EQ(succeeded->state, JobState::Succeeded);
  EXPECT_EQ(succeeded->output_artifact_id, failed->output_artifact_id);
  EXPECT_EQ(succeeded->output_commit_id, failed->output_commit_id);
  ASSERT_TRUE(succeeded->output_receipt.has_value());
  EXPECT_EQ(succeeded->output_receipt->attempt, retry->assignment);
  EXPECT_EQ(service.quota_snapshot().retention_bytes, 12U);
}

TEST(SingleTenantJobService,
     RestoresSucceededTruthAndRetentionAcrossRepeatedRestartAndDeletion) {
  ScopedTestStateRoot root;
  auto success_factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  JobId job_id;
  OutputCommitReceipt receipt;
  {
    SingleTenantJobService service(TenantId("tenant.test"), test_quota_limits(),
                                   root.path(), success_factory);
    const JobSubmission submission = service.submit(
        JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                test_job_resources()));
    job_id = submission.job_id;
    const std::optional<JobSnapshot> terminal =
        service.wait_for(job_id, std::chrono::seconds(2));
    ASSERT_TRUE(terminal.has_value());
    ASSERT_TRUE(terminal->output_receipt.has_value());
    receipt = *terminal->output_receipt;
  }

  std::atomic<std::size_t> recovered_worker_calls{0U};
  auto unused_factory = std::make_shared<FunctionWorkerFactory>(
      [&recovered_worker_calls](
          const JobAssignment& assignment,
          const std::function<bool()>& cancellation_requested) {
        recovered_worker_calls.fetch_add(1U);
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  {
    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(),
                                     unused_factory);
    const std::optional<JobSnapshot> snapshot = recovered.query(job_id);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->state, JobState::Succeeded);
    ASSERT_TRUE(snapshot->output_receipt.has_value());
    EXPECT_EQ(snapshot->output_receipt->artifact_id, receipt.artifact_id);
    EXPECT_EQ(snapshot->output_receipt->content_digest, receipt.content_digest);
    EXPECT_EQ(recovered.quota_snapshot().retention_bytes, 12U);
    EXPECT_TRUE(recovered.delete_artifact(receipt.artifact_id));
    EXPECT_EQ(recovered.quota_snapshot().retention_bytes, 0U);
  }
  {
    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(),
                                     unused_factory);
    const std::optional<JobSnapshot> snapshot = recovered.query(job_id);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->state, JobState::Succeeded);
    ASSERT_TRUE(snapshot->output_receipt.has_value());
    EXPECT_EQ(snapshot->output_receipt->artifact_id, receipt.artifact_id);
    EXPECT_EQ(recovered.find_artifact(receipt.artifact_id), nullptr);
    EXPECT_EQ(recovered.quota_snapshot().retention_bytes, 0U);
  }
  EXPECT_EQ(recovered_worker_calls.load(), 0U);
}

TEST(SingleTenantJobService,
     EraseFaultMatrixCoordinatesQuotaAliasesFailStopAndRestart) {
  constexpr std::array<DurableArtifactEraseStage, 12U> stages{
      DurableArtifactEraseStage::BeforeManifestRemoval,
      DurableArtifactEraseStage::ManifestRemoved,
      DurableArtifactEraseStage::ArtifactDirectorySynchronized,
      DurableArtifactEraseStage::ArtifactsDirectorySynchronized,
      DurableArtifactEraseStage::VisibilityRemovalConfirmed,
      DurableArtifactEraseStage::PayloadRemoved,
      DurableArtifactEraseStage::PrivateManifestRemoved,
      DurableArtifactEraseStage::ArtifactDirectoryCleanupSynchronized,
      DurableArtifactEraseStage::ArtifactDirectoryClosed,
      DurableArtifactEraseStage::ArtifactDirectoryRemoved,
      DurableArtifactEraseStage::ArtifactsDirectoryCleanupSynchronized,
      DurableArtifactEraseStage::CleanupBarriersCompleted};
  constexpr std::array<DurableArtifactEraseState, 12U> expected_states{
      DurableArtifactEraseState::NotRemoved,
      DurableArtifactEraseState::ManifestRemovedDurabilityUnconfirmed,
      DurableArtifactEraseState::ManifestRemovedDurabilityUnconfirmed,
      DurableArtifactEraseState::ManifestRemovedDurabilityUnconfirmed,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::VisibilityRemovalConfirmedCleanupPending,
      DurableArtifactEraseState::FullyCleaned};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });

  for (std::size_t index = 0U; index < stages.size(); ++index) {
    SCOPED_TRACE(index);
    ScopedTestStateRoot root;
    const auto failure =
        std::make_shared<OneShotArtifactEraseFailure>(stages[index]);
    JobId job_id;
    OutputCommitReceipt receipt;
    {
      SingleTenantJobService service(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(), factory,
                                     artifact_erase_failure_options(failure));
      const JobSubmission submission = service.submit(
          JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                  test_job_resources()));
      job_id = submission.job_id;
      const std::optional<JobSnapshot> terminal =
          service.wait_for(job_id, std::chrono::seconds(2));
      ASSERT_TRUE(terminal.has_value());
      ASSERT_EQ(terminal->state, JobState::Succeeded);
      ASSERT_TRUE(terminal->output_receipt.has_value());
      receipt = *terminal->output_receipt;
      ASSERT_EQ(service.quota_snapshot().retention_bytes, 12U);
      failure->arm();

      if (stages[index] == DurableArtifactEraseStage::BeforeManifestRemoval) {
        EXPECT_THROW(service.delete_artifact(receipt.artifact_id),
                     std::runtime_error);
        EXPECT_FALSE(
            SingleTenantJobServiceTestAccess::artifact_erase_faulted(service));
        EXPECT_FALSE(SingleTenantJobServiceTestAccess::durable_mutation_faulted(
            service));
        EXPECT_EQ(service.quota_snapshot().retention_bytes, 12U);
        EXPECT_NE(service.find_artifact(receipt.artifact_id), nullptr);
        EXPECT_TRUE(service.delete_artifact(receipt.artifact_id));
        EXPECT_EQ(service.quota_snapshot().retention_bytes, 0U);
      } else {
        bool caught = false;
        try {
          static_cast<void>(service.delete_artifact(receipt.artifact_id));
        } catch (const DurableArtifactEraseError& error) {
          caught = true;
          EXPECT_EQ(error.artifact_id(), receipt.artifact_id);
          EXPECT_EQ(error.state(), expected_states[index]);
          EXPECT_EQ(error.payload_bytes(), 12U);
          EXPECT_THROW(error.rethrow_cause(), std::runtime_error);
        }
        EXPECT_TRUE(caught);
        EXPECT_TRUE(
            SingleTenantJobServiceTestAccess::artifact_erase_faulted(service));
        EXPECT_TRUE(SingleTenantJobServiceTestAccess::durable_mutation_faulted(
            service));
        EXPECT_FALSE(
            SingleTenantJobServiceTestAccess::journal_faulted(service));
        EXPECT_EQ(service.find_artifact(receipt.artifact_id), nullptr);
        const std::uint64_t expected_retention = index <= 3U ? 12U : 0U;
        EXPECT_EQ(service.quota_snapshot().retention_bytes, expected_retention);
        EXPECT_THROW(service.submit(JobSpec(GraphArtifactId("graph.test"), 8,
                                            OutputSlotId("image.final"),
                                            test_job_resources())),
                     DurableStateError);
        EXPECT_THROW(service.delete_artifact(receipt.artifact_id),
                     DurableStateError);
      }

      const std::optional<JobSnapshot> historical = service.query(job_id);
      ASSERT_TRUE(historical.has_value());
      EXPECT_EQ(historical->state, JobState::Succeeded);
      ASSERT_TRUE(historical->output_receipt.has_value());
      EXPECT_EQ(historical->output_receipt->artifact_id, receipt.artifact_id);
    }

    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(), factory);
    const std::optional<JobSnapshot> historical = recovered.query(job_id);
    ASSERT_TRUE(historical.has_value());
    EXPECT_EQ(historical->state, JobState::Succeeded);
    ASSERT_TRUE(historical->output_receipt.has_value());
    EXPECT_EQ(historical->output_receipt->artifact_id, receipt.artifact_id);
    EXPECT_EQ(recovered.find_artifact(receipt.artifact_id), nullptr);
    EXPECT_EQ(recovered.quota_snapshot().retention_bytes, 0U);
    EXPECT_FALSE(
        SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));
  }
}

TEST(SingleTenantJobService,
     ConvertsInterruptedDurableAttemptToRetryableRecoveryFailure) {
  ScopedTestStateRoot root;
  const auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
      test_job_resources());
  AttemptIdentity identity = make_test_identity(6U);
  identity.job_spec_digest = spec->digest();
  DurableJobRecord interrupted;
  interrupted.tenant_id = identity.tenant_id;
  interrupted.job_id = identity.job_id;
  interrupted.spec = spec;
  interrupted.assignment = identity;
  interrupted.output_artifact_id = ArtifactId("artifact.test.interrupted");
  interrupted.output_commit_id = OutputCommitId("commit.test.interrupted");
  interrupted.state = JobState::Running;
  {
    DurableServerState store(root.path(), TenantId("tenant.test"));
    EXPECT_TRUE(store.persist_job(interrupted).succeeded());
  }

  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  SingleTenantJobService service(TenantId("tenant.test"), test_quota_limits(),
                                 root.path(), factory);
  const std::optional<JobSnapshot> recovered = service.query(identity.job_id);
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->state, JobState::Failed);
  EXPECT_TRUE(recovered->attempt_settled);
  EXPECT_EQ(recovered->attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(recovered->failure, JobAttemptFailure::RecoveryInterrupted);

  const std::optional<JobSubmission> retry = service.retry(identity.job_id);
  ASSERT_TRUE(retry.has_value());
  EXPECT_EQ(retry->job_id, identity.job_id);
  EXPECT_NE(retry->assignment.attempt_id, identity.attempt_id);
  EXPECT_EQ(retry->assignment.worker_lease_generation.value,
            identity.worker_lease_generation.value + 1U);
  const std::optional<JobSnapshot> terminal =
      service.wait_for(identity.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Succeeded);
}

TEST(SingleTenantJobService,
     RecoveryRejectsStableArtifactJoinedToDifferentJobIdentity) {
  ScopedTestStateRoot root;
  const auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
      test_job_resources());
  AttemptIdentity artifact_identity = make_test_identity(70U);
  artifact_identity.job_spec_digest = spec->digest();
  const ArtifactId stable_artifact("artifact.test.conflicting-join");
  const OutputCommitId stable_commit("commit.test.conflicting-join");
  {
    DurableServerState store(root.path(), TenantId("tenant.test"));
    ImageBuffer image = make_test_image();
    static_cast<void>(store.commit_artifact(
        DurableArtifactCommitRequest{
            artifact_identity, OutputSlotId("image.final"), stable_artifact,
            stable_commit, test_job_resources()},
        image));

    AttemptIdentity job_identity = make_test_identity(71U);
    job_identity.job_spec_digest = spec->digest();
    DurableJobRecord running;
    running.tenant_id = job_identity.tenant_id;
    running.job_id = job_identity.job_id;
    running.spec = spec;
    running.assignment = job_identity;
    running.output_artifact_id = stable_artifact;
    running.output_commit_id = stable_commit;
    running.state = JobState::Running;
    EXPECT_TRUE(store.persist_job(running).succeeded());
  }

  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  EXPECT_THROW(
      SingleTenantJobService(TenantId("tenant.test"), test_quota_limits(),
                             root.path(), factory),
      DurableCorruptionError);
}

TEST(SingleTenantJobService,
     SubmitJournalFailureRollsBackBeforePublishAndFailStopsAfterPublish) {
  const std::array<DurableJobCommitStage, 3U> stages{
      DurableJobCommitStage::PrivateFileSynchronized,
      DurableJobCommitStage::RecordPublished,
      DurableJobCommitStage::DirectoryBarriersCompleted};
  for (const DurableJobCommitStage stage : stages) {
    SCOPED_TRACE(static_cast<int>(stage));
    ScopedTestStateRoot root;
    auto failure = std::make_shared<OneShotJobCommitFailure>(stage);
    std::atomic<std::size_t> worker_calls{0U};
    auto factory = std::make_shared<FunctionWorkerFactory>(
        [&worker_calls](const JobAssignment& assignment,
                        const std::function<bool()>& cancellation_requested) {
          worker_calls.fetch_add(1U, std::memory_order_relaxed);
          EXPECT_FALSE(cancellation_requested());
          return successful_report(assignment);
        });
    JobId published_job;
    {
      SingleTenantJobService service(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(), factory,
                                     job_failure_options(failure));
      failure->arm();
      try {
        static_cast<void>(service.submit(JobSpec(GraphArtifactId("graph.test"),
                                                 7, OutputSlotId("image.final"),
                                                 test_job_resources())));
        ADD_FAILURE() << "injected Job journal failure was not reported";
      } catch (const DurableJobCommitError& error) {
        EXPECT_NE(stage, DurableJobCommitStage::PrivateFileSynchronized);
        EXPECT_EQ(
            error.state(),
            stage == DurableJobCommitStage::DirectoryBarriersCompleted
                ? DurableJobCommitState::ConfirmedCommitted
                : DurableJobCommitState::RecordPublishedDurabilityUnconfirmed);
        published_job = error.job_id();
      } catch (const std::runtime_error&) {
        EXPECT_EQ(stage, DurableJobCommitStage::PrivateFileSynchronized);
      }

      EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                      wait_for_owned_worker_thread_count_at_most(
                          service, 0U, std::chrono::seconds(2)));
      EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 0U);
      const bool expected_published =
          stage != DurableJobCommitStage::PrivateFileSynchronized;
      EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service),
                expected_published ? 1U : 0U);
      EXPECT_EQ(SingleTenantJobServiceTestAccess::journal_faulted(service),
                expected_published);
      EXPECT_EQ(service.quota_snapshot().active_attempts,
                expected_published ? 1U : 0U);
      if (expected_published) {
        const std::optional<JobSnapshot> snapshot =
            service.query(published_job);
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->state, JobState::Queued);
        EXPECT_THROW(service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                            OutputSlotId("image.final"),
                                            test_job_resources())),
                     DurableStateError);
      }
    }

    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(), factory);
    if (stage != DurableJobCommitStage::PrivateFileSynchronized) {
      const std::optional<JobSnapshot> snapshot =
          recovered.query(published_job);
      ASSERT_TRUE(snapshot.has_value());
      EXPECT_EQ(snapshot->state, JobState::Failed);
      EXPECT_EQ(snapshot->failure, JobAttemptFailure::RecoveryInterrupted);
    } else {
      EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(recovered),
                0U);
    }
    EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  }
}

TEST(SingleTenantJobService,
     RetryJournalFailurePreservesOldOrPublishesNewAttemptAcrossRestart) {
  const std::array<DurableJobCommitStage, 3U> stages{
      DurableJobCommitStage::PrivateFileSynchronized,
      DurableJobCommitStage::RecordPublished,
      DurableJobCommitStage::DirectoryBarriersCompleted};
  for (const DurableJobCommitStage stage : stages) {
    SCOPED_TRACE(static_cast<int>(stage));
    ScopedTestStateRoot root;
    auto failure = std::make_shared<OneShotJobCommitFailure>(stage);
    std::atomic<std::size_t> worker_calls{0U};
    auto factory = std::make_shared<FunctionWorkerFactory>(
        [&worker_calls](const JobAssignment& assignment,
                        const std::function<bool()>& cancellation_requested) {
          EXPECT_FALSE(cancellation_requested());
          if (worker_calls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
            return settled_failed_report(assignment);
          }
          return successful_report(assignment);
        });
    JobSubmission submission;
    AttemptIdentity original_assignment;
    AttemptIdentity observed_assignment;
    {
      SingleTenantJobService service(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(), factory,
                                     job_failure_options(failure));
      submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                          OutputSlotId("image.final"),
                                          test_job_resources()));
      const std::optional<JobSnapshot> initial =
          service.wait_for(submission.job_id, std::chrono::seconds(2));
      ASSERT_TRUE(initial.has_value());
      ASSERT_EQ(initial->state, JobState::Failed);
      original_assignment = initial->assignment;
      ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                      wait_for_owned_worker_thread_count_at_most(
                          service, 0U, std::chrono::seconds(2)));

      failure->arm();
      try {
        static_cast<void>(service.retry(submission.job_id));
        ADD_FAILURE() << "injected retry journal failure was not reported";
      } catch (const DurableJobCommitError& error) {
        EXPECT_NE(stage, DurableJobCommitStage::PrivateFileSynchronized);
        EXPECT_EQ(error.job_id(), submission.job_id);
      } catch (const std::runtime_error&) {
        EXPECT_EQ(stage, DurableJobCommitStage::PrivateFileSynchronized);
      }
      ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                      wait_for_owned_worker_thread_count_at_most(
                          service, 0U, std::chrono::seconds(2)));
      EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 1U);
      const std::optional<JobSnapshot> current =
          service.query(submission.job_id);
      ASSERT_TRUE(current.has_value());
      observed_assignment = current->assignment;
      const bool expected_published =
          stage != DurableJobCommitStage::PrivateFileSynchronized;
      EXPECT_EQ(current->assignment == original_assignment,
                !expected_published);
      EXPECT_EQ(current->state,
                expected_published ? JobState::Queued : JobState::Failed);
      EXPECT_EQ(service.quota_snapshot().active_attempts,
                expected_published ? 1U : 0U);
      EXPECT_EQ(SingleTenantJobServiceTestAccess::journal_faulted(service),
                expected_published);
    }

    auto unused_factory = std::make_shared<FunctionWorkerFactory>(
        [](const JobAssignment& assignment,
           const std::function<bool()>& cancellation_requested) {
          EXPECT_FALSE(cancellation_requested());
          return successful_report(assignment);
        });
    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(),
                                     unused_factory);
    const std::optional<JobSnapshot> snapshot =
        recovered.query(submission.job_id);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->state, JobState::Failed);
    if (stage != DurableJobCommitStage::PrivateFileSynchronized) {
      EXPECT_EQ(snapshot->assignment, observed_assignment);
      EXPECT_EQ(snapshot->failure, JobAttemptFailure::RecoveryInterrupted);
    } else {
      EXPECT_EQ(snapshot->assignment, original_assignment);
      EXPECT_EQ(snapshot->failure, JobAttemptFailure::Compute);
    }
    EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  }
}

TEST(SingleTenantJobService,
     CancelJournalFailurePreservesIntentBoundaryAndRestartTruth) {
  const std::array<DurableJobCommitStage, 3U> stages{
      DurableJobCommitStage::PrivateFileSynchronized,
      DurableJobCommitStage::RecordPublished,
      DurableJobCommitStage::DirectoryBarriersCompleted};
  for (const DurableJobCommitStage stage : stages) {
    SCOPED_TRACE(static_cast<int>(stage));
    ScopedTestStateRoot root;
    auto failure = std::make_shared<OneShotJobCommitFailure>(stage);
    auto gate = std::make_shared<WorkerGate>();
    auto factory = std::make_shared<FunctionWorkerFactory>(
        [gate](const JobAssignment& assignment,
               const std::function<bool()>& cancellation_requested) {
          gate->enter_and_wait();
          if (cancellation_requested()) {
            JobAttemptReport report;
            report.identity = assignment.identity;
            report.outcome = JobAttemptOutcome::Cancelled;
            report.settled = true;
            report.failure = JobAttemptFailure::CancellationObserved;
            return report;
          }
          return successful_report(assignment);
        });
    JobId job_id;
    {
      SingleTenantJobService service(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(), factory,
                                     job_failure_options(failure));
      WorkerGateReleaseGuard gate_release(gate);
      const JobSubmission submission = service.submit(
          JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                  test_job_resources()));
      job_id = submission.job_id;
      ASSERT_TRUE(gate->wait_until_entered());
      failure->arm();
      try {
        static_cast<void>(service.cancel(job_id));
        ADD_FAILURE() << "injected cancel journal failure was not reported";
      } catch (const DurableJobCommitError& error) {
        EXPECT_NE(stage, DurableJobCommitStage::PrivateFileSynchronized);
        EXPECT_EQ(error.job_id(), job_id);
      } catch (const std::runtime_error&) {
        EXPECT_EQ(stage, DurableJobCommitStage::PrivateFileSynchronized);
      }
      const std::optional<JobSnapshot> current = service.query(job_id);
      ASSERT_TRUE(current.has_value());
      const bool expected_published =
          stage != DurableJobCommitStage::PrivateFileSynchronized;
      EXPECT_EQ(current->cancellation_requested, expected_published);
      EXPECT_EQ(current->state,
                expected_published ? JobState::Cancelling : JobState::Running);
      EXPECT_EQ(SingleTenantJobServiceTestAccess::journal_faulted(service),
                expected_published);
      EXPECT_EQ(service.quota_snapshot().active_attempts, 1U);
      gate_release.release();
      EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                      wait_for_owned_worker_thread_count_at_most(
                          service, 0U, std::chrono::seconds(2)));
      if (!expected_published) {
        const std::optional<JobSnapshot> terminal =
            service.wait_for(job_id, std::chrono::seconds(2));
        ASSERT_TRUE(terminal.has_value());
        EXPECT_EQ(terminal->state, JobState::Succeeded);
      }
    }

    auto unused_factory = std::make_shared<FunctionWorkerFactory>(
        [](const JobAssignment& assignment,
           const std::function<bool()>& cancellation_requested) {
          EXPECT_FALSE(cancellation_requested());
          return successful_report(assignment);
        });
    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(),
                                     unused_factory);
    const std::optional<JobSnapshot> snapshot = recovered.query(job_id);
    ASSERT_TRUE(snapshot.has_value());
    if (stage != DurableJobCommitStage::PrivateFileSynchronized) {
      EXPECT_EQ(snapshot->state, JobState::Failed);
      EXPECT_EQ(snapshot->failure, JobAttemptFailure::RecoveryInterrupted);
    } else {
      EXPECT_EQ(snapshot->state, JobState::Succeeded);
    }
    EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  }
}

TEST(SingleTenantJobService,
     SubmitRollbackReleaseFailureRetainsStrandedOwnerAndRestartClearsQuota) {
  constexpr std::array<bool, 2U> kNotPublishedModes{false, true};
  for (const bool fail_not_published : kNotPublishedModes) {
    SCOPED_TRACE(::testing::Message()
                 << "not_published=" << fail_not_published);
    ScopedTestStateRoot root;
    const auto release_failure = std::make_shared<OneShotQuotaReleaseFailure>();
    const auto journal_failure = std::make_shared<OneShotJobCommitFailure>(
        DurableJobCommitStage::PrivateFileSynchronized);
    DurableServerStateOptions state_options;
    if (fail_not_published) {
      state_options = job_failure_options(journal_failure);
    }
    std::atomic<std::size_t> worker_calls{0U};
    auto factory = std::make_shared<FunctionWorkerFactory>(
        [&worker_calls](const JobAssignment& assignment,
                        const std::function<bool()>& cancellation_requested) {
          worker_calls.fetch_add(1U, std::memory_order_relaxed);
          EXPECT_FALSE(cancellation_requested());
          return successful_report(assignment);
        });
    JobId probe_job_id("job.test.submit-release-fault");
    {
      SingleTenantJobService service(
          TenantId("tenant.test"), test_quota_limits(), root.path(), factory,
          std::move(state_options),
          quota_release_failure_options(release_failure));
      release_failure->arm();
      if (fail_not_published) {
        journal_failure->arm();
        try {
          static_cast<void>(service.submit(
              JobSpec(GraphArtifactId("graph.test"), 7,
                      OutputSlotId("image.final"), test_job_resources())));
          ADD_FAILURE() << "injected submit journal failure was not reported";
        } catch (const std::runtime_error& error) {
          EXPECT_STREQ(error.what(), "injected one-shot Job journal failure");
        }
      } else {
        ScopedWorkerThreadStartFailure start_failure;
        EXPECT_THROW(service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                            OutputSlotId("image.final"),
                                            test_job_resources())),
                     std::system_error);
        ASSERT_TRUE(start_failure.attempted_job_id().has_value());
        probe_job_id = *start_failure.attempted_job_id();
      }

      EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                      wait_for_owned_worker_thread_count_at_most(
                          service, 0U, std::chrono::seconds(2)));
      EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 0U);
      EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service),
                0U);
      expect_quota_release_fail_stop(service, probe_job_id, false, 0U, 1U);
    }

    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(), factory);
    EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(recovered),
              0U);
    EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::quota_reservation_ownership(recovered)
            .total(),
        0U);
    EXPECT_FALSE(
        SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));
    const JobSubmission accepted = recovered.submit(
        JobSpec(GraphArtifactId("graph.test"), 8, OutputSlotId("image.final"),
                test_job_resources()));
    const std::optional<JobSnapshot> terminal =
        recovered.wait_for(accepted.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->state, JobState::Succeeded);
    EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 1U);
  }
}

TEST(SingleTenantJobService,
     RetryRollbackReleaseFailureKeepsPriorTruthAndRestartAllowsRetry) {
  constexpr std::array<bool, 2U> kNotPublishedModes{false, true};
  for (const bool fail_not_published : kNotPublishedModes) {
    SCOPED_TRACE(::testing::Message()
                 << "not_published=" << fail_not_published);
    ScopedTestStateRoot root;
    const auto release_failure = std::make_shared<OneShotQuotaReleaseFailure>();
    const auto journal_failure = std::make_shared<OneShotJobCommitFailure>(
        DurableJobCommitStage::PrivateFileSynchronized);
    DurableServerStateOptions state_options;
    if (fail_not_published) {
      state_options = job_failure_options(journal_failure);
    }
    std::atomic<std::size_t> worker_calls{0U};
    auto factory = std::make_shared<FunctionWorkerFactory>(
        [&worker_calls](const JobAssignment& assignment,
                        const std::function<bool()>& cancellation_requested) {
          EXPECT_FALSE(cancellation_requested());
          if (worker_calls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
            return settled_failed_report(assignment);
          }
          return successful_report(assignment);
        });
    JobSubmission submission;
    JobSnapshot prior_failure;
    {
      SingleTenantJobService service(
          TenantId("tenant.test"), test_quota_limits(), root.path(), factory,
          std::move(state_options),
          quota_release_failure_options(release_failure));
      submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                          OutputSlotId("image.final"),
                                          test_job_resources()));
      const std::optional<JobSnapshot> failed =
          service.wait_for(submission.job_id, std::chrono::seconds(2));
      ASSERT_TRUE(failed.has_value());
      ASSERT_EQ(failed->state, JobState::Failed);
      prior_failure = *failed;
      ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                      wait_for_owned_worker_thread_count_at_most(
                          service, 0U, std::chrono::seconds(2)));

      release_failure->arm();
      if (fail_not_published) {
        journal_failure->arm();
        try {
          static_cast<void>(service.retry(submission.job_id));
          ADD_FAILURE() << "injected retry journal failure was not reported";
        } catch (const std::runtime_error& error) {
          EXPECT_STREQ(error.what(), "injected one-shot Job journal failure");
        }
      } else {
        ScopedWorkerThreadStartFailure start_failure;
        EXPECT_THROW(static_cast<void>(service.retry(submission.job_id)),
                     std::system_error);
        ASSERT_TRUE(start_failure.attempted_job_id().has_value());
        EXPECT_EQ(*start_failure.attempted_job_id(), submission.job_id);
      }

      ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                      wait_for_owned_worker_thread_count_at_most(
                          service, 0U, std::chrono::seconds(2)));
      const std::optional<JobSnapshot> unchanged =
          service.query(submission.job_id);
      ASSERT_TRUE(unchanged.has_value());
      EXPECT_EQ(unchanged->state, prior_failure.state);
      EXPECT_EQ(unchanged->assignment, prior_failure.assignment);
      EXPECT_EQ(unchanged->attempt_settled, prior_failure.attempt_settled);
      EXPECT_EQ(unchanged->attempt_outcome, prior_failure.attempt_outcome);
      EXPECT_EQ(unchanged->failure, prior_failure.failure);
      EXPECT_EQ(unchanged->message, prior_failure.message);
      EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 1U);
      expect_quota_release_fail_stop(service, submission.job_id, true, 0U, 1U);
    }

    SingleTenantJobService recovered(TenantId("tenant.test"),
                                     test_quota_limits(), root.path(), factory);
    const std::optional<JobSnapshot> durable =
        recovered.query(submission.job_id);
    ASSERT_TRUE(durable.has_value());
    EXPECT_EQ(durable->state, prior_failure.state);
    EXPECT_EQ(durable->assignment, prior_failure.assignment);
    EXPECT_EQ(durable->failure, prior_failure.failure);
    EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
    EXPECT_FALSE(
        SingleTenantJobServiceTestAccess::durable_mutation_faulted(recovered));
    const std::optional<JobSubmission> retried =
        recovered.retry(submission.job_id);
    ASSERT_TRUE(retried.has_value());
    const std::optional<JobSnapshot> succeeded =
        recovered.wait_for(submission.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(succeeded.has_value());
    EXPECT_EQ(succeeded->state, JobState::Succeeded);
    EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 2U);
  }
}

TEST(SingleTenantJobService,
     ManagerThreadStartFailureErasesInsertedRecordAndRollsBackSubmission) {
  std::atomic<std::size_t> worker_calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&worker_calls](const JobAssignment& assignment,
                      const std::function<bool()>& cancellation_requested) {
        worker_calls.fetch_add(1U, std::memory_order_relaxed);
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);

  ScopedWorkerThreadStartFailure failure_injection;
  std::optional<std::error_code> start_error;
  try {
    static_cast<void>(service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                             OutputSlotId("image.final"),
                                             test_job_resources())));
    ADD_FAILURE() << "injected manager supervision-thread start did not fail";
  } catch (const std::system_error& error) {
    start_error = error.code();
  } catch (...) {
    FAIL() << "injected manager supervision-thread start raised a "
              "non-system exception";
  }
  ASSERT_TRUE(start_error.has_value());
  EXPECT_EQ(*start_error,
            std::make_error_code(std::errc::resource_unavailable_try_again));
  ASSERT_TRUE(failure_injection.attempted_job_id().has_value());
  EXPECT_TRUE(failure_injection.manager_record_inserted_before_failure());
  const JobId failed_job_id = *failure_injection.attempted_job_id();

  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service), 0U);
  EXPECT_EQ(service.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::quota_reservation_ownership(service)
          .total(),
      0U);
  EXPECT_FALSE(service.query(failed_job_id).has_value());
  const WorkerThreadOwnershipSnapshot failed_ownership =
      SingleTenantJobServiceTestAccess::worker_thread_ownership(service);
  EXPECT_EQ(failed_ownership.active, 0U);
  EXPECT_EQ(failed_ownership.completed, 0U);
  EXPECT_EQ(failed_ownership.joining, 0U);
  EXPECT_EQ(failed_ownership.total(), 0U);

  const JobSubmission recovery = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  EXPECT_NE(recovery.job_id, failed_job_id);
  const std::optional<JobSnapshot> terminal =
      service.wait_for(recovery.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Succeeded);
  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 1U);
  EXPECT_FALSE(service.query(failed_job_id).has_value());
  EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service), 1U);
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::owned_worker_thread_count(service), 0U);
}

TEST(SingleTenantJobService,
     RetryManagerThreadStartFailureErasesRecordAndRestoresPriorTruth) {
  std::atomic<std::size_t> worker_calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&worker_calls](const JobAssignment& assignment,
                      const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        if (worker_calls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
          return settled_failed_report(assignment);
        }
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> original_failure =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(original_failure.has_value());
  ASSERT_EQ(original_failure->state, JobState::Failed);
  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));

  ScopedWorkerThreadStartFailure failure_injection;
  EXPECT_THROW(static_cast<void>(service.retry(submission.job_id)),
               std::system_error);
  ASSERT_TRUE(failure_injection.attempted_job_id().has_value());
  EXPECT_EQ(*failure_injection.attempted_job_id(), submission.job_id);
  EXPECT_TRUE(failure_injection.manager_record_inserted_before_failure());

  const std::optional<JobSnapshot> restored = service.query(submission.job_id);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->state, original_failure->state);
  EXPECT_EQ(restored->assignment, original_failure->assignment);
  EXPECT_EQ(restored->attempt_settled, original_failure->attempt_settled);
  EXPECT_EQ(restored->attempt_outcome, original_failure->attempt_outcome);
  EXPECT_EQ(restored->failure, original_failure->failure);
  EXPECT_EQ(restored->message, original_failure->message);
  EXPECT_EQ(service.quota_snapshot().active_attempts, 0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::quota_reservation_ownership(service)
          .total(),
      0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::owned_worker_thread_count(service), 0U);

  const std::optional<JobSubmission> recovered_retry =
      service.retry(submission.job_id);
  ASSERT_TRUE(recovered_retry.has_value());
  const std::optional<JobSnapshot> succeeded =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  EXPECT_EQ(succeeded->state, JobState::Succeeded);
  EXPECT_EQ(succeeded->assignment, recovered_retry->assignment);
  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 2U);
}

TEST(WorkerManagerDescriptorOwnership,
     InterruptedCloseDoesNotRetryAgainstReusedDescriptorValue) {
  const int owned_descriptor = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(owned_descriptor, 0);
  const int replacement_source = ::open("/dev/null", O_RDONLY);
  if (replacement_source < 0) {
    close_test_descriptor_once(owned_descriptor);
    FAIL() << "failed to open replacement descriptor source";
  }
  ASSERT_NE(replacement_source, owned_descriptor);

  int owner = owned_descriptor;
  InterruptedCloseReuseProbe probe;
  probe.replacement_source = replacement_source;
  WorkerManagerTestAccess::reset_descriptor_for_test(
      &owner, -1, close_and_reuse_before_eintr, &probe);

  EXPECT_EQ(owner, -1);
  EXPECT_EQ(probe.close_error, 0);
  EXPECT_EQ(probe.duplicate_error, 0);
  EXPECT_EQ(probe.close_calls, 1U);
  EXPECT_NE(::fcntl(owned_descriptor, F_GETFD), -1)
      << "a repeated close consumed the descriptor reused after EINTR";

  close_test_descriptor_once(owned_descriptor);
  close_test_descriptor_once(replacement_source);
}

TEST(SingleTenantJobService,
     SubmitRecordBadAllocRollsBackJobQuotaAndWorkerOwnership) {
  ScopedTestStateRoot root;
  auto record_failure = std::make_shared<std::atomic<bool>>(true);
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  WorkerManagerOptions options;
  options.fail_record_construction_for_test = record_failure;
  {
    SingleTenantJobService service(
        TenantId("tenant.test"), test_quota_limits(), root.path(), factory,
        DurableServerStateOptions{}, TenantQuotaAuthorityOptions{}, options);

    EXPECT_THROW(service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                        OutputSlotId("image.final"),
                                        test_job_resources())),
                 std::bad_alloc);
    EXPECT_FALSE(record_failure->load(std::memory_order_acquire));
    EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service),
              0U);
    EXPECT_EQ(service.quota_snapshot().active_attempts, 0U);
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::quota_reservation_ownership(service)
            .total(),
        0U);
    EXPECT_EQ(SingleTenantJobServiceTestAccess::worker_thread_ownership(service)
                  .total(),
              0U);
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::live_worker_process_count(service),
        0U);
  }

  SingleTenantJobService recovered(
      TenantId("tenant.test"), test_quota_limits(), root.path(), factory,
      DurableServerStateOptions{}, TenantQuotaAuthorityOptions{}, options);
  EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(recovered),
            0U);
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  const JobSubmission accepted = recovered.submit(
      JobSpec(GraphArtifactId("graph.test.recovered"), 8,
              OutputSlotId("image.final"), test_job_resources()));
  const std::optional<JobSnapshot> terminal =
      recovered.wait_for(accepted.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Succeeded);
}

TEST(SingleTenantJobService,
     RetryRecordBadAllocPreservesPriorDurableTruthAndOwnership) {
  ScopedTestStateRoot root;
  auto record_failure = std::make_shared<std::atomic<bool>>(false);
  std::atomic<std::size_t> worker_calls{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [&worker_calls](const JobAssignment& assignment,
                      const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        if (worker_calls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
          return settled_failed_report(assignment);
        }
        return successful_report(assignment);
      });
  WorkerManagerOptions options;
  options.fail_record_construction_for_test = record_failure;
  JobSubmission submission;
  JobSnapshot prior_failure;
  {
    SingleTenantJobService service(
        TenantId("tenant.test"), test_quota_limits(), root.path(), factory,
        DurableServerStateOptions{}, TenantQuotaAuthorityOptions{}, options);
    submission = service.submit(JobSpec(GraphArtifactId("graph.test"), 7,
                                        OutputSlotId("image.final"),
                                        test_job_resources()));
    const std::optional<JobSnapshot> failed =
        service.wait_for(submission.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(failed.has_value());
    ASSERT_EQ(failed->state, JobState::Failed);
    prior_failure = *failed;
    ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                    wait_for_owned_worker_thread_count_at_most(
                        service, 0U, std::chrono::seconds(2)));

    record_failure->store(true, std::memory_order_release);
    EXPECT_THROW(static_cast<void>(service.retry(submission.job_id)),
                 std::bad_alloc);
    EXPECT_FALSE(record_failure->load(std::memory_order_acquire));
    const std::optional<JobSnapshot> unchanged =
        service.query(submission.job_id);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->state, prior_failure.state);
    EXPECT_EQ(unchanged->assignment, prior_failure.assignment);
    EXPECT_EQ(unchanged->attempt_settled, prior_failure.attempt_settled);
    EXPECT_EQ(unchanged->attempt_outcome, prior_failure.attempt_outcome);
    EXPECT_EQ(unchanged->failure, prior_failure.failure);
    EXPECT_EQ(unchanged->message, prior_failure.message);
    EXPECT_EQ(service.quota_snapshot().active_attempts, 0U);
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::quota_reservation_ownership(service)
            .total(),
        0U);
    EXPECT_EQ(SingleTenantJobServiceTestAccess::worker_thread_ownership(service)
                  .total(),
              0U);
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::live_worker_process_count(service),
        0U);
    EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 1U);
  }

  SingleTenantJobService recovered(
      TenantId("tenant.test"), test_quota_limits(), root.path(), factory,
      DurableServerStateOptions{}, TenantQuotaAuthorityOptions{}, options);
  const std::optional<JobSnapshot> durable = recovered.query(submission.job_id);
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(durable->state, prior_failure.state);
  EXPECT_EQ(durable->assignment, prior_failure.assignment);
  EXPECT_EQ(durable->attempt_settled, prior_failure.attempt_settled);
  EXPECT_EQ(durable->attempt_outcome, prior_failure.attempt_outcome);
  EXPECT_EQ(durable->failure, prior_failure.failure);
  EXPECT_EQ(durable->message, prior_failure.message);
  EXPECT_EQ(recovered.quota_snapshot().active_attempts, 0U);
  const std::optional<JobSubmission> retried =
      recovered.retry(submission.job_id);
  ASSERT_TRUE(retried.has_value());
  const std::optional<JobSnapshot> succeeded =
      recovered.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(succeeded.has_value());
  EXPECT_EQ(succeeded->state, JobState::Succeeded);
  EXPECT_EQ(succeeded->assignment, retried->assignment);
  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 2U);
}

TEST(SingleTenantJobService,
     ReapsSequentialWorkerThreadsWhileServiceRemainsAlive) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);

  constexpr std::size_t kSubmissionCount = 128U;
  std::vector<JobSubmission> submissions;
  submissions.reserve(kSubmissionCount);
  for (std::size_t index = 0U; index < kSubmissionCount; ++index) {
    submissions.push_back(service.submit(JobSpec(GraphArtifactId("graph.test"),
                                                 7, OutputSlotId("image.final"),
                                                 test_job_resources())));
    const std::optional<JobSnapshot> terminal =
        service.wait_for(submissions.back().job_id, std::chrono::seconds(2));
    ASSERT_TRUE(terminal.has_value());
    ASSERT_EQ(terminal->state, JobState::Succeeded);
  }

  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::owned_worker_thread_count(service), 0U);
  const std::optional<JobSnapshot> first =
      service.query(submissions.front().job_id);
  const std::optional<JobSnapshot> last =
      service.query(submissions.back().job_id);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(first->state, JobState::Succeeded);
  EXPECT_EQ(last->state, JobState::Succeeded);
}

TEST(SingleTenantJobService,
     ReapsCompletedWorkersWhileConcurrentWorkersRemainActive) {
  constexpr std::size_t kBlockedWorkerCount = 6U;
  constexpr std::size_t kCompletedWorkerCount = 96U;
  auto gate = std::make_shared<WorkerGroupGate>();
  std::atomic<std::size_t> worker_ordinal{0U};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [gate, &worker_ordinal](
          const JobAssignment& assignment,
          const std::function<bool()>& cancellation_requested) {
        const std::size_t ordinal = worker_ordinal.fetch_add(1U);
        if (ordinal < kBlockedWorkerCount) {
          gate->enter_and_wait();
        }
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  WorkerGroupGateReleaseGuard gate_release(gate);

  std::vector<JobSubmission> blocked_submissions;
  blocked_submissions.reserve(kBlockedWorkerCount);
  for (std::size_t index = 0U; index < kBlockedWorkerCount; ++index) {
    blocked_submissions.push_back(service.submit(
        JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                test_job_resources())));
  }
  ASSERT_TRUE(gate->wait_until_entered(kBlockedWorkerCount));

  for (std::size_t index = 0U; index < kCompletedWorkerCount; ++index) {
    const JobSubmission submission = service.submit(
        JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                test_job_resources()));
    const std::optional<JobSnapshot> terminal =
        service.wait_for(submission.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(terminal.has_value());
    ASSERT_EQ(terminal->state, JobState::Succeeded);
  }

  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, kBlockedWorkerCount, std::chrono::seconds(2)));
  const WorkerThreadOwnershipSnapshot ownership =
      SingleTenantJobServiceTestAccess::worker_thread_ownership(service);
  EXPECT_EQ(ownership.active, kBlockedWorkerCount);
  EXPECT_EQ(ownership.completed, 0U);
  EXPECT_EQ(ownership.joining, 0U);
  EXPECT_EQ(ownership.total(), kBlockedWorkerCount);

  gate_release.release();
  for (const JobSubmission& submission : blocked_submissions) {
    const std::optional<JobSnapshot> terminal =
        service.wait_for(submission.job_id, std::chrono::seconds(2));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->state, JobState::Succeeded);
  }
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(
                      service, 0U, std::chrono::seconds(2)));
}

TEST(SingleTenantJobService, DestructorWaitsForActiveWorkerAndReaperDrain) {
  auto gate = std::make_shared<WorkerGate>();
  std::atomic<bool> cancellation_observed{false};
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [gate, &cancellation_observed](
          const JobAssignment& assignment,
          const std::function<bool()>& cancellation_requested) {
        gate->enter_and_wait();
        cancellation_observed.store(cancellation_requested());
        return successful_report(assignment);
      });
  auto service =
      std::make_unique<TestJobService>(TenantId("tenant.test"), factory);
  service->submit(JobSpec(GraphArtifactId("graph.test"), 7,
                          OutputSlotId("image.final"), test_job_resources()));

  std::mutex destruction_mutex;
  std::condition_variable destruction_condition;
  bool destruction_started = false;
  bool destruction_finished = false;
  std::future<void> destruction;
  WorkerGateReleaseGuard gate_release(gate);
  ASSERT_TRUE(gate->wait_until_entered());
  destruction =
      std::async(std::launch::async,
                 [owned_service = std::move(service), &destruction_mutex,
                  &destruction_condition, &destruction_started,
                  &destruction_finished]() mutable {
                   {
                     std::lock_guard<std::mutex> lock(destruction_mutex);
                     destruction_started = true;
                   }
                   destruction_condition.notify_all();
                   owned_service.reset();
                   {
                     std::lock_guard<std::mutex> lock(destruction_mutex);
                     destruction_finished = true;
                   }
                   destruction_condition.notify_all();
                 });

  {
    std::unique_lock<std::mutex> lock(destruction_mutex);
    EXPECT_TRUE(destruction_condition.wait_for(
        lock, std::chrono::seconds(2), [&] { return destruction_started; }));
    EXPECT_FALSE(destruction_finished);
  }
  gate_release.release();
  EXPECT_EQ(destruction.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  destruction.get();
  EXPECT_TRUE(cancellation_observed.load());
  {
    std::lock_guard<std::mutex> lock(destruction_mutex);
    EXPECT_TRUE(destruction_finished);
  }
}

TEST(SingleTenantJobService, MissingRequiredImageFailsClosed) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        (void)cancellation_requested;
        JobAttemptReport report;
        report.identity = assignment.identity;
        report.outcome = JobAttemptOutcome::Succeeded;
        report.settled = true;
        report.failure = JobAttemptFailure::None;
        return report;
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  EXPECT_FALSE(terminal->attempt_outcome.has_value());
  EXPECT_FALSE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::ReportRejected);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

TEST(SingleTenantJobService, MalformedReportShapesFailClosedBeforeFactCopy) {
  constexpr std::array<MalformedReportShape, 12U> kShapes{
      MalformedReportShape::SucceededUnsettled,
      MalformedReportShape::SucceededWithFailure,
      MalformedReportShape::FailedWithoutFailure,
      MalformedReportShape::FailedWithImage,
      MalformedReportShape::FailedWithCancellationFailure,
      MalformedReportShape::FailedWithReportRejected,
      MalformedReportShape::FailedWithArtifactCommit,
      MalformedReportShape::CancelledUnsettled,
      MalformedReportShape::CancelledWithWorkerFailure,
      MalformedReportShape::CancelledWithImage,
      MalformedReportShape::InvalidOutcome,
      MalformedReportShape::InvalidFailure,
  };
  for (const MalformedReportShape shape : kShapes) {
    SCOPED_TRACE(::testing::Message()
                 << "shape=" << static_cast<unsigned int>(shape));
    auto factory = std::make_shared<FunctionWorkerFactory>(
        [shape](const JobAssignment& assignment,
                const std::function<bool()>& cancellation_requested) {
          (void)cancellation_requested;
          return malformed_report(assignment, shape);
        });
    TestJobService service(TenantId("tenant.test"), factory);
    const JobSubmission submission = service.submit(
        JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                test_job_resources()));
    const std::optional<JobSnapshot> terminal =
        service.wait_for(submission.job_id, std::chrono::seconds(2));

    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->state, JobState::Failed);
    EXPECT_FALSE(terminal->attempt_outcome.has_value());
    EXPECT_FALSE(terminal->attempt_settled);
    EXPECT_EQ(terminal->failure, JobAttemptFailure::ReportRejected);
    EXPECT_FALSE(terminal->output_receipt.has_value());
  }
}

TEST(SingleTenantJobService,
     LegalFailedReportShapesPreserveFailureAndSettlement) {
  constexpr std::array<JobAttemptFailure, 7U> kWorkerFailures{
      JobAttemptFailure::InvalidAssignment, JobAttemptFailure::GraphResolution,
      JobAttemptFailure::HostSetup,         JobAttemptFailure::GraphLoad,
      JobAttemptFailure::Compute,           JobAttemptFailure::Settlement,
      JobAttemptFailure::Unexpected,
  };
  constexpr std::array<bool, 2U> kSettlementFacts{false, true};
  for (const JobAttemptFailure failure : kWorkerFailures) {
    for (const bool settled : kSettlementFacts) {
      SCOPED_TRACE(::testing::Message()
                   << "failure=" << static_cast<unsigned int>(failure)
                   << ", settled=" << settled);
      auto factory = std::make_shared<FunctionWorkerFactory>(
          [failure, settled](
              const JobAssignment& assignment,
              const std::function<bool()>& cancellation_requested) {
            (void)cancellation_requested;
            JobAttemptReport report;
            report.identity = assignment.identity;
            report.outcome = JobAttemptOutcome::Failed;
            report.settled = settled;
            report.failure = failure;
            report.message = "typed worker failure";
            return report;
          });
      TestJobService service(TenantId("tenant.test"), factory);
      const JobSubmission submission = service.submit(
          JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                  test_job_resources()));
      const std::optional<JobSnapshot> terminal =
          service.wait_for(submission.job_id, std::chrono::seconds(2));

      ASSERT_TRUE(terminal.has_value());
      EXPECT_EQ(terminal->state, JobState::Failed);
      ASSERT_TRUE(terminal->attempt_outcome.has_value());
      EXPECT_EQ(*terminal->attempt_outcome, JobAttemptOutcome::Failed);
      EXPECT_EQ(terminal->attempt_settled, settled);
      EXPECT_EQ(terminal->failure, failure);
      EXPECT_FALSE(terminal->output_receipt.has_value());
    }
  }
}

TEST(SingleTenantJobService,
     CancellationReportWithoutControlIntentFailsClosed) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        (void)cancellation_requested;
        JobAttemptReport report;
        report.identity = assignment.identity;
        report.outcome = JobAttemptOutcome::Cancelled;
        report.settled = true;
        report.failure = JobAttemptFailure::CancellationObserved;
        return report;
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));

  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  EXPECT_FALSE(terminal->attempt_outcome.has_value());
  EXPECT_FALSE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::ReportRejected);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

TEST(SingleTenantJobService, MismatchedWorkerLeaseFailsCurrentAttemptClosed) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        (void)cancellation_requested;
        JobAttemptReport report = successful_report(assignment);
        ++report.identity.worker_lease_generation.value;
        return report;
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  EXPECT_FALSE(terminal->attempt_outcome.has_value());
  EXPECT_FALSE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::ReportRejected);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

TEST(SingleTenantJobService, MismatchedReportedJobIdFailsOwningJobClosed) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        (void)cancellation_requested;
        JobAttemptReport report = successful_report(assignment);
        report.identity.job_id = JobId("job.stale.other");
        return report;
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  EXPECT_FALSE(terminal->attempt_outcome.has_value());
  EXPECT_FALSE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::ReportRejected);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

TEST(SingleTenantJobService,
     FactoryAndWorkerExceptionsFailUnsettledWithoutArtifact) {
  constexpr std::array<ExceptionStage, 2U> kStages{ExceptionStage::Factory,
                                                   ExceptionStage::Worker};
  constexpr std::array<ExceptionKind, 2U> kKinds{ExceptionKind::Standard,
                                                 ExceptionKind::NonStandard};
  for (const ExceptionStage stage : kStages) {
    for (const ExceptionKind kind : kKinds) {
      SCOPED_TRACE(::testing::Message() << "stage=" << static_cast<int>(stage)
                                        << ", kind=" << static_cast<int>(kind));
      auto factory = std::make_shared<ThrowingWorkerFactory>(stage, kind);
      TestJobService service(TenantId("tenant.test"), factory);
      const JobSubmission submission = service.submit(
          JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                  test_job_resources()));

      const std::optional<JobSnapshot> terminal =
          service.wait_for(submission.job_id, std::chrono::seconds(2));
      ASSERT_TRUE(terminal.has_value());
      EXPECT_EQ(terminal->state, JobState::Failed);
      EXPECT_FALSE(terminal->attempt_settled);
      EXPECT_EQ(terminal->failure, JobAttemptFailure::Unexpected);
      EXPECT_FALSE(terminal->output_receipt.has_value());
    }
  }
}

TEST(SingleTenantJobService, NullFactoryResultFailsUnsettledWithoutArtifact) {
  auto factory = std::make_shared<NullWorkerFactory>();
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));

  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  EXPECT_FALSE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::HostSetup);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

TEST(SingleTenantJobService,
     CancellationCannotTurnUnsettledWorkerExceptionIntoCancelled) {
  auto gate = std::make_shared<WorkerGate>();
  auto factory = std::make_shared<ThrowingWorkerFactory>(
      ExceptionStage::Worker, ExceptionKind::NonStandard, gate);
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  WorkerGateReleaseGuard gate_release(gate);
  ASSERT_TRUE(gate->wait_until_entered());
  EXPECT_TRUE(service.cancel(submission.job_id));
  gate_release.release();

  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  ASSERT_TRUE(terminal->attempt_outcome.has_value());
  EXPECT_EQ(*terminal->attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_FALSE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::Unexpected);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

TEST(SingleTenantJobService, AcceptedCancellationPreservesWorkerStageFailures) {
  constexpr std::array<JobAttemptFailure, 3U> kStageFailures{
      JobAttemptFailure::GraphResolution,
      JobAttemptFailure::HostSetup,
      JobAttemptFailure::GraphLoad,
  };
  constexpr std::array<bool, 2U> kSettlementFacts{false, true};
  for (const JobAttemptFailure failure : kStageFailures) {
    for (const bool settled : kSettlementFacts) {
      SCOPED_TRACE(::testing::Message()
                   << "failure=" << static_cast<unsigned int>(failure)
                   << ", settled=" << settled);
      auto gate = std::make_shared<WorkerGate>();
      auto factory = std::make_shared<FunctionWorkerFactory>(
          [gate, failure, settled](
              const JobAssignment& assignment,
              const std::function<bool()>& cancellation_requested) {
            gate->enter_and_wait();
            EXPECT_TRUE(cancellation_requested());
            JobAttemptReport report;
            report.identity = assignment.identity;
            report.outcome = JobAttemptOutcome::Failed;
            report.settled = settled;
            report.failure = failure;
            report.message = "worker stage failed after cancellation";
            return report;
          });
      TestJobService service(TenantId("tenant.test"), factory);
      const JobSubmission submission = service.submit(
          JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
                  test_job_resources()));
      WorkerGateReleaseGuard gate_release(gate);
      ASSERT_TRUE(gate->wait_until_entered());
      EXPECT_TRUE(service.cancel(submission.job_id));
      gate_release.release();

      const std::optional<JobSnapshot> terminal =
          service.wait_for(submission.job_id, std::chrono::seconds(2));
      ASSERT_TRUE(terminal.has_value());
      EXPECT_EQ(terminal->state, JobState::Failed);
      EXPECT_TRUE(terminal->cancellation_requested);
      ASSERT_TRUE(terminal->attempt_outcome.has_value());
      EXPECT_EQ(*terminal->attempt_outcome, JobAttemptOutcome::Failed);
      EXPECT_EQ(terminal->attempt_settled, settled);
      EXPECT_EQ(terminal->failure, failure);
      EXPECT_EQ(terminal->message, "worker stage failed after cancellation");
      EXPECT_FALSE(terminal->output_receipt.has_value());
      EXPECT_FALSE(service.cancel(submission.job_id));
    }
  }
}

TEST(SingleTenantJobService, CancellationBeforeCommitWaitsForSettlement) {
  auto gate = std::make_shared<WorkerGate>();
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [gate](const JobAssignment& assignment,
             const std::function<bool()>& cancellation_requested) {
        gate->enter_and_wait();
        EXPECT_TRUE(cancellation_requested());
        return successful_report(assignment);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  WorkerGateReleaseGuard gate_release(gate);
  ASSERT_TRUE(gate->wait_until_entered());
  EXPECT_TRUE(service.cancel(submission.job_id));
  const std::optional<JobSnapshot> cancelling =
      service.query(submission.job_id);
  ASSERT_TRUE(cancelling.has_value());
  EXPECT_EQ(cancelling->state, JobState::Cancelling);
  EXPECT_FALSE(cancelling->attempt_settled);

  gate_release.release();
  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Cancelled);
  EXPECT_TRUE(terminal->attempt_settled);
  ASSERT_TRUE(terminal->attempt_outcome.has_value());
  EXPECT_EQ(*terminal->attempt_outcome, JobAttemptOutcome::Succeeded);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::CancellationObserved);
  EXPECT_FALSE(terminal->output_receipt.has_value());
  EXPECT_FALSE(service.cancel(submission.job_id));
}

TEST(SingleTenantJobService,
     CancellationCannotTurnMalformedSettledReportIntoCancelled) {
  auto gate = std::make_shared<WorkerGate>();
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [gate](const JobAssignment& assignment,
             const std::function<bool()>& cancellation_requested) {
        gate->enter_and_wait();
        EXPECT_TRUE(cancellation_requested());
        return malformed_report(assignment,
                                MalformedReportShape::FailedWithoutFailure);
      });
  TestJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"),
              test_job_resources()));
  WorkerGateReleaseGuard gate_release(gate);
  ASSERT_TRUE(gate->wait_until_entered());
  EXPECT_TRUE(service.cancel(submission.job_id));
  gate_release.release();

  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(2));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  EXPECT_FALSE(terminal->attempt_outcome.has_value());
  EXPECT_FALSE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::ReportRejected);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

}  // namespace
}  // namespace ps::server
