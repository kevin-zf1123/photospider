/**
 * @file test_m1_profile.cpp
 * @brief Verifies deterministic M1 timeline, fairness, and observer contracts.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/m1_canonical.hpp"        // NOLINT(build/include_subdir)
#include "benchmark/m1_evidence.hpp"         // NOLINT(build/include_subdir)
#include "benchmark/m1_profile.hpp"          // NOLINT(build/include_subdir)
#include "benchmark/observation_fanout.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/value.hpp"
#include "support/b1_test_environment.hpp"
#include "support/m1_test_evidence.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Pauses one deterministic observer callback after slot claim.
 * @throws Nothing for construction, destruction, or callback dispatch.
 * @note This deliberately blocking hook is test-only and must never be wired
 * into a production benchmark collector.
 */
class PausingPublicationHook final : public M1ObservationPublicationHook {
 public:
  /** @copydoc M1ObservationPublicationHook::after_slot_claim */
  void after_slot_claim() noexcept override {
    claimed_.store(true, std::memory_order_release);
    while (!released_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  /**
   * @brief Tests whether the callback reached the post-claim hook.
   * @return True after the callback has claimed its unique slot.
   * @throws Nothing.
   */
  bool claimed() const noexcept {
    return claimed_.load(std::memory_order_acquire);
  }

  /**
   * @brief Releases the paused callback to publish and complete.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept { released_.store(true, std::memory_order_release); }

 private:
  /** @brief True after the callback claims its slot. */
  std::atomic<bool> claimed_{false};

  /** @brief True when the callback may finish publication. */
  std::atomic<bool> released_{false};
};

/**
 * @brief Forces one coordinate sampler to overlap a contending reservation.
 * @throws Nothing for construction, destruction, or hook dispatch.
 * @note The first sampler pauses while holding the test-only coordinate gate;
 * the first contending reservation records the interleave and releases it.
 */
class PausingCoordinateSampleHook final : public M1ObservationPublicationHook {
 public:
  /** @copydoc M1ObservationPublicationHook::after_coordinate_sample */
  void after_coordinate_sample() noexcept override {
    if (sampled_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    while (!released_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    sample_finished_.store(true, std::memory_order_release);
  }

  /** @copydoc M1ObservationPublicationHook::after_coordinate_contention */
  void after_coordinate_contention() noexcept override {
    contention_.store(true, std::memory_order_release);
    released_.store(true, std::memory_order_release);
    while (!sample_finished_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  /** @copydoc M1ObservationPublicationHook::after_slot_claim */
  void after_slot_claim() noexcept override {}

  /**
   * @brief Tests whether the first reservation sampled while holding the gate.
   * @return True after the deterministic pause was entered.
   * @throws Nothing.
   */
  bool sampled() const noexcept {
    return sampled_.load(std::memory_order_acquire);
  }

  /**
   * @brief Tests whether another reservation encountered the occupied gate.
   * @return True after the deterministic contention interleave occurred.
   * @throws Nothing.
   */
  bool contention() const noexcept {
    return contention_.load(std::memory_order_acquire);
  }

  /**
   * @brief Releases the first sampler during defensive test cleanup.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept { released_.store(true, std::memory_order_release); }

 private:
  /** @brief True after the first coordinate time sample. */
  std::atomic<bool> sampled_{false};

  /** @brief True after another reservation observes the occupied gate. */
  std::atomic<bool> contention_{false};

  /** @brief True when the first sampler may finish sequence reservation. */
  std::atomic<bool> released_{false};

  /** @brief True after the first sampler leaves its deterministic pause. */
  std::atomic<bool> sample_finished_{false};
};

/**
 * @brief Holds one coordinate owner through the historical contention limit.
 *
 * @throws Nothing for construction, destruction, or hook dispatch.
 * @note One contender releases the owner only on its 4096th failed gate
 * observation. The old implementation then misclassified ordinary contention
 * as sticky numeric sequence exhaustion before attempting the now-free gate.
 */
class CoordinateContentionLimitHook final
    : public M1ObservationPublicationHook {
 public:
  /** @brief Historical attempt count that exposed false exhaustion. */
  static constexpr std::uint64_t kHistoricalAttemptLimit = 4096U;

  /** @copydoc M1ObservationPublicationHook::after_coordinate_sample */
  void after_coordinate_sample() noexcept override {
    if (sampled_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    while (!released_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    sample_finished_.store(true, std::memory_order_release);
  }

  /** @copydoc M1ObservationPublicationHook::after_coordinate_contention */
  void after_coordinate_contention() noexcept override {
    const std::uint64_t count =
        contention_count_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    if (count != kHistoricalAttemptLimit) {
      return;
    }
    released_.store(true, std::memory_order_release);
    while (!sample_finished_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  /** @copydoc M1ObservationPublicationHook::after_slot_claim */
  void after_slot_claim() noexcept override {}

  /**
   * @brief Tests whether the first reservation owns the paused sample gate.
   * @return True after its first steady-clock sample.
   * @throws Nothing.
   */
  bool sampled() const noexcept {
    return sampled_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns the exact number of observed failed gate acquisitions.
   * @return Monotonic contention callback count.
   * @throws Nothing.
   */
  std::uint64_t contention_count() const noexcept {
    return contention_count_.load(std::memory_order_acquire);
  }

  /**
   * @brief Releases the first sampler during defensive test cleanup.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept { released_.store(true, std::memory_order_release); }

 private:
  /** @brief True after the first reservation samples steady time. */
  std::atomic<bool> sampled_{false};

  /** @brief True when the first reservation may clear the sampling gate. */
  std::atomic<bool> released_{false};

  /** @brief True after the first reservation is ready to clear the gate. */
  std::atomic<bool> sample_finished_{false};

  /** @brief Number of contending atomic-flag observations. */
  std::atomic<std::uint64_t> contention_count_{0U};
};

/**
 * @brief Builds one complete passing inner-fairness input.
 * @return Exact 30-window evidence with every independent guard passing.
 * @throws std::bad_alloc when vector storage allocates.
 */
M1FairnessEvidenceInput make_passing_fairness_input() {
  M1FairnessEvidenceInput input;
  input.paired_isolated_b1 =
      M1PairedB1RateEvidence{1000000U, std::chrono::seconds(1)};
  for (std::size_t window = 0U; window < kM1MeasuredWindowCount; ++window) {
    input.progress_windows.push_back(
        M1ThroughputProgressSample{window, 200000U, std::chrono::seconds(1)});
    input.graph_service_windows.push_back(
        M1GraphServiceWindow{window, true, 100U, 100U});
  }
  for (std::size_t group = 0U; group < 3U; ++group) {
    input.class_starts.push_back(
        M1ClassStartSample{group + 1U, compute::ComputeRunQosClass::Interactive,
                           true, true, true});
  }
  input.class_starts.push_back(M1ClassStartSample{
      4U, compute::ComputeRunQosClass::Throughput, true, true, true});
  input.headroom_admissions = M1HeadroomAdmissionEvidence{
      kM1MeasuredI1AttemptCount, kM1MeasuredI1AttemptCount, 0U};
  for (std::size_t origin = 0U; origin < kM1MeasuredI1OriginCount; ++origin) {
    for (std::size_t edit = 0U; edit < kI1EditCount; ++edit) {
      input.headroom_outcomes.push_back(M1HeadroomAdmissionOutcome{
          origin, edit, true, OperationStatus{}, false});
    }
  }
  input.interactive_latency_verdict = I1Verdict::Pass;
  return input;
}

/**
 * @brief Builds one valid Run submission for direct observer-boundary tests.
 * @param graph_identity Unique Graph label copied into the Run descriptor.
 * @param graph_instance_id Nonzero Graph instance/revision test identity.
 * @param service_class Actual immutable scheduling class to observe.
 * @return Complete full-quality high-precision submission without a sink.
 * @throws std::bad_alloc when Graph identity ownership allocates.
 */
compute::ComputeRunSubmission make_observer_submission(
    std::string graph_identity, std::uint64_t graph_instance_id,
    compute::ComputeRunQosClass service_class) {
  return compute::ComputeRunSubmission{
      std::move(graph_identity),
      GraphInstanceId{graph_instance_id},
      GraphRevision{graph_instance_id},
      4,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{service_class, std::nullopt, 1U, 1U},
      compute::SupersessionIdentity{
          compute::SupersessionKey(4, ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1U)},
      nullptr};
}

/**
 * @brief Fixed-storage spy for same-coordinate fanout boundary tests.
 *
 * Every callback records only scalar counts and the last coordinate, keeping
 * the `noexcept` observation boundary allocation-free and nonblocking.
 *
 * @throws Nothing after construction.
 */
class RecordingObservationSink final
    : public compute::ComputeRunObservationSink {
 public:
  /**
   * @brief Binds the coordinate returned by this spy's allocator.
   * @param coordinate Immutable reservation result.
   * @param task_semantics Whether this child requests task callbacks.
   * @throws Nothing.
   */
  RecordingObservationSink(compute::ComputeRunObservationCoordinate coordinate,
                           bool task_semantics) noexcept
      : coordinate_(coordinate), task_semantics_(task_semantics) {}

  /** @copydoc compute::ComputeRunObservationSink::reserve_causal_coordinate */
  compute::ComputeRunObservationCoordinate reserve_causal_coordinate() noexcept
      override {
    ++reservation_count;
    return coordinate_;
  }

  /** @copydoc compute::ComputeRunObservationSink::abort_causal_coordinate */
  void abort_causal_coordinate(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    ++abort_count;
    last_coordinate = coordinate;
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_generation */
  void on_current_generation(
      const compute::SupersessionIdentity& identity,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(identity);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::observes_task_semantics */
  bool observes_task_semantics() const noexcept override {
    return task_semantics_;
  }

  /** @copydoc compute::ComputeRunObservationSink::on_task_ready */
  void on_task_ready(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      const compute::ComputeRunTaskReadyObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(task_identity);
    static_cast<void>(observation);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_service_start */
  void on_service_start(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      std::uint64_t service_charge,
      const compute::ComputeRunServiceStartObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(task_identity);
    static_cast<void>(service_charge);
    static_cast<void>(observation);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_task_terminal */
  void on_task_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      compute::ComputeRunTaskTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(task_identity);
    static_cast<void>(kind);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
  void on_cancellation(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunCancellationReason reason,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(reason);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_terminal */
  void on_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(kind);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
  void on_current_visible(
      const compute::ComputeRunDescriptor& descriptor, Value output,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    static_cast<void>(output);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
  void on_run_quiescent(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
  void on_run_resource_settled(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    static_cast<void>(descriptor);
    record(coordinate);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_host_settled */
  void on_host_settled(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(coordinate);
  }

  /** @brief Number of times this child was asked to allocate a coordinate. */
  std::uint64_t reservation_count = 0U;

  /** @brief Number of callbacks forwarded to this child. */
  std::uint64_t callback_count = 0U;

  /** @brief Number of abandoned reservations forwarded to this child. */
  std::uint64_t abort_count = 0U;

  /** @brief Last exact coordinate received through a callback. */
  compute::ComputeRunObservationCoordinate last_coordinate;

 private:
  /**
   * @brief Records one scalar callback without allocation or blocking.
   * @param coordinate Exact coordinate supplied by the fanout.
   * @return Nothing.
   * @throws Nothing.
   */
  void record(compute::ComputeRunObservationCoordinate coordinate) noexcept {
    ++callback_count;
    last_coordinate = coordinate;
  }

  /** @brief Fixed coordinate returned by the spy allocator. */
  compute::ComputeRunObservationCoordinate coordinate_;

  /** @brief Fixed task-semantics opt-in used by composition tests. */
  bool task_semantics_ = false;
};

/**
 * @brief Closed product callback set forwarded by the observation fanout.
 * @throws Nothing for value construction and comparison.
 * @note Reservation and abort lifecycle calls are deliberately excluded
 * because they belong only to the sequence authority.
 */
enum class FanoutProductEvent : std::uint8_t {
  /** @brief Accepted current-generation publication. */
  CurrentGeneration,
  /** @brief Dependency-ready task materialization. */
  TaskReady,
  /** @brief Physically committed callback service start. */
  ServiceStart,
  /** @brief Entered task terminal publication. */
  TaskTerminal,
  /** @brief Accepted Run cancellation. */
  Cancellation,
  /** @brief Exactly-once Run terminal publication. */
  Terminal,
  /** @brief Current-visible immutable Value publication. */
  CurrentVisible,
  /** @brief Progressive final permission consumption. */
  ProgressiveFinalTriggered,
  /** @brief Physical Run quiescence. */
  RunQuiescent,
  /** @brief Exact Run root-resource settlement. */
  RunResourceSettled,
  /** @brief Caller-visible Host settlement. */
  HostSettled,
};

/** @brief Exact number of product callback kinds forwarded by the fanout. */
inline constexpr std::size_t kFanoutProductEventCount = 11U;

/**
 * @brief Identifies which fanout child entered one recorded callback.
 * @throws Nothing for value construction and comparison.
 */
enum class FanoutRecipient : std::uint8_t {
  /** @brief Workload-specific same-coordinate mirror. */
  Mirror,
  /** @brief Shared coordinate authority and completion barrier. */
  SequenceAuthority,
};

/**
 * @brief Fixed scalar record of one fanout child callback entry.
 * @throws Nothing for value construction and copying.
 */
struct FanoutDeliveryRecord final {
  /** @brief Product event whose callback entered the child. */
  FanoutProductEvent event = FanoutProductEvent::CurrentGeneration;

  /** @brief Child role receiving the callback. */
  FanoutRecipient recipient = FanoutRecipient::Mirror;

  /** @brief Exact authority-owned coordinate delivered to the child. */
  compute::ComputeRunObservationCoordinate coordinate;

  /** @brief Whether a current-visible callback received a valid Value. */
  bool value_valid = false;

  /** @brief Current-visible Value revision, or the invalid sentinel. */
  ValueRevisionId value_revision;
};

/**
 * @brief Allocation-free ordered log shared by two fanout spy children.
 * @throws Nothing after construction.
 * @note Tests serialize callback entry through one fanout thread. Readers
 * inspect either after worker join or while the writer is stopped at the
 * release/acquire-synchronized current-visible barrier.
 */
class FanoutDeliveryLog final {
 public:
  /**
   * @brief Appends one child callback entry to fixed storage.
   * @param event Product event entering the child.
   * @param recipient Child role receiving the event.
   * @param coordinate Exact authority-owned coordinate.
   * @param output Optional borrowed current-visible Value.
   * @return Nothing.
   * @throws Nothing; excess entries set a sticky overflow flag.
   */
  void record(FanoutProductEvent event, FanoutRecipient recipient,
              compute::ComputeRunObservationCoordinate coordinate,
              const Value* output = nullptr) noexcept {
    if (size_ >= records_.size()) {
      overflowed_ = true;
      return;
    }
    const bool value_valid = output != nullptr && output->valid();
    records_[size_] = FanoutDeliveryRecord{
        event, recipient, coordinate, value_valid,
        value_valid ? output->revision_id() : ValueRevisionId{}};
    ++size_;
  }

  /**
   * @brief Returns the number of retained callback entries.
   * @return Count in `[0, 22]`.
   * @throws Nothing.
   */
  std::size_t size() const noexcept { return size_; }

  /**
   * @brief Returns one retained callback entry.
   * @param index Zero-based index strictly below `size()`.
   * @return Immutable record stored at `index`.
   * @throws Nothing.
   * @note The caller must validate the index before access.
   */
  const FanoutDeliveryRecord& at(std::size_t index) const noexcept {
    return records_[index];
  }

  /**
   * @brief Reports whether fixed callback storage was exceeded.
   * @return True after the first excess callback entry.
   * @throws Nothing.
   */
  bool overflowed() const noexcept { return overflowed_; }

 private:
  /** @brief One mirror/authority pair for every product callback kind. */
  std::array<FanoutDeliveryRecord, kFanoutProductEventCount * 2U> records_{};

  /** @brief Number of initialized records in fixed storage. */
  std::size_t size_ = 0U;

  /** @brief Sticky fixed-capacity overflow indicator. */
  bool overflowed_ = false;
};

/**
 * @brief Records every product callback and optionally wraps one real sink.
 *
 * The wrapper preserves the child sink's coordinate and publication behavior.
 * A test-only barrier can stop current-visible delivery after fanout entry but
 * before the wrapped mirror publishes its source observation.
 *
 * @throws Nothing after construction.
 * @note The barrier is deterministic test instrumentation and would violate
 * the production sink's nonblocking contract.
 */
class FanoutObservationSpySink final
    : public compute::ComputeRunObservationSink {
 public:
  /**
   * @brief Binds one child role, fixed log, and optional wrapped sink.
   * @param recipient Role represented by this wrapper.
   * @param log Non-null caller-owned log outliving every callback.
   * @param coordinate Fixed coordinate used without a wrapped authority.
   * @param task_semantics Fixed task opt-in used without a wrapped sink.
   * @param downstream Optional real collector receiving each callback.
   * @param pause_current_visible Whether to pause before wrapped publication.
   * @throws Nothing after shared-owner argument evaluation.
   * @note Tests must provide a non-null `log`; this test helper deliberately
   * avoids runtime validation inside the `noexcept` callback seam.
   */
  FanoutObservationSpySink(
      FanoutRecipient recipient, FanoutDeliveryLog* log,
      compute::ComputeRunObservationCoordinate coordinate, bool task_semantics,
      std::shared_ptr<compute::ComputeRunObservationSink> downstream = nullptr,
      bool pause_current_visible = false) noexcept
      : recipient_(recipient),
        log_(log),
        coordinate_(coordinate),
        task_semantics_(task_semantics),
        downstream_(std::move(downstream)),
        pause_current_visible_(pause_current_visible) {}

  /** @copydoc compute::ComputeRunObservationSink::reserve_causal_coordinate */
  compute::ComputeRunObservationCoordinate reserve_causal_coordinate() noexcept
      override {
    ++reservation_count_;
    return downstream_ ? downstream_->reserve_causal_coordinate() : coordinate_;
  }

  /** @copydoc compute::ComputeRunObservationSink::abort_causal_coordinate */
  void abort_causal_coordinate(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    ++abort_count_;
    if (downstream_) {
      downstream_->abort_causal_coordinate(coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_generation */
  void on_current_generation(
      const compute::SupersessionIdentity& identity,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::CurrentGeneration, coordinate);
    if (downstream_) {
      downstream_->on_current_generation(identity, coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::observes_task_semantics */
  bool observes_task_semantics() const noexcept override {
    return downstream_ ? downstream_->observes_task_semantics()
                       : task_semantics_;
  }

  /** @copydoc compute::ComputeRunObservationSink::on_task_ready */
  void on_task_ready(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      const compute::ComputeRunTaskReadyObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::TaskReady, coordinate);
    if (downstream_) {
      downstream_->on_task_ready(descriptor, task_identity, observation,
                                 coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_service_start */
  void on_service_start(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      std::uint64_t service_charge,
      const compute::ComputeRunServiceStartObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::ServiceStart, coordinate);
    if (downstream_) {
      downstream_->on_service_start(descriptor, task_identity, service_charge,
                                    observation, coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_task_terminal */
  void on_task_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      compute::ComputeRunTaskTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::TaskTerminal, coordinate);
    if (downstream_) {
      downstream_->on_task_terminal(descriptor, task_identity, kind,
                                    coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
  void on_cancellation(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunCancellationReason reason,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::Cancellation, coordinate);
    if (downstream_) {
      downstream_->on_cancellation(descriptor, reason, coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_terminal */
  void on_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::Terminal, coordinate);
    if (downstream_) {
      downstream_->on_terminal(descriptor, kind, coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
  void on_current_visible(
      const compute::ComputeRunDescriptor& descriptor, Value output,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    log_->record(FanoutProductEvent::CurrentVisible, recipient_, coordinate,
                 &output);
    ++callback_count_;
    if (pause_current_visible_) {
      current_visible_paused_.store(true, std::memory_order_release);
      while (!current_visible_released_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
    if (downstream_) {
      downstream_->on_current_visible(descriptor, std::move(output),
                                      coordinate);
    }
  }

  /**
   * @copydoc compute::ComputeRunObservationSink::on_progressive_final_triggered
   */
  void on_progressive_final_triggered(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::ProgressiveFinalTriggered, coordinate);
    if (downstream_) {
      downstream_->on_progressive_final_triggered(descriptor, coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
  void on_run_quiescent(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::RunQuiescent, coordinate);
    if (downstream_) {
      downstream_->on_run_quiescent(descriptor, coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
  void on_run_resource_settled(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::RunResourceSettled, coordinate);
    if (downstream_) {
      downstream_->on_run_resource_settled(descriptor, coordinate);
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_host_settled */
  void on_host_settled(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    record(FanoutProductEvent::HostSettled, coordinate);
    if (downstream_) {
      downstream_->on_host_settled(coordinate);
    }
  }

  /**
   * @brief Reports whether the current-visible callback reached its barrier.
   * @return True after callback entry and before wrapped publication.
   * @throws Nothing.
   */
  bool current_visible_paused() const noexcept {
    return current_visible_paused_.load(std::memory_order_acquire);
  }

  /**
   * @brief Releases the current-visible callback to the wrapped sink.
   * @return Nothing.
   * @throws Nothing.
   */
  void release_current_visible() noexcept {
    current_visible_released_.store(true, std::memory_order_release);
  }

  /**
   * @brief Returns the number of coordinates reserved through this wrapper.
   * @return Exact reservation count.
   * @throws Nothing.
   */
  std::uint64_t reservation_count() const noexcept {
    return reservation_count_;
  }

  /**
   * @brief Returns the number of callbacks entering this wrapper.
   * @return Exact product callback count.
   * @throws Nothing.
   */
  std::uint64_t callback_count() const noexcept { return callback_count_; }

  /**
   * @brief Returns the number of aborted coordinates through this wrapper.
   * @return Exact abort count.
   * @throws Nothing.
   */
  std::uint64_t abort_count() const noexcept { return abort_count_; }

 private:
  /**
   * @brief Records one non-Value product callback entry.
   * @param event Product event entering this wrapper.
   * @param coordinate Exact authority-owned coordinate.
   * @return Nothing.
   * @throws Nothing.
   */
  void record(FanoutProductEvent event,
              compute::ComputeRunObservationCoordinate coordinate) noexcept {
    log_->record(event, recipient_, coordinate);
    ++callback_count_;
  }

  /** @brief Child role represented by this wrapper. */
  FanoutRecipient recipient_;

  /** @brief Caller-owned fixed callback log. */
  FanoutDeliveryLog* log_ = nullptr;

  /** @brief Fixed coordinate used when no real collector is wrapped. */
  compute::ComputeRunObservationCoordinate coordinate_;

  /** @brief Fixed task opt-in used when no real collector is wrapped. */
  bool task_semantics_ = false;

  /** @brief Optional real collector receiving each callback after the spy. */
  std::shared_ptr<compute::ComputeRunObservationSink> downstream_;

  /** @brief Whether current-visible callback entry activates the barrier. */
  bool pause_current_visible_ = false;

  /** @brief True after current-visible callback entry reaches the barrier. */
  std::atomic<bool> current_visible_paused_{false};

  /** @brief True when current-visible publication may proceed. */
  std::atomic<bool> current_visible_released_{false};

  /** @brief Number of coordinate reservations through this wrapper. */
  std::uint64_t reservation_count_ = 0U;

  /** @brief Number of product callbacks entering this wrapper. */
  std::uint64_t callback_count_ = 0U;

  /** @brief Number of aborted coordinates through this wrapper. */
  std::uint64_t abort_count_ = 0U;
};

/**
 * @brief Recasts one eligible required-storage M1 environment as its I1 peer.
 * @param m1 Same-subject, same-ordinal M1 environment.
 * @return Base-identical storage-N/A isolated I1 environment evidence.
 * @throws Canonical encoding and allocation failures unchanged.
 */
B1EnvironmentEvidence make_isolated_i1_peer(B1EnvironmentEvidence m1) {
  m1.workload_id = kI1WorkloadId;
  m1.storage_manifest.reset();
  m1.claimed_storage_digest.reset();
  m1.storage_raw_proof.reset();
  m1.storage_eligibility.reset();
  m1.storage_actual_observation.reset();
  const std::vector<B1CanonicalField> environment_class{
      testing::known_b1_field("base_environment_digest", "sha256",
                              b1_digest_hex(m1.claimed_base_digest)),
      testing::known_b1_field("storage_environment_applicability", "enum",
                              "not-applicable"),
      testing::known_b1_field("storage_environment_not_applicable_reason",
                              "enum", "row-has-no-output-commit"),
      testing::not_applicable_b1_field("storage_environment_digest", "sha256",
                                       "row-has-no-output-commit")};
  m1.environment_class_manifest =
      encode_b1_environment_class(environment_class);
  m1.claimed_environment_class_digest =
      digest_b1_environment_manifest(m1.environment_class_manifest);
  return m1;
}

/**
 * @brief Builds one complete same-ordinal M1/I1/B1 environment triple.
 * @param ordinal Replicate ordinal in `[1,3]`.
 * @return M1 required-storage side, base-only I1 side, and full B1 cap-eight.
 * @throws Canonical fixture and allocation failures unchanged.
 */
std::array<B1EnvironmentEvidence, 3U> make_environment_triple(
    std::uint64_t ordinal) {
  B1EnvironmentEvidence m1 = testing::make_b1_test_environment(8U, ordinal);
  B1EnvironmentEvidence b1 = m1;
  m1.workload_id = kM1WorkloadId;
  return {m1, make_isolated_i1_peer(m1), b1};
}

/**
 * @brief Builds one exact deterministic M1 phase/offer/carryover protocol.
 * @return Structurally passing evidence in which both independent Graphs
 * complete one verified occurrence in every measured window and retain one
 * final offer through U.
 * @throws Checked time, job identity, and allocation failures unchanged.
 */
M1ProtocolEvidenceInput make_passing_protocol() {
  const auto b =
      std::chrono::steady_clock::time_point(std::chrono::seconds(100));
  const M1Timeline timeline = derive_m1_timeline(b);
  const auto coordinate = [](std::chrono::steady_clock::time_point timestamp,
                             std::uint64_t sequence) {
    return M1EventCoordinate{timestamp, sequence};
  };

  M1ProtocolEvidenceInput input;
  input.replicate_ordinal = 1U;
  input.boundaries =
      M1BoundaryEvidence{coordinate(timeline.cold_start, 1U),
                         coordinate(timeline.warmup_start, 1000U),
                         coordinate(timeline.measurement_start, 2000U),
                         coordinate(timeline.measurement_end, 10000U)};

  for (std::size_t index = 0U; index < kM1TotalI1OriginCount; ++index) {
    B1JobPhase phase = B1JobPhase::Measured;
    std::size_t ordinal = 0U;
    std::chrono::steady_clock::time_point origin;
    std::uint64_t origin_sequence = 0U;
    if (index == 0U) {
      phase = B1JobPhase::Cold;
      origin = timeline.cold_start;
      origin_sequence = 2U;
    } else if (index <= kM1WarmupI1OriginCount) {
      phase = B1JobPhase::Warmup;
      ordinal = index - 1U;
      origin = checked_i1_time_add(
          timeline.warmup_start,
          std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                   kI1EpisodeStride.count()));
      origin_sequence = ordinal == 0U ? 1001U : 1100U + ordinal;
    } else {
      ordinal = index - 1U - kM1WarmupI1OriginCount;
      origin = checked_i1_time_add(
          timeline.measurement_start,
          std::chrono::nanoseconds(static_cast<std::int64_t>(ordinal) *
                                   kI1EpisodeStride.count()));
      origin_sequence = ordinal == 0U ? 2001U : 4000U + ordinal;
    }
    const auto settlement =
        checked_i1_time_add(origin, kI1MeasurementEndOffset);
    const bool final_warmup =
        phase == B1JobPhase::Warmup && ordinal + 1U == kM1WarmupI1OriginCount;
    input.interactive_occurrences.push_back(M1InteractiveOccurrenceEvidence{
        phase, ordinal, coordinate(origin, origin_sequence), settlement,
        coordinate(settlement, 20000U + index),
        phase == B1JobPhase::Measured ? std::optional<std::chrono::nanoseconds>(
                                            std::chrono::milliseconds(10))
                                      : std::nullopt,
        I1ServiceEvidence{100U, 20U, 0U, 0.2}, I1Verdict::Pass, I1Verdict::Pass,
        I1Verdict::Pass, I1Verdict::Pass, true, final_warmup, final_warmup});
  }

  const auto make_offer =
      [&](B1JobPhase phase, std::uint64_t cycle, std::uint64_t job_index,
          std::uint64_t local_ordinal, M1EventCoordinate offered,
          std::optional<B1JobInstance> predecessor,
          std::optional<M1EventCoordinate> predecessor_end,
          std::optional<M1EventCoordinate> endpoint, bool output_removed) {
        return M1BatchOfferEvidence{
            B1JobInstance{kM1WorkloadId, 1U, phase, cycle, job_index, 8U},
            local_ordinal,
            0U,
            offered,
            std::move(predecessor),
            predecessor_end,
            endpoint,
            endpoint.has_value(),
            output_removed,
            true,
            true,
            true};
      };
  const B1JobInstance a252{kM1WorkloadId, 1U, B1JobPhase::Cold, 0U, 252U, 8U};
  const B1JobInstance b253{kM1WorkloadId, 1U, B1JobPhase::Warmup, 0U, 253U, 8U};
  const B1JobInstance a254{kM1WorkloadId, 1U, B1JobPhase::Warmup, 0U, 254U, 8U};
  const B1JobInstance b255{kM1WorkloadId, 1U, B1JobPhase::Warmup, 0U, 255U, 8U};
  const M1EventCoordinate a252_endpoint = coordinate(
      checked_i1_time_add(timeline.cold_start, std::chrono::milliseconds(700)),
      500U);
  const M1EventCoordinate b253_endpoint = coordinate(
      checked_i1_time_add(timeline.warmup_start, std::chrono::seconds(1)),
      1499U);
  input.batch_offers.push_back(make_offer(
      B1JobPhase::Cold, 0U, 252U, 0U, coordinate(timeline.cold_start, 3U),
      std::nullopt, std::nullopt, a252_endpoint, true));
  input.batch_offers.push_back(
      make_offer(B1JobPhase::Warmup, 0U, 253U, 0U,
                 coordinate(timeline.warmup_start, 1002U), std::nullopt,
                 std::nullopt, b253_endpoint, true));
  input.batch_offers.push_back(
      make_offer(B1JobPhase::Warmup, 0U, 254U, 1U,
                 coordinate(timeline.warmup_start, 1003U), a252, a252_endpoint,
                 coordinate(checked_i1_time_add(timeline.measurement_start,
                                                std::chrono::milliseconds(250)),
                            2500U),
                 true));
  input.batch_offers.push_back(make_offer(
      B1JobPhase::Warmup, 0U, 255U, 1U,
      coordinate(b253_endpoint.timestamp, 1500U), b253, b253_endpoint,
      coordinate(checked_i1_time_add(timeline.measurement_start,
                                     std::chrono::milliseconds(500)),
                 2600U),
      true));

  std::array<B1JobInstance, 2U> predecessors{a254, b255};
  std::array<std::optional<M1EventCoordinate>, 2U> predecessor_endpoints;
  for (std::size_t local_index = 0U; local_index <= kM1MeasuredWindowCount;
       ++local_index) {
    const std::uint64_t cycle = local_index / 15U;
    const auto endpoint_time =
        local_index == kM1MeasuredWindowCount
            ? timeline.measurement_end
            : checked_i1_time_add(
                  timeline.measurement_start,
                  std::chrono::milliseconds(500) +
                      std::chrono::seconds(
                          static_cast<std::int64_t>(local_index)));
    for (std::size_t parity = 0U; parity < 2U; ++parity) {
      const std::uint64_t job =
          2U * (local_index % 15U) + static_cast<std::uint64_t>(parity);
      const B1JobInstance current{kM1WorkloadId, 1U,  B1JobPhase::Measured,
                                  cycle,         job, 8U};
      const M1EventCoordinate offered =
          local_index == 0U
              ? coordinate(timeline.measurement_start, 2002U + parity)
              : coordinate(predecessor_endpoints[parity]->timestamp,
                           30000U + 4U * (local_index - 1U) + 2U + parity);
      const M1EventCoordinate endpoint =
          coordinate(endpoint_time, 30000U + 4U * local_index + parity);
      input.batch_offers.push_back(
          make_offer(B1JobPhase::Measured, cycle, job, 2U + local_index,
                     offered, predecessors[parity],
                     predecessor_endpoints[parity], endpoint, false));
      predecessors[parity] = current;
      predecessor_endpoints[parity] = endpoint;
    }
  }

  input.carryover = {
      M1CarryoverEntry{"i1:warmup:6", B1JobPhase::Warmup,
                       M1CarryoverState::Running, "", true, true, false},
      M1CarryoverEntry{"b1:" + encode_b1_job_instance(a254), B1JobPhase::Warmup,
                       M1CarryoverState::Running, "", true, false, false},
      M1CarryoverEntry{"b1:" + encode_b1_job_instance(b255), B1JobPhase::Warmup,
                       M1CarryoverState::Queued, "", true, false, false}};
  input.first_measured_admission = M1FirstMeasuredAdmissionEvidence{
      0U,
      timeline.measurement_start,
      true,
      checked_i1_time_add(timeline.measurement_start,
                          std::chrono::milliseconds(1)),
      2004U,
      true,
      I1AcceptedCoordinate(checked_i1_time_add(timeline.measurement_start,
                                               std::chrono::milliseconds(1)),
                           2004U),
      true,
      true,
      false,
      checked_i1_time_add(timeline.measurement_start,
                          kI1MeasurementStartOffset)};
  input.shared_execution_domain = true;
  input.boundary_was_zero_duration = true;
  input.raw_history_preserved = true;
  input.warmup_sources_closed = true;
  input.measured_counters_reset = true;
  input.final_settlement_proved = true;
  return input;
}

/**
 * @brief Builds one complete source-private execution snapshot for row tests.
 * @param active Whether the cut contains bounded active mixed work.
 * @param high_water Complete lifetime Host high-water at this cut.
 * @return Self-consistent limits, ready, I/O, lifecycle, and reservation data.
 * @throws std::bad_alloc when lifecycle/vector storage allocates.
 */
M1ExecutionSnapshot make_m1_snapshot(bool active, ResourceVector high_water) {
  M1ExecutionSnapshot snapshot;
  snapshot.host_resources.limits =
      ResourceVector{32U, 1073741824U, 536870912U, 65536U, 268435456U};
  snapshot.host_resources.reserved =
      active ? ResourceVector{4U, 1048576U, 524288U, 3U, 4096U}
             : ResourceVector{};
  snapshot.host_resources.high_water = high_water;
  const DeviceResourceVector device_limits{536870912U, 268435456U};
  const DeviceResourceVector device_reserved =
      active ? DeviceResourceVector{4096U, 2048U} : DeviceResourceVector{};
  const DeviceResourceVector device_high_water =
      high_water != ResourceVector{} ? DeviceResourceVector{8192U, 4096U}
                                     : DeviceResourceVector{};
  snapshot.device_resources.push_back(ResourceLedger::DeviceSnapshot{
      DeviceId(DeviceBackend::Metal, 0U), device_limits, device_reserved,
      device_high_water,
      DeviceResourceVector{device_limits.device_memory_bytes -
                               device_reserved.device_memory_bytes,
                           device_limits.device_scratch_bytes -
                               device_reserved.device_scratch_bytes}});
  snapshot.compute_io.task_limit = kB1ComputeIoTaskLimit;
  snapshot.compute_io.planned_bytes_limit = kB1ComputeIoPlannedByteLimit;
  snapshot.compute_io.active_tasks = 0U;
  snapshot.compute_io.active_planned_bytes = 0U;
  snapshot.compute_io.constructing_tasks = 0U;
  snapshot.compute_io.queued_tasks = 0U;
  snapshot.compute_io.running_tasks = 0U;
  snapshot.compute_io.accepting = true;
  snapshot.throughput.capacity =
      ResourceVector{31U, 1006632960U, 503316480U, 64512U, 251658240U};
  snapshot.throughput.reserved =
      active ? ResourceVector{3U, 524288U, 262144U, 2U, 2048U}
             : ResourceVector{};
  snapshot.ready_classes =
      active ? compute::ExecutionReadyClassSnapshot{1U, 2U, 3U, true}
             : compute::ExecutionReadyClassSnapshot{};
  snapshot.lifecycle.schema_version =
      compute::kExecutionLifecycleTelemetrySchemaVersion;
  snapshot.lifecycle.capacity = compute::kExecutionLifecycleTelemetryCapacity;
  snapshot.lifecycle.service_instance_id = 1U;
  snapshot.lifecycle.telemetry_epoch = 1U;
  snapshot.lifecycle.service_state =
      compute::ExecutionLifecycleServiceState::Accepting;
  if (active) {
    snapshot.lifecycle.counters.ready_entry_count = 3U;
    snapshot.lifecycle.counters.entered_callback_count = 2U;
    snapshot.lifecycle.counters.live_root_reservation_count = 2U;
    snapshot.lifecycle.counters.live_child_grant_count = 3U;
  }
  return snapshot;
}

/**
 * @brief Builds one canonical same-service lifecycle event for row tests.
 * @param sequence Exact positive telemetry order.
 * @param kind Closed event transition kind.
 * @param counters Complete post-transition counters.
 * @param graph_instance_id Optional exact Graph identity.
 * @param run_id Optional exact child Run identity.
 * @param run_group_id Optional exact realtime group identity.
 * @param generation Optional candidate, bundle, close, or shutdown identity.
 * @param category Exact cancellation/terminal category or None.
 * @return Versioned event with deterministic identities and timestamp.
 * @throws Nothing.
 */
compute::ExecutionLifecycleEvent make_m1_lifecycle_event(
    std::uint64_t sequence, compute::ExecutionLifecycleEventKind kind,
    compute::ExecutionLifecycleCounters counters,
    std::uint64_t graph_instance_id = 0U, std::uint64_t run_id = 0U,
    std::uint64_t run_group_id = 0U, std::uint64_t generation = 0U,
    compute::ExecutionLifecycleCategory category =
        compute::ExecutionLifecycleCategory::None) noexcept {
  compute::ExecutionLifecycleEvent event;
  event.sequence = sequence;
  event.timestamp_us = sequence;
  event.service_instance_id = 1U;
  event.telemetry_epoch = 1U;
  event.graph_instance_id = graph_instance_id;
  event.run_id = run_id;
  event.run_group_id = run_group_id;
  event.generation = generation;
  event.kind = kind;
  event.category = category;
  event.counters = counters;
  return event;
}

/**
 * @brief Assigns one exact lossless cursor page to a temporal test snapshot.
 * @param snapshot Mutable execution snapshot.
 * @param ordinal Chronological capture coordinate.
 * @param after_cursor Exact requested prior cut.
 * @param snapshot_cut Exact atomic page cut.
 * @param records Complete contiguous records after the cursor.
 * @return Nothing.
 * @throws std::bad_alloc when record ownership moves.
 */
void set_m1_lifecycle_page(
    M1ExecutionSnapshot* snapshot, std::size_t ordinal,
    std::uint64_t after_cursor, std::uint64_t snapshot_cut,
    std::vector<compute::ExecutionLifecycleEvent> records) {
  snapshot->temporal_capture_ordinal = ordinal;
  snapshot->lifecycle_after_cursor = after_cursor;
  snapshot->lifecycle.snapshot_cut = snapshot_cut;
  snapshot->lifecycle.first_retained_sequence = snapshot_cut == 0U ? 0U : 1U;
  snapshot->lifecycle.next_sequence = snapshot_cut + 1U;
  snapshot->lifecycle.records = std::move(records);
  snapshot->lifecycle.next_cursor = snapshot_cut;
  if (!snapshot->lifecycle.records.empty()) {
    snapshot->lifecycle.counters = snapshot->lifecycle.records.back().counters;
  }
}

/**
 * @brief Populates the four passing snapshots with one continuous history.
 * @param snapshots Exact four-snapshot test timeline to mutate.
 * @return Nothing.
 * @throws std::invalid_argument when the timeline cardinality drifts.
 * @throws std::bad_alloc when event vectors allocate.
 */
void configure_passing_m1_lifecycle_history(
    std::vector<M1ExecutionSnapshot>* snapshots) {
  if (snapshots == nullptr || snapshots->size() != 4U) {
    throw std::invalid_argument(
        "M1 lifecycle test history requires exactly four snapshots");
  }
  using Category = compute::ExecutionLifecycleCategory;
  using Kind = compute::ExecutionLifecycleEventKind;
  compute::ExecutionLifecycleCounters counters;
  counters.live_policy_binding_count = 2U;
  const compute::ExecutionLifecycleCounters service_started = counters;

  counters.registered_graph_count = 1U;
  counters.open_graph_count = 1U;
  const compute::ExecutionLifecycleCounters graph_one = counters;
  counters.registered_graph_count = 2U;
  counters.open_graph_count = 2U;
  const compute::ExecutionLifecycleCounters graph_two = counters;
  set_m1_lifecycle_page(
      &(*snapshots)[0U], 0U, 0U, 3U,
      {make_m1_lifecycle_event(1U, Kind::ServiceStarted, service_started),
       make_m1_lifecycle_event(2U, Kind::GraphRegistered, graph_one, 1U),
       make_m1_lifecycle_event(3U, Kind::GraphRegistered, graph_two, 2U)});

  counters.pending_candidate_count = 1U;
  const compute::ExecutionLifecycleCounters candidate_one = counters;
  counters.pending_candidate_count = 2U;
  const compute::ExecutionLifecycleCounters candidate_two = counters;
  counters.pending_candidate_count = 1U;
  const compute::ExecutionLifecycleCounters candidate_rollback = counters;
  counters.pending_candidate_count = 2U;
  counters.live_root_reservation_count = 1U;
  counters.live_child_grant_count = 2U;
  const compute::ExecutionLifecycleCounters cross_graph_candidates = counters;
  counters.pending_candidate_count = 1U;
  counters.admitted_run_group_count = 1U;
  counters.admitted_child_run_count = 2U;
  const compute::ExecutionLifecycleCounters group_admitted = counters;
  counters.pending_candidate_count = 0U;
  counters.admitted_standalone_run_count = 1U;
  counters.ready_entry_count = 3U;
  counters.entered_callback_count = 2U;
  counters.live_root_reservation_count = 2U;
  counters.live_child_grant_count = 5U;
  const compute::ExecutionLifecycleCounters mixed_active = counters;
  counters.open_graph_count = 1U;
  counters.closing_graph_count = 1U;
  const compute::ExecutionLifecycleCounters graph_one_closing = counters;
  set_m1_lifecycle_page(
      &(*snapshots)[1U], 1U, 3U, 10U,
      {make_m1_lifecycle_event(4U, Kind::CandidateBegan, candidate_one, 1U, 0U,
                               0U, 101U),
       make_m1_lifecycle_event(5U, Kind::CandidateBegan, candidate_two, 1U, 0U,
                               0U, 102U),
       make_m1_lifecycle_event(6U, Kind::CandidateRolledBack,
                               candidate_rollback, 1U, 0U, 0U, 102U),
       make_m1_lifecycle_event(7U, Kind::CandidateBegan, cross_graph_candidates,
                               2U, 0U, 0U, 201U),
       make_m1_lifecycle_event(8U, Kind::BundleAdmitted, group_admitted, 1U,
                               11U, 51U, 301U),
       make_m1_lifecycle_event(9U, Kind::BundleAdmitted, mixed_active, 2U, 21U,
                               0U, 302U),
       make_m1_lifecycle_event(10U, Kind::GraphClosing, graph_one_closing, 1U,
                               0U, 0U, 1U, Category::GraphClose)});
  (*snapshots)[2U].lifecycle.counters = graph_one_closing;
  set_m1_lifecycle_page(&(*snapshots)[2U], 2U, 10U, 10U, {});

  std::vector<compute::ExecutionLifecycleEvent> final_events;
  final_events.push_back(make_m1_lifecycle_event(
      11U, Kind::CancellationRequested, graph_one_closing, 1U, 0U, 0U, 1U,
      Category::GraphClose));
  counters = graph_one_closing;
  counters.ready_entry_count = 0U;
  counters.entered_callback_count = 0U;
  counters.live_root_reservation_count = 0U;
  counters.live_child_grant_count = 0U;
  counters.terminal_not_quiescent_run_count = 1U;
  counters.finalizing_run_count = 1U;
  final_events.push_back(make_m1_lifecycle_event(12U, Kind::RunTerminal,
                                                 counters, 1U, 11U, 51U, 301U,
                                                 Category::Cancelled));
  counters.terminal_not_quiescent_run_count = 2U;
  counters.finalizing_run_count = 2U;
  final_events.push_back(make_m1_lifecycle_event(13U, Kind::RunTerminal,
                                                 counters, 1U, 12U, 51U, 301U,
                                                 Category::Cancelled));
  counters.terminal_not_quiescent_run_count = 3U;
  counters.finalizing_run_count = 3U;
  final_events.push_back(make_m1_lifecycle_event(14U, Kind::RunTerminal,
                                                 counters, 2U, 21U, 0U, 302U,
                                                 Category::Succeeded));
  counters.terminal_not_quiescent_run_count = 2U;
  final_events.push_back(make_m1_lifecycle_event(15U, Kind::RunQuiescent,
                                                 counters, 2U, 21U, 0U, 302U));
  counters.terminal_not_quiescent_run_count = 1U;
  final_events.push_back(make_m1_lifecycle_event(16U, Kind::RunQuiescent,
                                                 counters, 1U, 11U, 51U, 301U));
  counters.terminal_not_quiescent_run_count = 0U;
  final_events.push_back(make_m1_lifecycle_event(17U, Kind::RunQuiescent,
                                                 counters, 1U, 12U, 51U, 301U));
  final_events.push_back(make_m1_lifecycle_event(18U, Kind::ResourceSettled,
                                                 counters, 2U, 21U, 0U, 302U));
  final_events.push_back(make_m1_lifecycle_event(19U, Kind::ResourceSettled,
                                                 counters, 1U, 11U, 51U, 301U));
  final_events.push_back(make_m1_lifecycle_event(20U, Kind::ResourceSettled,
                                                 counters, 1U, 12U, 51U, 301U));
  counters.admitted_standalone_run_count = 0U;
  counters.finalizing_run_count = 2U;
  final_events.push_back(make_m1_lifecycle_event(21U, Kind::RunUnregistered,
                                                 counters, 2U, 21U, 0U, 302U));
  counters.admitted_run_group_count = 0U;
  counters.admitted_child_run_count = 0U;
  counters.finalizing_run_count = 0U;
  final_events.push_back(make_m1_lifecycle_event(22U, Kind::RunUnregistered,
                                                 counters, 1U, 11U, 51U, 301U));
  final_events.push_back(make_m1_lifecycle_event(23U, Kind::RunUnregistered,
                                                 counters, 1U, 12U, 51U, 301U));
  counters.registered_graph_count = 1U;
  counters.open_graph_count = 1U;
  counters.closing_graph_count = 0U;
  final_events.push_back(make_m1_lifecycle_event(24U, Kind::GraphRowRemoved,
                                                 counters, 1U, 0U, 0U, 1U));
  counters.open_graph_count = 0U;
  counters.closing_graph_count = 1U;
  final_events.push_back(make_m1_lifecycle_event(
      25U, Kind::GraphClosing, counters, 2U, 0U, 0U, 1U, Category::GraphClose));
  counters.registered_graph_count = 0U;
  counters.closing_graph_count = 0U;
  final_events.push_back(make_m1_lifecycle_event(26U, Kind::GraphRowRemoved,
                                                 counters, 2U, 0U, 0U, 1U));
  final_events.push_back(make_m1_lifecycle_event(27U, Kind::WorkerJoined,
                                                 counters, 0U, 0U, 0U, 77U));
  final_events.push_back(make_m1_lifecycle_event(28U, Kind::WorkerJoined,
                                                 counters, 0U, 0U, 0U, 77U));
  counters.live_policy_binding_count = 1U;
  final_events.push_back(make_m1_lifecycle_event(29U, Kind::BindingRetired,
                                                 counters, 0U, 0U, 0U, 1U));
  counters.live_policy_binding_count = 0U;
  final_events.push_back(make_m1_lifecycle_event(30U, Kind::BindingRetired,
                                                 counters, 0U, 0U, 0U, 1U));
  final_events.push_back(make_m1_lifecycle_event(31U, Kind::ServiceStopped,
                                                 counters, 0U, 0U, 0U, 77U));
  set_m1_lifecycle_page(&(*snapshots)[3U], 3U, 10U, 31U,
                        std::move(final_events));
  (*snapshots)[3U].lifecycle.next_sequence =
      std::numeric_limits<std::uint64_t>::max();
  (*snapshots)[3U].lifecycle.shutdown_generation = 77U;
  (*snapshots)[3U].lifecycle.service_state =
      compute::ExecutionLifecycleServiceState::Stopped;
}

/**
 * @brief Returns one mutable lifecycle event by its global sequence.
 * @param snapshots Complete mutable temporal snapshot chain.
 * @param sequence Exact nonzero event sequence to locate.
 * @return Stable reference into the owning snapshot record vector.
 * @throws std::invalid_argument when the input is null or sequence is absent.
 * @note Tests must not retain the reference across vector reallocation.
 */
compute::ExecutionLifecycleEvent& require_m1_lifecycle_event(
    std::vector<M1ExecutionSnapshot>* snapshots, std::uint64_t sequence) {
  if (snapshots != nullptr) {
    for (M1ExecutionSnapshot& snapshot : *snapshots) {
      const auto found = std::find_if(
          snapshot.lifecycle.records.begin(), snapshot.lifecycle.records.end(),
          [sequence](const compute::ExecutionLifecycleEvent& event) {
            return event.sequence == sequence;
          });
      if (found != snapshot.lifecycle.records.end()) {
        return *found;
      }
    }
  }
  throw std::invalid_argument("M1 lifecycle test event sequence is absent");
}

/**
 * @brief Builds one complete passing five-axis M1 row input.
 * @return Exact protocol, SLO samples, fault-free waste, and zero settlement.
 * @throws Allocation and checked-time failures unchanged.
 */
M1InnerRowInput make_passing_inner_row_input() {
  M1InnerRowInput input;
  input.replicate_ordinal = 1U;
  input.protocol = make_passing_protocol();
  input.fairness = make_passing_fairness_input();
  input.paired_isolated_i1_p99 = std::chrono::milliseconds(10);
  testing::attach_m1_test_i1_sources(&input);
  testing::attach_m1_test_batch_sources(&input);
  testing::attach_m1_test_source_fairness_projection(&input);
  const ResourceVector zero_high_water{};
  const ResourceVector active_high_water{4U, 1048576U, 524288U, 3U, 4096U};
  input.temporal_snapshots = {make_m1_snapshot(false, zero_high_water),
                              make_m1_snapshot(true, active_high_water),
                              make_m1_snapshot(true, active_high_water),
                              make_m1_snapshot(false, active_high_water)};
  configure_passing_m1_lifecycle_history(&input.temporal_snapshots);
  input.occurrence_attribution_proved = true;
  input.temporal_effects_complete = true;
  return input;
}

/**
 * @brief Rewrites one B1 source to an exact positive total service charge.
 * @param source Mutable complete B1 source.
 * @param total Exact desired sum across all physical task starts.
 * @return Nothing after distributing the total without per-source overflow.
 * @throws std::invalid_argument when the source is null, empty, or the total
 * cannot assign at least one service unit to every start.
 * @note B1 semantic records bind task resources rather than this separately
 * retained charged-service scalar, so no semantic-trace rewrite is required.
 */
void set_m1_source_service_total(M1BatchSourceEvidence* source,
                                 std::uint64_t total) {
  if (source == nullptr || source->physical_trace.service_starts.empty() ||
      total < source->physical_trace.service_starts.size()) {
    throw std::invalid_argument(
        "M1 source service total cannot cover every physical start.");
  }
  const std::uint64_t count = source->physical_trace.service_starts.size();
  const std::uint64_t quotient = total / count;
  const std::uint64_t remainder = total % count;
  for (std::size_t index = 0U;
       index < source->physical_trace.service_starts.size(); ++index) {
    source->physical_trace.service_starts[index].service_charge =
        quotient + static_cast<std::uint64_t>(index < remainder);
  }
}

/**
 * @brief Builds closed mixed observations exactly backing one row's starts.
 * @param row Evaluated row whose class-start projection is retained.
 * @return Stable complete observation snapshot for canonical replay tests.
 * @throws std::bad_alloc when event storage allocates.
 */
M1FairnessObservationSnapshot make_passing_observation_snapshot(
    const M1InnerRow& row) {
  M1FairnessObservationSnapshot observations;
  observations.stable_publication_cut = true;
  for (std::size_t index = 0U;
       index < row.evidence.fairness.class_starts.size(); ++index) {
    const M1ClassStartSample& start = row.evidence.fairness.class_starts[index];
    observations.events.push_back(M1FairnessObservation{
        M1ObservationKind::ServiceStart,
        start.service_class == compute::ComputeRunQosClass::Interactive
            ? M1ObservedRequestTag::Interactive
            : M1ObservedRequestTag::ThroughputGraphA,
        start.service_class, true, start.causal_sequence,
        row.evidence.protocol.boundaries.measurement_start.timestamp +
            std::chrono::milliseconds(static_cast<std::int64_t>(index + 1U)),
        index + 1U, index, 1U, compute::ComputeRunTaskTerminalKind::Succeeded,
        compute::ComputeRunTerminalKind::Succeeded,
        start.interactive_candidate_startable,
        start.throughput_candidate_startable, start.execution_grant_committed});
  }
  observations.reservation_entry_frontier = observations.events.size();
  observations.reservation_completion_frontier = observations.events.size();
  observations.claimed_slot_frontier = observations.events.size();
  observations.published_slot_frontier = observations.events.size();
  return observations;
}

/**
 * @brief Encodes one test-only ordered list using the canonical frame grammar.
 * @param records Complete records in semantic order.
 * @return Count-prefixed canonical framed list.
 * @throws std::bad_alloc when output ownership allocates.
 */
std::string encode_m1_test_record_list(
    const std::vector<std::string>& records) {
  std::string result = std::to_string(records.size()) + ":";
  for (const std::string& record : records) {
    result.append(b1_environment_frame(record));
  }
  return result;
}

/**
 * @brief Proves exact M1 boundary derivation and checked arithmetic failures.
 * @throws GoogleTest assertion control and checked clock exceptions.
 */
TEST(M1Profile, DerivesExactTimelineAndRejectsClockOverflow) {
  const auto measurement_start =
      std::chrono::steady_clock::time_point(std::chrono::seconds(100));
  const M1Timeline timeline = derive_m1_timeline(measurement_start);
  EXPECT_EQ(timeline.cold_start,
            std::chrono::steady_clock::time_point(std::chrono::seconds(94)));
  EXPECT_EQ(timeline.warmup_start,
            std::chrono::steady_clock::time_point(std::chrono::seconds(95)));
  EXPECT_EQ(timeline.measurement_start, measurement_start);
  EXPECT_EQ(timeline.measurement_end,
            std::chrono::steady_clock::time_point(std::chrono::seconds(130)));

  EXPECT_THROW(derive_m1_timeline(std::chrono::steady_clock::time_point::min()),
               std::overflow_error);
  EXPECT_THROW(derive_m1_timeline(std::chrono::steady_clock::time_point::max()),
               std::overflow_error);
}

/**
 * @brief Proves exact cold/warmup/carryover and producer-local cycle protocol.
 * @throws GoogleTest assertion control and protocol allocation failures.
 */
TEST(M1Profile, PassesExactCrossBoundaryProtocolAndIndependentCycles) {
  const M1ProtocolEvidenceInput input = make_passing_protocol();
  const M1ProtocolSummary summary = evaluate_m1_protocol(input);
  EXPECT_EQ(summary.verdict, I1Verdict::Pass)
      << (summary.validity_reasons.empty() ? "no diagnostic"
                                           : summary.validity_reasons.front());
  EXPECT_TRUE(summary.validity_reasons.empty());
  ASSERT_GT(input.batch_offers.size(), 6U);
  EXPECT_EQ(
      input.batch_offers[input.batch_offers.size() - 2U].job.cycle_ordinal, 2U);
  EXPECT_EQ(input.batch_offers[input.batch_offers.size() - 2U].job.job_index,
            0U);
  EXPECT_EQ(input.batch_offers.back().job.cycle_ordinal, 2U);
  EXPECT_EQ(input.batch_offers.back().job.job_index, 1U);
  EXPECT_EQ(input.batch_offers[5U].job.cycle_ordinal, 0U);
  EXPECT_EQ(input.batch_offers[5U].job.job_index, 1U);
}

/**
 * @brief Proves B255, carryover, and final-warmup exception drift fail closed.
 * @throws GoogleTest assertion control and protocol allocation failures.
 */
TEST(M1Profile, RejectsWarmupCarryoverAndCurrentHoldDrift) {
  M1ProtocolEvidenceInput b255 = make_passing_protocol();
  b255.batch_offers[3U].predecessor_terminal.reset();
  EXPECT_EQ(evaluate_m1_protocol(std::move(b255)).verdict, I1Verdict::Invalid);

  M1ProtocolEvidenceInput carryover = make_passing_protocol();
  carryover.carryover.pop_back();
  EXPECT_EQ(evaluate_m1_protocol(std::move(carryover)).verdict,
            I1Verdict::Invalid);

  M1ProtocolEvidenceInput current_hold = make_passing_protocol();
  current_hold.interactive_occurrences[kM1WarmupI1OriginCount]
      .publication_current_at_measurement = false;
  EXPECT_EQ(evaluate_m1_protocol(std::move(current_hold)).verdict,
            I1Verdict::Invalid);

  M1ProtocolEvidenceInput admission = make_passing_protocol();
  admission.first_measured_admission.accepted_coordinate.reset();
  admission.first_measured_admission.host_succeeded = false;
  EXPECT_EQ(evaluate_m1_protocol(std::move(admission)).verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves shared barriers, phase rewrites, and post-cutoff offers fail.
 * @throws GoogleTest assertion control and protocol allocation failures.
 */
TEST(M1Profile, RejectsProducerBarrierPhaseAndCutoffDrift) {
  M1ProtocolEvidenceInput phase = make_passing_protocol();
  phase.batch_offers[2U].job.phase = B1JobPhase::Measured;
  EXPECT_EQ(evaluate_m1_protocol(std::move(phase)).verdict, I1Verdict::Invalid);

  M1ProtocolEvidenceInput gap = make_passing_protocol();
  gap.batch_offers.back().offered.timestamp += std::chrono::nanoseconds(1);
  EXPECT_EQ(evaluate_m1_protocol(std::move(gap)).verdict, I1Verdict::Invalid);

  M1ProtocolEvidenceInput cutoff = make_passing_protocol();
  cutoff.batch_offers.back().offered = cutoff.boundaries.measurement_end;
  cutoff.batch_offers.back().offered.event_sequence += 100U;
  EXPECT_EQ(evaluate_m1_protocol(std::move(cutoff)).verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves all five closed M1 axes pass without cross-axis substitution.
 * @throws GoogleTest assertion control and row evaluation allocation failures.
 */
TEST(M1Profile, PassesClosedFiveAxisInnerRow) {
  const M1InnerRow row = evaluate_m1_inner_row(make_passing_inner_row_input());
  EXPECT_EQ(row.schema, kM1InnerRowSchema);
  EXPECT_EQ(row.schema_version, kM1InnerRowSchemaVersion);
  EXPECT_EQ(row.workload_id, kM1WorkloadId);
  EXPECT_EQ(row.protocol.verdict, I1Verdict::Pass)
      << ::testing::PrintToString(row.protocol.validity_reasons);
  EXPECT_TRUE(row.source_evidence_closed);
  EXPECT_EQ(row.evidence.fairness.progress_windows.size(),
            kM1MeasuredWindowCount);
  EXPECT_EQ(row.evidence.fairness.graph_service_windows.size(),
            kM1MeasuredWindowCount);
  EXPECT_EQ(row.evidence.fairness.headroom_outcomes.size(),
            kM1MeasuredI1AttemptCount);
  EXPECT_EQ(row.evidence.fairness.headroom_admissions.attempted_edits,
            kM1MeasuredI1AttemptCount);
  EXPECT_EQ(row.evidence.fairness.headroom_admissions.classified_outcomes,
            kM1MeasuredI1AttemptCount);
  ASSERT_TRUE(row.latency.has_value());
  EXPECT_EQ(row.latency->p50, std::chrono::milliseconds(10));
  EXPECT_EQ(row.latency->p95, std::chrono::milliseconds(10));
  EXPECT_EQ(row.latency->p99, std::chrono::milliseconds(10));
  ASSERT_TRUE(row.relative_latency_p99.has_value());
  EXPECT_DOUBLE_EQ(*row.relative_latency_p99, 1.0);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.fairness_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.overall_verdict, I1Verdict::Pass)
      << (row.validity_reasons.empty() ? "no diagnostic"
                                       : row.validity_reasons.front());
  EXPECT_DOUBLE_EQ(*row.interactive_discarded_ratio, 0.0);
  EXPECT_EQ(row.compute_io_task_high_water, 1U);
  EXPECT_EQ(row.compute_io_planned_byte_high_water, kB1PayloadBytes);
}

/**
 * @brief Proves a self-consistent forged A0 cannot diverge from raw source.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 * @note The protocol-only record remains internally valid; source closure must
 * reject it before the six axis/overall verdicts can be composed.
 */
TEST(M1Profile, RejectsFirstAdmissionProjectionDriftBeforeProtocolReturn) {
  M1InnerRowInput input = make_passing_inner_row_input();
  M1FirstMeasuredAdmissionEvidence& first =
      input.protocol.first_measured_admission;
  const auto forged_sample =
      checked_i1_time_add(first.admission_sample, std::chrono::nanoseconds(1));
  ASSERT_TRUE(first.accepted_coordinate.has_value());
  first.admission_sample = forged_sample;
  first.accepted_coordinate.emplace(
      forged_sample, first.accepted_coordinate->event_sequence());
  EXPECT_EQ(evaluate_m1_protocol(input.protocol).verdict, I1Verdict::Pass);

  const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
  EXPECT_EQ(row.protocol.verdict, I1Verdict::Pass);
  EXPECT_FALSE(row.source_evidence_closed);
  EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.throughput_progress_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.fairness_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
}

/**
 * @brief Rejects raw final-warmup facts that contradict current hold at B.
 *
 * One case moves the final visible publication just beyond the measured
 * boundary while keeping it before the later accepted replacement.  The
 * other inserts a nominally `Superseded` cancellation before the measured
 * product-current observation.  Each complete Issue #93 source is replayed
 * back into its occurrence projection. The late-publication protocol remains
 * independently valid; the cancellation protocol is independently Invalid.
 * In both cases the shared source gate must derive and report the current-hold
 * contradiction before the protocol result can return.
 *
 * @throws GoogleTest assertion control, replay, and allocation failures.
 */
TEST(M1Profile, RejectsFinalWarmupRawCurrentHoldDriftBeforeProtocolReturn) {
  constexpr std::size_t final_warmup_index =
      kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
  const auto synchronize_occurrence = [](M1InnerRowInput* input) {
    const I1EpisodeInnerRow replay = evaluate_i1_episode(
        input->interactive_sources[final_warmup_index].episode);
    M1InteractiveOccurrenceEvidence& occurrence =
        input->protocol.interactive_occurrences[final_warmup_index];
    occurrence.final_latency = replay.final_latency;
    occurrence.service = replay.service;
    occurrence.latency_verdict = replay.latency_verdict;
    occurrence.waste_verdict = replay.waste_verdict;
    occurrence.memory_verdict = replay.memory_verdict;
    occurrence.output_verdict = replay.output_verdict;
  };
  const auto expect_source_rejection = [](M1InnerRowInput input,
                                          bool expected_current,
                                          bool expected_boundary_cancellation,
                                          I1Verdict expected_protocol) {
    const M1SourceFairnessProjection projection =
        derive_m1_source_fairness_projection(
            input.protocol, input.interactive_sources, input.batch_sources);
    EXPECT_EQ(projection.first_measured_admission
                  .warmup_publication_current_before_acceptance,
              expected_current);
    EXPECT_EQ(projection.first_measured_admission.boundary_only_cancellation,
              expected_boundary_cancellation);
    EXPECT_EQ(evaluate_m1_protocol(input.protocol).verdict, expected_protocol);
    const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
    EXPECT_FALSE(row.source_evidence_closed);
    EXPECT_EQ(row.latency_verdict, I1Verdict::Invalid);
    EXPECT_EQ(row.throughput_progress_verdict, I1Verdict::Invalid);
    EXPECT_EQ(row.fairness_verdict, I1Verdict::Invalid);
    EXPECT_EQ(row.waste_verdict, I1Verdict::Invalid);
    EXPECT_EQ(row.memory_verdict, I1Verdict::Invalid);
    EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
    EXPECT_TRUE(
        std::any_of(row.validity_reasons.begin(), row.validity_reasons.end(),
                    [](const std::string& reason) {
                      return reason.find("current hold") != std::string::npos;
                    }));
  };

  {
    SCOPED_TRACE("visible publication after B");
    M1InnerRowInput late_publication = make_passing_inner_row_input();
    I1EpisodeEvidenceInput& late_source =
        late_publication.interactive_sources[final_warmup_index].episode;
    const auto measured_boundary =
        late_publication.protocol.boundaries.measurement_start.timestamp;
    late_source.observations.visible_outputs.back().observed_at =
        checked_i1_time_add(measured_boundary, std::chrono::microseconds(1));
    late_source.observations.terminals.back().observed_at =
        checked_i1_time_add(measured_boundary, std::chrono::microseconds(2));
    late_source.observations.run_quiescences.back().observed_at =
        checked_i1_time_add(measured_boundary, std::chrono::microseconds(3));
    late_source.observations.resource_settlements.back().observed_at =
        checked_i1_time_add(measured_boundary, std::chrono::microseconds(4));
    late_source.observations.host_settlements.back().observed_at =
        checked_i1_time_add(measured_boundary, std::chrono::microseconds(5));
    synchronize_occurrence(&late_publication);
    expect_source_rejection(std::move(late_publication), false, false,
                            I1Verdict::Pass);
  }

  {
    SCOPED_TRACE("Superseded cancellation before replacement");
    M1InnerRowInput boundary_cancellation = make_passing_inner_row_input();
    I1EpisodeEvidenceInput& cancellation_source =
        boundary_cancellation.interactive_sources[final_warmup_index].episode;
    const auto measured_boundary =
        boundary_cancellation.protocol.boundaries.measurement_start.timestamp;
    const I1ObservedCurrentGeneration& warmup_current =
        cancellation_source.observations.current_generations.back();
    const std::uint64_t cancellation_sequence =
        cancellation_source.observation_cut.causal_sequence;
    cancellation_source.observations.cancellations.push_back(
        I1ObservedCancellation{
            kI1EditCount - 1U, 100U + kI1EditCount - 1U,
            warmup_current.generation,
            compute::ComputeRunCancellationReason::Superseded,
            checked_i1_time_add(measured_boundary,
                                std::chrono::microseconds(500)),
            cancellation_sequence});
    ++cancellation_source.observation_cut.causal_sequence;
    synchronize_occurrence(&boundary_cancellation);
    expect_source_rejection(std::move(boundary_cancellation), true, true,
                            I1Verdict::Invalid);
  }
}

/**
 * @brief Orders equal-time supersession by the M1 observer coordinate.
 *
 * The source-positive case retains measured current `(B,n)` followed by the
 * displaced final-warmup cancellation `(B,n+1)`. It must keep current hold,
 * avoid boundary-only classification, close source replay, and round-trip
 * canonically. Moving only cancellation to `(B,n-1)` must instead fail the
 * source/current-hold gate in both direct and canonical-reader paths.
 *
 * @throws GoogleTest assertion control, replay, canonical, and allocation
 * failures.
 * @note The final-warmup fixture has already published a successful visible
 * output, so its added accepted cancellation is independently Invalid under
 * Issue #93. This test requires M1 source projection/closure correctness and
 * intentionally does not weaken that separate Run contract.
 */
TEST(M1Profile, OrdersEqualTimeSupersessionByObserverSequence) {
  const auto has_reason = [](const M1InnerRow& row, std::string_view needle) {
    return std::any_of(row.validity_reasons.begin(), row.validity_reasons.end(),
                       [needle](const std::string& reason) {
                         return reason.find(needle) != std::string::npos;
                       });
  };

  M1InnerRowInput following = make_passing_inner_row_input();
  testing::configure_m1_test_equal_time_supersession(&following, true);
  const M1SourceFairnessProjection following_projection =
      derive_m1_source_fairness_projection(following.protocol,
                                           following.interactive_sources,
                                           following.batch_sources);
  EXPECT_TRUE(following_projection.first_measured_admission
                  .warmup_publication_current_before_acceptance);
  EXPECT_TRUE(following_projection.first_measured_admission
                  .superseded_exactly_at_acceptance);
  EXPECT_FALSE(
      following_projection.first_measured_admission.boundary_only_cancellation);
  ASSERT_TRUE(following.interactive_sources[8U]
                  .episode.edits[0U]
                  .accepted_coordinate.has_value());
  ASSERT_FALSE(following.interactive_sources[8U]
                   .episode.observations.current_generations.empty());
  EXPECT_NE(following.interactive_sources[8U]
                .episode.edits[0U]
                .accepted_coordinate->event_sequence(),
            following.interactive_sources[8U]
                .episode.observations.current_generations.front()
                .causal_sequence);

  testing::attach_m1_test_source_fairness_projection(&following);
  const M1ProtocolSummary following_protocol =
      evaluate_m1_protocol(following.protocol);
  EXPECT_EQ(following_protocol.verdict, I1Verdict::Invalid);
  EXPECT_TRUE(std::any_of(
      following_protocol.validity_reasons.begin(),
      following_protocol.validity_reasons.end(), [](const std::string& reason) {
        return reason.find("I1 inner evidence is invalid") != std::string::npos;
      }));
  const M1InnerRow following_row = evaluate_m1_inner_row(std::move(following));
  EXPECT_TRUE(following_row.source_evidence_closed);
  EXPECT_EQ(following_row.overall_verdict, I1Verdict::Invalid);
  EXPECT_FALSE(has_reason(following_row, "current hold differs"));
  EXPECT_FALSE(has_reason(following_row,
                          "source-derived admission/fairness replay failed"));

  const M1FairnessObservationSnapshot observations =
      make_passing_observation_snapshot(following_row);
  const std::string canonical =
      materialize_m1_inner_row(following_row, observations);
  const M1CanonicalReplay replay =
      parse_and_recompute_m1_inner_row(canonical, 1U);
  EXPECT_TRUE(replay.row.source_evidence_closed);
  EXPECT_TRUE(replay.row.evidence.protocol.first_measured_admission
                  .warmup_publication_current_before_acceptance);
  EXPECT_FALSE(replay.row.evidence.protocol.first_measured_admission
                   .boundary_only_cancellation);
  EXPECT_FALSE(has_reason(replay.row, "current hold differs"));
  EXPECT_FALSE(has_reason(replay.row,
                          "source-derived admission/fairness replay failed"));
  EXPECT_EQ(materialize_m1_inner_row(replay.row, replay.observations),
            canonical);

  const std::string reversed_canonical =
      testing::reverse_m1_test_equal_time_cancellation_order(canonical);
  EXPECT_THROW(parse_and_recompute_m1_inner_row(reversed_canonical, 1U),
               std::invalid_argument);

  M1InnerRowInput preceding = make_passing_inner_row_input();
  testing::configure_m1_test_equal_time_supersession(&preceding, true);
  testing::attach_m1_test_source_fairness_projection(&preceding);
  constexpr std::size_t final_warmup_index =
      kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
  std::vector<I1ObservedCancellation>& cancellations =
      preceding.interactive_sources[final_warmup_index]
          .episode.observations.cancellations;
  ASSERT_EQ(cancellations.size(), 1U);
  ASSERT_GT(cancellations.front().causal_sequence, 2U);
  cancellations.front().causal_sequence -= 2U;
  const M1SourceFairnessProjection preceding_projection =
      derive_m1_source_fairness_projection(preceding.protocol,
                                           preceding.interactive_sources,
                                           preceding.batch_sources);
  EXPECT_FALSE(preceding_projection.first_measured_admission
                   .warmup_publication_current_before_acceptance);
  EXPECT_TRUE(
      preceding_projection.first_measured_admission.boundary_only_cancellation);
  const M1InnerRow preceding_row = evaluate_m1_inner_row(std::move(preceding));
  EXPECT_FALSE(preceding_row.source_evidence_closed);
  EXPECT_EQ(preceding_row.overall_verdict, I1Verdict::Invalid);
  EXPECT_TRUE(has_reason(
      preceding_row,
      "M1 first admission/current hold differs from retained raw sources"));
  EXPECT_TRUE(has_reason(
      preceding_row,
      "M1 final-warmup current hold differs from retained raw sources"));
  EXPECT_FALSE(has_reason(preceding_row,
                          "source-derived admission/fairness replay failed"));
}

/**
 * @brief Proves one passing producer row round-trips through strict replay.
 * @throws GoogleTest assertion control and canonical/evaluator failures.
 */
TEST(M1Profile, RoundTripsPassingCanonicalInnerRowByRecomputation) {
  M1InnerRowInput input = make_passing_inner_row_input();
  ASSERT_TRUE(
      input.interactive_sources[8U].episode.edits[0U].host_return.has_value());
  input.interactive_sources[8U].episode.edits[0U].host_return->status.name =
      "m1\nstatus";
  input.interactive_sources[8U].episode.edits[0U].host_return->status.message =
      std::string("m1\0message", 10U);
  testing::attach_m1_test_source_fairness_projection(&input);
  const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
  const M1FairnessObservationSnapshot observations =
      make_passing_observation_snapshot(row);
  ASSERT_FALSE(observations.events.empty());
  EXPECT_EQ(observations.events.front().local_task_id, 0U);

  const std::string canonical = materialize_m1_inner_row(row, observations);
  const M1CanonicalReplay replay =
      parse_and_recompute_m1_inner_row(canonical, 1U);
  EXPECT_EQ(replay.row.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(replay.row.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(replay.row.fairness_verdict, I1Verdict::Pass);
  EXPECT_EQ(replay.row.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(replay.row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(replay.row.overall_verdict, I1Verdict::Pass);
  ASSERT_TRUE(replay.row.evidence.fairness.headroom_outcomes.front()
                  .host_status.has_value());
  EXPECT_EQ(
      replay.row.evidence.fairness.headroom_outcomes.front().host_status->name,
      "m1\nstatus");
  EXPECT_EQ(replay.row.evidence.fairness.headroom_outcomes.front()
                .host_status->message,
            std::string("m1\0message", 10U));
  EXPECT_EQ(materialize_m1_inner_row(replay.row, replay.observations),
            canonical);
}

/**
 * @brief Proves I1 source joins reject missing, duplicate, reordered, and
 * same-cardinality wrong identities before any five-axis result can survive.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsMissingDuplicateReorderedAndWrongI1Sources) {
  const auto expect_invalid = [](M1InnerRowInput input,
                                 std::string_view diagnostic,
                                 std::string_view scenario) {
    SCOPED_TRACE(scenario);
    const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
    EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
    EXPECT_TRUE(
        std::any_of(row.validity_reasons.begin(), row.validity_reasons.end(),
                    [diagnostic](const std::string& reason) {
                      return reason.find(diagnostic) != std::string::npos;
                    }));
  };

  M1InnerRowInput missing = make_passing_inner_row_input();
  missing.interactive_sources.pop_back();
  expect_invalid(std::move(missing), "source cardinality", "missing");

  M1InnerRowInput duplicate = make_passing_inner_row_input();
  duplicate.interactive_sources[9U] = duplicate.interactive_sources[8U];
  expect_invalid(std::move(duplicate), "source identity/order", "duplicate");

  M1InnerRowInput reordered = make_passing_inner_row_input();
  std::swap(reordered.interactive_sources[8U],
            reordered.interactive_sources[9U]);
  expect_invalid(std::move(reordered), "source identity/order", "reordered");

  M1InnerRowInput wrong_ordinal = make_passing_inner_row_input();
  ++wrong_ordinal.interactive_sources[8U].phase_ordinal;
  expect_invalid(std::move(wrong_ordinal), "source identity/order",
                 "wrong ordinal");

  M1InnerRowInput wrong_slot = make_passing_inner_row_input();
  ++wrong_slot.interactive_sources[8U].episode.slot;
  expect_invalid(std::move(wrong_slot), "source identity/order", "wrong slot");
}

/**
 * @brief Proves complete I1 replay, not retained occurrence scalars, controls
 * the joined latency/service/verdict projection.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsI1SourceAndOccurrenceProjectionContradiction) {
  M1InnerRowInput input = make_passing_inner_row_input();
  ASSERT_TRUE(
      input.protocol.interactive_occurrences[8U].final_latency.has_value());
  ++*input.protocol.interactive_occurrences[8U].final_latency;
  const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
  EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
  EXPECT_TRUE(std::any_of(
      row.validity_reasons.begin(), row.validity_reasons.end(),
      [](const std::string& reason) {
        return reason.find("derived projection drifted") != std::string::npos;
      }));
}

/**
 * @brief Proves retained progress cardinality, identity, and numerator cannot
 * diverge from replayed B1 sources even when the forged fairness facts pass.
 * @throws GoogleTest assertion control and source replay allocation failures.
 */
TEST(M1Profile, RejectsSourceDerivedProgressProjectionContradictions) {
  M1InnerRowInput numerator = make_passing_inner_row_input();
  ++numerator.fairness.progress_windows[0U].successful_site_operations;
  EXPECT_EQ(evaluate_m1_fairness(numerator.fairness).composite_fairness_verdict,
            I1Verdict::Pass);
  M1InnerRow numerator_row = evaluate_m1_inner_row(std::move(numerator));
  EXPECT_FALSE(numerator_row.source_evidence_closed);
  EXPECT_EQ(numerator_row.overall_verdict, I1Verdict::Invalid);
  EXPECT_TRUE(std::any_of(
      numerator_row.validity_reasons.begin(),
      numerator_row.validity_reasons.end(), [](const std::string& reason) {
        return reason.find("progress projection differs") != std::string::npos;
      }));

  M1InnerRowInput cardinality = make_passing_inner_row_input();
  cardinality.fairness.progress_windows.pop_back();
  EXPECT_FALSE(
      evaluate_m1_inner_row(std::move(cardinality)).source_evidence_closed);

  M1InnerRowInput identity = make_passing_inner_row_input();
  ++identity.fairness.progress_windows[0U].window_ordinal;
  EXPECT_FALSE(
      evaluate_m1_inner_row(std::move(identity)).source_evidence_closed);
}

/**
 * @brief Proves both Graph service fields, demand, cardinality, and window
 * identity exact-match the source-derived projection.
 * @throws GoogleTest assertion control and source replay allocation failures.
 */
TEST(M1Profile, RejectsSourceDerivedGraphProjectionContradictions) {
  const std::vector<std::function<void(M1InnerRowInput*)>> rewrites{
      [](M1InnerRowInput* input) {
        ++input->fairness.graph_service_windows[0U].graph_a_completed_service;
      },
      [](M1InnerRowInput* input) {
        ++input->fairness.graph_service_windows[0U].graph_b_completed_service;
      },
      [](M1InnerRowInput* input) {
        input->fairness.graph_service_windows[0U]
            .both_graphs_continuously_demanding = false;
      },
      [](M1InnerRowInput* input) {
        input->fairness.graph_service_windows.pop_back();
      },
      [](M1InnerRowInput* input) {
        ++input->fairness.graph_service_windows[0U].window_ordinal;
      }};
  for (std::size_t index = 0U; index < rewrites.size(); ++index) {
    SCOPED_TRACE(index);
    M1InnerRowInput input = make_passing_inner_row_input();
    rewrites[index](&input);
    if (index < 3U) {
      EXPECT_EQ(evaluate_m1_fairness(input.fairness).composite_fairness_verdict,
                I1Verdict::Pass);
    }
    const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
    EXPECT_FALSE(row.source_evidence_closed);
    EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
    EXPECT_TRUE(std::any_of(
        row.validity_reasons.begin(), row.validity_reasons.end(),
        [](const std::string& reason) {
          return reason.find("Graph projection differs") != std::string::npos;
        }));
  }
}

/**
 * @brief Proves every headroom status/flag/identity/order field and all three
 * aggregates exact-match the forty measured episode sources.
 * @throws GoogleTest assertion control and source replay allocation failures.
 */
TEST(M1Profile, RejectsSourceDerivedHeadroomProjectionContradictions) {
  const std::vector<std::function<void(M1InnerRowInput*)>> rewrites{
      [](M1InnerRowInput* input) {
        input->fairness.headroom_outcomes[0U].host_status->message = "forged";
      },
      [](M1InnerRowInput* input) {
        input->fairness.headroom_outcomes[0U].throughput_headroom_failure =
            true;
      },
      [](M1InnerRowInput* input) {
        ++input->fairness.headroom_outcomes[0U].origin_ordinal;
      },
      [](M1InnerRowInput* input) {
        ++input->fairness.headroom_outcomes[0U].edit_index;
      },
      [](M1InnerRowInput* input) {
        std::swap(input->fairness.headroom_outcomes[0U],
                  input->fairness.headroom_outcomes[1U]);
      },
      [](M1InnerRowInput* input) {
        input->fairness.headroom_outcomes.pop_back();
      }};
  for (std::size_t index = 0U; index < rewrites.size(); ++index) {
    SCOPED_TRACE(index);
    M1InnerRowInput input = make_passing_inner_row_input();
    rewrites[index](&input);
    if (index == 0U) {
      EXPECT_EQ(evaluate_m1_fairness(input.fairness).composite_fairness_verdict,
                I1Verdict::Pass);
    }
    const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
    EXPECT_FALSE(row.source_evidence_closed);
    EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
    EXPECT_TRUE(std::any_of(
        row.validity_reasons.begin(), row.validity_reasons.end(),
        [](const std::string& reason) {
          return reason.find("headroom outcome projection differs") !=
                 std::string::npos;
        }));
  }

  const std::vector<std::function<void(M1HeadroomAdmissionEvidence*)>>
      aggregate_rewrites{[](M1HeadroomAdmissionEvidence* aggregate) {
                           --aggregate->attempted_edits;
                         },
                         [](M1HeadroomAdmissionEvidence* aggregate) {
                           --aggregate->classified_outcomes;
                         },
                         [](M1HeadroomAdmissionEvidence* aggregate) {
                           ++aggregate->throughput_headroom_failures;
                         }};
  for (std::size_t index = 0U; index < aggregate_rewrites.size(); ++index) {
    SCOPED_TRACE(index);
    M1InnerRowInput input = make_passing_inner_row_input();
    aggregate_rewrites[index](&input.fairness.headroom_admissions);
    const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
    EXPECT_FALSE(row.source_evidence_closed);
    EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
    EXPECT_TRUE(std::any_of(
        row.validity_reasons.begin(), row.validity_reasons.end(),
        [](const std::string& reason) {
          return reason.find("headroom aggregate differs") != std::string::npos;
        }));
  }
}

/**
 * @brief Proves checked source-derived service aggregation cannot wrap.
 * @throws GoogleTest assertion control and source replay allocation failures.
 */
TEST(M1Profile, RejectsSourceDerivedFairnessProjectionOverflow) {
  M1InnerRowInput input = make_passing_inner_row_input();
  std::vector<std::size_t> graph_a_sources;
  for (std::size_t index = 0U; index < input.batch_sources.size(); ++index) {
    const M1BatchSourceEvidence& source = input.batch_sources[index];
    if (source.job.phase == B1JobPhase::Measured &&
        (source.job.job_index & 1U) == 0U) {
      graph_a_sources.push_back(index);
    }
  }
  ASSERT_GE(graph_a_sources.size(), 2U);
  const std::size_t first = graph_a_sources[0U];
  const std::size_t second = graph_a_sources[1U];
  const auto forced_endpoint =
      checked_i1_time_add(input.protocol.boundaries.measurement_start.timestamp,
                          std::chrono::milliseconds(750));
  input.protocol.batch_offers[second].endpoint->timestamp = forced_endpoint;
  input.batch_sources[second].endpoint_at = forced_endpoint;
  const std::uint64_t half_plus_one =
      std::numeric_limits<std::uint64_t>::max() / 2U + 1U;
  set_m1_source_service_total(&input.batch_sources[first], half_plus_one);
  set_m1_source_service_total(&input.batch_sources[second], half_plus_one);

  EXPECT_THROW(
      derive_m1_source_fairness_projection(
          input.protocol, input.interactive_sources, input.batch_sources),
      std::overflow_error);
  const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
  EXPECT_FALSE(row.source_evidence_closed);
  EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves B1 source joins and replay-derived waste reject cardinality,
 * identity/order, raw-trace, and stale scalar contradictions.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsB1SourceJoinRawTraceAndWasteContradictions) {
  const auto expect_invalid = [](M1InnerRowInput input,
                                 std::string_view diagnostic,
                                 std::string_view scenario) {
    SCOPED_TRACE(scenario);
    const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
    EXPECT_EQ(row.overall_verdict, I1Verdict::Invalid);
    EXPECT_TRUE(
        std::any_of(row.validity_reasons.begin(), row.validity_reasons.end(),
                    [diagnostic](const std::string& reason) {
                      return reason.find(diagnostic) != std::string::npos;
                    }))
        << ::testing::PrintToString(row.validity_reasons);
  };

  M1InnerRowInput missing = make_passing_inner_row_input();
  missing.batch_sources.pop_back();
  expect_invalid(std::move(missing), "source cardinality", "missing");

  M1InnerRowInput duplicate = make_passing_inner_row_input();
  duplicate.batch_sources[5U] = duplicate.batch_sources[4U];
  expect_invalid(std::move(duplicate), "source identity/order", "duplicate");

  M1InnerRowInput reordered = make_passing_inner_row_input();
  std::swap(reordered.batch_sources[4U], reordered.batch_sources[5U]);
  expect_invalid(std::move(reordered), "source identity/order", "reordered");

  M1InnerRowInput wrong_ordinal = make_passing_inner_row_input();
  ++wrong_ordinal.batch_sources[4U].producer_offer_ordinal;
  expect_invalid(std::move(wrong_ordinal), "source identity/order",
                 "wrong ordinal");

  M1InnerRowInput missing_endpoint = make_passing_inner_row_input();
  missing_endpoint.protocol.batch_offers[4U].endpoint.reset();
  expect_invalid(std::move(missing_endpoint), "offer endpoint drifted",
                 "missing endpoint");

  M1InnerRowInput stale_waste = make_passing_inner_row_input();
  ++stale_waste.batch_waste.discarded_started_service;
  expect_invalid(std::move(stale_waste), "waste projection differs",
                 "stale waste");

  M1InnerRowInput raw_trace = make_passing_inner_row_input();
  const auto measured = std::find_if(
      raw_trace.batch_sources.begin(), raw_trace.batch_sources.end(),
      [](const M1BatchSourceEvidence& source) {
        return source.job.phase == B1JobPhase::Measured;
      });
  ASSERT_NE(measured, raw_trace.batch_sources.end());
  ASSERT_FALSE(measured->physical_trace.service_starts.empty());
  ++measured->physical_trace.service_starts[0U].service_charge;
  expect_invalid(std::move(raw_trace), "waste projection differs", "raw trace");

  M1InnerRowInput stale_endpoint = make_passing_inner_row_input();
  const auto endpoint_source = std::find_if(
      stale_endpoint.batch_sources.begin(), stale_endpoint.batch_sources.end(),
      [](const M1BatchSourceEvidence& source) {
        return source.job.phase == B1JobPhase::Measured;
      });
  ASSERT_NE(endpoint_source, stale_endpoint.batch_sources.end());
  ASSERT_TRUE(endpoint_source->verified_endpoint);
  endpoint_source->verified_endpoint = false;
  expect_invalid(std::move(stale_endpoint), "source replay failed closed",
                 "stale verified endpoint");
}

/**
 * @brief Proves strict canonical replay rejects source-list and projection
 * contradictions even when the enclosing manifest is re-encoded exactly.
 * @throws GoogleTest assertion control and canonical/evaluator failures.
 */
TEST(M1Profile, RejectsCanonicalSourceJoinAndProjectionContradictions) {
  const M1InnerRow row = evaluate_m1_inner_row(make_passing_inner_row_input());
  const std::string canonical =
      materialize_m1_inner_row(row, make_passing_observation_snapshot(row));
  const auto rewrite_list =
      [&canonical](
          std::size_t field_index,
          const std::function<void(std::vector<std::string>*)>& rewrite) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(canonical);
        std::vector<std::string> records =
            parse_b1_framed_list(manifest.fields[field_index].payload);
        rewrite(&records);
        manifest.fields[field_index].payload =
            encode_m1_test_record_list(records);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      };

  const std::vector<std::string> tampered{
      rewrite_list(
          5U, [](std::vector<std::string>* records) { records->pop_back(); }),
      rewrite_list(5U,
                   [](std::vector<std::string>* records) {
                     (*records)[1U] = (*records)[0U];
                   }),
      rewrite_list(5U,
                   [](std::vector<std::string>* records) {
                     std::swap((*records)[0U], (*records)[1U]);
                   }),
      rewrite_list(5U,
                   [](std::vector<std::string>* records) {
                     std::vector<std::string> source =
                         parse_b1_fixed_record((*records)[1U], 5U);
                     source[1U] = "1";
                     (*records)[1U] = encode_b1_fixed_record(source);
                   }),
      rewrite_list(4U,
                   [](std::vector<std::string>* records) {
                     std::vector<std::string> occurrence =
                         parse_b1_fixed_record((*records)[8U], 18U);
                     occurrence[8U] = std::to_string(
                         parse_b1_canonical_uint64(occurrence[8U]) + 1U);
                     (*records)[8U] = encode_b1_fixed_record(occurrence);
                   }),
      [&canonical]() {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(canonical);
        std::vector<std::string> first =
            parse_b1_fixed_record(manifest.fields[8U].payload, 12U);
        first[3U] = std::to_string(parse_b1_canonical_uint64(first[3U]) + 1U);
        first[6U] = first[3U];
        manifest.fields[8U].payload = encode_b1_fixed_record(first);
        manifest.fields[19U].payload = encode_b1_fixed_record(
            {"invalid", "invalid", "invalid", "invalid", "invalid", "invalid"});
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      }(),
      [&canonical]() {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(canonical);
        std::vector<std::string> occurrences =
            parse_b1_framed_list(manifest.fields[4U].payload);
        constexpr std::size_t final_warmup_index =
            kM1ColdI1OriginCount + kM1WarmupI1OriginCount - 1U;
        std::vector<std::string> final_warmup =
            parse_b1_fixed_record(occurrences[final_warmup_index], 18U);
        final_warmup[16U] = "false";
        occurrences[final_warmup_index] = encode_b1_fixed_record(final_warmup);
        manifest.fields[4U].payload = encode_m1_test_record_list(occurrences);
        manifest.fields[19U].payload = encode_b1_fixed_record(
            {"invalid", "invalid", "invalid", "invalid", "invalid", "invalid"});
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      }(),
      rewrite_list(
          13U, [](std::vector<std::string>* records) { records->pop_back(); }),
      rewrite_list(13U,
                   [](std::vector<std::string>* records) {
                     (*records)[1U] = (*records)[0U];
                   }),
      rewrite_list(13U,
                   [](std::vector<std::string>* records) {
                     std::swap((*records)[0U], (*records)[1U]);
                   }),
      rewrite_list(13U,
                   [](std::vector<std::string>* records) {
                     std::vector<std::string> source =
                         parse_b1_fixed_record((*records)[1U], 13U);
                     source[5U] = source[5U] == "true" ? "false" : "true";
                     (*records)[1U] = encode_b1_fixed_record(source);
                   }),
      [&canonical]() {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(canonical);
        std::vector<std::string> waste =
            parse_b1_fixed_record(manifest.fields[18U].payload, 5U);
        waste[1U] = std::to_string(parse_b1_canonical_uint64(waste[1U]) + 1U);
        manifest.fields[18U].payload = encode_b1_fixed_record(waste);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      }()};
  for (std::size_t index = 0U; index < tampered.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_THROW(parse_and_recompute_m1_inner_row(tampered[index], 1U),
                 std::invalid_argument);
  }
}

/**
 * @brief Proves strict replay rejects duration rescaling and stale verdicts.
 * @throws GoogleTest assertion control and canonical/evaluator failures.
 */
TEST(M1Profile, RejectsCanonicalDurationAndVerdictRehashContradictions) {
  const M1InnerRow row = evaluate_m1_inner_row(make_passing_inner_row_input());
  const std::string canonical =
      materialize_m1_inner_row(row, make_passing_observation_snapshot(row));
  const auto rewrite_progress =
      [](const std::string& source,
         const std::function<void(std::vector<std::string>*)>& rewrite) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(source);
        std::vector<std::string> records =
            parse_b1_framed_list(manifest.fields[9U].payload);
        rewrite(&records);
        manifest.fields[9U].payload = encode_m1_test_record_list(records);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      };
  const auto set_duration = [](std::vector<std::string>* records,
                               std::size_t index, std::string duration) {
    std::vector<std::string> fields =
        parse_b1_fixed_record((*records)[index], 3U);
    fields[2U] = std::move(duration);
    (*records)[index] = encode_b1_fixed_record(fields);
  };

  const std::vector<std::string> duration_tampering{
      rewrite_progress(canonical,
                       [&](std::vector<std::string>* records) {
                         for (std::size_t index = 0U; index < records->size();
                              ++index) {
                           set_duration(records, index, "500000000");
                         }
                       }),
      rewrite_progress(canonical,
                       [&](std::vector<std::string>* records) {
                         for (std::size_t index = 0U; index < records->size();
                              ++index) {
                           set_duration(records, index, "2000000000");
                         }
                       }),
      rewrite_progress(canonical,
                       [&](std::vector<std::string>* records) {
                         set_duration(records, 17U, "500000000");
                       }),
      rewrite_progress(canonical, [&](std::vector<std::string>* records) {
        for (std::size_t index : {0U, 1U}) {
          std::vector<std::string> fields =
              parse_b1_fixed_record((*records)[index], 3U);
          fields[1U] = "100000";
          fields[2U] = "500000000";
          (*records)[index] = encode_b1_fixed_record(fields);
        }
      })};
  for (const std::string& tampered : duration_tampering) {
    EXPECT_THROW(parse_and_recompute_m1_inner_row(tampered, 1U),
                 std::invalid_argument);
  }

  const std::string stale_progress =
      rewrite_progress(canonical, [](std::vector<std::string>* records) {
        for (std::size_t index : {0U, 1U}) {
          std::vector<std::string> fields =
              parse_b1_fixed_record((*records)[index], 3U);
          fields[1U] = "0";
          (*records)[index] = encode_b1_fixed_record(fields);
        }
      });
  EXPECT_THROW(parse_and_recompute_m1_inner_row(stale_progress, 1U),
               std::invalid_argument);

  B1CanonicalManifest stale_verdict = parse_b1_canonical_manifest(canonical);
  stale_verdict.fields[19U].payload =
      encode_b1_fixed_record({"pass", "pass", "fail", "pass", "pass", "pass"});
  EXPECT_THROW(parse_and_recompute_m1_inner_row(
                   encode_b1_canonical_manifest(stale_verdict.schema,
                                                stale_verdict.fields),
                   1U),
               std::invalid_argument);
}

/**
 * @brief Proves protocol and every independently recomputed axis reject raw
 * tampering while stale retained Pass verdicts remain unchanged.
 * @throws GoogleTest assertion control and canonical/evaluator failures.
 */
TEST(M1Profile, RejectsProtocolAndFiveAxisRawTampering) {
  const M1InnerRow row = evaluate_m1_inner_row(make_passing_inner_row_input());
  const std::string canonical =
      materialize_m1_inner_row(row, make_passing_observation_snapshot(row));
  using ManifestMutation = std::function<void(B1CanonicalManifest*)>;
  const auto mutate = [&canonical](const ManifestMutation& mutation) {
    B1CanonicalManifest manifest = parse_b1_canonical_manifest(canonical);
    mutation(&manifest);
    return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
  };
  const auto mutate_list_record =
      [](B1CanonicalManifest* manifest, std::size_t field_index,
         std::size_t record_index,
         const std::function<void(std::vector<std::string>*)>& rewrite) {
        std::vector<std::string> records =
            parse_b1_framed_list(manifest->fields[field_index].payload);
        std::vector<std::string> fields = parse_b1_fixed_record(
            records[record_index], field_index == 4U    ? 18U
                                   : field_index == 10U ? 4U
                                                        : 11U);
        rewrite(&fields);
        records[record_index] = encode_b1_fixed_record(fields);
        manifest->fields[field_index].payload =
            encode_m1_test_record_list(records);
      };

  const std::vector<std::string> tampered{
      mutate([](B1CanonicalManifest* manifest) {
        std::vector<std::string> flags =
            parse_b1_fixed_record(manifest->fields[3U].payload, 12U);
        flags[0U] = "false";
        manifest->fields[3U].payload = encode_b1_fixed_record(flags);
      }),
      mutate([&mutate_list_record](B1CanonicalManifest* manifest) {
        mutate_list_record(manifest, 4U, 8U,
                           [](std::vector<std::string>* fields) {
                             (*fields)[7U] = "200000000";
                           });
      }),
      mutate([](B1CanonicalManifest* manifest) {
        std::vector<std::string> records =
            parse_b1_framed_list(manifest->fields[9U].payload);
        for (std::size_t index : {0U, 1U}) {
          std::vector<std::string> fields =
              parse_b1_fixed_record(records[index], 3U);
          fields[1U] = "0";
          records[index] = encode_b1_fixed_record(fields);
        }
        manifest->fields[9U].payload = encode_m1_test_record_list(records);
      }),
      mutate([&mutate_list_record](B1CanonicalManifest* manifest) {
        for (std::size_t index : {0U, 1U}) {
          mutate_list_record(manifest, 10U, index,
                             [](std::vector<std::string>* fields) {
                               (*fields)[2U] = "100";
                               (*fields)[3U] = "1";
                             });
        }
      }),
      mutate([](B1CanonicalManifest* manifest) {
        std::vector<std::string> waste =
            parse_b1_fixed_record(manifest->fields[18U].payload, 5U);
        waste[1U] = "1";
        manifest->fields[18U].payload = encode_b1_fixed_record(waste);
      }),
      mutate([&mutate_list_record](B1CanonicalManifest* manifest) {
        mutate_list_record(
            manifest, 14U, 1U, [](std::vector<std::string>* snapshot) {
              std::vector<std::string> high_water =
                  parse_b1_fixed_record((*snapshot)[2U], 5U);
              high_water[0U] = "1000";
              (*snapshot)[2U] = encode_b1_fixed_record(high_water);
            });
      })};
  for (std::size_t index = 0U; index < tampered.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_THROW(parse_and_recompute_m1_inner_row(tampered[index], 1U),
                 std::invalid_argument);
  }
}

/**
 * @brief Proves a short accepted/settled I/O task entirely between sparse M1
 * cuts still contributes to event-aligned high-water.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, DerivesIoHighWaterBetweenSparseTemporalCuts) {
  M1InnerRowInput input = make_passing_inner_row_input();
  ASSERT_TRUE(std::all_of(
      input.temporal_snapshots.begin(), input.temporal_snapshots.end(),
      [](const M1ExecutionSnapshot& snapshot) {
        return snapshot.compute_io.active_tasks == 0U &&
               snapshot.compute_io.active_planned_bytes == 0U;
      }));

  const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(row.compute_io_task_high_water, 1U);
  EXPECT_EQ(row.compute_io_planned_byte_high_water, kB1PayloadBytes);
}

/**
 * @brief Rejects every Host/device high-water lower bound and device decline.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsUnderreportedHostAndDeviceLifetimeHighWater) {
  const std::array<std::uint64_t ResourceVector::*, 5U> host_dimensions{
      &ResourceVector::cpu_slots, &ResourceVector::retained_memory_bytes,
      &ResourceVector::scratch_bytes, &ResourceVector::ready_entries,
      &ResourceVector::ready_bytes};
  for (std::uint64_t ResourceVector::* dimension : host_dimensions) {
    M1InnerRowInput input = make_passing_inner_row_input();
    ResourceLedger::Snapshot& active =
        input.temporal_snapshots[1U].host_resources;
    ASSERT_GT(active.reserved.*dimension, 0U);
    active.high_water.*dimension = active.reserved.*dimension - 1U;
    EXPECT_EQ(evaluate_m1_inner_row(std::move(input)).memory_verdict,
              I1Verdict::Invalid);
  }

  const std::array<std::uint64_t DeviceResourceVector::*, 2U> device_dimensions{
      &DeviceResourceVector::device_memory_bytes,
      &DeviceResourceVector::device_scratch_bytes};
  for (std::uint64_t DeviceResourceVector::* dimension : device_dimensions) {
    M1InnerRowInput lower_bound = make_passing_inner_row_input();
    ResourceLedger::DeviceSnapshot& active =
        lower_bound.temporal_snapshots[1U].device_resources.front();
    ASSERT_GT(active.reserved.*dimension, 0U);
    active.high_water.*dimension = active.reserved.*dimension - 1U;
    EXPECT_EQ(evaluate_m1_inner_row(std::move(lower_bound)).memory_verdict,
              I1Verdict::Invalid);

    M1InnerRowInput decreasing = make_passing_inner_row_input();
    ResourceLedger::DeviceSnapshot& prior =
        decreasing.temporal_snapshots[1U].device_resources.front();
    ResourceLedger::DeviceSnapshot& current =
        decreasing.temporal_snapshots[2U].device_resources.front();
    ASSERT_GT(prior.high_water.*dimension, current.reserved.*dimension);
    current.high_water.*dimension = current.reserved.*dimension;
    EXPECT_EQ(evaluate_m1_inner_row(std::move(decreasing)).memory_verdict,
              I1Verdict::Invalid);
  }
}

/**
 * @brief Rejects canonical Host/device lower bounds and device high-water drop.
 * @throws GoogleTest assertion control and canonical/evaluator failures.
 */
TEST(M1Profile, RejectsCanonicalUnderreportedLifetimeHighWater) {
  const M1InnerRow row = evaluate_m1_inner_row(make_passing_inner_row_input());
  const std::string canonical =
      materialize_m1_inner_row(row, make_passing_observation_snapshot(row));
  const auto rewrite_snapshot =
      [&canonical](
          std::size_t snapshot_index,
          const std::function<void(std::vector<std::string>*)>& rewrite) {
        B1CanonicalManifest manifest = parse_b1_canonical_manifest(canonical);
        std::vector<std::string> snapshots =
            parse_b1_framed_list(manifest.fields[14U].payload);
        std::vector<std::string> fields =
            parse_b1_fixed_record(snapshots[snapshot_index], 11U);
        rewrite(&fields);
        snapshots[snapshot_index] = encode_b1_fixed_record(fields);
        manifest.fields[14U].payload = encode_m1_test_record_list(snapshots);
        return encode_b1_canonical_manifest(manifest.schema, manifest.fields);
      };
  const auto rewrite_device = [&rewrite_snapshot](std::size_t snapshot_index,
                                                  std::size_t component_index,
                                                  std::string replacement) {
    return rewrite_snapshot(
        snapshot_index, [component_index, replacement = std::move(replacement)](
                            std::vector<std::string>* snapshot) {
          std::vector<std::string> devices =
              parse_b1_framed_list((*snapshot)[3U]);
          std::vector<std::string> device =
              parse_b1_fixed_record(devices.front(), 10U);
          device[component_index] = replacement;
          devices.front() = encode_b1_fixed_record(device);
          (*snapshot)[3U] = encode_m1_test_record_list(devices);
        });
  };

  const std::vector<std::string> tampered{
      rewrite_snapshot(1U,
                       [](std::vector<std::string>* snapshot) {
                         std::vector<std::string> reserved =
                             parse_b1_fixed_record((*snapshot)[1U], 5U);
                         std::vector<std::string> high_water =
                             parse_b1_fixed_record((*snapshot)[2U], 5U);
                         high_water[0U] = std::to_string(
                             parse_b1_canonical_uint64(reserved[0U]) - 1U);
                         (*snapshot)[2U] = encode_b1_fixed_record(high_water);
                       }),
      rewrite_device(1U, 8U, "4095"), rewrite_device(2U, 9U, "2048")};
  for (std::size_t index = 0U; index < tampered.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_THROW(parse_and_recompute_m1_inner_row(tampered[index], 1U),
                 std::invalid_argument);
  }
}

/**
 * @brief Proves latency, B1 waste, and final settlement fail independently.
 * @throws GoogleTest assertion control and row evaluation allocation failures.
 */
TEST(M1Profile, KeepsFiveAxisFailuresIndependent) {
  M1InnerRowInput latency_input = make_passing_inner_row_input();
  latency_input.paired_isolated_i1_p99 = std::chrono::milliseconds(4);
  const M1InnerRow latency = evaluate_m1_inner_row(std::move(latency_input));
  EXPECT_EQ(latency.latency_verdict, I1Verdict::Fail);
  EXPECT_EQ(latency.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(latency.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(latency.memory_verdict, I1Verdict::Pass);

  M1InnerRowInput waste_input = make_passing_inner_row_input();
  const auto measured_source = std::find_if(
      waste_input.batch_sources.begin(), waste_input.batch_sources.end(),
      [](const M1BatchSourceEvidence& source) {
        return source.job.phase == B1JobPhase::Measured;
      });
  ASSERT_NE(measured_source, waste_input.batch_sources.end());
  ASSERT_TRUE(measured_source->output_receipt.has_value());
  measured_source->output_receipt->commit_id.push_back('x');
  measured_source->verified_endpoint = false;
  waste_input.batch_waste =
      derive_m1_batch_waste_evidence(waste_input.batch_sources);
  testing::attach_m1_test_source_fairness_projection(&waste_input);
  const M1InnerRow waste = evaluate_m1_inner_row(std::move(waste_input));
  EXPECT_EQ(waste.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(waste.waste_verdict, I1Verdict::Fail);
  EXPECT_EQ(waste.memory_verdict, I1Verdict::Pass);

  M1InnerRowInput memory_input = make_passing_inner_row_input();
  memory_input.temporal_snapshots.back().host_resources.reserved.cpu_slots = 1U;
  const M1InnerRow memory = evaluate_m1_inner_row(std::move(memory_input));
  EXPECT_EQ(memory.latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(memory.waste_verdict, I1Verdict::Pass);
  EXPECT_EQ(memory.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(memory.overall_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves M1 rejects every incomplete or contradictory raw I/O stream.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsMissingDuplicateMalformedAndOverlimitIoEvidence) {
  M1InnerRowInput missing = make_passing_inner_row_input();
  missing.batch_sources.pop_back();
  EXPECT_EQ(evaluate_m1_inner_row(std::move(missing)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput duplicate = make_passing_inner_row_input();
  ASSERT_GE(duplicate.batch_sources.size(), 2U);
  const std::uint64_t duplicate_sequence =
      duplicate.batch_sources[0U].io_observations[1U].admission_event->sequence;
  auto& duplicate_stream = duplicate.batch_sources[1U].io_observations;
  duplicate_stream[1U].admission_event->sequence = duplicate_sequence;
  duplicate_stream[2U].admission_event->sequence = duplicate_sequence;
  duplicate_stream[2U].settlement_event->admission_sequence =
      duplicate_sequence;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(duplicate)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput reordered = make_passing_inner_row_input();
  std::swap(reordered.batch_sources[0U].io_observations[1U],
            reordered.batch_sources[0U].io_observations[2U]);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(reordered)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput arithmetic = make_passing_inner_row_input();
  auto& arithmetic_stream = arithmetic.batch_sources[0U].io_observations;
  --arithmetic_stream[1U].admission_event->charged_planned_bytes;
  --arithmetic_stream[2U].admission_event->charged_planned_bytes;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(arithmetic)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput unknown = make_passing_inner_row_input();
  unknown.batch_sources[0U].io_observations[1U].point =
      static_cast<B1IoObservationPoint>(255U);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(unknown)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput overlimit = make_passing_inner_row_input();
  auto& overlimit_stream = overlimit.batch_sources[0U].io_observations;
  auto invalid_snapshot = overlimit_stream[1U].snapshot;
  invalid_snapshot.active_tasks = kB1ComputeIoTaskLimit + 1U;
  invalid_snapshot.queued_tasks = kB1ComputeIoTaskLimit + 1U;
  overlimit_stream[1U].snapshot = invalid_snapshot;
  overlimit_stream[1U].admission_event->snapshot_after = invalid_snapshot;
  overlimit_stream[2U].admission_event->snapshot_after = invalid_snapshot;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(overlimit)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput nonzero_final = make_passing_inner_row_input();
  nonzero_final.temporal_snapshots.back().compute_io.active_tasks = 1U;
  nonzero_final.temporal_snapshots.back().compute_io.queued_tasks = 1U;
  nonzero_final.temporal_snapshots.back().compute_io.active_planned_bytes = 1U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(nonzero_final)).memory_verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves lifecycle pages fail closed for continuity and post-stop drift.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsLifecyclePageContinuityAndPostStopEvidence) {
  M1InnerRowInput empty = make_passing_inner_row_input();
  for (std::size_t index = 0U; index < empty.temporal_snapshots.size();
       ++index) {
    set_m1_lifecycle_page(&empty.temporal_snapshots[index], index, 0U, 0U, {});
  }
  EXPECT_EQ(evaluate_m1_inner_row(std::move(empty)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput missing_page = make_passing_inner_row_input();
  missing_page.temporal_snapshots.erase(
      missing_page.temporal_snapshots.begin() + 1);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(missing_page)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput missing_record = make_passing_inner_row_input();
  missing_record.temporal_snapshots[1U].lifecycle.records.erase(
      missing_record.temporal_snapshots[1U].lifecycle.records.begin() + 2);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(missing_record)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput duplicate = make_passing_inner_row_input();
  duplicate.temporal_snapshots[1U].lifecycle.records[1U].sequence = 3U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(duplicate)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput reordered = make_passing_inner_row_input();
  std::swap(reordered.temporal_snapshots[1U].lifecycle.records[0U],
            reordered.temporal_snapshots[1U].lifecycle.records[1U]);
  EXPECT_EQ(evaluate_m1_inner_row(std::move(reordered)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput broken_cursor = make_passing_inner_row_input();
  broken_cursor.temporal_snapshots[2U].lifecycle_after_cursor = 7U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(broken_cursor)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput broken_cut = make_passing_inner_row_input();
  broken_cut.temporal_snapshots[2U].lifecycle.snapshot_cut = 9U;
  broken_cut.temporal_snapshots[2U].lifecycle.next_cursor = 9U;
  broken_cut.temporal_snapshots[2U].lifecycle.next_sequence = 10U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(broken_cut)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput broken_identity = make_passing_inner_row_input();
  broken_identity.temporal_snapshots[0U]
      .lifecycle.records[1U]
      .graph_instance_id = 0U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(broken_identity)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput missing_stop = make_passing_inner_row_input();
  compute::ExecutionLifecyclePage& stopping_page =
      missing_stop.temporal_snapshots.back().lifecycle;
  stopping_page.records.pop_back();
  stopping_page.snapshot_cut = 30U;
  stopping_page.next_cursor = 30U;
  stopping_page.next_sequence = 31U;
  stopping_page.service_state =
      compute::ExecutionLifecycleServiceState::Stopping;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(missing_stop)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput post_stop = make_passing_inner_row_input();
  const M1ExecutionSnapshot& stopped = post_stop.temporal_snapshots.back();
  M1ExecutionSnapshot after_stop = stopped;
  set_m1_lifecycle_page(
      &after_stop, 4U, 31U, 32U,
      {make_m1_lifecycle_event(
          32U, compute::ExecutionLifecycleEventKind::GraphRegistered,
          stopped.lifecycle.counters, 3U)});
  after_stop.lifecycle.next_sequence =
      std::numeric_limits<std::uint64_t>::max();
  after_stop.lifecycle.shutdown_generation = 77U;
  after_stop.lifecycle.service_state =
      compute::ExecutionLifecycleServiceState::Stopped;
  post_stop.temporal_snapshots.push_back(std::move(after_stop));
  const M1InnerRow post_stop_row = evaluate_m1_inner_row(std::move(post_stop));
  EXPECT_EQ(post_stop_row.memory_verdict, I1Verdict::Invalid);
  EXPECT_TRUE(std::any_of(
      post_stop_row.validity_reasons.begin(),
      post_stop_row.validity_reasons.end(), [](const std::string& reason) {
        return reason.find("ordinary event appears after ServiceStopped") !=
               std::string::npos;
      }));
}

/**
 * @brief Proves a producer-faithful group/rollback/interleaving replay passes.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, AcceptsProducerFaithfulLifecycleReplay) {
  M1InnerRowInput input = make_passing_inner_row_input();
  const auto& rollback =
      require_m1_lifecycle_event(&input.temporal_snapshots, 6U);
  const auto& group = require_m1_lifecycle_event(&input.temporal_snapshots, 8U);
  const auto& standalone_terminal =
      require_m1_lifecycle_event(&input.temporal_snapshots, 14U);
  const auto& group_quiescent =
      require_m1_lifecycle_event(&input.temporal_snapshots, 16U);
  EXPECT_EQ(rollback.kind,
            compute::ExecutionLifecycleEventKind::CandidateRolledBack);
  EXPECT_NE(group.run_group_id, 0U);
  EXPECT_EQ(standalone_terminal.run_group_id, 0U);
  EXPECT_NE(group_quiescent.run_group_id, 0U);

  const M1InnerRow row = evaluate_m1_inner_row(std::move(input));
  EXPECT_EQ(row.memory_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves all nine registry-derived fields are replayed exactly.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsEveryRegistryCounterMutation) {
  using Counters = compute::ExecutionLifecycleCounters;
  const std::array<std::uint64_t Counters::*, 9U> registry_fields{
      &Counters::registered_graph_count,
      &Counters::open_graph_count,
      &Counters::closing_graph_count,
      &Counters::pending_candidate_count,
      &Counters::admitted_standalone_run_count,
      &Counters::admitted_run_group_count,
      &Counters::admitted_child_run_count,
      &Counters::terminal_not_quiescent_run_count,
      &Counters::finalizing_run_count};

  for (std::size_t index = 0U; index < registry_fields.size(); ++index) {
    SCOPED_TRACE(index);
    M1InnerRowInput input = make_passing_inner_row_input();
    compute::ExecutionLifecycleEvent& origin =
        require_m1_lifecycle_event(&input.temporal_snapshots, 1U);
    origin.counters.*registry_fields[index] = 1U;
    EXPECT_EQ(evaluate_m1_inner_row(std::move(input)).memory_verdict,
              I1Verdict::Invalid);
  }
}

/**
 * @brief Proves causal, identity, group, and rollback corruption fails closed.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsLifecycleCausalAndIdentitySplices) {
  M1InnerRowInput causal = make_passing_inner_row_input();
  require_m1_lifecycle_event(&causal.temporal_snapshots, 15U).kind =
      compute::ExecutionLifecycleEventKind::ResourceSettled;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(causal)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput graph_association = make_passing_inner_row_input();
  require_m1_lifecycle_event(&graph_association.temporal_snapshots, 14U)
      .graph_instance_id = 1U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(graph_association)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput cross_run = make_passing_inner_row_input();
  require_m1_lifecycle_event(&cross_run.temporal_snapshots, 18U).run_id = 11U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(cross_run)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput group_admission = make_passing_inner_row_input();
  --require_m1_lifecycle_event(&group_admission.temporal_snapshots, 8U)
        .counters.admitted_child_run_count;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(group_admission)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput rollback = make_passing_inner_row_input();
  require_m1_lifecycle_event(&rollback.temporal_snapshots, 6U).generation =
      999U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(rollback)).memory_verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves the six physical fields obey only conservative producer facts.
 * @throws GoogleTest assertion control and row-evaluation allocation failures.
 */
TEST(M1Profile, RejectsImpossibleLifecyclePhysicalFacts) {
  M1InnerRowInput ready_limit = make_passing_inner_row_input();
  const std::uint64_t ready_entry_limit =
      ready_limit.temporal_snapshots.front()
          .host_resources.limits.ready_entries;
  require_m1_lifecycle_event(&ready_limit.temporal_snapshots, 9U)
      .counters.ready_entry_count = ready_entry_limit + 1U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(ready_limit)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput grant_bound = make_passing_inner_row_input();
  require_m1_lifecycle_event(&grant_bound.temporal_snapshots, 9U)
      .counters.ready_entry_count = 4U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(grant_bound)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput root_reachability = make_passing_inner_row_input();
  require_m1_lifecycle_event(&root_reachability.temporal_snapshots, 9U)
      .counters.live_root_reservation_count = 0U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(root_reachability)).memory_verdict,
            I1Verdict::Invalid);

  M1InnerRowInput binding_reachability = make_passing_inner_row_input();
  compute::ExecutionLifecycleCounters& binding_counters =
      require_m1_lifecycle_event(&binding_reachability.temporal_snapshots, 9U)
          .counters;
  binding_counters.live_policy_invocation_count = 1U;
  binding_counters.live_policy_binding_count = 0U;
  EXPECT_EQ(
      evaluate_m1_inner_row(std::move(binding_reachability)).memory_verdict,
      I1Verdict::Invalid);

  M1InnerRowInput admission_reachability = make_passing_inner_row_input();
  require_m1_lifecycle_event(&admission_reachability.temporal_snapshots, 2U)
      .counters.live_root_reservation_count = 1U;
  EXPECT_EQ(
      evaluate_m1_inner_row(std::move(admission_reachability)).memory_verdict,
      I1Verdict::Invalid);

  M1InnerRowInput final_zero = make_passing_inner_row_input();
  require_m1_lifecycle_event(&final_zero.temporal_snapshots, 31U)
      .counters.live_policy_binding_count = 1U;
  EXPECT_EQ(evaluate_m1_inner_row(std::move(final_zero)).memory_verdict,
            I1Verdict::Invalid);
}

/**
 * @brief Proves missing temporal or phase-attribution evidence is Invalid.
 * @throws GoogleTest assertion control and row evaluation allocation failures.
 */
TEST(M1Profile, InvalidatesMissingTemporalAndAttributionEvidence) {
  M1InnerRowInput temporal = make_passing_inner_row_input();
  temporal.temporal_snapshots.resize(3U);
  const M1InnerRow temporal_row = evaluate_m1_inner_row(std::move(temporal));
  EXPECT_EQ(temporal_row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(temporal_row.overall_verdict, I1Verdict::Invalid);

  M1InnerRowInput attribution = make_passing_inner_row_input();
  attribution.occurrence_attribution_proved = false;
  const M1InnerRow attribution_row =
      evaluate_m1_inner_row(std::move(attribution));
  EXPECT_EQ(attribution_row.latency_verdict, I1Verdict::Invalid);
  EXPECT_EQ(attribution_row.throughput_progress_verdict, I1Verdict::Invalid);
  EXPECT_EQ(attribution_row.fairness_verdict, I1Verdict::Invalid);
  EXPECT_EQ(attribution_row.waste_verdict, I1Verdict::Invalid);
  EXPECT_EQ(attribution_row.memory_verdict, I1Verdict::Pass);
  EXPECT_EQ(attribution_row.overall_verdict, I1Verdict::Invalid);

  M1InnerRowInput temporal_effects = make_passing_inner_row_input();
  temporal_effects.temporal_effects_complete = false;
  const M1InnerRow effects_row =
      evaluate_m1_inner_row(std::move(temporal_effects));
  EXPECT_EQ(effects_row.fairness_verdict, I1Verdict::Invalid);
  EXPECT_EQ(effects_row.memory_verdict, I1Verdict::Invalid);
  EXPECT_EQ(effects_row.overall_verdict, I1Verdict::Invalid);
}

/**
 * @brief Proves every exact fairness threshold and guard passes independently.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, PassesCompleteIndependentFairnessGuards) {
  const M1FairnessSummary summary =
      evaluate_m1_fairness(make_passing_fairness_input());
  ASSERT_TRUE(summary.throughput_progress_p05.has_value());
  EXPECT_DOUBLE_EQ(*summary.throughput_progress_p05,
                   kM1ThroughputProgressP05Floor);
  ASSERT_TRUE(summary.graph_jain_p05.has_value());
  EXPECT_DOUBLE_EQ(*summary.graph_jain_p05, 1.0);
  EXPECT_EQ(summary.maximum_interactive_burst, kM1InteractiveBurstLimit);
  EXPECT_TRUE(summary.validity_reasons.empty());
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.graph_jain_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.interactive_headroom_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.interactive_latency_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves nearest-rank p05 exposes starving windows despite high average.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, ProgressP05FailsDespitePassingOverallAverage) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.progress_windows[0U].successful_site_operations = 0U;
  input.progress_windows[1U].successful_site_operations = 190000U;
  for (std::size_t index = 2U; index < input.progress_windows.size(); ++index) {
    input.progress_windows[index].successful_site_operations = 1000000U;
  }
  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  ASSERT_TRUE(summary.throughput_progress_p05.has_value());
  EXPECT_DOUBLE_EQ(*summary.throughput_progress_p05, 0.19);
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves positive non-one-second windows cannot rescale M1 progress.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, InvalidatesEveryNonOneSecondProgressDuration) {
  const auto expect_invalid = [](M1FairnessEvidenceInput input) {
    const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
    EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Invalid);
    EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Invalid);
  };

  M1FairnessEvidenceInput half_second = make_passing_fairness_input();
  for (M1ThroughputProgressSample& window : half_second.progress_windows) {
    window.duration = std::chrono::milliseconds(500);
  }
  expect_invalid(std::move(half_second));

  M1FairnessEvidenceInput two_seconds = make_passing_fairness_input();
  for (M1ThroughputProgressSample& window : two_seconds.progress_windows) {
    window.duration = std::chrono::seconds(2);
  }
  expect_invalid(std::move(two_seconds));

  M1FairnessEvidenceInput mixed = make_passing_fairness_input();
  mixed.progress_windows[17U].duration = std::chrono::milliseconds(500);
  expect_invalid(std::move(mixed));

  M1FairnessEvidenceInput ratio_flip = make_passing_fairness_input();
  for (std::size_t index : {0U, 1U}) {
    ratio_flip.progress_windows[index].successful_site_operations = 100000U;
    ratio_flip.progress_windows[index].duration =
        std::chrono::milliseconds(500);
  }
  expect_invalid(std::move(ratio_flip));
}

/**
 * @brief Proves peer service and class-start failures remain independent.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, FailsPeerJainAndFourInteractiveBurstIndependently) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  for (M1GraphServiceWindow& window : input.graph_service_windows) {
    window.graph_a_completed_service = 100U;
    window.graph_b_completed_service = 1U;
  }
  input.class_starts.clear();
  for (std::size_t index = 0U; index < 4U; ++index) {
    input.class_starts.push_back(
        M1ClassStartSample{index + 1U, compute::ComputeRunQosClass::Interactive,
                           true, true, true});
  }
  input.class_starts.push_back(M1ClassStartSample{
      5U, compute::ComputeRunQosClass::Throughput, true, true, true});

  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  ASSERT_TRUE(summary.graph_jain_p05.has_value());
  EXPECT_LT(*summary.graph_jain_p05, kM1GraphJainP05Floor);
  EXPECT_EQ(summary.graph_jain_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.maximum_interactive_burst, 4U);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves a fourth Interactive start counts only at a real dual-start
 * cut.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 * @note Nominal offer overlap is deliberately absent from the evidence model;
 * the negative case differs only in product-authored Throughput startability.
 */
TEST(M1Profile, ExcludesFourthStartWhenThroughputWasNotReallyStartable) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.class_starts = {
      {1U, compute::ComputeRunQosClass::Interactive, true, true, true},
      {2U, compute::ComputeRunQosClass::Interactive, true, true, true},
      {3U, compute::ComputeRunQosClass::Interactive, true, true, true},
      {4U, compute::ComputeRunQosClass::Interactive, true, false, true},
      {5U, compute::ComputeRunQosClass::Throughput, true, true, true}};

  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  EXPECT_EQ(summary.maximum_interactive_burst, 3U);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Pass);
}

/**
 * @brief Proves headroom and latency failures cannot substitute for each other.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, KeepsHeadroomAndLatencyVerdictsNonSubstitutable) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.headroom_admissions.throughput_headroom_failures = 1U;
  input.headroom_outcomes.front().host_status = OperationStatus{
      false, OperationErrorDomain::Graph, 1, "headroom", "headroom"};
  input.headroom_outcomes.front().throughput_headroom_failure = true;
  input.interactive_latency_verdict = I1Verdict::Fail;
  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.graph_jain_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Pass);
  EXPECT_EQ(summary.interactive_headroom_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.interactive_latency_verdict, I1Verdict::Fail);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Fail);
}

/**
 * @brief Proves missing, zero, and overflowed evidence fails closed as invalid.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, InvalidatesIncompleteAndOverflowedEvidence) {
  M1FairnessEvidenceInput input = make_passing_fairness_input();
  input.progress_windows.pop_back();
  input.graph_service_windows.front() = M1GraphServiceWindow{0U, true, 0U, 0U};
  input.class_starts.pop_back();
  input.headroom_admissions.classified_outcomes -= 1U;
  input.observation_overflowed = true;
  input.observation_sequence_exhausted = true;
  const M1FairnessSummary summary = evaluate_m1_fairness(std::move(input));
  EXPECT_EQ(summary.throughput_progress_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.graph_jain_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.class_start_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.interactive_headroom_verdict, I1Verdict::Invalid);
  EXPECT_EQ(summary.composite_fairness_verdict, I1Verdict::Invalid);
  EXPECT_FALSE(summary.validity_reasons.empty());
}

/**
 * @brief Proves unknown closed verdict and QoS representations fail closed.
 * @throws GoogleTest assertion control and evaluator allocation failures.
 */
TEST(M1Profile, UnknownClosedEnumsInvalidateCompositeEvidence) {
  M1FairnessEvidenceInput unknown_verdict = make_passing_fairness_input();
  unknown_verdict.interactive_latency_verdict = static_cast<I1Verdict>(255U);
  const M1FairnessSummary verdict_summary =
      evaluate_m1_fairness(std::move(unknown_verdict));
  EXPECT_EQ(verdict_summary.composite_fairness_verdict, I1Verdict::Invalid);
  EXPECT_NE(std::find(verdict_summary.validity_reasons.begin(),
                      verdict_summary.validity_reasons.end(),
                      "M1 fairness verdict contains an unknown value"),
            verdict_summary.validity_reasons.end());

  M1FairnessEvidenceInput unknown_class = make_passing_fairness_input();
  unknown_class.class_starts.insert(
      unknown_class.class_starts.begin(),
      M1ClassStartSample{1U, static_cast<compute::ComputeRunQosClass>(255U),
                         true, true, true});
  const M1FairnessSummary class_summary =
      evaluate_m1_fairness(std::move(unknown_class));
  EXPECT_EQ(class_summary.class_start_verdict, I1Verdict::Invalid);
  EXPECT_EQ(class_summary.composite_fairness_verdict, I1Verdict::Invalid);
  EXPECT_NE(std::find(class_summary.validity_reasons.begin(),
                      class_summary.validity_reasons.end(),
                      "M1 class-start evidence contains an unknown QoS class"),
            class_summary.validity_reasons.end());
}

/**
 * @brief Proves fanout allocates once and forwards one identical coordinate.
 * @throws GoogleTest assertion control and shared-owner allocation failures.
 */
TEST(M1Profile, ObservationFanoutUsesOnlySharedSequenceAuthority) {
  const compute::ComputeRunObservationCoordinate authority_coordinate{
      std::chrono::steady_clock::time_point(std::chrono::nanoseconds(17)), 41U};
  const compute::ComputeRunObservationCoordinate mirror_coordinate{
      std::chrono::steady_clock::time_point(std::chrono::nanoseconds(99)),
      700U};
  const auto authority =
      std::make_shared<RecordingObservationSink>(authority_coordinate, false);
  const auto mirror =
      std::make_shared<RecordingObservationSink>(mirror_coordinate, true);

  EXPECT_THROW(make_compute_run_observation_fanout(nullptr, mirror),
               std::invalid_argument);
  EXPECT_THROW(make_compute_run_observation_fanout(authority, authority),
               std::invalid_argument);
  const auto fanout = make_compute_run_observation_fanout(authority, mirror);
  ASSERT_NE(fanout, nullptr);
  EXPECT_TRUE(fanout->observes_task_semantics());

  const compute::ComputeRunObservationCoordinate reserved =
      fanout->reserve_causal_coordinate();
  EXPECT_EQ(reserved.observed_at, authority_coordinate.observed_at);
  EXPECT_EQ(reserved.causal_sequence, authority_coordinate.causal_sequence);
  EXPECT_EQ(authority->reservation_count, 1U);
  EXPECT_EQ(mirror->reservation_count, 0U);

  const compute::ComputeRunObservationCoordinate aborted =
      fanout->reserve_causal_coordinate();
  fanout->abort_causal_coordinate(aborted);
  EXPECT_EQ(authority->reservation_count, 2U);
  EXPECT_EQ(authority->abort_count, 1U);
  EXPECT_EQ(mirror->reservation_count, 0U);
  EXPECT_EQ(mirror->abort_count, 0U);

  compute::ComputeRun run(make_observer_submission(
      "m1-fanout", 60U, compute::ComputeRunQosClass::Interactive));
  fanout->on_service_start(
      run.descriptor(),
      compute::ComputeRunTaskIdentity(run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(1U)),
      8U, compute::ComputeRunServiceStartObservation{true, true, true},
      reserved);
  EXPECT_EQ(authority->callback_count, 1U);
  EXPECT_EQ(mirror->callback_count, 1U);
  EXPECT_EQ(authority->last_coordinate.observed_at,
            authority_coordinate.observed_at);
  EXPECT_EQ(mirror->last_coordinate.observed_at,
            authority_coordinate.observed_at);
  EXPECT_EQ(authority->last_coordinate.causal_sequence,
            authority_coordinate.causal_sequence);
  EXPECT_EQ(mirror->last_coordinate.causal_sequence,
            authority_coordinate.causal_sequence);
}

/**
 * @brief Proves every fanout product callback publishes mirror-first.
 *
 * One centralized spy exercises the complete closed callback set, including
 * the Value-bearing current-visible path. Every event must enter the mirror
 * before the sequence authority, retain the exact authority-owned coordinate,
 * and preserve the immutable Value revision in both child calls.
 *
 * @throws GoogleTest assertion, Value, and shared-owner allocation failures.
 */
TEST(M1Profile, ObservationFanoutDeliversEveryProductEventMirrorFirst) {
  const compute::ComputeRunObservationCoordinate coordinate{
      std::chrono::steady_clock::time_point(std::chrono::nanoseconds(23)), 47U};
  FanoutDeliveryLog log;
  const auto authority = std::make_shared<FanoutObservationSpySink>(
      FanoutRecipient::SequenceAuthority, &log, coordinate, true);
  const auto mirror = std::make_shared<FanoutObservationSpySink>(
      FanoutRecipient::Mirror, &log, coordinate, true);
  const auto fanout = make_compute_run_observation_fanout(authority, mirror);
  ASSERT_NE(fanout, nullptr);

  compute::ComputeRun run(make_observer_submission(
      "m1-fanout-order", 61U, compute::ComputeRunQosClass::Interactive));
  const compute::ComputeRunTaskIdentity task_identity(
      run.descriptor().id(), compute::ComputeRunLocalTaskId(1U));
  const Value output = testing::make_m1_test_output();
  const ValueRevisionId output_revision = output.revision_id();

  fanout->on_current_generation(run.descriptor().supersession(), coordinate);
  fanout->on_task_ready(run.descriptor(), task_identity,
                        compute::ComputeRunTaskReadyObservation{}, coordinate);
  fanout->on_service_start(
      run.descriptor(), task_identity, 8U,
      compute::ComputeRunServiceStartObservation{true, true, true}, coordinate);
  fanout->on_task_terminal(run.descriptor(), task_identity,
                           compute::ComputeRunTaskTerminalKind::Succeeded,
                           coordinate);
  fanout->on_cancellation(run.descriptor(),
                          compute::ComputeRunCancellationReason::Superseded,
                          coordinate);
  fanout->on_terminal(run.descriptor(),
                      compute::ComputeRunTerminalKind::Cancelled, coordinate);
  fanout->on_current_visible(run.descriptor(), output, coordinate);
  fanout->on_progressive_final_triggered(run.descriptor(), coordinate);
  fanout->on_run_quiescent(run.descriptor(), coordinate);
  fanout->on_run_resource_settled(run.descriptor(), coordinate);
  fanout->on_host_settled(coordinate);

  constexpr std::array<FanoutProductEvent, kFanoutProductEventCount>
      kExpectedEvents{
          FanoutProductEvent::CurrentGeneration,
          FanoutProductEvent::TaskReady,
          FanoutProductEvent::ServiceStart,
          FanoutProductEvent::TaskTerminal,
          FanoutProductEvent::Cancellation,
          FanoutProductEvent::Terminal,
          FanoutProductEvent::CurrentVisible,
          FanoutProductEvent::ProgressiveFinalTriggered,
          FanoutProductEvent::RunQuiescent,
          FanoutProductEvent::RunResourceSettled,
          FanoutProductEvent::HostSettled,
      };
  ASSERT_FALSE(log.overflowed());
  ASSERT_EQ(log.size(), kExpectedEvents.size() * 2U);
  for (std::size_t index = 0U; index < kExpectedEvents.size(); ++index) {
    const FanoutDeliveryRecord& mirror_record = log.at(index * 2U);
    const FanoutDeliveryRecord& authority_record = log.at(index * 2U + 1U);
    EXPECT_EQ(mirror_record.event, kExpectedEvents[index]);
    EXPECT_EQ(authority_record.event, kExpectedEvents[index]);
    EXPECT_EQ(mirror_record.recipient, FanoutRecipient::Mirror);
    EXPECT_EQ(authority_record.recipient, FanoutRecipient::SequenceAuthority);
    EXPECT_EQ(mirror_record.coordinate.observed_at, coordinate.observed_at);
    EXPECT_EQ(authority_record.coordinate.observed_at, coordinate.observed_at);
    EXPECT_EQ(mirror_record.coordinate.causal_sequence,
              coordinate.causal_sequence);
    EXPECT_EQ(authority_record.coordinate.causal_sequence,
              coordinate.causal_sequence);
  }
  const FanoutDeliveryRecord& mirror_visible = log.at(12U);
  const FanoutDeliveryRecord& authority_visible = log.at(13U);
  EXPECT_TRUE(mirror_visible.value_valid);
  EXPECT_TRUE(authority_visible.value_valid);
  EXPECT_EQ(mirror_visible.value_revision, output_revision);
  EXPECT_EQ(authority_visible.value_revision, output_revision);
  EXPECT_EQ(mirror->callback_count(), kFanoutProductEventCount);
  EXPECT_EQ(authority->callback_count(), kFanoutProductEventCount);
}

/**
 * @brief Proves a mirror-paused fanout callback keeps the M1 cut open.
 *
 * Current-generation publication first establishes one source record. A
 * worker then pauses at current-visible mirror entry before the wrapped I1
 * collector publishes. A boundary marker reserved and aborted during that
 * pause must leave the visible coordinate outstanding in M1, so the cut is
 * unstable and the source has no visible output. Releasing the mirror must
 * publish both sides at the same coordinate and close the cut exactly once.
 *
 * @throws GoogleTest assertion, thread, Value, observer, and snapshot
 * allocation failures.
 */
TEST(M1Profile, MirrorPausedFanoutPublicationKeepsObservationCutOpen) {
  M1FairnessObservationCollector mixed_collector(1U);
  I1EpisodeObservationCollector source_collector;
  const auto mixed_sink =
      mixed_collector.make_sink(M1ObservedRequestTag::Interactive);
  const auto source_sink = source_collector.make_edit_sink(0U);
  FanoutDeliveryLog log;
  const compute::ComputeRunObservationCoordinate unused_coordinate{};
  const auto authority = std::make_shared<FanoutObservationSpySink>(
      FanoutRecipient::SequenceAuthority, &log, unused_coordinate, true,
      mixed_sink);
  const auto mirror = std::make_shared<FanoutObservationSpySink>(
      FanoutRecipient::Mirror, &log, unused_coordinate, false, source_sink,
      true);
  const auto fanout = make_compute_run_observation_fanout(authority, mirror);
  ASSERT_NE(fanout, nullptr);

  compute::ComputeRun run(make_observer_submission(
      "m1-fanout-frontier", 62U, compute::ComputeRunQosClass::Interactive));
  const compute::ComputeRunObservationCoordinate current_coordinate =
      fanout->reserve_causal_coordinate();
  fanout->on_current_generation(run.descriptor().supersession(),
                                current_coordinate);
  const I1EpisodeObservationSnapshot source_before =
      source_collector.snapshot();
  ASSERT_EQ(source_before.current_generations.size(), 1U);
  ASSERT_TRUE(source_before.visible_outputs.empty());

  const Value output = testing::make_m1_test_output();
  const ValueRevisionId output_revision = output.revision_id();
  compute::ComputeRunObservationCoordinate visible_coordinate;
  std::thread worker([&] {
    visible_coordinate = fanout->reserve_causal_coordinate();
    fanout->on_current_visible(run.descriptor(), output, visible_coordinate);
  });
  for (std::size_t attempt = 0U;
       attempt < 1000000U && !mirror->current_visible_paused(); ++attempt) {
    std::this_thread::yield();
  }
  if (!mirror->current_visible_paused()) {
    mirror->release_current_visible();
    worker.join();
    FAIL() << "mirror callback did not reach its deterministic barrier";
    return;
  }

  const auto boundary_sink =
      mixed_collector.make_sink(M1ObservedRequestTag::Interactive);
  const compute::ComputeRunObservationCoordinate boundary_coordinate =
      boundary_sink->reserve_causal_coordinate();
  boundary_sink->abort_causal_coordinate(boundary_coordinate);
  const M1FairnessObservationSnapshot in_flight = mixed_collector.snapshot();
  const I1EpisodeObservationSnapshot source_in_flight =
      source_collector.snapshot();
  EXPECT_FALSE(in_flight.stable_publication_cut);
  EXPECT_EQ(in_flight.reservation_entry_frontier, 3U);
  EXPECT_EQ(in_flight.reservation_completion_frontier, 2U);
  EXPECT_EQ(in_flight.claimed_slot_frontier, 0U);
  EXPECT_EQ(in_flight.published_slot_frontier, 0U);
  ASSERT_EQ(source_in_flight.current_generations.size(), 1U);
  EXPECT_TRUE(source_in_flight.visible_outputs.empty());
  EXPECT_EQ(log.size(), 3U);

  mirror->release_current_visible();
  worker.join();
  const M1FairnessObservationSnapshot after = mixed_collector.snapshot();
  const I1EpisodeObservationSnapshot source_after = source_collector.snapshot();
  EXPECT_TRUE(after.stable_publication_cut);
  EXPECT_EQ(after.reservation_entry_frontier, 3U);
  EXPECT_EQ(after.reservation_completion_frontier, 3U);
  EXPECT_EQ(after.claimed_slot_frontier, 0U);
  EXPECT_EQ(after.published_slot_frontier, 0U);
  ASSERT_EQ(source_after.current_generations.size(), 1U);
  ASSERT_EQ(source_after.visible_outputs.size(), 1U);
  const I1ObservedCurrentGeneration& current =
      source_after.current_generations.front();
  const I1ObservedVisibleOutput& visible = source_after.visible_outputs.front();
  EXPECT_EQ(current.generation,
            run.descriptor().supersession().generation.value());
  EXPECT_EQ(visible.generation, current.generation);
  EXPECT_EQ(visible.run_id, run.descriptor().id().value());
  EXPECT_EQ(current.observed_at, current_coordinate.observed_at);
  EXPECT_EQ(current.causal_sequence, current_coordinate.causal_sequence);
  EXPECT_EQ(visible.observed_at, visible_coordinate.observed_at);
  EXPECT_EQ(visible.causal_sequence, visible_coordinate.causal_sequence);
  EXPECT_LT(current.causal_sequence, visible.causal_sequence);
  ASSERT_TRUE(visible.output.valid());
  EXPECT_EQ(visible.output.revision_id(), output_revision);

  ASSERT_FALSE(log.overflowed());
  ASSERT_EQ(log.size(), 4U);
  EXPECT_EQ(log.at(0U).event, FanoutProductEvent::CurrentGeneration);
  EXPECT_EQ(log.at(0U).recipient, FanoutRecipient::Mirror);
  EXPECT_EQ(log.at(0U).coordinate.observed_at, current_coordinate.observed_at);
  EXPECT_EQ(log.at(0U).coordinate.causal_sequence,
            current_coordinate.causal_sequence);
  EXPECT_EQ(log.at(1U).event, FanoutProductEvent::CurrentGeneration);
  EXPECT_EQ(log.at(1U).recipient, FanoutRecipient::SequenceAuthority);
  EXPECT_EQ(log.at(1U).coordinate.observed_at, current_coordinate.observed_at);
  EXPECT_EQ(log.at(1U).coordinate.causal_sequence,
            current_coordinate.causal_sequence);
  EXPECT_EQ(log.at(2U).event, FanoutProductEvent::CurrentVisible);
  EXPECT_EQ(log.at(2U).recipient, FanoutRecipient::Mirror);
  EXPECT_EQ(log.at(2U).coordinate.observed_at, visible_coordinate.observed_at);
  EXPECT_EQ(log.at(2U).coordinate.causal_sequence,
            visible_coordinate.causal_sequence);
  EXPECT_EQ(log.at(3U).event, FanoutProductEvent::CurrentVisible);
  EXPECT_EQ(log.at(3U).recipient, FanoutRecipient::SequenceAuthority);
  EXPECT_EQ(log.at(3U).coordinate.observed_at, visible_coordinate.observed_at);
  EXPECT_EQ(log.at(3U).coordinate.causal_sequence,
            visible_coordinate.causal_sequence);
  EXPECT_TRUE(log.at(2U).value_valid);
  EXPECT_TRUE(log.at(3U).value_valid);
  EXPECT_EQ(log.at(2U).value_revision, output_revision);
  EXPECT_EQ(log.at(3U).value_revision, output_revision);
  EXPECT_EQ(authority->reservation_count(), 2U);
  EXPECT_EQ(mirror->reservation_count(), 0U);
  EXPECT_EQ(authority->abort_count(), 0U);
  EXPECT_EQ(mirror->abort_count(), 0U);
}

/**
 * @brief Proves observer construction, ordering, and exhaustion are exact.
 * @throws GoogleTest assertion control and observer allocation failures.
 */
TEST(M1Profile, ObservationSinksShareOneNonzeroCausalSequence) {
  EXPECT_THROW(M1FairnessObservationCollector(0U), std::invalid_argument);
  M1FairnessObservationCollector collector(2U);
  const auto interactive =
      collector.make_sink(M1ObservedRequestTag::Interactive);
  const auto throughput =
      collector.make_sink(M1ObservedRequestTag::ThroughputGraphA);
  ASSERT_NE(interactive, nullptr);
  ASSERT_NE(throughput, nullptr);
  EXPECT_TRUE(interactive->observes_task_semantics());
  EXPECT_TRUE(throughput->observes_task_semantics());

  compute::ComputeRun interactive_run(make_observer_submission(
      "m1-observer-interactive", 1U, compute::ComputeRunQosClass::Interactive));
  compute::ComputeRun throughput_run(make_observer_submission(
      "m1-observer-throughput", 2U, compute::ComputeRunQosClass::Throughput));
  const compute::ComputeRunObservationCoordinate first =
      interactive->reserve_causal_coordinate();
  const compute::ComputeRunObservationCoordinate second =
      throughput->reserve_causal_coordinate();
  EXPECT_NE(first.causal_sequence, 0U);
  EXPECT_EQ(second.causal_sequence, first.causal_sequence + 1U);

  throughput->on_service_start(
      throughput_run.descriptor(),
      compute::ComputeRunTaskIdentity(throughput_run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(2U)),
      22U, compute::ComputeRunServiceStartObservation{true, true, true},
      second);
  interactive->on_service_start(
      interactive_run.descriptor(),
      compute::ComputeRunTaskIdentity(interactive_run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(1U)),
      11U, compute::ComputeRunServiceStartObservation{true, true, true}, first);

  const compute::ComputeRunObservationCoordinate third =
      throughput->reserve_causal_coordinate();
  throughput->on_service_start(
      interactive_run.descriptor(),
      compute::ComputeRunTaskIdentity(interactive_run.descriptor().id(),
                                      compute::ComputeRunLocalTaskId(3U)),
      33U, compute::ComputeRunServiceStartObservation{true, true, true}, third);
  EXPECT_THROW(collector.make_sink(static_cast<M1ObservedRequestTag>(255U)),
               std::invalid_argument);
  const M1FairnessObservationSnapshot snapshot = collector.snapshot();
  ASSERT_EQ(snapshot.events.size(), 2U);
  EXPECT_EQ(snapshot.events[0U].causal_sequence, first.causal_sequence);
  EXPECT_EQ(snapshot.events[0U].request_tag, M1ObservedRequestTag::Interactive);
  EXPECT_EQ(snapshot.events[0U].service_charge, 11U);
  EXPECT_EQ(snapshot.events[1U].causal_sequence, second.causal_sequence);
  EXPECT_EQ(snapshot.events[1U].request_tag,
            M1ObservedRequestTag::ThroughputGraphA);
  EXPECT_EQ(snapshot.events[1U].service_charge, 22U);
  EXPECT_TRUE(snapshot.overflowed);
  EXPECT_FALSE(snapshot.sequence_exhausted);
  EXPECT_TRUE(snapshot.qos_mismatch);
}

/**
 * @brief Proves terminal sequence and bounded capacity become sticky invalid.
 * @throws GoogleTest assertion control and observer allocation failures.
 */
TEST(M1Profile, ObservationExhaustionCannotWrapBackToPass) {
  M1FairnessObservationCollector collector(
      1U, std::numeric_limits<std::uint64_t>::max());
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  const compute::ComputeRunObservationCoordinate last =
      sink->reserve_causal_coordinate();
  const compute::ComputeRunObservationCoordinate exhausted =
      sink->reserve_causal_coordinate();
  const compute::ComputeRunObservationCoordinate still_exhausted =
      sink->reserve_causal_coordinate();
  EXPECT_EQ(last.causal_sequence, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(exhausted.causal_sequence, 0U);
  EXPECT_EQ(still_exhausted.causal_sequence, 0U);
  sink->abort_causal_coordinate(last);
  sink->abort_causal_coordinate(exhausted);
  sink->abort_causal_coordinate(still_exhausted);
  const M1FairnessObservationSnapshot snapshot = collector.snapshot();
  EXPECT_TRUE(snapshot.sequence_exhausted);
  EXPECT_TRUE(snapshot.events.empty());
}

/**
 * @brief Proves an explicitly aborted reservation closes the M1 cut frontier.
 *
 * The test reserves one causal coordinate without publishing an observation,
 * verifies that the in-flight reservation prevents a stable cut, and then
 * retires the coordinate through the production abort path.  The final
 * snapshot must be stable without manufacturing an observation event.
 *
 * @throws GoogleTest assertion control and observer allocation failures.
 */
TEST(M1Profile, AbortedObservationReservationDoesNotLeakCutFrontier) {
  M1FairnessObservationCollector collector(1U);
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  const M1FairnessObservationSnapshot before = collector.snapshot();
  ASSERT_TRUE(before.stable_publication_cut);

  const compute::ComputeRunObservationCoordinate coordinate =
      sink->reserve_causal_coordinate();
  ASSERT_NE(coordinate.causal_sequence, 0U);
  const M1FairnessObservationSnapshot in_flight = collector.snapshot();
  EXPECT_FALSE(in_flight.stable_publication_cut);
  EXPECT_EQ(in_flight.reservation_entry_frontier, 1U);
  EXPECT_EQ(in_flight.reservation_completion_frontier, 0U);
  EXPECT_TRUE(in_flight.events.empty());

  sink->abort_causal_coordinate(coordinate);
  const M1FairnessObservationSnapshot after = collector.snapshot();
  EXPECT_TRUE(after.stable_publication_cut);
  EXPECT_EQ(after.reservation_entry_frontier, 1U);
  EXPECT_EQ(after.reservation_completion_frontier, 1U);
  EXPECT_EQ(after.claimed_slot_frontier, 0U);
  EXPECT_EQ(after.published_slot_frontier, 0U);
  EXPECT_TRUE(after.events.empty());
  EXPECT_FALSE(m1_observation_cut_unchanged(before, after));
}

/**
 * @brief Proves a forced sample-before-sequence overlap remains ordered.
 *
 * One reservation pauses after sampling its steady time and before assigning
 * its sequence.  A second reservation must observe the occupied atomic gate,
 * release the first, and then receive the next sequence with a nondecreasing
 * timestamp.  Both reservations are explicitly retired after inspection.
 *
 * @throws GoogleTest assertion control, thread, and observer allocation
 * failures.
 */
TEST(M1Profile, CoordinateReservationSerializesSampleAndSequence) {
  const auto hook = std::make_shared<PausingCoordinateSampleHook>();
  M1FairnessObservationCollector collector(2U, 1U, hook);
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  std::array<compute::ComputeRunObservationCoordinate, 2U> coordinates;

  std::thread first(
      [&] { coordinates[0U] = sink->reserve_causal_coordinate(); });
  for (std::size_t attempt = 0U; attempt < 1000000U && !hook->sampled();
       ++attempt) {
    std::this_thread::yield();
  }
  if (!hook->sampled()) {
    hook->release();
    first.join();
    FAIL() << "first coordinate did not reach its deterministic sample hook";
    return;
  }

  std::thread second(
      [&] { coordinates[1U] = sink->reserve_causal_coordinate(); });
  first.join();
  second.join();

  EXPECT_TRUE(hook->contention());
  EXPECT_NE(coordinates[0U].causal_sequence, 0U);
  EXPECT_EQ(coordinates[1U].causal_sequence,
            coordinates[0U].causal_sequence + 1U);
  EXPECT_LE(coordinates[0U].observed_at, coordinates[1U].observed_at);
  sink->abort_causal_coordinate(coordinates[0U]);
  sink->abort_causal_coordinate(coordinates[1U]);
  EXPECT_TRUE(collector.snapshot().stable_publication_cut);
}

/**
 * @brief Proves scheduling contention cannot masquerade as numeric sequence
 * exhaustion at the historical 4096-attempt boundary.
 *
 * @throws GoogleTest assertion control, thread, and observer allocation
 * failures.
 * @note A single contender is sufficient: the hook releases the first owner on
 * the last old retry, making the false exhaustion deterministic without a
 * probabilistic high-thread-count race.
 */
TEST(M1Profile, CoordinateContentionCannotExhaustNumericSequence) {
  const auto hook = std::make_shared<CoordinateContentionLimitHook>();
  M1FairnessObservationCollector collector(2U, 1U, hook);
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  std::array<compute::ComputeRunObservationCoordinate, 2U> coordinates;

  std::thread first(
      [&] { coordinates[0U] = sink->reserve_causal_coordinate(); });
  for (std::size_t attempt = 0U; attempt < 1000000U && !hook->sampled();
       ++attempt) {
    std::this_thread::yield();
  }
  if (!hook->sampled()) {
    hook->release();
    first.join();
    FAIL() << "first coordinate did not reach the contention-limit hook";
    return;
  }

  std::thread second(
      [&] { coordinates[1U] = sink->reserve_causal_coordinate(); });
  first.join();
  second.join();

  EXPECT_GE(hook->contention_count(),
            CoordinateContentionLimitHook::kHistoricalAttemptLimit);
  EXPECT_EQ(coordinates[0U].causal_sequence, 1U);
  EXPECT_EQ(coordinates[1U].causal_sequence, 2U);
  EXPECT_LE(coordinates[0U].observed_at, coordinates[1U].observed_at);
  sink->abort_causal_coordinate(coordinates[0U]);
  sink->abort_causal_coordinate(coordinates[1U]);
  const M1FairnessObservationSnapshot snapshot = collector.snapshot();
  EXPECT_FALSE(snapshot.sequence_exhausted);
  EXPECT_TRUE(snapshot.stable_publication_cut);
}

/**
 * @brief Proves concurrent callback publication terminates with unique slots.
 * @throws GoogleTest assertion control, thread, and allocation failures.
 */
TEST(M1Profile, ObservationCallbacksRemainFiniteUnderConcurrency) {
  constexpr std::size_t kThreadCount = 8U;
  constexpr std::size_t kEventsPerThread = 32U;
  M1FairnessObservationCollector collector(kThreadCount * kEventsPerThread);
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  compute::ComputeRun run(make_observer_submission(
      "m1-observer-concurrent", 50U, compute::ComputeRunQosClass::Interactive));
  std::atomic<bool> release{false};
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);
  for (std::size_t thread = 0U; thread < kThreadCount; ++thread) {
    workers.emplace_back([&, thread] {
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t event = 0U; event < kEventsPerThread; ++event) {
        const compute::ComputeRunObservationCoordinate coordinate =
            sink->reserve_causal_coordinate();
        sink->on_service_start(
            run.descriptor(),
            compute::ComputeRunTaskIdentity(
                run.descriptor().id(), compute::ComputeRunLocalTaskId(
                                           thread * kEventsPerThread + event)),
            1U, compute::ComputeRunServiceStartObservation{true, true, true},
            coordinate);
      }
    });
  }
  release.store(true, std::memory_order_release);
  for (std::thread& worker : workers) {
    worker.join();
  }
  const M1FairnessObservationSnapshot snapshot = collector.snapshot();
  EXPECT_FALSE(snapshot.overflowed);
  EXPECT_FALSE(snapshot.sequence_exhausted);
  EXPECT_FALSE(snapshot.qos_mismatch);
  EXPECT_TRUE(snapshot.stable_publication_cut);
  EXPECT_EQ(snapshot.reservation_entry_frontier,
            kThreadCount * kEventsPerThread);
  EXPECT_EQ(snapshot.reservation_completion_frontier,
            kThreadCount * kEventsPerThread);
  EXPECT_EQ(snapshot.claimed_slot_frontier, kThreadCount * kEventsPerThread);
  EXPECT_EQ(snapshot.published_slot_frontier, kThreadCount * kEventsPerThread);
  ASSERT_EQ(snapshot.events.size(), kThreadCount * kEventsPerThread);
  for (std::size_t index = 1U; index < snapshot.events.size(); ++index) {
    EXPECT_LT(snapshot.events[index - 1U].causal_sequence,
              snapshot.events[index].causal_sequence);
    EXPECT_LE(snapshot.events[index - 1U].observed_at,
              snapshot.events[index].observed_at);
  }
}

/**
 * @brief Proves a claimed-but-unpublished callback invalidates the M1 cut.
 * @throws GoogleTest assertion control, thread, or observer allocation
 * failures.
 */
TEST(M1Profile, ObservationBoundaryRejectsClaimedUnpublishedCallback) {
  const auto hook = std::make_shared<PausingPublicationHook>();
  EXPECT_THROW(M1FairnessObservationCollector(1U, 1U, nullptr),
               std::invalid_argument);
  M1FairnessObservationCollector collector(1U, 1U, hook);
  const auto sink = collector.make_sink(M1ObservedRequestTag::Interactive);
  compute::ComputeRun run(make_observer_submission(
      "m1-observer-paused", 51U, compute::ComputeRunQosClass::Interactive));
  const M1FairnessObservationSnapshot before = collector.snapshot();
  ASSERT_TRUE(before.stable_publication_cut);

  std::thread worker([&] {
    const compute::ComputeRunObservationCoordinate coordinate =
        sink->reserve_causal_coordinate();
    sink->on_service_start(
        run.descriptor(),
        compute::ComputeRunTaskIdentity(run.descriptor().id(),
                                        compute::ComputeRunLocalTaskId(1U)),
        1U, compute::ComputeRunServiceStartObservation{true, true, true},
        coordinate);
  });
  for (std::size_t attempt = 0U; attempt < 1000000U && !hook->claimed();
       ++attempt) {
    std::this_thread::yield();
  }
  if (!hook->claimed()) {
    hook->release();
    worker.join();
    FAIL() << "observer callback did not reach the deterministic claim hook";
    return;
  }

  const M1FairnessObservationSnapshot in_flight = collector.snapshot();
  EXPECT_FALSE(in_flight.stable_publication_cut);
  EXPECT_EQ(in_flight.reservation_entry_frontier, 1U);
  EXPECT_EQ(in_flight.reservation_completion_frontier, 0U);
  EXPECT_EQ(in_flight.claimed_slot_frontier, 1U);
  EXPECT_EQ(in_flight.published_slot_frontier, 0U);
  EXPECT_TRUE(in_flight.events.empty());
  EXPECT_FALSE(m1_observation_cut_unchanged(before, in_flight));

  M1ProtocolEvidenceInput protocol = make_passing_protocol();
  protocol.boundary_was_zero_duration =
      m1_observation_cut_unchanged(before, in_flight);
  protocol.raw_history_preserved = protocol.boundary_was_zero_duration;
  EXPECT_EQ(evaluate_m1_protocol(std::move(protocol)).verdict,
            I1Verdict::Invalid);

  hook->release();
  worker.join();
  const M1FairnessObservationSnapshot after = collector.snapshot();
  EXPECT_TRUE(after.stable_publication_cut);
  EXPECT_EQ(after.reservation_entry_frontier, 1U);
  EXPECT_EQ(after.reservation_completion_frontier, 1U);
  EXPECT_EQ(after.claimed_slot_frontier, 1U);
  EXPECT_EQ(after.published_slot_frontier, 1U);
  ASSERT_EQ(after.events.size(), 1U);
  EXPECT_FALSE(m1_observation_cut_unchanged(before, after));
}

/**
 * @brief Proves malformed evidence fails both exact delegated pair relations.
 * @throws GoogleTest assertion control only.
 */
TEST(M1Profile, EnvironmentPairsDelegateAndFailClosed) {
  const B1EnvironmentEvidence malformed;
  const M1EnvironmentPairCompatibility compatibility =
      evaluate_m1_environment_pairs(malformed, malformed, malformed);
  EXPECT_FALSE(compatibility.isolated_i1_base_only);
  EXPECT_FALSE(compatibility.isolated_b1_cap_eight);
  EXPECT_FALSE(compatibility.compatible());
}

/**
 * @brief Proves two ordinals use distinct base-only and full pair relations.
 * @throws Canonical fixture, observer, and GoogleTest failures unchanged.
 */
TEST(M1Profile, EnvironmentPairsPassForTwoCompleteSameOrdinalTriples) {
  for (const std::uint64_t ordinal : {1U, 3U}) {
    const auto evidence = make_environment_triple(ordinal);
    const M1EnvironmentPairCompatibility compatibility =
        evaluate_m1_environment_pairs(evidence[0U], evidence[1U], evidence[2U]);
    EXPECT_TRUE(compatibility.isolated_i1_base_only);
    EXPECT_TRUE(compatibility.isolated_b1_cap_eight);
    EXPECT_TRUE(compatibility.compatible());
  }
}

/**
 * @brief Proves storage drift affects only the full B1 relation and that
 * argument-role reversal cannot accidentally pass either delegated check.
 * @throws Canonical fixture, observer, and GoogleTest failures unchanged.
 */
TEST(M1Profile, EnvironmentPairsDistinguishStorageAndArgumentRoles) {
  auto evidence = make_environment_triple(2U);
  evidence[2U] = testing::synchronously_recast_b1_test_storage(
      std::move(evidence[2U]), "forgedfs");
  const M1EnvironmentPairCompatibility storage_drift =
      evaluate_m1_environment_pairs(evidence[0U], evidence[1U], evidence[2U]);
  EXPECT_TRUE(storage_drift.isolated_i1_base_only);
  EXPECT_FALSE(storage_drift.isolated_b1_cap_eight);
  EXPECT_FALSE(storage_drift.compatible());

  const M1EnvironmentPairCompatibility reversed =
      evaluate_m1_environment_pairs(evidence[1U], evidence[0U], evidence[2U]);
  EXPECT_FALSE(reversed.isolated_i1_base_only);
  EXPECT_FALSE(reversed.isolated_b1_cap_eight);
  EXPECT_FALSE(reversed.compatible());
}

}  // namespace
}  // namespace ps::benchmark
