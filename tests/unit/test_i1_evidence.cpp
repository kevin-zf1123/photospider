#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "benchmark/i1_evidence.hpp"
#include "photospider/data/value.hpp"

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
  const ContentDigestResult output_digest = compute_content_digest(output);
  if (output_digest.digest.has_value()) {
    input.expected_final_digest = output_digest.digest;
  }

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
        I1ObservedCurrentGeneration{edit_index, generation, admission,
                                    causal_sequence++});
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
        edit_index, run_id, generation, visible_at, causal_sequence++, output});
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
 * @brief Proves an invalid final Value cannot produce output correctness.
 * @throws Nothing when digest failure remains typed and fail-closed.
 */
TEST(I1Evidence, InvalidFinalValueMakesOnlyOutputEvidenceInvalid) {
  I1EpisodeEvidenceInput input = make_valid_input(0U);
  event_for_edit(&input.observations.visible_outputs, kI1EditCount - 1U)
      .output = Value{};

  const I1EpisodeInnerRow row = evaluate_i1_episode(std::move(input));
  EXPECT_EQ(row.final_digest.state, ContentDigestState::InvalidDescriptor);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
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
 * @brief Proves the exact 221-slot aggregate excludes warmup and uses p-ranks.
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
    rows.push_back(evaluate_i1_episode(make_valid_input(slot, latency)));
  }

  const I1ReplicateSummary summary = evaluate_i1_replicate(rows);
  ASSERT_TRUE(summary.latency.has_value());
  EXPECT_EQ(summary.measured_sample_count, kI1MeasuredSlotCount);
  EXPECT_EQ(summary.latency->p50, 40ms);
  EXPECT_EQ(summary.latency->p95, 80ms);
  EXPECT_EQ(summary.latency->p99, 120ms);
  EXPECT_EQ(summary.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Pass);
}

}  // namespace
}  // namespace ps::benchmark
