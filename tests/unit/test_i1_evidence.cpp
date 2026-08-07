#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/evidence_envelope.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/i1_evidence.hpp"
#include "compute/compute_run.hpp"
#include "photospider/data/value.hpp"
#include "support/b1_test_environment.hpp"
#include "verification/i1_evidence_json.hpp"
#include "verification/i1_evidence_workflow.hpp"

namespace ps::benchmark {
namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;
using std::chrono_literals::operator""us;

/**
 * @brief Creates a tiny Ready DenseTensor with deterministic logical content.
 * @return Immutable one-byte CPU Value usable by canonical digest tests.
 * @throws Value publication/allocation failures unchanged.
 */
Value make_test_output() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {1U, 1U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                      StridedLayout{{1, 1}}, {std::byte{0x2a}});
}

/**
 * @brief Creates one canonical success status.
 * @return Public operation success value.
 * @throws Nothing.
 */
OperationStatus success_status() {
  return OperationStatus{};
}

/**
 * @brief Builds the exact storage-N/A environment accepted by the I1 producer.
 * @return Self-validating same-base I1 claims with the shared frozen fixture.
 * @throws Canonical environment and allocation failures unchanged.
 */
B1EnvironmentEvidence make_i1_pair_environment() {
  B1EnvironmentEvidence environment = testing::make_b1_test_environment(8U, 1U);
  environment.workload_id = kI1WorkloadId;
  environment.fixture_digest = evidence_i1_component_fixture_digest();
  environment.storage_manifest.reset();
  environment.claimed_storage_digest.reset();
  environment.storage_raw_proof.reset();
  environment.storage_eligibility.reset();
  environment.storage_actual_observation.reset();
  environment.environment_class_manifest = encode_b1_environment_class(
      {testing::known_b1_field("base_environment_digest", "sha256",
                               b1_digest_hex(environment.claimed_base_digest)),
       testing::known_b1_field("storage_environment_applicability", "enum",
                               "not-applicable"),
       testing::known_b1_field("storage_environment_not_applicable_reason",
                               "enum", "row-has-no-output-commit"),
       testing::not_applicable_b1_field("storage_environment_digest", "sha256",
                                        "row-has-no-output-commit")});
  environment.claimed_environment_class_digest =
      digest_b1_environment_manifest(environment.environment_class_manifest);
  return environment;
}

/**
 * @brief Creates one complete synthetic episode using exact frozen controls.
 * @param slot Continuous I1 grid slot.
 * @param final_latency Accepted-to-visible final duration.
 * @return Complete raw evaluator input with no discarded work.
 * @throws Product Value/digest allocation and checked-time errors unchanged.
 */
I1EpisodeEvidenceInput make_valid_input(
    std::size_t slot, std::chrono::nanoseconds final_latency = 10ms) {
  I1EpisodeEvidenceInput input;
  input.replicate_ordinal = 1U;
  input.slot = slot;
  input.grid_origin = std::chrono::steady_clock::time_point(1s);
  input.episode_origin = i1_episode_origin(input.grid_origin, slot);
  input.terminal_boundary = i1_terminal_boundary(input.grid_origin);
  input.measurement_start =
      checked_i1_time_add(input.episode_origin, kI1MeasurementStartOffset);
  input.measurement_end =
      checked_i1_time_add(input.episode_origin, kI1MeasurementEndOffset);
  input.final_snapshot_sample = input.measurement_end;

  input.baseline.host_resources.limits.cpu_slots = 8U;
  input.baseline.host_resources.limits.retained_memory_bytes = 1U << 20U;
  input.baseline.host_resources.limits.scratch_bytes = 1U << 20U;
  input.baseline.host_resources.limits.ready_entries = 1024U;
  input.baseline.host_resources.limits.ready_bytes = 1U << 20U;
  input.final_snapshot.host_resources.limits =
      input.baseline.host_resources.limits;
  input.final_snapshot.host_resources.high_water.cpu_slots = 1U;
  input.final_snapshot.host_resources.high_water.retained_memory_bytes = 4096U;
  input.final_snapshot.host_resources.high_water.ready_entries = 1U;
  input.final_snapshot.host_resources.high_water.ready_bytes = 4096U;

  input.baseline.lifecycle.service_instance_id = 41U;
  input.baseline.lifecycle.telemetry_epoch = 43U;
  input.baseline.lifecycle.snapshot_cut = 10U;
  input.baseline.lifecycle.counters.registered_graph_count = 1U;
  input.baseline.lifecycle.counters.open_graph_count = 1U;
  input.baseline.lifecycle.counters.live_policy_binding_count = 1U;
  input.final_snapshot.lifecycle = input.baseline.lifecycle;
  input.final_snapshot.lifecycle.snapshot_cut = 1000U;
  input.final_snapshot.lifecycle.next_cursor = 1000U;

  const Value output = make_test_output();
  input.expected_final_digest = i1_frozen_final_content_digest();

  std::uint64_t causal_sequence = 1U;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    const auto nominal = checked_i1_time_add(
        input.episode_origin,
        std::chrono::nanoseconds(kI1EditStride.count() *
                                 static_cast<std::int64_t>(edit_index)));
    const auto admission = checked_i1_time_add(nominal, 1ms);
    const auto deadline = checked_i1_time_add(admission, kI1DeadlineBudget);
    input.edits[edit_index] = I1EditEvidence{
        edit_index,
        kI1EditCoefficients[edit_index],
        i1_edit_region(edit_index),
        nominal,
        true,
        admission,
        true,
        static_cast<std::uint64_t>(edit_index + 1U),
        deadline,
        I1HostReturnEvidence{checked_i1_time_add(admission, 100us),
                             success_status(), true},
        I1AcceptedCoordinate{admission,
                             static_cast<std::uint64_t>(edit_index + 1U)},
        success_status(),
    };

    const std::uint64_t generation = edit_index + 1U;
    const std::uint64_t run_id = 100U + edit_index;
    input.observations.current_generations.push_back(
        I1ObservedCurrentGeneration{
            edit_index, generation, admission, causal_sequence++,
            I1AcceptedCoordinate{admission, edit_index + 1U}});
    input.observations.service_starts.push_back(I1ObservedServiceStart{
        edit_index,
        run_id,
        generation,
        0U,
        compute::ComputeRunQuality::Full,
        compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                               deadline, 1U, 8U},
        100U,
        checked_i1_time_add(admission, 1ms),
        causal_sequence++,
    });
    const auto visible_at = checked_i1_time_add(
        admission, edit_index == kI1EditCount - 1U ? final_latency : 2ms);
    input.observations.visible_outputs.push_back(I1ObservedVisibleOutput{
        edit_index, run_id, generation, visible_at, causal_sequence++, output,
        false, std::nullopt});
    input.observations.terminals.push_back(I1ObservedTerminal{
        edit_index, run_id, generation,
        compute::ComputeRunTerminalKind::Succeeded,
        checked_i1_time_add(visible_at, 1us), causal_sequence++});
    input.observations.run_quiescences.push_back(
        I1ObservedRunLifecycleTransition{edit_index, run_id, generation,
                                         checked_i1_time_add(visible_at, 2us),
                                         causal_sequence++});
    input.observations.resource_settlements.push_back(
        I1ObservedRunLifecycleTransition{edit_index, run_id, generation,
                                         checked_i1_time_add(visible_at, 3us),
                                         causal_sequence++});
    input.observations.host_settlements.push_back(I1ObservedHostSettlement{
        edit_index, checked_i1_time_add(visible_at, 4us), causal_sequence++});
  }
  input.observation_cut =
      I1ObservationHistoryCut{input.measurement_end, causal_sequence};
  for (I1ObservedVisibleOutput& visible : input.observations.visible_outputs) {
    freeze_i1_visible_output_digest(&visible);
  }
  input.observations.visible_outputs.back().content_digest =
      ContentDigestResult{ContentDigestState::Available,
                          i1_frozen_final_content_digest(),
                          {}};
  return input;
}

/**
 * @brief Selects a mutable observation by edit from one event vector.
 * @tparam Event Observation type carrying `edit_index`.
 * @param events Mutable event vector.
 * @param edit_index Desired frozen edit.
 * @return Reference to the first matching event.
 * @throws std::logic_error when the synthetic fixture is malformed.
 */
template <typename Event>
Event& event_for_edit(std::vector<Event>* events, std::size_t edit_index) {
  const auto position = std::find_if(
      events->begin(), events->end(),
      [edit_index](const Event& e) { return e.edit_index == edit_index; });
  if (position == events->end()) {
    throw std::logic_error("synthetic I1 event is missing");
  }
  return *position;
}

/**
 * @brief Rebinds every synthetic product event for one edit to a generation.
 * @param input Complete mutable synthetic episode.
 * @param edit_index Frozen edit whose Run join keys are updated.
 * @param generation Nonzero unique generation to assign.
 * @return Nothing.
 * @throws std::logic_error when the synthetic fixture lacks currentness.
 * @note The helper changes only the preparation identity. Accepted-coordinate
 * and observation-causal ordering remain authoritative and unchanged.
 */
void rebind_edit_generation(I1EpisodeEvidenceInput* input,
                            std::size_t edit_index, std::uint64_t generation) {
  event_for_edit(&input->observations.current_generations, edit_index)
      .generation = generation;
  for (I1ObservedServiceStart& event : input->observations.service_starts) {
    if (event.edit_index == edit_index) {
      event.generation = generation;
    }
  }
  for (I1ObservedCancellation& event : input->observations.cancellations) {
    if (event.edit_index == edit_index) {
      event.generation = generation;
    }
  }
  for (I1ObservedTerminal& event : input->observations.terminals) {
    if (event.edit_index == edit_index) {
      event.generation = generation;
    }
  }
  for (I1ObservedVisibleOutput& event : input->observations.visible_outputs) {
    if (event.edit_index == edit_index) {
      event.generation = generation;
    }
  }
  for (I1ObservedRunLifecycleTransition& event :
       input->observations.run_quiescences) {
    if (event.edit_index == edit_index) {
      event.generation = generation;
    }
  }
  for (I1ObservedRunLifecycleTransition& event :
       input->observations.resource_settlements) {
    if (event.edit_index == edit_index) {
      event.generation = generation;
    }
  }
}

/**
 * @brief Shifts every sequence at or after one insertion coordinate.
 * @tparam Event Observation type carrying `causal_sequence`.
 * @param events Mutable event vector.
 * @param first_shifted First sequence moved forward by one.
 * @return Nothing.
 * @throws Nothing.
 */
template <typename Event>
void shift_event_sequences(std::vector<Event>* events,
                           std::uint64_t first_shifted) noexcept {
  for (Event& event : *events) {
    if (event.causal_sequence >= first_shifted) {
      ++event.causal_sequence;
    }
  }
}

/**
 * @brief Opens one sequence gap across a complete synthetic observation set.
 * @param observations Mutable episode observation vectors.
 * @param cut Mutable first-excluded history coordinate.
 * @param first_shifted First existing sequence moved forward by one.
 * @return Nothing.
 * @throws Nothing while the bounded synthetic sequence is below UINT64_MAX.
 */
void open_observation_sequence_gap(I1EpisodeObservationSnapshot* observations,
                                   I1ObservationHistoryCut* cut,
                                   std::uint64_t first_shifted) noexcept {
  shift_event_sequences(&observations->current_generations, first_shifted);
  shift_event_sequences(&observations->service_starts, first_shifted);
  shift_event_sequences(&observations->cancellations, first_shifted);
  shift_event_sequences(&observations->terminals, first_shifted);
  shift_event_sequences(&observations->visible_outputs, first_shifted);
  shift_event_sequences(&observations->run_quiescences, first_shifted);
  shift_event_sequences(&observations->resource_settlements, first_shifted);
  shift_event_sequences(&observations->host_settlements, first_shifted);
  if (cut->causal_sequence >= first_shifted) {
    ++cut->causal_sequence;
  }
}

/**
 * @brief Removes every synthetic observation belonging to one failed edit.
 * @tparam Event Observation type carrying `edit_index`.
 * @param events Mutable event vector.
 * @param edit_index Failed edit whose product facts must be absent.
 * @return Nothing.
 * @throws Nothing under vector element movement/destruction.
 */
template <typename Event>
void erase_events_for_edit(std::vector<Event>* events,
                           std::size_t edit_index) noexcept {
  events->erase(std::remove_if(events->begin(), events->end(),
                               [edit_index](const Event& event) {
                                 return event.edit_index == edit_index;
                               }),
                events->end());
}

/**
 * @brief Deterministically rejects one async launch as resource exhaustion.
 * @param task Unscheduled task destroyed on exceptional return.
 * @return Never returns.
 * @throws std::system_error with a stable injected resource diagnostic.
 */
std::future<I1EpisodeInnerRow> throw_system_error_launcher(
    I1EpisodeEvaluationTask task) {
  (void)task;
  throw std::system_error(
      std::make_error_code(std::errc::resource_unavailable_try_again),
      "injected I1 async launcher failure");
}

/**
 * @brief Deterministically rejects one async launch as allocation failure.
 * @param task Unscheduled task destroyed on exceptional return.
 * @return Never returns.
 * @throws std::bad_alloc unconditionally.
 */
std::future<I1EpisodeInnerRow> throw_bad_alloc_launcher(
    I1EpisodeEvaluationTask task) {
  (void)task;
  throw std::bad_alloc();
}

/**
 * @brief Deterministically rejects synchronous recovery evaluation.
 * @param input Recovered closed evidence consumed by value.
 * @return Never returns.
 * @throws std::runtime_error with a stable injected evaluation diagnostic.
 */
I1EpisodeInnerRow throw_recovery_evaluator(I1EpisodeEvidenceInput input) {
  (void)input;
  throw std::runtime_error("injected I1 synchronous evaluation failure");
}

/** @brief True after a controlled worker reaches the internal commit gate. */
std::atomic<bool> gate_worker_arrived{false};

/** @brief True after the controlled evaluator is allowed to consume input. */
std::atomic<bool> gate_evaluator_entered{false};

/** @brief Whether the launcher observed evaluation before returning. */
std::atomic<bool> gate_launcher_saw_evaluator{false};

/**
 * @brief Records that a controlled worker reached the prepared launch gate.
 * @return Nothing.
 * @throws Nothing.
 */
void observe_worker_at_launch_gate() noexcept {
  gate_worker_arrived.store(true, std::memory_order_release);
}

/**
 * @brief Launches a worker and waits until it reaches the prepared gate.
 * @param task Gated task whose evaluator must remain blocked before return.
 * @return Sole valid async future.
 * @throws std::system_error or std::bad_alloc from `std::async` unchanged.
 * @note A one-second observation guard avoids an unbounded test hang; the
 * future is still returned so the workflow can commit/join before assertions.
 */
std::future<I1EpisodeInnerRow> launch_worker_before_return(
    I1EpisodeEvaluationTask task) {
  std::future<I1EpisodeInnerRow> future =
      std::async(std::launch::async, std::move(task));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!gate_worker_arrived.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  gate_launcher_saw_evaluator.store(
      gate_evaluator_entered.load(std::memory_order_acquire),
      std::memory_order_release);
  return future;
}

/**
 * @brief Records evaluator entry before applying the real I1 evaluation.
 * @param input Recovered or asynchronously committed closed evidence.
 * @return Real evaluated row.
 * @throws Evaluator allocation and structural exceptions unchanged.
 */
I1EpisodeInnerRow observe_then_evaluate(I1EpisodeEvidenceInput input) {
  gate_evaluator_entered.store(true, std::memory_order_release);
  return evaluate_i1_episode(std::move(input));
}

/**
 * @brief Decodes exact slot order from verification NDJSON text.
 * @param content Complete newline-delimited row text.
 * @return Slot values in physical line order.
 * @throws JSON parsing/type and allocation failures unchanged.
 */
std::vector<std::size_t> ndjson_slots(const std::string& content) {
  std::vector<std::size_t> slots;
  std::istringstream input(content);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      slots.push_back(
          nlohmann::json::parse(line).at("grid").at("slot").get<std::size_t>());
    }
  }
  return slots;
}

/**
 * @brief Rejects every inner-row byte and records physical stream attempts.
 * @throws Nothing for construction, destruction, or counter inspection.
 * @note Tests may clear only the owning `std::ostream` state after a failure so
 * a prohibited second flush would become visible as another buffer call; the
 * buffer itself continues to reject all output.
 */
class RejectingI1EpisodeStreamBuffer final : public std::streambuf {
 public:
  /**
   * @brief Returns the number of bulk, scalar, and sync attempts.
   * @return Monotonic physical call count.
   * @throws Nothing.
   */
  std::size_t attempt_count() const noexcept { return attempt_count_; }

 protected:
  /**
   * @brief Rejects one bulk write without consuming a byte.
   * @param source Borrowed bytes ignored by the rejecting fixture.
   * @param count Requested byte count.
   * @return Zero bytes consumed.
   * @throws Nothing.
   */
  std::streamsize xsputn(const char* source, std::streamsize count) override {
    static_cast<void>(source);
    static_cast<void>(count);
    ++attempt_count_;
    return 0;
  }

  /**
   * @brief Rejects one scalar write.
   * @param value Requested character.
   * @return End-of-file to signal failure.
   * @throws Nothing.
   */
  int_type overflow(int_type value) override {
    static_cast<void>(value);
    ++attempt_count_;
    return traits_type::eof();
  }

  /**
   * @brief Rejects one explicit stream synchronization.
   * @return Negative failure result.
   * @throws Nothing.
   */
  int sync() override {
    ++attempt_count_;
    return -1;
  }

 private:
  /** @brief Monotonic physical output/synchronization attempt count. */
  std::size_t attempt_count_ = 0U;
};

/**
 * @brief Proves a launcher `system_error` closes and drains the current slot.
 * @throws Nothing when recovery, exact ordering, and propagation are stable.
 * @note The statement following the throwing workflow models later slot
 * submission and must remain unreachable. A second drain proves cursor-based
 * retry does not duplicate either the prior or recovered row.
 */
TEST(I1Evidence, AsyncSystemErrorRecoversCurrentRowBeforeFailurePropagation) {
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  rows.push_back(evaluate_i1_episode(make_valid_input(0U)));
  std::optional<std::future<I1EpisodeInnerRow>> pending_evaluation;
  std::size_t later_submission_count = 0U;
  bool failure_propagated = false;

  try {
    start_i1_episode_evaluation(make_valid_input(1U), &pending_evaluation,
                                &rows, &throw_system_error_launcher,
                                &evaluate_i1_episode);
    ++later_submission_count;
  } catch (const std::system_error& error) {
    failure_propagated = true;
    EXPECT_EQ(error.code(),
              std::make_error_code(std::errc::resource_unavailable_try_again));
    EXPECT_NE(
        std::string(error.what()).find("injected I1 async launcher failure"),
        std::string::npos);
  }

  EXPECT_TRUE(failure_propagated);
  EXPECT_EQ(later_submission_count, 0U);
  EXPECT_FALSE(pending_evaluation.has_value());
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0U].evidence.slot, 0U);
  EXPECT_EQ(rows[1U].evidence.slot, 1U);
  EXPECT_EQ(
      std::count_if(rows.begin(), rows.end(),
                    [](const auto& row) { return row.evidence.slot == 1U; }),
      1);

  std::ostringstream output;
  std::size_t written = 0U;
  flush_i1_episode_rows(&output, rows, &written);
  flush_i1_episode_rows(&output, rows, &written);
  EXPECT_EQ(written, 2U);
  EXPECT_EQ(ndjson_slots(output.str()), (std::vector<std::size_t>{0U, 1U}));
}

/**
 * @brief Proves allocation-like launcher failure uses the same recovery path.
 * @throws Nothing when the original `bad_alloc` propagates after one row.
 */
TEST(I1Evidence, AsyncBadAllocRecoversCurrentRowBeforeFailurePropagation) {
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  std::optional<std::future<I1EpisodeInnerRow>> pending_evaluation;

  EXPECT_THROW(start_i1_episode_evaluation(
                   make_valid_input(0U), &pending_evaluation, &rows,
                   &throw_bad_alloc_launcher, &evaluate_i1_episode),
               std::bad_alloc);
  EXPECT_FALSE(pending_evaluation.has_value());
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().evidence.slot, 0U);
}

/**
 * @brief Proves dual launch/evaluation failure retains both exact exceptions.
 * @throws Nothing when both diagnostics survive and prior rows remain
 * drainable.
 */
TEST(I1Evidence, AsyncLaunchAndRecoveryFailureRetainBothDiagnostics) {
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  rows.push_back(evaluate_i1_episode(make_valid_input(0U)));
  std::optional<std::future<I1EpisodeInnerRow>> pending_evaluation;

  try {
    start_i1_episode_evaluation(make_valid_input(1U), &pending_evaluation,
                                &rows, &throw_system_error_launcher,
                                &throw_recovery_evaluator);
    FAIL() << "expected combined I1 launch/recovery failure";
  } catch (const I1EpisodeEvaluationRecoveryError& error) {
    EXPECT_NE(
        std::string(error.what()).find("injected I1 async launcher failure"),
        std::string::npos);
    EXPECT_NE(std::string(error.what())
                  .find("injected I1 synchronous evaluation failure"),
              std::string::npos);
    EXPECT_THROW(std::rethrow_exception(error.launch_failure()),
                 std::system_error);
    EXPECT_THROW(std::rethrow_exception(error.evaluation_failure()),
                 std::runtime_error);
  }

  EXPECT_FALSE(pending_evaluation.has_value());
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().evidence.slot, 0U);
  std::ostringstream output;
  std::size_t written = 0U;
  flush_i1_episode_rows(&output, rows, &written);
  EXPECT_EQ(ndjson_slots(output.str()), (std::vector<std::size_t>{0U}));
}

/**
 * @brief Proves successful launch installs exactly one future and no fallback.
 * @throws Nothing when the production async boundary transfers ownership once.
 */
TEST(I1Evidence, AsyncSuccessInstallsSoleFutureWithoutSynchronousFallback) {
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  std::optional<std::future<I1EpisodeInnerRow>> pending_evaluation;

  start_i1_episode_evaluation(make_valid_input(0U), &pending_evaluation, &rows);
  ASSERT_TRUE(pending_evaluation.has_value());
  EXPECT_TRUE(pending_evaluation->valid());
  EXPECT_TRUE(rows.empty());

  rows.push_back(pending_evaluation->get());
  pending_evaluation.reset();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().evidence.slot, 0U);
}

/**
 * @brief Proves a worker entering before launcher return cannot consume input.
 * @throws Nothing when the atomic commit gate enforces the handoff order.
 */
TEST(I1Evidence, AsyncWorkerWaitsForFutureInstallationBeforeEvaluation) {
  gate_worker_arrived.store(false, std::memory_order_relaxed);
  gate_evaluator_entered.store(false, std::memory_order_relaxed);
  gate_launcher_saw_evaluator.store(false, std::memory_order_relaxed);
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  std::optional<std::future<I1EpisodeInnerRow>> pending_evaluation;

  start_i1_episode_evaluation(make_valid_input(0U), &pending_evaluation, &rows,
                              &launch_worker_before_return,
                              &observe_then_evaluate,
                              &observe_worker_at_launch_gate);

  EXPECT_TRUE(gate_worker_arrived.load(std::memory_order_acquire));
  EXPECT_FALSE(gate_launcher_saw_evaluator.load(std::memory_order_acquire));
  ASSERT_TRUE(pending_evaluation.has_value());
  rows.push_back(pending_evaluation->get());
  pending_evaluation.reset();
  EXPECT_TRUE(gate_evaluator_entered.load(std::memory_order_acquire));
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().evidence.slot, 0U);
}

/**
 * @brief Proves nearest rank uses one-based `ceil(p*N)` selection.
 * @throws Nothing when exact percentile selection remains stable.
 */
TEST(I1Evidence, NearestRankUsesCeilingOneBasedIndex) {
  std::vector<std::chrono::nanoseconds> samples;
  for (std::int64_t value = 1; value <= 200; ++value) {
    samples.emplace_back(value);
  }
  std::reverse(samples.begin(), samples.end());

  EXPECT_EQ(i1_nearest_rank(samples, 50U, 100U), std::chrono::nanoseconds(100));
  EXPECT_EQ(i1_nearest_rank(samples, 95U, 100U), std::chrono::nanoseconds(190));
  EXPECT_EQ(i1_nearest_rank(samples, 99U, 100U), std::chrono::nanoseconds(198));
  EXPECT_THROW(i1_nearest_rank({}, 50U, 100U), std::invalid_argument);
}

/**
 * @brief Proves product binding uses row-local sequence, not causal sequence.
 * @throws Nothing when the complete synthetic row remains valid.
 */
TEST(I1Evidence, AcceptedAndObservationSequenceDomainsRemainIndependent) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  const I1ObservedCurrentGeneration& second_generation =
      event_for_edit(&input.observations.current_generations, 1U);
  ASSERT_TRUE(second_generation.accepted_coordinate.has_value());
  EXPECT_EQ(second_generation.accepted_coordinate->event_sequence(), 2U);
  EXPECT_NE(second_generation.accepted_coordinate->event_sequence(),
            second_generation.causal_sequence);

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves accepted-coordinate currentness permits inverse generation
 * allocation order.
 * @throws Nothing when the complete synthetic row remains valid.
 * @note Edit zero carries generation two and edit one carries generation one,
 * reproducing publication after the requests prepared in the opposite order.
 */
TEST(I1Evidence, BoundCoordinateOrderDoesNotRequireIncreasingGenerations) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  rebind_edit_generation(&input, 0U, 2U);
  rebind_edit_generation(&input, 1U, 1U);

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_TRUE(row.validity_reasons.empty());
  ASSERT_TRUE(row.accepted_products[0U].has_value());
  ASSERT_TRUE(row.accepted_products[1U].has_value());
  ASSERT_TRUE(row.accepted_products[0U]->accepted_coordinate.has_value());
  ASSERT_TRUE(row.accepted_products[1U]->accepted_coordinate.has_value());
  EXPECT_EQ(row.accepted_products[0U]->generation, 2U);
  EXPECT_EQ(row.accepted_products[1U]->generation, 1U);
  EXPECT_LT(*row.accepted_products[0U]->accepted_coordinate,
            *row.accepted_products[1U]->accepted_coordinate);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves inverse ordering does not permit duplicate preparation IDs.
 * @throws Nothing when the evaluator fails closed on the duplicate.
 */
TEST(I1Evidence, BoundCoordinateOrderStillRequiresUniqueGenerations) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  rebind_edit_generation(&input, 1U, 1U);

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "edit has missing, zero, or duplicate generation"),
            row.validity_reasons.end());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
}

/**
 * @brief Rejects a generation callback bound to any coordinate but its edit.
 * @throws Nothing when the evaluator fails closed on the exact mismatch.
 */
TEST(I1Evidence, CurrentGenerationRequiresExactAcceptedCoordinateBinding) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  I1ObservedCurrentGeneration& generation =
      event_for_edit(&input.observations.current_generations, 3U);
  generation.accepted_coordinate =
      I1AcceptedCoordinate{input.edits[3U].admission_sample,
                           *input.edits[3U].reserved_event_sequence + 100U};

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "current generation is not bound to the accepted "
                      "coordinate"),
            row.validity_reasons.end());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves a failed Host admission remains a source-faithful inner JSON
 * row without accepted/current/product facts.
 * @throws Nothing when evaluation and the runner-owned serializer fail closed.
 * @note The first ten edits retain their observer history. The eleventh edit
 * retains its pre-call sample, reservation, deadline, and exact raw Host
 * return; the fixed-width twelfth suffix records that admission was not
 * attempted. Graph-close state proves publication/resource revocation.
 */
TEST(I1Evidence, FailedAdmissionSerializesRawEvidenceWithoutAcceptedProduct) {
  constexpr std::size_t kFailedEdit = kI1EditCount - 2U;
  constexpr std::size_t kUnattemptedEdit = kI1EditCount - 1U;
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  I1EditEvidence& failed_edit = input.edits[kFailedEdit];
  const OperationStatus admission_failure{
      false,
      OperationErrorDomain::Graph,
      static_cast<std::int32_t>(GraphErrc::ComputeError),
      graph_error_stable_name(GraphErrc::ComputeError),
      "injected I1 admission failure",
  };
  failed_edit.host_return = I1HostReturnEvidence{
      checked_i1_time_add(failed_edit.admission_sample, 100us),
      admission_failure, false};
  failed_edit.accepted_coordinate.reset();
  failed_edit.settlement_status.reset();

  I1EditEvidence& unattempted_edit = input.edits[kUnattemptedEdit];
  unattempted_edit.admission_attempted = false;
  unattempted_edit.admission_sample = {};
  unattempted_edit.admission_window_valid = false;
  unattempted_edit.reserved_event_sequence.reset();
  unattempted_edit.deadline.reset();
  unattempted_edit.host_return.reset();
  unattempted_edit.accepted_coordinate.reset();
  unattempted_edit.settlement_status.reset();

  erase_events_for_edit(&input.observations.current_generations, kFailedEdit);
  erase_events_for_edit(&input.observations.current_generations,
                        kUnattemptedEdit);
  erase_events_for_edit(&input.observations.service_starts, kFailedEdit);
  erase_events_for_edit(&input.observations.service_starts, kUnattemptedEdit);
  erase_events_for_edit(&input.observations.cancellations, kFailedEdit);
  erase_events_for_edit(&input.observations.cancellations, kUnattemptedEdit);
  erase_events_for_edit(&input.observations.terminals, kFailedEdit);
  erase_events_for_edit(&input.observations.terminals, kUnattemptedEdit);
  erase_events_for_edit(&input.observations.visible_outputs, kFailedEdit);
  erase_events_for_edit(&input.observations.visible_outputs, kUnattemptedEdit);
  erase_events_for_edit(&input.observations.run_quiescences, kFailedEdit);
  erase_events_for_edit(&input.observations.run_quiescences, kUnattemptedEdit);
  erase_events_for_edit(&input.observations.resource_settlements, kFailedEdit);
  erase_events_for_edit(&input.observations.resource_settlements,
                        kUnattemptedEdit);
  erase_events_for_edit(&input.observations.host_settlements, kFailedEdit);
  erase_events_for_edit(&input.observations.host_settlements, kUnattemptedEdit);
  input.final_snapshot.lifecycle.counters.registered_graph_count = 0U;
  input.final_snapshot.lifecycle.counters.open_graph_count = 0U;
  input.final_snapshot.lifecycle.counters.live_policy_binding_count = 0U;

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  const I1EditEvidence& recorded_failed_edit = row.evidence.edits[kFailedEdit];
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
  EXPECT_FALSE(row.accepted_products[kFailedEdit].has_value());

  const nlohmann::json encoded = i1_inner_row_json(row);
  const nlohmann::json& encoded_edit = encoded.at("edits").at(kFailedEdit);
  EXPECT_TRUE(encoded_edit.at("admission_attempted").get<bool>());
  EXPECT_EQ(encoded_edit.at("admission_sample_ns").get<std::int64_t>(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                recorded_failed_edit.admission_sample.time_since_epoch())
                .count());
  EXPECT_EQ(encoded_edit.at("reserved_event_sequence").get<std::uint64_t>(),
            kFailedEdit + 1U);
  ASSERT_TRUE(recorded_failed_edit.deadline.has_value());
  EXPECT_EQ(encoded_edit.at("deadline_ns").get<std::int64_t>(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                recorded_failed_edit.deadline->time_since_epoch())
                .count());
  const nlohmann::json& encoded_return = encoded_edit.at("host_return");
  EXPECT_FALSE(encoded_return.at("status").at("ok").get<bool>());
  EXPECT_EQ(encoded_return.at("status").at("domain").get<std::uint32_t>(),
            static_cast<std::uint32_t>(OperationErrorDomain::Graph));
  EXPECT_EQ(encoded_return.at("status").at("code").get<std::int32_t>(),
            static_cast<std::int32_t>(GraphErrc::ComputeError));
  EXPECT_EQ(encoded_return.at("status").at("name").get<std::string>(),
            graph_error_stable_name(GraphErrc::ComputeError));
  EXPECT_EQ(encoded_return.at("status").at("message").get<std::string>(),
            "injected I1 admission failure");
  EXPECT_FALSE(encoded_return.at("future_valid").get<bool>());
  EXPECT_TRUE(encoded_edit.at("accepted_coordinate").is_null());
  EXPECT_TRUE(encoded_edit.at("settlement_status").is_null());
  EXPECT_TRUE(encoded.at("accepted_products").at(kFailedEdit).is_null());

  ASSERT_EQ(encoded.at("edits").size(), kI1EditCount);
  const nlohmann::json& encoded_unattempted_edit =
      encoded.at("edits").at(kUnattemptedEdit);
  EXPECT_FALSE(encoded_unattempted_edit.at("admission_attempted").get<bool>());
  EXPECT_TRUE(encoded_unattempted_edit.at("admission_sample_ns").is_null());
  EXPECT_FALSE(
      encoded_unattempted_edit.at("admission_window_valid").get<bool>());
  EXPECT_TRUE(encoded_unattempted_edit.at("reserved_event_sequence").is_null());
  EXPECT_TRUE(encoded_unattempted_edit.at("deadline_ns").is_null());
  EXPECT_TRUE(encoded_unattempted_edit.at("host_return").is_null());
  EXPECT_TRUE(encoded_unattempted_edit.at("accepted_coordinate").is_null());
  EXPECT_TRUE(encoded_unattempted_edit.at("settlement_status").is_null());
  EXPECT_TRUE(encoded.at("accepted_products").at(kUnattemptedEdit).is_null());

  const nlohmann::json& encoded_observations = encoded.at("observations");
  EXPECT_EQ(encoded_observations.at("current_generations").size(), kFailedEdit);
  EXPECT_EQ(encoded_observations.at("service_starts").size(), kFailedEdit);
  EXPECT_EQ(encoded_observations.at("visible_outputs").size(), kFailedEdit);
  for (const nlohmann::json& current :
       encoded_observations.at("current_generations")) {
    EXPECT_LT(current.at("edit_index").get<std::size_t>(), kFailedEdit);
  }
  EXPECT_EQ(encoded.at("resource_high_water_and_final")
                .at("host")
                .at("reserved")
                .at("cpu_slots")
                .get<std::uint64_t>(),
            0U);
  const nlohmann::json& final_counters =
      encoded.at("resource_high_water_and_final")
          .at("lifecycle")
          .at("counters");
  EXPECT_EQ(final_counters.at("registered_graph_count").get<std::uint64_t>(),
            0U);
  EXPECT_EQ(final_counters.at("open_graph_count").get<std::uint64_t>(), 0U);
  EXPECT_EQ(encoded.at("verdicts").at("latency").get<std::string>(), "invalid");
  EXPECT_EQ(encoded.at("verdicts").at("waste").get<std::string>(), "invalid");
  EXPECT_EQ(encoded.at("verdicts").at("memory").get<std::string>(), "invalid");
  EXPECT_EQ(encoded.at("verdicts").at("output").get<std::string>(), "invalid");
  EXPECT_FALSE(encoded.at("outer_canonical_envelope_claim").get<bool>());

  const I1ReplicateSummary summary = evaluate_i1_replicate({row});
  EXPECT_EQ(summary.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Deterministic additive outer-persistence outcome for workflow tests.
 * @throws Nothing for construction, copying, or comparison.
 */
enum class I1OuterPersistenceFailureMode : std::uint8_t {
  /** @brief Outer persistence completes normally. */
  None,
  /** @brief Outer persistence throws a standard runtime exception. */
  Standard,
  /** @brief Outer persistence throws a non-standard integer exception. */
  NonStandard,
};

/**
 * @brief Deterministic failed-admission Host and runner-finalization port.
 *
 * The first Host call succeeds with an immediately ready settlement so the
 * collector can retain a prior visible Value. The second call returns a raw
 * Graph failure without a future. Port operations record the exact shared
 * finalizer sequence and whether outer persistence observed a flushed row.
 *
 * @throws std::bad_alloc when copied snapshot, diagnostic, or stream evidence
 * cannot allocate.
 * @note The fake grants no Graph, scheduling, cancellation, or persistence
 * authority outside this regression.
 */
class FailedAdmissionWorkflowHost final
    : public I1Host,
      public I1FailedAdmissionFinalizationPort {
 public:
  /**
   * @brief Binds deterministic closed evidence and flush observers.
   * @param closed_snapshot Snapshot returned only after Graph close.
   * @param final_sample Deterministic post-cut monotonic sample.
   * @param lifecycle_cursor Expected baseline lifecycle cursor.
   * @param episode_output Inner-row stream inspected by outer persistence.
   * @param completed_rows Shared exact-slot row sequence.
   * @param written_rows Shared durable inner-row cursor.
   * @param failure_mode Optional deterministic outer-persistence exception.
   * @throws std::bad_alloc when snapshot ownership cannot allocate.
   * @note Every borrowed object outlives the finalizer call.
   */
  FailedAdmissionWorkflowHost(
      I1ExecutionSnapshot closed_snapshot,
      std::chrono::steady_clock::time_point final_sample,
      std::uint64_t lifecycle_cursor, const std::ostream& episode_output,
      const std::vector<I1EpisodeInnerRow>& completed_rows,
      const std::size_t& written_rows,
      I1OuterPersistenceFailureMode failure_mode =
          I1OuterPersistenceFailureMode::None)
      : closed_snapshot_(std::move(closed_snapshot)),
        final_sample_(final_sample),
        lifecycle_cursor_(lifecycle_cursor),
        episode_output_(episode_output),
        completed_rows_(completed_rows),
        written_rows_(written_rows),
        failure_mode_(failure_mode) {}

  /** @copydoc I1Host::compute_i1_async */
  Result<std::future<OperationStatus>> compute_i1_async(
      I1HostComputeRequest request) override {
    ++host_call_count;
    Result<std::future<OperationStatus>> result;
    if (host_call_count == 1U) {
      result.status = success_status();
      std::promise<OperationStatus> promise;
      result.value = promise.get_future();
      promise.set_value(success_status());
      return result;
    }

    failed_call_observer_present = request.observation_sink != nullptr;
    failed_proposed_coordinate = request.accepted_coordinate;
    result.status = OperationStatus{
        false, OperationErrorDomain::Graph,
        static_cast<std::int32_t>(GraphErrc::ComputeError),
        graph_error_stable_name(GraphErrc::ComputeError),
        "injected Host admission failure after visible publication"};
    return result;
  }

  /** @copydoc I1Host::i1_execution_snapshot */
  I1ExecutionSnapshot i1_execution_snapshot(std::uint64_t after_cursor,
                                            std::size_t limit) const override {
    ++snapshot_call_count;
    snapshot_saw_closed_graph = graph_closed;
    snapshot_after_cursor = after_cursor;
    snapshot_limit = limit;
    return closed_snapshot_;
  }

  /** @copydoc I1FailedAdmissionFinalizationPort::close_graph */
  OperationStatus close_graph() override {
    ++close_call_count;
    graph_closed = true;
    return success_status();
  }

  /**
   * @brief Captures the deterministic execution state after fake Graph close.
   * @return Owned closed lifecycle/resource snapshot.
   * @throws std::bad_alloc when snapshot copying cannot allocate.
   * @note The call uses the same cursor/page limit as the manual runner.
   */
  I1ExecutionSnapshot capture_closed_execution_snapshot() override {
    return i1_execution_snapshot(lifecycle_cursor_, 4096U);
  }

  /** @copydoc I1FailedAdmissionFinalizationPort::monotonic_now */
  std::chrono::steady_clock::time_point monotonic_now() override {
    ++monotonic_call_count;
    return final_sample_;
  }

  /**
   * @brief Records one outer attempt and injects the configured outcome.
   * @param diagnostic Complete finalizer diagnostic retained by the fixture.
   * @return Nothing when `failure_mode_` is `None`.
   * @throws std::runtime_error in `Standard` mode.
   * @throws The integer `93` in `NonStandard` mode.
   * @throws std::bad_alloc when diagnostic ownership cannot allocate.
   * @note The call records whether the inner row was already durable before
   * injecting either exception.
   */
  void persist_outer_failure(std::string_view diagnostic) override {
    ++outer_persistence_call_count;
    persisted_diagnostic = std::string(diagnostic);
    outer_persistence_saw_flushed_inner_row = written_rows_ == 1U &&
                                              completed_rows_.size() == 1U &&
                                              episode_output_.good();
    if (failure_mode_ == I1OuterPersistenceFailureMode::Standard) {
      throw std::runtime_error("injected outer persistence failure");
    }
    if (failure_mode_ == I1OuterPersistenceFailureMode::NonStandard) {
      throw 93;
    }
  }

  /** @copydoc I1FailedAdmissionFinalizationPort::observe_stage */
  void observe_stage(
      I1FailedAdmissionFinalizationStage stage) noexcept override {
    if (stage_count < stages.size()) {
      stages[stage_count++] = stage;
    } else {
      stage_overflow = true;
    }
  }

  /** @brief Number of real fake-Host admission calls. */
  std::size_t host_call_count = 0U;
  /** @brief Whether the failed call retained a non-null observer. */
  bool failed_call_observer_present = false;
  /** @brief Proposed coordinate passed to the failed Host call. */
  std::optional<I1AcceptedCoordinate> failed_proposed_coordinate;
  /** @brief Number of exact-once Graph-close calls. */
  std::size_t close_call_count = 0U;
  /** @brief Whether the fake Graph is synchronously closed. */
  bool graph_closed = false;
  /** @brief Number of closed execution snapshot calls. */
  mutable std::size_t snapshot_call_count = 0U;
  /** @brief Whether snapshot capture observed the closed Graph. */
  mutable bool snapshot_saw_closed_graph = false;
  /** @brief Cursor supplied to the closed lifecycle snapshot. */
  mutable std::uint64_t snapshot_after_cursor = 0U;
  /** @brief Page bound supplied to the closed lifecycle snapshot. */
  mutable std::size_t snapshot_limit = 0U;
  /** @brief Number of final monotonic samples. */
  std::size_t monotonic_call_count = 0U;
  /** @brief Number of additive outer failure persistence calls. */
  std::size_t outer_persistence_call_count = 0U;
  /** @brief Diagnostic presented to additive outer persistence. */
  std::string persisted_diagnostic;
  /** @brief Whether outer persistence saw one already-flushed inner row. */
  bool outer_persistence_saw_flushed_inner_row = false;
  /** @brief Exact shared-finalizer stage trace. */
  std::array<I1FailedAdmissionFinalizationStage, 6U> stages{};
  /** @brief Number of retained stage observations. */
  std::size_t stage_count = 0U;
  /** @brief Whether the finalizer emitted more than its closed vocabulary. */
  bool stage_overflow = false;

 private:
  /** @brief Owned deterministic closed execution snapshot. */
  I1ExecutionSnapshot closed_snapshot_;
  /** @brief Deterministic monotonic sample paired with that snapshot. */
  std::chrono::steady_clock::time_point final_sample_;
  /** @brief Baseline lifecycle cursor preceding the failed episode. */
  std::uint64_t lifecycle_cursor_ = 0U;
  /** @brief Borrowed ordered inner-row stream. */
  const std::ostream& episode_output_;
  /** @brief Borrowed exact-slot row sequence. */
  const std::vector<I1EpisodeInnerRow>& completed_rows_;
  /** @brief Borrowed durable inner-row cursor. */
  const std::size_t& written_rows_;
  /** @brief Deterministic outer-persistence exception selection. */
  I1OuterPersistenceFailureMode failure_mode_ =
      I1OuterPersistenceFailureMode::None;
};

/**
 * @brief Creates one minimal ordinary request for shared-flow verification.
 * @param edit_index Frozen edit whose Region is attached.
 * @return Request accepted by the deterministic private Host boundary.
 * @throws std::out_of_range for an invalid edit index.
 */
HostComputeRequest make_failed_admission_request(std::size_t edit_index) {
  HostComputeRequest request;
  request.session = GraphSessionId{"i1-failed-finalization-test"};
  request.node = NodeId{kI1TargetNodeId};
  request.dirty_roi = i1_edit_region(edit_index);
  return request;
}

/**
 * @brief Creates fixed-width evidence with one attempted failed admission.
 * @param episode_origin Exact episode origin used for nominal times.
 * @return Twelve records whose first entry retains raw Host failure facts and
 * whose suffix entries remain explicitly unattempted.
 * @throws Checked-time or status-allocation failures unchanged.
 */
std::array<I1EditAdmissionResult, kI1EditCount>
make_failed_finalization_admissions(
    std::chrono::steady_clock::time_point episode_origin) {
  std::array<I1EditAdmissionResult, kI1EditCount> admissions;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    admissions[edit_index].edit_index = edit_index;
    admissions[edit_index].nominal_time = checked_i1_time_add(
        episode_origin,
        std::chrono::nanoseconds(kI1EditStride.count() *
                                 static_cast<std::int64_t>(edit_index)));
  }

  I1EditAdmissionResult& failed = admissions.front();
  failed.admission_attempted = true;
  failed.admission_sample = episode_origin;
  failed.admission_window_valid = true;
  failed.reserved_event_sequence = 1U;
  failed.deadline = checked_i1_time_add(episode_origin, kI1DeadlineBudget);
  failed.host_return = I1HostReturnEvidence{
      checked_i1_time_add(episode_origin, 100us),
      OperationStatus{false, OperationErrorDomain::Graph,
                      static_cast<std::int32_t>(GraphErrc::ComputeError),
                      graph_error_stable_name(GraphErrc::ComputeError),
                      "injected failed-finalization admission"},
      false};
  return admissions;
}

/**
 * @brief Derives one deterministic post-close snapshot for finalizer tests.
 * @param input Complete episode input supplying baseline coordinates.
 * @return Snapshot with no live resources or Graph lifecycle ownership.
 * @throws Snapshot copy allocation failures unchanged.
 */
I1ExecutionSnapshot make_failed_finalization_closed_snapshot(
    const I1EpisodeEvidenceInput& input) {
  I1ExecutionSnapshot snapshot = input.final_snapshot;
  snapshot.host_resources.reserved = ResourceVector{};
  snapshot.lifecycle.counters = compute::ExecutionLifecycleCounters{};
  snapshot.lifecycle.snapshot_cut = input.baseline.lifecycle.snapshot_cut + 3U;
  snapshot.lifecycle.next_cursor = snapshot.lifecycle.snapshot_cut;
  return snapshot;
}

/**
 * @brief Proves runner and test share the complete failed-admission terminator.
 * @throws Allocation, fake Host, ComputeRun, evaluation, JSON, and stream
 * failures unchanged to GoogleTest.
 * @note A real `I1AcceptedBoundaryCollector` call receives the fake Host's raw
 * failure. The shared runner symbol must then close, cut, release without
 * hashing, snapshot, emit four Invalid verdicts, flush NDJSON, and only then
 * begin outer failure persistence. Reordering or restoring Freeze breaks the
 * stage, flush, or missing-digest assertions.
 */
TEST(I1Evidence, FailedAdmissionUsesSharedRunnerFinalizationFlow) {
  std::ostringstream output;
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  std::size_t written = 0U;

  I1EpisodeEvidenceInput input = make_valid_input(0U);
  const auto wall_sample = std::chrono::steady_clock::now();
  input.grid_origin =
      checked_i1_time_subtract(wall_sample, kI1MeasurementEndOffset + 1ms);
  input.episode_origin = i1_episode_origin(input.grid_origin, input.slot);
  input.terminal_boundary = i1_terminal_boundary(input.grid_origin);
  input.measurement_start =
      checked_i1_time_add(input.episode_origin, kI1MeasurementStartOffset);
  input.measurement_end =
      checked_i1_time_add(input.episode_origin, kI1MeasurementEndOffset);

  I1ExecutionSnapshot closed_snapshot = input.final_snapshot;
  closed_snapshot.host_resources.reserved = ResourceVector{};
  closed_snapshot.lifecycle.counters = compute::ExecutionLifecycleCounters{};
  closed_snapshot.lifecycle.snapshot_cut =
      input.baseline.lifecycle.snapshot_cut + 3U;
  closed_snapshot.lifecycle.next_cursor =
      closed_snapshot.lifecycle.snapshot_cut;
  const auto final_sample = checked_i1_time_add(input.measurement_end, 2ms);
  FailedAdmissionWorkflowHost host(std::move(closed_snapshot), final_sample,
                                   input.baseline.lifecycle.snapshot_cut,
                                   output, rows, written);

  I1EpisodeObservationCollector observations;
  std::shared_ptr<compute::ComputeRunObservationSink> first_sink =
      observations.make_edit_sink(0U);
  std::shared_ptr<compute::ComputeRunObservationSink> failed_sink =
      observations.make_edit_sink(1U);
  const auto first_admission = input.episode_origin;
  const auto failed_admission =
      checked_i1_time_add(input.episode_origin, kI1EditStride);
  std::vector<std::chrono::steady_clock::time_point> samples{
      first_admission, checked_i1_time_add(first_admission, 100us),
      failed_admission, checked_i1_time_add(failed_admission, 100us)};
  std::size_t sample_index = 0U;
  I1AcceptedBoundaryCollector admissions(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [](auto) {}, 31U);

  I1EditAdmissionResult accepted = admissions.admit_edit(
      input.episode_origin, 0U, make_failed_admission_request(0U), first_sink);
  ASSERT_TRUE(accepted.accepted_coordinate.has_value());
  ASSERT_TRUE(accepted.settlement.valid());

  compute::ComputeRunSubmission submission{
      "i1-failed-admission-shared-flow",
      GraphInstanceId{8101U},
      GraphRevision{8101U},
      kI1TargetNodeId,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             accepted.deadline, 1U, 8U},
      compute::SupersessionIdentity{
          compute::SupersessionKey(kI1TargetNodeId,
                                   ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1U), accepted.accepted_coordinate},
      first_sink};
  compute::ComputeRun run(std::move(submission));
  compute::ComputeRunLease lease = run.acquire_lease();
  first_sink->on_current_generation(lease.descriptor().supersession(),
                                    first_sink->reserve_causal_coordinate());
  first_sink->on_current_visible(lease.descriptor(), make_test_output(),
                                 first_sink->reserve_causal_coordinate());

  I1EditAdmissionResult failed = admissions.admit_edit(
      input.episode_origin, 1U, make_failed_admission_request(1U), failed_sink);
  I1OuterPersistenceOwnershipGate ownership_gate;
  ASSERT_TRUE(claim_i1_failed_admission_if_needed(failed, ownership_gate));
  ASSERT_TRUE(failed.admission_attempted);
  ASSERT_TRUE(failed.admission_window_valid);
  ASSERT_TRUE(failed.host_return.has_value());
  EXPECT_FALSE(failed.host_return->status.ok);
  EXPECT_FALSE(failed.host_return->future_valid);
  EXPECT_FALSE(failed.accepted_coordinate.has_value());
  EXPECT_FALSE(failed.settlement.valid());

  std::array<I1EditAdmissionResult, kI1EditCount> results;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    results[edit_index].edit_index = edit_index;
    results[edit_index].nominal_time = checked_i1_time_add(
        input.episode_origin,
        std::chrono::nanoseconds(kI1EditStride.count() *
                                 static_cast<std::int64_t>(edit_index)));
  }
  results[0U] = std::move(accepted);
  results[1U] = std::move(failed);

  bool saw_terminal_error = false;
  std::string terminal_diagnostic;
  try {
    finalize_i1_failed_admission(
        "Host admission failed at edit 1", std::move(input), std::move(results),
        ownership_gate, &observations, &host, &output, &rows, &written);
  } catch (const I1FailedAdmissionFinalizationError& error) {
    saw_terminal_error = true;
    terminal_diagnostic = error.what();
  }
  EXPECT_TRUE(saw_terminal_error);
  EXPECT_NE(terminal_diagnostic.find("Host admission failed at edit 1"),
            std::string::npos);

  EXPECT_EQ(host.host_call_count, 2U);
  EXPECT_TRUE(host.failed_call_observer_present);
  ASSERT_TRUE(host.failed_proposed_coordinate.has_value());
  EXPECT_EQ(host.failed_proposed_coordinate->event_sequence(), 32U);
  EXPECT_EQ(host.close_call_count, 1U);
  EXPECT_TRUE(host.graph_closed);
  EXPECT_EQ(host.snapshot_call_count, 1U);
  EXPECT_TRUE(host.snapshot_saw_closed_graph);
  EXPECT_EQ(host.snapshot_after_cursor, 10U);
  EXPECT_EQ(host.snapshot_limit, 4096U);
  EXPECT_EQ(host.monotonic_call_count, 1U);
  EXPECT_EQ(host.outer_persistence_call_count, 1U);
  EXPECT_TRUE(host.outer_persistence_saw_flushed_inner_row);
  EXPECT_NE(host.persisted_diagnostic.find("Host admission failed at edit 1"),
            std::string::npos);

  const std::array<I1FailedAdmissionFinalizationStage, 6U> expected_stages{
      I1FailedAdmissionFinalizationStage::GraphCloseCompleted,
      I1FailedAdmissionFinalizationStage::HistoryCutCaptured,
      I1FailedAdmissionFinalizationStage::UnfrozenOutputsReleased,
      I1FailedAdmissionFinalizationStage::ClosedExecutionSnapshotCaptured,
      I1FailedAdmissionFinalizationStage::InnerRowFlushed,
      I1FailedAdmissionFinalizationStage::OuterFailurePersistenceStarted};
  EXPECT_FALSE(host.stage_overflow);
  EXPECT_EQ(host.stage_count, expected_stages.size());
  EXPECT_EQ(host.stages, expected_stages);

  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(written, 1U);
  const I1EpisodeInnerRow& row = rows.front();
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
  ASSERT_TRUE(row.evidence.edits[0U].settlement_status.has_value());
  EXPECT_TRUE(row.evidence.edits[0U].settlement_status->ok);
  ASSERT_TRUE(row.evidence.edits[1U].host_return.has_value());
  EXPECT_FALSE(row.evidence.edits[1U].host_return->status.ok);
  EXPECT_FALSE(row.evidence.edits[1U].accepted_coordinate.has_value());
  EXPECT_FALSE(row.accepted_products[1U].has_value());
  EXPECT_FALSE(row.evidence.edits[2U].admission_attempted);
  ASSERT_EQ(row.evidence.observations.visible_outputs.size(), 1U);
  const I1ObservedVisibleOutput& visible =
      row.evidence.observations.visible_outputs.front();
  EXPECT_GT(row.evidence.observation_cut.causal_sequence,
            visible.causal_sequence);
  EXPECT_TRUE(visible.value_valid_at_capture);
  EXPECT_FALSE(visible.output.valid());
  EXPECT_FALSE(visible.content_digest.has_value());
  EXPECT_EQ(row.evidence.final_snapshot.lifecycle.counters.open_graph_count,
            0U);
  EXPECT_EQ(
      row.evidence.final_snapshot.lifecycle.counters.registered_graph_count,
      0U);

  const nlohmann::json encoded = nlohmann::json::parse(output.str());
  EXPECT_EQ(encoded.at("verdicts").at("latency").get<std::string>(), "invalid");
  EXPECT_EQ(encoded.at("verdicts").at("waste").get<std::string>(), "invalid");
  EXPECT_EQ(encoded.at("verdicts").at("memory").get<std::string>(), "invalid");
  EXPECT_EQ(encoded.at("verdicts").at("output").get<std::string>(), "invalid");
  EXPECT_TRUE(encoded.at("accepted_products").at(1U).is_null());
  EXPECT_EQ(encoded.at("observations")
                .at("visible_outputs")
                .front()
                .at("content_digest")
                .at("state")
                .get<std::string>(),
            "invalid-descriptor");
}

/**
 * @brief Observable result of one outer-persistence exception exercise.
 * @throws Nothing for default construction and scalar movement.
 */
struct I1OuterPersistenceExerciseResult final {
  /** @brief Whether the no-throw failed-admission claim remained visible. */
  bool ownership_claimed = false;
  /** @brief Whether the shared terminal exception crossed runner drain. */
  bool terminal_failure_rethrown = false;
  /** @brief Number of shared-finalizer outer-port calls. */
  std::size_t finalizer_outer_calls = 0U;
  /** @brief Number of main-like generic outer-writer calls. */
  std::size_t generic_outer_calls = 0U;
  /** @brief Durable inner-row cursor after finalization. */
  std::size_t written_rows = 0U;
  /** @brief Whether the finalizer port observed durable inner evidence. */
  bool outer_saw_flushed_inner_row = false;
  /** @brief Terminal diagnostic retained after the injected port failure. */
  std::string terminal_diagnostic;
};

/**
 * @brief Exercises one throwing outer port through shared finalizer/runner
 * ownership.
 * @param failure_mode Standard or non-standard deterministic exception mode.
 * @return Counts and terminal facts after the main-like generic writer gate.
 * @throws Setup/evaluation failures unrelated to the injected outer exception.
 * @note The helper invokes the same classifier, finalizer, generic-drain, and
 * generic-outer functions as the manual runner; it does not reproduce their
 * branching policy.
 */
I1OuterPersistenceExerciseResult exercise_outer_persistence_failure(
    I1OuterPersistenceFailureMode failure_mode) {
  std::ostringstream output;
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  std::size_t written = 0U;
  std::optional<std::future<I1EpisodeInnerRow>> pending_evaluation;

  I1EpisodeEvidenceInput input = make_valid_input(0U);
  const auto final_sample = checked_i1_time_add(input.measurement_end, 2ms);
  I1ExecutionSnapshot closed_snapshot =
      make_failed_finalization_closed_snapshot(input);
  FailedAdmissionWorkflowHost host(std::move(closed_snapshot), final_sample,
                                   input.baseline.lifecycle.snapshot_cut,
                                   output, rows, written, failure_mode);
  I1EpisodeObservationCollector observations;
  auto admissions = make_failed_finalization_admissions(input.episode_origin);
  I1OuterPersistenceOwnershipGate ownership_gate;
  if (!claim_i1_failed_admission_if_needed(admissions.front(),
                                           ownership_gate)) {
    throw std::logic_error("failed-admission exercise was not classified");
  }

  std::exception_ptr primary_failure;
  try {
    finalize_i1_failed_admission(
        "injected failed admission", std::move(input), std::move(admissions),
        ownership_gate, &observations, &host, &output, &rows, &written);
  } catch (...) {
    primary_failure = std::current_exception();
  }
  if (primary_failure == nullptr) {
    throw std::logic_error("failed-admission finalizer returned unexpectedly");
  }

  I1OuterPersistenceExerciseResult result;
  result.ownership_claimed =
      ownership_gate.failed_admission_finalizer_owns_persistence();
  result.finalizer_outer_calls = host.outer_persistence_call_count;
  result.written_rows = written;
  result.outer_saw_flushed_inner_row =
      host.outer_persistence_saw_flushed_inner_row;
  try {
    rethrow_i1_runner_failure_after_generic_drain(
        primary_failure, ownership_gate, &pending_evaluation, &rows, &output,
        &written);
  } catch (const std::exception& error) {
    result.terminal_failure_rethrown = true;
    result.terminal_diagnostic = error.what();
  } catch (...) {
    result.terminal_failure_rethrown = true;
    result.terminal_diagnostic = "non-standard terminal failure";
  }
  try_i1_generic_outer_failure_persistence(
      ownership_gate, [&result] { ++result.generic_outer_calls; });
  return result;
}

/**
 * @brief Proves failed inner flush cannot trigger generic reflush or outer I/O.
 * @throws Setup/evaluation failures unchanged to GoogleTest.
 * @note Clearing only `std::ostream` iostate after the first rejection makes a
 * prohibited generic retry observable; the same stream buffer remains a
 * rejecting sink for the entire test.
 */
TEST(I1Evidence,
     FailedAdmissionRejectedInnerStreamSuppressesGenericDrainAndOuterWrite) {
  RejectingI1EpisodeStreamBuffer buffer;
  std::ostream output(&buffer);
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  std::size_t written = 0U;
  std::optional<std::future<I1EpisodeInnerRow>> pending_evaluation;

  I1EpisodeEvidenceInput input = make_valid_input(0U);
  const auto final_sample = checked_i1_time_add(input.measurement_end, 2ms);
  I1ExecutionSnapshot closed_snapshot =
      make_failed_finalization_closed_snapshot(input);
  FailedAdmissionWorkflowHost host(std::move(closed_snapshot), final_sample,
                                   input.baseline.lifecycle.snapshot_cut,
                                   output, rows, written);
  I1EpisodeObservationCollector observations;
  auto admissions = make_failed_finalization_admissions(input.episode_origin);
  I1OuterPersistenceOwnershipGate ownership_gate;
  ASSERT_TRUE(
      claim_i1_failed_admission_if_needed(admissions.front(), ownership_gate));

  std::exception_ptr primary_failure;
  try {
    finalize_i1_failed_admission(
        "injected failed admission", std::move(input), std::move(admissions),
        ownership_gate, &observations, &host, &output, &rows, &written);
  } catch (...) {
    primary_failure = std::current_exception();
  }
  ASSERT_NE(primary_failure, nullptr);
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(written, 0U);
  EXPECT_FALSE(output.good());
  EXPECT_EQ(host.outer_persistence_call_count, 0U);
  const std::size_t finalizer_stream_attempts = buffer.attempt_count();
  EXPECT_GT(finalizer_stream_attempts, 0U);

  output.clear();
  bool runner_rethrew = false;
  try {
    rethrow_i1_runner_failure_after_generic_drain(
        primary_failure, ownership_gate, &pending_evaluation, &rows, &output,
        &written);
  } catch (...) {
    runner_rethrew = true;
  }
  EXPECT_TRUE(runner_rethrew);
  EXPECT_EQ(buffer.attempt_count(), finalizer_stream_attempts);
  EXPECT_EQ(written, 0U);

  std::size_t main_outer_calls = 0U;
  try_i1_generic_outer_failure_persistence(
      ownership_gate, [&main_outer_calls] { ++main_outer_calls; });
  EXPECT_EQ(main_outer_calls, 0U);
  EXPECT_EQ(host.outer_persistence_call_count, 0U);
}

/**
 * @brief Proves a standard outer exception is attempted once without retry.
 * @throws Setup/evaluation failures unchanged to GoogleTest.
 */
TEST(I1Evidence, FailedAdmissionStandardOuterExceptionIsNotRetriedByMain) {
  const I1OuterPersistenceExerciseResult result =
      exercise_outer_persistence_failure(
          I1OuterPersistenceFailureMode::Standard);
  EXPECT_TRUE(result.ownership_claimed);
  EXPECT_TRUE(result.terminal_failure_rethrown);
  EXPECT_EQ(result.written_rows, 1U);
  EXPECT_TRUE(result.outer_saw_flushed_inner_row);
  EXPECT_EQ(result.finalizer_outer_calls, 1U);
  EXPECT_EQ(result.generic_outer_calls, 0U);
  EXPECT_NE(result.terminal_diagnostic.find(
                "failure artifact persistence failed: injected outer "
                "persistence failure"),
            std::string::npos);
}

/**
 * @brief Proves a non-standard outer exception is attempted once without
 * retry.
 * @throws Setup/evaluation failures unchanged to GoogleTest.
 */
TEST(I1Evidence, FailedAdmissionNonStandardOuterExceptionIsNotRetriedByMain) {
  const I1OuterPersistenceExerciseResult result =
      exercise_outer_persistence_failure(
          I1OuterPersistenceFailureMode::NonStandard);
  EXPECT_TRUE(result.ownership_claimed);
  EXPECT_TRUE(result.terminal_failure_rethrown);
  EXPECT_EQ(result.written_rows, 1U);
  EXPECT_TRUE(result.outer_saw_flushed_inner_row);
  EXPECT_EQ(result.finalizer_outer_calls, 1U);
  EXPECT_EQ(result.generic_outer_calls, 0U);
  EXPECT_NE(result.terminal_diagnostic.find(
                "failure artifact persistence raised a non-standard "
                "exception"),
            std::string::npos);
}

/**
 * @brief Proves diagnostic allocation failure cannot restore generic outer
 * ownership.
 * @throws Nothing when the injected `std::bad_alloc` is caught as intended.
 * @note The classifier is the exact no-throw operation used immediately after
 * runner admission detection; the lambda models the first fallible diagnostic
 * construction that follows it.
 */
TEST(I1Evidence, FailedAdmissionClaimPrecedesDiagnosticBadAlloc) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  auto admissions = make_failed_finalization_admissions(input.episode_origin);
  I1OuterPersistenceOwnershipGate ownership_gate;
  const auto build_diagnostic = []() -> std::string { throw std::bad_alloc(); };

  bool allocation_failed = false;
  try {
    ASSERT_TRUE(claim_i1_failed_admission_if_needed(admissions.front(),
                                                    ownership_gate));
    const std::string diagnostic = build_diagnostic();
    static_cast<void>(diagnostic);
  } catch (const std::bad_alloc&) {
    allocation_failed = true;
  }
  EXPECT_TRUE(allocation_failed);
  EXPECT_TRUE(ownership_gate.failed_admission_finalizer_owns_persistence());
  EXPECT_FALSE(ownership_gate.generic_inner_drain_is_permitted());

  std::size_t generic_outer_calls = 0U;
  try_i1_generic_outer_failure_persistence(
      ownership_gate, [&generic_outer_calls] { ++generic_outer_calls; });
  EXPECT_EQ(generic_outer_calls, 0U);
}

/**
 * @brief Proves optional unsigned evidence serializes as a value or null.
 * @throws Nothing when evaluation and JSON inspection remain deterministic.
 * @note Absent cases are applied after evaluation so this test isolates the
 * encoder shape from I1 evaluator validity and QoS requirements.
 */
TEST(I1Evidence, OptionalUnsignedEvidenceSerializesAsValueOrNull) {
  I1EpisodeInnerRow row = evaluate_i1_episode(make_valid_input(0U));
  ASSERT_TRUE(row.validity_reasons.empty());
  ASSERT_GE(row.evidence.observations.service_starts.size(), 2U);
  row.evidence.edits[1U].reserved_event_sequence.reset();
  row.evidence.observations.service_starts[1U].qos.maximum_parallelism.reset();

  const nlohmann::json encoded = i1_inner_row_json(row);
  const nlohmann::json& present_sequence =
      encoded.at("edits").at(0U).at("reserved_event_sequence");
  const nlohmann::json& absent_sequence =
      encoded.at("edits").at(1U).at("reserved_event_sequence");
  EXPECT_TRUE(present_sequence.is_number_unsigned());
  EXPECT_EQ(present_sequence.get<std::uint64_t>(), 1U);
  EXPECT_TRUE(absent_sequence.is_null());

  const nlohmann::json& service_starts =
      encoded.at("observations").at("service_starts");
  const nlohmann::json& present_parallelism =
      service_starts.at(0U).at("maximum_parallelism");
  const nlohmann::json& absent_parallelism =
      service_starts.at(1U).at("maximum_parallelism");
  EXPECT_TRUE(present_parallelism.is_number_unsigned());
  EXPECT_EQ(present_parallelism.get<std::uint32_t>(), 8U);
  EXPECT_TRUE(absent_parallelism.is_null());
}

/**
 * @brief Proves noncommitting and post-cancel starts remain separate waste.
 * @throws Nothing when complete synthetic evidence is classified correctly.
 */
TEST(I1Evidence, CountsDiscardedAndPostCancelServiceIndependently) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  input.observations.visible_outputs.erase(
      std::remove_if(input.observations.visible_outputs.begin(),
                     input.observations.visible_outputs.end(),
                     [](const I1ObservedVisibleOutput& event) {
                       return event.edit_index == 0U;
                     }),
      input.observations.visible_outputs.end());
  I1ObservedTerminal& terminal =
      event_for_edit(&input.observations.terminals, 0U);
  terminal.kind = compute::ComputeRunTerminalKind::Cancelled;
  const std::uint64_t post_cancel_sequence = terminal.causal_sequence;
  const std::uint64_t cancellation_sequence = post_cancel_sequence - 1U;
  open_observation_sequence_gap(&input.observations, &input.observation_cut,
                                post_cancel_sequence);
  input.observations.cancellations.push_back(I1ObservedCancellation{
      0U, terminal.run_id, terminal.generation,
      compute::ComputeRunCancellationReason::Superseded,
      checked_i1_time_add(input.edits[0].admission_sample, 1500us),
      cancellation_sequence});
  input.edits[0].settlement_status->ok = false;
  const auto& original_start =
      event_for_edit(&input.observations.service_starts, 0U);
  I1ObservedServiceStart post_cancel = original_start;
  post_cancel.local_task_id = 1U;
  post_cancel.causal_sequence = post_cancel_sequence;
  post_cancel.observed_at =
      checked_i1_time_add(input.edits[0].admission_sample, 2ms);
  input.observations.service_starts.push_back(post_cancel);

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_EQ(row.service.all_started_service, 1300U);
  EXPECT_EQ(row.service.discarded_started_service, 200U);
  EXPECT_EQ(row.service.post_cancel_started_service, 100U);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Defers the discarded-ratio threshold to replicate aggregation.
 * @throws Nothing when complete episode evidence with no post-cancel start
 * remains valid; allocation failures are reported by GoogleTest.
 */
TEST(I1Evidence, EpisodeDiscardedRatioDoesNotApplyReplicateThreshold) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  input.observations.visible_outputs.erase(
      std::remove_if(input.observations.visible_outputs.begin(),
                     input.observations.visible_outputs.end(),
                     [](const I1ObservedVisibleOutput& event) {
                       return event.edit_index == 0U;
                     }),
      input.observations.visible_outputs.end());
  I1ObservedServiceStart& discarded_start =
      event_for_edit(&input.observations.service_starts, 0U);
  discarded_start.service_charge = 500U;
  I1ObservedTerminal& terminal =
      event_for_edit(&input.observations.terminals, 0U);
  terminal.kind = compute::ComputeRunTerminalKind::Cancelled;
  input.observations.cancellations.push_back(I1ObservedCancellation{
      0U, terminal.run_id, terminal.generation,
      compute::ComputeRunCancellationReason::Superseded,
      checked_i1_time_add(input.edits[0].admission_sample, 1500us),
      terminal.causal_sequence - 1U});
  input.edits[0].settlement_status->ok = false;

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_EQ(row.service.all_started_service, 1600U);
  EXPECT_EQ(row.service.discarded_started_service, 500U);
  EXPECT_EQ(row.service.post_cancel_started_service, 0U);
  ASSERT_TRUE(row.service.discarded_ratio.has_value());
  EXPECT_DOUBLE_EQ(*row.service.discarded_ratio, 0.3125);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves an in-cut start after Host settlement is structurally invalid.
 * @throws Nothing when the evaluator rejects the per-Run order violation.
 */
TEST(I1Evidence, ServiceStartAfterHostSettlementInvalidatesTheRow) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  I1ObservedServiceStart& start =
      event_for_edit(&input.observations.service_starts, kI1EditCount - 1U);
  const I1ObservedHostSettlement& host =
      event_for_edit(&input.observations.host_settlements, kI1EditCount - 1U);
  start.observed_at = checked_i1_time_add(host.observed_at, 1us);
  start.causal_sequence = input.observation_cut.causal_sequence++;
  ASSERT_LE(start.observed_at, input.measurement_end);
  ASSERT_LT(start.causal_sequence, input.observation_cut.causal_sequence);

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "service start does not precede Run terminal"),
            row.validity_reasons.end());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves an invalid frozen final digest cannot produce correctness.
 * @throws Nothing when capture failure remains typed and fail-closed.
 */
TEST(I1Evidence, InvalidFrozenFinalDigestMakesOnlyOutputEvidenceInvalid) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  I1ObservedVisibleOutput& final_visible =
      event_for_edit(&input.observations.visible_outputs, kI1EditCount - 1U);
  final_visible.content_digest =
      ContentDigestResult{ContentDigestState::InvalidDescriptor, std::nullopt,
                          "injected frozen digest failure"};

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_EQ(row.final_digest.state, ContentDigestState::InvalidDescriptor);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves an absent independent golden fails closed instead of passing.
 * @throws Nothing when only output evidence becomes Invalid.
 */
TEST(I1Evidence, MissingExpectedDigestInvalidatesOnlyOutputEvidence) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  input.expected_final_digest.reset();

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "independent expected final digest is missing"),
            row.validity_reasons.end());
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves a present but non-frozen expected digest is invalid evidence.
 * @throws Nothing when the candidate cannot redefine its own oracle.
 */
TEST(I1Evidence, NonFrozenExpectedDigestInvalidatesOutputEvidence) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  ASSERT_TRUE(input.expected_final_digest.has_value());
  input.expected_final_digest->bytes[0U] ^= std::byte{0x01};

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "independent expected final digest does not match the "
                      "frozen I1 golden"),
            row.validity_reasons.end());
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves candidate output drift fails against the unchanged golden.
 * @throws Nothing when complete digest evidence produces a negative verdict.
 */
TEST(I1Evidence, CandidateDigestMismatchFailsOnlyOutputEvidence) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  I1ObservedVisibleOutput& final_visible =
      event_for_edit(&input.observations.visible_outputs, kI1EditCount - 1U);
  ASSERT_TRUE(final_visible.content_digest.has_value());
  ASSERT_TRUE(final_visible.content_digest->digest.has_value());
  final_visible.content_digest->digest->bytes[0U] ^= std::byte{0x01};

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_EQ(row.output_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves evaluation and JSON reuse one digest after Value release.
 * @throws nlohmann/std allocation errors unchanged.
 */
TEST(I1Evidence, FrozenDigestSurvivesValueReleaseAndSerialization) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  for (const I1ObservedVisibleOutput& visible :
       input.observations.visible_outputs) {
    EXPECT_FALSE(visible.output.valid());
    EXPECT_TRUE(visible.value_valid_at_capture);
    ASSERT_TRUE(visible.content_digest.has_value());
  }

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  ASSERT_EQ(row.output_verdict, I1Verdict::Pass);
  const nlohmann::json encoded = i1_inner_row_json(row);
  const nlohmann::json& visible_outputs =
      encoded.at("observations").at("visible_outputs");
  ASSERT_EQ(visible_outputs.size(), kI1EditCount);
  EXPECT_TRUE(visible_outputs.back().at("value_valid").get<bool>());
  EXPECT_EQ(visible_outputs.back()
                .at("content_digest")
                .at("state")
                .get<std::string>(),
            "available");
}

/**
 * @brief Proves every intermediate deadline revokes visible publication.
 * @throws Nothing when expired current output invalidates the complete row.
 * @note Final latency has a separate threshold verdict; an intermediate output
 * after its own `D_i` contradicts the frozen workload/product contract.
 */
TEST(I1Evidence, ExpiredIntermediatePublicationInvalidatesTheRow) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  I1ObservedVisibleOutput& visible =
      event_for_edit(&input.observations.visible_outputs, 0U);
  ASSERT_TRUE(input.edits[0].deadline.has_value());
  visible.observed_at = checked_i1_time_add(*input.edits[0].deadline,
                                            std::chrono::nanoseconds(1));
  event_for_edit(&input.observations.terminals, 0U).observed_at =
      checked_i1_time_add(visible.observed_at, 1us);

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "an intermediate edit published after its deadline"),
            row.validity_reasons.end());
}

/**
 * @brief Proves an equal-time lifecycle return after the `Q_end` causal cut
 * cannot be hidden by a later settled snapshot.
 * @throws Nothing when the authoritative time/sequence cut rejects the row.
 */
TEST(I1Evidence, ResourceSettlementCrossingQEndInvalidatesTheRow) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  I1ObservedRunLifecycleTransition& resource = event_for_edit(
      &input.observations.resource_settlements, kI1EditCount - 1U);
  I1ObservedHostSettlement& host =
      event_for_edit(&input.observations.host_settlements, kI1EditCount - 1U);
  const std::uint64_t first_excluded = input.observation_cut.causal_sequence;
  resource.observed_at = input.measurement_end;
  resource.causal_sequence = first_excluded;
  host.observed_at = input.measurement_end;
  host.causal_sequence = first_excluded + 1U;

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "observation lies after the Q_end history cut"),
            row.validity_reasons.end());
}

/**
 * @brief Proves accepted cancellation cannot coexist with Succeeded terminal.
 * @throws Nothing when the per-Run state machine rejects the contradiction.
 */
TEST(I1Evidence, CancellationWithSucceededTerminalInvalidatesTheRow) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  const I1ObservedTerminal& terminal =
      event_for_edit(&input.observations.terminals, kI1EditCount - 1U);
  input.observations.cancellations.push_back(I1ObservedCancellation{
      kI1EditCount - 1U, terminal.run_id, terminal.generation,
      compute::ComputeRunCancellationReason::Superseded,
      checked_i1_time_add(input.edits.back().admission_sample, 5ms),
      input.observation_cut.causal_sequence++});

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "non-cancelled Run carries accepted cancellation"),
            row.validity_reasons.end());
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves a Succeeded terminal cannot causally precede visibility.
 * @throws Nothing when sequence order, not vector insertion, is authoritative.
 */
TEST(I1Evidence, TerminalBeforeVisibleInvalidatesTheRow) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  I1ObservedTerminal& terminal =
      event_for_edit(&input.observations.terminals, kI1EditCount - 1U);
  I1ObservedVisibleOutput& visible =
      event_for_edit(&input.observations.visible_outputs, kI1EditCount - 1U);
  std::swap(terminal.causal_sequence, visible.causal_sequence);

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "visible publication does not precede terminal"),
            row.validity_reasons.end());
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves Host success/failure status must agree with Run terminal kind.
 * @throws Nothing when contradictory settlement fails closed.
 */
TEST(I1Evidence, HostSettlementContradictingTerminalInvalidatesTheRow) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  ASSERT_TRUE(input.edits.back().settlement_status.has_value());
  input.edits.back().settlement_status->ok = false;

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "Host settlement contradicts Run terminal kind"),
            row.validity_reasons.end());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves complete over-limit/unsettled resource evidence is a failure.
 * @throws Nothing when memory evidence remains structurally complete.
 */
TEST(I1Evidence, ResourceNonSettlementIsMemoryFailure) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  input.final_snapshot.host_resources.reserved.cpu_slots = 1U;
  input.final_snapshot.host_resources.high_water.cpu_slots = 9U;

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_FALSE(row.memory_settled);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves exact measured latency and replicate-level Waste aggregation.
 * @throws Nothing when small deterministic Values and rows evaluate exactly.
 */
TEST(I1Evidence, AggregatesExactlyTwoHundredMeasuredRows) {
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  for (std::size_t slot = 0U; slot < kI1GridSlotCount; ++slot) {
    std::chrono::nanoseconds latency = 10ms;
    if (slot >= 21U && slot < 121U) {
      latency = 40ms;
    } else if (slot >= 121U && slot < 211U) {
      latency = 80ms;
    } else if (slot >= 211U) {
      latency = 120ms;
    }
    I1EpisodeInnerRow row =
        evaluate_i1_episode(make_valid_input(slot, latency));
    if (classify_i1_slot(slot).first == I1EpisodePhase::Measured) {
      row.service.all_started_service = 100U;
      row.service.discarded_started_service =
          slot == kI1WarmupSlotCount + 1U ? 26U : 0U;
      row.service.post_cancel_started_service = 0U;
      row.service.discarded_ratio =
          static_cast<double>(row.service.discarded_started_service) / 100.0;
    }
    rows.push_back(std::move(row));
  }

  const I1ReplicateSummary summary = evaluate_i1_replicate(rows);
  ASSERT_TRUE(summary.latency.has_value());
  EXPECT_EQ(summary.measured_sample_count, kI1MeasuredSlotCount);
  EXPECT_EQ(summary.latency->p50, 40ms);
  EXPECT_EQ(summary.latency->p95, 80ms);
  EXPECT_EQ(summary.latency->p99, 120ms);
  EXPECT_EQ(summary.measured_service.all_started_service, 20000U);
  EXPECT_EQ(summary.measured_service.discarded_started_service, 26U);
  EXPECT_EQ(summary.measured_service.post_cancel_started_service, 0U);
  ASSERT_TRUE(summary.measured_service.discarded_ratio.has_value());
  EXPECT_DOUBLE_EQ(*summary.measured_service.discarded_ratio, 0.0013);
  EXPECT_EQ(summary.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves the real I1 evaluator output produces a loadable native pair.
 * @throws Test fixture, evaluator, canonical pack, and framework failures.
 */
TEST(I1Evidence, ProducesCanonicalPairObjectFromCompleteReplicate) {
  std::vector<I1EpisodeInnerRow> rows;
  rows.reserve(kI1GridSlotCount);
  for (std::size_t slot = 0U; slot < kI1GridSlotCount; ++slot) {
    rows.push_back(evaluate_i1_episode(make_valid_input(slot, 10ms)));
  }

  const EvidencePairObject produced = make_i1_evidence_pair_object(
      rows, make_i1_pair_environment(),
      EvidencePairProducerOptions{EvidenceSubjectRole::Reference,
                                  std::nullopt});
  const std::string pack = materialize_evidence_pair_object(produced);
  const EvidencePairObject loaded = load_evidence_pair_object(
      pack, produced.row.digest, produced.bundle.digest);
  EXPECT_EQ(loaded.row.digest, produced.row.digest);
  EXPECT_EQ(loaded.bundle.digest, produced.bundle.digest);
  const B1CanonicalManifest measurement =
      parse_b1_canonical_manifest(loaded.row.source.measurement_evidence.bytes);
  ASSERT_EQ(measurement.fields.size(), 4U);
  EXPECT_EQ(parse_b1_framed_list(measurement.fields[2U].payload).size(),
            kI1MeasuredSlotCount);
  EXPECT_EQ(
      parse_b1_canonical_uint64(measurement.fields[3U].payload),
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(10ms).count()));
}

}  // namespace
}  // namespace ps::benchmark
