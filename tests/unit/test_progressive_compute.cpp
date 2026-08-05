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
#include <limits>
#include <memory>
#include <optional>
#include <thread>

#include "compute/progressive_compute.hpp"
#include "core/image_buffer_processing.hpp"
#include "photospider/core/image_buffer.hpp"

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
 * @brief Writes one FP32 scalar into an owned CPU ImageBuffer.
 * @param buffer Valid writable FP32 buffer.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param channel Channel index.
 * @param value Binary32 value to write.
 * @return Nothing.
 * @throws Nothing for valid test coordinates.
 */
void write_float(const ImageBuffer& buffer, int x, int y, int channel,
                 float value) noexcept {
  const std::size_t offset =
      static_cast<std::size_t>(y) * buffer.step +
      (static_cast<std::size_t>(x) * static_cast<std::size_t>(buffer.channels) +
       static_cast<std::size_t>(channel)) *
          sizeof(float);
  std::memcpy(static_cast<std::byte*>(buffer.data.get()) + offset, &value,
              sizeof(value));
}

/**
 * @brief Reads one FP32 scalar from an owned CPU ImageBuffer.
 * @param buffer Valid readable FP32 buffer.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param channel Channel index.
 * @return Stored binary32 value.
 * @throws Nothing for valid test coordinates.
 */
float read_float(const ImageBuffer& buffer, int x, int y,
                 int channel) noexcept {
  const std::size_t offset =
      static_cast<std::size_t>(y) * buffer.step +
      (static_cast<std::size_t>(x) * static_cast<std::size_t>(buffer.channels) +
       static_cast<std::size_t>(channel)) *
          sizeof(float);
  float value = 0.0F;
  std::memcpy(&value, static_cast<const std::byte*>(buffer.data.get()) + offset,
              sizeof(value));
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
  if (gate->try_trigger()) {
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
  ASSERT_TRUE(gate->try_trigger());

  EXPECT_TRUE(run.cancellation_source().request_cancellation(
      compute::ComputeRunCancellationReason::Superseded));
  EXPECT_EQ(gate->state(), compute::ProgressiveFinalGate::State::Triggered);
  const auto terminal = lease.terminal_outcome();
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->kind, compute::ComputeRunTerminalKind::Cancelled);
}

TEST(ExactBoxAverageFactorFour, RoundsOnceToNearestAndRestoresEnvironment) {
  TestFloatingEnvironment restore_environment;
  ImageBuffer source =
      make_aligned_cpu_image_buffer(8, 8, 4, DataType::FLOAT32);
  ImageBuffer destination =
      make_aligned_cpu_image_buffer(2, 2, 4, DataType::FLOAT32);
  const float lower = std::nextafter(1.0F, 2.0F);
  const float upper = std::nextafter(lower, 2.0F);
  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) {
      for (int channel = 0; channel < source.channels; ++channel) {
        write_float(source, x, y, channel,
                    ((x + y * source.width) % 2 == 0) ? lower : upper);
      }
    }
  }
  for (int y = 0; y < destination.height; ++y) {
    for (int x = 0; x < destination.width; ++x) {
      for (int channel = 0; channel < destination.channels; ++channel) {
        write_float(destination, x, y, channel, -7.0F);
      }
    }
  }

  ASSERT_EQ(fesetround(FE_DOWNWARD), 0);
  ASSERT_EQ(feclearexcept(FE_ALL_EXCEPT), 0);
  ASSERT_EQ(feraiseexcept(FE_INVALID), 0);
  image_processing::exact_box_average_factor_four_region(source, destination,
                                                         PixelRect{1, 0, 1, 1});

  EXPECT_EQ(fegetround(), FE_DOWNWARD);
  EXPECT_NE(fetestexcept(FE_INVALID), 0);
  for (int channel = 0; channel < destination.channels; ++channel) {
    EXPECT_EQ(read_float(destination, 1, 0, channel), upper);
    EXPECT_EQ(read_float(destination, 0, 0, channel), -7.0F);
    EXPECT_EQ(read_float(destination, 0, 1, channel), -7.0F);
    EXPECT_EQ(read_float(destination, 1, 1, channel), -7.0F);
  }
}

TEST(ExactBoxAverageFactorFour, RejectsWrongGeometryAndAliasing) {
  ImageBuffer source =
      make_aligned_cpu_image_buffer(8, 8, 4, DataType::FLOAT32);
  ImageBuffer wrong_destination =
      make_aligned_cpu_image_buffer(3, 2, 4, DataType::FLOAT32);
  EXPECT_THROW(image_processing::exact_box_average_factor_four_region(
                   source, wrong_destination, PixelRect{0, 0, 1, 1}),
               std::invalid_argument);
  EXPECT_THROW(image_processing::exact_box_average_factor_four_region(
                   source, source, PixelRect{0, 0, 1, 1}),
               std::invalid_argument);
  ImageBuffer destination =
      make_aligned_cpu_image_buffer(2, 2, 4, DataType::FLOAT32);
  EXPECT_THROW(image_processing::exact_box_average_factor_four_region(
                   source, destination, PixelRect{2, 0, 1, 1}),
               std::out_of_range);
}

/**
 * @brief Proves valid factor-four geometry cannot hide overlapping envelopes.
 * @throws Allocation failures are reported by GoogleTest.
 * @note Both cases use different starting addresses, so the test cannot pass
 * through the former start-pointer equality check.
 */
TEST(ExactBoxAverageFactorFour,
     RejectsGeometricallyValidOverlappingStorageEnvelopes) {
  auto shared_storage = std::make_shared<std::array<std::byte, 68U>>();
  std::shared_ptr<void> shared_source_owner(shared_storage,
                                            shared_storage->data());
  std::shared_ptr<void> shared_destination_owner(
      shared_storage, shared_storage->data() + sizeof(float));
  ImageBuffer shared_source{
      4, 4, 1, DataType::FLOAT32, Device::CPU, 16U, shared_source_owner, {}};
  ImageBuffer shared_destination{
      1, 1, 1, DataType::FLOAT32, Device::CPU, 4U, shared_destination_owner,
      {}};
  ASSERT_NE(shared_source.data.get(), shared_destination.data.get());
  ASSERT_FALSE(shared_source.data.owner_before(shared_destination.data));
  ASSERT_FALSE(shared_destination.data.owner_before(shared_source.data));
  EXPECT_THROW(image_processing::exact_box_average_factor_four_region(
                   shared_source, shared_destination, PixelRect{0, 0, 1, 1}),
               std::invalid_argument);

  std::array<std::byte, 68U> distinct_storage{};
  /**
   * @brief Suppresses deletion for stack-owned test storage.
   * @param storage Borrowed address whose lifetime is owned by the test scope.
   * @return Nothing.
   * @throws Nothing.
   * @note Both independent control blocks are destroyed before the underlying
   * lexical storage leaves scope.
   */
  const auto retain_external_storage = [](void* storage) noexcept {
    static_cast<void>(storage);
  };
  std::shared_ptr<void> distinct_source_owner(distinct_storage.data(),
                                              retain_external_storage);
  std::shared_ptr<void> distinct_destination_owner(
      distinct_storage.data() + sizeof(float), retain_external_storage);
  ImageBuffer distinct_source{
      4, 4, 1, DataType::FLOAT32, Device::CPU, 16U, distinct_source_owner, {}};
  ImageBuffer distinct_destination{
      1, 1, 1, DataType::FLOAT32, Device::CPU, 4U, distinct_destination_owner,
      {}};
  ASSERT_NE(distinct_source.data.get(), distinct_destination.data.get());
  ASSERT_TRUE(distinct_source.data.owner_before(distinct_destination.data) ||
              distinct_destination.data.owner_before(distinct_source.data));
  EXPECT_THROW(
      image_processing::exact_box_average_factor_four_region(
          distinct_source, distinct_destination, PixelRect{0, 0, 1, 1}),
      std::invalid_argument);
}

}  // namespace
}  // namespace ps::testing
