#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/i2_evidence.hpp"
#include "photospider/data/value.hpp"
#include "verification/i2_evidence_json.hpp"
#include "verification/i2_evidence_workflow.hpp"

namespace ps::benchmark {
namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;
using std::chrono_literals::operator""us;

/**
 * @brief Creates one tiny Ready CPU Value for closed binding evidence.
 * @return Immutable one-byte DenseTensor with stable access facts.
 * @throws Value validation, allocation, and publication failures unchanged.
 */
Value make_i2_evidence_value() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {1U, 1U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                      StridedLayout{{1, 1}}, {std::byte{0x2a}});
}

/**
 * @brief Captures the exact direct Host facts of one Ready CPU Value.
 * @param value Valid host-visible Value retained by the caller.
 * @return Authority-free Direct access evidence with no executor submission.
 * @throws Value access-classification failures unchanged.
 */
I2ValueAccessEvidence direct_i2_access(const Value& value) {
  const StorageBinding binding = value.storage_binding();
  return I2ValueAccessEvidence{
      value.plan_access(AccessTarget{DeviceId(DeviceBackend::CPU),
                                     MemoryDomain::Host, true, false}),
      value.revision_id(),
      binding,
      binding.allocation,
      value.storage_size(),
      false};
}

/**
 * @brief Creates complete no-Metal acquisition evidence for one CPU Value.
 * @param value Exact visible publication whose binding is captured twice.
 * @return Closed Host-direct and deterministic Metal-N/A evidence.
 * @throws Value access-classification failures unchanged.
 */
I2ValueAcquisitionEvidence make_i2_acquisition(const Value& value) {
  I2ValueAcquisitionEvidence result;
  result.host_first = direct_i2_access(value);
  result.host_second = direct_i2_access(value);
  result.metal.available = false;
  result.metal.unavailable_reason =
      "not-applicable: process Metal executor unavailable";
  return result;
}

/**
 * @brief Creates complete synthetic one-transfer/one-reuse Metal evidence.
 * @param host Exact first Host access whose revision and byte envelope move.
 * @return Closed available-Metal evidence with distinct device allocation.
 * @throws Value allocation or optional-plan construction failures unchanged.
 * @note A second tiny CPU Value mints a distinct allocation identity; only its
 * authority-free identity is retained while the synthetic binding is relabeled
 * as device-local Metal storage.
 */
I2MetalAcquisitionEvidence make_i2_metal_acquisition(
    const I2ValueAccessEvidence& host) {
  const Value allocation_source = make_i2_evidence_value();
  StorageBinding metal_binding = allocation_source.storage_binding();
  metal_binding.device = DeviceId(DeviceBackend::Metal);
  metal_binding.memory_domain = MemoryDomain::DeviceLocal;
  metal_binding.byte_size = host.storage_bytes;
  metal_binding.host_visible = false;

  const AccessTarget transfer_target{DeviceId(DeviceBackend::Metal),
                                     MemoryDomain::DeviceLocal, false, true};
  const AccessTarget reuse_target{DeviceId(DeviceBackend::Metal),
                                  MemoryDomain::DeviceLocal, false, false};
  I2MetalAcquisitionEvidence result;
  result.available = true;
  result.first = I2ValueAccessEvidence{
      AccessPlan{AccessPlanKind::Transfer, host.revision.value(), host.binding,
                 transfer_target, VisibilityObligations{}, host.storage_bytes},
      host.revision,
      metal_binding,
      metal_binding.allocation,
      host.storage_bytes,
      true};
  result.second = I2ValueAccessEvidence{
      AccessPlan{AccessPlanKind::Direct, host.revision.value(), metal_binding,
                 reuse_target, VisibilityObligations{}, 0U},
      host.revision,
      metal_binding,
      metal_binding.allocation,
      host.storage_bytes,
      false};

  execution::DeviceExecutorDiagnostics before;
  before.device = Device::GPU_METAL;
  before.queue_ready = true;
  execution::DeviceExecutorDiagnostics after_first = before;
  after_first.submission_count = 1U;
  after_first.invocation_count = 1U;
  after_first.total_allocations = 2U;
  result.before = before;
  result.after_first = after_first;
  result.after_second = after_first;

  ResourceLedger::DeviceSnapshot resources_before;
  resources_before.device = DeviceId(DeviceBackend::Metal);
  resources_before.limits.device_memory_bytes = 1U << 20U;
  resources_before.available = resources_before.limits;
  ResourceLedger::DeviceSnapshot resources_after = resources_before;
  resources_after.high_water.device_memory_bytes = host.storage_bytes;
  result.resources_before = resources_before;
  result.resources_after_first = resources_after;
  result.resources_after_second = resources_after;
  return result;
}

/**
 * @brief Copies one exact synthetic I2 child descriptor.
 * @param edit_index Frozen edit identity.
 * @param run_id Unique product Run identity.
 * @param generation Shared request generation.
 * @param quality Preview Interactive or final Full quality.
 * @param deadline Exact child deadline.
 * @param coordinate Shared accepted-boundary identity.
 * @return Complete child descriptor satisfying the frozen private contract.
 * @throws Nothing after optional coordinate copying.
 */
I2ObservedChildDescriptor make_i2_child(
    std::size_t edit_index, std::uint64_t run_id, std::uint64_t generation,
    compute::ComputeRunQuality quality,
    std::chrono::steady_clock::time_point deadline,
    const compute::AcceptedBoundaryCoordinate& coordinate) {
  return I2ObservedChildDescriptor{
      edit_index,
      run_id,
      41U,
      43U,
      kI1TargetNodeId,
      quality == compute::ComputeRunQuality::Interactive
          ? ComputeIntent::RealTimeUpdate
          : ComputeIntent::GlobalHighPrecision,
      quality,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive, deadline,
                             1U, 8U},
      generation,
      ComputeIntent::RealTimeUpdate,
      coordinate};
}

/**
 * @brief Creates one frozen visible record after payload evidence capture.
 * @param child Exact matching child descriptor.
 * @param observed_at Current-visible product time.
 * @param sequence Unique observer causal sequence.
 * @param value Ready CPU Value whose scalar facts are retained.
 * @param digest Independent typed digest assigned to this synthetic endpoint.
 * @return Released-Value observation with complete digest/access evidence.
 * @throws Acquisition or optional/string allocation failures unchanged.
 */
I2ObservedVisibleOutput make_i2_visible(
    const I2ObservedChildDescriptor& child,
    std::chrono::steady_clock::time_point observed_at, std::uint64_t sequence,
    const Value& value, ContentDigest digest) {
  const StorageBinding binding = value.storage_binding();
  return I2ObservedVisibleOutput{
      child,
      observed_at,
      sequence,
      Value{},
      true,
      ContentDigestResult{ContentDigestState::Available, std::move(digest), {}},
      make_i2_acquisition(value),
      value.revision_id(),
      binding,
      binding.allocation,
      value.storage_size()};
}

/**
 * @brief Creates one structurally complete synthetic I2 episode.
 * @param slot Continuous grid slot in `[0,110]`.
 * @param grid_origin Replicate origin used for every row-local time formula.
 * @return Raw closed evidence whose four evaluator verdicts are Pass.
 * @throws Checked-time, Value, digest, and allocation failures unchanged.
 */
I2EpisodeEvidenceInput make_valid_i2_input(
    std::size_t slot, std::chrono::steady_clock::time_point grid_origin =
                          std::chrono::steady_clock::time_point(1s)) {
  I2EpisodeEvidenceInput input;
  input.replicate_ordinal = 1U;
  input.slot = slot;
  input.grid_origin = grid_origin;
  input.episode_origin = i2_episode_origin(input.grid_origin, slot);
  input.terminal_boundary = i2_terminal_boundary(input.grid_origin);

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
  input.baseline.lifecycle.service_instance_id = 47U;
  input.baseline.lifecycle.telemetry_epoch = 53U;
  input.baseline.lifecycle.counters.registered_graph_count = 1U;
  input.baseline.lifecycle.counters.open_graph_count = 1U;
  input.baseline.lifecycle.counters.live_policy_binding_count = 1U;
  input.final_snapshot.lifecycle = input.baseline.lifecycle;

  const Value value = make_i2_evidence_value();
  const ContentDigest preview_digest = i2_frozen_preview_content_digest();
  const ContentDigest final_digest = i1_frozen_final_content_digest();
  input.expected_preview_digest = preview_digest;
  input.expected_final_digest = final_digest;

  std::uint64_t sequence = 1U;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    const auto nominal = checked_i1_time_add(
        input.episode_origin,
        kI1EditStride * static_cast<std::int64_t>(edit_index));
    const auto admission = checked_i1_time_add(nominal, 1ms);
    const auto preview_deadline =
        checked_i1_time_add(admission, kI2PreviewDeadlineBudget);
    const auto final_deadline =
        checked_i1_time_add(admission, kI2FinalDeadlineBudget);
    const compute::AcceptedBoundaryCoordinate coordinate(
        admission, static_cast<std::uint64_t>(edit_index + 1U));
    input.edits[edit_index] = I2EditEvidence{
        edit_index,
        kI1EditCoefficients[edit_index],
        i1_edit_region(edit_index),
        i2_preview_region(edit_index),
        nominal,
        true,
        admission,
        true,
        static_cast<std::uint64_t>(edit_index + 1U),
        preview_deadline,
        final_deadline,
        I1HostReturnEvidence{checked_i1_time_add(admission, 100us),
                             OperationStatus{}, true},
        coordinate,
        OperationStatus{}};

    const std::uint64_t generation = edit_index + 1U;
    const I2ObservedChildDescriptor preview = make_i2_child(
        edit_index, 100U + edit_index * 2U, generation,
        compute::ComputeRunQuality::Interactive, preview_deadline, coordinate);
    const I2ObservedChildDescriptor final = make_i2_child(
        edit_index, 101U + edit_index * 2U, generation,
        compute::ComputeRunQuality::Full, final_deadline, coordinate);
    const auto preview_visible_at = checked_i1_time_add(admission, 2ms);
    const auto trigger_at = checked_i1_time_add(admission, 4ms);
    const auto final_visible_at = checked_i1_time_add(admission, 6ms);

    input.observations.current_generations.push_back(
        I1ObservedCurrentGeneration{edit_index, generation, admission,
                                    sequence++, coordinate});
    input.observations.service_starts.push_back(I2ObservedServiceStart{
        preview, 0U, 100U, checked_i1_time_add(admission, 1ms), sequence++});
    input.observations.visible_outputs.push_back(make_i2_visible(
        preview, preview_visible_at, sequence++, value, preview_digest));
    input.observations.terminals.push_back(
        I2ObservedTerminal{preview, compute::ComputeRunTerminalKind::Succeeded,
                           checked_i1_time_add(admission, 3ms), sequence++});
    input.observations.run_quiescences.push_back(
        I2ObservedRunLifecycleTransition{
            preview, checked_i1_time_add(admission, 3100us), sequence++});
    input.observations.resource_settlements.push_back(
        I2ObservedRunLifecycleTransition{
            preview, checked_i1_time_add(admission, 3200us), sequence++});
    input.observations.final_triggers.push_back(
        I2ObservedFinalTrigger{final, trigger_at, sequence++});
    input.observations.service_starts.push_back(I2ObservedServiceStart{
        final, 0U, 400U, checked_i1_time_add(admission, 5ms), sequence++});
    input.observations.visible_outputs.push_back(make_i2_visible(
        final, final_visible_at, sequence++, value, final_digest));
    input.observations.terminals.push_back(
        I2ObservedTerminal{final, compute::ComputeRunTerminalKind::Succeeded,
                           checked_i1_time_add(admission, 7ms), sequence++});
    input.observations.run_quiescences.push_back(
        I2ObservedRunLifecycleTransition{
            final, checked_i1_time_add(admission, 7100us), sequence++});
    input.observations.resource_settlements.push_back(
        I2ObservedRunLifecycleTransition{
            final, checked_i1_time_add(admission, 7200us), sequence++});
    input.observations.host_settlements.push_back(I1ObservedHostSettlement{
        edit_index, checked_i1_time_add(admission, 8ms), sequence++});
  }
  input.observation_cut = I1ObservationHistoryCut{
      checked_i1_time_add(input.episode_origin, 500ms), sequence};
  input.final_snapshot_sample =
      checked_i1_time_add(input.observation_cut.captured_at, 1us);
  return input;
}

/**
 * @brief Creates 111 aggregate-ready rows on one exact continuous grid.
 * @return Complete slot-indexed rows with closed grid facts and Pass row axes.
 * @throws std::bad_alloc when row storage grows.
 * @note The latency distribution intentionally exercises aggregate thresholds;
 * these rows bypass episode evaluation only to isolate replicate arithmetic.
 */
std::vector<I2EpisodeInnerRow> make_i2_aggregate_rows() {
  const auto grid_origin = std::chrono::steady_clock::time_point(1s);
  const auto terminal_boundary = i2_terminal_boundary(grid_origin);
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  for (std::size_t slot = 0U; slot < kI2GridSlotCount; ++slot) {
    I2EpisodeInnerRow row;
    row.evidence.replicate_ordinal = 1U;
    row.evidence.slot = slot;
    row.evidence.grid_origin = grid_origin;
    row.evidence.episode_origin = i2_episode_origin(grid_origin, slot);
    row.evidence.terminal_boundary = terminal_boundary;
    const std::chrono::milliseconds measured_rank =
        slot <= kI2WarmupSlotCount
            ? 1ms
            : std::chrono::milliseconds(static_cast<std::int64_t>(slot - 10U));
    row.latencies.preview = measured_rank;
    row.latencies.final = measured_rank * 2;
    row.service = I1ServiceEvidence{100U, 10U, 0U, 0.1};
    row.latency_verdict = I1Verdict::Pass;
    row.waste_verdict = I1Verdict::Pass;
    row.memory_verdict = I1Verdict::Pass;
    row.output_verdict = I1Verdict::Pass;
    rows.push_back(std::move(row));
  }
  return rows;
}

/**
 * @brief Produces a typed digest that differs from one frozen oracle.
 * @param digest Complete digest to copy and alter deterministically.
 * @return Same typed algorithm with a different first digest byte.
 * @throws Nothing.
 */
ContentDigest forge_i2_digest(ContentDigest digest) noexcept {
  digest.bytes.front() = digest.bytes.front() == std::byte{0x00}
                             ? std::byte{0x01}
                             : std::byte{0x00};
  return digest;
}

/**
 * @brief Finds one twelfth-edit visible endpoint in mutable raw evidence.
 * @param input Complete episode evidence to inspect.
 * @param quality Interactive preview or Full final endpoint selector.
 * @return Mutable matching record, or null when the endpoint is absent.
 * @throws Nothing.
 */
I2ObservedVisibleOutput* find_i2_endpoint(
    I2EpisodeEvidenceInput* input,
    compute::ComputeRunQuality quality) noexcept {
  const auto found =
      std::find_if(input->observations.visible_outputs.begin(),
                   input->observations.visible_outputs.end(),
                   [quality](const I2ObservedVisibleOutput& output) {
                     return output.child.edit_index == kI1EditCount - 1U &&
                            output.child.quality == quality;
                   });
  return found == input->observations.visible_outputs.end() ? nullptr : &*found;
}

/**
 * @brief Tests whether one evaluated row contains an exact validity reason.
 * @param row Evaluated I2 row.
 * @param reason Stable reason text to locate.
 * @return True when the reason occurs at least once.
 * @throws Nothing.
 */
bool has_i2_reason(const I2EpisodeInnerRow& row,
                   const std::string& reason) noexcept {
  return std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                   reason) != row.validity_reasons.end();
}

/**
 * @brief Erases one edit/quality child from a homogeneous event vector.
 * @tparam Event Child-aware I2 observation type exposing `child`.
 * @param events Mutable event vector.
 * @param edit_index Exact edit identity.
 * @param quality Interactive preview or Full final selector.
 * @return Nothing after stable erase-remove compaction.
 * @throws Nothing for the maintained trivially movable observation types.
 */
template <typename Event>
void erase_i2_child_events(std::vector<Event>* events, std::size_t edit_index,
                           compute::ComputeRunQuality quality) {
  events->erase(std::remove_if(events->begin(), events->end(),
                               [edit_index, quality](const Event& event) {
                                 return event.child.edit_index == edit_index &&
                                        event.child.quality == quality;
                               }),
                events->end());
}

/**
 * @brief Removes every lifecycle fact for one synthetic materialized child.
 * @param input Complete mutable episode evidence.
 * @param edit_index Exact edit identity.
 * @param quality Interactive preview or Full final selector.
 * @return Nothing after removing all child-owned observations.
 * @throws Nothing for maintained observation-vector compaction.
 * @note Current-generation, Host return, and Host settlement evidence remain
 * request-owned and are deliberately preserved.
 */
void erase_i2_materialized_child(I2EpisodeEvidenceInput* input,
                                 std::size_t edit_index,
                                 compute::ComputeRunQuality quality) {
  erase_i2_child_events(&input->observations.service_starts, edit_index,
                        quality);
  erase_i2_child_events(&input->observations.cancellations, edit_index,
                        quality);
  erase_i2_child_events(&input->observations.terminals, edit_index, quality);
  erase_i2_child_events(&input->observations.final_triggers, edit_index,
                        quality);
  erase_i2_child_events(&input->observations.visible_outputs, edit_index,
                        quality);
  erase_i2_child_events(&input->observations.run_quiescences, edit_index,
                        quality);
  erase_i2_child_events(&input->observations.resource_settlements, edit_index,
                        quality);
}

/**
 * @brief Converts one synthetic successful final into a cancelled final.
 * @param input Complete mutable episode evidence.
 * @param edit_index Exact edit whose final is changed.
 * @return Nothing after adding exact earlier cancellation and failed Host
 * status.
 * @throws std::logic_error when the expected final terminal is absent.
 * @throws std::bad_alloc when cancellation storage grows.
 * @note The removed final-visible sequence is reused between service start and
 * terminal, preserving unique request-causal order.
 */
void cancel_i2_final(I2EpisodeEvidenceInput* input, std::size_t edit_index) {
  const auto terminal = std::find_if(
      input->observations.terminals.begin(),
      input->observations.terminals.end(),
      [edit_index](const I2ObservedTerminal& event) {
        return event.child.edit_index == edit_index &&
               event.child.quality == compute::ComputeRunQuality::Full;
      });
  if (terminal == input->observations.terminals.end()) {
    throw std::logic_error("Synthetic I2 final terminal is missing.");
  }
  erase_i2_child_events(&input->observations.visible_outputs, edit_index,
                        compute::ComputeRunQuality::Full);
  terminal->kind = compute::ComputeRunTerminalKind::Cancelled;
  input->observations.cancellations.push_back(I2ObservedCancellation{
      terminal->child, compute::ComputeRunCancellationReason::Superseded,
      checked_i1_time_add(input->edits[edit_index].admission_sample, 6500us),
      terminal->causal_sequence - 1U});
  input->edits[edit_index].settlement_status->ok = false;
}

/**
 * @brief Shifts one homogeneous event vector to open a causal sequence.
 * @tparam Event I2 observation type exposing mutable `causal_sequence`.
 * @param events Mutable observation vector.
 * @param insertion_sequence Nonzero sequence to make available.
 * @return Nothing.
 * @throws std::overflow_error when any shifted sequence is exhausted.
 * @note Event order in the vector is irrelevant; only causal scalars change.
 */
template <typename Event>
void shift_i2_sequences_at_or_after(std::vector<Event>* events,
                                    std::uint64_t insertion_sequence) {
  for (Event& event : *events) {
    if (event.causal_sequence >= insertion_sequence) {
      if (event.causal_sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Synthetic I2 causal sequence exhausted.");
      }
      ++event.causal_sequence;
    }
  }
}

/**
 * @brief Opens one causal sequence across a complete synthetic observation cut.
 * @param input Mutable complete episode evidence.
 * @param insertion_sequence Nonzero sequence to reserve for a new event.
 * @return Nothing.
 * @throws std::invalid_argument when the requested sequence is zero.
 * @throws std::overflow_error when an event or history cut is exhausted.
 * @note Accepted-coordinate event sequences are a separate row-local domain
 * and deliberately remain unchanged.
 */
void make_i2_causal_sequence_room(I2EpisodeEvidenceInput* input,
                                  std::uint64_t insertion_sequence) {
  if (insertion_sequence == 0U) {
    throw std::invalid_argument(
        "Synthetic I2 insertion sequence must be nonzero.");
  }
  shift_i2_sequences_at_or_after(&input->observations.current_generations,
                                 insertion_sequence);
  shift_i2_sequences_at_or_after(&input->observations.service_starts,
                                 insertion_sequence);
  shift_i2_sequences_at_or_after(&input->observations.cancellations,
                                 insertion_sequence);
  shift_i2_sequences_at_or_after(&input->observations.terminals,
                                 insertion_sequence);
  shift_i2_sequences_at_or_after(&input->observations.final_triggers,
                                 insertion_sequence);
  shift_i2_sequences_at_or_after(&input->observations.visible_outputs,
                                 insertion_sequence);
  shift_i2_sequences_at_or_after(&input->observations.run_quiescences,
                                 insertion_sequence);
  shift_i2_sequences_at_or_after(&input->observations.resource_settlements,
                                 insertion_sequence);
  shift_i2_sequences_at_or_after(&input->observations.host_settlements,
                                 insertion_sequence);
  if (input->observation_cut.causal_sequence >= insertion_sequence) {
    if (input->observation_cut.causal_sequence ==
        std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Synthetic I2 history cut exhausted.");
    }
    ++input->observation_cut.causal_sequence;
  }
}

/**
 * @brief Appends one service start immediately before its matching terminal.
 * @param input Mutable complete episode evidence.
 * @param start Prototype carrying exact child, charge, task, and time facts.
 * @return Nothing.
 * @throws std::logic_error when the matching child terminal is absent.
 * @throws std::overflow_error when causal sequence space is exhausted.
 * @throws std::bad_alloc when observation storage grows.
 * @note Callers may preserve the prototype task id to model duplicate/retry or
 * replace it to model a distinct useful local task. The event time is retained
 * verbatim so post-cancel tests can select the exact intersection boundary.
 */
void append_i2_service_start_before_terminal(I2EpisodeEvidenceInput* input,
                                             I2ObservedServiceStart start) {
  const auto terminal =
      std::find_if(input->observations.terminals.begin(),
                   input->observations.terminals.end(),
                   [&start](const I2ObservedTerminal& event) {
                     return event.child.run_id == start.child.run_id;
                   });
  if (terminal == input->observations.terminals.end()) {
    throw std::logic_error("Synthetic I2 child terminal is missing.");
  }
  const std::uint64_t insertion_sequence = terminal->causal_sequence;
  make_i2_causal_sequence_room(input, insertion_sequence);
  start.causal_sequence = insertion_sequence;
  input->observations.service_starts.push_back(std::move(start));
}

/**
 * @brief Creates aggregate rows whose measured latency and waste both pass.
 * @return Complete continuous grid with 100 identical measured samples.
 * @throws std::bad_alloc when row storage grows.
 */
std::vector<I2EpisodeInnerRow> make_passing_i2_aggregate_rows() {
  std::vector<I2EpisodeInnerRow> rows = make_i2_aggregate_rows();
  for (I2EpisodeInnerRow& row : rows) {
    row.latencies.preview = 10ms;
    row.latencies.final = 20ms;
  }
  return rows;
}

/** @brief Whether the injected I2 async worker reached its ownership gate. */
std::atomic_bool i2_gate_worker_arrived{false};

/** @brief Whether the injected I2 evaluator consumed its closed input. */
std::atomic_bool i2_gate_evaluator_entered{false};

/** @brief Whether a controllably delayed evaluator may finish consumption. */
std::atomic_bool i2_gate_evaluator_released{false};

/** @brief Whether the launcher observed premature evaluator entry. */
std::atomic_bool i2_gate_launcher_saw_evaluator{false};

/** @brief Borrowed slot-order sink used only by the serializer seam. */
std::vector<std::size_t>* i2_serialized_slots = nullptr;

/**
 * @brief Records that one worker reached the recoverable launch gate.
 * @return Nothing.
 * @throws Nothing.
 */
void observe_i2_worker_at_launch_gate() noexcept {
  i2_gate_worker_arrived.store(true, std::memory_order_release);
}

/**
 * @brief Records evaluator entry and delegates to the real I2 evaluator.
 * @param input Complete closed Value-free evidence.
 * @return Evaluated row for the same slot.
 * @throws Evaluation failures unchanged.
 */
I2EpisodeInnerRow observe_then_evaluate_i2(I2EpisodeEvidenceInput input) {
  i2_gate_evaluator_entered.store(true, std::memory_order_release);
  return evaluate_i2_episode(std::move(input));
}

/**
 * @brief Holds Value-free evaluation until the test completes baseline work.
 * @param input Complete closed Value-free evidence.
 * @return Evaluated row after explicit release.
 * @throws Evaluation failures unchanged.
 * @note The bounded test process owns the release flag; the workflow still
 * owns the sole future and input-consumption decision.
 */
I2EpisodeInnerRow block_then_evaluate_i2(I2EpisodeEvidenceInput input) {
  i2_gate_evaluator_entered.store(true, std::memory_order_release);
  while (!i2_gate_evaluator_released.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  return evaluate_i2_episode(std::move(input));
}

/**
 * @brief Launches a worker and waits until it is blocked before returning.
 * @param task Sole recoverable I2 evaluation task.
 * @return Valid future owning that task.
 * @throws std::system_error or std::bad_alloc from `std::async` unchanged.
 * @note The launcher records whether evaluation began before future
 * installation was allowed, making ownership order deterministic.
 */
std::future<I2EpisodeInnerRow> launch_i2_worker_before_return(
    I2EpisodeEvaluationTask task) {
  std::future<I2EpisodeInnerRow> future =
      std::async(std::launch::async, std::move(task));
  while (!i2_gate_worker_arrived.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  i2_gate_launcher_saw_evaluator.store(
      i2_gate_evaluator_entered.load(std::memory_order_acquire),
      std::memory_order_release);
  return future;
}

/**
 * @brief Rejects one evaluator launch with a stable system error.
 * @param task Unretained task whose input must remain recoverable.
 * @return Never returns a future.
 * @throws std::system_error unconditionally.
 */
std::future<I2EpisodeInnerRow> throw_i2_system_error_launcher(
    I2EpisodeEvaluationTask task) {
  static_cast<void>(task);
  throw std::system_error(
      std::make_error_code(std::errc::resource_unavailable_try_again),
      "injected I2 async launcher failure");
}

/**
 * @brief Serializes one row while recording exact explicit-drain order.
 * @param row Complete un-compacted I2 row.
 * @return Full production JSON text.
 * @throws std::logic_error when the test-owned order sink is absent.
 * @throws JSON or allocation failures unchanged.
 */
std::string observe_i2_serializer(const I2EpisodeInnerRow& row) {
  if (i2_serialized_slots == nullptr) {
    throw std::logic_error("I2 serializer observation sink is absent");
  }
  i2_serialized_slots->push_back(row.evidence.slot);
  return i2_inner_row_json(row).dump();
}

/**
 * @brief Records one row and rejects the second slot before stream mutation.
 * @param row Complete un-compacted I2 row.
 * @return Production JSON text for every slot except one.
 * @throws std::runtime_error unconditionally for slot one.
 * @throws std::logic_error when the observation sink is absent.
 */
std::string observe_then_fail_i2_serializer(const I2EpisodeInnerRow& row) {
  if (i2_serialized_slots == nullptr) {
    throw std::logic_error("I2 serializer observation sink is absent");
  }
  i2_serialized_slots->push_back(row.evidence.slot);
  if (row.evidence.slot == 1U) {
    throw std::runtime_error("injected I2 serializer failure");
  }
  return i2_inner_row_json(row).dump();
}

/**
 * @brief Creates fixed-width I2 admissions with one attempted failure.
 * @param episode_origin Exact failed episode origin.
 * @return Twelve records whose first is accepted, second fails, and suffix is
 * untouched.
 * @throws Checked-time, future-state, or diagnostic allocation failures
 * unchanged.
 */
std::array<I2EditAdmissionResult, kI1EditCount>
make_i2_failed_finalization_admissions(
    std::chrono::steady_clock::time_point episode_origin) {
  std::array<I2EditAdmissionResult, kI1EditCount> admissions;
  for (std::size_t edit_index = 0U; edit_index < kI1EditCount; ++edit_index) {
    admissions[edit_index].edit_index = edit_index;
    admissions[edit_index].nominal_time = checked_i1_time_add(
        episode_origin, kI1EditStride * static_cast<std::int64_t>(edit_index));
  }
  I2EditAdmissionResult& accepted = admissions.front();
  accepted.admission_attempted = true;
  accepted.admission_sample = episode_origin;
  accepted.admission_window_valid = true;
  accepted.reserved_event_sequence = 1U;
  accepted.preview_deadline =
      checked_i1_time_add(episode_origin, kI2PreviewDeadlineBudget);
  accepted.final_deadline =
      checked_i1_time_add(episode_origin, kI2FinalDeadlineBudget);
  accepted.host_return = I1HostReturnEvidence{
      checked_i1_time_add(episode_origin, 100us), OperationStatus{}, true};
  accepted.accepted_coordinate =
      compute::AcceptedBoundaryCoordinate(episode_origin, 1U);
  std::promise<OperationStatus> accepted_settlement;
  accepted.settlement = accepted_settlement.get_future();
  accepted_settlement.set_value(OperationStatus{});

  I2EditAdmissionResult& failed = admissions[1U];
  failed.admission_attempted = true;
  failed.admission_sample = failed.nominal_time;
  failed.admission_window_valid = true;
  failed.reserved_event_sequence = 2U;
  failed.preview_deadline =
      checked_i1_time_add(failed.admission_sample, kI2PreviewDeadlineBudget);
  failed.final_deadline =
      checked_i1_time_add(failed.admission_sample, kI2FinalDeadlineBudget);
  failed.host_return = I1HostReturnEvidence{
      checked_i1_time_add(failed.admission_sample, 100us),
      OperationStatus{false, OperationErrorDomain::Graph,
                      static_cast<std::int32_t>(GraphErrc::ComputeError),
                      graph_error_stable_name(GraphErrc::ComputeError),
                      "injected I2 failed admission"},
      false};
  return admissions;
}

/**
 * @brief Deterministic failed-admission finalization port.
 * @throws Nothing after its fixed final sample is constructed.
 * @note The port records only ordering and outer-write ownership; it has no
 * Host, Graph, stream, Value, or residency authority.
 */
class RecordingI2FailedAdmissionPort final
    : public I2FailedAdmissionFinalizationPort {
 public:
  /**
   * @brief Binds one post-close sample inside the current episode guard.
   * @param final_sample Fixed monotonic sample returned after snapshot.
   * @throws Nothing.
   */
  explicit RecordingI2FailedAdmissionPort(
      std::chrono::steady_clock::time_point final_sample) noexcept
      : final_sample_(final_sample) {}

  /** @copydoc I2FailedAdmissionFinalizationPort::close_graph */
  OperationStatus close_graph() override {
    ++close_calls;
    return {};
  }

  /** @copydoc
   * I2FailedAdmissionFinalizationPort::capture_closed_execution_snapshot */
  I1ExecutionSnapshot capture_closed_execution_snapshot() override {
    ++snapshot_calls;
    return {};
  }

  /** @copydoc I2FailedAdmissionFinalizationPort::monotonic_now */
  std::chrono::steady_clock::time_point monotonic_now() override {
    ++monotonic_calls;
    return final_sample_;
  }

  /** @copydoc I2FailedAdmissionFinalizationPort::persist_outer_failure */
  void persist_outer_failure(std::string_view diagnostic) override {
    ++outer_calls;
    persisted_diagnostic.assign(diagnostic.data(), diagnostic.size());
  }

  /** @copydoc I2FailedAdmissionFinalizationPort::observe_stage */
  void observe_stage(
      I2FailedAdmissionFinalizationStage stage) noexcept override {
    if (stage_count >= stages.size()) {
      stage_overflow = true;
      return;
    }
    stages[stage_count++] = stage;
  }

  /** @brief Exact Graph-close invocation count. */
  std::size_t close_calls = 0U;
  /** @brief Exact closed-snapshot invocation count. */
  std::size_t snapshot_calls = 0U;
  /** @brief Exact monotonic-sample invocation count. */
  std::size_t monotonic_calls = 0U;
  /** @brief Exact outer-persistence invocation count. */
  std::size_t outer_calls = 0U;
  /** @brief Whether stage storage exceeded its fixed six entries. */
  bool stage_overflow = false;
  /** @brief Number of monotonically recorded finalization stages. */
  std::size_t stage_count = 0U;
  /** @brief Fixed no-allocation stage sequence. */
  std::array<I2FailedAdmissionFinalizationStage, 6U> stages{};
  /** @brief Last complete outer diagnostic. */
  std::string persisted_diagnostic;

 private:
  /** @brief Fixed final snapshot sample owned by the fake. */
  std::chrono::steady_clock::time_point final_sample_;
};

/**
 * @brief Proves the I2 worker cannot consume input before future installation.
 * @throws Workflow and evaluation failures reach GoogleTest.
 */
TEST(I2EvidenceWorkflow,
     AsyncWorkerWaitsForFutureInstallationBeforeEvaluation) {
  i2_gate_worker_arrived.store(false, std::memory_order_relaxed);
  i2_gate_evaluator_entered.store(false, std::memory_order_relaxed);
  i2_gate_launcher_saw_evaluator.store(false, std::memory_order_relaxed);
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  std::optional<std::future<I2EpisodeInnerRow>> pending;

  start_i2_episode_evaluation(
      make_valid_i2_input(0U), &pending, &rows, &launch_i2_worker_before_return,
      &observe_then_evaluate_i2, &observe_i2_worker_at_launch_gate);

  EXPECT_TRUE(i2_gate_worker_arrived.load(std::memory_order_acquire));
  EXPECT_FALSE(i2_gate_launcher_saw_evaluator.load(std::memory_order_acquire));
  EXPECT_TRUE(pending.has_value());
  collect_i2_episode_evaluation_until(
      checked_i1_time_add(std::chrono::steady_clock::now(), 1s), &pending,
      &rows);
  EXPECT_TRUE(i2_gate_evaluator_entered.load(std::memory_order_acquire));
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().evidence.slot, 0U);
}

/**
 * @brief Proves baseline work may overlap exactly one delayed evaluator.
 * @throws Workflow and evaluation failures reach GoogleTest.
 * @note An already-expired handoff fails without consuming the future or
 * shifting the next origin; explicit release then permits ordered collection.
 */
TEST(I2EvidenceWorkflow,
     DelayedEvaluatorOverlapsBaselineButCannotMoveFixedHandoff) {
  i2_gate_worker_arrived.store(false, std::memory_order_relaxed);
  i2_gate_evaluator_entered.store(false, std::memory_order_relaxed);
  i2_gate_evaluator_released.store(false, std::memory_order_relaxed);
  i2_gate_launcher_saw_evaluator.store(false, std::memory_order_relaxed);
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  std::optional<std::future<I2EpisodeInnerRow>> pending;
  const auto grid_origin =
      std::chrono::steady_clock::time_point(std::chrono::nanoseconds(123));
  const auto next_origin = i2_episode_origin(grid_origin, 1U);

  start_i2_episode_evaluation(
      make_valid_i2_input(0U), &pending, &rows, &launch_i2_worker_before_return,
      &block_then_evaluate_i2, &observe_i2_worker_at_launch_gate);
  while (!i2_gate_evaluator_entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  if (!pending.has_value()) {
    i2_gate_evaluator_released.store(true, std::memory_order_release);
    FAIL() << "I2 delayed evaluator future was not installed";
    return;
  }
  EXPECT_EQ(pending->wait_for(std::chrono::nanoseconds::zero()),
            std::future_status::timeout);

  std::size_t baseline_preparation_count = 0U;
  ++baseline_preparation_count;
  EXPECT_EQ(baseline_preparation_count, 1U);
  EXPECT_THROW(
      collect_i2_episode_evaluation_until(
          std::chrono::steady_clock::time_point::min(), &pending, &rows),
      std::runtime_error);
  EXPECT_TRUE(pending.has_value());
  EXPECT_TRUE(rows.empty());
  EXPECT_EQ(i2_episode_origin(grid_origin, 1U), next_origin);

  i2_gate_evaluator_released.store(true, std::memory_order_release);
  collect_i2_episode_evaluation_until(
      checked_i1_time_add(std::chrono::steady_clock::now(), 1s), &pending,
      &rows);
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().evidence.slot, 0U);
  EXPECT_EQ(i2_episode_origin(grid_origin, 1U), next_origin);
}

/**
 * @brief Proves launch failure evaluates one recoverable row then propagates.
 * @throws Nothing when the expected injected system error is observed.
 */
TEST(I2EvidenceWorkflow,
     AsyncLaunchFailureRecoversCurrentRowBeforePropagation) {
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  std::optional<std::future<I2EpisodeInnerRow>> pending;

  EXPECT_THROW(start_i2_episode_evaluation(
                   make_valid_i2_input(0U), &pending, &rows,
                   &throw_i2_system_error_launcher, &evaluate_i2_episode),
               std::system_error);
  EXPECT_FALSE(pending.has_value());
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().evidence.slot, 0U);
}

/**
 * @brief Proves evaluation never enters the serializer before explicit drain.
 * @throws Workflow, JSON, or stream failures reach GoogleTest.
 * @note Repeated drain is cursor-idempotent and preserves physical slot order.
 */
TEST(I2EvidenceWorkflow,
     SerializationBeginsOnlyAtExplicitOrderedTerminalDrain) {
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  std::optional<std::future<I2EpisodeInnerRow>> pending;
  std::vector<std::size_t> serialized_slots;
  i2_serialized_slots = &serialized_slots;

  start_i2_episode_evaluation(make_valid_i2_input(0U), &pending, &rows);
  collect_i2_episode_evaluation_until(
      checked_i1_time_add(std::chrono::steady_clock::now(), 1s), &pending,
      &rows);
  start_i2_episode_evaluation(make_valid_i2_input(1U), &pending, &rows);
  collect_i2_episode_evaluation_until(
      checked_i1_time_add(std::chrono::steady_clock::now(), 1s), &pending,
      &rows);
  EXPECT_TRUE(serialized_slots.empty());
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_GE(rows.capacity(), kI2GridSlotCount);

  std::ostringstream output;
  std::size_t written = 0U;
  flush_i2_episode_rows(&output, rows, &written, &observe_i2_serializer);
  flush_i2_episode_rows(&output, rows, &written, &observe_i2_serializer);
  i2_serialized_slots = nullptr;

  EXPECT_EQ(written, 2U);
  EXPECT_EQ(serialized_slots, (std::vector<std::size_t>{0U, 1U}));
  EXPECT_FALSE(rows.front().evidence.observations.visible_outputs.empty());
}

/**
 * @brief Proves a serializer failure preserves cursor and raw row ownership.
 * @throws Workflow, JSON, or injected serializer failures are checked by
 * GoogleTest.
 * @note A later explicit drain resumes at the first uncommitted row and does
 * not duplicate the already flushed prefix.
 */
TEST(I2EvidenceWorkflow,
     SerializerFailurePreservesDurableCursorAndCompleteRows) {
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  std::optional<std::future<I2EpisodeInnerRow>> pending;
  for (std::size_t slot = 0U; slot < 2U; ++slot) {
    start_i2_episode_evaluation(make_valid_i2_input(slot), &pending, &rows);
    collect_i2_episode_evaluation_until(
        checked_i1_time_add(std::chrono::steady_clock::now(), 1s), &pending,
        &rows);
  }
  std::vector<std::size_t> serialized_slots;
  i2_serialized_slots = &serialized_slots;
  std::ostringstream output;
  std::size_t written = 0U;

  EXPECT_THROW(flush_i2_episode_rows(&output, rows, &written,
                                     &observe_then_fail_i2_serializer),
               std::runtime_error);
  EXPECT_EQ(written, 1U);
  ASSERT_EQ(serialized_slots, (std::vector<std::size_t>{0U, 1U}));
  EXPECT_FALSE(rows[1U].evidence.observations.visible_outputs.empty());

  serialized_slots.clear();
  flush_i2_episode_rows(&output, rows, &written, &observe_i2_serializer);
  i2_serialized_slots = nullptr;
  EXPECT_EQ(written, 2U);
  EXPECT_EQ(serialized_slots, (std::vector<std::size_t>{1U}));
}

/**
 * @brief Proves generic abort joins one row and flushes it exactly once.
 * @throws Nothing when the original primary diagnostic remains authoritative.
 */
TEST(I2EvidenceWorkflow, GenericAbortDrainsOnlyCompleteOrderedRows) {
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  std::optional<std::future<I2EpisodeInnerRow>> pending;
  I2OuterPersistenceOwnershipGate ownership_gate;
  std::ostringstream output;
  std::size_t written = 0U;
  start_i2_episode_evaluation(make_valid_i2_input(0U), &pending, &rows);

  try {
    rethrow_i2_runner_failure_after_generic_drain(
        std::make_exception_ptr(std::runtime_error("primary I2 abort")),
        ownership_gate, &pending, &rows, &output, &written);
    FAIL() << "expected primary I2 abort";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()), "primary I2 abort");
  }

  EXPECT_FALSE(pending.has_value());
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(written, 1U);
  std::size_t generic_outer_calls = 0U;
  try_i2_generic_outer_failure_persistence(
      ownership_gate, [&generic_outer_calls] { ++generic_outer_calls; });
  EXPECT_EQ(generic_outer_calls, 1U);
}

/**
 * @brief Proves failed admission uniquely owns Invalid-row then outer output.
 * @throws Setup and finalization failures are checked by GoogleTest.
 */
TEST(I2EvidenceWorkflow,
     FailedAdmissionFlushesInvalidRowAndSuppressesGenericPersistence) {
  I2EpisodeEvidenceInput input;
  input.replicate_ordinal = 1U;
  input.slot = 0U;
  input.grid_origin = std::chrono::steady_clock::now();
  input.episode_origin = i2_episode_origin(input.grid_origin, input.slot);
  input.terminal_boundary = i2_terminal_boundary(input.grid_origin);
  auto admissions =
      make_i2_failed_finalization_admissions(input.episode_origin);
  I2EpisodeObservationCollector observations;
  RecordingI2FailedAdmissionPort port(
      checked_i1_time_add(input.episode_origin, 100ms));
  I2OuterPersistenceOwnershipGate ownership_gate;
  ASSERT_TRUE(
      claim_i2_failed_admission_if_needed(admissions[1U], ownership_gate));
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  std::ostringstream output;
  std::size_t written = 0U;

  std::exception_ptr terminal_failure;
  try {
    finalize_i2_failed_admission("injected I2 admission failure",
                                 std::move(input), std::move(admissions),
                                 ownership_gate, &observations, &port, &output,
                                 &rows, &written);
  } catch (...) {
    terminal_failure = std::current_exception();
  }
  ASSERT_NE(terminal_failure, nullptr);
  EXPECT_TRUE(ownership_gate.failed_admission_finalizer_owns_persistence());
  EXPECT_EQ(port.close_calls, 1U);
  EXPECT_EQ(port.snapshot_calls, 1U);
  EXPECT_EQ(port.monotonic_calls, 1U);
  EXPECT_EQ(port.outer_calls, 1U);
  EXPECT_FALSE(port.stage_overflow);
  EXPECT_EQ(port.stage_count, port.stages.size());
  EXPECT_EQ(port.stages[0U],
            I2FailedAdmissionFinalizationStage::GraphCloseCompleted);
  EXPECT_EQ(port.stages[1U],
            I2FailedAdmissionFinalizationStage::HistoryCutCaptured);
  EXPECT_EQ(port.stages[2U],
            I2FailedAdmissionFinalizationStage::UnfrozenOutputsReleased);
  EXPECT_EQ(
      port.stages[3U],
      I2FailedAdmissionFinalizationStage::ClosedExecutionSnapshotCaptured);
  EXPECT_EQ(port.stages[4U],
            I2FailedAdmissionFinalizationStage::InnerRowFlushed);
  EXPECT_EQ(port.stages[5U],
            I2FailedAdmissionFinalizationStage::OuterFailurePersistenceStarted);
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(rows.front().latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(rows.front().waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(rows.front().memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(rows.front().output_verdict, I1Verdict::Invalid);
  EXPECT_TRUE(rows.front().evidence.edits.front().admission_attempted);
  EXPECT_TRUE(
      rows.front().evidence.edits.front().settlement_status.has_value());
  EXPECT_TRUE(rows.front().evidence.edits[1U].admission_attempted);
  EXPECT_FALSE(rows.front().evidence.edits[1U].accepted_coordinate.has_value());
  EXPECT_FALSE(rows.front().evidence.edits[2U].admission_attempted);

  std::optional<std::future<I2EpisodeInnerRow>> pending;
  try {
    rethrow_i2_runner_failure_after_generic_drain(
        terminal_failure, ownership_gate, &pending, &rows, &output, &written);
  } catch (...) {
  }
  std::size_t generic_outer_calls = 0U;
  try_i2_generic_outer_failure_persistence(
      ownership_gate, [&generic_outer_calls] { ++generic_outer_calls; });
  EXPECT_EQ(generic_outer_calls, 0U);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(port.outer_calls, 1U);
}

/**
 * @brief Proves a complete closed row independently passes all four axes.
 * @throws Nothing when the synthetic product/evidence contract stays stable.
 */
TEST(I2Evidence, CompleteEpisodePassesIndependentVerdicts) {
  const I2EpisodeInnerRow row = evaluate_i2_episode(make_valid_i2_input(0U));

  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.latencies.preview, 2ms);
  EXPECT_EQ(row.latencies.final, 6ms);
}

/**
 * @brief Proves only the first start of a visible Run-local task is useful.
 * @throws Synthetic evidence allocation or checked arithmetic failures fail
 * the test.
 * @note The duplicate remains pre-cancel, so only discarded service changes;
 * the replicate-level ratio gate is exercised separately below.
 */
TEST(I2Evidence, SuccessfulVisibleRunDuplicateTaskCountsAsDiscarded) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  const I2ObservedServiceStart original =
      input.observations.service_starts.back();
  append_i2_service_start_before_terminal(&input, original);

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.service.all_started_service, 6400U);
  EXPECT_EQ(row.service.discarded_started_service, 400U);
  EXPECT_EQ(row.service.post_cancel_started_service, 0U);
  ASSERT_TRUE(row.service.discarded_ratio.has_value());
  EXPECT_DOUBLE_EQ(*row.service.discarded_ratio, 0.0625);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves distinct local tasks of one visible Run remain independently
 * useful.
 * @throws Synthetic evidence allocation or checked arithmetic failures fail
 * the test.
 */
TEST(I2Evidence, SuccessfulVisibleRunDistinctLocalTasksRemainUseful) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  I2ObservedServiceStart distinct = input.observations.service_starts.back();
  distinct.local_task_id = 1U;
  append_i2_service_start_before_terminal(&input, std::move(distinct));

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.service.all_started_service, 6400U);
  EXPECT_EQ(row.service.discarded_started_service, 0U);
  EXPECT_EQ(row.service.post_cancel_started_service, 0U);
  ASSERT_TRUE(row.service.discarded_ratio.has_value());
  EXPECT_DOUBLE_EQ(*row.service.discarded_ratio, 0.0);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves duplicate and post-cancel service intersect without drift.
 * @throws Synthetic evidence allocation or checked arithmetic failures fail
 * the test.
 * @note The cancelled final's original and repeated starts are both discarded;
 * only the repeated start occurs after cancellation and is counted once in the
 * independent post-cancel field.
 */
TEST(I2Evidence, PostCancelDuplicateCountsOnceInEachWasteDimension) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  cancel_i2_final(&input, 0U);
  const auto original = std::find_if(
      input.observations.service_starts.begin(),
      input.observations.service_starts.end(),
      [](const I2ObservedServiceStart& event) {
        return event.child.edit_index == 0U &&
               event.child.quality == compute::ComputeRunQuality::Full;
      });
  ASSERT_NE(original, input.observations.service_starts.end());
  I2ObservedServiceStart duplicate = *original;
  duplicate.observed_at =
      checked_i1_time_add(input.edits[0U].admission_sample, 6750us);
  append_i2_service_start_before_terminal(&input, std::move(duplicate));

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.service.all_started_service, 6400U);
  EXPECT_EQ(row.service.discarded_started_service, 800U);
  EXPECT_EQ(row.service.post_cancel_started_service, 400U);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves duplicate task retries can fail measured replicate waste.
 * @throws Synthetic evidence allocation or checked arithmetic failures fail
 * the test.
 * @note Six exact repeats of one 400-unit visible final task yield a measured
 * discarded ratio above 0.25 without introducing post-cancel work.
 */
TEST(I2Evidence, MeasuredDuplicateTaskRetriesFailReplicateWasteRatio) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  const I2ObservedServiceStart original =
      input.observations.service_starts.back();
  for (std::size_t duplicate_index = 0U; duplicate_index < 6U;
       ++duplicate_index) {
    append_i2_service_start_before_terminal(&input, original);
  }
  const I2EpisodeInnerRow duplicate_row = evaluate_i2_episode(std::move(input));
  ASSERT_TRUE(duplicate_row.validity_reasons.empty());
  ASSERT_TRUE(duplicate_row.service.discarded_ratio.has_value());
  EXPECT_GT(*duplicate_row.service.discarded_ratio,
            kI2DiscardedServiceRatioLimit);

  std::vector<I2EpisodeInnerRow> rows = make_passing_i2_aggregate_rows();
  for (std::size_t slot = kI2WarmupSlotCount + 1U; slot < kI2GridSlotCount;
       ++slot) {
    rows[slot].service = duplicate_row.service;
  }
  const I2ReplicateSummary summary = evaluate_i2_replicate(rows);

  ASSERT_TRUE(summary.measured_service.discarded_ratio.has_value());
  EXPECT_GT(*summary.measured_service.discarded_ratio,
            kI2DiscardedServiceRatioLimit);
  EXPECT_EQ(summary.measured_service.post_cancel_started_service, 0U);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves a preview-only materialization settles Host success exactly.
 * @throws Nothing when the single successful child remains causally closed.
 */
TEST(I2Evidence, PreviewOnlyHostSettlementMatchesSuccessfulOutcome) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  erase_i2_materialized_child(&input, 0U, compute::ComputeRunQuality::Full);

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves a successful preview plus cancelled final settles Host failure.
 * @throws Nothing when cancellation, terminal, resource, and Host order close.
 */
TEST(I2Evidence, CancelledFinalHostSettlementMatchesFailedOutcome) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  cancel_i2_final(&input, 0U);

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves successful progressive children reject failed Host status.
 * @throws Nothing when the status contradiction makes every row axis Invalid.
 */
TEST(I2Evidence, SuccessfulChildrenRejectFailedHostSettlement) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  input.edits[0U].settlement_status->ok = false;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(has_i2_reason(
      row, "I2 Host settlement contradicts progressive child outcomes"));
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves a cancelled final rejects successful Host settlement status.
 * @throws Nothing when the status contradiction makes every row axis Invalid.
 */
TEST(I2Evidence, CancelledFinalRejectsSuccessfulHostSettlement) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  cancel_i2_final(&input, 0U);
  input.edits[0U].settlement_status->ok = true;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(has_i2_reason(
      row, "I2 Host settlement contradicts progressive child outcomes"));
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves no materialized child maps to failed Host product status.
 * @throws Nothing when missing preview remains independently workload-Invalid.
 * @note The frozen workload still requires an early preview before the next
 * edit; this test isolates Host aggregation without fabricating child outcome.
 */
TEST(I2Evidence, NoMaterializedChildRequiresFailedHostSettlement) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  erase_i2_materialized_child(&input, 0U,
                              compute::ComputeRunQuality::Interactive);
  erase_i2_materialized_child(&input, 0U, compute::ComputeRunQuality::Full);
  input.edits[0U].settlement_status->ok = false;

  const I2EpisodeInnerRow legal_status = evaluate_i2_episode(input);
  EXPECT_TRUE(has_i2_reason(
      legal_status, "I2 early edit did not materialize a preview child"));
  EXPECT_FALSE(has_i2_reason(
      legal_status,
      "I2 Host settlement contradicts progressive child outcomes"));

  input.edits[0U].settlement_status->ok = true;
  const I2EpisodeInnerRow contradictory_status =
      evaluate_i2_episode(std::move(input));
  EXPECT_TRUE(has_i2_reason(
      contradictory_status,
      "I2 Host settlement contradicts progressive child outcomes"));
}

/**
 * @brief Proves Host causal sequence must follow final resource settlement.
 * @throws Nothing when swapping two unique sequences fails closed.
 */
TEST(I2Evidence, HostSequenceBeforeChildResourceSettlementIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  auto host = std::find_if(input.observations.host_settlements.begin(),
                           input.observations.host_settlements.end(),
                           [](const I1ObservedHostSettlement& event) {
                             return event.edit_index == 0U;
                           });
  auto resource = std::find_if(
      input.observations.resource_settlements.begin(),
      input.observations.resource_settlements.end(),
      [](const I2ObservedRunLifecycleTransition& event) {
        return event.child.edit_index == 0U &&
               event.child.quality == compute::ComputeRunQuality::Full;
      });
  ASSERT_NE(host, input.observations.host_settlements.end());
  ASSERT_NE(resource, input.observations.resource_settlements.end());
  std::swap(host->causal_sequence, resource->causal_sequence);

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(has_i2_reason(
      row, "I2 Host settlement did not follow all child resource settlements"));
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves Host steady time cannot precede child resource settlement.
 * @throws Nothing when sequence remains legal but time fails closed.
 */
TEST(I2Evidence, HostTimeBeforeChildResourceSettlementIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  auto host = std::find_if(input.observations.host_settlements.begin(),
                           input.observations.host_settlements.end(),
                           [](const I1ObservedHostSettlement& event) {
                             return event.edit_index == 0U;
                           });
  const auto resource = std::find_if(
      input.observations.resource_settlements.begin(),
      input.observations.resource_settlements.end(),
      [](const I2ObservedRunLifecycleTransition& event) {
        return event.child.edit_index == 0U &&
               event.child.quality == compute::ComputeRunQuality::Full;
      });
  ASSERT_NE(host, input.observations.host_settlements.end());
  ASSERT_NE(resource, input.observations.resource_settlements.end());
  host->observed_at = resource->observed_at - 1us;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(has_i2_reason(
      row, "I2 Host settlement did not follow all child resource settlements"));
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves synchronized preview expected/candidate forgery is Invalid.
 * @throws Nothing when the frozen preview oracle remains authoritative.
 */
TEST(I2Evidence, SynchronizedPreviewDigestForgeryIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  const ContentDigest forged =
      forge_i2_digest(i2_frozen_preview_content_digest());
  input.expected_preview_digest = forged;
  I2ObservedVisibleOutput* preview =
      find_i2_endpoint(&input, compute::ComputeRunQuality::Interactive);
  ASSERT_NE(preview, nullptr);
  ASSERT_TRUE(preview->content_digest.has_value());
  ASSERT_TRUE(preview->content_digest->digest.has_value());
  preview->content_digest->digest = forged;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves synchronized final expected/candidate forgery is Invalid.
 * @throws Nothing when the frozen I1 final oracle remains authoritative.
 */
TEST(I2Evidence, SynchronizedFinalDigestForgeryIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  const ContentDigest forged =
      forge_i2_digest(i1_frozen_final_content_digest());
  input.expected_final_digest = forged;
  I2ObservedVisibleOutput* final =
      find_i2_endpoint(&input, compute::ComputeRunQuality::Full);
  ASSERT_NE(final, nullptr);
  ASSERT_TRUE(final->content_digest.has_value());
  ASSERT_TRUE(final->content_digest->digest.has_value());
  final->content_digest->digest = forged;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves a candidate-only preview digest mismatch is Fail.
 * @throws Nothing when complete output evidence remains independently valid.
 */
TEST(I2Evidence, CandidateOnlyPreviewDigestMismatchIsFail) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  I2ObservedVisibleOutput* preview =
      find_i2_endpoint(&input, compute::ComputeRunQuality::Interactive);
  ASSERT_NE(preview, nullptr);
  ASSERT_TRUE(preview->content_digest.has_value());
  ASSERT_TRUE(preview->content_digest->digest.has_value());
  preview->content_digest->digest =
      forge_i2_digest(*preview->content_digest->digest);

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves a candidate-only final digest mismatch is Fail.
 * @throws Nothing when complete output evidence remains independently valid.
 */
TEST(I2Evidence, CandidateOnlyFinalDigestMismatchIsFail) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  I2ObservedVisibleOutput* final =
      find_i2_endpoint(&input, compute::ComputeRunQuality::Full);
  ASSERT_NE(final, nullptr);
  ASSERT_TRUE(final->content_digest.has_value());
  ASSERT_TRUE(final->content_digest->digest.has_value());
  final->content_digest->digest =
      forge_i2_digest(*final->content_digest->digest);

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves the repeated second Metal allocation cannot drift from reuse.
 * @throws Nothing when otherwise-valid available-Metal evidence fails closed.
 */
TEST(I2Evidence, MetalSecondAllocationDriftIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  ASSERT_FALSE(input.observations.visible_outputs.empty());
  auto& acquisition = input.observations.visible_outputs.front().acquisition;
  ASSERT_TRUE(acquisition.has_value());
  acquisition->metal = make_i2_metal_acquisition(acquisition->host_first);
  EXPECT_EQ(evaluate_i2_episode(input).output_verdict, I1Verdict::Pass);
  ASSERT_TRUE(acquisition->metal.second.has_value());
  acquisition->metal.second->allocation = acquisition->host_first.allocation;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_EQ(row.validity_reasons,
            std::vector<std::string>{
                "I2 Metal binding/allocation/storage-byte facts are not exact "
                "reuse"});
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves the repeated second Metal byte envelope cannot drift from
 * reuse.
 * @throws Nothing when otherwise-valid available-Metal evidence fails closed.
 */
TEST(I2Evidence, MetalSecondStorageBytesDriftIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  ASSERT_FALSE(input.observations.visible_outputs.empty());
  auto& acquisition = input.observations.visible_outputs.front().acquisition;
  ASSERT_TRUE(acquisition.has_value());
  acquisition->metal = make_i2_metal_acquisition(acquisition->host_first);
  EXPECT_EQ(evaluate_i2_episode(input).output_verdict, I1Verdict::Pass);
  ASSERT_TRUE(acquisition->metal.second.has_value());
  ++acquisition->metal.second->storage_bytes;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_EQ(row.validity_reasons,
            std::vector<std::string>{
                "I2 Metal binding/allocation/storage-byte facts are not exact "
                "reuse"});
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves a shared binding cannot drift from repeated Metal byte facts.
 * @throws Nothing when the adjusted Direct plan remains otherwise consistent.
 */
TEST(I2Evidence, MetalBindingByteSizeDriftIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  ASSERT_FALSE(input.observations.visible_outputs.empty());
  auto& acquisition = input.observations.visible_outputs.front().acquisition;
  ASSERT_TRUE(acquisition.has_value());
  acquisition->metal = make_i2_metal_acquisition(acquisition->host_first);
  EXPECT_EQ(evaluate_i2_episode(input).output_verdict, I1Verdict::Pass);
  ASSERT_TRUE(acquisition->metal.first.has_value());
  ASSERT_TRUE(acquisition->metal.second.has_value());
  auto& first = *acquisition->metal.first;
  auto& second = *acquisition->metal.second;
  ++first.binding.byte_size;
  second.binding = first.binding;
  second.plan =
      AccessPlan{AccessPlanKind::Direct,
                 second.revision.value(),
                 second.binding,
                 AccessTarget{DeviceId(DeviceBackend::Metal),
                              MemoryDomain::DeviceLocal, false, false},
                 VisibilityObligations{},
                 0U};

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_EQ(row.validity_reasons,
            std::vector<std::string>{
                "I2 Metal binding/allocation/storage-byte facts are not exact "
                "reuse"});
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves the second Metal reuse cannot add transfer or allocation work.
 * @throws Nothing when otherwise-valid diagnostics fail output closed.
 */
TEST(I2Evidence, MetalSecondReuseCounterDriftIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  ASSERT_FALSE(input.observations.visible_outputs.empty());
  auto& acquisition = input.observations.visible_outputs.front().acquisition;
  ASSERT_TRUE(acquisition.has_value());
  acquisition->metal = make_i2_metal_acquisition(acquisition->host_first);
  EXPECT_EQ(evaluate_i2_episode(input).output_verdict, I1Verdict::Pass);
  ASSERT_TRUE(acquisition->metal.after_second.has_value());
  ++acquisition->metal.after_second->submission_count;
  ++acquisition->metal.after_second->total_allocations;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(has_i2_reason(
      row, "I2 Metal executor counters do not prove one transfer then reuse"));
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves second reuse cannot change retained device-memory ownership.
 * @throws Nothing when otherwise-valid resource evidence fails output closed.
 */
TEST(I2Evidence, MetalSecondReuseDeviceMemoryDriftIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  ASSERT_FALSE(input.observations.visible_outputs.empty());
  auto& acquisition = input.observations.visible_outputs.front().acquisition;
  ASSERT_TRUE(acquisition.has_value());
  acquisition->metal = make_i2_metal_acquisition(acquisition->host_first);
  EXPECT_EQ(evaluate_i2_episode(input).output_verdict, I1Verdict::Pass);
  ASSERT_TRUE(acquisition->metal.resources_after_second.has_value());
  acquisition->metal.resources_after_second->reserved.device_memory_bytes =
      acquisition->host_first.storage_bytes;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(has_i2_reason(
      row, "I2 Metal resources changed during reuse or retained scratch"));
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves final device-memory reservation must equal its row baseline.
 * @throws Nothing when memory alone becomes Fail with other axes independent.
 * @note Scratch remains unchanged, so this regression detects the previously
 * omitted persistent device-memory component specifically.
 */
TEST(I2Evidence, DeviceMemoryReservationDriftIsMemoryFailure) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  ResourceLedger::DeviceSnapshot baseline;
  baseline.device = DeviceId(DeviceBackend::Metal);
  baseline.limits = DeviceResourceVector{1024U, 512U};
  baseline.available = baseline.limits;
  input.baseline.device_resources.push_back(baseline);
  ResourceLedger::DeviceSnapshot final = baseline;
  final.reserved.device_memory_bytes = 16U;
  final.high_water.device_memory_bytes = 16U;
  final.available.device_memory_bytes -= 16U;
  input.final_snapshot.device_resources.push_back(final);

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_TRUE(row.validity_reasons.empty());
  EXPECT_FALSE(row.memory_settled);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Fail);
  EXPECT_EQ(row.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves cross-event child drift invalidates otherwise complete data.
 * @throws Nothing when descriptor joins remain exact and fail-closed.
 */
TEST(I2Evidence, CrossEventChildDescriptorDriftIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  input.observations.service_starts.front().child.qos.weight = 2U;

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves a Cancelled terminal cannot pass without cancellation proof.
 * @throws Nothing when the evaluator rejects the otherwise complete episode.
 * @note The synthetic final was triggered and started before cancellation and
 * has no visible output. Before terminal/cancellation cardinality validation,
 * this shape could still produce Pass on all four independent verdicts.
 */
TEST(I2Evidence, CancelledTerminalWithoutCancellationEvidenceIsInvalid) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  auto terminal = std::find_if(
      input.observations.terminals.begin(), input.observations.terminals.end(),
      [](const I2ObservedTerminal& event) {
        return event.child.edit_index == 0U &&
               event.child.quality == compute::ComputeRunQuality::Full;
      });
  ASSERT_NE(terminal, input.observations.terminals.end());
  const std::uint64_t run_id = terminal->child.run_id;
  terminal->kind = compute::ComputeRunTerminalKind::Cancelled;
  input.observations.visible_outputs.erase(
      std::remove_if(input.observations.visible_outputs.begin(),
                     input.observations.visible_outputs.end(),
                     [run_id](const I2ObservedVisibleOutput& output) {
                       return output.child.run_id == run_id;
                     }),
      input.observations.visible_outputs.end());

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "I2 terminal/cancellation cardinality is not exactly "
                      "one-to-one"),
            row.validity_reasons.end());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves accepted generation causally precedes every child event.
 * @throws Nothing when swapping only two causal sequences invalidates the row.
 */
TEST(I2Evidence, CurrentGenerationMustPrecedePreviewServiceStart) {
  I2EpisodeEvidenceInput input = make_valid_i2_input(0U);
  ASSERT_FALSE(input.observations.current_generations.empty());
  ASSERT_FALSE(input.observations.service_starts.empty());
  ASSERT_EQ(input.observations.current_generations.front().edit_index,
            input.observations.service_starts.front().child.edit_index);
  std::swap(input.observations.current_generations.front().causal_sequence,
            input.observations.service_starts.front().causal_sequence);

  const I2EpisodeInnerRow row = evaluate_i2_episode(std::move(input));

  EXPECT_NE(std::find(row.validity_reasons.begin(), row.validity_reasons.end(),
                      "I2 child event does not follow its current generation"),
            row.validity_reasons.end());
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves malformed extreme-time input becomes Invalid without escaping.
 * @throws Nothing when checked evaluator arithmetic remains fail-closed.
 */
TEST(I2Evidence, OverflowingRawEvidenceIsInvalidWithoutThrowing) {
  I2EpisodeEvidenceInput input;
  input.replicate_ordinal = 1U;
  input.slot = 0U;
  input.grid_origin = std::chrono::steady_clock::time_point::max();
  input.episode_origin = input.grid_origin;
  input.terminal_boundary = input.grid_origin;

  std::optional<I2EpisodeInnerRow> row;
  EXPECT_NO_THROW(row = evaluate_i2_episode(std::move(input)));
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(row->latency_verdict, I1Verdict::Invalid);
  EXPECT_FALSE(row->validity_reasons.empty());
}

/**
 * @brief Proves measured-only nearest-rank aggregation and thresholds.
 * @throws Nothing when the exact 111-slot row schema remains stable.
 */
TEST(I2Evidence, ReplicateUsesOnlyOneHundredMeasuredSlots) {
  const std::vector<I2EpisodeInnerRow> rows = make_i2_aggregate_rows();

  const I2ReplicateSummary summary = evaluate_i2_replicate(rows);

  ASSERT_TRUE(summary.latency.has_value());
  EXPECT_EQ(summary.measured_sample_count, kI2MeasuredSlotCount);
  EXPECT_EQ(summary.latency->preview_p50, 50ms);
  EXPECT_EQ(summary.latency->preview_p95, 95ms);
  EXPECT_EQ(summary.latency->preview_p99, 99ms);
  EXPECT_EQ(summary.latency->final_p95, 190ms);
  EXPECT_EQ(summary.latency_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves cold/warmup latency and waste Fail do not enter aggregates.
 * @throws Nothing when only the 100 measured rows supply samples and service.
 */
TEST(I2Evidence, ReplicateIgnoresColdWarmupLatencyWasteFailures) {
  std::vector<I2EpisodeInnerRow> rows = make_passing_i2_aggregate_rows();
  for (std::size_t slot = 0U; slot <= kI2WarmupSlotCount; ++slot) {
    rows[slot].latency_verdict = I1Verdict::Fail;
    rows[slot].waste_verdict = I1Verdict::Fail;
    rows[slot].latencies.preview = 10s;
    rows[slot].latencies.final = 10s;
    rows[slot].service = I1ServiceEvidence{100000U, 100000U, 100000U, 1.0};
  }

  const I2ReplicateSummary summary = evaluate_i2_replicate(rows);

  EXPECT_EQ(summary.measured_sample_count, kI2MeasuredSlotCount);
  EXPECT_EQ(summary.measured_service.all_started_service, 10000U);
  EXPECT_EQ(summary.measured_service.discarded_started_service, 1000U);
  EXPECT_EQ(summary.measured_service.post_cancel_started_service, 0U);
  EXPECT_EQ(summary.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves non-measured latency/waste Invalid still fails closed.
 * @throws Nothing when cold and warmup structural invalidity propagates.
 */
TEST(I2Evidence, ReplicatePropagatesColdWarmupLatencyWasteInvalid) {
  std::vector<I2EpisodeInnerRow> rows = make_passing_i2_aggregate_rows();
  rows.front().latency_verdict = I1Verdict::Invalid;
  rows[kI2WarmupSlotCount].waste_verdict = I1Verdict::Invalid;

  const I2ReplicateSummary summary = evaluate_i2_replicate(rows);

  EXPECT_EQ(summary.measured_sample_count, kI2MeasuredSlotCount);
  EXPECT_EQ(summary.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves measured latency/waste Fail still reaches both summaries.
 * @throws Nothing when measured row verdicts remain authoritative.
 */
TEST(I2Evidence, ReplicatePropagatesMeasuredLatencyWasteFailures) {
  std::vector<I2EpisodeInnerRow> rows = make_passing_i2_aggregate_rows();
  constexpr std::size_t kFirstMeasuredSlot = kI2WarmupSlotCount + 1U;
  rows[kFirstMeasuredSlot].latency_verdict = I1Verdict::Fail;
  rows[kFirstMeasuredSlot].waste_verdict = I1Verdict::Fail;

  const I2ReplicateSummary summary = evaluate_i2_replicate(rows);

  EXPECT_EQ(summary.latency_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves memory and output continue to consume all 111 phase rows.
 * @throws Nothing when non-measured failures remain visible on those axes.
 */
TEST(I2Evidence, ReplicateUsesAllPhasesForMemoryAndOutput) {
  std::vector<I2EpisodeInnerRow> rows = make_passing_i2_aggregate_rows();
  rows.front().memory_verdict = I1Verdict::Fail;
  rows[1U].output_verdict = I1Verdict::Fail;

  const I2ReplicateSummary summary = evaluate_i2_replicate(rows);

  EXPECT_EQ(summary.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves one row-local-valid translated grid cannot join a replicate.
 * @throws Nothing when all 111 individual rows evaluate before aggregation.
 */
TEST(I2Evidence, ReplicateRejectsOneTranslatedRowLocalGrid) {
  const auto common_grid_origin = std::chrono::steady_clock::time_point(1s);
  const auto translated_grid_origin = std::chrono::steady_clock::time_point(2s);
  constexpr std::size_t kTranslatedSlot = 57U;
  std::vector<I2EpisodeInnerRow> rows;
  rows.reserve(kI2GridSlotCount);
  for (std::size_t slot = 0U; slot < kI2GridSlotCount; ++slot) {
    const auto grid_origin =
        slot == kTranslatedSlot ? translated_grid_origin : common_grid_origin;
    I2EpisodeInnerRow row =
        evaluate_i2_episode(make_valid_i2_input(slot, grid_origin));
    ASSERT_TRUE(row.validity_reasons.empty()) << "slot=" << slot;
    ASSERT_EQ(row.latency_verdict, I1Verdict::Pass) << "slot=" << slot;
    ASSERT_EQ(row.waste_verdict, I1Verdict::Pass) << "slot=" << slot;
    ASSERT_EQ(row.memory_verdict, I1Verdict::Pass) << "slot=" << slot;
    ASSERT_EQ(row.output_verdict, I1Verdict::Pass) << "slot=" << slot;
    rows.push_back(std::move(row));
  }

  const I2ReplicateSummary summary = evaluate_i2_replicate(rows);

  EXPECT_EQ(summary.validity_reasons,
            std::vector<std::string>{
                "I2 replicate grid origin/episode origins/terminal boundary "
                "do not form one checked 111-slot grid"});
  EXPECT_EQ(summary.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves replicate checked-time overflow is captured as Invalid.
 * @throws Nothing when the evaluator preserves its fail-closed boundary.
 */
TEST(I2Evidence, ReplicateGridOverflowIsInvalidWithoutThrowing) {
  std::vector<I2EpisodeInnerRow> rows = make_i2_aggregate_rows();
  for (I2EpisodeInnerRow& row : rows) {
    row.evidence.grid_origin = std::chrono::steady_clock::time_point::max();
    row.evidence.episode_origin = row.evidence.grid_origin;
    row.evidence.terminal_boundary = row.evidence.grid_origin;
  }

  std::optional<I2ReplicateSummary> summary;
  EXPECT_NO_THROW(summary = evaluate_i2_replicate(rows));
  ASSERT_TRUE(summary.has_value());
  EXPECT_EQ(summary->validity_reasons,
            std::vector<std::string>{
                "I2 replicate grid origin/episode origins/terminal boundary "
                "do not form one checked 111-slot grid"});
  EXPECT_EQ(summary->latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary->waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary->memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary->output_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves serialization retains the closed child/access evidence shape.
 * @throws Nothing when complete JSON construction and field lookup succeed.
 */
TEST(I2Evidence, JsonRetainsClosedVisibleAcquisitionFacts) {
  const I2EpisodeInnerRow row = evaluate_i2_episode(make_valid_i2_input(0U));

  const nlohmann::json encoded = i2_inner_row_json(row);

  EXPECT_EQ(encoded.at("schema"), kI2InnerRowSchema);
  EXPECT_FALSE(encoded.at("outer_canonical_envelope_claim").get<bool>());
  const auto& visible = encoded.at("observations").at("visible_outputs");
  ASSERT_EQ(visible.size(), kI1EditCount * 2U);
  EXPECT_TRUE(visible.front().at("value_valid_at_capture").get<bool>());
  EXPECT_FALSE(
      visible.front().at("payload_retained_at_serialization").get<bool>());
  EXPECT_EQ(visible.front()
                .at("acquisition")
                .at("host_first")
                .at("plan")
                .at("transfer_bytes"),
            0U);
  EXPECT_FALSE(visible.front()
                   .at("acquisition")
                   .at("metal")
                   .at("available")
                   .get<bool>());
}

}  // namespace
}  // namespace ps::benchmark
