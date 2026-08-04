/**
 * @file i1_evidence_workflow.hpp
 * @brief Declares verification-only I1 evaluation and ordered-drain workflow.
 */
#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iosfwd>
#include <optional>
#include <vector>

#include "benchmark/i1_evidence.hpp"

namespace ps::benchmark {

/**
 * @brief Pre-history-cut digest traversal selected for one episode outcome.
 * @throws Nothing for value construction or comparison.
 */
enum class I1PreCutDigestPolicy : std::uint8_t {
  /** @brief Freeze outputs during the normal pre-cut measurement window. */
  FreezePublishedOutputs,
  /** @brief Preserve missing digests after a failed admission and Graph close.
   */
  SkipPayloadTraversal,
};

/**
 * @brief Selects whether an episode may traverse visible payloads before cut.
 * @param admission_failed True when failed admission selects Graph close.
 * @return Freeze for a normal episode, otherwise skip payload traversal.
 * @throws Nothing.
 * @note After the history cut, both paths may only release unfrozen Values;
 * neither path may compute a digest.
 */
I1PreCutDigestPolicy i1_pre_cut_digest_policy(bool admission_failed) noexcept;

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
