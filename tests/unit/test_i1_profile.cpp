#include <fenv.h>  // NOLINT(build/c++11)
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/i1/i1_profile.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Restores the calling test thread's complete floating-point state.
 *
 * @throws std::runtime_error when the initial environment cannot be captured.
 * @note This owner does not modify the environment after capture. Destruction
 * restores rounding, sticky exception flags, and every other environment
 * control; restoration failure terminates because contaminating a later test
 * would make suite behavior order-dependent.
 */
class ScopedTestFloatingPointEnvironment final {
 public:
  /**
   * @brief Captures the complete environment of the current test thread.
   * @throws std::runtime_error when `fegetenv` cannot capture that state.
   */
  ScopedTestFloatingPointEnvironment() {
    if (fegetenv(&initial_environment_) != 0) {
      throw std::runtime_error(
          "cannot capture the test thread floating-point environment");
    }
  }

  /**
   * @brief Restores the complete environment captured at construction.
   * @throws Nothing; restoration failure terminates the process.
   */
  ~ScopedTestFloatingPointEnvironment() noexcept {
    if (fesetenv(&initial_environment_) != 0) {
      std::terminate();
    }
  }

  /**
   * @brief Prevents duplicate ownership of test-thread restoration.
   * @param other Guard retaining restoration responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedTestFloatingPointEnvironment(
      const ScopedTestFloatingPointEnvironment& other) = delete;

  /**
   * @brief Prevents replacing test-thread restoration ownership.
   * @param other Guard that remains responsible for its captured environment.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedTestFloatingPointEnvironment& operator=(
      const ScopedTestFloatingPointEnvironment& other) = delete;

 private:
  /** @brief Complete calling-thread environment restored after the test. */
  fenv_t initial_environment_{};
};

/**
 * @brief Owns one RNE scope for the independent mathematical oracle.
 *
 * @throws std::runtime_error when the platform cannot capture the complete
 * calling-thread environment, install RNE, or restore the captured environment
 * after installation fails.
 * @note The complete prior environment, including rounding and sticky
 * exception flags, is restored on every normal or exceptional exit.
 * Destruction-time restoration failure terminates because leaking oracle state
 * would contaminate later tests on the reused thread.
 */
class ScopedReferenceBinary32RoundToNearest final {
 public:
  /**
   * @brief Captures the complete caller environment and installs RNE.
   * @throws std::runtime_error when capture or installation fails. After a
   * successful capture, installation failure first attempts complete
   * restoration and reports separately if that recovery also fails.
   */
  ScopedReferenceBinary32RoundToNearest() {
    if (fegetenv(&previous_environment_) != 0) {
      throw std::runtime_error(
          "I1 oracle cannot capture the floating-point environment");
    }
    if (fesetround(FE_TONEAREST) != 0) {
      if (fesetenv(&previous_environment_) != 0) {
        throw std::runtime_error(
            "I1 oracle cannot install binary32 RNE rounding or restore the "
            "floating-point environment");
      }
      throw std::runtime_error(
          "I1 oracle cannot install binary32 RNE rounding");
    }
  }

  /**
   * @brief Restores the caller's complete prior floating-point environment.
   * @throws Nothing; restoration failure terminates the process.
   * @note Full restoration removes exceptions raised by oracle arithmetic while
   * preserving every flag and control value present on scope entry.
   */
  ~ScopedReferenceBinary32RoundToNearest() noexcept {
    if (fesetenv(&previous_environment_) != 0) {
      std::terminate();
    }
  }

  /**
   * @brief Prevents duplicate ownership of one oracle rounding scope.
   * @param other Guard retaining restoration responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedReferenceBinary32RoundToNearest(
      const ScopedReferenceBinary32RoundToNearest& other) = delete;

  /**
   * @brief Prevents replacing one oracle rounding scope.
   * @param other Guard retaining restoration responsibility.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedReferenceBinary32RoundToNearest& operator=(
      const ScopedReferenceBinary32RoundToNearest& other) = delete;

 private:
  /**
   * @brief Complete environment restored before returning to the caller.
   * @note The value becomes valid only after successful `fegetenv`; a failed
   * constructor never reaches destruction.
   */
  fenv_t previous_environment_{};
};

/**
 * @brief Independently rounds one exact byte fraction to IEEE binary32.
 * @param numerator Unsigned numerator in `[0,255]`.
 * @return Round-to-nearest-ties-to-even encoding of `numerator / 255`.
 * @throws Nothing.
 * @note This test oracle deliberately duplicates the frozen mathematical
 * contract and does not invoke the OpenCV operation provider.
 */
float reference_byte_fraction(std::uint8_t numerator) noexcept {
  if (numerator == 0U) {
    return 0.0F;
  }
  if (numerator == 255U) {
    const std::uint32_t bits = 0x3f800000U;
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
  }

  unsigned int highest_bit = 0U;
  for (std::uint32_t value = numerator; value > 1U; value >>= 1U) {
    ++highest_bit;
  }
  int exponent = static_cast<int>(highest_bit) - 8;
  const unsigned int shift = static_cast<unsigned int>(23 - exponent);
  const std::uint64_t scaled = static_cast<std::uint64_t>(numerator) << shift;
  std::uint64_t significand = scaled / 255U;
  const std::uint64_t remainder = scaled % 255U;
  if (remainder * 2U > 255U ||
      (remainder * 2U == 255U && (significand & 1U) != 0U)) {
    ++significand;
  }
  if (significand == (std::uint64_t{1U} << 24U)) {
    significand >>= 1U;
    ++exponent;
  }
  const std::uint32_t bits =
      (static_cast<std::uint32_t>(exponent + 127) << 23U) |
      static_cast<std::uint32_t>(significand - (std::uint64_t{1U} << 23U));
  float result = 0.0F;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

/**
 * @brief Applies one independently specified curve stage with binary32 cuts.
 * @param input Exact binary32 input sample.
 * @param coefficient Exact binary32 stage coefficient.
 * @return Binary32 result of `1 / (1 + input * coefficient)`.
 * @throws Nothing.
 * @note Volatile intermediates forbid contraction and retain the three
 * contract rounding boundaries without calling candidate provider code. The
 * caller owns an active `ScopedReferenceBinary32RoundToNearest`.
 */
float reference_curve_stage(float input, float coefficient) noexcept {
  volatile float product = input * coefficient;
  volatile float denominator = 1.0F + product;
  volatile float result = 1.0F / denominator;
  return result;
}

/**
 * @brief Recomputes the frozen final digest from the mathematical contract.
 * @return Canonical-v1 logical-content digest of the exact edit-eleven image.
 * @throws std::runtime_error when the complete caller environment cannot be
 * captured, RNE cannot be installed, failed installation cannot restore the
 * captured environment, or the canonical digest is unavailable.
 * @throws std::invalid_argument when the fixed tensor descriptor, image facet,
 * layout, or storage envelope is rejected.
 * @throws std::overflow_error when the fixed Value address envelope cannot be
 * represented.
 * @throws std::bad_alloc when oracle storage, immutable Value state, or digest
 * metadata cannot allocate.
 * @note The oracle constructs HWC bytes directly and never loads the frozen
 * YAML, Host, Kernel, scheduler, cache, or OpenCV provider implementation. It
 * restores the complete caller environment on normal and exceptional exit;
 * destruction-time restoration failure is fail-stop.
 */
ContentDigest recompute_i1_golden_from_reference_contract() {
  ScopedReferenceBinary32RoundToNearest rounding_scope;
  constexpr std::size_t kChannels = 4U;
  constexpr std::size_t kElementBytes = sizeof(float);
  constexpr std::array<float, kI1FrozenCurveNodeCount> kCoefficients{
      1.04F, 1.00F, 1.20F, 1.40F};
  const std::size_t element_count =
      kI1FrozenImageEdge * kI1FrozenImageEdge * kChannels;
  std::vector<std::byte> storage(element_count * kElementBytes);
  std::size_t offset = 0U;
  for (std::size_t y = 0U; y < kI1FrozenImageEdge; ++y) {
    for (std::size_t x = 0U; x < kI1FrozenImageEdge; ++x) {
      for (std::size_t channel = 0U; channel < kChannels; ++channel) {
        const std::uint8_t numerator = static_cast<std::uint8_t>(
            (17U * x + 31U * y + 47U * channel) & 255U);
        float sample = reference_byte_fraction(numerator);
        for (const float coefficient : kCoefficients) {
          sample = reference_curve_stage(sample, coefficient);
        }
        std::memcpy(storage.data() + offset, &sample, sizeof(sample));
        offset += sizeof(sample);
      }
    }
  }

  DenseTensorDescriptor descriptor;
  descriptor.shape = {kI1FrozenImageEdge, kI1FrozenImageEdge, kChannels};
  descriptor.element_semantics = ElementSemantics::FloatingPoint;
  descriptor.storage_encoding = StorageEncoding{32U};
  const ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  const std::ptrdiff_t row_stride = static_cast<std::ptrdiff_t>(
      kI1FrozenImageEdge * kChannels * kElementBytes);
  const std::ptrdiff_t pixel_stride =
      static_cast<std::ptrdiff_t>(kChannels * kElementBytes);
  Value value = Value::from_cpu_dense_tensor(
      std::move(descriptor), image,
      StridedLayout{{row_stride, pixel_stride,
                     static_cast<std::ptrdiff_t>(kElementBytes)}},
      std::move(storage));
  const ContentDigestResult result = compute_content_digest(value);
  if (result.state != ContentDigestState::Available ||
      !result.digest.has_value()) {
    throw std::runtime_error("independent I1 reference digest unavailable: " +
                             result.diagnostic);
  }
  return *result.digest;
}

/**
 * @brief Creates one tiny valid tensor for collector ownership regressions.
 * @return Immutable one-pixel FP32 Value with a complete HWC descriptor.
 * @throws Value descriptor, storage, and allocation failures unchanged.
 */
Value make_collector_test_output() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {1U, 1U, 1U};
  descriptor.element_semantics = ElementSemantics::FloatingPoint;
  descriptor.storage_encoding = StorageEncoding{32U};
  const ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  const float sample = 0.5F;
  std::vector<std::byte> storage(sizeof(sample));
  std::memcpy(storage.data(), &sample, sizeof(sample));
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), image,
      StridedLayout{{static_cast<std::ptrdiff_t>(sizeof(sample)),
                     static_cast<std::ptrdiff_t>(sizeof(sample)),
                     static_cast<std::ptrdiff_t>(sizeof(sample))}},
      std::move(storage));
}

/**
 * @brief Proves the literal golden is independently recomputable.
 * @throws Nothing when the mathematical oracle and frozen bytes agree;
 * allocation/descriptor failures are reported by GoogleTest as test errors.
 */
TEST(I1Profile, FrozenGoldenMatchesIndependentReferenceContract) {
  EXPECT_EQ(recompute_i1_golden_from_reference_contract(),
            i1_frozen_final_content_digest());
}

/**
 * @brief Proves the independent golden ignores and preserves ambient fenv.
 * @throws Floating-point setup, allocation, descriptor, and digest failures
 * unchanged to GoogleTest.
 * @note The test installs upward rounding and two sticky exceptions before the
 * full source, four-stage, and digest calculation. The oracle must still reach
 * the frozen RNE golden and restore both rounding and every `FE_ALL_EXCEPT`
 * flag. The outer owner restores the test thread's original environment after
 * any assertion or exception.
 */
TEST(I1Profile, FrozenGoldenRestoresNonDefaultCallerEnvironment) {
  ScopedTestFloatingPointEnvironment restore_initial_environment;
  ASSERT_EQ(fesetenv(FE_DFL_ENV), 0);
  ASSERT_EQ(fesetround(FE_UPWARD), 0);
  ASSERT_EQ(feclearexcept(FE_ALL_EXCEPT), 0);
  constexpr int kPresetExceptions = FE_INVALID | FE_DIVBYZERO;
  ASSERT_EQ(feraiseexcept(kPresetExceptions), 0);
  ASSERT_EQ(fegetround(), FE_UPWARD);
  ASSERT_EQ(fetestexcept(FE_ALL_EXCEPT), kPresetExceptions);

  EXPECT_EQ(recompute_i1_golden_from_reference_contract(),
            i1_frozen_final_content_digest());

  EXPECT_EQ(fegetround(), FE_UPWARD);
  EXPECT_EQ(fetestexcept(FE_ALL_EXCEPT), kPresetExceptions);
}

/**
 * @brief Captures private QoS facts from one fake Host admission.
 * @throws Nothing for value construction and copying.
 */
struct CapturedAdmission final {
  /** @brief Exact private QoS received by the final call. */
  compute::ComputeRunQos qos;

  /** @brief Ordinary request after collector normalization. */
  HostComputeRequest request;

  /** @brief Whether a non-null observation-only sink was supplied. */
  bool has_observation_sink = false;

  /** @brief Exact pre-call coordinate delivered at Host invocation. */
  std::optional<I1AcceptedCoordinate> accepted_coordinate;
};

/**
 * @brief Deterministic I1 Host fake used only for admission-boundary tests.
 *
 * The fake records final-call values and returns a preselected scheduling
 * status. It does not emulate Kernel, scheduling, cancellation, or graph state;
 * real-path behavior is covered separately by embedded integration tests.
 *
 * @throws std::bad_alloc when captured request/status storage allocates.
 */
class RecordingI1Host final : public I1Host {
 public:
  /** @copydoc I1Host::compute_i1_async */
  Result<std::future<OperationStatus>> compute_i1_async(
      I1HostComputeRequest request) override {
    admissions.push_back(CapturedAdmission{request.qos, request.request,
                                           request.observation_sink != nullptr,
                                           request.accepted_coordinate});
    Result<std::future<OperationStatus>> result;
    result.status = schedule_status;
    if (result.status.ok) {
      std::promise<OperationStatus> promise;
      result.value = promise.get_future();
      promise.set_value(settlement_status);
    }
    return result;
  }

  /** @copydoc I1Host::i1_execution_snapshot */
  I1ExecutionSnapshot i1_execution_snapshot(std::uint64_t after_cursor,
                                            std::size_t limit) const override {
    static_cast<void>(after_cursor);
    static_cast<void>(limit);
    return I1ExecutionSnapshot{};
  }

  /** @brief Scheduling status returned directly by the final Host call. */
  OperationStatus schedule_status;

  /** @brief Later status fulfilled through the accepted settlement future. */
  OperationStatus settlement_status;

  /** @brief Complete ordered list of final Host calls. */
  std::vector<CapturedAdmission> admissions;
};

/**
 * @brief Constructs a minimal ordinary request for fake-boundary verification.
 * @param edit_index Frozen edit whose Region is attached.
 * @return Host request ready for collector normalization.
 * @throws std::out_of_range for an invalid edit index.
 */
HostComputeRequest make_test_request(std::size_t edit_index) {
  HostComputeRequest request;
  request.session = GraphSessionId{"i1-test-session"};
  request.node = NodeId{4};
  request.dirty_roi = i1_edit_region(edit_index);
  return request;
}

/**
 * @brief Creates one edit-specific preallocated observation sink.
 * @param collector Episode observation store that outlives the sink.
 * @param edit_index Frozen edit identity.
 * @return Shared observation-only sink.
 * @throws The collector's index/allocation errors unchanged.
 */
std::shared_ptr<compute::ComputeRunObservationSink> make_test_sink(
    I1EpisodeObservationCollector& collector, std::size_t edit_index) {
  return collector.make_edit_sink(edit_index);
}

/**
 * @brief Preserves the exact pre-call coordinate and deadline on Host success.
 * @throws Nothing when deterministic fake inputs satisfy the frozen contract.
 */
TEST(I1AcceptedBoundaryCollector, SuccessfulCallUsesPreCallCoordinate) {
  RecordingI1Host host;
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(1000000000));
  const auto nominal = checked_i1_time_add(origin, kI1MeasurementStartOffset);
  const auto admission = checked_i1_time_add(nominal, kI1AdmissionLateness);
  const auto returned =
      checked_i1_time_add(admission, std::chrono::nanoseconds(123));
  std::vector<std::chrono::steady_clock::time_point> samples{admission,
                                                             returned};
  std::size_t sample_index = 0U;
  std::vector<std::chrono::steady_clock::time_point> sleeps;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [&sleeps](auto target) { sleeps.push_back(target); }, 7U);

  I1EditAdmissionResult result = collector.admit_edit(
      origin, 11U, make_test_request(11U), make_test_sink(observations, 11U));

  ASSERT_EQ(sleeps.size(), 1U);
  EXPECT_EQ(sleeps.front(), nominal);
  EXPECT_TRUE(result.admission_attempted);
  EXPECT_TRUE(result.admission_window_valid);
  ASSERT_TRUE(result.reserved_event_sequence.has_value());
  EXPECT_EQ(*result.reserved_event_sequence, 7U);
  ASSERT_TRUE(result.deadline.has_value());
  EXPECT_EQ(*result.deadline,
            checked_i1_time_add(admission, kI1DeadlineBudget));
  ASSERT_TRUE(result.host_return.has_value());
  EXPECT_EQ(result.host_return->return_time, returned);
  EXPECT_TRUE(result.host_return->status.ok);
  EXPECT_TRUE(result.host_return->future_valid);
  ASSERT_TRUE(result.accepted_coordinate.has_value());
  EXPECT_EQ(result.accepted_coordinate->admission_time(), admission);
  EXPECT_EQ(result.accepted_coordinate->event_sequence(), 7U);
  ASSERT_TRUE(result.settlement.valid());
  EXPECT_TRUE(result.settlement.get().ok);

  ASSERT_EQ(host.admissions.size(), 1U);
  const CapturedAdmission& captured = host.admissions.front();
  EXPECT_EQ(captured.qos.service_class,
            compute::ComputeRunQosClass::Interactive);
  EXPECT_EQ(captured.qos.deadline, result.deadline);
  EXPECT_EQ(captured.qos.weight, 1U);
  EXPECT_EQ(captured.qos.maximum_parallelism, std::optional<std::uint32_t>(8U));
  EXPECT_TRUE(captured.request.execution.parallel);
  EXPECT_EQ(captured.request.execution.maximum_parallelism,
            std::optional<std::uint32_t>(8U));
  EXPECT_EQ(captured.request.intent,
            std::optional<ComputeIntent>(ComputeIntent::GlobalHighPrecision));
  EXPECT_TRUE(captured.has_observation_sink);
  ASSERT_TRUE(captured.accepted_coordinate.has_value());
  EXPECT_EQ(captured.accepted_coordinate->admission_time(), admission);
  EXPECT_EQ(captured.accepted_coordinate->event_sequence(), 7U);
}

/**
 * @brief Retains reservation/return facts without accepting a failed call.
 * @throws Nothing when the fake returns one canonical Graph failure status.
 */
TEST(I1AcceptedBoundaryCollector, FailedHostCallCreatesNoAcceptedEvent) {
  RecordingI1Host host;
  host.schedule_status.ok = false;
  host.schedule_status.domain = OperationErrorDomain::Graph;
  host.schedule_status.code =
      static_cast<std::int32_t>(GraphErrc::ComputeError);
  host.schedule_status.name = "compute-error";
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(2000000000));
  std::vector<std::chrono::steady_clock::time_point> samples{
      origin, checked_i1_time_add(origin, std::chrono::nanoseconds(50))};
  std::size_t sample_index = 0U;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [](auto) {}, 19U);

  I1EditAdmissionResult result = collector.admit_edit(
      origin, 0U, make_test_request(0U), make_test_sink(observations, 0U));

  EXPECT_TRUE(result.admission_attempted);
  EXPECT_TRUE(result.admission_window_valid);
  EXPECT_EQ(result.reserved_event_sequence, std::optional<std::uint64_t>(19U));
  EXPECT_TRUE(result.deadline.has_value());
  ASSERT_TRUE(result.host_return.has_value());
  EXPECT_FALSE(result.host_return->status.ok);
  EXPECT_FALSE(result.host_return->future_valid);
  EXPECT_FALSE(result.accepted_coordinate.has_value());
  EXPECT_FALSE(result.settlement.valid());
  ASSERT_EQ(host.admissions.size(), 1U);
  EXPECT_TRUE(observations.snapshot().current_generations.empty());
  ASSERT_TRUE(host.admissions.front().accepted_coordinate.has_value());
  EXPECT_EQ(host.admissions.front().accepted_coordinate->admission_time(),
            origin);
  EXPECT_EQ(host.admissions.front().accepted_coordinate->event_sequence(), 19U);
}

/**
 * @brief Rejects early/late samples without calls, reservations, or backfill.
 * @throws Nothing when scripted samples cover both exclusive invalid sides.
 */
TEST(I1AcceptedBoundaryCollector, InvalidWindowsNeverCallOrReserve) {
  RecordingI1Host host;
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(3000000000));
  const auto second_nominal = checked_i1_time_add(origin, kI1EditStride);
  std::vector<std::chrono::steady_clock::time_point> samples{
      origin - std::chrono::nanoseconds(1),
      checked_i1_time_add(second_nominal,
                          kI1AdmissionLateness + std::chrono::nanoseconds(1))};
  std::size_t sample_index = 0U;
  std::vector<std::chrono::steady_clock::time_point> sleeps;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [&sleeps](auto target) { sleeps.push_back(target); });

  I1EditAdmissionResult early = collector.admit_edit(
      origin, 0U, make_test_request(0U), make_test_sink(observations, 0U));
  I1EditAdmissionResult late = collector.admit_edit(
      origin, 1U, make_test_request(1U), make_test_sink(observations, 1U));

  EXPECT_TRUE(early.admission_attempted);
  EXPECT_TRUE(late.admission_attempted);
  EXPECT_FALSE(early.admission_window_valid);
  EXPECT_FALSE(late.admission_window_valid);
  EXPECT_FALSE(early.reserved_event_sequence.has_value());
  EXPECT_FALSE(late.reserved_event_sequence.has_value());
  EXPECT_FALSE(early.deadline.has_value());
  EXPECT_FALSE(late.deadline.has_value());
  EXPECT_FALSE(early.host_return.has_value());
  EXPECT_FALSE(late.host_return.has_value());
  EXPECT_TRUE(host.admissions.empty());
  EXPECT_TRUE(observations.snapshot().current_generations.empty());
  ASSERT_EQ(sleeps.size(), 2U);
  EXPECT_EQ(sleeps[0], origin);
  EXPECT_EQ(sleeps[1], second_nominal);
}

/**
 * @brief Accepts both inclusive window endpoints with increasing sequences.
 * @throws Nothing when fake calls and settlement futures succeed.
 */
TEST(I1AcceptedBoundaryCollector, AdmissionWindowEndpointsAreInclusive) {
  RecordingI1Host host;
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(4000000000));
  const auto second_nominal = checked_i1_time_add(origin, kI1EditStride);
  std::vector<std::chrono::steady_clock::time_point> samples{
      origin, origin, checked_i1_time_add(second_nominal, kI1AdmissionLateness),
      checked_i1_time_add(second_nominal, kI1AdmissionLateness)};
  std::size_t sample_index = 0U;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [](auto) {}, 41U);

  I1EditAdmissionResult first = collector.admit_edit(
      origin, 0U, make_test_request(0U), make_test_sink(observations, 0U));
  I1EditAdmissionResult second = collector.admit_edit(
      origin, 1U, make_test_request(1U), make_test_sink(observations, 1U));

  EXPECT_TRUE(first.admission_window_valid);
  EXPECT_TRUE(second.admission_window_valid);
  EXPECT_EQ(first.accepted_coordinate->event_sequence(), 41U);
  EXPECT_EQ(second.accepted_coordinate->event_sequence(), 42U);
  EXPECT_EQ(host.admissions.size(), 2U);
}

/**
 * @brief Uses UINT64_MAX once and rejects sequence reuse before a second call.
 * @throws Nothing when exhaustion is reported as the required overflow error.
 */
TEST(I1AcceptedBoundaryCollector, SequenceExhaustionPreventsSecondHostCall) {
  RecordingI1Host host;
  I1EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(5000000000));
  const auto second_nominal = checked_i1_time_add(origin, kI1EditStride);
  std::vector<std::chrono::steady_clock::time_point> samples{origin, origin,
                                                             second_nominal};
  std::size_t sample_index = 0U;
  I1AcceptedBoundaryCollector collector(
      host, [&samples, &sample_index] { return samples.at(sample_index++); },
      [](auto) {}, std::numeric_limits<std::uint64_t>::max());

  I1EditAdmissionResult first = collector.admit_edit(
      origin, 0U, make_test_request(0U), make_test_sink(observations, 0U));
  ASSERT_TRUE(first.accepted_coordinate.has_value());
  EXPECT_EQ(first.accepted_coordinate->event_sequence(),
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_THROW(collector.admit_edit(origin, 1U, make_test_request(1U),
                                    make_test_sink(observations, 1U)),
               std::overflow_error);
  EXPECT_EQ(host.admissions.size(), 1U);
}

/**
 * @brief Derives the grid, runner deadlines, guard, and equal-time order.
 * @throws Nothing when all exact constants and checked boundaries agree.
 */
TEST(I1FrozenArithmetic, GridRegionsAndTieOrderRemainExact) {
  const auto grid_origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(6000000000));
  EXPECT_EQ(i1_episode_origin(grid_origin, 0U), grid_origin);
  EXPECT_EQ(i1_episode_origin(grid_origin, 220U),
            checked_i1_time_add(grid_origin,
                                std::chrono::nanoseconds(220LL * 750000000LL)));
  EXPECT_EQ(i1_terminal_boundary(grid_origin),
            checked_i1_time_add(grid_origin,
                                std::chrono::nanoseconds(221LL * 750000000LL)));
  EXPECT_EQ(classify_i1_slot(0U),
            (std::pair<I1EpisodePhase, std::size_t>{I1EpisodePhase::Cold, 0U}));
  EXPECT_EQ(classify_i1_slot(20U), (std::pair<I1EpisodePhase, std::size_t>{
                                       I1EpisodePhase::Warmup, 19U}));
  EXPECT_EQ(classify_i1_slot(21U), (std::pair<I1EpisodePhase, std::size_t>{
                                       I1EpisodePhase::Measured, 0U}));
  EXPECT_EQ(classify_i1_slot(220U), (std::pair<I1EpisodePhase, std::size_t>{
                                        I1EpisodePhase::Measured, 199U}));
  EXPECT_EQ(i1_edit_region(0U), (PixelRect{0, 0, 256, 256}));
  EXPECT_EQ(i1_edit_region(11U), (PixelRect{768, 512, 256, 256}));
  EXPECT_EQ(kI1MeasurementEndOffset + kI1NextOriginGuard, kI1EpisodeStride);
  const auto next_episode_origin = i1_episode_origin(grid_origin, 1U);
  EXPECT_EQ(checked_i1_time_subtract(next_episode_origin, kI1AdmissionLateness),
            checked_i1_time_add(grid_origin,
                                kI1EpisodeStride - kI1AdmissionLateness));
  const auto measurement_end =
      checked_i1_time_add(grid_origin, kI1MeasurementEndOffset);
  EXPECT_EQ(
      checked_i1_time_subtract(measurement_end, kI1DigestFreezeSafetyMargin),
      checked_i1_time_add(
          grid_origin, kI1MeasurementEndOffset - kI1DigestFreezeSafetyMargin));
  const auto latest_final_admission = checked_i1_time_add(
      checked_i1_time_add(grid_origin, kI1MeasurementStartOffset),
      kI1AdmissionLateness);
  EXPECT_EQ(checked_i1_time_add(latest_final_admission, kI1DeadlineBudget),
            checked_i1_time_add(grid_origin, kI1LatestFinalDeadlineOffset));
  EXPECT_EQ(
      i1_measurement_start_tie_rank(I1MeasurementStartEventKind::NominalMarker),
      0);
  EXPECT_EQ(i1_measurement_start_tie_rank(
                I1MeasurementStartEventKind::AcceptedAdmission),
            1);
  EXPECT_THROW(i1_episode_origin(grid_origin, kI1GridSlotCount),
               std::out_of_range);
  EXPECT_THROW(checked_i1_time_add(std::chrono::steady_clock::time_point::max(),
                                   std::chrono::nanoseconds(1)),
               std::overflow_error);
  EXPECT_THROW(
      checked_i1_time_subtract(std::chrono::steady_clock::time_point::min(),
                               std::chrono::nanoseconds(1)),
      std::overflow_error);
  EXPECT_THROW(
      checked_i1_time_subtract(grid_origin, std::chrono::nanoseconds(-1)),
      std::invalid_argument);
}

/**
 * @brief Proves the derived service-start store is lossless at its exact bound.
 * @throws Allocation and ComputeRun construction failures unchanged.
 * @note Synthetic callbacks exercise only the fixed collector slots; this is
 * not the 221-slot benchmark workload and performs no product computation.
 */
TEST(I1EpisodeObservationCollector,
     DerivedServiceStartCapacityFailsClosedOnlyAfterBoundary) {
  EXPECT_EQ(kI1FrozenTilesPerCurveNode, 64U);
  EXPECT_EQ(kI1MaximumServiceStartsPerRun, 257U);
  EXPECT_EQ(kI1EpisodeServiceStartCapacity, 3084U);

  I1EpisodeObservationCollector collector;
  std::shared_ptr<compute::ComputeRunObservationSink> sink =
      collector.make_edit_sink(0U);
  compute::ComputeRunSubmission submission{
      "i1-service-start-capacity",
      GraphInstanceId{7001U},
      GraphRevision{7001U},
      kI1TargetNodeId,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             std::nullopt, 1U, 8U},
      compute::SupersessionIdentity{
          compute::SupersessionKey(kI1TargetNodeId,
                                   ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1U)},
      nullptr};
  compute::ComputeRun run(std::move(submission));
  compute::ComputeRunLease lease = run.acquire_lease();

  for (std::size_t index = 0U; index < kI1EpisodeServiceStartCapacity;
       ++index) {
    const compute::ComputeRunObservationCoordinate coordinate =
        sink->reserve_causal_coordinate();
    sink->on_service_start(
        lease.descriptor(), lease.task_identity(index), 1U,
        compute::ComputeRunServiceStartObservation{true, true, true},
        coordinate);
  }
  const I1EpisodeObservationSnapshot at_capacity = collector.snapshot();
  EXPECT_FALSE(at_capacity.overflowed);
  EXPECT_EQ(at_capacity.service_starts.size(), kI1EpisodeServiceStartCapacity);

  const compute::ComputeRunObservationCoordinate overflow_coordinate =
      sink->reserve_causal_coordinate();
  sink->on_service_start(
      lease.descriptor(), lease.task_identity(kI1EpisodeServiceStartCapacity),
      1U, compute::ComputeRunServiceStartObservation{true, true, true},
      overflow_coordinate);
  const I1EpisodeObservationSnapshot overflowed = collector.snapshot();
  EXPECT_TRUE(overflowed.overflowed);
  EXPECT_EQ(overflowed.service_starts.size(), kI1EpisodeServiceStartCapacity);
}

/**
 * @brief Proves digest freezing releases the collector's retained Value once.
 * @throws Allocation, descriptor, digest, and ComputeRun failures unchanged.
 */
TEST(I1EpisodeObservationCollector,
     FrozenVisibleDigestReleasesCollectorSlotAndRemainsIdempotent) {
  I1EpisodeObservationCollector collector;
  std::shared_ptr<compute::ComputeRunObservationSink> sink =
      collector.make_edit_sink(kI1EditCount - 1U);
  compute::ComputeRunSubmission submission{
      "i1-visible-freeze",
      GraphInstanceId{8001U},
      GraphRevision{8001U},
      kI1TargetNodeId,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             std::nullopt, 1U, 8U},
      compute::SupersessionIdentity{
          compute::SupersessionKey(kI1TargetNodeId,
                                   ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1U)},
      nullptr};
  compute::ComputeRun run(std::move(submission));
  compute::ComputeRunLease lease = run.acquire_lease();
  sink->on_current_visible(lease.descriptor(), make_collector_test_output(),
                           sink->reserve_causal_coordinate());

  const I1EpisodeObservationSnapshot before = collector.snapshot();
  ASSERT_EQ(before.visible_outputs.size(), 1U);
  EXPECT_TRUE(before.visible_outputs.front().output.valid());
  EXPECT_FALSE(before.visible_outputs.front().content_digest.has_value());

  EXPECT_EQ(collector.freeze_visible_output_digests(), 1U);
  EXPECT_EQ(collector.freeze_visible_output_digests(), 1U);
  const I1EpisodeObservationSnapshot frozen = collector.snapshot();
  ASSERT_EQ(frozen.visible_outputs.size(), 1U);
  EXPECT_FALSE(frozen.visible_outputs.front().output.valid());
  EXPECT_TRUE(frozen.visible_outputs.front().value_valid_at_capture);
  ASSERT_TRUE(frozen.visible_outputs.front().content_digest.has_value());
  EXPECT_EQ(frozen.visible_outputs.front().content_digest->state,
            ContentDigestState::Available);
}

/**
 * @brief Proves a late publication can be released without post-cut hashing.
 * @throws Allocation, descriptor, and ComputeRun failures unchanged.
 */
TEST(I1EpisodeObservationCollector,
     UnfrozenVisibleOutputReleasesAsExplicitMissingDigestEvidence) {
  I1EpisodeObservationCollector collector;
  std::shared_ptr<compute::ComputeRunObservationSink> sink =
      collector.make_edit_sink(0U);
  compute::ComputeRunSubmission submission{
      "i1-visible-release",
      GraphInstanceId{8002U},
      GraphRevision{8002U},
      kI1TargetNodeId,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             std::nullopt, 1U, 8U},
      compute::SupersessionIdentity{
          compute::SupersessionKey(kI1TargetNodeId,
                                   ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1U)},
      nullptr};
  compute::ComputeRun run(std::move(submission));
  compute::ComputeRunLease lease = run.acquire_lease();
  sink->on_current_visible(lease.descriptor(), make_collector_test_output(),
                           sink->reserve_causal_coordinate());

  collector.release_unfrozen_visible_outputs();
  const I1EpisodeObservationSnapshot released = collector.snapshot();
  ASSERT_EQ(released.visible_outputs.size(), 1U);
  EXPECT_FALSE(released.visible_outputs.front().output.valid());
  EXPECT_TRUE(released.visible_outputs.front().value_valid_at_capture);
  EXPECT_FALSE(released.visible_outputs.front().content_digest.has_value());
}

/**
 * @brief Freezes the exact source/serial-transform document and Host request.
 * @throws Nothing when owned YAML/request construction matches the workload.
 */
TEST(I1FrozenWorkload, GraphMutationAndRequestRemainExact) {
  const std::string graph = i1_frozen_graph_yaml();
  EXPECT_NE(graph.find("width: 2048\n    height: 2048\n    channels: 4\n"
                       "    seed: 0\n"),
            std::string::npos);
  EXPECT_NE(graph.find("id: 1\n  name: i1_curve_one"), std::string::npos);
  EXPECT_NE(graph.find("id: 4\n  name: i1_curve_four"), std::string::npos);
  EXPECT_NE(graph.find("k: 0.80"), std::string::npos);
  EXPECT_NE(graph.find("k: 1.00"), std::string::npos);
  EXPECT_NE(graph.find("k: 1.20"), std::string::npos);
  EXPECT_NE(graph.find("k: 1.40"), std::string::npos);

  const std::string final_edit = i1_edit_node_one_yaml(11U);
  EXPECT_NE(final_edit.find("from_node_id: 0"), std::string::npos);
  EXPECT_NE(final_edit.find("k: 1.04\n"), std::string::npos);
  EXPECT_THROW(i1_edit_node_one_yaml(kI1EditCount), std::out_of_range);

  const GraphSessionId session{"i1-frozen-workload"};
  const HostComputeRequest request = make_i1_host_compute_request(session, 11U);
  EXPECT_EQ(request.session.value, session.value);
  EXPECT_EQ(request.node.value, kI1TargetNodeId);
  EXPECT_EQ(request.cache.precision, "fp32");
  EXPECT_TRUE(request.cache.force_recache);
  EXPECT_TRUE(request.cache.disable_disk_cache);
  EXPECT_TRUE(request.cache.nosave);
  EXPECT_TRUE(request.execution.parallel);
  EXPECT_TRUE(request.execution.quiet);
  EXPECT_EQ(request.execution.maximum_parallelism,
            std::optional<std::uint32_t>{8U});
  EXPECT_EQ(request.intent,
            std::optional<ComputeIntent>{ComputeIntent::GlobalHighPrecision});
  EXPECT_EQ(request.dirty_roi, (PixelRect{768, 512, 256, 256}));
}

}  // namespace
}  // namespace ps::benchmark
