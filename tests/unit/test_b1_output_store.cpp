/**
 * @file test_b1_output_store.cpp
 * @brief Verifies immutable crash-durable B1 artifact publication.
 */
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "benchmark/b1/b1_output_store.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {
namespace {

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
 * @brief Test-only context that releases one blocking executor task on retry.
 * @throws Nothing for aggregate construction.
 */
struct CapacityRetryReleaseContext final {
  /** @brief Promise whose publication releases the blocking callback. */
  std::promise<void>* release = nullptr;
  /** @brief Accepted blocker whose settlement is awaited before retry. */
  const execution::ComputeIoSubmission* blocker = nullptr;
  /** @brief Ensures only the first rejection performs release. */
  std::atomic<bool> released{false};
  /** @brief Sticky observer failure reported after commit returns. */
  std::atomic<bool> failed{false};
};

/**
 * @brief Distinguishable exception thrown by deterministic transaction seams.
 * @throws Standard runtime-error allocation failures at construction.
 */
class B1OutputFaultSentinel final : public std::runtime_error {
 public:
  /** @brief Creates the stable injected failure. */
  B1OutputFaultSentinel() : std::runtime_error("injected B1 output failure") {}
};

/**
 * @brief Selects one throwing transaction boundary for a focused test.
 * @throws Nothing for aggregate construction.
 */
struct ThrowingFaultContext final {
  /** @brief Exact boundary that throws while enabled. */
  B1OutputStoreFaultPoint selected = B1OutputStoreFaultPoint::AfterSlotCreated;
  /** @brief Whether the injected exception remains armed. */
  bool enabled = true;
};

/**
 * @brief Throws exactly at one selected output transaction boundary.
 * @param opaque Borrowed `ThrowingFaultContext`.
 * @param point Boundary reached by the store.
 * @return Nothing outside the selected enabled boundary.
 * @throws B1OutputFaultSentinel at the selected enabled boundary.
 */
void throw_selected_output_fault(void* opaque, B1OutputStoreFaultPoint point) {
  auto* context = static_cast<ThrowingFaultContext*>(opaque);
  if (context != nullptr && context->enabled && context->selected == point) {
    throw B1OutputFaultSentinel();
  }
}

/**
 * @brief Owns and restores one deterministic root-path replacement scenario.
 * @throws Filesystem allocation failures while staging sibling paths.
 */
class RootReplacementContext final {
 public:
  /**
   * @brief Derives exact displaced/replacement siblings for one test root.
   * @param root Existing test-owned root path.
   * @throws std::bad_alloc when path storage cannot allocate.
   */
  explicit RootReplacementContext(std::filesystem::path root)
      : root_(std::move(root)),
        displaced_(root_.string() + "-displaced"),
        replacement_(root_.string() + "-replacement") {}

  /** @brief Restores/removes only the three exact test-owned paths. */
  ~RootReplacementContext() noexcept { restore(); }

  /** @brief Cleanup ownership cannot be copied. */
  RootReplacementContext(const RootReplacementContext&) = delete;

  /** @brief Cleanup ownership cannot be assigned. */
  RootReplacementContext& operator=(const RootReplacementContext&) = delete;

  /**
   * @brief Replaces the selected root path with a symlink to a fresh sibling.
   * @return Nothing after the original directory is displaced.
   * @throws Filesystem errors unchanged.
   */
  void replace() {
    if (!enabled_ || replaced_) {
      return;
    }
    std::filesystem::rename(root_, displaced_);
    if (!std::filesystem::create_directory(replacement_)) {
      throw std::runtime_error("failed to create replacement B1 root");
    }
    std::filesystem::create_directory_symlink(replacement_, root_);
    replaced_ = true;
  }

  /**
   * @brief Restores the original inode to its selected path and removes target.
   * @return Nothing.
   * @throws Nothing; cleanup is best effort and restricted to owned siblings.
   */
  void restore() noexcept {
    if (!replaced_) {
      return;
    }
    std::error_code error;
    std::filesystem::remove(root_, error);
    error.clear();
    std::filesystem::rename(displaced_, root_, error);
    error.clear();
    std::filesystem::remove_all(replacement_, error);
    replaced_ = false;
    enabled_ = false;
  }

  /** @brief Returns the displaced original directory. */
  const std::filesystem::path& displaced() const noexcept { return displaced_; }

  /** @brief Returns the replacement symlink target directory. */
  const std::filesystem::path& replacement() const noexcept {
    return replacement_;
  }

 private:
  /** @brief Selected path whose leaf is temporarily replaced. */
  std::filesystem::path root_;
  /** @brief Temporary path retaining the original held inode. */
  std::filesystem::path displaced_;
  /** @brief Fresh directory that must receive no artifact writes. */
  std::filesystem::path replacement_;
  /** @brief Whether replacement currently requires restoration. */
  bool replaced_ = false;
  /** @brief Whether the one-shot replacement remains armed. */
  bool enabled_ = true;
};

/**
 * @brief Applies one root replacement at the post-verification test boundary.
 * @param opaque Borrowed `RootReplacementContext`.
 * @param point Boundary reached by the store.
 * @return Nothing outside the root-binding boundary.
 * @throws Filesystem errors from deterministic replacement unchanged.
 */
void replace_root_after_verification(void* opaque,
                                     B1OutputStoreFaultPoint point) {
  auto* context = static_cast<RootReplacementContext*>(opaque);
  if (context != nullptr &&
      point == B1OutputStoreFaultPoint::AfterRootBindingVerified) {
    context->replace();
  }
}

/**
 * @brief Computes the exact deterministic commit id for one test occurrence.
 * @param job Complete immutable occurrence identity.
 * @return Lowercase SHA-256 commit id used by all private/public names.
 * @throws Digest, validation, or allocation failures unchanged.
 */
std::string commit_id_for_job(const B1JobInstance& job) {
  B1Sha256 hash;
  hash.update("execution-profile-output-commit-id-v1\n");
  hash.update(encode_b1_job_instance(job));
  return b1_digest_hex(hash.finish());
}

/**
 * @brief Computes the exact private staging anchor for one test occurrence.
 * @param root Existing selected output root.
 * @param job Complete immutable occurrence identity.
 * @return Root plus the exact `.b1-staging-<commit-id>` leaf.
 * @throws Digest, validation, or allocation failures unchanged.
 */
std::filesystem::path private_anchor_for_job(const std::filesystem::path& root,
                                             const B1JobInstance& job) {
  return root / (".b1-staging-" + commit_id_for_job(job));
}

/**
 * @brief Computes the exact private slot for one test occurrence.
 * @param root Existing selected output root.
 * @param job Complete immutable occurrence identity.
 * @return Exact private anchor plus `slot`.
 * @throws Digest, validation, or allocation failures unchanged.
 */
std::filesystem::path private_slot_for_job(const std::filesystem::path& root,
                                           const B1JobInstance& job) {
  return private_anchor_for_job(root, job) / "slot";
}

/**
 * @brief Closed filesystem mutation selected at an output-store fault seam.
 * @throws Nothing for value construction and comparison.
 */
enum class OutputNamespaceMutation : std::uint8_t {
  /** @brief Replace the just-created pre-guard private staging anchor. */
  ReplacePrivateAnchor,
  /** @brief Replace the just-created private slot with a real directory. */
  ReplacePrivateSlot,
  /** @brief Install a real public destination immediately before publication.
   */
  InstallPublicSlot,
  /** @brief Add one unowned extra leaf before strict rollback. */
  AddExtraLeaf,
  /** @brief Replace the owned payload with a directory before rollback. */
  ReplacePayloadType,
};

/**
 * @brief Test-owned namespace mutation and optional post-mutation exception.
 * @throws Nothing for aggregate construction except path/string allocation.
 */
struct OutputNamespaceMutationContext final {
  /** @brief Existing test output root. */
  std::filesystem::path root;
  /** @brief Exact deterministic private staging anchor. */
  std::filesystem::path private_anchor;
  /** @brief Exact deterministic private slot. */
  std::filesystem::path private_slot;
  /** @brief Exact public occurrence path for publication collision tests. */
  std::filesystem::path public_slot;
  /** @brief Exact path receiving the fresh replacement object. */
  std::filesystem::path replacement_namespace;
  /** @brief Exact path retaining the displaced original inode. */
  std::filesystem::path displaced_namespace;
  /** @brief Selected deterministic mutation. */
  OutputNamespaceMutation mutation =
      OutputNamespaceMutation::ReplacePrivateSlot;
  /** @brief Boundary at which the mutation is performed. */
  B1OutputStoreFaultPoint point =
      B1OutputStoreFaultPoint::AfterStagingSlotMkdirBeforeOpen;
  /** @brief Whether to throw after mutation to enter rollback. */
  bool throw_after = false;
  /** @brief Exact-once guard for repeated task-settlement boundaries. */
  bool mutated = false;
};

/**
 * @brief Applies one deterministic real-filesystem namespace mutation.
 * @param opaque Borrowed `OutputNamespaceMutationContext`.
 * @param point Exact store seam.
 * @return Nothing outside the selected or already-used seam.
 * @throws Filesystem failures or `B1OutputFaultSentinel` after mutation.
 */
void mutate_output_namespace(void* opaque, B1OutputStoreFaultPoint point) {
  auto* context = static_cast<OutputNamespaceMutationContext*>(opaque);
  if (context == nullptr || context->mutated || context->point != point) {
    return;
  }
  context->mutated = true;
  switch (context->mutation) {
    case OutputNamespaceMutation::ReplacePrivateAnchor:
      if (context->replacement_namespace != context->private_anchor ||
          context->displaced_namespace.empty()) {
        throw std::runtime_error("B1 anchor replacement paths are incomplete");
      }
      std::filesystem::rename(context->replacement_namespace,
                              context->displaced_namespace);
      if (!std::filesystem::create_directory(context->replacement_namespace)) {
        throw std::runtime_error("failed to replace private B1 anchor");
      }
      std::ofstream(context->replacement_namespace / "replacement-marker")
          << "replacement";
      break;
    case OutputNamespaceMutation::ReplacePrivateSlot:
      if (context->replacement_namespace != context->private_slot ||
          context->displaced_namespace.empty()) {
        throw std::runtime_error("B1 slot replacement paths are incomplete");
      }
      std::filesystem::rename(context->replacement_namespace,
                              context->displaced_namespace);
      if (!std::filesystem::create_directory(context->replacement_namespace)) {
        throw std::runtime_error("failed to replace private B1 slot");
      }
      std::ofstream(context->replacement_namespace / "replacement-marker")
          << "replacement";
      break;
    case OutputNamespaceMutation::InstallPublicSlot:
      if (!std::filesystem::create_directory(context->public_slot)) {
        throw std::runtime_error("failed to install public B1 slot collision");
      }
      std::ofstream(context->public_slot / "replacement-marker")
          << "replacement";
      break;
    case OutputNamespaceMutation::AddExtraLeaf:
      std::ofstream(context->private_slot / "unowned-extra") << "extra";
      break;
    case OutputNamespaceMutation::ReplacePayloadType:
      std::filesystem::rename(context->private_slot / "output.rgba32le",
                              context->private_slot / "displaced-payload");
      if (!std::filesystem::create_directory(context->private_slot /
                                             "output.rgba32le")) {
        throw std::runtime_error("failed to replace B1 payload type");
      }
      break;
  }
  if (context->throw_after) {
    throw B1OutputFaultSentinel();
  }
}

/**
 * @brief Closed mutation/error selected at one strict-cleanup seam.
 * @throws Nothing for value construction and comparison.
 */
enum class CleanupMutation : std::uint8_t {
  /** @brief Return a caller-selected errno without invoking unlink. */
  InjectErrno,
  /** @brief Rename the exact inode away and back under the same name. */
  RestoreSameIdentity,
  /** @brief Replace the exact name with a different regular-file inode. */
  InstallDifferentIdentity,
  /** @brief Replace the final-checked name, then fail before store removal. */
  InstallDifferentIdentityThenFail,
};

/**
 * @brief Test state for identity-safe strict-cleanup mutation and accounting.
 * @throws Nothing for aggregate construction except path allocation.
 */
struct CleanupMutationContext final {
  /** @brief Existing test output root used for settlement marker evidence. */
  std::filesystem::path root;
  /** @brief Exact deterministic private slot targeted by cleanup mutation. */
  std::filesystem::path private_slot;
  /** @brief Executor whose accepted charge must already be settled. */
  execution::ComputeIoExecutor* executor = nullptr;
  /** @brief Exact cleanup object at which to inject. */
  B1OutputStoreCleanupOperation selected =
      B1OutputStoreCleanupOperation::PayloadLeaf;
  /** @brief Selected cleanup mutation. */
  CleanupMutation mutation = CleanupMutation::InjectErrno;
  /** @brief Errno returned for `InjectErrno`. */
  int injected_errno = EIO;
  /** @brief Exact-once guard. */
  bool invoked = false;
  /** @brief True when the executor carried no active transaction charge. */
  bool charge_was_settled = false;
};

/**
 * @brief Injects one cleanup errno or real same/different-identity race.
 * @param opaque Borrowed `CleanupMutationContext`.
 * @param operation Exact identity-verified cleanup object.
 * @return Zero after a namespace mutation, selected errno for injected error.
 * @throws Nothing; filesystem failures collapse to `EIO`.
 */
int mutate_strict_cleanup(void* opaque,
                          B1OutputStoreCleanupOperation operation) noexcept {
  auto* context = static_cast<CleanupMutationContext*>(opaque);
  if (context == nullptr || context->invoked ||
      context->selected != operation) {
    return 0;
  }
  context->invoked = true;
  if (context->executor != nullptr) {
    const execution::ComputeIoExecutorSnapshot snapshot =
        context->executor->snapshot();
    context->charge_was_settled =
        snapshot.active_tasks == 0U && snapshot.active_planned_bytes == 0U;
  }
  if (!context->charge_was_settled) {
    return EBUSY;
  }
  if (context->mutation != CleanupMutation::RestoreSameIdentity) {
    std::ofstream(context->root / ".cleanup-charge-settled") << "settled";
  }
  if (context->mutation == CleanupMutation::InjectErrno) {
    return context->injected_errno;
  }
  try {
    const std::filesystem::path payload =
        context->private_slot / "output.rgba32le";
    const std::filesystem::path displaced =
        context->private_slot / "payload-displaced";
    std::filesystem::rename(payload, displaced);
    if (context->mutation == CleanupMutation::RestoreSameIdentity) {
      std::filesystem::rename(displaced, payload);
      return 0;
    }
    std::ofstream(payload) << "different-identity";
    if (context->mutation ==
        CleanupMutation::InstallDifferentIdentityThenFail) {
      return context->injected_errno;
    }
    return 0;
  } catch (...) {
    return EIO;
  }
}

/**
 * @brief Computes the exact public occurrence path for one test job.
 * @param root Existing selected output root.
 * @param job Exact occurrence identity.
 * @return Root plus deterministic immutable occurrence leaf.
 * @throws Digest or allocation failures unchanged.
 */
std::filesystem::path public_slot_for_job(const std::filesystem::path& root,
                                          const B1JobInstance& job) {
  return root / ("occurrence-" + commit_id_for_job(job));
}

/**
 * @brief Coordinates a real unrelated executor task during payload settlement.
 * @throws Synchronization allocation failures during construction.
 */
struct ConcurrentCommitContext final {
  /** @brief Creates the reusable release future for the unrelated callback. */
  ConcurrentCommitContext() : release_future(release.get_future().share()) {}

  /** @brief Releases any pending unrelated callback on early test exit. */
  ~ConcurrentCommitContext() noexcept { release_once(); }

  /** @brief Serializes worker-entry and helper-admission facts. */
  std::mutex mutex;
  /** @brief Wakes the helper or worker after the paired fact is published. */
  std::condition_variable condition;
  /** @brief True after the payload worker reaches its pre-mutation seam. */
  bool payload_worker_entered = false;
  /** @brief True after the helper admitted the unrelated queued task. */
  bool unrelated_admitted = false;
  /** @brief Counts accepted payload/manifest observations on the caller. */
  std::size_t accepted_boundaries = 0U;
  /** @brief Counts worker callbacks so only payload waits for helper admission.
   */
  std::size_t worker_boundaries = 0U;
  /** @brief Promise releasing the unrelated provider callback. */
  std::promise<void> release;
  /** @brief Copyable release wait retained by unrelated provider work. */
  std::shared_future<void> release_future;
  /** @brief Exact-once release protection. */
  std::atomic<bool> released{false};

  /** @brief Opens the unrelated callback gate exactly once. */
  void release_once() noexcept {
    if (released.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      release.set_value();
    } catch (...) {
      std::terminate();
    }
  }
};

/**
 * @brief Coordinates payload settlement with one real unrelated executor task.
 * @param opaque Borrowed `ConcurrentCommitContext`.
 * @param point Boundary reached by the store or its I/O worker.
 * @return Nothing after the required deterministic handshake.
 * @throws Synchronization failures unchanged.
 */
void coordinate_concurrent_commit(void* opaque, B1OutputStoreFaultPoint point) {
  auto* context = static_cast<ConcurrentCommitContext*>(opaque);
  if (context == nullptr) {
    return;
  }
  if (point == B1OutputStoreFaultPoint::BeforeTaskWork) {
    std::unique_lock<std::mutex> lock(context->mutex);
    if (context->worker_boundaries++ != 0U) {
      return;
    }
    context->payload_worker_entered = true;
    context->condition.notify_all();
    context->condition.wait(
        lock, [context]() { return context->unrelated_admitted; });
    return;
  }
  if (point == B1OutputStoreFaultPoint::AfterTaskAccepted) {
    std::lock_guard<std::mutex> lock(context->mutex);
    ++context->accepted_boundaries;
    if (context->accepted_boundaries == 2U) {
      context->release_once();
    }
  }
}

/**
 * @brief Releases and settles a blocker after the first capacity rejection.
 * @param opaque Borrowed `CapacityRetryReleaseContext`.
 * @param identity Stable rejected task identity, retained only for validation.
 * @param attempt_number One-based rejected attempt number.
 * @return Nothing.
 * @throws Nothing; failures are retained in the context.
 */
void release_capacity_blocker(void* opaque, const B1IoTaskIdentity& identity,
                              std::size_t attempt_number) noexcept {
  auto* context = static_cast<CapacityRetryReleaseContext*>(opaque);
  if (context == nullptr || attempt_number != 1U || identity.attempt != 0U ||
      context->released.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  try {
    if (context->release == nullptr || context->blocker == nullptr) {
      context->failed.store(true, std::memory_order_release);
      return;
    }
    context->release->set_value();
    if (context->blocker->completion().wait().status() !=
        execution::ComputeIoCompletionStatus::Succeeded) {
      context->failed.store(true, std::memory_order_release);
    }
  } catch (...) {
    context->failed.store(true, std::memory_order_release);
  }
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
 * @brief Compares every stable field in two store-minted receipts.
 * @param lhs First successful commit/reconciliation receipt.
 * @param rhs Second successful commit/reconciliation receipt.
 * @return Nothing after publishing GoogleTest expectations.
 * @throws Test framework failures only.
 */
void expect_same_receipt(const B1OutputCommitReceipt& lhs,
                         const B1OutputCommitReceipt& rhs) {
  EXPECT_EQ(lhs.commit_id(), rhs.commit_id());
  EXPECT_EQ(lhs.resolved_root(), rhs.resolved_root());
  EXPECT_EQ(lhs.rooted_slot(), rhs.rooted_slot());
  EXPECT_EQ(lhs.job(), rhs.job());
  EXPECT_EQ(lhs.logical_descriptor(), rhs.logical_descriptor());
  EXPECT_EQ(lhs.logical_content_digest(), rhs.logical_content_digest());
  EXPECT_EQ(lhs.committed_generation(), rhs.committed_generation());
  EXPECT_EQ(lhs.payload_name(), rhs.payload_name());
  EXPECT_EQ(lhs.manifest_name(), rhs.manifest_name());
  EXPECT_EQ(lhs.payload_length(), rhs.payload_length());
  EXPECT_EQ(lhs.manifest_length(), rhs.manifest_length());
  EXPECT_EQ(lhs.payload_digest(), rhs.payload_digest());
  EXPECT_EQ(lhs.manifest_digest(), rhs.manifest_digest());
  EXPECT_EQ(lhs.requested_durability(), rhs.requested_durability());
  EXPECT_EQ(lhs.achieved_durability(), rhs.achieved_durability());
  EXPECT_EQ(lhs.published_manifest_identity(),
            rhs.published_manifest_identity());
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

  {
    B1OutputStore weaker(
        output_root.root(), executor,
        B1OutputStoreOptions{B1OutputDurability::AtomicVisible, true});
    EXPECT_EQ(weaker.commit(job, ImageBuffer{}).status,
              B1OutputCommitStatus::InvalidRequest);
  }

  {
    B1OutputStore unsupported(
        output_root.root(), executor,
        B1OutputStoreOptions{B1OutputDurability::CrashDurable, false});
    EXPECT_EQ(unsupported.commit(job, ImageBuffer{}).status,
              B1OutputCommitStatus::DurabilityUnsupported);
  }

  B1OutputStore store(output_root.root(), executor);
  EXPECT_EQ(store.commit(job, ImageBuffer{}).status,
            B1OutputCommitStatus::InvalidImage);
  EXPECT_TRUE(std::filesystem::is_empty(output_root.root()));
}

/**
 * @brief Proves the selected root has one cooperating store owner and exposes
 * descriptor-derived identity/filesystem facts.
 * @throws Test framework, filesystem, descriptor, and executor failures.
 */
TEST(B1OutputStore, HoldsExclusiveRootOwnershipAndObservesLiveAuthority) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  B1OutputStore store(output_root.root(), executor);
  const B1OutputStoreRootObservation observed = store.observe_root_authority();
  const B1OutputStoreRootAuthority retained = store.retain_root_authority();
  const B1OutputStoreRootObservation reobserved = retained.observe();
  EXPECT_EQ(observed.resolved_root,
            std::filesystem::canonical(output_root.root()));
  EXPECT_FALSE(observed.root_authority_identity.empty());
  EXPECT_EQ(reobserved.resolved_root, observed.resolved_root);
  EXPECT_EQ(reobserved.root_authority_identity,
            observed.root_authority_identity);
  EXPECT_EQ(reobserved.filesystem_type, observed.filesystem_type);
#if defined(__APPLE__)
  EXPECT_EQ(observed.filesystem_type.rfind("darwin-", 0U), 0U);
#elif defined(__linux__)
  EXPECT_EQ(observed.filesystem_type.rfind("linux-fs-", 0U), 0U);
#endif

  execution::ComputeIoExecutor competing_executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  EXPECT_THROW(
      {
        B1OutputStore competing(output_root.root(), competing_executor);
        static_cast<void>(competing);
      },
      std::system_error);
}

/**
 * @brief Proves retained root capability copies extend lock and fd lifetime.
 * @throws Test framework, filesystem, descriptor, and executor failures.
 */
TEST(B1OutputStore, RetainedRootAuthorityOutlivesStoreAndKeepsOwnership) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  std::optional<B1OutputStoreRootAuthority> retained;
  {
    B1OutputStore store(output_root.root(), executor);
    retained = store.retain_root_authority();
  }
  ASSERT_TRUE(retained.has_value());
  EXPECT_EQ(retained->observe().resolved_root,
            std::filesystem::canonical(output_root.root()));
  EXPECT_THROW(
      {
        B1OutputStore competing(output_root.root(), executor);
        static_cast<void>(competing);
      },
      std::system_error);

  retained.reset();
  B1OutputStore reacquired(output_root.root(), executor);
  EXPECT_EQ(reacquired.observe_root_authority().resolved_root,
            std::filesystem::canonical(output_root.root()));
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
  EXPECT_EQ(receipt.job(), job);
  EXPECT_EQ(receipt.resolved_root(),
            std::filesystem::canonical(output_root.root()));
  EXPECT_EQ(receipt.commit_id().size(), 64U);
  EXPECT_EQ(receipt.logical_content_digest(), golden.logical_digest);
  EXPECT_EQ(receipt.payload_digest(), golden.raw_payload_digest);
  EXPECT_EQ(receipt.payload_length(), kB1PayloadBytes);
  EXPECT_EQ(receipt.manifest_length(), b1_manifest_length(0U));
  EXPECT_EQ(receipt.requested_durability(), B1OutputDurability::CrashDurable);
  EXPECT_EQ(receipt.achieved_durability(), B1OutputDurability::CrashDurable);
  EXPECT_FALSE(receipt.published_manifest_identity().empty());

  const std::filesystem::path slot =
      receipt.resolved_root() / receipt.rooted_slot();
  const std::filesystem::path payload = slot / receipt.payload_name();
  const std::filesystem::path manifest = slot / receipt.manifest_name();
  EXPECT_EQ(std::filesystem::file_size(payload), kB1PayloadBytes);
  EXPECT_EQ(std::filesystem::file_size(manifest), receipt.manifest_length());
  const std::string expected_manifest =
      b1_artifact_manifest(0U, golden.raw_payload_digest);
  EXPECT_EQ(read_text_file(manifest), expected_manifest);
  EXPECT_EQ(receipt.manifest_digest(), b1_sha256(expected_manifest));
  EXPECT_FALSE(std::filesystem::exists(slot / ".manifest.private"));

  ASSERT_EQ(committed.io_observations.size(), 6U);
  EXPECT_EQ(committed.io_observations.front().point,
            B1IoObservationPoint::Initial);
  EXPECT_EQ(committed.io_observations.back().point,
            B1IoObservationPoint::Final);
  EXPECT_EQ(committed.io_observations.back().snapshot.active_tasks, 0U);
  EXPECT_EQ(committed.io_observations.back().snapshot.active_planned_bytes, 0U);

  const B1OutputCommitResult duplicate = store.commit(job, seed_zero_image());
  ASSERT_TRUE(duplicate.succeeded()) << duplicate.diagnostic;
  ASSERT_TRUE(duplicate.receipt.has_value());
  EXPECT_TRUE(duplicate.io_observations.empty());
  expect_same_receipt(receipt, *duplicate.receipt);
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

  CapacityRetryReleaseContext retry_context{&release, &blocker};
  B1OutputStoreOptions options;
  options.capacity_rejection_observer = &release_capacity_blocker;
  options.capacity_rejection_observer_context = &retry_context;
  B1OutputStore store(output_root.root(), executor, options);
  const B1JobInstance job = measured_job_zero();
  const B1OutputCommitResult committed = store.commit(job, seed_zero_image());
  ASSERT_TRUE(committed.succeeded()) << committed.diagnostic;
  EXPECT_FALSE(retry_context.failed.load(std::memory_order_acquire));

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

/**
 * @brief Proves a permanent capacity blocker returns at the frozen attempt
 * bound and cleans the failed occurrence.
 * @throws Test framework, synchronization, and filesystem failures.
 */
TEST(B1OutputStore,
     PermanentCapacityBlockerExhaustsBoundAndPublishesFiniteFinalEvidence) {
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
  const B1OutputCommitResult failed = store.commit(job, seed_zero_image());
  EXPECT_EQ(failed.status, B1OutputCommitStatus::AdmissionFailed);
  EXPECT_FALSE(failed.receipt.has_value());
  ASSERT_EQ(failed.io_observations.size(),
            kB1CapacityAdmissionAttemptLimit + 2U);
  EXPECT_EQ(failed.io_observations.front().point,
            B1IoObservationPoint::Initial);
  EXPECT_EQ(failed.io_observations.back().point, B1IoObservationPoint::Final);
  std::size_t rejection_count = 0U;
  for (const B1ComputeIoObservation& observation : failed.io_observations) {
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
  EXPECT_EQ(rejection_count, kB1CapacityAdmissionAttemptLimit);
  EXPECT_TRUE(std::filesystem::is_empty(output_root.root()));

  release.set_value();
  EXPECT_EQ(blocker.completion().wait().status(),
            execution::ComputeIoCompletionStatus::Succeeded);
}

/**
 * @brief Proves every prepublication exception rolls back and remains
 * retryable.
 * @throws Test framework, oracle, filesystem, executor, and injected failures.
 */
TEST(B1OutputStore,
     PrepublicationExceptionsSettleExecutorRemoveSlotAndPermitExactRetry) {
  const std::array<B1OutputStoreFaultPoint, 5U> points{
      B1OutputStoreFaultPoint::AfterSlotCreated,
      B1OutputStoreFaultPoint::InsideTaskFactory,
      B1OutputStoreFaultPoint::AfterTaskAccepted,
      B1OutputStoreFaultPoint::AfterTaskSettled,
      B1OutputStoreFaultPoint::BeforeSlotPublication};
  for (const B1OutputStoreFaultPoint point : points) {
    SCOPED_TRACE(static_cast<std::uint32_t>(point));
    ScopedB1OutputRoot output_root;
    execution::ComputeIoExecutor executor(
        {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
    ThrowingFaultContext context{point, true};
    B1OutputStoreOptions options;
    options.fault_injector = &throw_selected_output_fault;
    options.fault_injector_context = &context;
    B1OutputStore store(output_root.root(), executor, options);

    if (point == B1OutputStoreFaultPoint::BeforeSlotPublication) {
      const B1OutputCommitResult failed =
          store.commit(measured_job_zero(), seed_zero_image());
      EXPECT_EQ(failed.status, B1OutputCommitStatus::RevalidationFailed);
      EXPECT_FALSE(failed.receipt.has_value());
    } else {
      EXPECT_THROW(static_cast<void>(
                       store.commit(measured_job_zero(), seed_zero_image())),
                   B1OutputFaultSentinel);
    }
    const execution::ComputeIoExecutorSnapshot rolled_back =
        executor.snapshot();
    EXPECT_EQ(rolled_back.active_tasks, 0U);
    EXPECT_EQ(rolled_back.active_planned_bytes, 0U);
    EXPECT_EQ(rolled_back.constructing_tasks, 0U);
    EXPECT_EQ(rolled_back.queued_tasks, 0U);
    EXPECT_EQ(rolled_back.running_tasks, 0U);
    EXPECT_TRUE(std::filesystem::is_empty(output_root.root()));

    if (point == B1OutputStoreFaultPoint::InsideTaskFactory) {
      const auto lifetime = std::make_shared<int>(19);
      const execution::ComputeIoSubmission first_signed = executor.try_submit(
          1U, lifetime,
          []() -> execution::ComputeIoExecutor::Task { return []() {}; });
      ASSERT_TRUE(first_signed.accepted());
      EXPECT_EQ(first_signed.admission_event().sequence, 1U);
      EXPECT_EQ(first_signed.completion().wait().status(),
                execution::ComputeIoCompletionStatus::Succeeded);
    }

    context.enabled = false;
    const B1OutputCommitResult retried =
        store.commit(measured_job_zero(), seed_zero_image());
    EXPECT_TRUE(retried.succeeded()) << retried.diagnostic;
  }
}

/**
 * @brief Proves every post-rename failure preserves and reconciles one output.
 * @throws Test framework, oracle, filesystem, executor, and injected failures.
 */
TEST(B1OutputStore,
     PostPublicationFailuresPreserveOccurrenceAndReturnStableReceiptOnRetry) {
  const std::array<B1OutputStoreFaultPoint, 4U> points{
      B1OutputStoreFaultPoint::AfterSlotPublicationBeforeSourceBarrier,
      B1OutputStoreFaultPoint::AfterSourceBarrierBeforeRootBarrier,
      B1OutputStoreFaultPoint::BeforeFinalPublicRevalidation,
      B1OutputStoreFaultPoint::BeforeReceiptAssembly};
  for (const B1OutputStoreFaultPoint point : points) {
    SCOPED_TRACE(static_cast<std::uint32_t>(point));
    ScopedB1OutputRoot output_root;
    execution::ComputeIoExecutor executor(
        {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
    ThrowingFaultContext context{point, true};
    B1OutputStoreOptions options;
    options.fault_injector = &throw_selected_output_fault;
    options.fault_injector_context = &context;
    B1OutputStore store(output_root.root(), executor, options);
    const B1JobInstance job = measured_job_zero();
    const std::filesystem::path public_slot =
        public_slot_for_job(output_root.root(), job);
    const std::filesystem::path private_anchor =
        private_anchor_for_job(output_root.root(), job);

    const B1OutputCommitResult failed = store.commit(job, seed_zero_image());
    EXPECT_EQ(failed.status, B1OutputCommitStatus::RevalidationFailed)
        << failed.diagnostic;
    EXPECT_FALSE(failed.receipt.has_value());
    EXPECT_TRUE(std::filesystem::is_directory(public_slot));
    EXPECT_TRUE(std::filesystem::exists(public_slot / "output.rgba32le"));
    EXPECT_TRUE(std::filesystem::exists(public_slot / "manifest.txt"));
    EXPECT_TRUE(std::filesystem::is_directory(private_anchor));
    const execution::ComputeIoExecutorSnapshot settled = executor.snapshot();
    EXPECT_EQ(settled.active_tasks, 0U);
    EXPECT_EQ(settled.active_planned_bytes, 0U);
    EXPECT_EQ(settled.constructing_tasks, 0U);
    EXPECT_EQ(settled.queued_tasks, 0U);
    EXPECT_EQ(settled.running_tasks, 0U);

    context.enabled = false;
    const B1OutputCommitResult reconciled =
        store.commit(job, seed_zero_image());
    ASSERT_TRUE(reconciled.succeeded()) << reconciled.diagnostic;
    ASSERT_TRUE(reconciled.receipt.has_value());
    EXPECT_TRUE(reconciled.io_observations.empty());
    EXPECT_FALSE(std::filesystem::exists(private_anchor));

    const B1OutputCommitResult replayed = store.commit(job, seed_zero_image());
    ASSERT_TRUE(replayed.succeeded()) << replayed.diagnostic;
    ASSERT_TRUE(replayed.receipt.has_value());
    EXPECT_TRUE(replayed.io_observations.empty());
    expect_same_receipt(*reconciled.receipt, *replayed.receipt);
  }
}

/**
 * @brief Proves no-transaction-leaf public directories are plainly foreign.
 * @throws Test framework, oracle, filesystem, and executor failures unchanged.
 * @note Both cases exist before `commit` starts and must remain byte-for-byte
 * untouched while the initial `fstatat` reconciliation branch returns
 * `SlotExists` without creating private or transaction-looking leaves.
 */
TEST(B1OutputStore,
     EmptyAndMarkerOnlyExistingPublicSlotsRemainUntouchedSlotExists) {
  for (const bool marker_present : {false, true}) {
    SCOPED_TRACE(marker_present);
    ScopedB1OutputRoot output_root;
    execution::ComputeIoExecutor executor(
        {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
    const B1JobInstance job = measured_job_zero();
    const std::filesystem::path public_slot =
        public_slot_for_job(output_root.root(), job);
    ASSERT_TRUE(std::filesystem::create_directory(public_slot));
    const std::filesystem::path marker = public_slot / "foreign-marker";
    if (marker_present) {
      std::ofstream output(marker, std::ios::binary);
      ASSERT_TRUE(output.good());
      output << "foreign-marker-content";
      output.close();
      ASSERT_TRUE(output.good());
    }
    B1OutputStore store(output_root.root(), executor);

    const B1OutputCommitResult rejected = store.commit(job, seed_zero_image());
    EXPECT_EQ(rejected.status, B1OutputCommitStatus::SlotExists)
        << rejected.diagnostic;
    EXPECT_FALSE(rejected.receipt.has_value());
    EXPECT_TRUE(rejected.io_observations.empty());
    EXPECT_TRUE(std::filesystem::is_directory(public_slot));
    EXPECT_FALSE(std::filesystem::exists(public_slot / "output.rgba32le"));
    EXPECT_FALSE(std::filesystem::exists(public_slot / "manifest.txt"));
    EXPECT_FALSE(std::filesystem::exists(public_slot / ".manifest.private"));
    EXPECT_FALSE(std::filesystem::exists(
        private_anchor_for_job(output_root.root(), job)));
    if (marker_present) {
      EXPECT_EQ(read_text_file(marker), "foreign-marker-content");
    } else {
      EXPECT_TRUE(std::filesystem::is_empty(public_slot));
    }
    const execution::ComputeIoExecutorSnapshot untouched = executor.snapshot();
    EXPECT_EQ(untouched.active_tasks, 0U);
    EXPECT_EQ(untouched.active_planned_bytes, 0U);
    EXPECT_EQ(untouched.constructing_tasks, 0U);
    EXPECT_EQ(untouched.queued_tasks, 0U);
    EXPECT_EQ(untouched.running_tasks, 0U);
  }
}

/**
 * @brief Proves incomplete or drifted pending public state fails closed.
 * @throws Test framework, oracle, filesystem, executor, and injected failures.
 */
TEST(B1OutputStore, PendingPublicIncompleteOrDriftedStateIsNeverRewritten) {
  for (const bool remove_manifest : {false, true}) {
    SCOPED_TRACE(remove_manifest);
    ScopedB1OutputRoot output_root;
    execution::ComputeIoExecutor executor(
        {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
    ThrowingFaultContext context{
        B1OutputStoreFaultPoint::AfterSlotPublicationBeforeSourceBarrier, true};
    B1OutputStoreOptions options;
    options.fault_injector = &throw_selected_output_fault;
    options.fault_injector_context = &context;
    B1OutputStore store(output_root.root(), executor, options);
    const B1JobInstance job = measured_job_zero();
    const std::filesystem::path public_slot =
        public_slot_for_job(output_root.root(), job);
    const std::filesystem::path manifest = public_slot / "manifest.txt";

    const B1OutputCommitResult failed = store.commit(job, seed_zero_image());
    ASSERT_EQ(failed.status, B1OutputCommitStatus::RevalidationFailed);
    ASSERT_TRUE(std::filesystem::exists(manifest));
    context.enabled = false;
    if (remove_manifest) {
      ASSERT_TRUE(std::filesystem::remove(manifest));
    } else {
      std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
      ASSERT_TRUE(output.good());
      output << "drifted-manifest";
      output.close();
      ASSERT_TRUE(output.good());
    }

    const B1OutputCommitResult rejected = store.commit(job, seed_zero_image());
    EXPECT_EQ(rejected.status, B1OutputCommitStatus::RevalidationFailed)
        << rejected.diagnostic;
    EXPECT_FALSE(rejected.receipt.has_value());
    EXPECT_TRUE(std::filesystem::is_directory(public_slot));
    EXPECT_EQ(std::filesystem::exists(manifest), !remove_manifest);
    EXPECT_TRUE(std::filesystem::exists(public_slot / "output.rgba32le"));
  }
}

/**
 * @brief Proves root replacement cannot redirect any artifact mutation.
 * @throws Test framework, oracle, filesystem, and executor failures unchanged.
 */
TEST(B1OutputStore,
     HeldRootDescriptorFailsClosedAcrossReplacementWithoutRedirectedWrites) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  RootReplacementContext context(output_root.root());
  B1OutputStoreOptions options;
  options.fault_injector = &replace_root_after_verification;
  options.fault_injector_context = &context;
  B1OutputStore store(output_root.root(), executor, options);
  const B1OutputStoreRootAuthority retained = store.retain_root_authority();

  const B1OutputCommitResult failed =
      store.commit(measured_job_zero(), seed_zero_image());
  EXPECT_EQ(failed.status, B1OutputCommitStatus::RootUnavailable)
      << failed.diagnostic;
  EXPECT_FALSE(failed.receipt.has_value());
  EXPECT_TRUE(std::filesystem::is_empty(context.replacement()));
  EXPECT_TRUE(std::filesystem::is_empty(context.displaced()));
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
  EXPECT_THROW(static_cast<void>(retained.observe()), std::runtime_error);

  context.restore();
  EXPECT_NO_THROW(static_cast<void>(retained.observe()));
  const B1OutputCommitResult retried =
      store.commit(measured_job_zero(), seed_zero_image());
  EXPECT_TRUE(retried.succeeded()) << retried.diagnostic;
}

/**
 * @brief Proves a pre-guard anchor takeover propagates and preserves ambiguous
 * replacement/displaced residue without claiming same-identity retryability.
 * @throws Test framework, oracle, executor, and filesystem failures unchanged.
 */
TEST(B1OutputStore,
     PreGuardAnchorHandoffFailurePreservesResidueAndDoesNotReturnSuccess) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  const B1JobInstance job = measured_job_zero();
  OutputNamespaceMutationContext context;
  context.root = output_root.root();
  context.private_anchor = private_anchor_for_job(output_root.root(), job);
  context.private_slot = private_slot_for_job(output_root.root(), job);
  context.replacement_namespace = context.private_anchor;
  context.displaced_namespace =
      output_root.root() / (".b1-displaced-" + commit_id_for_job(job));
  context.mutation = OutputNamespaceMutation::ReplacePrivateAnchor;
  context.point = B1OutputStoreFaultPoint::AfterStagingAnchorMkdirBeforeOpen;
  B1OutputStoreOptions options;
  options.fault_injector = &mutate_output_namespace;
  options.fault_injector_context = &context;
  B1OutputStore store(output_root.root(), executor, options);

  EXPECT_THROW(static_cast<void>(store.commit(job, seed_zero_image())),
               std::runtime_error);
  EXPECT_TRUE(std::filesystem::exists(context.replacement_namespace /
                                      "replacement-marker"));
  EXPECT_TRUE(std::filesystem::is_directory(context.displaced_namespace));
  EXPECT_FALSE(std::filesystem::exists(context.replacement_namespace /
                                       "output.rgba32le"));
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

#if GTEST_HAS_DEATH_TEST
/**
 * @brief Proves mkdir-to-open replacement is detected before any artifact
 * write and its real-directory replacement is never deleted by name.
 * @throws Test framework, oracle, executor, and filesystem failures unchanged.
 */
TEST(B1OutputStore,
     PrivateSlotHandoffReplacementFailsStopAndPreservesReplacementDirectory) {
  ScopedB1OutputRoot output_root;
  static_cast<void>(seed_zero_image());
  const B1JobInstance job = measured_job_zero();
  OutputNamespaceMutationContext context;
  context.root = output_root.root();
  context.private_anchor = private_anchor_for_job(output_root.root(), job);
  context.private_slot = private_slot_for_job(output_root.root(), job);
  context.replacement_namespace = context.private_slot;
  context.displaced_namespace = context.private_anchor / "slot-displaced";
  context.mutation = OutputNamespaceMutation::ReplacePrivateSlot;
  context.point = B1OutputStoreFaultPoint::AfterStagingSlotMkdirBeforeOpen;

  EXPECT_DEATH(
      {
        execution::ComputeIoExecutor executor(
            {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
        B1OutputStoreOptions options;
        options.fault_injector = &mutate_output_namespace;
        options.fault_injector_context = &context;
        B1OutputStore store(output_root.root(), executor, options);
        static_cast<void>(store.commit(job, seed_zero_image()));
      },
      ".*");
  EXPECT_TRUE(std::filesystem::is_directory(context.replacement_namespace));
  EXPECT_TRUE(std::filesystem::exists(context.replacement_namespace /
                                      "replacement-marker"));
  EXPECT_TRUE(std::filesystem::is_directory(context.displaced_namespace));
  EXPECT_FALSE(std::filesystem::exists(context.replacement_namespace /
                                       "output.rgba32le"));
}
#endif

/**
 * @brief Proves a real destination installed just before publication wins,
 * remains untouched, and leaves a fully retryable private cleanup.
 * @throws Test framework, oracle, executor, and filesystem failures unchanged.
 */
TEST(B1OutputStore,
     AtomicNoReplacePublicationPreservesRealDirectoryCollisionAndRetries) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  const B1JobInstance job = measured_job_zero();
  OutputNamespaceMutationContext context;
  context.root = output_root.root();
  context.public_slot = public_slot_for_job(output_root.root(), job);
  context.mutation = OutputNamespaceMutation::InstallPublicSlot;
  context.point = B1OutputStoreFaultPoint::BeforeSlotPublication;
  B1OutputStoreOptions options;
  options.fault_injector = &mutate_output_namespace;
  options.fault_injector_context = &context;
  B1OutputStore store(output_root.root(), executor, options);

  const B1OutputCommitResult collided = store.commit(job, seed_zero_image());
  EXPECT_EQ(collided.status, B1OutputCommitStatus::SlotExists)
      << collided.diagnostic;
  EXPECT_TRUE(std::filesystem::is_directory(context.public_slot));
  EXPECT_TRUE(
      std::filesystem::exists(context.public_slot / "replacement-marker"));
  EXPECT_FALSE(
      std::filesystem::exists(context.public_slot / "output.rgba32le"));
  EXPECT_FALSE(
      std::filesystem::exists(private_anchor_for_job(output_root.root(), job)));
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
  ASSERT_GT(std::filesystem::remove_all(context.public_slot), 0U);

  const B1OutputCommitResult retried = store.commit(job, seed_zero_image());
  EXPECT_TRUE(retried.succeeded()) << retried.diagnostic;
}

#if GTEST_HAS_DEATH_TEST
/**
 * @brief Proves unowned extra leaves and type replacements cannot be silently
 * swallowed during rollback after the accepted charge has settled.
 * @throws Test framework, oracle, executor, and filesystem failures unchanged.
 */
TEST(B1OutputStore, StrictCleanupFailsStopOnExtraLeafAndTypeReplacement) {
  static_cast<void>(seed_zero_image());
  for (const OutputNamespaceMutation mutation :
       {OutputNamespaceMutation::AddExtraLeaf,
        OutputNamespaceMutation::ReplacePayloadType}) {
    SCOPED_TRACE(static_cast<std::uint32_t>(mutation));
    ScopedB1OutputRoot output_root;
    OutputNamespaceMutationContext context;
    context.root = output_root.root();
    context.private_anchor =
        private_anchor_for_job(output_root.root(), measured_job_zero());
    context.private_slot =
        private_slot_for_job(output_root.root(), measured_job_zero());
    context.mutation = mutation;
    context.point = B1OutputStoreFaultPoint::AfterTaskSettled;
    context.throw_after = true;
    EXPECT_DEATH(
        {
          execution::ComputeIoExecutor executor(
              {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
          B1OutputStoreOptions options;
          options.fault_injector = &mutate_output_namespace;
          options.fault_injector_context = &context;
          B1OutputStore store(output_root.root(), executor, options);
          static_cast<void>(
              store.commit(measured_job_zero(), seed_zero_image()));
        },
        ".*");
  }
}

/**
 * @brief Proves injected unlink `EIO`/`EROFS` are fail-stop only after the
 * accepted executor charge has settled.
 * @throws Test framework, oracle, executor, and filesystem failures unchanged.
 */
TEST(B1OutputStore, StrictCleanupFailsStopOnInjectedUnlinkErrors) {
  static_cast<void>(seed_zero_image());
  for (const int injected_errno : {EIO, EROFS}) {
    SCOPED_TRACE(injected_errno);
    ScopedB1OutputRoot output_root;
    CleanupMutationContext cleanup;
    cleanup.root = output_root.root();
    cleanup.private_slot =
        private_slot_for_job(output_root.root(), measured_job_zero());
    cleanup.mutation = CleanupMutation::InjectErrno;
    cleanup.injected_errno = injected_errno;
    ThrowingFaultContext failure{B1OutputStoreFaultPoint::AfterTaskSettled,
                                 true};
    EXPECT_DEATH(
        {
          execution::ComputeIoExecutor executor(
              {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
          cleanup.executor = &executor;
          B1OutputStoreOptions options;
          options.fault_injector = &throw_selected_output_fault;
          options.fault_injector_context = &failure;
          options.cleanup_injector = &mutate_strict_cleanup;
          options.cleanup_injector_context = &cleanup;
          B1OutputStore store(output_root.root(), executor, options);
          static_cast<void>(
              store.commit(measured_job_zero(), seed_zero_image()));
        },
        ".*");
    EXPECT_TRUE(std::filesystem::exists(output_root.root() /
                                        ".cleanup-charge-settled"));
  }
}

/**
 * @brief Proves the final-recheck/pre-unlink seam can mutate then fail-stop.
 * @throws Test framework, oracle, executor, and filesystem failures unchanged.
 * @note This deliberately models an out-of-contract same-UID mutation but
 * returns `EIO` before the store's name removal. It proves only that the final
 * seam is deterministic and failure precedes `unlinkat`; it does not claim an
 * atomic identity-selected POSIX removal.
 */
TEST(B1OutputStore, FinalCleanupBoundaryFailurePrecedesNameRemoval) {
  ScopedB1OutputRoot output_root;
  static_cast<void>(seed_zero_image());
  CleanupMutationContext cleanup;
  cleanup.root = output_root.root();
  cleanup.private_slot =
      private_slot_for_job(output_root.root(), measured_job_zero());
  cleanup.mutation = CleanupMutation::InstallDifferentIdentityThenFail;
  cleanup.injected_errno = EIO;
  ThrowingFaultContext failure{B1OutputStoreFaultPoint::AfterTaskSettled, true};
  EXPECT_DEATH(
      {
        execution::ComputeIoExecutor executor(
            {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
        cleanup.executor = &executor;
        B1OutputStoreOptions options;
        options.fault_injector = &throw_selected_output_fault;
        options.fault_injector_context = &failure;
        options.final_cleanup_injector = &mutate_strict_cleanup;
        options.final_cleanup_injector_context = &cleanup;
        B1OutputStore store(output_root.root(), executor, options);
        static_cast<void>(store.commit(measured_job_zero(), seed_zero_image()));
      },
      ".*");
  EXPECT_TRUE(
      std::filesystem::exists(output_root.root() / ".cleanup-charge-settled"));
  const std::filesystem::path payload =
      cleanup.private_slot / "output.rgba32le";
  EXPECT_TRUE(std::filesystem::is_regular_file(payload));
  std::ifstream replacement(payload, std::ios::binary);
  ASSERT_TRUE(replacement.good());
  std::ostringstream replacement_bytes;
  replacement_bytes << replacement.rdbuf();
  EXPECT_EQ(replacement_bytes.str(), "different-identity");
  EXPECT_TRUE(std::filesystem::is_regular_file(cleanup.private_slot /
                                               "payload-displaced"));
}
#endif

/**
 * @brief Proves a same-inode rename-away/back remains removable and retryable.
 * @throws Test framework, oracle, executor, and filesystem failures unchanged.
 */
TEST(B1OutputStore, StrictCleanupAcceptsSameIdentityRaceAfterSettlement) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  CleanupMutationContext cleanup;
  cleanup.root = output_root.root();
  cleanup.private_slot =
      private_slot_for_job(output_root.root(), measured_job_zero());
  cleanup.executor = &executor;
  cleanup.mutation = CleanupMutation::RestoreSameIdentity;
  ThrowingFaultContext failure{B1OutputStoreFaultPoint::AfterTaskSettled, true};
  B1OutputStoreOptions options;
  options.fault_injector = &throw_selected_output_fault;
  options.fault_injector_context = &failure;
  options.cleanup_injector = &mutate_strict_cleanup;
  options.cleanup_injector_context = &cleanup;
  B1OutputStore store(output_root.root(), executor, options);

  EXPECT_THROW(
      static_cast<void>(store.commit(measured_job_zero(), seed_zero_image())),
      B1OutputFaultSentinel);
  EXPECT_TRUE(cleanup.invoked);
  EXPECT_TRUE(cleanup.charge_was_settled);
  EXPECT_TRUE(std::filesystem::is_empty(output_root.root()));
  failure.enabled = false;
  const B1OutputCommitResult retried =
      store.commit(measured_job_zero(), seed_zero_image());
  EXPECT_TRUE(retried.succeeded()) << retried.diagnostic;
}

#if GTEST_HAS_DEATH_TEST
/**
 * @brief Proves a different-inode replacement after the first identity check
 * is preserved and forces fail-stop instead of redirected deletion.
 * @throws Test framework, oracle, executor, and filesystem failures unchanged.
 */
TEST(B1OutputStore, StrictCleanupFailsStopOnDifferentIdentityRace) {
  ScopedB1OutputRoot output_root;
  static_cast<void>(seed_zero_image());
  CleanupMutationContext cleanup;
  cleanup.root = output_root.root();
  cleanup.private_slot =
      private_slot_for_job(output_root.root(), measured_job_zero());
  cleanup.mutation = CleanupMutation::InstallDifferentIdentity;
  ThrowingFaultContext failure{B1OutputStoreFaultPoint::AfterTaskSettled, true};
  EXPECT_DEATH(
      {
        execution::ComputeIoExecutor executor(
            {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
        cleanup.executor = &executor;
        B1OutputStoreOptions options;
        options.fault_injector = &throw_selected_output_fault;
        options.fault_injector_context = &failure;
        options.cleanup_injector = &mutate_strict_cleanup;
        options.cleanup_injector_context = &cleanup;
        B1OutputStore store(output_root.root(), executor, options);
        static_cast<void>(store.commit(measured_job_zero(), seed_zero_image()));
      },
      ".*");
  EXPECT_TRUE(
      std::filesystem::exists(output_root.root() / ".cleanup-charge-settled"));
  EXPECT_TRUE(std::filesystem::is_regular_file(cleanup.private_slot /
                                               "output.rgba32le"));
  EXPECT_TRUE(
      std::filesystem::exists(cleanup.private_slot / "payload-displaced"));
}
#endif

/**
 * @brief Proves a real concurrent charge survives one B1 task's settlement cut.
 * @throws Test framework, oracle, synchronization, and executor failures.
 */
TEST(B1OutputStore, RealConcurrentTaskDoesNotCorruptExactCommitChargeEvidence) {
  ScopedB1OutputRoot output_root;
  execution::ComputeIoExecutor executor(
      {kB1ComputeIoTaskLimit, kB1ComputeIoPlannedByteLimit});
  ConcurrentCommitContext context;
  B1OutputStoreOptions options;
  options.fault_injector = &coordinate_concurrent_commit;
  options.fault_injector_context = &context;
  B1OutputStore store(output_root.root(), executor, options);
  const std::shared_ptr<const void> unrelated_lifetime =
      std::make_shared<int>(99);

  std::future<execution::ComputeIoSubmission> unrelated =
      std::async(std::launch::async, [&]() {
        {
          std::unique_lock<std::mutex> lock(context.mutex);
          const bool entered = context.condition.wait_for(
              lock, 2s,
              [&context]() { return context.payload_worker_entered; });
          if (!entered) {
            return execution::ComputeIoSubmission{};
          }
        }
        execution::ComputeIoSubmission submission = executor.try_submit(
            1U, unrelated_lifetime,
            [&context]() -> execution::ComputeIoExecutor::Task {
              return [&context]() { context.release_future.wait(); };
            });
        {
          std::lock_guard<std::mutex> lock(context.mutex);
          context.unrelated_admitted = true;
        }
        context.condition.notify_all();
        return submission;
      });

  const B1OutputCommitResult committed =
      store.commit(measured_job_zero(), seed_zero_image());
  ASSERT_TRUE(committed.succeeded()) << committed.diagnostic;
  execution::ComputeIoSubmission unrelated_submission = unrelated.get();
  ASSERT_TRUE(unrelated_submission.accepted());
  EXPECT_EQ(unrelated_submission.completion().wait().status(),
            execution::ComputeIoCompletionStatus::Succeeded);

  const auto payload_settlement = std::find_if(
      committed.io_observations.begin(), committed.io_observations.end(),
      [](const B1ComputeIoObservation& observation) {
        return observation.point == B1IoObservationPoint::Settlement &&
               observation.task.has_value() &&
               observation.task->stage == B1IoStage::PayloadStage;
      });
  ASSERT_NE(payload_settlement, committed.io_observations.end());
  ASSERT_TRUE(payload_settlement->settlement_event.has_value());
  EXPECT_EQ(payload_settlement->settlement_event->released_tasks, 1U);
  EXPECT_EQ(payload_settlement->settlement_event->released_planned_bytes,
            kB1PayloadBytes);
  EXPECT_GE(payload_settlement->snapshot.active_tasks, 1U);
  EXPECT_GE(payload_settlement->snapshot.active_planned_bytes, 1U);
  EXPECT_EQ(payload_settlement->snapshot.active_tasks,
            payload_settlement->settlement_event->snapshot_after.active_tasks);
  EXPECT_EQ(payload_settlement->snapshot.active_planned_bytes,
            payload_settlement->settlement_event->snapshot_after
                .active_planned_bytes);
}

}  // namespace
}  // namespace ps::benchmark
