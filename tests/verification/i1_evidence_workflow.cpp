/**
 * @file i1_evidence_workflow.cpp
 * @brief Implements recoverable I1 evaluation launch and ordered row drain.
 */
#include "verification/i1_evidence_workflow.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "verification/i1_evidence_json.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Ownership decision visible to the gated async evaluation task.
 * @throws Nothing for value construction and atomic transport.
 */
enum class I1EvaluationDisposition : std::uint8_t {
  /** @brief Launcher has not yet returned a committed future. */
  Prepared,
  /** @brief The sole future owns permission to consume the input. */
  AsyncCommitted,
  /** @brief Launch failed and the caller owns synchronous recovery. */
  SynchronousFallback,
};

/**
 * @brief Retains one closed input until async launch ownership is committed.
 * @throws Evaluation exceptions from `evaluate_async` or `evaluate_fallback`.
 * @note A worker may start before `std::async` returns, but it can only spin on
 * the atomic disposition. It cannot move the input until the caller installs
 * the valid future and commits `AsyncCommitted` with release semantics.
 */
class RecoverableI1EpisodeEvaluation final {
 public:
  /** @brief Creates empty prepared ownership. @throws Nothing. */
  RecoverableI1EpisodeEvaluation() noexcept = default;

  /** @brief Prevents duplicate ownership of the single closed input. */
  RecoverableI1EpisodeEvaluation(const RecoverableI1EpisodeEvaluation&) =
      delete;
  /** @brief Prevents replacing ownership of the single closed input. */
  RecoverableI1EpisodeEvaluation& operator=(
      const RecoverableI1EpisodeEvaluation&) = delete;

  /**
   * @brief Installs the still-caller-owned input before task construction.
   * @param input Closed evidence moved only after state allocation succeeds.
   * @param evaluator Non-null evaluation function.
   * @param gate_observer Optional no-throw worker-at-gate observation.
   * @return Nothing.
   * @throws Nothing because the evidence move is statically required no-throw.
   */
  void retain(I1EpisodeEvidenceInput&& input, I1EpisodeEvaluator evaluator,
              I1EpisodeLaunchGateObserver gate_observer) noexcept {
    input_.emplace(std::move(input));
    evaluator_ = evaluator;
    gate_observer_ = gate_observer;
  }

  /**
   * @brief Grants the installed async future sole input-consumption authority.
   * @return Nothing.
   * @throws Nothing.
   */
  void commit_async() noexcept {
    disposition_.store(I1EvaluationDisposition::AsyncCommitted,
                       std::memory_order_release);
  }

  /**
   * @brief Waits for launch commitment and evaluates on the sole worker.
   * @return Evaluated row for the retained slot.
   * @throws Evaluator exceptions unchanged.
   * @throws std::logic_error if a noncompliant launcher invokes a revoked task.
   */
  I1EpisodeInnerRow evaluate_async() {
    if (gate_observer_ != nullptr) {
      gate_observer_();
    }
    I1EvaluationDisposition disposition =
        disposition_.load(std::memory_order_acquire);
    while (disposition == I1EvaluationDisposition::Prepared) {
      std::this_thread::yield();
      disposition = disposition_.load(std::memory_order_acquire);
    }
    if (disposition != I1EvaluationDisposition::AsyncCommitted) {
      throw std::logic_error(
          "I1 async evaluation task ran after launch recovery");
    }
    return consume_input();
  }

  /**
   * @brief Revokes async consumption and evaluates on the caller thread.
   * @return Evaluated row for the retained slot.
   * @throws Evaluator exceptions unchanged.
   * @throws std::logic_error if async ownership was already committed.
   */
  I1EpisodeInnerRow evaluate_fallback() {
    I1EvaluationDisposition expected = I1EvaluationDisposition::Prepared;
    if (!disposition_.compare_exchange_strong(
            expected, I1EvaluationDisposition::SynchronousFallback,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      throw std::logic_error(
          "I1 evaluation input was already committed to another owner");
    }
    return consume_input();
  }

 private:
  /**
   * @brief Moves the retained input exactly once into the configured evaluator.
   * @return Evaluated inner row.
   * @throws Evaluator exceptions unchanged.
   * @throws std::logic_error for structurally absent input/evaluator state.
   */
  I1EpisodeInnerRow consume_input() {
    if (!input_.has_value() || evaluator_ == nullptr) {
      throw std::logic_error("I1 evaluation input state is incomplete");
    }
    I1EpisodeEvidenceInput input = std::move(*input_);
    input_.reset();
    return evaluator_(std::move(input));
  }

  /** @brief Atomic one-owner launch/fallback decision. */
  std::atomic<I1EvaluationDisposition> disposition_{
      I1EvaluationDisposition::Prepared};
  /** @brief Closed payload-free input retained until ownership commitment. */
  std::optional<I1EpisodeEvidenceInput> input_;
  /** @brief Non-owning evaluator function selected before launch. */
  I1EpisodeEvaluator evaluator_ = nullptr;
  /** @brief Optional verification-only worker-at-gate observation. */
  I1EpisodeLaunchGateObserver gate_observer_ = nullptr;
};

static_assert(
    std::is_nothrow_move_constructible_v<I1EpisodeEvidenceInput>,
    "recoverable I1 launch requires no-throw evidence ownership transfer");
static_assert(std::is_nothrow_move_constructible_v<I1EpisodeInnerRow>,
              "ordered I1 fallback append requires no-throw row movement");
static_assert(
    std::is_nothrow_move_constructible_v<std::future<I1EpisodeInnerRow>>,
    "I1 launch commit requires no-throw future installation");

/**
 * @brief Launches one owned task with the production `std::async` boundary.
 * @param task Gated payload-free evaluation task.
 * @return Valid sole worker future.
 * @throws std::system_error or std::bad_alloc from `std::async` unchanged.
 */
std::future<I1EpisodeInnerRow> launch_i1_episode_task(
    I1EpisodeEvaluationTask task) {
  return std::async(std::launch::async, std::move(task));
}

/**
 * @brief Returns exception text without translating or allocating ownership.
 * @param failure Retained exception pointer.
 * @return Stable exception-owned text, or a static non-standard fallback.
 * @throws Nothing.
 */
const char* exception_diagnostic(const std::exception_ptr& failure) noexcept {
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "non-standard exception";
  }
}

/**
 * @brief Appends one semicolon-delimited failed-finalization diagnostic.
 * @param diagnostic Mutable complete runner diagnostic.
 * @param detail Nonempty additional fact without a leading separator.
 * @return Nothing after exact text ownership is appended.
 * @throws std::bad_alloc when string growth cannot allocate.
 */
void append_failure_diagnostic(std::string* diagnostic,
                               std::string_view detail) {
  if (diagnostic == nullptr) {
    throw std::invalid_argument("I1 failure diagnostic is null");
  }
  diagnostic->append("; ");
  diagnostic->append(detail.data(), detail.size());
}

/**
 * @brief Reports whether every independent episode verdict is Invalid.
 * @param row Evaluated failed-admission row.
 * @return True only for four Invalid verdicts.
 * @throws Nothing.
 * @note Failed admission aborts the frozen episode, so retaining any partial
 * Pass or Fail claim is structurally inconsistent with the closed evidence.
 */
bool row_is_fully_invalid(const I1EpisodeInnerRow& row) noexcept {
  return row.latency_verdict == I1Verdict::Invalid &&
         row.waste_verdict == I1Verdict::Invalid &&
         row.memory_verdict == I1Verdict::Invalid &&
         row.output_verdict == I1Verdict::Invalid;
}

/**
 * @brief Detects the required failed/invalid admission in fixed-width input.
 * @param admissions Complete episode admission results.
 * @return True when an attempted boundary has no accepted coordinate.
 * @throws Nothing.
 */
bool contains_failed_admission(
    const std::array<I1EditAdmissionResult, kI1EditCount>&
        admissions) noexcept {
  for (const I1EditAdmissionResult& admission : admissions) {
    if (admission.admission_attempted &&
        !admission.accepted_coordinate.has_value()) {
      return true;
    }
  }
  return false;
}

}  // namespace

/** @copydoc I1FailedAdmissionFinalizationPort::observe_stage */
void I1FailedAdmissionFinalizationPort::observe_stage(
    I1FailedAdmissionFinalizationStage stage) noexcept {
  static_cast<void>(stage);
}

/**
 * @brief Owns one terminal diagnostic for the specialized runner catch.
 * @param diagnostic Complete failed-admission/finalization diagnostic.
 * @throws std::bad_alloc when base exception storage cannot allocate.
 */
I1FailedAdmissionFinalizationError::I1FailedAdmissionFinalizationError(
    std::string diagnostic)
    : std::runtime_error(std::move(diagnostic)) {}

/** @copydoc finalize_i1_failed_admission */
[[noreturn]] void finalize_i1_failed_admission(
    std::string diagnostic, I1EpisodeEvidenceInput input,
    std::array<I1EditAdmissionResult, kI1EditCount> admissions,
    I1EpisodeObservationCollector* observations,
    I1FailedAdmissionFinalizationPort* port, std::ostream* episode_output,
    std::vector<I1EpisodeInnerRow>* completed_rows, std::size_t* written_rows) {
  try {
    if (observations == nullptr || port == nullptr ||
        episode_output == nullptr || completed_rows == nullptr ||
        written_rows == nullptr) {
      throw std::invalid_argument(
          "I1 failed-admission finalization state is null");
    }
    if (!contains_failed_admission(admissions)) {
      throw std::invalid_argument(
          "I1 failed-admission finalization lacks a failed admission");
    }
    if (input.slot >= kI1GridSlotCount ||
        input.slot != completed_rows->size() ||
        completed_rows->capacity() < kI1GridSlotCount) {
      throw std::invalid_argument(
          "I1 failed-admission row is not the reserved next grid slot");
    }

    OperationStatus close_status = port->close_graph();
    port->observe_stage(
        I1FailedAdmissionFinalizationStage::GraphCloseCompleted);
    if (!close_status.ok) {
      append_failure_diagnostic(
          &diagnostic,
          std::string("graph-close publication revocation failed: ") +
              close_status.message);
    }

    input.observation_cut = observations->capture_history_cut();
    port->observe_stage(I1FailedAdmissionFinalizationStage::HistoryCutCaptured);

    std::array<std::optional<OperationStatus>, kI1EditCount> settlements;
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      std::future<OperationStatus>& future = admissions[edit_index].settlement;
      if (!future.valid()) {
        continue;
      }
      if (future.wait_for(std::chrono::nanoseconds(0)) !=
          std::future_status::ready) {
        append_failure_diagnostic(&diagnostic,
                                  std::string("accepted edit ") +
                                      std::to_string(edit_index) +
                                      " remained active after graph close");
        continue;
      }
      try {
        settlements[edit_index] = future.get();
      } catch (const std::exception& error) {
        append_failure_diagnostic(&diagnostic,
                                  std::string("accepted edit ") +
                                      std::to_string(edit_index) +
                                      " settlement raised: " + error.what());
      } catch (...) {
        append_failure_diagnostic(
            &diagnostic, std::string("accepted edit ") +
                             std::to_string(edit_index) +
                             " settlement raised a non-standard exception");
      }
    }

    observations->release_unfrozen_visible_outputs();
    port->observe_stage(
        I1FailedAdmissionFinalizationStage::UnfrozenOutputsReleased);
    input.final_snapshot = port->capture_closed_execution_snapshot();
    input.final_snapshot_sample = port->monotonic_now();
    port->observe_stage(
        I1FailedAdmissionFinalizationStage::ClosedExecutionSnapshotCaptured);
    input.observations = observations->snapshot();
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      input.edits[edit_index] = capture_i1_edit_evidence(
          admissions[edit_index], std::move(settlements[edit_index]));
    }

    completed_rows->push_back(evaluate_i1_episode(std::move(input)));
    if (!row_is_fully_invalid(completed_rows->back())) {
      append_failure_diagnostic(
          &diagnostic,
          "failed admission did not produce four Invalid verdicts");
    }
    flush_i1_episode_rows(episode_output, *completed_rows, written_rows);
    port->observe_stage(I1FailedAdmissionFinalizationStage::InnerRowFlushed);
    port->observe_stage(
        I1FailedAdmissionFinalizationStage::OuterFailurePersistenceStarted);
    try {
      port->persist_outer_failure(diagnostic);
    } catch (const std::exception& error) {
      append_failure_diagnostic(
          &diagnostic,
          std::string("failure artifact persistence failed: ") + error.what());
    } catch (...) {
      append_failure_diagnostic(
          &diagnostic,
          "failure artifact persistence raised a non-standard exception");
    }
    throw I1FailedAdmissionFinalizationError(std::move(diagnostic));
  } catch (const I1FailedAdmissionFinalizationError&) {
    throw;
  } catch (const std::exception& error) {
    append_failure_diagnostic(
        &diagnostic,
        std::string("failed-admission finalization stopped before outer "
                    "failure persistence: ") +
            error.what());
    throw I1FailedAdmissionFinalizationError(std::move(diagnostic));
  } catch (...) {
    append_failure_diagnostic(
        &diagnostic,
        "failed-admission finalization stopped on a non-standard exception "
        "before outer failure persistence");
    throw I1FailedAdmissionFinalizationError(std::move(diagnostic));
  }
}

/** @copydoc I1EpisodeEvaluationRecoveryError::I1EpisodeEvaluationRecoveryError
 */
I1EpisodeEvaluationRecoveryError::I1EpisodeEvaluationRecoveryError(
    std::exception_ptr launch_failure,
    std::exception_ptr evaluation_failure) noexcept
    : launch_failure_(std::move(launch_failure)),
      evaluation_failure_(std::move(evaluation_failure)) {
  (void)std::snprintf(
      diagnostic_.data(), diagnostic_.size(),
      "I1 async evaluation launch failed: %s; synchronous fallback "
      "evaluation failed: %s",
      exception_diagnostic(launch_failure_),
      exception_diagnostic(evaluation_failure_));
  diagnostic_.back() = '\0';
}

/** @copydoc I1EpisodeEvaluationRecoveryError::what */
const char* I1EpisodeEvaluationRecoveryError::what() const noexcept {
  return diagnostic_.data();
}

/** @copydoc I1EpisodeEvaluationRecoveryError::launch_failure */
std::exception_ptr I1EpisodeEvaluationRecoveryError::launch_failure()
    const noexcept {
  return launch_failure_;
}

/** @copydoc I1EpisodeEvaluationRecoveryError::evaluation_failure */
std::exception_ptr I1EpisodeEvaluationRecoveryError::evaluation_failure()
    const noexcept {
  return evaluation_failure_;
}

/** @copydoc start_i1_episode_evaluation */
void start_i1_episode_evaluation(
    I1EpisodeEvidenceInput input,
    std::optional<std::future<I1EpisodeInnerRow>>* pending_evaluation,
    std::vector<I1EpisodeInnerRow>* completed_rows) {
  start_i1_episode_evaluation(std::move(input), pending_evaluation,
                              completed_rows, &launch_i1_episode_task,
                              &evaluate_i1_episode, nullptr);
}

/** @copydoc start_i1_episode_evaluation */
void start_i1_episode_evaluation(
    I1EpisodeEvidenceInput input,
    std::optional<std::future<I1EpisodeInnerRow>>* pending_evaluation,
    std::vector<I1EpisodeInnerRow>* completed_rows,
    I1EpisodeEvaluationLauncher launcher, I1EpisodeEvaluator evaluator,
    I1EpisodeLaunchGateObserver gate_observer) {
  if (pending_evaluation == nullptr || completed_rows == nullptr ||
      launcher == nullptr || evaluator == nullptr) {
    throw std::invalid_argument("I1 evaluation workflow state is null");
  }
  if (pending_evaluation->has_value()) {
    throw std::invalid_argument("I1 evaluation future is already occupied");
  }
  if (input.slot >= kI1GridSlotCount || input.slot != completed_rows->size()) {
    throw std::invalid_argument(
        "I1 evaluation input is not the next exact ordered slot");
  }
  if (completed_rows->capacity() < kI1GridSlotCount) {
    throw std::invalid_argument(
        "I1 evaluation rows lack frozen-grid reserved capacity");
  }

  std::shared_ptr<RecoverableI1EpisodeEvaluation> retained;
  try {
    retained = std::make_shared<RecoverableI1EpisodeEvaluation>();
    retained->retain(std::move(input), evaluator, gate_observer);
    I1EpisodeEvaluationTask task(
        [retained]() { return retained->evaluate_async(); });
    std::future<I1EpisodeInnerRow> future = launcher(std::move(task));
    if (!future.valid()) {
      throw std::runtime_error(
          "I1 async evaluation launcher returned an invalid future");
    }
    pending_evaluation->emplace(std::move(future));
    retained->commit_async();
    return;
  } catch (...) {
    const std::exception_ptr launch_failure = std::current_exception();
    try {
      I1EpisodeInnerRow recovered = retained != nullptr
                                        ? retained->evaluate_fallback()
                                        : evaluator(std::move(input));
      completed_rows->push_back(std::move(recovered));
    } catch (...) {
      throw I1EpisodeEvaluationRecoveryError(launch_failure,
                                             std::current_exception());
    }
    std::rethrow_exception(launch_failure);
  }
}

/** @copydoc flush_i1_episode_rows */
void flush_i1_episode_rows(std::ostream* output,
                           const std::vector<I1EpisodeInnerRow>& rows,
                           std::size_t* written) {
  if (output == nullptr || written == nullptr || *written > rows.size()) {
    throw std::invalid_argument("I1 episode output state is invalid");
  }
  while (*written < rows.size()) {
    if (rows[*written].evidence.slot != *written) {
      throw std::invalid_argument(
          "I1 episode rows are not in exact slot order");
    }
    *output << i1_inner_row_json(rows[*written]).dump() << '\n';
    output->flush();
    if (!*output) {
      throw std::runtime_error("failed to append episodes.ndjson");
    }
    ++*written;
  }
}

}  // namespace ps::benchmark
