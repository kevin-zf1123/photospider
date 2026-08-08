/**
 * @file test_single_tenant_job_product_path.cpp
 * @brief Verifies Issue #98 through the real Embedded Host product path.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "server/embedded_job_worker.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

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
         << "  name: issue98_coordinate_pattern\n"
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

TEST(SingleTenantJobProductPath,
     RealEmbeddedHostCompletesWithIdentityBoundArtifact) {
  ScopedJobProductRoot product_root;
  const std::filesystem::path yaml = product_root.root() / "graph.yaml";
  write_job_graph(yaml);

  const GraphArtifactId graph_id("graph.issue98.fixture");
  auto resolver = std::make_shared<TestGraphArtifactResolver>(
      graph_id, product_root.root() / "sessions", yaml,
      product_root.root() / "cache");
  auto factory = std::make_shared<EmbeddedHostJobWorkerFactory>(resolver);
  SingleTenantJobService service(TenantId("tenant.issue98"), factory);

  const JobSubmission submission =
      service.submit(JobSpec(graph_id, 0, OutputSlotId("image.final"), 2U));
  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(10));
  ASSERT_TRUE(terminal.has_value());
  ASSERT_EQ(terminal->state, JobState::Succeeded) << terminal->message;
  EXPECT_TRUE(terminal->attempt_settled);
  EXPECT_EQ(terminal->attempt_outcome, JobAttemptOutcome::Succeeded);
  EXPECT_EQ(terminal->assignment, submission.assignment);
  EXPECT_EQ(terminal->spec->digest(), submission.job_spec_digest);
  ASSERT_TRUE(terminal->output_receipt.has_value());

  const OutputCommitReceipt& receipt = *terminal->output_receipt;
  EXPECT_EQ(receipt.attempt, submission.assignment);
  EXPECT_EQ(receipt.output_slot_id, OutputSlotId("image.final"));
  EXPECT_EQ(receipt.achieved_durability, ArtifactDurability::ProcessLifetime);
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
}

TEST(SingleTenantJobProductPath, MissingGraphArtifactFailsBeforeHostExecution) {
  ScopedJobProductRoot product_root;
  const std::filesystem::path yaml = product_root.root() / "graph.yaml";
  write_job_graph(yaml);

  auto resolver = std::make_shared<TestGraphArtifactResolver>(
      GraphArtifactId("graph.present"), product_root.root() / "sessions", yaml,
      product_root.root() / "cache");
  auto factory = std::make_shared<EmbeddedHostJobWorkerFactory>(resolver);
  SingleTenantJobService service(TenantId("tenant.issue98"), factory);
  const JobSubmission submission = service.submit(JobSpec(
      GraphArtifactId("graph.absent"), 0, OutputSlotId("image.final"), 1U));

  const std::optional<JobSnapshot> terminal =
      service.wait_for(submission.job_id, std::chrono::seconds(5));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Failed);
  EXPECT_TRUE(terminal->attempt_settled);
  EXPECT_EQ(terminal->failure, JobAttemptFailure::GraphResolution);
  EXPECT_FALSE(terminal->output_receipt.has_value());
}

TEST(SingleTenantJobProductPath, MutatedAssignmentDigestFailsBeforeResolution) {
  ScopedJobProductRoot product_root;
  const std::filesystem::path yaml = product_root.root() / "graph.yaml";
  write_job_graph(yaml);

  auto resolver = std::make_shared<TestGraphArtifactResolver>(
      GraphArtifactId("graph.present"), product_root.root() / "sessions", yaml,
      product_root.root() / "cache");
  auto spec = std::make_shared<const JobSpec>(
      GraphArtifactId("graph.present"), 0, OutputSlotId("image.final"), 1U);
  JobAssignment assignment;
  assignment.identity.tenant_id = TenantId("tenant.issue98");
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
