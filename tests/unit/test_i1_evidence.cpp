#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/i1_evidence.hpp"
#include "photospider/data/value.hpp"
#include "verification/i1_evidence_json.hpp"

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
