/**
 * @file i2_evidence_workflow.hpp
 * @brief Declares verification-only I2 evaluation and ordered persistence.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/i2_evidence.hpp"

namespace ps::benchmark {

/**
 * @brief Runner-owned one-way authority for I2 failure persistence.
 * @throws Nothing for construction, claim, or inspection.
 * @note One runner invocation owns one gate on its main thread. A failed
 * admission permanently transfers both inner-row and outer-failure writes to
 * its terminal finalizer before any diagnostic allocation can fail.
 */
class I2OuterPersistenceOwnershipGate final {
 public:
  /** @brief Creates one unclaimed runner-owned gate. @throws Nothing. */
  I2OuterPersistenceOwnershipGate() noexcept = default;

  /** @brief Prevents duplicating one runner's persistence authority. */
  I2OuterPersistenceOwnershipGate(const I2OuterPersistenceOwnershipGate&) =
      delete;
  /** @brief Prevents replacing one runner's persistence authority. */
  I2OuterPersistenceOwnershipGate& operator=(
      const I2OuterPersistenceOwnershipGate&) = delete;

  /**
   * @brief Permanently assigns persistence to failed-admission finalization.
   * @return Nothing.
   * @throws Nothing.
   */
  void claim_failed_admission_finalizer() noexcept;

  /**
   * @brief Reports whether failed-admission finalization owns persistence.
   * @return True only after the monotonic claim.
   * @throws Nothing.
   */
  bool failed_admission_finalizer_owns_persistence() const noexcept;

  /**
   * @brief Reports whether generic ordered inner drain remains permitted.
   * @return False after failed-admission ownership is claimed.
   * @throws Nothing.
   */
  bool generic_inner_drain_is_permitted() const noexcept;

  /**
   * @brief Reports whether generic outer persistence remains permitted.
   * @return False after failed-admission ownership is claimed.
   * @throws Nothing.
   */
  bool generic_outer_persistence_is_permitted() const noexcept;

 private:
  /** @brief Monotonic no-allocation failed-admission ownership bit. */
  bool failed_admission_finalizer_owns_persistence_ = false;
};

/**
 * @brief Detects absent acceptance and immediately claims finalizer ownership.
 * @param admission Complete attempted or invalid-window admission evidence.
 * @param ownership_gate Runner-owned monotonic persistence gate.
 * @return True when the admission has no accepted coordinate.
 * @throws Nothing.
 * @note This must be the runner's first classification after `admit_edit()`;
 * diagnostic construction and terminal finalization follow the claim.
 */
bool claim_i2_failed_admission_if_needed(
    const I2EditAdmissionResult& admission,
    I2OuterPersistenceOwnershipGate& ownership_gate) noexcept;

/**
 * @brief Attempts one generic outer write only while its authority is live.
 * @tparam Persistence No-argument concrete persistence callable.
 * @param ownership_gate Runner-owned monotonic persistence gate.
 * @param persistence Concrete outer failure write.
 * @return Nothing after one attempt or claimed-path suppression.
 * @throws Nothing; writer failures remain best-effort at the outer boundary.
 */
template <typename Persistence>
void try_i2_generic_outer_failure_persistence(
    const I2OuterPersistenceOwnershipGate& ownership_gate,
    Persistence&& persistence) noexcept {
  if (!ownership_gate.generic_outer_persistence_is_permitted()) {
    return;
  }
  try {
    std::forward<Persistence>(persistence)();
  } catch (...) {
  }
}

/**
 * @brief Observable stages in failed I2 admission finalization.
 * @throws Nothing for construction, copying, and comparison.
 * @note The sequence deliberately has no digest or acquisition stage because
 * failed admission is release-only after Graph closure.
 */
enum class I2FailedAdmissionFinalizationStage : std::uint8_t {
  /** @brief Graph close was attempted before any evidence cut. */
  GraphCloseCompleted,
  /** @brief The first excluded causal coordinate was reserved. */
  HistoryCutCaptured,
  /** @brief Every unfrozen visible Value was released without traversal. */
  UnfrozenOutputsReleased,
  /** @brief Closed lifecycle/resource state was captured after release. */
  ClosedExecutionSnapshotCaptured,
  /** @brief The all-Invalid inner row reached successful ordered flush. */
  InnerRowFlushed,
  /** @brief Outer failure persistence began after the inner flush. */
  OuterFailurePersistenceStarted,
};

/**
 * @brief Injectable authorities used only after failed I2 admission.
 * @throws As documented by individual virtual operations.
 * @note The interface grants no admission, payload, scheduling, cancellation,
 * JSON, or normal-path publication authority.
 */
class I2FailedAdmissionFinalizationPort {
 public:
  /** @brief Releases the verification port without product mutation. */
  virtual ~I2FailedAdmissionFinalizationPort() noexcept = default;

  /**
   * @brief Synchronously closes the episode Graph and revokes publication.
   * @return Exact close status retained in failure diagnostics.
   * @throws Host ownership or synchronization failures unchanged.
   */
  virtual OperationStatus close_graph() = 0;

  /**
   * @brief Captures authoritative closed execution/resource state.
   * @return Closed snapshot after cut and release-only Value disposal.
   * @throws Snapshot allocation or synchronization failures unchanged.
   */
  virtual I1ExecutionSnapshot capture_closed_execution_snapshot() = 0;

  /**
   * @brief Samples the final snapshot's process-monotonic time.
   * @return Time paired with the closed execution snapshot.
   * @throws Injected monotonic-clock failures unchanged.
   */
  virtual std::chrono::steady_clock::time_point monotonic_now() = 0;

  /**
   * @brief Persists additive outer failure metadata after the Invalid row.
   * @param diagnostic Complete failure/finalization diagnostic.
   * @return Nothing after the artifact is written.
   * @throws I/O, encoding, or allocation failures unchanged to the workflow.
   */
  virtual void persist_outer_failure(std::string_view diagnostic) = 0;

  /**
   * @brief Observes a completed ordering stage without gaining authority.
   * @param stage Monotonic stage reached by the workflow.
   * @return Nothing.
   * @throws Nothing; implementations must remain no-throw.
   */
  virtual void observe_stage(I2FailedAdmissionFinalizationStage stage) noexcept;
};

/**
 * @brief Diagnoses terminal failed-admission finalization.
 * @throws std::bad_alloc when diagnostic ownership cannot allocate.
 * @note Persistence ownership is already fixed by the no-throw gate and never
 * depends on this dynamic exception type.
 */
class I2FailedAdmissionFinalizationError final : public std::runtime_error {
 public:
  /**
   * @brief Owns one complete terminal diagnostic.
   * @param diagnostic Source and finalization diagnostic.
   * @throws std::bad_alloc when base exception storage cannot allocate.
   */
  explicit I2FailedAdmissionFinalizationError(std::string diagnostic);
};

/**
 * @brief Closes, seals, persists, and aborts one failed-admission episode.
 * @param diagnostic Initial raw failed-admission diagnostic.
 * @param input Prepopulated immutable grid/baseline/golden row facts.
 * @param admissions Fixed-width attempted and untouched suffix admissions.
 * @param ownership_gate Already-claimed runner persistence authority.
 * @param observations Episode collector outliving every attached sink.
 * @param port Concrete close/snapshot/outer-persistence authorities.
 * @param episode_output Open ordered inner-row stream.
 * @param completed_rows Pre-reserved exact-slot row sequence.
 * @param written_rows Durable in/out stream cursor.
 * @return Never returns.
 * @throws I2FailedAdmissionFinalizationError after ordered terminal handling,
 * or when handling cannot safely reach outer persistence.
 * @throws Allocation failures when even terminal diagnostic ownership fails.
 * @note The workflow performs Graph close, history cut, ready settlement
 * consumption, release-only Value disposal, closed snapshot, fixed-width
 * capture, all-Invalid evaluation, inner flush, and outer write exactly once.
 * It performs no payload traversal and grants no later edit or slot authority.
 */
[[noreturn]] void finalize_i2_failed_admission(
    std::string diagnostic, I2EpisodeEvidenceInput input,
    std::array<I2EditAdmissionResult, kI1EditCount> admissions,
    I2OuterPersistenceOwnershipGate& ownership_gate,
    I2EpisodeObservationCollector* observations,
    I2FailedAdmissionFinalizationPort* port, std::ostream* episode_output,
    std::vector<I2EpisodeInnerRow>* completed_rows, std::size_t* written_rows);

/**
 * @brief Rethrows one runner failure after any permitted ordered row drain.
 * @param primary_failure Non-null primary exception from the runner catch.
 * @param ownership_gate Runner-owned monotonic persistence authority.
 * @param pending_evaluation Optional sole Value-free evaluator future.
 * @param completed_rows Pre-reserved exact-slot row sequence.
 * @param episode_output Open ordered inner-row stream.
 * @param written_rows Durable in/out stream cursor.
 * @return Never returns.
 * @throws Original primary failure after successful drain, or a combined
 * standard diagnostic when ordered flush also fails.
 * @note Failed-admission ownership immediately rethrows without consuming a
 * future or touching the stream, preventing duplicate terminal artifacts.
 */
[[noreturn]] void rethrow_i2_runner_failure_after_generic_drain(
    std::exception_ptr primary_failure,
    const I2OuterPersistenceOwnershipGate& ownership_gate,
    std::optional<std::future<I2EpisodeInnerRow>>* pending_evaluation,
    std::vector<I2EpisodeInnerRow>* completed_rows,
    std::ostream* episode_output, std::size_t* written_rows);

/**
 * @brief Owned nullary work item for one Value-free I2 evaluation.
 * @throws Evaluator exceptions through the launcher future.
 * @note The task has no Host, Graph, collector, mutable Value, serializer, or
 * file authority.
 */
using I2EpisodeEvaluationTask = std::function<I2EpisodeInnerRow()>;

/**
 * @brief Injectable launcher for one sole Value-free evaluation task.
 * @param task Sole recoverable evaluation task.
 * @return Valid future representing exactly that task.
 * @throws std::system_error or std::bad_alloc on launch failure.
 */
using I2EpisodeEvaluationLauncher =
    std::future<I2EpisodeInnerRow> (*)(  // NOLINT(whitespace/indent_namespace)
        I2EpisodeEvaluationTask task);

/**
 * @brief Injectable evaluator for one complete closed I2 input.
 * @param input Complete Value-free evidence owned by value.
 * @return Evaluated row for the same slot.
 * @throws Evaluation failures unchanged.
 */
using I2EpisodeEvaluator = I2EpisodeInnerRow (*)(I2EpisodeEvidenceInput input);

/**
 * @brief Optional no-throw observation that a worker reached its launch gate.
 * @return Nothing.
 * @throws Nothing.
 */
using I2EpisodeLaunchGateObserver = void (*)() noexcept;

/**
 * @brief Injectable full-row serializer used only by explicit ordered drain.
 * @param row Complete un-compacted Value-free row.
 * @return One JSON record without the trailing newline.
 * @throws Encoding or allocation failures unchanged.
 */
using I2EpisodeRowSerializer = std::string (*)(const I2EpisodeInnerRow& row);

/**
 * @brief Reports both an async launch failure and failed sync recovery.
 * @throws Nothing for construction, copying, destruction, and inspection.
 */
class I2EpisodeEvaluationRecoveryError final : public std::exception {
 public:
  /**
   * @brief Captures primary launch and secondary evaluation failures.
   * @param launch_failure Original launcher/setup exception.
   * @param evaluation_failure Synchronous fallback exception.
   * @throws Nothing.
   */
  I2EpisodeEvaluationRecoveryError(
      std::exception_ptr launch_failure,
      std::exception_ptr evaluation_failure) noexcept;

  /** @brief Returns the bounded combined diagnostic. @throws Nothing. */
  const char* what() const noexcept override;

  /** @brief Returns the exact launch failure. @throws Nothing. */
  std::exception_ptr launch_failure() const noexcept;

  /** @brief Returns the exact fallback failure. @throws Nothing. */
  std::exception_ptr evaluation_failure() const noexcept;

 private:
  /** @brief Original launcher/setup failure. */
  std::exception_ptr launch_failure_;
  /** @brief Synchronous recovery failure. */
  std::exception_ptr evaluation_failure_;
  /** @brief Bounded no-allocation diagnostic buffer. */
  std::array<char, 2048U> diagnostic_{};
};

/**
 * @brief Starts the sole evaluator or synchronously recovers launch failure.
 * @param input Closed Value-free evidence for the next ordered slot.
 * @param pending_evaluation Empty optional receiving the sole valid future.
 * @param completed_rows Pre-reserved exact-slot row sequence.
 * @return Nothing after future installation.
 * @throws std::invalid_argument for null, occupied, unordered, or unreserved
 * state; launcher failure after exact synchronous recovery; or
 * I2EpisodeEvaluationRecoveryError when both launch and recovery fail.
 * @note Input consumption is atomically gated until a valid future is stored.
 */
void start_i2_episode_evaluation(
    I2EpisodeEvidenceInput input,
    std::optional<std::future<I2EpisodeInnerRow>>* pending_evaluation,
    std::vector<I2EpisodeInnerRow>* completed_rows);

/**
 * @brief Starts or recovers evaluation through deterministic injected seams.
 * @param input Closed Value-free evidence for the next ordered slot.
 * @param pending_evaluation Empty optional receiving the sole valid future.
 * @param completed_rows Pre-reserved exact-slot row sequence.
 * @param launcher Non-null async launch boundary.
 * @param evaluator Non-null async/fallback evaluator.
 * @param gate_observer Optional no-throw worker-at-gate observation.
 * @return Nothing after future installation.
 * @throws Same failures as the production overload.
 * @note This overload is a long-lived behavior seam, not product authority.
 */
void start_i2_episode_evaluation(
    I2EpisodeEvidenceInput input,
    std::optional<std::future<I2EpisodeInnerRow>>* pending_evaluation,
    std::vector<I2EpisodeInnerRow>* completed_rows,
    I2EpisodeEvaluationLauncher launcher, I2EpisodeEvaluator evaluator,
    I2EpisodeLaunchGateObserver gate_observer = nullptr);

/**
 * @brief Collects the sole evaluator before an immutable runner boundary.
 * @param deadline Absolute maximum wait boundary.
 * @param pending_evaluation Occupied optional holding one valid future.
 * @param completed_rows Pre-reserved exact-slot row sequence.
 * @return Nothing after appending exactly the next slot.
 * @throws std::invalid_argument for null, absent, invalid, or unreserved state.
 * @throws std::runtime_error when the evaluator is not ready by `deadline`.
 * @throws Evaluator failures unchanged after future ownership is cleared.
 * @note No serializer or persistence operation occurs here.
 */
void collect_i2_episode_evaluation_until(
    std::chrono::steady_clock::time_point deadline,
    std::optional<std::future<I2EpisodeInnerRow>>* pending_evaluation,
    std::vector<I2EpisodeInnerRow>* completed_rows);

/**
 * @brief Flushes every not-yet-written row in deterministic slot order.
 * @param output Open destination stream.
 * @param rows Complete un-compacted rows in exact slot order.
 * @param written Durable in/out row cursor.
 * @return Nothing after every new line is encoded, written, flushed, checked,
 * and only then committed by cursor advance.
 * @throws std::invalid_argument for null state, invalid cursor, or bad order.
 * @throws std::runtime_error when encoding, output, or flush fails.
 * @note This is the only production JSON/NDJSON authority in the workflow.
 */
void flush_i2_episode_rows(std::ostream* output,
                           const std::vector<I2EpisodeInnerRow>& rows,
                           std::size_t* written);

/**
 * @brief Flushes rows through a deterministic injected serializer.
 * @param output Open destination stream.
 * @param rows Complete un-compacted rows in exact slot order.
 * @param written Durable in/out row cursor.
 * @param serializer Non-null full-row serializer.
 * @return Nothing after ordered durable cursor advancement.
 * @throws Same failures as the production overload, plus serializer failures.
 * @note Evaluation never receives this callable; only explicit drain does.
 */
void flush_i2_episode_rows(std::ostream* output,
                           const std::vector<I2EpisodeInnerRow>& rows,
                           std::size_t* written,
                           I2EpisodeRowSerializer serializer);

}  // namespace ps::benchmark
