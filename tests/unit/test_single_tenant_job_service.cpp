/**
 * @file test_single_tenant_job_service.cpp
 * @brief Verifies Issue #99 quota, durable artifacts, retry, and fencing.
 */
#include <gtest/gtest.h>

#include <array>
#include <atomic>
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
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)
#include "server/single_tenant_job_service_test_access.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

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
   * @throws As `SingleTenantJobService` construction.
   */
  TestJobService(TenantId tenant_id,
                 std::shared_ptr<JobAttemptWorkerFactory> factory)
      : service_(std::move(tenant_id), test_quota_limits(), root_.path(),
                 std::move(factory)) {}

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
   * @throws Nothing.
   */
  bool cancel(const JobId& id) noexcept { return service_.cancel(id); }
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
class FunctionWorkerFactory final : public JobAttemptWorkerFactory {
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
class ThrowingWorkerFactory final : public JobAttemptWorkerFactory {
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
class NullWorkerFactory final : public JobAttemptWorkerFactory {
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

  EXPECT_TRUE(authority.release_retained_artifact(artifact_id));
  EXPECT_FALSE(authority.release_retained_artifact(artifact_id));
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
        store.find_artifact(request.artifact_id);
    ASSERT_NE(published, nullptr);
    original = published->receipt;

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
  EXPECT_NO_THROW(store.persist_job(record));

  record.state = JobState::Succeeded;
  record.attempt_settled = true;
  record.attempt_outcome = JobAttemptOutcome::Succeeded;
  EXPECT_THROW(store.persist_job(record), std::invalid_argument);

  record.state = JobState::Failed;
  record.attempt_settled = false;
  record.attempt_outcome = JobAttemptOutcome::None;
  record.failure = JobAttemptFailure::Compute;
  EXPECT_THROW(store.persist_job(record), std::invalid_argument);
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
  const std::optional<JobSnapshot> historical = service.query(first.job_id);
  ASSERT_TRUE(historical.has_value());
  EXPECT_EQ(historical->state, JobState::Succeeded);
  ASSERT_TRUE(historical->output_receipt.has_value());
  EXPECT_EQ(historical->output_receipt->artifact_id,
            succeeded->output_receipt->artifact_id);
  EXPECT_EQ(historical->output_receipt->output_commit_id,
            succeeded->output_receipt->output_commit_id);
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
    store.persist_job(interrupted);
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
    store.persist_job(running);
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
     AssignmentThreadStartFailureRollsBackStateAndOwnership) {
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
    ADD_FAILURE() << "injected worker-thread start did not fail";
  } catch (const std::system_error& error) {
    start_error = error.code();
  } catch (...) {
    FAIL() << "injected worker-thread start raised a non-system exception";
  }
  ASSERT_TRUE(start_error.has_value());
  EXPECT_EQ(*start_error,
            std::make_error_code(std::errc::resource_unavailable_try_again));
  ASSERT_TRUE(failure_injection.attempted_job_id().has_value());
  const JobId failed_job_id = *failure_injection.attempted_job_id();

  EXPECT_EQ(worker_calls.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(SingleTenantJobServiceTestAccess::accepted_job_count(service), 0U);
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
     RetryThreadStartFailureRestoresDurableTruthQuotaAndOwnership) {
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
