/**
 * @file i1_evidence_workflow.hpp
 * @brief Declares verification-only I1 evaluation and ordered-drain workflow.
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
#include <vector>

#include "benchmark/i1_evidence.hpp"

namespace ps::benchmark {

/**
 * @brief Observable milestones in the runner-owned failed-admission terminator.
 * @throws Nothing for value construction, copying, or comparison.
 * @note The sequence is closed and monotonic. In particular, no payload-digest
 * milestone exists because failed admission is release-only after Graph close.
 */
enum class I1FailedAdmissionFinalizationStage : std::uint8_t {
  /** @brief Graph close was attempted before any history cut. */
  GraphCloseCompleted,
  /** @brief The first excluded causal coordinate has been reserved. */
  HistoryCutCaptured,
  /** @brief Every unfrozen visible Value was released without traversal. */
  UnfrozenOutputsReleased,
  /** @brief Closed lifecycle/resource state was captured after release. */
  ClosedExecutionSnapshotCaptured,
  /** @brief The all-Invalid inner row reached a successful stream flush. */
  InnerRowFlushed,
  /** @brief Outer failure persistence may begin after the inner-row flush. */
  OuterFailurePersistenceStarted,
};

/**
 * @brief Injectable runner authorities used only after failed I1 admission.
 *
 * The workflow owns ordering while this port supplies the concrete Graph close,
 * closed execution snapshot, monotonic sample, and additive failure-artifact
 * persistence. The interface exposes no normal-path digest traversal and no
 * admission, scheduling, cancellation, or publication authority.
 *
 * @throws As documented by individual operations.
 * @note The manual runner and deterministic unit regression implement this same
 * interface; it is verification-only and not part of an installed ABI.
 */
class I1FailedAdmissionFinalizationPort {
 public:
  /**
   * @brief Releases a verification port without changing product state.
   * @throws Nothing.
   */
  virtual ~I1FailedAdmissionFinalizationPort() noexcept = default;

  /**
   * @brief Synchronously closes the episode Graph and revokes publication.
   * @return Exact close status; a failed status is retained in diagnostics.
   * @throws Host ownership or synchronization failures unchanged.
   * @note This is always the first externally visible finalization operation.
   */
  virtual OperationStatus close_graph() = 0;

  /**
   * @brief Captures authoritative state after close, cut, and Value release.
   * @return Closed lifecycle and resource snapshot for the failed row.
   * @throws Snapshot allocation or synchronization failures unchanged.
   */
  virtual I1ExecutionSnapshot capture_closed_execution_snapshot() = 0;

  /**
   * @brief Samples the final snapshot's process-monotonic observation time.
   * @return Time paired with `capture_closed_execution_snapshot()`.
   * @throws Injected monotonic-clock failures unchanged.
   */
  virtual std::chrono::steady_clock::time_point monotonic_now() = 0;

  /**
   * @brief Persists additive outer failure metadata after durable inner
   * evidence.
   * @param diagnostic Complete failed-admission/finalization diagnostic.
   * @return Nothing after the failure artifact is durably written.
   * @throws I/O, JSON, or allocation failures to the workflow, which preserves
   * the primary failed-admission exception and does not permit an early retry.
   */
  virtual void persist_outer_failure(std::string_view diagnostic) = 0;

  /**
   * @brief Observes one completed ordering milestone without authority.
   * @param stage Monotonic finalization stage reached by the workflow.
   * @return Nothing.
   * @throws Nothing; implementations must remain no-throw.
   * @note The production runner uses the default no-op. Deterministic tests use
   * this callback only to prove close/cut/release/flush ordering.
   */
  virtual void observe_stage(I1FailedAdmissionFinalizationStage stage) noexcept;
};

/**
 * @brief Signals that failed admission has already owned outer persistence.
 * @throws std::bad_alloc when diagnostic ownership cannot allocate.
 * @note The manual `main` catches this type separately and must not write
 * `failure.json` again. The artifact was either written after the inner row, or
 * deliberately withheld because the inner row could not be flushed first.
 */
class I1FailedAdmissionFinalizationError final : public std::runtime_error {
 public:
  /**
   * @brief Owns the terminal failed-admission diagnostic.
   * @param diagnostic Complete source and finalization diagnostic.
   * @throws std::bad_alloc when base exception storage cannot allocate.
   */
  explicit I1FailedAdmissionFinalizationError(std::string diagnostic);
};

/**
 * @brief Closes, seals, persists, and aborts one failed-admission episode.
 *
 * The workflow performs these phases exactly once and in order: Graph close;
 * history cut; ready-settlement consumption; release-only Value disposal;
 * closed observation/resource capture; fixed-width edit capture; all-Invalid
 * evaluation; ordered `episodes.ndjson` flush; additive outer failure write;
 * and terminal exception propagation. It never calls payload digest traversal.
 *
 * @param diagnostic Initial raw failed-admission diagnostic.
 * @param input Prepopulated immutable grid/baseline/golden row facts.
 * @param admissions All fixed-width admission results, including untouched
 * suffix positions and any earlier accepted settlement futures.
 * @param observations Episode collector that outlives every attached sink.
 * @param port Concrete close/snapshot/failure-persistence authorities.
 * @param episode_output Open ordered inner-row stream.
 * @param completed_rows Pre-reserved exact-slot row sequence.
 * @param written_rows Durable in/out row cursor for `episode_output`.
 * @return Never returns; throws after persistence or fail-closed suppression.
 * @throws I1FailedAdmissionFinalizationError after the inner row and outer
 * failure artifact are ordered, or when finalization cannot safely permit the
 * outer artifact. Lower-level diagnostics are retained in `what()` when
 * possible.
 * @note The function requires at least one attempted admission without an
 * accepted coordinate and forbids later edit/slot submission by construction.
 */
[[noreturn]] void finalize_i1_failed_admission(
    std::string diagnostic, I1EpisodeEvidenceInput input,
    std::array<I1EditAdmissionResult, kI1EditCount> admissions,
    I1EpisodeObservationCollector* observations,
    I1FailedAdmissionFinalizationPort* port, std::ostream* episode_output,
    std::vector<I1EpisodeInnerRow>* completed_rows, std::size_t* written_rows);

/**
 * @brief Owned nullary work item for one payload-free episode evaluation.
 * @throws Evaluator exceptions through the future returned by its launcher.
 * @note The task owns only a recoverable closed-input state. It has no Host,
 * Graph, collector, mutable Value, JSON, or file-I/O authority.
 */
using I1EpisodeEvaluationTask = std::function<I1EpisodeInnerRow()>;

/**
 * @brief Injectable launcher for one payload-free episode task.
 * @param task Sole owned evaluation task.
 * @return Valid future representing exactly that task.
 * @throws std::system_error when a worker cannot be launched.
 * @throws std::bad_alloc when launcher-owned state cannot allocate.
 * @note An exceptional return or invalid future means the task was not
 * retained for later invocation. The workflow keeps input consumption gated
 * until a valid future has been installed, so compliant launch failures leave
 * the closed input recoverable for synchronous evaluation.
 */
using I1EpisodeEvaluationLauncher =
    std::future<I1EpisodeInnerRow> (*)(  // NOLINT(whitespace/indent_namespace)
        I1EpisodeEvaluationTask task);

/**
 * @brief Injectable evaluator for one closed payload-free episode input.
 * @param input Complete closed evidence owned by value.
 * @return Evaluated inner row for the same slot.
 * @throws Evaluation allocation or structural exceptions unchanged.
 * @note Production workflow use supplies `evaluate_i1_episode`; injection is
 * retained only so deterministic tests can prove dual-failure diagnostics.
 */
using I1EpisodeEvaluator = I1EpisodeInnerRow (*)(I1EpisodeEvidenceInput input);

/**
 * @brief Optional no-throw observation that a worker reached the launch gate.
 * @return Nothing.
 * @throws Nothing.
 * @note This verification-only seam grants no input or workflow authority and
 * is null in the manual runner. Deterministic tests use it to hold launcher
 * return until the worker is known to be waiting for ownership commitment.
 */
using I1EpisodeLaunchGateObserver = void (*)() noexcept;

/**
 * @brief Reports both an async-launch failure and failed synchronous recovery.
 * @throws Nothing during construction, copying, destruction, or inspection.
 * @note `what()` combines bounded human-readable diagnostics for runner
 * `failure.json`; the two exact exception pointers remain separately
 * available even if that text is truncated.
 */
class I1EpisodeEvaluationRecoveryError final : public std::exception {
 public:
  /**
   * @brief Captures the primary launch and secondary evaluation failures.
   * @param launch_failure Original launcher/setup exception.
   * @param evaluation_failure Synchronous fallback evaluator exception.
   * @throws Nothing.
   */
  I1EpisodeEvaluationRecoveryError(
      std::exception_ptr launch_failure,
      std::exception_ptr evaluation_failure) noexcept;

  /**
   * @brief Returns a combined diagnostic suitable for failure metadata.
   * @return Stable null-terminated text owned by this exception.
   * @throws Nothing.
   */
  const char* what() const noexcept override;

  /**
   * @brief Returns the exact primary async-launch exception.
   * @return Retained non-null exception pointer.
   * @throws Nothing.
   */
  std::exception_ptr launch_failure() const noexcept;

  /**
   * @brief Returns the exact secondary synchronous-evaluation exception.
   * @return Retained non-null exception pointer.
   * @throws Nothing.
   */
  std::exception_ptr evaluation_failure() const noexcept;

 private:
  /** @brief Original launcher/setup failure retained without translation. */
  std::exception_ptr launch_failure_;
  /** @brief Synchronous recovery failure retained without translation. */
  std::exception_ptr evaluation_failure_;
  /** @brief Bounded no-allocation combined runner diagnostic. */
  std::array<char, 2048U> diagnostic_{};
};

/**
 * @brief Starts the sole evaluator or synchronously recovers a launch failure.
 * @param input Closed Value-free evidence for the next exact ordered slot.
 * @param pending_evaluation Empty optional receiving the sole valid future.
 * @param completed_rows Pre-reserved exact-slot row sequence.
 * @return Nothing after installing one future on successful launch.
 * @throws std::invalid_argument for null, occupied, unordered, or unreserved
 * workflow state.
 * @throws Original launcher/setup exception after synchronous fallback has
 * appended the current row exactly once.
 * @throws I1EpisodeEvaluationRecoveryError when launch and synchronous
 * fallback evaluation both fail; prior completed rows remain drainable.
 * @note The worker cannot consume `input` before the launcher returns a valid
 * future. On launch failure no later slot is submitted by the throwing caller.
 * `completed_rows` must reserve all `kI1GridSlotCount` rows up front so the
 * fallback append itself cannot allocate or duplicate the current slot.
 */
void start_i1_episode_evaluation(
    I1EpisodeEvidenceInput input,
    std::optional<std::future<I1EpisodeInnerRow>>* pending_evaluation,
    std::vector<I1EpisodeInnerRow>* completed_rows);

/**
 * @brief Starts/recover one evaluator through deterministic injected seams.
 * @param input Closed Value-free evidence for the next exact ordered slot.
 * @param pending_evaluation Empty optional receiving the sole valid future.
 * @param completed_rows Pre-reserved exact-slot row sequence.
 * @param launcher Non-null async launch boundary.
 * @param evaluator Non-null episode evaluator used by async and fallback paths.
 * @param gate_observer Optional no-throw worker-at-gate test observation.
 * @return Nothing after installing one future on successful launch.
 * @throws std::invalid_argument for invalid workflow state or null seams.
 * @throws Original launcher/setup exception after successful synchronous
 * recovery, or I1EpisodeEvaluationRecoveryError for dual failure.
 * @note This overload exists for deterministic long-lived regression tests;
 * the manual runner uses the production overload above.
 */
void start_i1_episode_evaluation(
    I1EpisodeEvidenceInput input,
    std::optional<std::future<I1EpisodeInnerRow>>* pending_evaluation,
    std::vector<I1EpisodeInnerRow>* completed_rows,
    I1EpisodeEvaluationLauncher launcher, I1EpisodeEvaluator evaluator,
    I1EpisodeLaunchGateObserver gate_observer = nullptr);

/**
 * @brief Appends every not-yet-written row in deterministic slot order.
 * @param output Open destination stream.
 * @param rows Closed rows already ordered by exact grid slot.
 * @param written In/out durable row cursor.
 * @return Nothing after every newly appended line is flushed and checked.
 * @throws std::invalid_argument for null state, an invalid cursor, or a row
 * whose slot differs from its exact ordered position.
 * @throws std::runtime_error when encoding, output, or flush fails.
 * @note The cursor advances only after a successful flush, so normal and
 * exceptional drains cannot write one closed row twice.
 */
void flush_i1_episode_rows(std::ostream* output,
                           const std::vector<I1EpisodeInnerRow>& rows,
                           std::size_t* written);

}  // namespace ps::benchmark
