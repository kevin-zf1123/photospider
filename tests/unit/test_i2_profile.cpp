#include <fenv.h>  // NOLINT(build/c++11)
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/i2/i2_profile.hpp"
#include "compute/execution/i2_metal_acquisition_deadline.hpp"

namespace ps::benchmark {
namespace {

/**
 * @brief Restores the complete caller floating-point environment.
 * @throws std::runtime_error when capture or RNE installation fails.
 * @note Destruction restores rounding, sticky exceptions, and controls.
 */
class ScopedI2ReferenceRoundToNearest final {
 public:
  /** @brief Captures the caller environment and installs FE_TONEAREST. */
  ScopedI2ReferenceRoundToNearest() {
    if (fegetenv(&previous_) != 0 || fesetround(FE_TONEAREST) != 0) {
      throw std::runtime_error("I2 reference cannot install RNE.");
    }
  }

  /** @brief Restores the complete captured caller environment. */
  ~ScopedI2ReferenceRoundToNearest() noexcept {
    if (fesetenv(&previous_) != 0) {
      std::terminate();
    }
  }

  /** @brief Prevents duplicate floating-environment ownership. */
  ScopedI2ReferenceRoundToNearest(const ScopedI2ReferenceRoundToNearest&) =
      delete;

  /** @brief Prevents duplicate floating-environment assignment. */
  ScopedI2ReferenceRoundToNearest& operator=(
      const ScopedI2ReferenceRoundToNearest&) = delete;

 private:
  /** @brief Complete caller environment restored at destruction. */
  fenv_t previous_{};
};

/**
 * @brief Independently rounds one exact byte fraction to IEEE binary32.
 * @param numerator Unsigned numerator in `[0,255]`.
 * @return Round-to-nearest-ties-to-even encoding of `numerator/255`.
 * @throws Nothing.
 */
float i2_reference_byte_fraction(std::uint8_t numerator) noexcept {
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
 * @brief Applies one independent binary32 curve stage.
 * @param input Exact binary32 input.
 * @param coefficient Exact binary32 coefficient.
 * @return Three-cut binary32 `1/(1+input*coefficient)`.
 * @throws Nothing.
 */
float i2_reference_curve_stage(float input, float coefficient) noexcept {
  volatile float product = input * coefficient;
  volatile float denominator = 1.0F + product;
  volatile float result = 1.0F / denominator;
  return result;
}

/**
 * @brief Recomputes one preview digest from a parameterized independent oracle.
 * @param node_one Node-one edit coefficient.
 * @param source_x_offset Deliberate source-block offset for negative tests.
 * @param reverse_tail Whether stages 1.20 and 1.40 are deliberately swapped.
 * @param premature_rounding Whether accumulation incorrectly rounds in FP32.
 * @return Typed canonical digest of the constructed HWC preview Value.
 * @throws Value, allocation, digest, and floating-environment failures.
 * @note The helper does not call candidate box-average or operation providers.
 */
ContentDigest recompute_i2_preview_reference(float node_one,
                                             std::size_t source_x_offset,
                                             bool reverse_tail,
                                             bool premature_rounding) {
  ScopedI2ReferenceRoundToNearest rounding;
  constexpr std::size_t kChannels = 4U;
  constexpr std::size_t kElementBytes = sizeof(float);
  std::array<float, kI1FrozenCurveNodeCount> coefficients{node_one, 1.00F,
                                                          1.20F, 1.40F};
  if (reverse_tail) {
    std::swap(coefficients[2], coefficients[3]);
  }
  std::vector<std::byte> storage(kI2PreviewImageEdge * kI2PreviewImageEdge *
                                 kChannels * kElementBytes);
  std::size_t offset = 0U;
  for (std::size_t preview_y = 0U; preview_y < kI2PreviewImageEdge;
       ++preview_y) {
    for (std::size_t preview_x = 0U; preview_x < kI2PreviewImageEdge;
         ++preview_x) {
      for (std::size_t channel = 0U; channel < kChannels; ++channel) {
        double exact_sum = 0.0;
        volatile float rounded_sum = 0.0F;
        for (std::size_t block_y = 0U; block_y < 4U; ++block_y) {
          for (std::size_t block_x = 0U; block_x < 4U; ++block_x) {
            const std::size_t source_x =
                4U * preview_x + block_x + source_x_offset;
            const std::size_t source_y = 4U * preview_y + block_y;
            const std::uint8_t numerator = static_cast<std::uint8_t>(
                (17U * source_x + 31U * source_y + 47U * channel) & 255U);
            const float source = i2_reference_byte_fraction(numerator);
            exact_sum += static_cast<double>(source);
            rounded_sum = rounded_sum + source;
          }
        }
        float sample = premature_rounding
                           ? static_cast<float>(rounded_sum * 0.0625F)
                           : static_cast<float>(exact_sum * 0.0625);
        for (const float coefficient : coefficients) {
          sample = i2_reference_curve_stage(sample, coefficient);
        }
        std::memcpy(storage.data() + offset, &sample, sizeof(sample));
        offset += sizeof(sample);
      }
    }
  }
  DenseTensorDescriptor descriptor;
  descriptor.shape = {kI2PreviewImageEdge, kI2PreviewImageEdge, kChannels};
  descriptor.element_semantics = ElementSemantics::FloatingPoint;
  descriptor.storage_encoding = StorageEncoding{32U};
  const ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  const StridedLayout layout{
      {static_cast<std::ptrdiff_t>(kI2PreviewImageEdge * kChannels *
                                   kElementBytes),
       static_cast<std::ptrdiff_t>(kChannels * kElementBytes),
       static_cast<std::ptrdiff_t>(kElementBytes)},
      0U};
  const Value value = Value::from_cpu_dense_tensor(std::move(descriptor), image,
                                                   layout, std::move(storage));
  const ContentDigestResult result = compute_content_digest(value);
  if (result.state != ContentDigestState::Available ||
      !result.digest.has_value()) {
    throw std::runtime_error("Independent I2 preview digest unavailable: " +
                             result.diagnostic);
  }
  return *result.digest;
}

/**
 * @brief Encodes digest bytes for actionable golden mismatch diagnostics.
 * @param digest Typed digest to display.
 * @return Lowercase 64-character hexadecimal payload.
 * @throws std::bad_alloc when stream/string ownership allocates.
 */
std::string digest_hex(const ContentDigest& digest) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const std::byte byte : digest.bytes) {
    output << std::setw(2) << std::to_integer<unsigned int>(byte);
  }
  return output.str();
}

/**
 * @brief Captured scalar facts from one fake I2 Host admission.
 * @throws Nothing for construction after request strings exist.
 */
struct CapturedI2Admission final {
  /** @brief Preview child QoS. */
  compute::ComputeRunQos preview_qos;
  /** @brief Final child QoS. */
  compute::ComputeRunQos final_qos;
  /** @brief Ordinary request after collector normalization. */
  HostComputeRequest request;
  /** @brief Whether the shared observation sink was present. */
  bool has_observation_sink = false;
  /** @brief Exact pre-call accepted coordinate. */
  std::optional<compute::AcceptedBoundaryCoordinate> accepted_coordinate;
};

/**
 * @brief Deterministic I2 Host fake for accepted-boundary tests.
 * @throws std::bad_alloc when captured request/status storage allocates.
 * @note It emulates no Kernel, Run, device, resource, or currentness behavior.
 */
class RecordingI2Host final : public I2Host {
 public:
  /** @copydoc I2Host::compute_i2_async */
  Result<std::future<OperationStatus>> compute_i2_async(
      I2HostComputeRequest request) override {
    admissions.push_back(CapturedI2Admission{
        request.preview_qos, request.final_qos, request.request,
        request.observation_sink != nullptr, request.accepted_coordinate});
    Result<std::future<OperationStatus>> result;
    result.status = schedule_status;
    if (result.status.ok) {
      std::promise<OperationStatus> promise;
      result.value = promise.get_future();
      promise.set_value(settlement_status);
    }
    return result;
  }

  /**
   * @copydoc I2Host::acquire_i2_value
   * @note When both completion controls are populated, the fake advances the
   * borrowed harness clock immediately before returning otherwise complete
   * synthetic evidence. It transfers no Value or clock ownership.
   */
  I2ValueAcquisitionEvidence acquire_i2_value(
      Value value, const I2ValueLineage& lineage,
      std::chrono::steady_clock::time_point capture_deadline) override {
    static_cast<void>(lineage);
    last_capture_deadline = capture_deadline;
    ++acquisition_call_count;
    if (fail_acquisition) {
      throw std::runtime_error("synthetic I2 acquisition failure");
    }
    const StorageBinding binding = value.storage_binding();
    const I2ValueAccessEvidence access{std::nullopt,      value.revision_id(),
                                       binding,           binding.allocation,
                                       binding.byte_size, false};
    I2ValueAcquisitionEvidence evidence;
    evidence.host_first = access;
    evidence.host_second = access;
    if (acquisition_completion_clock != nullptr &&
        acquisition_completion_time.has_value()) {
      *acquisition_completion_clock = *acquisition_completion_time;
    }
    return evidence;
  }

  /** @copydoc I2Host::i2_execution_snapshot */
  I1ExecutionSnapshot i2_execution_snapshot(std::uint64_t after_cursor,
                                            std::size_t limit) const override {
    static_cast<void>(after_cursor);
    static_cast<void>(limit);
    return {};
  }

  /** @brief Scheduling status returned by the fake Host call. */
  OperationStatus schedule_status;
  /** @brief Later settlement status fulfilled through the future. */
  OperationStatus settlement_status;
  /** @brief Ordered captured Host admissions. */
  std::vector<CapturedI2Admission> admissions;
  /** @brief Exact explicit Value-acquisition invocation count. */
  std::size_t acquisition_call_count = 0U;
  /** @brief Last absolute capture deadline received by the fake Host. */
  std::optional<std::chrono::steady_clock::time_point> last_capture_deadline;
  /** @brief Whether the next and later Value acquisitions fail. */
  bool fail_acquisition = false;
  /** @brief Borrowed fake clock advanced immediately before a successful
   * return. */
  std::chrono::steady_clock::time_point* acquisition_completion_clock = nullptr;
  /** @brief Exact completion point assigned to the borrowed fake clock. */
  std::optional<std::chrono::steady_clock::time_point>
      acquisition_completion_time;
};

/**
 * @brief Builds one small Ready Host Value for collector freeze tests.
 * @return Valid rank-one FP32 Value with stable revision and binding facts.
 * @throws Value validation or allocation failures unchanged.
 */
Value make_i2_freeze_test_value() {
  std::vector<std::byte> storage(4U * sizeof(float));
  return Value::from_cpu_dense_tensor(
      DenseTensorDescriptor{{4U},
                            ElementSemantics::FloatingPoint,
                            StorageEncoding{32U}},
      std::nullopt,
      StridedLayout{{static_cast<std::ptrdiff_t>(sizeof(float))}, 0U},
      std::move(storage));
}

/**
 * @brief Builds one synthetic child submission for collector callbacks.
 * @param intent Child domain intent.
 * @param quality Child output quality.
 * @param deadline Exact child absolute deadline.
 * @param accepted Shared accepted request coordinate.
 * @return Complete product-style Run submission.
 * @throws ComputeRun and string allocation failures unchanged.
 */
compute::ComputeRunSubmission make_i2_test_submission(
    ComputeIntent intent, compute::ComputeRunQuality quality,
    std::chrono::steady_clock::time_point deadline,
    compute::AcceptedBoundaryCoordinate accepted) {
  compute::SupersessionIdentity supersession{
      compute::SupersessionKey(kI1TargetNodeId, ComputeIntent::RealTimeUpdate),
      compute::SupersessionGeneration(9U)};
  supersession.accepted_coordinate = accepted;
  return compute::ComputeRunSubmission{
      "i2-observer",
      GraphInstanceId{9001U},
      GraphRevision{9002U},
      kI1TargetNodeId,
      intent,
      quality,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive, deadline,
                             1U, 8U},
      supersession,
      nullptr};
}

/**
 * @brief Proves the continuous 111-slot grid and exact terminal guard.
 * @throws Checked-time failures are reported by GoogleTest.
 */
TEST(I2Profile, ContinuousGridAndTerminalGuardAreExact) {
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(1000000000));
  EXPECT_EQ(i2_episode_origin(origin, 0U), origin);
  EXPECT_EQ(i2_episode_origin(origin, 110U),
            checked_i1_time_add(origin, kI2EpisodeStride * 110));
  EXPECT_EQ(i2_terminal_boundary(origin),
            checked_i1_time_add(origin, kI2EpisodeStride * 111));
  EXPECT_EQ(classify_i2_slot(0U),
            std::make_pair(I2EpisodePhase::Cold, std::size_t{0U}));
  EXPECT_EQ(classify_i2_slot(10U),
            std::make_pair(I2EpisodePhase::Warmup, std::size_t{9U}));
  EXPECT_EQ(classify_i2_slot(11U),
            std::make_pair(I2EpisodePhase::Measured, std::size_t{0U}));
  EXPECT_EQ(classify_i2_slot(110U),
            std::make_pair(I2EpisodePhase::Measured, std::size_t{99U}));
  EXPECT_EQ(kI2LatestFinalDeadlineOffset + kI2TerminalGuard, kI2EpisodeStride);
  EXPECT_THROW(i2_episode_origin(origin, 111U), std::out_of_range);
  EXPECT_THROW(classify_i2_slot(111U), std::out_of_range);
}

/**
 * @brief Proves one pre-call sample anchors both child deadlines and identity.
 * @throws Fake storage and checked-time failures are reported by GoogleTest.
 */
TEST(I2AcceptedBoundaryCollector, OneSampleAnchorsBothChildren) {
  RecordingI2Host host;
  I2EpisodeObservationCollector observations;
  const auto origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(2000000000));
  const auto nominal = checked_i1_time_add(
      origin, kI1EditStride * static_cast<std::int64_t>(11));
  const auto admission = checked_i1_time_add(nominal, kI1AdmissionLateness);
  const auto returned =
      checked_i1_time_add(admission, std::chrono::nanoseconds(17));
  std::vector<std::chrono::steady_clock::time_point> samples{admission,
                                                             returned};
  std::size_t sample_index = 0U;
  std::vector<std::chrono::steady_clock::time_point> sleeps;
  I2AcceptedBoundaryCollector collector(
      host, [&] { return samples.at(sample_index++); },
      [&](auto target) { sleeps.push_back(target); }, 7U);
  I2EditAdmissionResult result = collector.admit_edit(
      origin, 11U, make_i2_host_compute_request(GraphSessionId{"i2-test"}, 11U),
      observations.make_edit_sink(11U));

  ASSERT_TRUE(result.accepted_coordinate.has_value());
  EXPECT_EQ(result.accepted_coordinate->admission_time(), admission);
  EXPECT_EQ(result.accepted_coordinate->event_sequence(), 7U);
  EXPECT_EQ(result.preview_deadline,
            checked_i1_time_add(admission, kI2PreviewDeadlineBudget));
  EXPECT_EQ(result.final_deadline,
            checked_i1_time_add(admission, kI2FinalDeadlineBudget));
  ASSERT_EQ(host.admissions.size(), 1U);
  EXPECT_EQ(host.admissions[0].accepted_coordinate, result.accepted_coordinate);
  EXPECT_EQ(host.admissions[0].preview_qos.deadline, result.preview_deadline);
  EXPECT_EQ(host.admissions[0].final_qos.deadline, result.final_deadline);
  EXPECT_EQ(host.admissions[0].request.intent, ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(host.admissions[0].request.dirty_roi, i1_edit_region(11U));
  EXPECT_EQ(i2_preview_region(11U), PixelRect({192, 128, 64, 64}));
  EXPECT_TRUE(result.settlement.valid());
  EXPECT_TRUE(result.settlement.get().ok);
  ASSERT_EQ(sleeps.size(), 1U);
  EXPECT_EQ(sleeps[0], nominal);
}

/**
 * @brief Proves the collector retains two distinct child descriptors and the
 * strict preview-visible/final-trigger observations without allocation in
 * callbacks.
 * @throws ComputeRun construction failures are reported by GoogleTest.
 */
TEST(I2EpisodeObservationCollector, RetainsChildAwareProgressiveOrder) {
  I2EpisodeObservationCollector collector;
  std::shared_ptr<compute::ComputeRunObservationSink> sink =
      collector.make_edit_sink(11U);
  const auto accepted_time = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(3000000000));
  const compute::AcceptedBoundaryCoordinate accepted(accepted_time, 5U);
  compute::ComputeRun preview(make_i2_test_submission(
      ComputeIntent::RealTimeUpdate, compute::ComputeRunQuality::Interactive,
      checked_i1_time_add(accepted_time, kI2PreviewDeadlineBudget), accepted));
  compute::ComputeRun final(make_i2_test_submission(
      ComputeIntent::GlobalHighPrecision, compute::ComputeRunQuality::Full,
      checked_i1_time_add(accepted_time, kI2FinalDeadlineBudget), accepted));
  compute::ComputeRunLease preview_lease = preview.acquire_lease();
  compute::ComputeRunLease final_lease = final.acquire_lease();

  auto coordinate = sink->reserve_causal_coordinate();
  sink->on_current_visible(preview_lease.descriptor(), Value{}, coordinate);
  const std::uint64_t preview_visible_sequence = coordinate.causal_sequence;
  coordinate = sink->reserve_causal_coordinate();
  sink->on_progressive_final_triggered(final_lease.descriptor(), coordinate);
  const std::uint64_t trigger_sequence = coordinate.causal_sequence;
  coordinate = sink->reserve_causal_coordinate();
  sink->on_service_start(
      final_lease.descriptor(), final_lease.task_identity(0U), 1U,
      compute::ComputeRunServiceStartObservation{true, true, true}, coordinate);

  const I2EpisodeObservationSnapshot snapshot = collector.snapshot();
  ASSERT_EQ(snapshot.visible_outputs.size(), 1U);
  ASSERT_EQ(snapshot.final_triggers.size(), 1U);
  ASSERT_EQ(snapshot.service_starts.size(), 1U);
  EXPECT_EQ(snapshot.visible_outputs[0].child.quality,
            compute::ComputeRunQuality::Interactive);
  EXPECT_EQ(snapshot.final_triggers[0].child.quality,
            compute::ComputeRunQuality::Full);
  EXPECT_EQ(snapshot.final_triggers[0].child.child_intent,
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(snapshot.final_triggers[0].child.request_intent,
            ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(snapshot.final_triggers[0].child.accepted_coordinate, accepted);
  EXPECT_NE(snapshot.visible_outputs[0].child.run_id,
            snapshot.final_triggers[0].child.run_id);
  EXPECT_LT(preview_visible_sequence, trigger_sequence);
  EXPECT_LT(trigger_sequence, snapshot.service_starts[0].causal_sequence);
  EXPECT_FALSE(snapshot.overflowed);
}

/**
 * @brief Proves successful visible capture is sticky across repeated cleanup.
 * @throws Value, digest, Host, or collector failures reach GoogleTest.
 * @note The sole Host acquisition must run once; payload release cannot erase
 * any captured identity, digest, or acquisition fact.
 */
TEST(I2EpisodeObservationCollector,
     SuccessfulFreezeRemainsStickyAcrossRepeatAndRelease) {
  I2EpisodeObservationCollector collector;
  RecordingI2Host host;
  std::shared_ptr<compute::ComputeRunObservationSink> sink =
      collector.make_edit_sink(11U);
  const auto accepted_time = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(4000000000));
  const compute::AcceptedBoundaryCoordinate accepted(accepted_time, 9U);
  compute::ComputeRun preview(make_i2_test_submission(
      ComputeIntent::RealTimeUpdate, compute::ComputeRunQuality::Interactive,
      checked_i1_time_add(accepted_time, kI2PreviewDeadlineBudget), accepted));
  compute::ComputeRunLease preview_lease = preview.acquire_lease();
  const compute::ComputeRunObservationCoordinate coordinate =
      sink->reserve_causal_coordinate();
  sink->on_current_visible(preview_lease.descriptor(),
                           make_i2_freeze_test_value(), coordinate);

  const auto capture_deadline = checked_i1_time_add(
      std::chrono::steady_clock::now(), std::chrono::seconds(1));
  ASSERT_EQ(collector.freeze_visible_outputs(host, capture_deadline), 1U);
  ASSERT_EQ(host.acquisition_call_count, 1U);
  EXPECT_EQ(host.last_capture_deadline, capture_deadline);
  const I2EpisodeObservationSnapshot frozen = collector.snapshot();
  ASSERT_EQ(frozen.visible_outputs.size(), 1U);
  const I2ObservedVisibleOutput& baseline = frozen.visible_outputs.front();
  ASSERT_TRUE(baseline.value_valid_at_capture);
  EXPECT_FALSE(baseline.output.valid());
  ASSERT_TRUE(baseline.value_revision.valid());
  ASSERT_TRUE(baseline.value_allocation.valid());
  ASSERT_TRUE(baseline.content_digest.has_value());
  ASSERT_TRUE(baseline.acquisition.has_value());

  ASSERT_EQ(collector.freeze_visible_outputs(host, capture_deadline), 1U);
  collector.release_unfrozen_visible_outputs();
  collector.release_unfrozen_visible_outputs();
  ASSERT_EQ(collector.freeze_visible_outputs(host, capture_deadline), 1U);
  EXPECT_EQ(host.acquisition_call_count, 1U);
  const I2EpisodeObservationSnapshot repeated = collector.snapshot();
  ASSERT_EQ(repeated.visible_outputs.size(), 1U);
  const I2ObservedVisibleOutput& retained = repeated.visible_outputs.front();
  EXPECT_TRUE(retained.value_valid_at_capture);
  EXPECT_FALSE(retained.output.valid());
  EXPECT_EQ(retained.value_revision, baseline.value_revision);
  EXPECT_EQ(retained.value_binding, baseline.value_binding);
  EXPECT_EQ(retained.value_allocation, baseline.value_allocation);
  EXPECT_EQ(retained.value_storage_bytes, baseline.value_storage_bytes);
  ASSERT_TRUE(retained.content_digest.has_value());
  EXPECT_EQ(retained.content_digest->state, baseline.content_digest->state);
  EXPECT_EQ(retained.content_digest->digest, baseline.content_digest->digest);
  EXPECT_EQ(retained.content_digest->diagnostic,
            baseline.content_digest->diagnostic);
  ASSERT_TRUE(retained.acquisition.has_value());
  EXPECT_EQ(retained.acquisition->host_first.revision,
            baseline.acquisition->host_first.revision);
  EXPECT_EQ(retained.acquisition->host_first.binding,
            baseline.acquisition->host_first.binding);
}

/**
 * @brief Proves partial and never-frozen payloads release as explicit Invalid.
 * @throws Value, digest, or synthetic Host failures reach GoogleTest.
 * @note Missing acquisition or digest stays missing after release, later
 * freeze calls perform no Host work, and every collector-owned Value is empty.
 */
TEST(I2EpisodeObservationCollector,
     ReleasePreservesExplicitIncompleteFreezeEvidence) {
  const auto accepted_time = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(5000000000));
  const compute::AcceptedBoundaryCoordinate accepted(accepted_time, 10U);
  compute::ComputeRun preview(make_i2_test_submission(
      ComputeIntent::RealTimeUpdate, compute::ComputeRunQuality::Interactive,
      checked_i1_time_add(accepted_time, kI2PreviewDeadlineBudget), accepted));
  compute::ComputeRunLease preview_lease = preview.acquire_lease();

  I2EpisodeObservationCollector partial_collector;
  std::shared_ptr<compute::ComputeRunObservationSink> partial_sink =
      partial_collector.make_edit_sink(10U);
  partial_sink->on_current_visible(preview_lease.descriptor(),
                                   make_i2_freeze_test_value(),
                                   partial_sink->reserve_causal_coordinate());
  RecordingI2Host failing_host;
  failing_host.fail_acquisition = true;
  const auto capture_deadline = checked_i1_time_add(
      std::chrono::steady_clock::now(), std::chrono::seconds(1));
  EXPECT_THROW(
      partial_collector.freeze_visible_outputs(failing_host, capture_deadline),
      std::runtime_error);
  EXPECT_EQ(failing_host.acquisition_call_count, 1U);
  partial_collector.release_unfrozen_visible_outputs();
  EXPECT_EQ(
      partial_collector.freeze_visible_outputs(failing_host, capture_deadline),
      1U);
  EXPECT_EQ(failing_host.acquisition_call_count, 1U);
  const I2EpisodeObservationSnapshot partial = partial_collector.snapshot();
  ASSERT_EQ(partial.visible_outputs.size(), 1U);
  EXPECT_TRUE(partial.visible_outputs.front().value_valid_at_capture);
  EXPECT_FALSE(partial.visible_outputs.front().output.valid());
  EXPECT_TRUE(partial.visible_outputs.front().content_digest.has_value());
  EXPECT_FALSE(partial.visible_outputs.front().acquisition.has_value());

  I2EpisodeObservationCollector unfrozen_collector;
  std::shared_ptr<compute::ComputeRunObservationSink> unfrozen_sink =
      unfrozen_collector.make_edit_sink(9U);
  unfrozen_sink->on_current_visible(preview_lease.descriptor(),
                                    make_i2_freeze_test_value(),
                                    unfrozen_sink->reserve_causal_coordinate());
  unfrozen_collector.release_unfrozen_visible_outputs();
  const I2EpisodeObservationSnapshot unfrozen = unfrozen_collector.snapshot();
  ASSERT_EQ(unfrozen.visible_outputs.size(), 1U);
  EXPECT_TRUE(unfrozen.visible_outputs.front().value_valid_at_capture);
  EXPECT_FALSE(unfrozen.visible_outputs.front().output.valid());
  EXPECT_FALSE(unfrozen.visible_outputs.front().content_digest.has_value());
  EXPECT_FALSE(unfrozen.visible_outputs.front().acquisition.has_value());
}

/**
 * @brief Proves the exclusive capture deadline rejects new payload work.
 * @throws Value and collector construction failures reach GoogleTest.
 * @note The Host must not be entered when the same absolute deadline is
 * already expired; the caller can then release the still-unfrozen Value.
 */
TEST(I2EpisodeObservationCollector,
     ExpiredCaptureDeadlineRejectsBeforeHostAcquisition) {
  I2EpisodeObservationCollector collector;
  RecordingI2Host host;
  std::shared_ptr<compute::ComputeRunObservationSink> sink =
      collector.make_edit_sink(11U);
  const auto accepted_time = std::chrono::steady_clock::now();
  const compute::AcceptedBoundaryCoordinate accepted(accepted_time, 11U);
  compute::ComputeRun preview(make_i2_test_submission(
      ComputeIntent::RealTimeUpdate, compute::ComputeRunQuality::Interactive,
      checked_i1_time_add(accepted_time, kI2PreviewDeadlineBudget), accepted));
  compute::ComputeRunLease preview_lease = preview.acquire_lease();
  sink->on_current_visible(preview_lease.descriptor(),
                           make_i2_freeze_test_value(),
                           sink->reserve_causal_coordinate());

  EXPECT_THROW(collector.freeze_visible_outputs(
                   host, std::chrono::steady_clock::time_point::min()),
               std::runtime_error);
  EXPECT_EQ(host.acquisition_call_count, 0U);
  EXPECT_FALSE(host.last_capture_deadline.has_value());
  collector.release_unfrozen_visible_outputs();
  const I2EpisodeObservationSnapshot snapshot = collector.snapshot();
  ASSERT_EQ(snapshot.visible_outputs.size(), 1U);
  EXPECT_FALSE(snapshot.visible_outputs.front().output.valid());
  EXPECT_FALSE(snapshot.visible_outputs.front().content_digest.has_value());
  EXPECT_FALSE(snapshot.visible_outputs.front().acquisition.has_value());
}

/**
 * @brief Proves a Host result that returns at D cannot freeze acquisition
 * facts.
 * @throws Value, digest, Host, or collector failures reach GoogleTest.
 * @note The injected collector clock begins at `D-1ns`; the fake Host advances
 * it exactly to `D` immediately before returning complete evidence. The late
 * Value remains Pending until explicit unfrozen release, with no timing sleep.
 */
TEST(I2EpisodeObservationCollector,
     HostReturnAtCaptureDeadlineDoesNotFreezeEvidenceOrValue) {
  const auto deadline = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(10000000000));
  auto clock_now = deadline - std::chrono::nanoseconds(1);
  std::size_t clock_samples = 0U;
  I2EpisodeObservationCollector collector([&] {
    ++clock_samples;
    return clock_now;
  });
  RecordingI2Host host;
  host.acquisition_completion_clock = &clock_now;
  host.acquisition_completion_time = deadline;
  std::shared_ptr<compute::ComputeRunObservationSink> sink =
      collector.make_edit_sink(11U);
  const compute::AcceptedBoundaryCoordinate accepted(
      std::chrono::steady_clock::time_point(
          std::chrono::nanoseconds(6000000000)),
      12U);
  compute::ComputeRun preview(make_i2_test_submission(
      ComputeIntent::RealTimeUpdate, compute::ComputeRunQuality::Interactive,
      checked_i1_time_add(accepted.admission_time(), kI2PreviewDeadlineBudget),
      accepted));
  compute::ComputeRunLease preview_lease = preview.acquire_lease();
  sink->on_current_visible(preview_lease.descriptor(),
                           make_i2_freeze_test_value(),
                           sink->reserve_causal_coordinate());

  EXPECT_THROW(collector.freeze_visible_outputs(host, deadline),
               std::runtime_error);
  EXPECT_EQ(host.acquisition_call_count, 1U);
  EXPECT_EQ(host.last_capture_deadline, deadline);
  EXPECT_EQ(clock_now, deadline);
  EXPECT_EQ(clock_samples, 3U);
  const I2EpisodeObservationSnapshot late = collector.snapshot();
  ASSERT_EQ(late.visible_outputs.size(), 1U);
  EXPECT_TRUE(late.visible_outputs.front().value_valid_at_capture);
  EXPECT_TRUE(late.visible_outputs.front().output.valid());
  EXPECT_TRUE(late.visible_outputs.front().content_digest.has_value());
  EXPECT_FALSE(late.visible_outputs.front().acquisition.has_value());

  collector.release_unfrozen_visible_outputs();
  EXPECT_EQ(collector.freeze_visible_outputs(host, deadline), 1U);
  EXPECT_EQ(host.acquisition_call_count, 1U);
  const I2EpisodeObservationSnapshot released = collector.snapshot();
  ASSERT_EQ(released.visible_outputs.size(), 1U);
  EXPECT_TRUE(released.visible_outputs.front().value_valid_at_capture);
  EXPECT_FALSE(released.visible_outputs.front().output.valid());
  EXPECT_TRUE(released.visible_outputs.front().content_digest.has_value());
  EXPECT_FALSE(released.visible_outputs.front().acquisition.has_value());
}

/**
 * @brief Proves capture timeout cannot alter the later fixed-grid schedule.
 * @throws Checked I2 grid arithmetic or fence allocation failures reach
 * GoogleTest.
 * @note The exclusive tie is rejected before fence observation; subsequent
 * origin and terminal calculations remain the original `G + slot * 1.5s`.
 */
TEST(I2Profile, CaptureTimeoutPreservesLaterOriginAndTerminalBoundary) {
  const auto grid_origin = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(123456789));
  constexpr std::size_t kTimedOutSlot = 17U;
  const auto episode_end = checked_i1_time_add(
      i2_episode_origin(grid_origin, kTimedOutSlot), kI2EpisodeStride);
  const auto capture_deadline =
      checked_i1_time_subtract(episode_end, std::chrono::milliseconds(100));
  const auto next_origin = i2_episode_origin(grid_origin, kTimedOutSlot + 1U);
  const auto terminal_boundary = i2_terminal_boundary(grid_origin);
  PendingReadyFence pending = make_pending_ready_fence();

  const auto terminal = compute::wait_for_i2_metal_completion_until(
      pending.fence, capture_deadline,
      [capture_deadline] { return capture_deadline; },
      [](std::chrono::steady_clock::time_point) {
        ADD_FAILURE() << "expired I2 Metal wait attempted to sleep";
      });

  EXPECT_FALSE(terminal.has_value());
  EXPECT_EQ(i2_episode_origin(grid_origin, kTimedOutSlot + 1U), next_origin);
  EXPECT_EQ(next_origin, episode_end);
  EXPECT_EQ(i2_terminal_boundary(grid_origin), terminal_boundary);
}

/**
 * @brief Proves the literal preview golden is independently recomputable and
 * rejects contract drift in coefficient, block, stage order, and rounding.
 * @throws Oracle allocation, Value, digest, and fenv failures are reported by
 * GoogleTest.
 * @note The final inherits the independently recomputed and real-product-
 * validated I1 final golden; this test also proves preview and final identities
 * cannot be substituted for one another.
 */
TEST(I2Profile, PreviewGoldenMatchesOnlyTheFrozenReferenceContract) {
  const ContentDigest expected = i2_frozen_preview_content_digest();
  const ContentDigest actual =
      recompute_i2_preview_reference(1.04F, 0U, false, false);
  EXPECT_EQ(actual, expected) << "actual=" << digest_hex(actual);
  EXPECT_FALSE(recompute_i2_preview_reference(0.96F, 0U, false, false) ==
               expected);
  EXPECT_FALSE(recompute_i2_preview_reference(1.04F, 1U, false, false) ==
               expected);
  EXPECT_FALSE(recompute_i2_preview_reference(1.04F, 0U, true, false) ==
               expected);
  EXPECT_FALSE(recompute_i2_preview_reference(1.04F, 0U, false, true) ==
               expected);
  EXPECT_FALSE(i1_frozen_final_content_digest() == expected);
}

}  // namespace
}  // namespace ps::benchmark
