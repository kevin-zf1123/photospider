/**
 * @file test_single_tenant_job_product_path.cpp
 * @brief Verifies Issue #99 through the real Embedded Host product path.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "server/embedded_job_worker.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_PHOTOSPIDER_WORKER_PATH
#error "PS_PHOTOSPIDER_WORKER_PATH must name photospider-worker"
#endif

namespace ps::server {
namespace {

/**
 * @brief Cancellation observation reached immediately before Host compute.
 * @note The worker's documented sequence is before resolution, before Host
 * construction, before graph load, before compute, and after compute.
 */
constexpr std::size_t kBeforeComputeCancellationObservation = 4U;

/**
 * @brief Cancellation observation reached after Host compute returns.
 * @note Blocking this observation proves the compute fact already exists
 * before the test concurrently accepts cancellation.
 */
constexpr std::size_t kAfterComputeCancellationObservation = 5U;

/**
 * @brief Builds one complete resource envelope for real Embedded Host tests.
 * @param cpu_slots Positive maximum Embedded Host callback parallelism.
 * @return Valid bounded CPU-only server quota request.
 * @throws Nothing.
 */
JobResourceRequest product_job_resources(std::uint32_t cpu_slots = 2U) {
  JobResourceRequest request;
  request.cpu_slots = cpu_slots;
  request.host_memory_bytes = 512ULL << 30U;
  request.output_bytes = 8U << 20U;
  request.staging_bytes = 8U << 20U;
  request.retention_bytes = 8U << 20U;
  return request;
}

/**
 * @brief Builds permissive finite tenant capacity for the product tests.
 * @return Capacity for concurrent real Host attempts and retained fixtures.
 * @throws Nothing.
 */
TenantQuotaLimits product_quota_limits() {
  TenantQuotaLimits limits;
  limits.maximum_active_attempts = 8U;
  limits.capacity.cpu_slots = 16U;
  limits.capacity.host_memory_bytes = 4ULL << 40U;
  limits.capacity.output_bytes = 64U << 20U;
  limits.capacity.staging_bytes = 64U << 20U;
  limits.capacity.retention_bytes = 64U << 20U;
  return limits;
}

/**
 * @brief Builds production-mode process supervision bounds for Embedded Host.
 * @return Valid configuration using the built non-installed worker executable.
 * @throws Path allocation failures unchanged.
 */
WorkerManagerOptions product_worker_options() {
  WorkerManagerOptions options;
  options.worker_executable = PS_PHOTOSPIDER_WORKER_PATH;
  options.startup_timeout = std::chrono::seconds(10);
  options.heartbeat_interval = std::chrono::milliseconds(250);
  options.heartbeat_timeout = std::chrono::seconds(5);
  options.attempt_runtime_timeout = std::chrono::seconds(30);
  options.post_report_timeout = std::chrono::seconds(5);
  options.cooperative_cancel_timeout = std::chrono::seconds(2);
  options.terminate_timeout = std::chrono::seconds(1);
  options.kill_reap_timeout = std::chrono::seconds(2);
  options.io_timeout = std::chrono::seconds(5);
  return options;
}

/**
 * @brief Owns one isolated filesystem tree for the real Job product-path test.
 * @throws Filesystem failures when root creation fails.
 * @note Destruction best-effort removes only the exact owned temporary root.
 */
class ScopedJobProductRoot final {
 public:
  /** @brief Creates one process-local unique temporary root. */
  ScopedJobProductRoot() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("photospider-job-product-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    if (!std::filesystem::create_directory(root_)) {
      throw std::runtime_error("failed to create Job product test root");
    }
  }

  /** @brief Removes the exact owned tree without throwing. */
  ~ScopedJobProductRoot() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /** @brief Prevents duplicate temporary-root ownership. */
  ScopedJobProductRoot(const ScopedJobProductRoot&) = delete;
  /** @brief Prevents duplicate temporary-root assignment. */
  ScopedJobProductRoot& operator=(const ScopedJobProductRoot&) = delete;

  /**
   * @brief Returns the exact existing temporary root.
   * @return Borrowed path valid through this owner.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Exact recursively cleaned temporary path. */
  std::filesystem::path root_;
};

/**
 * @brief Writes one tiny deterministic coordinate-pattern graph fixture.
 * @param path Test-owned YAML destination.
 * @return Nothing after complete close.
 * @throws std::runtime_error for open, write, or close failure.
 */
void write_job_graph(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to open Job graph YAML");
  }
  output << "- id: 0\n"
         << "  name: issue99_coordinate_pattern\n"
         << "  type: image_generator\n"
         << "  subtype: coordinate_pattern\n"
         << "  parameters:\n"
         << "    width: 8\n"
         << "    height: 6\n"
         << "    channels: 4\n"
         << "    seed: 17\n";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write Job graph YAML");
  }
}

/**
 * @brief Builds one complete worker assignment for direct product-path tests.
 * @param graph_id Immutable graph material identity accepted by the resolver.
 * @param target_node Node selected for real Embedded Host compute.
 * @param identity_suffix Unique suffix for attempt-local product identities.
 * @return Valid immutable assignment with lease generation one.
 * @throws std::invalid_argument for invalid identities or JobSpec values.
 * @throws std::bad_alloc when retained identity/spec storage exhausts memory.
 */
JobAssignment make_worker_assignment(const GraphArtifactId& graph_id,
                                     int target_node,
                                     std::string identity_suffix) {
  auto spec = std::make_shared<const JobSpec>(graph_id, target_node,
                                              OutputSlotId("image.final"),
                                              product_job_resources(1U));
  JobAssignment assignment;
  assignment.identity.tenant_id = TenantId("tenant.issue99");
  assignment.identity.job_id = JobId("job.product." + identity_suffix);
  assignment.identity.job_spec_digest = spec->digest();
  assignment.identity.attempt_id =
      JobAttemptId("attempt.product." + identity_suffix);
  assignment.identity.worker_instance_id =
      WorkerInstanceId("worker.product." + identity_suffix);
  assignment.identity.worker_lease_generation = WorkerLeaseGeneration{1U};
  assignment.spec = std::move(spec);
  return assignment;
}

/**
 * @brief Blocks one selected cancellation observation until cancellation is
 * accepted by the coordinating test thread.
 *
 * Earlier observations return false. The selected and all later observations
 * return true only after `accept()` publishes monotonic cancellation intent.
 * This turns the worker's documented cooperative checkpoints into a
 * deterministic concurrency boundary without a product-only test hook.
 *
 * @throws std::invalid_argument when the selected observation is zero.
 * @throws std::system_error from mutex or condition-variable operations.
 * @note One manager supervision thread calls `observe()` while one test thread
 * waits and accepts cancellation. The gate must outlive both threads.
 */
class CancellationObservationGate final {
 public:
  /**
   * @brief Selects the one-based worker observation that must block.
   * @param blocking_observation Positive one-based observation ordinal.
   * @throws std::invalid_argument when the ordinal is zero.
   */
  explicit CancellationObservationGate(std::size_t blocking_observation)
      : blocking_observation_(blocking_observation) {
    if (blocking_observation_ == 0U) {
      throw std::invalid_argument("cancellation observation must be positive");
    }
  }

  /**
   * @brief Implements the worker's monotonic cancellation observer.
   * @return False before the selected checkpoint; true after test acceptance.
   * @throws std::system_error from mutex or condition-variable operations.
   */
  bool observe() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++observation_count_;
    if (observation_count_ < blocking_observation_) {
      return false;
    }
    blocked_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return cancellation_accepted_; });
    return true;
  }

  /**
   * @brief Waits a fixed bound for the selected worker checkpoint.
   * @return True when the worker reached and blocked at the checkpoint.
   * @throws std::system_error from mutex or condition-variable operations.
   */
  bool wait_until_blocked() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [&] { return blocked_; });
  }

  /**
   * @brief Publishes monotonic cancellation intent and releases the worker.
   * @return Nothing.
   * @throws std::system_error from mutex or condition-variable operations.
   */
  void accept() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancellation_accepted_ = true;
    }
    condition_.notify_all();
  }

 private:
  /** @brief One-based worker observation that forms the deterministic gate. */
  const std::size_t blocking_observation_;
  /** @brief Serializes observation, acceptance, and release state. */
  mutable std::mutex mutex_;
  /** @brief Coordinates the worker checkpoint with the accepting test. */
  mutable std::condition_variable condition_;
  /** @brief Number of cancellation observations made by the worker. */
  std::size_t observation_count_ = 0U;
  /** @brief Whether the worker reached the selected checkpoint. */
  bool blocked_ = false;
  /** @brief Monotonic cancellation intent published by the test thread. */
  bool cancellation_accepted_ = false;
};

/**
 * @brief Guarantees that a blocked cancellation observer is released.
 * @throws Nothing; an unexpected synchronization failure terminates because
 * an outstanding worker future would otherwise remain blocked.
 * @note Declare after the future so early fatal assertions release the gate
 * before the future joins during stack unwinding.
 */
class CancellationAcceptanceGuard final {
 public:
  /**
   * @brief Arms cleanup for one live cancellation gate.
   * @param gate Gate that outlives this guard.
   * @throws Nothing.
   */
  explicit CancellationAcceptanceGuard(
      CancellationObservationGate& gate) noexcept
      : gate_(&gate) {}

  /** @brief Accepts cancellation if cleanup remains armed. */
  ~CancellationAcceptanceGuard() noexcept { accept(); }

  /** @brief Prevents duplicate gate-release ownership. */
  CancellationAcceptanceGuard(const CancellationAcceptanceGuard&) = delete;
  /** @brief Prevents duplicate gate-release assignment. */
  CancellationAcceptanceGuard& operator=(const CancellationAcceptanceGuard&) =
      delete;

  /**
   * @brief Accepts cancellation immediately and disarms cleanup.
   * @return Nothing.
   * @throws Nothing; terminates on an unexpected synchronization failure.
   */
  void accept() noexcept {
    if (gate_ == nullptr) {
      return;
    }
    try {
      gate_->accept();
      gate_ = nullptr;
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /** @brief Borrowed armed gate, or null after acceptance. */
  CancellationObservationGate* gate_;
};

/**
 * @brief Trusted one-entry graph artifact resolver for product-path tests.
 * @throws std::bad_alloc when retaining paths exhausts memory.
 */
class TestGraphArtifactResolver final : public GraphArtifactResolver {
 public:
  /**
   * @brief Retains one exact accepted graph id and local test material.
   * @param accepted Exact graph artifact identity.
   * @param root Test-owned Host session root.
   * @param yaml Existing explicit YAML path.
   * @param cache Test-owned cache root.
   * @throws std::bad_alloc when copying values exhausts memory.
   */
  TestGraphArtifactResolver(GraphArtifactId accepted,
                            std::filesystem::path root,
                            std::filesystem::path yaml,
                            std::filesystem::path cache)
      : accepted_(std::move(accepted)),
        root_(std::move(root)),
        yaml_(std::move(yaml)),
        cache_(std::move(cache)) {}

  /**
   * @brief Resolves only the exact configured immutable graph identity.
   * @param graph_artifact_id Candidate JobSpec graph identity.
   * @return Trusted paths on match or typed failure diagnostic otherwise.
   * @throws std::bad_alloc when constructing result strings exhausts memory.
   */
  ResolvedGraphArtifact resolve(
      const GraphArtifactId& graph_artifact_id) const override {
    resolve_calls_.fetch_add(1U);
    if (graph_artifact_id != accepted_) {
      ResolvedGraphArtifact failed;
      failed.message = "graph artifact is not present in test authority";
      return failed;
    }
    ResolvedGraphArtifact resolved;
    resolved.ok = true;
    resolved.root_dir = root_.string();
    resolved.yaml_path = yaml_.string();
    resolved.cache_root_dir = cache_.string();
    return resolved;
  }

  /**
   * @brief Returns how many assignments reached trusted graph resolution.
   * @return Monotonic resolve call count.
   * @throws Nothing.
   */
  std::uint64_t resolve_calls() const noexcept { return resolve_calls_.load(); }

 private:
  /** @brief Sole accepted immutable graph identity. */
  GraphArtifactId accepted_;
  /** @brief Trusted Host graph-session root. */
  std::filesystem::path root_;
  /** @brief Trusted explicit graph YAML path. */
  std::filesystem::path yaml_;
  /** @brief Trusted attempt cache root. */
  std::filesystem::path cache_;
  /** @brief Monotonic count proving validation occurs before resolution. */
  mutable std::atomic<std::uint64_t> resolve_calls_{0U};
};

/**
 * @brief Builds one immutable pre-resolved external graph catalog.
 * @param graph_id Exact authorized graph identity.
 * @param root Trusted graph-session root.
 * @param yaml Trusted explicit YAML path, which need not be open yet.
 * @param cache Trusted worker-private cache root.
 * @return Non-null one-entry catalog safe for manager-side memory lookup.
 * @throws Contract, path conversion, or allocation failures unchanged.
 * @note Filesystem opening remains worker-process work after exec.
 */
std::shared_ptr<const PreparedExternalGraphCatalog> prepared_graph_catalog(
    GraphArtifactId graph_id, const std::filesystem::path& root,
    const std::filesystem::path& yaml, const std::filesystem::path& cache) {
  ResolvedGraphArtifact graph;
  graph.ok = true;
  graph.root_dir = root.string();
  graph.yaml_path = yaml.string();
  graph.cache_root_dir = cache.string();
  std::vector<PreparedExternalGraphEntry> entries;
  entries.push_back(
      PreparedExternalGraphEntry{std::move(graph_id), std::move(graph)});
  return std::make_shared<const PreparedExternalGraphCatalog>(
      std::move(entries));
}

TEST(SingleTenantJobProductPath,
     RealEmbeddedHostCompletesCheckpointAndRestartDurableArtifactPath) {
  ScopedJobProductRoot product_root;
  const std::filesystem::path yaml = product_root.root() / "graph.yaml";
  write_job_graph(yaml);

  const GraphArtifactId graph_id("graph.issue99.fixture");
  auto catalog =
      prepared_graph_catalog(graph_id, product_root.root() / "sessions", yaml,
                             product_root.root() / "cache");
  auto factory = std::make_shared<EmbeddedHostJobWorkerFactory>(catalog);
  const std::filesystem::path state_root = product_root.root() / "state";
  JobId producer_job_id;
  JobId consumer_job_id;
  OutputCommitReceipt receipt;
  {
    SingleTenantJobService service(TenantId("tenant.issue99"),
                                   product_quota_limits(), state_root, factory,
                                   {}, {}, product_worker_options());
    const JobSubmission submission = service.submit(JobSpec(
        graph_id, 0, OutputSlotId("image.final"), product_job_resources()));
    producer_job_id = submission.job_id;
    const std::optional<JobSnapshot> terminal =
        service.wait_for(submission.job_id, std::chrono::seconds(10));
    ASSERT_TRUE(terminal.has_value());
    ASSERT_EQ(terminal->state, JobState::Succeeded) << terminal->message;
    EXPECT_TRUE(terminal->attempt_settled);
    EXPECT_EQ(terminal->attempt_outcome, JobAttemptOutcome::Succeeded);
    EXPECT_EQ(terminal->assignment, submission.assignment);
    EXPECT_EQ(terminal->spec->digest(), submission.job_spec_digest);
    ASSERT_TRUE(terminal->output_receipt.has_value());

    receipt = *terminal->output_receipt;
    EXPECT_EQ(receipt.attempt, submission.assignment);
    EXPECT_EQ(receipt.output_slot_id, OutputSlotId("image.final"));
    EXPECT_EQ(receipt.achieved_durability, ArtifactDurability::CrashDurable);
    EXPECT_EQ(receipt.descriptor.width, 8);
    EXPECT_EQ(receipt.descriptor.height, 6);
    EXPECT_EQ(receipt.descriptor.channels, 4);
    EXPECT_EQ(receipt.descriptor.type, DataType::FLOAT32);
    EXPECT_EQ(receipt.descriptor.row_bytes, 8U * 4U * sizeof(float));
    EXPECT_EQ(receipt.descriptor.payload_bytes, 8U * 6U * 4U * sizeof(float));

    const std::shared_ptr<const ArtifactRecord> artifact =
        service.find_artifact(receipt.artifact_id);
    ASSERT_NE(artifact, nullptr);
    EXPECT_EQ(artifact->receipt.artifact_id, receipt.artifact_id);
    EXPECT_EQ(artifact->receipt.content_digest, receipt.content_digest);
    EXPECT_EQ(artifact->payload.size(), receipt.descriptor.payload_bytes);

    const JobSubmission consumer =
        service.submit(JobSpec(graph_id, 0, OutputSlotId("image.final"),
                               product_job_resources(1U), receipt.artifact_id));
    consumer_job_id = consumer.job_id;
    const std::optional<JobSnapshot> consumed =
        service.wait_for(consumer.job_id, std::chrono::seconds(10));
    ASSERT_TRUE(consumed.has_value());
    ASSERT_EQ(consumed->state, JobState::Succeeded) << consumed->message;
    ASSERT_TRUE(consumed->spec->checkpoint_artifact_id().has_value());
    EXPECT_EQ(*consumed->spec->checkpoint_artifact_id(), receipt.artifact_id);
  }

  SingleTenantJobService recovered(TenantId("tenant.issue99"),
                                   product_quota_limits(), state_root, factory,
                                   {}, {}, product_worker_options());
  const std::optional<JobSnapshot> producer = recovered.query(producer_job_id);
  const std::optional<JobSnapshot> consumer = recovered.query(consumer_job_id);
  ASSERT_TRUE(producer.has_value());
  ASSERT_TRUE(consumer.has_value());
  EXPECT_EQ(producer->state, JobState::Succeeded);
  EXPECT_EQ(consumer->state, JobState::Succeeded);
  ASSERT_TRUE(producer->output_receipt.has_value());
  EXPECT_EQ(producer->output_receipt->artifact_id, receipt.artifact_id);
  EXPECT_NE(recovered.find_artifact(receipt.artifact_id), nullptr);
  EXPECT_EQ(recovered.quota_snapshot().retained_artifacts, 2U);
}

TEST(SingleTenantJobProductPath, MissingGraphArtifactFailsBeforeHostExecution) {
  ScopedJobProductRoot product_root;
  const std::filesystem::path yaml = product_root.root() / "graph.yaml";
  write_job_graph(yaml);

  auto catalog = prepared_graph_catalog(GraphArtifactId("graph.present"),
                                        product_root.root() / "sessions", yaml,
                                        product_root.root() / "cache");
  auto factory = std::make_shared<EmbeddedHostJobWorkerFactory>(catalog);
  SingleTenantJobService service(
      TenantId("tenant.issue99"), product_quota_limits(),
      product_root.root() / "state", factory, {}, {}, product_worker_options());
  const JobSubmission submission = service.submit(
      JobSpec(GraphArtifactId("graph.absent"), 0, OutputSlotId("image.final"),
              product_job_resources(1U)));

  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(5));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  EXPECT_TRUE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::GraphResolution);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

TEST(SingleTenantJobProductPath,
     ComputeFailureAfterConcurrentCancellationRemainsFailed) {
  ScopedJobProductRoot product_root;
  const std::filesystem::path yaml = product_root.root() / "graph.yaml";
  write_job_graph(yaml);

  const GraphArtifactId graph_id("graph.compute.failure.race");
  auto resolver = std::make_shared<TestGraphArtifactResolver>(
      graph_id, product_root.root() / "sessions", yaml,
      product_root.root() / "cache");
  const JobAssignment assignment =
      make_worker_assignment(graph_id, 99, "compute.failure.race");

  EmbeddedHostJobWorker baseline_worker(resolver);
  const JobAttemptReport baseline =
      baseline_worker.execute(assignment, [] { return false; });
  ASSERT_EQ(baseline.outcome, JobAttemptOutcome::Failed);
  ASSERT_TRUE(baseline.settled);
  ASSERT_EQ(baseline.failure, JobAttemptFailure::Compute);
  ASSERT_EQ(baseline.message,
            "graph compute failed [not_found]: Node 99 not found in graph.");
  ASSERT_FALSE(baseline.image.has_value());

  EmbeddedHostJobWorker racing_worker(resolver);
  CancellationObservationGate gate(kAfterComputeCancellationObservation);
  std::future<JobAttemptReport> report_future;
  CancellationAcceptanceGuard cancellation_acceptance(gate);
  report_future = std::async(std::launch::async, [&] {
    return racing_worker.execute(assignment, [&] { return gate.observe(); });
  });

  ASSERT_TRUE(gate.wait_until_blocked());
  cancellation_acceptance.accept();
  const JobAttemptReport report = report_future.get();
  EXPECT_EQ(report.identity, assignment.identity);
  EXPECT_EQ(report.outcome, JobAttemptOutcome::Failed);
  EXPECT_TRUE(report.settled);
  EXPECT_EQ(report.failure, JobAttemptFailure::Compute);
  EXPECT_EQ(report.message, baseline.message);
  EXPECT_FALSE(report.image.has_value());
}

TEST(SingleTenantJobProductPath,
     PreComputeCancellationDoesNotBecomeMissingOutputFailure) {
  ScopedJobProductRoot product_root;
  const std::filesystem::path yaml = product_root.root() / "graph.yaml";
  write_job_graph(yaml);

  const GraphArtifactId graph_id("graph.precompute.cancel");
  auto resolver = std::make_shared<TestGraphArtifactResolver>(
      graph_id, product_root.root() / "sessions", yaml,
      product_root.root() / "cache");
  const JobAssignment assignment =
      make_worker_assignment(graph_id, 0, "precompute.cancel");
  EmbeddedHostJobWorker worker(resolver);
  CancellationObservationGate gate(kBeforeComputeCancellationObservation);
  std::future<JobAttemptReport> report_future;
  CancellationAcceptanceGuard cancellation_acceptance(gate);
  report_future = std::async(std::launch::async, [&] {
    return worker.execute(assignment, [&] { return gate.observe(); });
  });

  ASSERT_TRUE(gate.wait_until_blocked());
  cancellation_acceptance.accept();
  const JobAttemptReport report = report_future.get();
  EXPECT_EQ(report.identity, assignment.identity);
  EXPECT_EQ(report.outcome, JobAttemptOutcome::Cancelled);
  EXPECT_TRUE(report.settled);
  EXPECT_EQ(report.failure, JobAttemptFailure::CancellationObserved);
  EXPECT_EQ(report.message, "cancellation observed before artifact commit");
  EXPECT_FALSE(report.image.has_value());
}

TEST(SingleTenantJobProductPath, MutatedAssignmentDigestFailsBeforeResolution) {
  ScopedJobProductRoot product_root;
  const std::filesystem::path yaml = product_root.root() / "graph.yaml";
  write_job_graph(yaml);

  auto resolver = std::make_shared<TestGraphArtifactResolver>(
      GraphArtifactId("graph.present"), product_root.root() / "sessions", yaml,
      product_root.root() / "cache");
  auto spec = std::make_shared<const JobSpec>(GraphArtifactId("graph.present"),
                                              0, OutputSlotId("image.final"),
                                              product_job_resources(1U));
  JobAssignment assignment;
  assignment.identity.tenant_id = TenantId("tenant.issue99");
  assignment.identity.job_id = JobId("job.digest.test");
  assignment.identity.job_spec_digest = spec->digest();
  assignment.identity.job_spec_digest.bytes[0] ^= std::byte{0x01};
  assignment.identity.attempt_id = JobAttemptId("attempt.digest.test");
  assignment.identity.worker_instance_id =
      WorkerInstanceId("worker.digest.test");
  assignment.identity.worker_lease_generation = WorkerLeaseGeneration{1U};
  assignment.spec = std::move(spec);

  EmbeddedHostJobWorker worker(resolver);
  const JobAttemptReport report =
      worker.execute(assignment, [] { return false; });
  EXPECT_EQ(report.outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(report.failure, JobAttemptFailure::InvalidAssignment);
  EXPECT_TRUE(report.settled);
  EXPECT_EQ(resolver->resolve_calls(), 0U);
}

}  // namespace
}  // namespace ps::server
