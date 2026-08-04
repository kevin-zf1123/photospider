/**
 * @file test_progressive_compute.cpp
 * @brief Tests private progressive gating and exact preview-source arithmetic.
 */
#include <fenv.h>  // NOLINT(build/c++11)
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

#include "compute/progressive_compute.hpp"
#include "core/image_buffer_processing.hpp"
#include "photospider/core/image_buffer.hpp"

namespace ps::testing {
namespace {

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

}  // namespace
}  // namespace ps::testing
