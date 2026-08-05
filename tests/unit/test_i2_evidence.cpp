#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/i2_evidence.hpp"
#include "photospider/data/value.hpp"
#include "verification/i2_evidence_json.hpp"

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
