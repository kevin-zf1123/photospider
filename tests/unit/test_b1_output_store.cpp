/**
 * @file test_b1_output_store.cpp
 * @brief Verifies immutable crash-durable B1 artifact publication.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "benchmark/b1_output_store.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {
namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;

/**
 * @brief Owns one unique output root and removes it after each test.
 * @throws Filesystem failures when the root cannot be created.
 * @note Cleanup is best effort and targets only the exact owned path.
 */
class ScopedB1OutputRoot final {
 public:
  /** @brief Creates one process-local unique empty output root. */
  ScopedB1OutputRoot() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("photospider-b1-output-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
    if (!std::filesystem::create_directory(root_)) {
      throw std::runtime_error("failed to create B1 output test root");
    }
  }

  /** @brief Removes the exact owned output root without throwing. */
  ~ScopedB1OutputRoot() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /** @brief Prevents duplicate cleanup ownership. */
  ScopedB1OutputRoot(const ScopedB1OutputRoot&) = delete;

  /** @brief Prevents duplicate cleanup assignment. */
  ScopedB1OutputRoot& operator=(const ScopedB1OutputRoot&) = delete;

  /**
   * @brief Returns the existing canonicalizable root.
   * @return Borrowed path valid for this owner lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Exact recursively cleaned path. */
  std::filesystem::path root_;
};

/**
 * @brief Returns one immutable measured occurrence for seed zero.
 * @return Complete valid cap-eight job identity.
 * @throws Nothing except owned-string allocation.
 */
B1JobInstance measured_job_zero() {
  return B1JobInstance{kB1WorkloadId, 1U, B1JobPhase::Measured, 0U, 0U, 8U};
}

/**
 * @brief Lazily creates the exact 64 MiB seed-zero oracle once per process.
 * @return Borrowed immutable candidate descriptor.
 * @throws Oracle or allocation failures on first call.
 * @note Tests never mutate the shared payload.
 */
const ImageBuffer& seed_zero_image() {
  static const ImageBuffer image = generate_b1_oracle_image(0U);
  return image;
}

/**
 * @brief Reads one small canonical manifest as binary bytes.
 * @param path Existing published manifest.
 * @return Complete file contents.
 * @throws std::runtime_error when the file cannot be opened/read.
 */
std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open B1 test manifest");
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  if (!input.eof() && input.fail()) {
    throw std::runtime_error("failed to read B1 test manifest");
  }
  return bytes.str();
}

/**
 * @brief Proves invalid policy/image requests fail before filesystem mutation.
 * @throws Test framework, executor, and filesystem failures unchanged.
 */
TEST(B1OutputStore, RejectsWeakerDurabilityAndInvalidCandidateFailClosed) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  const B1JobInstance job = measured_job_zero();

  B1OutputStore weaker(
      output_root.root(), executor,
      B1OutputStoreOptions{B1OutputDurability::AtomicVisible, true});
  EXPECT_EQ(weaker.commit(job, ImageBuffer{}).status,
            B1OutputCommitStatus::InvalidRequest);

  B1OutputStore unsupported(
      output_root.root(), executor,
      B1OutputStoreOptions{B1OutputDurability::CrashDurable, false});
  EXPECT_EQ(unsupported.commit(job, ImageBuffer{}).status,
            B1OutputCommitStatus::DurabilityUnsupported);

  B1OutputStore store(output_root.root(), executor);
  EXPECT_EQ(store.commit(job, ImageBuffer{}).status,
            B1OutputCommitStatus::InvalidImage);
  EXPECT_TRUE(std::filesystem::is_empty(output_root.root()));
}

/**
 * @brief Proves exact payload/manifest durability receipt and no-replace slot.
 * @throws Test framework, oracle, filesystem, and executor failures unchanged.
 */
TEST(B1OutputStore, CommitsExactPayloadAndPublishesManifestLastWithoutReplace) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  B1OutputStore store(output_root.root(), executor);
  const B1JobInstance job = measured_job_zero();

  const B1OutputCommitResult committed = store.commit(job, seed_zero_image());
  ASSERT_TRUE(committed.succeeded()) << committed.diagnostic;
  ASSERT_TRUE(committed.receipt.has_value());
  const B1OutputCommitReceipt& receipt = *committed.receipt;
  const B1JobGolden golden = b1_frozen_job_golden(0U);
  EXPECT_EQ(receipt.job, job);
  EXPECT_EQ(receipt.resolved_root,
            std::filesystem::canonical(output_root.root()));
  EXPECT_EQ(receipt.commit_id.size(), 64U);
  EXPECT_EQ(receipt.logical_content_digest, golden.logical_digest);
  EXPECT_EQ(receipt.payload_digest, golden.raw_payload_digest);
  EXPECT_EQ(receipt.payload_length, kB1PayloadBytes);
  EXPECT_EQ(receipt.manifest_length, b1_manifest_length(0U));
  EXPECT_EQ(receipt.requested_durability, B1OutputDurability::CrashDurable);
  EXPECT_EQ(receipt.achieved_durability, B1OutputDurability::CrashDurable);
  EXPECT_FALSE(receipt.published_manifest_identity.empty());

  const std::filesystem::path slot =
      receipt.resolved_root / receipt.rooted_slot;
  const std::filesystem::path payload = slot / receipt.payload_name;
  const std::filesystem::path manifest = slot / receipt.manifest_name;
  EXPECT_EQ(std::filesystem::file_size(payload), kB1PayloadBytes);
  EXPECT_EQ(std::filesystem::file_size(manifest), receipt.manifest_length);
  const std::string expected_manifest =
      b1_artifact_manifest(0U, golden.raw_payload_digest);
  EXPECT_EQ(read_text_file(manifest), expected_manifest);
  EXPECT_EQ(receipt.manifest_digest, b1_sha256(expected_manifest));
  EXPECT_FALSE(std::filesystem::exists(slot / ".manifest.private"));

  ASSERT_EQ(committed.io_observations.size(), 6U);
  EXPECT_EQ(committed.io_observations.front().point,
            B1IoObservationPoint::Initial);
  EXPECT_EQ(committed.io_observations.back().point,
            B1IoObservationPoint::Final);
  EXPECT_EQ(committed.io_observations.back().snapshot.active_tasks, 0U);
  EXPECT_EQ(committed.io_observations.back().snapshot.active_planned_bytes, 0U);

  const B1OutputCommitResult duplicate = store.commit(job, seed_zero_image());
  EXPECT_EQ(duplicate.status, B1OutputCommitStatus::SlotExists);
  EXPECT_FALSE(duplicate.receipt.has_value());
  EXPECT_EQ(std::filesystem::file_size(payload), kB1PayloadBytes);
  EXPECT_EQ(read_text_file(manifest), expected_manifest);
}

/**
 * @brief Proves capacity retries preserve one attempt-zero identity and charge.
 * @throws Test framework, oracle, synchronization, and filesystem failures.
 */
TEST(B1OutputStore, CapacityRetryKeepsStableAttemptAndPlannedByteCharge) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor({1U, kB1ComputeIoPlannedByteLimit});
  const auto blocker_lifetime = std::make_shared<int>(1);
  std::promise<void> entered;
  std::shared_future<void> entered_future = entered.get_future().share();
  std::promise<void> release;
  std::shared_future<void> release_future = release.get_future().share();
  const execution::ComputeIoSubmission blocker =
      executor.try_submit(1U, blocker_lifetime, [&entered, release_future]() {
        return execution::ComputeIoExecutor::Task([&entered, release_future]() {
          entered.set_value();
          release_future.wait();
        });
      });
  ASSERT_TRUE(blocker.accepted());
  ASSERT_EQ(entered_future.wait_for(2s), std::future_status::ready);

  B1OutputStore store(output_root.root(), executor);
  const B1JobInstance job = measured_job_zero();
  std::future<B1OutputCommitResult> pending = std::async(
      std::launch::async,
      [&store, &job]() { return store.commit(job, seed_zero_image()); });

  const auto slot_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::filesystem::is_empty(output_root.root()) &&
         std::chrono::steady_clock::now() < slot_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_FALSE(std::filesystem::is_empty(output_root.root()));
  std::this_thread::sleep_for(5ms);
  release.set_value();
  EXPECT_EQ(blocker.completion().wait().status(),
            execution::ComputeIoCompletionStatus::Succeeded);
  ASSERT_EQ(pending.wait_for(10s), std::future_status::ready);
  const B1OutputCommitResult committed = pending.get();
  ASSERT_TRUE(committed.succeeded()) << committed.diagnostic;

  std::size_t rejection_count = 0U;
  for (const B1ComputeIoObservation& observation : committed.io_observations) {
    if (observation.point != B1IoObservationPoint::OfferRejected) {
      continue;
    }
    ++rejection_count;
    ASSERT_TRUE(observation.task.has_value());
    EXPECT_EQ(observation.task->job, job);
    EXPECT_EQ(observation.task->stage, B1IoStage::PayloadStage);
    EXPECT_EQ(observation.task->attempt, 0U);
    EXPECT_EQ(observation.planned_bytes, kB1PayloadBytes);
    EXPECT_EQ(observation.admission,
              execution::ComputeIoAdmissionStatus::TaskLimit);
  }
  EXPECT_GT(rejection_count, 0U);
  EXPECT_EQ(committed.io_observations.back().point,
            B1IoObservationPoint::Final);
}

}  // namespace
}  // namespace ps::benchmark
