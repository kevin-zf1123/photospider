/**
 * @file i2_evidence_workflow.cpp
 * @brief Implements recoverable I2 evaluation and ordered row persistence.
 */
#include "verification/i2_evidence_workflow.hpp"

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

#include "verification/i2_evidence_json.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Ownership decision visible to one gated async evaluator.
 * @throws Nothing for value construction and atomic transport.
 */
enum class I2EvaluationDisposition : std::uint8_t {
  /** @brief Launcher has not returned a committed future. */
  Prepared,
  /** @brief The installed sole future may consume the input. */
  AsyncCommitted,
  /** @brief Launch failed and the caller owns synchronous recovery. */
  SynchronousFallback,
};

/**
 * @brief Retains one closed I2 input until launch ownership is committed.
 * @throws Evaluator exceptions from the selected consumption path.
 * @note A worker may exist before `std::async` returns, but cannot move input
 * while disposition remains `Prepared`.
 */
class RecoverableI2EpisodeEvaluation final {
 public:
  /** @brief Creates empty prepared ownership. @throws Nothing. */
  RecoverableI2EpisodeEvaluation() noexcept = default;

  /** @brief Prevents duplicate ownership of the closed input. */
  RecoverableI2EpisodeEvaluation(const RecoverableI2EpisodeEvaluation&) =
      delete;
  /** @brief Prevents replacing ownership of the closed input. */
  RecoverableI2EpisodeEvaluation& operator=(
      const RecoverableI2EpisodeEvaluation&) = delete;

  /**
   * @brief Installs still-caller-owned input before task construction.
   * @param input Closed evidence moved after state allocation succeeds.
   * @param evaluator Non-null evaluator selected before launch.
   * @param gate_observer Optional no-throw worker observation.
   * @return Nothing.
   * @throws Nothing because evidence movement is statically no-throw.
   */
  void retain(I2EpisodeEvidenceInput&& input, I2EpisodeEvaluator evaluator,
              I2EpisodeLaunchGateObserver gate_observer) noexcept {
    input_.emplace(std::move(input));
    evaluator_ = evaluator;
    gate_observer_ = gate_observer;
  }

  /**
   * @brief Grants the installed future sole consumption authority.
   * @return Nothing.
   * @throws Nothing.
   */
  void commit_async() noexcept {
    disposition_.store(I2EvaluationDisposition::AsyncCommitted,
                       std::memory_order_release);
  }

  /**
   * @brief Waits for launch commitment and evaluates on the sole worker.
   * @return Evaluated row for the retained slot.
   * @throws Evaluator failures unchanged.
   * @throws std::logic_error if a noncompliant launcher invokes a revoked task.
   */
  I2EpisodeInnerRow evaluate_async() {
    if (gate_observer_ != nullptr) {
      gate_observer_();
    }
    I2EvaluationDisposition disposition =
        disposition_.load(std::memory_order_acquire);
    while (disposition == I2EvaluationDisposition::Prepared) {
      std::this_thread::yield();
      disposition = disposition_.load(std::memory_order_acquire);
    }
    if (disposition != I2EvaluationDisposition::AsyncCommitted) {
      throw std::logic_error(
          "I2 async evaluation task ran after launch recovery");
    }
    return consume_input();
  }

  /**
   * @brief Revokes async consumption and evaluates on the caller thread.
   * @return Evaluated row for the retained slot.
   * @throws Evaluator failures unchanged.
   * @throws std::logic_error if another owner was already committed.
   */
  I2EpisodeInnerRow evaluate_fallback() {
    I2EvaluationDisposition expected = I2EvaluationDisposition::Prepared;
    if (!disposition_.compare_exchange_strong(
            expected, I2EvaluationDisposition::SynchronousFallback,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      throw std::logic_error(
          "I2 evaluation input was already committed to another owner");
    }
    return consume_input();
  }

 private:
  /**
   * @brief Moves the retained input exactly once into its evaluator.
   * @return Evaluated row.
   * @throws Evaluator failures unchanged.
   * @throws std::logic_error for absent input or evaluator state.
   */
  I2EpisodeInnerRow consume_input() {
    if (!input_.has_value() || evaluator_ == nullptr) {
      throw std::logic_error("I2 evaluation input state is incomplete");
    }
    I2EpisodeEvidenceInput input = std::move(*input_);
    input_.reset();
    return evaluator_(std::move(input));
  }

  /** @brief Atomic one-owner launch/fallback decision. */
  std::atomic<I2EvaluationDisposition> disposition_{
      I2EvaluationDisposition::Prepared};
  /** @brief Closed Value-free input retained until ownership commitment. */
  std::optional<I2EpisodeEvidenceInput> input_;
  /** @brief Non-owning evaluator selected before launch. */
  I2EpisodeEvaluator evaluator_ = nullptr;
  /** @brief Optional verification-only worker-at-gate observation. */
  I2EpisodeLaunchGateObserver gate_observer_ = nullptr;
};

static_assert(
    std::is_nothrow_move_constructible_v<I2EpisodeEvidenceInput>,
    "recoverable I2 launch requires no-throw evidence ownership transfer");
static_assert(std::is_nothrow_move_constructible_v<I2EpisodeInnerRow>,
              "ordered I2 append requires no-throw row movement");
static_assert(
    std::is_nothrow_move_constructible_v<std::future<I2EpisodeInnerRow>>,
    "I2 launch commit requires no-throw future installation");

/**
 * @brief Launches one task with the production `std::async` boundary.
 * @param task Gated Value-free evaluation task.
 * @return Valid sole worker future.
 * @throws std::system_error or std::bad_alloc from `std::async` unchanged.
 */
std::future<I2EpisodeInnerRow> launch_i2_episode_task(
    I2EpisodeEvaluationTask task) {
  return std::async(std::launch::async, std::move(task));
}

/**
 * @brief Returns exception text without translating ownership.
 * @param failure Retained exception pointer.
 * @return Stable exception-owned text or static non-standard fallback.
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
 * @brief Appends one semicolon-delimited finalization diagnostic.
 * @param diagnostic Mutable complete runner diagnostic.
 * @param detail Nonempty fact without a leading separator.
 * @return Nothing after exact text is appended.
 * @throws std::invalid_argument for null destination.
 * @throws std::bad_alloc when string growth cannot allocate.
 */
void append_failure_diagnostic(std::string* diagnostic,
                               std::string_view detail) {
  if (diagnostic == nullptr) {
    throw std::invalid_argument("I2 failure diagnostic is null");
  }
  diagnostic->append("; ");
  diagnostic->append(detail.data(), detail.size());
}

/**
 * @brief Reports whether every failed-admission row axis is Invalid.
 * @param row Evaluated failed-admission row.
 * @return True only for four Invalid verdicts.
 * @throws Nothing.
 */
bool row_is_fully_invalid(const I2EpisodeInnerRow& row) noexcept {
  return row.latency_verdict == I1Verdict::Invalid &&
         row.waste_verdict == I1Verdict::Invalid &&
         row.memory_verdict == I1Verdict::Invalid &&
         row.output_verdict == I1Verdict::Invalid;
}

/**
 * @brief Detects one attempted admission without acceptance.
 * @param admissions Complete fixed-width episode admission records.
 * @return True when at least one attempted boundary lacks a coordinate.
 * @throws Nothing.
 */
bool contains_failed_admission(
    const std::array<I2EditAdmissionResult, kI1EditCount>&
        admissions) noexcept {
  for (const I2EditAdmissionResult& admission : admissions) {
    if (admission.admission_attempted &&
        !admission.accepted_coordinate.has_value()) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Encodes one complete I2 row with the production JSON schema.
 * @param row Complete un-compacted row.
 * @return One JSON record without a trailing newline.
 * @throws JSON or allocation failures unchanged.
 */
std::string serialize_i2_episode_row(const I2EpisodeInnerRow& row) {
  return i2_inner_row_json(row).dump();
}

/**
 * @brief Attempts outer persistence once after an earlier finalizer failure.
 * @param diagnostic Mutable failure diagnostic.
 * @param port Optional concrete outer writer.
 * @param outer_started In/out exact-once attempt flag.
 * @return Nothing after one attempt or unavailable-port suppression.
 * @throws std::bad_alloc only while appending a writer-failure diagnostic.
 * @note This helper never retries a writer already entered by the normal path.
 */
void persist_outer_after_finalizer_failure(
    std::string* diagnostic, I2FailedAdmissionFinalizationPort* port,
    bool* outer_started) {
  if (port == nullptr || outer_started == nullptr || *outer_started) {
    return;
  }
  *outer_started = true;
  port->observe_stage(
      I2FailedAdmissionFinalizationStage::OuterFailurePersistenceStarted);
  try {
    port->persist_outer_failure(*diagnostic);
  } catch (const std::exception& error) {
    append_failure_diagnostic(
        diagnostic,
        std::string("failure artifact persistence failed: ") + error.what());
  } catch (...) {
    append_failure_diagnostic(
        diagnostic,
        "failure artifact persistence raised a non-standard exception");
  }
}

}  // namespace

/** @copydoc I2OuterPersistenceOwnershipGate::claim_failed_admission_finalizer
 */
void I2OuterPersistenceOwnershipGate::
    claim_failed_admission_finalizer() noexcept {  // NOLINT(whitespace/indent_namespace)
  failed_admission_finalizer_owns_persistence_ = true;
}

/** @copydoc
 * I2OuterPersistenceOwnershipGate::failed_admission_finalizer_owns_persistence
 */
bool I2OuterPersistenceOwnershipGate::
    failed_admission_finalizer_owns_persistence()  // NOLINT(whitespace/indent_namespace)
    const noexcept {  // NOLINT(whitespace/indent_namespace)
  return failed_admission_finalizer_owns_persistence_;
}

/** @copydoc I2OuterPersistenceOwnershipGate::generic_inner_drain_is_permitted
 */
bool I2OuterPersistenceOwnershipGate::generic_inner_drain_is_permitted()
    const noexcept {  // NOLINT(whitespace/indent_namespace)
  return !failed_admission_finalizer_owns_persistence_;
}

/** @copydoc
 * I2OuterPersistenceOwnershipGate::generic_outer_persistence_is_permitted */
bool I2OuterPersistenceOwnershipGate::generic_outer_persistence_is_permitted()
    const noexcept {  // NOLINT(whitespace/indent_namespace)
  return !failed_admission_finalizer_owns_persistence_;
}

/** @copydoc claim_i2_failed_admission_if_needed */
bool claim_i2_failed_admission_if_needed(
    const I2EditAdmissionResult& admission,
    I2OuterPersistenceOwnershipGate& ownership_gate) noexcept {
  if (admission.accepted_coordinate.has_value()) {
    return false;
  }
  ownership_gate.claim_failed_admission_finalizer();
  return true;
}

/** @copydoc I2FailedAdmissionFinalizationPort::observe_stage */
void I2FailedAdmissionFinalizationPort::observe_stage(
    I2FailedAdmissionFinalizationStage stage) noexcept {
  static_cast<void>(stage);
}

/** @copydoc
 * I2FailedAdmissionFinalizationError::I2FailedAdmissionFinalizationError */
I2FailedAdmissionFinalizationError::I2FailedAdmissionFinalizationError(
    std::string diagnostic)
    : std::runtime_error(std::move(diagnostic)) {}

/** @copydoc finalize_i2_failed_admission */
[[noreturn]] void finalize_i2_failed_admission(
    std::string diagnostic, I2EpisodeEvidenceInput input,
    std::array<I2EditAdmissionResult, kI1EditCount> admissions,
    I2OuterPersistenceOwnershipGate& ownership_gate,
    I2EpisodeObservationCollector* observations,
    I2FailedAdmissionFinalizationPort* port, std::ostream* episode_output,
    std::vector<I2EpisodeInnerRow>* completed_rows, std::size_t* written_rows) {
  ownership_gate.claim_failed_admission_finalizer();
  bool outer_started = false;
  try {
    if (observations == nullptr || port == nullptr ||
        episode_output == nullptr || completed_rows == nullptr ||
        written_rows == nullptr) {
      throw std::invalid_argument(
          "I2 failed-admission finalization state is null");
    }
    if (!contains_failed_admission(admissions)) {
      throw std::invalid_argument(
          "I2 failed-admission finalization lacks a failed admission");
    }
    if (input.slot >= kI2GridSlotCount ||
        input.slot != completed_rows->size() ||
        completed_rows->capacity() < kI2GridSlotCount) {
      throw std::invalid_argument(
          "I2 failed-admission row is not the reserved next grid slot");
    }

    OperationStatus close_status = port->close_graph();
    port->observe_stage(
        I2FailedAdmissionFinalizationStage::GraphCloseCompleted);
    if (!close_status.ok) {
      append_failure_diagnostic(
          &diagnostic,
          std::string("graph-close publication revocation failed: ") +
              close_status.message);
    }

    input.observation_cut = observations->capture_history_cut();
    port->observe_stage(I2FailedAdmissionFinalizationStage::HistoryCutCaptured);

    std::array<std::optional<OperationStatus>, kI1EditCount> settlements;
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      std::future<OperationStatus>& future = admissions[edit_index].settlement;
      if (!future.valid()) {
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
        I2FailedAdmissionFinalizationStage::UnfrozenOutputsReleased);
    input.final_snapshot = port->capture_closed_execution_snapshot();
    input.final_snapshot_sample = port->monotonic_now();
    port->observe_stage(
        I2FailedAdmissionFinalizationStage::ClosedExecutionSnapshotCaptured);
    input.observations = observations->snapshot();
    for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
      input.edits[edit_index] = capture_i2_edit_evidence(
          admissions[edit_index], std::move(settlements[edit_index]));
    }

    completed_rows->push_back(evaluate_i2_episode(std::move(input)));
    if (!row_is_fully_invalid(completed_rows->back())) {
      throw std::runtime_error(
          "failed admission did not produce four Invalid verdicts");
    }
    flush_i2_episode_rows(episode_output, *completed_rows, written_rows);
    port->observe_stage(I2FailedAdmissionFinalizationStage::InnerRowFlushed);
    outer_started = true;
    port->observe_stage(
        I2FailedAdmissionFinalizationStage::OuterFailurePersistenceStarted);
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
    throw I2FailedAdmissionFinalizationError(std::move(diagnostic));
  } catch (const I2FailedAdmissionFinalizationError&) {
    throw;
  } catch (const std::exception& error) {
    append_failure_diagnostic(
        &diagnostic,
        std::string("failed-admission finalization stopped: ") + error.what());
    persist_outer_after_finalizer_failure(&diagnostic, port, &outer_started);
    throw I2FailedAdmissionFinalizationError(std::move(diagnostic));
  } catch (...) {
    append_failure_diagnostic(
        &diagnostic,
        "failed-admission finalization stopped on a non-standard exception");
    persist_outer_after_finalizer_failure(&diagnostic, port, &outer_started);
    throw I2FailedAdmissionFinalizationError(std::move(diagnostic));
  }
}

/** @copydoc rethrow_i2_runner_failure_after_generic_drain */
[[noreturn]] void rethrow_i2_runner_failure_after_generic_drain(
    std::exception_ptr primary_failure,
    const I2OuterPersistenceOwnershipGate& ownership_gate,
    std::optional<std::future<I2EpisodeInnerRow>>* pending_evaluation,
    std::vector<I2EpisodeInnerRow>* completed_rows,
    std::ostream* episode_output, std::size_t* written_rows) {
  if (!ownership_gate.generic_inner_drain_is_permitted()) {
    std::rethrow_exception(primary_failure);
  }
  if (!primary_failure || pending_evaluation == nullptr ||
      completed_rows == nullptr || episode_output == nullptr ||
      written_rows == nullptr) {
    throw std::invalid_argument("I2 generic drain state is invalid");
  }

  if (pending_evaluation->has_value() && (*pending_evaluation)->valid()) {
    try {
      I2EpisodeInnerRow row = (*pending_evaluation)->get();
      pending_evaluation->reset();
      if (row.evidence.slot != completed_rows->size() ||
          completed_rows->size() >= kI2GridSlotCount) {
        throw std::invalid_argument(
            "I2 generic drain evaluator returned an unordered slot");
      }
      completed_rows->push_back(std::move(row));
    } catch (...) {
      pending_evaluation->reset();
    }
  }
  try {
    flush_i2_episode_rows(episode_output, *completed_rows, written_rows);
  } catch (const std::exception& flush_error) {
    try {
      std::rethrow_exception(primary_failure);
    } catch (const std::exception& primary_error) {
      throw std::runtime_error(
          std::string(primary_error.what()) +
          "; evidence flush failed: " + flush_error.what());
    } catch (...) {
      throw;
    }
  }
  std::rethrow_exception(primary_failure);
}

/** @copydoc I2EpisodeEvaluationRecoveryError::I2EpisodeEvaluationRecoveryError
 */
I2EpisodeEvaluationRecoveryError::I2EpisodeEvaluationRecoveryError(
    std::exception_ptr launch_failure,
    std::exception_ptr evaluation_failure) noexcept
    : launch_failure_(std::move(launch_failure)),
      evaluation_failure_(std::move(evaluation_failure)) {
  (void)std::snprintf(
      diagnostic_.data(), diagnostic_.size(),
      "I2 async evaluation launch failed: %s; synchronous fallback "
      "evaluation failed: %s",
      exception_diagnostic(launch_failure_),
      exception_diagnostic(evaluation_failure_));
  diagnostic_.back() = '\0';
}

/** @copydoc I2EpisodeEvaluationRecoveryError::what */
const char* I2EpisodeEvaluationRecoveryError::what() const noexcept {
  return diagnostic_.data();
}

/** @copydoc I2EpisodeEvaluationRecoveryError::launch_failure */
std::exception_ptr I2EpisodeEvaluationRecoveryError::launch_failure()
    const noexcept {
  return launch_failure_;
}

/** @copydoc I2EpisodeEvaluationRecoveryError::evaluation_failure */
std::exception_ptr I2EpisodeEvaluationRecoveryError::evaluation_failure()
    const noexcept {
  return evaluation_failure_;
}

/** @copydoc start_i2_episode_evaluation */
void start_i2_episode_evaluation(
    I2EpisodeEvidenceInput input,
    std::optional<std::future<I2EpisodeInnerRow>>* pending_evaluation,
    std::vector<I2EpisodeInnerRow>* completed_rows) {
  start_i2_episode_evaluation(std::move(input), pending_evaluation,
                              completed_rows, &launch_i2_episode_task,
                              &evaluate_i2_episode, nullptr);
}

/** @copydoc start_i2_episode_evaluation */
void start_i2_episode_evaluation(
    I2EpisodeEvidenceInput input,
    std::optional<std::future<I2EpisodeInnerRow>>* pending_evaluation,
    std::vector<I2EpisodeInnerRow>* completed_rows,
    I2EpisodeEvaluationLauncher launcher, I2EpisodeEvaluator evaluator,
    I2EpisodeLaunchGateObserver gate_observer) {
  if (pending_evaluation == nullptr || completed_rows == nullptr ||
      launcher == nullptr || evaluator == nullptr) {
    throw std::invalid_argument("I2 evaluation workflow state is null");
  }
  if (pending_evaluation->has_value()) {
    throw std::invalid_argument("I2 evaluation future is already occupied");
  }
  if (input.slot >= kI2GridSlotCount || input.slot != completed_rows->size()) {
    throw std::invalid_argument(
        "I2 evaluation input is not the next exact ordered slot");
  }
  if (completed_rows->capacity() < kI2GridSlotCount) {
    throw std::invalid_argument(
        "I2 evaluation rows lack frozen-grid reserved capacity");
  }

  std::shared_ptr<RecoverableI2EpisodeEvaluation> retained;
  try {
    retained = std::make_shared<RecoverableI2EpisodeEvaluation>();
    retained->retain(std::move(input), evaluator, gate_observer);
    I2EpisodeEvaluationTask task(
        [retained]() { return retained->evaluate_async(); });
    std::future<I2EpisodeInnerRow> future = launcher(std::move(task));
    if (!future.valid()) {
      throw std::runtime_error(
          "I2 async evaluation launcher returned an invalid future");
    }
    pending_evaluation->emplace(std::move(future));
    retained->commit_async();
    return;
  } catch (...) {
    const std::exception_ptr launch_failure = std::current_exception();
    try {
      I2EpisodeInnerRow recovered = retained != nullptr
                                        ? retained->evaluate_fallback()
                                        : evaluator(std::move(input));
      completed_rows->push_back(std::move(recovered));
    } catch (...) {
      throw I2EpisodeEvaluationRecoveryError(launch_failure,
                                             std::current_exception());
    }
    std::rethrow_exception(launch_failure);
  }
}

/** @copydoc collect_i2_episode_evaluation_until */
void collect_i2_episode_evaluation_until(
    std::chrono::steady_clock::time_point deadline,
    std::optional<std::future<I2EpisodeInnerRow>>* pending_evaluation,
    std::vector<I2EpisodeInnerRow>* completed_rows) {
  if (pending_evaluation == nullptr || completed_rows == nullptr ||
      !pending_evaluation->has_value() || !(*pending_evaluation)->valid() ||
      completed_rows->capacity() < kI2GridSlotCount ||
      completed_rows->size() >= kI2GridSlotCount) {
    throw std::invalid_argument("I2 evaluator collection state is invalid");
  }
  if ((*pending_evaluation)->wait_until(deadline) !=
      std::future_status::ready) {
    throw std::runtime_error(
        "I2 Value-free evaluator missed its immutable collection boundary");
  }

  try {
    I2EpisodeInnerRow row = (*pending_evaluation)->get();
    pending_evaluation->reset();
    if (row.evidence.slot != completed_rows->size()) {
      throw std::invalid_argument(
          "I2 evaluator returned a row outside exact slot order");
    }
    completed_rows->push_back(std::move(row));
  } catch (...) {
    pending_evaluation->reset();
    throw;
  }
}

/** @copydoc flush_i2_episode_rows */
void flush_i2_episode_rows(std::ostream* output,
                           const std::vector<I2EpisodeInnerRow>& rows,
                           std::size_t* written) {
  flush_i2_episode_rows(output, rows, written, &serialize_i2_episode_row);
}

/** @copydoc flush_i2_episode_rows */
void flush_i2_episode_rows(std::ostream* output,
                           const std::vector<I2EpisodeInnerRow>& rows,
                           std::size_t* written,
                           I2EpisodeRowSerializer serializer) {
  if (output == nullptr || written == nullptr || serializer == nullptr ||
      *written > rows.size()) {
    throw std::invalid_argument("I2 episode output state is invalid");
  }
  while (*written < rows.size()) {
    const I2EpisodeInnerRow& row = rows[*written];
    if (row.evidence.slot != *written) {
      throw std::invalid_argument(
          "I2 episode rows are not in exact slot order");
    }
    const std::string encoded = serializer(row);
    *output << encoded << '\n';
    output->flush();
    if (!*output) {
      throw std::runtime_error("failed to append I2 episodes.ndjson");
    }
    ++*written;
  }
}

}  // namespace ps::benchmark
