/**
 * @file b1_output_store.hpp
 * @brief Declares the source-private crash-durable B1 artifact owner.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/b1_profile.hpp"           // NOLINT(build/include_subdir)
#include "execution/compute_io_executor.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/** @brief Exact deterministic number of capacity admission attempts per task.
 */
inline constexpr std::size_t kB1CapacityAdmissionAttemptLimit = 64U;

/**
 * @brief Closed durability vocabulary requested and achieved by B1 output.
 * @throws Nothing for value construction and comparison.
 */
enum class B1OutputDurability : std::uint8_t {
  /** @brief Atomic visibility without crash-survival proof. */
  AtomicVisible,
  /** @brief File data and every namespace level completed durability barriers.
   */
  CrashDurable,
};

/**
 * @brief Closed terminal result of one B1 commit request.
 * @throws Nothing for value construction and comparison.
 */
enum class B1OutputCommitStatus : std::uint8_t {
  /** @brief Receipt was produced after complete crash-durable revalidation. */
  Succeeded,
  /** @brief Job identity or requested durability was invalid. */
  InvalidRequest,
  /** @brief Candidate image did not match the frozen B1 descriptor. */
  InvalidImage,
  /** @brief Selected root was unavailable, moved, or not a directory. */
  RootUnavailable,
  /** @brief The immutable occurrence slot already existed. */
  SlotExists,
  /** @brief Compute I/O admission was rejected or exhausted its attempt bound.
   */
  AdmissionFailed,
  /** @brief An accepted payload or manifest task failed or was cancelled. */
  TaskFailed,
  /** @brief Required no-replace or durability primitive was unsupported. */
  DurabilityUnsupported,
  /** @brief Reopened payload, manifest, or rooted identity did not match. */
  RevalidationFailed,
};

/**
 * @brief Event boundary at which one Compute I/O snapshot was retained.
 * @throws Nothing for value construction and comparison.
 */
enum class B1IoObservationPoint : std::uint8_t {
  /** @brief Snapshot before either task is offered. */
  Initial,
  /** @brief Snapshot immediately after one typed capacity/control rejection. */
  OfferRejected,
  /** @brief Snapshot immediately after one accepted admission. */
  AcceptedAdmission,
  /** @brief Snapshot immediately after one accepted task settles. */
  Settlement,
  /** @brief Snapshot after the complete commit reaches quiescence. */
  Final,
};

/**
 * @brief Complete event-aligned Compute I/O admission/accounting observation.
 * @throws Nothing for ordinary value operations except diagnostic allocation.
 */
struct B1ComputeIoObservation final {
  /** @brief Boundary represented by this snapshot. */
  B1IoObservationPoint point = B1IoObservationPoint::Initial;
  /** @brief Task identity when the boundary is task-specific. */
  std::optional<B1IoTaskIdentity> task;
  /** @brief Exact immutable task charge, or zero for row-level boundaries. */
  std::uint64_t planned_bytes = 0U;
  /** @brief Typed admission result when an offer occurred. */
  std::optional<execution::ComputeIoAdmissionStatus> admission;
  /** @brief Typed completion result when an accepted task settled. */
  std::optional<execution::ComputeIoCompletionStatus> completion;
  /** @brief Same-lock executor admission event for offer boundaries. */
  std::optional<execution::ComputeIoAdmissionEvent> admission_event;
  /** @brief Exact executor settlement event for accepted task completion. */
  std::optional<execution::ComputeIoSettlementEvent> settlement_event;
  /** @brief Authority-free process executor state at this exact boundary. */
  execution::ComputeIoExecutorSnapshot snapshot;
};

/**
 * @brief Immutable successful crash-durable B1 output receipt.
 * @throws Nothing for ordinary value operations except owned string/path copy.
 * @note Construction is internal to `B1OutputStore` after all barriers.
 */
struct B1OutputCommitReceipt final {
  /** @brief Stable lowercase SHA-256 commit identity. */
  std::string commit_id;
  /** @brief Canonical selected output root. */
  std::filesystem::path resolved_root;
  /** @brief Root-relative immutable occurrence slot. */
  std::filesystem::path rooted_slot;
  /** @brief Complete occurrence identity bound before first offer. */
  B1JobInstance job;
  /** @brief Fixed logical descriptor identity. */
  std::string logical_descriptor;
  /** @brief Typed logical candidate content identity. */
  ContentDigest logical_content_digest;
  /** @brief Fixed committed generation for the immutable occurrence. */
  std::uint64_t committed_generation = 1U;
  /** @brief Exact payload leaf name. */
  std::string payload_name;
  /** @brief Exact manifest leaf name published last. */
  std::string manifest_name;
  /** @brief Exact committed payload length. */
  std::uint64_t payload_length = 0U;
  /** @brief Exact committed manifest length. */
  std::uint64_t manifest_length = 0U;
  /** @brief SHA-256 of exact little-endian payload bytes. */
  B1Sha256Digest payload_digest;
  /** @brief SHA-256 of exact canonical manifest bytes. */
  B1Sha256Digest manifest_digest;
  /** @brief Requested durability contract. */
  B1OutputDurability requested_durability = B1OutputDurability::CrashDurable;
  /** @brief Achieved durability after all barriers. */
  B1OutputDurability achieved_durability = B1OutputDurability::CrashDurable;
  /** @brief Stable published manifest filesystem identity. */
  std::string published_manifest_identity;
};

/**
 * @brief Complete typed outcome and evidence from one B1 commit request.
 * @throws Nothing for ordinary movement except owned diagnostic/evidence copy.
 */
struct B1OutputCommitResult final {
  /** @brief Exact terminal status. */
  B1OutputCommitStatus status = B1OutputCommitStatus::InvalidRequest;
  /** @brief Human-readable non-authoritative failure detail. */
  std::string diagnostic;
  /** @brief Present exactly for `Succeeded`. */
  std::optional<B1OutputCommitReceipt> receipt;
  /** @brief Complete initial/admission/settlement/final snapshots. */
  std::vector<B1ComputeIoObservation> io_observations;

  /**
   * @brief Reports whether this result carries a valid receipt.
   * @return True exactly for successful receipt-bearing completion.
   * @throws Nothing.
   */
  bool succeeded() const noexcept {
    return status == B1OutputCommitStatus::Succeeded && receipt.has_value();
  }
};

/**
 * @brief Deterministic source-private exception/root-race injection boundary.
 * @throws Nothing for value construction and comparison.
 * @note Production leaves the associated hook null. The hook may mutate only
 * test-owned external state or throw; it receives no descriptor or authority.
 */
enum class B1OutputStoreFaultPoint : std::uint8_t {
  /** @brief Root binding passed immediately before fd-relative slot creation.
   */
  AfterRootBindingVerified,
  /** @brief Slot and retained slot descriptor exist, before task-state setup.
   */
  AfterSlotCreated,
  /** @brief Accepted budget is charged and the lazy task factory is entered. */
  InsideTaskFactory,
  /** @brief Submission returned Accepted and its completion was guarded. */
  AfterTaskAccepted,
  /** @brief I/O worker entered the accepted callback before artifact mutation.
   */
  BeforeTaskWork,
  /** @brief Accepted task settled and the transaction no longer owns a wait. */
  AfterTaskSettled,
  /** @brief Both I/O tasks succeeded immediately before receipt construction.
   */
  BeforeReceiptAssembly,
};

/**
 * @brief Construction policy for one selected B1 output root.
 * @throws Nothing for value construction except path allocation.
 */
struct B1OutputStoreOptions final {
  /** @brief Requested durability; B1 runners require crash durable. */
  B1OutputDurability requested_durability = B1OutputDurability::CrashDurable;
  /** @brief Test/platform capability switch that may only weaken/fail closed.
   */
  bool crash_durability_supported = true;

  /**
   * @brief Optional source-private observation hook after capacity rejection.
   * @param context Borrowed context valid for the complete commit call.
   * @param identity Stable attempt-zero task identity being retried.
   * @param attempt_number One-based attempt number just rejected.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   * @note The hook exists for deterministic mechanism tests. It cannot change
   * identity, charge, retry count, admission status, cleanup, or receipt
   * policy and is absent from installed/public contracts.
   */
  using CapacityRejectionObserver =
      void (*)(void* context, const B1IoTaskIdentity& identity,
               std::size_t attempt_number) noexcept;

  /** @brief Optional capacity-rejection observer; production leaves null. */
  CapacityRejectionObserver capacity_rejection_observer = nullptr;

  /** @brief Borrowed context paired with `capacity_rejection_observer`. */
  void* capacity_rejection_observer_context = nullptr;

  /**
   * @brief Optional deterministic fault/root-race test callback.
   * @param context Borrowed caller context valid for `commit()`.
   * @param point Exact internal boundary currently reached.
   * @return Nothing when execution should continue.
   * @throws Any test-selected exception; the commit transaction first cancels
   * and settles accepted work, removes the owned slot, then propagates it.
   * @note The callback is source-private and cannot receive root/slot fds.
   */
  using FaultInjector = void (*)(void* context, B1OutputStoreFaultPoint point);

  /** @brief Optional deterministic fault injector; production leaves null. */
  FaultInjector fault_injector = nullptr;

  /** @brief Borrowed context paired with `fault_injector`. */
  void* fault_injector_context = nullptr;
};

/**
 * @brief Owns B1 path, no-replace, durability, revalidation, and receipt
 * policy.
 *
 * The store composes exactly two ordered jobs on the supplied process
 * `ComputeIoExecutor`; it creates no worker, Graph authority, scheduler,
 * resource ledger, cache, or installed Host surface.
 *
 * @throws std::invalid_argument for a missing/non-directory root.
 * @throws std::filesystem::filesystem_error when canonical root selection
 * fails.
 * @note The executor and selected root must outlive this store and every call.
 */
class B1OutputStore final {
 public:
  /**
   * @brief Selects and fingerprints one canonical root before warmup.
   * @param root Existing directory dedicated to benchmark output.
   * @param executor Existing process-owned Compute I/O executor.
   * @param options Closed requested/platform durability policy.
   * @throws Constructor contract failures described by the class.
   */
  B1OutputStore(std::filesystem::path root,
                execution::ComputeIoExecutor& executor,
                B1OutputStoreOptions options = {});

  /**
   * @brief Releases the held root descriptor after every synchronous commit.
   * @throws Nothing; descriptor-close failure terminates fail-stop.
   */
  ~B1OutputStore() noexcept;

  /** @brief Store ownership is unique and cannot be copied. */
  B1OutputStore(const B1OutputStore&) = delete;

  /** @brief Store authority cannot be reassigned. */
  B1OutputStore& operator=(const B1OutputStore&) = delete;

  /**
   * @brief Returns the immutable selected canonical root.
   * @return Borrowed path valid for the store lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& resolved_root() const noexcept { return root_; }

  /**
   * @brief Commits one exact candidate image using two ordered I/O tasks.
   * @param job Complete immutable occurrence allocated before offer.
   * @param image Exact candidate CPU FP32 RGBA image.
   * @return Typed result, complete evidence, and receipt only after barriers.
   * @throws std::bad_alloc or synchronization exceptions that prevent even a
   * typed result from being assembled.
   * @note Capacity rejection retries the same attempt-zero identity/charge for
   * exactly `kB1CapacityAdmissionAttemptLimit` total attempts. Exhaustion or a
   * non-capacity rejection returns `AdmissionFailed`, cleans the occurrence
   * slot, and records the final observation without a timing-derived policy.
   */
  B1OutputCommitResult commit(const B1JobInstance& job,
                              const ImageBuffer& image);

 private:
  /** @brief Canonical root selected once before any commit. */
  std::filesystem::path root_;
  /** @brief Existing process executor; owns the sole I/O worker. */
  execution::ComputeIoExecutor& executor_;
  /** @brief Closed durability policy selected at construction. */
  B1OutputStoreOptions options_;
  /** @brief Selected POSIX root device identity, zero on unsupported hosts. */
  std::uint64_t root_device_ = 0U;
  /** @brief Selected POSIX root inode identity, zero on unsupported hosts. */
  std::uint64_t root_inode_ = 0U;
  /** @brief Held POSIX root directory descriptor, negative when unsupported. */
  int root_descriptor_ = -1;
};

}  // namespace ps::benchmark
