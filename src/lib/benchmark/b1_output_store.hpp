/**
 * @file b1_output_store.hpp
 * @brief Declares the source-private crash-durable B1 artifact owner.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/b1_profile.hpp"           // NOLINT(build/include_subdir)
#include "execution/compute_io_executor.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

namespace testing {
struct B1OutputCommitReceiptTestAccess;
}  // namespace testing

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
 * @brief Live root facts observed through the held `B1OutputStore` descriptor.
 * @throws Nothing for ordinary movement except owned path/string allocation.
 * @note This is a diagnostic result, not the descriptor capability itself;
 * copying its strings into memory or a file does not preserve authority.
 */
struct B1OutputStoreRootObservation final {
  /** @brief Canonical selected root still bound to the held descriptor. */
  std::filesystem::path resolved_root;
  /** @brief Stable descriptor-derived device/inode authority identity. */
  std::string root_authority_identity;
  /** @brief Normalized filesystem type returned by descriptor `fstatfs`. */
  std::string filesystem_type;
};

/**
 * @brief Opaque retained capability for one live B1 output-root descriptor.
 *
 * Copies share the same duplicated descriptor and advisory-lock lifetime. The
 * descriptor is re-observed on every `observe()` call; copied diagnostic path,
 * identity, or filesystem strings cannot construct this capability.
 *
 * @throws Nothing for copy, move, and destruction; `observe()` reports live
 * descriptor or namespace failures separately.
 * @note The last capability copy closes the duplicated descriptor fail-stop
 * and may therefore extend exclusive-root ownership beyond `B1OutputStore`
 * until copied `B1InnerRow` evidence is released.
 * @note `observe()` is read-only and may run through distinct live copies in
 * parallel; callers must not race destruction of the same C++ object instance.
 */
class B1OutputStoreRootAuthority final {
 public:
  /** @brief Shares one existing live descriptor capability. */
  B1OutputStoreRootAuthority(const B1OutputStoreRootAuthority&) noexcept;

  /** @brief Transfers one existing live descriptor capability. */
  B1OutputStoreRootAuthority(B1OutputStoreRootAuthority&&) noexcept;

  /** @brief Shares one existing live descriptor capability. */
  B1OutputStoreRootAuthority& operator=(
      const B1OutputStoreRootAuthority&) noexcept;

  /** @brief Transfers one existing live descriptor capability. */
  B1OutputStoreRootAuthority& operator=(B1OutputStoreRootAuthority&&) noexcept;

  /**
   * @brief Releases one shared owner of the duplicated descriptor.
   * @throws Nothing; final descriptor-close failure terminates fail-stop.
   */
  ~B1OutputStoreRootAuthority() noexcept;

  /**
   * @brief Re-observes the held descriptor and its selected pathname binding.
   * @return Fresh descriptor-derived root identity and filesystem facts.
   * @throws std::system_error for descriptor/filesystem observation failure.
   * @throws std::runtime_error for path, type, or identity drift.
   * @note No serialized value or retained proof participates in this check.
   */
  B1OutputStoreRootObservation observe() const;

 private:
  /** @brief Source-private shared descriptor and frozen binding state. */
  struct State;

  /**
   * @brief Adopts one store-minted shared descriptor state.
   * @param state Non-null state created by `B1OutputStore`.
   * @throws std::invalid_argument when `state` is null.
   */
  explicit B1OutputStoreRootAuthority(std::shared_ptr<const State> state);

  /** @brief Shared live descriptor source; never serialized. */
  std::shared_ptr<const State> state_;

  friend class B1OutputStore;
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
  /** @brief A non-directory or no-transaction-leaf foreign collision exists. */
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
 * Concurrent reads are safe while the receipt object's lifetime is protected.
 */
class B1OutputCommitReceipt final {
 public:
  /** @brief Copies one store-minted immutable receipt capability. */
  B1OutputCommitReceipt(const B1OutputCommitReceipt&) = default;

  /** @brief Moves one store-minted immutable receipt capability. */
  B1OutputCommitReceipt(B1OutputCommitReceipt&&) noexcept = default;

  /** @brief Copies one store-minted immutable receipt capability. */
  B1OutputCommitReceipt& operator=(const B1OutputCommitReceipt&) = default;

  /** @brief Moves one store-minted immutable receipt capability. */
  B1OutputCommitReceipt& operator=(B1OutputCommitReceipt&&) noexcept = default;

  /** @brief Destroys immutable receipt storage without external side effects.
   */
  ~B1OutputCommitReceipt() = default;

  /** @brief Returns the stable lowercase SHA-256 commit identity. */
  const std::string& commit_id() const noexcept { return fields_.commit_id; }

  /** @brief Returns the canonical selected output root. */
  const std::filesystem::path& resolved_root() const noexcept {
    return fields_.resolved_root;
  }

  /** @brief Returns the root-relative immutable occurrence slot. */
  const std::filesystem::path& rooted_slot() const noexcept {
    return fields_.rooted_slot;
  }

  /** @brief Returns the complete occurrence identity bound before offer. */
  const B1JobInstance& job() const noexcept { return fields_.job; }

  /** @brief Returns the fixed logical descriptor identity. */
  const std::string& logical_descriptor() const noexcept {
    return fields_.logical_descriptor;
  }

  /** @brief Returns the typed logical candidate content identity. */
  const ContentDigest& logical_content_digest() const noexcept {
    return fields_.logical_content_digest;
  }

  /** @brief Returns the fixed immutable committed generation. */
  std::uint64_t committed_generation() const noexcept {
    return fields_.committed_generation;
  }

  /** @brief Returns the exact payload leaf name. */
  const std::string& payload_name() const noexcept {
    return fields_.payload_name;
  }

  /** @brief Returns the exact manifest leaf name published last. */
  const std::string& manifest_name() const noexcept {
    return fields_.manifest_name;
  }

  /** @brief Returns the exact committed payload length. */
  std::uint64_t payload_length() const noexcept {
    return fields_.payload_length;
  }

  /** @brief Returns the exact committed manifest length. */
  std::uint64_t manifest_length() const noexcept {
    return fields_.manifest_length;
  }

  /** @brief Returns the SHA-256 of exact little-endian payload bytes. */
  const B1Sha256Digest& payload_digest() const noexcept {
    return fields_.payload_digest;
  }

  /** @brief Returns the SHA-256 of exact canonical manifest bytes. */
  const B1Sha256Digest& manifest_digest() const noexcept {
    return fields_.manifest_digest;
  }

  /** @brief Returns the requested durability contract. */
  B1OutputDurability requested_durability() const noexcept {
    return fields_.requested_durability;
  }

  /** @brief Returns the achieved durability after all barriers. */
  B1OutputDurability achieved_durability() const noexcept {
    return fields_.achieved_durability;
  }

  /** @brief Returns the stable published-manifest filesystem identity. */
  const std::string& published_manifest_identity() const noexcept {
    return fields_.published_manifest_identity;
  }

 private:
  /**
   * @brief Complete immutable fields accepted only from a minting owner.
   * @throws Nothing for aggregate initialization except owned-value movement.
   */
  struct Fields final {
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
   * @brief Mints one immutable receipt after complete store revalidation.
   * @param fields Complete trusted store result.
   * @throws std::bad_alloc only if owned movement unexpectedly allocates.
   * @note No public constructor accepts serialized or retained proof fields.
   */
  explicit B1OutputCommitReceipt(Fields fields) : fields_(std::move(fields)) {}

  /** @brief Immutable store-minted receipt facts. */
  Fields fields_;

  friend class B1OutputStore;
  friend struct testing::B1OutputCommitReceiptTestAccess;
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
  /**
   * @brief Complete snapshots for tasks newly executed by this request.
   * @note Exact public-occurrence reconciliation performs no new Compute I/O
   * work and therefore returns an empty sequence rather than fabricating a
   * current-request FSM. A caller evaluating that occurrence must retain the
   * earlier new-work stream; without it, evidence evaluation fails closed.
   */
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
  /** @brief Private anchor exists, before descriptor acquisition/recheck. */
  AfterStagingAnchorMkdirBeforeOpen,
  /** @brief Private slot name exists, before descriptor acquisition/recheck. */
  AfterStagingSlotMkdirBeforeOpen,
  /** @brief Private slot and retained descriptor exist, before task setup.
   */
  AfterSlotCreated,
  /** @brief Provisional budget is reserved and the lazy factory is entered. */
  InsideTaskFactory,
  /** @brief Submission returned Accepted and its completion was guarded. */
  AfterTaskAccepted,
  /** @brief I/O worker entered the accepted callback before artifact mutation.
   */
  BeforeTaskWork,
  /** @brief Accepted task settled and the transaction no longer owns a wait. */
  AfterTaskSettled,
  /** @brief Both tasks settled and the private slot is ready to publish. */
  BeforeSlotPublication,
  /** @brief Rename succeeded, before the private-source directory barrier. */
  AfterSlotPublicationBeforeSourceBarrier,
  /** @brief Source barrier succeeded, before the destination-root barrier. */
  AfterSourceBarrierBeforeRootBarrier,
  /** @brief Namespace barriers succeeded, before final public revalidation. */
  BeforeFinalPublicRevalidation,
  /** @brief Public slot and barriers revalidated before receipt construction.
   */
  BeforeReceiptAssembly,
};

/**
 * @brief Closed strict-cleanup operation vocabulary for fault/race tests.
 * @throws Nothing for value construction and comparison.
 */
enum class B1OutputStoreCleanupOperation : std::uint8_t {
  /** @brief Exact private manifest leaf, when still present. */
  PrivateManifestLeaf,
  /** @brief Exact published manifest leaf inside the transaction slot. */
  ManifestLeaf,
  /** @brief Exact payload leaf inside the transaction slot. */
  PayloadLeaf,
  /** @brief Exact transaction slot directory in its current namespace. */
  SlotDirectory,
  /** @brief Exact private staging-anchor directory under the held root. */
  StagingAnchorDirectory,
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
   * @throws Any test-selected exception. Before public rename, the transaction
   * first cancels/settles accepted work and removes its private slot. After
   * public rename, the public occurrence and private anchor remain pending for
   * same-commit descriptor-relative reconciliation and are never rolled back.
   * @note The callback is source-private and cannot receive root/slot fds.
   */
  using FaultInjector = void (*)(void* context, B1OutputStoreFaultPoint point);

  /** @brief Optional deterministic fault injector; production leaves null. */
  FaultInjector fault_injector = nullptr;

  /** @brief Borrowed context paired with `fault_injector`. */
  void* fault_injector_context = nullptr;

  /**
   * @brief Optional strict-cleanup race/error injection callback.
   * @param context Borrowed caller context valid through `commit()`.
   * @param operation Exact identity-verified object about to be removed.
   * @return Zero to continue, otherwise an errno value to inject.
   * @throws Nothing; implementations must contain every failure.
   * @note The callback runs only after accepted work has settled and after the
   * first identity check. Production leaves it null. A test may rename an
   * object away and back to exercise the mandatory second identity check.
   */
  using CleanupInjector =
      int (*)(void* context, B1OutputStoreCleanupOperation operation) noexcept;

  /** @brief Optional strict-cleanup injector; production leaves null. */
  CleanupInjector cleanup_injector = nullptr;

  /** @brief Borrowed context paired with `cleanup_injector`. */
  void* cleanup_injector_context = nullptr;

  /**
   * @brief Optional failure seam after the final identity recheck and before
   * name-based removal.
   * @param context Borrowed caller context valid through `commit()`.
   * @param operation Exact rechecked object awaiting removal.
   * @return Zero to continue, otherwise an errno value to fail-stop before
   * `unlinkat`/`rmdir` is attempted.
   * @throws Nothing; implementations must contain every failure.
   * @note This seam makes the unavoidable POSIX check-to-remove boundary
   * deterministic in tests. It does not authorize namespace mutation. A
   * non-cooperating same-UID actor that mutates a reserved name at this point
   * violates the class ownership precondition and is outside the contract.
   */
  using FinalCleanupInjector =
      int (*)(void* context, B1OutputStoreCleanupOperation operation) noexcept;

  /** @brief Optional final-recheck failure injector; production leaves null. */
  FinalCleanupInjector final_cleanup_injector = nullptr;

  /** @brief Borrowed context paired with `final_cleanup_injector`. */
  void* final_cleanup_injector_context = nullptr;
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
 * @throws std::system_error when descriptor acquisition, identity observation,
 * or nonblocking exclusive lock acquisition fails.
 * @note The executor and selected root must outlive this store and every call.
 * The store holds a nonblocking exclusive advisory lock on the selected root
 * for its lifetime. All cooperating processes and threads must honor that lock
 * and reserve `.b1-staging-*` plus `occurrence-*` names exclusively to the one
 * store owner during commit and cleanup. Arbitrary non-cooperating same-UID
 * namespace mutation is not covered because Darwin/Linux provide no portable
 * atomic identity-selected unlink/rmdir primitive.
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
   * @brief Re-observes root identity and filesystem type through the held fd.
   * @return Descriptor-derived root facts after path/fd binding verification.
   * @throws std::system_error for `fstatfs` failure.
   * @throws std::runtime_error for root binding or unsupported platform drift.
   * @note Call before and after a row to bind retained expected storage input
   * to the actual output namespace. Serialized strings are diagnostic only.
   */
  B1OutputStoreRootObservation observe_root_authority() const;

  /**
   * @brief Retains a copyable live root-descriptor capability for evidence.
   * @return Opaque capability sharing the current root lock and binding.
   * @throws std::system_error when descriptor duplication fails.
   * @throws std::runtime_error when the current root binding has drifted.
   * @throws std::bad_alloc when shared capability state cannot be allocated.
   * @note The capability re-observes descriptor/path/filesystem state during
   * validation and may outlive this store. Its diagnostic values cannot be
   * serialized and reconstructed into a replacement capability.
   */
  B1OutputStoreRootAuthority retain_root_authority() const;

  /**
   * @brief Commits one exact candidate image using two ordered I/O tasks.
   * @param job Complete immutable occurrence allocated before offer.
   * @param image Exact candidate CPU FP32 RGBA image.
   * @return Typed result, new-work evidence, and receipt only after barriers.
   * @throws std::bad_alloc or synchronization exceptions that prevent even a
   * typed result from being assembled.
   * @note Capacity rejection retries the same attempt-zero identity/charge for
   * exactly `kB1CapacityAdmissionAttemptLimit` total attempts. Exhaustion or a
   * non-capacity rejection returns `AdmissionFailed`, cleans the occurrence
   * slot, and records the final observation without a timing-derived policy.
   * Cleanup retryability assumes the cooperative exclusive namespace contract.
   * A matching existing public occurrence is descriptor-relatively revalidated,
   * completes any missing source/root barriers, and returns the same stable
   * receipt without writing into the public directory or fabricating new I/O
   * observations. A non-directory, empty directory, or marker-only directory
   * with no transaction-looking payload/manifest/private-manifest leaf is a
   * plainly foreign `SlotExists` collision. Any payload, manifest, or private-
   * manifest presence makes incomplete, extra, or drifted state a
   * `RevalidationFailed` transaction occurrence that remains untouched. A pre-
   * guard anchor takeover throws and preserves the ambiguous name; post-anchor
   * slot takeover or detected private cleanup drift terminates fail-stop.
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
