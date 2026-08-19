/**
 * @file test_progressive_compute.cpp
 * @brief Tests private progressive gating and exact preview-source arithmetic.
 */
#include <fenv.h>  // NOLINT(build/c++11)
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "compute/execution/progressive_compute.hpp"
#include "core/dense_image_processing.hpp"
#include "photospider/data/image_view.hpp"

namespace ps::testing {
namespace {

/**
 * @brief Builds one deterministic HP child for progressive gate arbitration.
 * @return Valid full-quality child under a realtime request lineage.
 * @throws std::bad_alloc when owned descriptor strings cannot allocate.
 * @note The Run has no deadline or observation sink; tests drive only its
 * private cancellation authority and the bound progressive gate.
 */
compute::ComputeRunSubmission make_progressive_gate_submission() {
  return compute::ComputeRunSubmission{
      "progressive-gate-linearization",
      GraphInstanceId{1U},
      GraphRevision{1U},
      4,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             std::nullopt, 1U, 8U},
      compute::SupersessionIdentity{
          compute::SupersessionKey(4, ComputeIntent::RealTimeUpdate),
          compute::SupersessionGeneration(1U)},
      nullptr};
}

/**
 * @brief Records and optionally pauses Run-owned final-trigger observation.
 * @throws Nothing from construction, callbacks, and scalar inspection.
 * @note The trigger callback may deliberately wait in tests while the Run
 * terminal arbiter is held. Production observers remain bounded/nonblocking.
 */
class ProgressiveTriggerOrderObservationSink final
    : public compute::ComputeRunObservationSink {
 public:
  /** @copydoc compute::ComputeRunObservationSink::reserve_causal_coordinate */
  compute::ComputeRunObservationCoordinate reserve_causal_coordinate() noexcept
      override {
    return {std::chrono::steady_clock::now(),
            next_sequence_.fetch_add(1U, std::memory_order_relaxed)};
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_generation */
  void on_current_generation(
      const compute::SupersessionIdentity& identity,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)identity;
    (void)coordinate;
  }

  /** @copydoc compute::ComputeRunObservationSink::on_service_start */
  void on_service_start(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      std::uint64_t service_charge,
      const compute::ComputeRunServiceStartObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)task_identity;
    (void)service_charge;
    (void)observation;
    (void)coordinate;
    service_count_.fetch_add(1U, std::memory_order_release);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
  void on_cancellation(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunCancellationReason reason,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)reason;
    cancellation_sequence_.store(coordinate.causal_sequence,
                                 std::memory_order_release);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_terminal */
  void on_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)kind;
    terminal_sequence_.store(coordinate.causal_sequence,
                             std::memory_order_release);
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
  void on_current_visible(
      const compute::ComputeRunDescriptor& descriptor, Value output,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)output;
    (void)coordinate;
    visible_count_.fetch_add(1U, std::memory_order_release);
  }

  /** @copydoc
   * compute::ComputeRunObservationSink::on_progressive_final_triggered */
  void on_progressive_final_triggered(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    trigger_sequence_.store(coordinate.causal_sequence,
                            std::memory_order_release);
    trigger_entered_.store(true, std::memory_order_release);
    while (pause_trigger_.load(std::memory_order_acquire) &&
           !release_trigger_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
  void on_run_quiescent(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)coordinate;
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
  void on_run_resource_settled(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)coordinate;
  }

  /** @copydoc compute::ComputeRunObservationSink::on_host_settled */
  void on_host_settled(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)coordinate;
  }

  /**
   * @brief Enables the trigger-callback test barrier before invocation.
   * @return Nothing.
   * @throws Nothing.
   * @note Call only before the Run-owned trigger operation starts.
   */
  void pause_trigger() noexcept {
    pause_trigger_.store(true, std::memory_order_release);
  }

  /**
   * @brief Releases a trigger callback paused in the Run critical section.
   * @return Nothing.
   * @throws Nothing.
   * @note Repeated release is idempotent.
   */
  void release_trigger() noexcept {
    release_trigger_.store(true, std::memory_order_release);
  }

  /**
   * @brief Returns whether the trigger callback reached its barrier.
   * @return True after the real trigger observation entered.
   * @throws Nothing.
   * @note Acquire ordering exposes the already-published trigger sequence.
   */
  bool trigger_entered() const noexcept {
    return trigger_entered_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns the unique trigger causal sequence.
   * @return Nonzero sequence after trigger observation, otherwise zero.
   * @throws Nothing.
   * @note The value is copied from the real Run-owned callback coordinate.
   */
  std::uint64_t trigger_sequence() const noexcept {
    return trigger_sequence_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns the accepted cancellation causal sequence.
   * @return Nonzero sequence after cancellation observation, otherwise zero.
   * @throws Nothing.
   * @note Rejected cancellation never changes the value.
   */
  std::uint64_t cancellation_sequence() const noexcept {
    return cancellation_sequence_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns the exactly-once terminal causal sequence.
   * @return Nonzero sequence after terminal observation, otherwise zero.
   * @throws Nothing.
   * @note The tests create only one terminal contender.
   */
  std::uint64_t terminal_sequence() const noexcept {
    return terminal_sequence_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns observed HP service-start count.
   * @return Exact callback count.
   * @throws Nothing.
   * @note Cancellation-first tests require zero.
   */
  std::size_t service_count() const noexcept {
    return service_count_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns observed visible-final count.
   * @return Exact callback count.
   * @throws Nothing.
   * @note Cancellation-first tests require zero.
   */
  std::size_t visible_count() const noexcept {
    return visible_count_.load(std::memory_order_acquire);
  }

 private:
  /** @brief Nonzero observation sequence allocator. */
  std::atomic<std::uint64_t> next_sequence_{1U};
  /** @brief Whether the trigger callback should wait. */
  std::atomic<bool> pause_trigger_{false};
  /** @brief Whether the trigger callback reached its barrier. */
  std::atomic<bool> trigger_entered_{false};
  /** @brief Barrier release flag. */
  std::atomic<bool> release_trigger_{false};
  /** @brief Unique trigger causal sequence. */
  std::atomic<std::uint64_t> trigger_sequence_{0U};
  /** @brief Accepted cancellation causal sequence. */
  std::atomic<std::uint64_t> cancellation_sequence_{0U};
  /** @brief Cancelled terminal causal sequence. */
  std::atomic<std::uint64_t> terminal_sequence_{0U};
  /** @brief Observed service-start count. */
  std::atomic<std::size_t> service_count_{0U};
  /** @brief Observed visible-final count. */
  std::atomic<std::size_t> visible_count_{0U};
};

/**
 * @brief Waits boundedly for one atomic test predicate.
 * @param predicate Nonblocking predicate over test-owned atomics.
 * @return True when the predicate became true before two seconds elapsed.
 * @throws Nothing when the predicate is nonthrowing.
 * @note The bound prevents a broken barrier from hanging the test process.
 */
template <typename Predicate>
bool wait_for_progressive_predicate(Predicate predicate) noexcept {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return predicate();
}

/**
 * @brief Restores the test thread floating-point environment after one scope.
 * @throws Nothing; inability to restore terminates the test process.
 */
class TestFloatingEnvironment final {
 public:
  /**
   * @brief Captures the current complete floating-point environment.
   * @throws Nothing; capture failure terminates because the test cannot clean
   * up safely.
   */
  TestFloatingEnvironment() noexcept {
    if (fegetenv(&environment_) != 0) {
      std::terminate();
    }
  }

  /**
   * @brief Restores the environment captured by construction.
   * @throws Nothing; restoration failure terminates.
   */
  ~TestFloatingEnvironment() noexcept {
    if (fesetenv(&environment_) != 0) {
      std::terminate();
    }
  }

  /**
   * @brief Prevents copying environment-restoration ownership.
   * @param other Owner that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  TestFloatingEnvironment(const TestFloatingEnvironment& other) = delete;

  /**
   * @brief Prevents assignment of environment-restoration ownership.
   * @param other Owner that cannot be assigned.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  TestFloatingEnvironment& operator=(const TestFloatingEnvironment& other) =
      delete;

 private:
  /** @brief Complete test-thread environment restored at destruction. */
  fenv_t environment_{};
};

/**
 * @brief Publishes one tightly packed Ready CPU FP32 image Value.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive channel count.
 * @param samples Exact row-major interleaved sample payload.
 * @return Fresh zero-origin ordinary-image Value.
 * @throws std::invalid_argument for shape/payload mismatch; Value and
 *         allocation failures otherwise propagate unchanged.
 */
Value make_float_image(std::size_t width, std::size_t height,
                       std::size_t channels,
                       const std::vector<float>& samples) {
  if (width == 0U || height == 0U || channels == 0U ||
      samples.size() != width * height * channels) {
    throw std::invalid_argument("progressive image fixture shape is invalid");
  }
  DenseTensorDescriptor descriptor{{height, width, channels},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  std::vector<std::byte> bytes(samples.size() * sizeof(float));
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{
          {static_cast<std::ptrdiff_t>(width * channels * sizeof(float)),
           static_cast<std::ptrdiff_t>(channels * sizeof(float)),
           static_cast<std::ptrdiff_t>(sizeof(float))}},
      std::move(bytes));
}

/**
 * @brief Reads one FP32 scalar through a checked immutable image view.
 * @param image Valid retaining FP32 image view.
 * @param x Zero-based pixel column.
 * @param y Zero-based pixel row.
 * @param channel Channel index.
 * @return Stored binary32 value.
 * @throws Nothing for valid test coordinates.
 */
float read_float(const ImageView& image, std::size_t x, std::size_t y,
                 std::size_t channel) noexcept {
  float value = 0.0F;
  std::memcpy(&value, image.channel_data(x, y, channel), sizeof(value));
  return value;
}

TEST(ProgressiveFinalGate, PreviewSuccessAllowsExactlyOneFinalTrigger) {
  compute::ProgressiveFinalGate gate;
  EXPECT_EQ(gate.state(), compute::ProgressiveFinalGate::State::Pending);
  EXPECT_TRUE(gate.arm());
  EXPECT_FALSE(gate.arm());
  EXPECT_EQ(gate.state(), compute::ProgressiveFinalGate::State::Armed);
  EXPECT_TRUE(gate.try_trigger());
  EXPECT_FALSE(gate.try_trigger());
  EXPECT_FALSE(gate.deny());
  EXPECT_EQ(gate.state(), compute::ProgressiveFinalGate::State::Triggered);
}

TEST(ProgressiveFinalGate, CancellationDeniesPendingOrArmedFinal) {
  compute::ProgressiveFinalGate pending_gate;
  EXPECT_TRUE(pending_gate.deny());
  EXPECT_FALSE(pending_gate.arm());
  EXPECT_FALSE(pending_gate.try_trigger());
  EXPECT_FALSE(pending_gate.deny());
  EXPECT_EQ(pending_gate.state(), compute::ProgressiveFinalGate::State::Denied);

  compute::ProgressiveFinalGate armed_gate;
  ASSERT_TRUE(armed_gate.arm());
  EXPECT_TRUE(armed_gate.deny());
  EXPECT_FALSE(armed_gate.try_trigger());
  EXPECT_EQ(armed_gate.state(), compute::ProgressiveFinalGate::State::Denied);
}

/**
 * @brief Proves cancellation denial precedes terminal-visible cleanup delay.
 * @throws Run synchronization or thread-construction failures fail the test.
 * @note The registered cleanup callback intentionally delays a redundant
 * `deny()`. Reaching it proves `Cancelled` has already been committed, while
 * the Run-bound gate must already reject the simultaneous final attempt.
 */
TEST(ProgressiveFinalGate,
     CommittedCancellationDeniesBeforeDelayedCleanupCanTriggerFinal) {
  compute::ComputeRun run(make_progressive_gate_submission());
  compute::ComputeRunLease lease = run.acquire_lease();
  auto gate = std::make_shared<compute::ProgressiveFinalGate>();
  lease.bind_progressive_final_gate(gate);
  ASSERT_TRUE(gate->arm());

  std::atomic<bool> cleanup_entered{false};
  std::atomic<bool> release_cleanup{false};
  std::atomic<bool> delayed_deny_won{true};
  /**
   * @brief Delays the former callback-owned gate denial behind a test barrier.
   * @param reason Accepted cancellation reason; identity is irrelevant here.
   * @return Nothing.
   * @throws Nothing.
   */
  const auto delayed_cleanup =
      [&](compute::ComputeRunCancellationReason reason) noexcept {
        (void)reason;
        cleanup_entered.store(true, std::memory_order_release);
        while (!release_cleanup.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        delayed_deny_won.store(gate->deny(), std::memory_order_release);
      };
  compute::ComputeRunCancellationRegistration registration =
      lease.register_cancellation_notification(delayed_cleanup);
  ASSERT_TRUE(registration.active());
  bool cancellation_won = false;
  std::exception_ptr cancellation_failure;
  std::thread canceller([&] {
    try {
      cancellation_won = run.cancellation_source().request_cancellation(
          compute::ComputeRunCancellationReason::Superseded);
    } catch (...) {
      cancellation_failure = std::current_exception();
    }
  });

  const auto wait_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!cleanup_entered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < wait_deadline) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(cleanup_entered.load(std::memory_order_acquire));

  std::uint64_t final_trigger_count = 0U;
  std::uint64_t hp_service_count = 0U;
  std::uint64_t visible_final_count = 0U;
  if (lease.try_publish_progressive_final_trigger()) {
    ++final_trigger_count;
    ++hp_service_count;
    ++visible_final_count;
  }
  EXPECT_EQ(final_trigger_count, 0U);
  EXPECT_EQ(hp_service_count, 0U);
  EXPECT_EQ(visible_final_count, 0U);
  EXPECT_EQ(gate->state(), compute::ProgressiveFinalGate::State::Denied);

  release_cleanup.store(true, std::memory_order_release);
  canceller.join();
  EXPECT_FALSE(cancellation_failure);
  EXPECT_TRUE(cancellation_won);
  EXPECT_FALSE(delayed_deny_won.load(std::memory_order_acquire));
  const auto terminal = lease.terminal_outcome();
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->kind, compute::ComputeRunTerminalKind::Cancelled);
}

/**
 * @brief Proves a final trigger that linearizes first remains permitted.
 * @throws Run synchronization failures fail the test.
 * @note Later cancellation still owns the Run terminal and stale-publication
 * policy; it cannot roll the already-consumed request gate backward.
 */
TEST(ProgressiveFinalGate, TriggerWinnerRemainsTriggeredAfterCancellation) {
  compute::ComputeRun run(make_progressive_gate_submission());
  compute::ComputeRunLease lease = run.acquire_lease();
  auto gate = std::make_shared<compute::ProgressiveFinalGate>();
  lease.bind_progressive_final_gate(gate);
  ASSERT_TRUE(gate->arm());
  ASSERT_TRUE(lease.try_publish_progressive_final_trigger());

  EXPECT_TRUE(run.cancellation_source().request_cancellation(
      compute::ComputeRunCancellationReason::Superseded));
  EXPECT_EQ(gate->state(), compute::ProgressiveFinalGate::State::Triggered);
  const auto terminal = lease.terminal_outcome();
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->kind, compute::ComputeRunTerminalKind::Cancelled);
}

/**
 * @brief Proves trigger observation completes before concurrent cancellation.
 * @throws Run, future, allocation, or synchronization failures fail the test.
 * @note The observer deliberately pauses inside the Run-owned trigger
 * critical section. Cancellation may begin concurrently but cannot reserve or
 * publish either of its real observations until the trigger callback returns.
 */
TEST(ProgressiveFinalGate,
     RunOwnedTriggerObservationPrecedesConcurrentCancellationTerminal) {
  auto sink = std::make_shared<ProgressiveTriggerOrderObservationSink>();
  compute::ComputeRunSubmission submission = make_progressive_gate_submission();
  submission.observation_sink = sink;
  compute::ComputeRun run(std::move(submission));
  compute::ComputeRunLease lease = run.acquire_lease();
  auto gate = std::make_shared<compute::ProgressiveFinalGate>();
  lease.bind_progressive_final_gate(gate);
  ASSERT_TRUE(gate->arm());
  sink->pause_trigger();

  std::future<bool> trigger = std::async(std::launch::async, [&lease] {
    return lease.try_publish_progressive_final_trigger();
  });
  const bool trigger_entered = wait_for_progressive_predicate(
      [&sink] { return sink->trigger_entered(); });
  if (!trigger_entered) {
    sink->release_trigger();
    EXPECT_TRUE(trigger_entered);
    trigger.wait();
    return;
  }

  std::atomic<bool> cancellation_called{false};
  const compute::ComputeRunCancellationSource cancellation_source =
      run.cancellation_source();
  std::future<bool> cancellation = std::async(
      std::launch::async, [cancellation_source, &cancellation_called] {
        cancellation_called.store(true, std::memory_order_release);
        return cancellation_source.request_cancellation(
            compute::ComputeRunCancellationReason::Superseded);
      });
  const bool cancellation_started =
      wait_for_progressive_predicate([&cancellation_called] {
        return cancellation_called.load(std::memory_order_acquire);
      });
  if (!cancellation_started) {
    sink->release_trigger();
    EXPECT_TRUE(cancellation_started);
    trigger.wait();
    cancellation.wait();
    return;
  }
  EXPECT_EQ(sink->cancellation_sequence(), 0U);
  EXPECT_EQ(sink->terminal_sequence(), 0U);

  sink->release_trigger();
  ASSERT_EQ(trigger.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_TRUE(trigger.get());
  ASSERT_EQ(cancellation.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_TRUE(cancellation.get());
  EXPECT_EQ(gate->state(), compute::ProgressiveFinalGate::State::Triggered);
  ASSERT_NE(sink->trigger_sequence(), 0U);
  ASSERT_NE(sink->cancellation_sequence(), 0U);
  ASSERT_NE(sink->terminal_sequence(), 0U);
  EXPECT_LT(sink->trigger_sequence(), sink->cancellation_sequence());
  EXPECT_LT(sink->cancellation_sequence(), sink->terminal_sequence());
}

/**
 * @brief Proves a cancellation winner emits no trigger or downstream output.
 * @throws Run, allocation, or synchronization failures fail the test.
 * @note Cancellation and terminal coordinates are real Run observations; a
 * later final attempt cannot emit trigger, service-start, or visibility.
 */
TEST(ProgressiveFinalGate,
     CancellationTerminalPreventsRunOwnedTriggerAndDownstreamObservations) {
  auto sink = std::make_shared<ProgressiveTriggerOrderObservationSink>();
  compute::ComputeRunSubmission submission = make_progressive_gate_submission();
  submission.observation_sink = sink;
  compute::ComputeRun run(std::move(submission));
  compute::ComputeRunLease lease = run.acquire_lease();
  auto gate = std::make_shared<compute::ProgressiveFinalGate>();
  lease.bind_progressive_final_gate(gate);
  ASSERT_TRUE(gate->arm());

  ASSERT_TRUE(run.cancellation_source().request_cancellation(
      compute::ComputeRunCancellationReason::Superseded));
  EXPECT_FALSE(lease.try_publish_progressive_final_trigger());
  EXPECT_EQ(gate->state(), compute::ProgressiveFinalGate::State::Denied);
  EXPECT_EQ(sink->trigger_sequence(), 0U);
  ASSERT_NE(sink->cancellation_sequence(), 0U);
  ASSERT_NE(sink->terminal_sequence(), 0U);
  EXPECT_LT(sink->cancellation_sequence(), sink->terminal_sequence());
  EXPECT_EQ(sink->service_count(), 0U);
  EXPECT_EQ(sink->visible_count(), 0U);
}

TEST(ExactBoxAverageFactorFour, RoundsOnceToNearestAndRestoresEnvironment) {
  TestFloatingEnvironment restore_environment;
  const float lower = std::nextafter(1.0F, 2.0F);
  const float upper = std::nextafter(lower, 2.0F);
  std::vector<float> samples(8U * 8U * 4U);
  for (std::size_t y = 0U; y < 8U; ++y) {
    for (std::size_t x = 0U; x < 8U; ++x) {
      for (std::size_t channel = 0U; channel < 4U; ++channel) {
        samples[(y * 8U + x) * 4U + channel] =
            ((x + y * 8U) % 2U == 0U) ? lower : upper;
      }
    }
  }
  const Value source = make_float_image(8U, 8U, 4U, samples);

  ASSERT_EQ(fesetround(FE_DOWNWARD), 0);
  ASSERT_EQ(feclearexcept(FE_ALL_EXCEPT), 0);
  ASSERT_EQ(feraiseexcept(FE_INVALID), 0);
  const Value destination =
      dense_image_processing::exact_box_average_factor_four(
          source, PixelRect{1, 0, 1, 1});
  const ImageView destination_view(destination);

  EXPECT_EQ(fegetround(), FE_DOWNWARD);
  EXPECT_NE(fetestexcept(FE_INVALID), 0);
  for (std::size_t channel = 0U; channel < 4U; ++channel) {
    EXPECT_EQ(read_float(destination_view, 1U, 0U, channel), upper);
    EXPECT_EQ(read_float(destination_view, 0U, 0U, channel), 0.0F);
    EXPECT_EQ(read_float(destination_view, 0U, 1U, channel), 0.0F);
    EXPECT_EQ(read_float(destination_view, 1U, 1U, channel), 0.0F);
  }
}

TEST(ExactBoxAverageFactorFour, RejectsWrongGeometryAndOutOfRangeRegion) {
  const Value source =
      make_float_image(8U, 8U, 4U, std::vector<float>(8U * 8U * 4U));
  const Value wrong_source =
      make_float_image(7U, 8U, 4U, std::vector<float>(7U * 8U * 4U));
  EXPECT_THROW(dense_image_processing::exact_box_average_factor_four(
                   wrong_source, PixelRect{0, 0, 1, 1}),
               std::invalid_argument);
  EXPECT_THROW(dense_image_processing::exact_box_average_factor_four(
                   source, PixelRect{2, 0, 1, 1}),
               std::out_of_range);
}

}  // namespace
}  // namespace ps::testing
