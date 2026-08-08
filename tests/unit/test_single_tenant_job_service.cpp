/**
 * @file test_single_tenant_job_service.cpp
 * @brief Verifies Issue #98 immutable Job, fencing, cancellation, and
 * artifacts.
 */
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/**
 * @brief Builds one complete deterministic assignment identity for store tests.
 * @param ordinal Positive suffix used only to avoid accidental text equality.
 * @return Complete valid tuple with a real JobSpec digest.
 * @throws Validation or allocation failures unchanged.
 */
AttemptIdentity make_test_identity(std::uint64_t ordinal) {
  const JobSpec spec(GraphArtifactId("graph.test"), 7,
                     OutputSlotId("image.final"), 2U);
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
                      OutputSlotId("image.final"), 2U);
  const JobSpec second(GraphArtifactId("graph.test"), 7,
                       OutputSlotId("image.final"), 2U);
  EXPECT_EQ(first.canonical_bytes(), second.canonical_bytes());
  EXPECT_EQ(first.digest(), second.digest());
  EXPECT_EQ(first.canonical_bytes(),
            "jobspec-v110:graph.test1:711:image.final1:2"
            "15:embedded-cpu-v116:process-lifetime");
  EXPECT_EQ(first.digest().hex(),
            "3095a8869fbb6db00f8723445a2cd0e9ba2d63a4d92afaa9f96d519515ffa637");

  constexpr char kKnownInput[] = "abc";
  const JobSpecDigest known =
      hash_job_spec_bytes(reinterpret_cast<const std::byte*>(kKnownInput), 3U);
  EXPECT_EQ(known.hex(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_THROW(GraphArtifactId("/tmp/graph.yaml"), std::invalid_argument);
  EXPECT_THROW(JobSpec(GraphArtifactId("graph.test"), -1,
                       OutputSlotId("image.final"), 2U),
               std::invalid_argument);
  EXPECT_THROW(JobSpec(GraphArtifactId("graph.test"), 7,
                       OutputSlotId("image.final"), 0U),
               std::invalid_argument);
}

TEST(ProcessLifetimeArtifactStore,
     CopiesActiveRowsAndKeepsIdentitySeparateFromContent) {
  ProcessLifetimeArtifactStore store;
  ImageBuffer image = make_test_image();
  const AttemptIdentity first_identity = make_test_identity(1U);
  const OutputCommitReceipt first = store.commit(ArtifactCommitRequest{
      first_identity, OutputSlotId("image.final"), image});

  auto* source = static_cast<std::byte*>(image.data.get());
  source[0] = std::byte{0x7f};
  const std::shared_ptr<const ArtifactRecord> record =
      store.find(first.artifact_id);
  ASSERT_NE(record, nullptr);
  ASSERT_EQ(record->payload.size(), 12U);
  EXPECT_EQ(record->payload[0], std::byte{1U});
  EXPECT_EQ(record->payload[6], std::byte{11U});
  EXPECT_EQ(record->receipt.descriptor.row_bytes, 6U);
  EXPECT_EQ(record->receipt.descriptor.payload_bytes, 12U);

  source[0] = std::byte{1U};
  const OutputCommitReceipt second = store.commit(ArtifactCommitRequest{
      make_test_identity(2U), OutputSlotId("image.final"), image});
  EXPECT_EQ(first.content_digest, second.content_digest);
  EXPECT_NE(first.artifact_id, second.artifact_id);
  EXPECT_NE(first.output_commit_id, second.output_commit_id);
  EXPECT_EQ(store.size(), 2U);
}

TEST(SingleTenantJobService, SuccessRequiresReceiptAndSupportsArtifactLookup) {
  auto factory = std::make_shared<FunctionWorkerFactory>(
      [](const JobAssignment& assignment,
         const std::function<bool()>& cancellation_requested) {
        EXPECT_FALSE(cancellation_requested());
        return successful_report(assignment);
      });
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
    SingleTenantJobService service(TenantId("tenant.test"), factory);
    const JobSubmission submission = service.submit(JobSpec(
        GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
      SingleTenantJobService service(TenantId("tenant.test"), factory);
      const JobSubmission submission = service.submit(JobSpec(
          GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
      SingleTenantJobService service(TenantId("tenant.test"), factory);
      const JobSubmission submission = service.submit(JobSpec(
          GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));

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
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));

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
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
      SingleTenantJobService service(TenantId("tenant.test"), factory);
      const JobSubmission submission = service.submit(JobSpec(
          GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
  SingleTenantJobService service(TenantId("tenant.test"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.test"), 7, OutputSlotId("image.final"), 2U));
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
