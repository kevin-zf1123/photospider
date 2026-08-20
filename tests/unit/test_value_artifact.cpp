#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/pending_value.hpp"
#include "photospider/data/image_view.hpp"
#include "photospider/data/value_artifact.hpp"
#include "photospider/host/value_artifact_result.hpp"

namespace ps {
namespace {

/**
 * @brief Publishes one rich padded ordinary image used by artifact tests.
 * @return Ready Value with signed windows and complete channel/sample/color
 *         interpretation.
 * @throws Value validation and allocation failures unchanged.
 * @note Padding is retained as physical payload while logical content uses
 *       only addressed elements.
 */
Value make_rich_artifact_image() {
  DenseTensorDescriptor descriptor{{2U, 2U, 3U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{16U}};
  ImageFacet facet;
  facet.y_axis = 0U;
  facet.x_axis = 1U;
  facet.channel_axis = 2U;
  facet.data_window = ImageBounds{-7, 11, -5, 13};
  facet.display_window = ImageBounds{-20, -10, 30, 40};
  facet.channel_schema =
      ChannelSchema{{{ChannelId{1U}, "red"},
                     {ChannelId{2U}, "green"},
                     {ChannelId{3U}, "blue"}},
                    {{ChannelGroupId{9U},
                      "rgb",
                      {ChannelId{1U}, ChannelId{2U}, ChannelId{3U}}}}};
  facet.sample_domain =
      SampleDomainFacet{1U,
                        SampleEncoding{1U, SampleEncodingKind::CodeValue},
                        SampleDomain{SampleDomainKind::CodeValue, 0.0, 65535.0},
                        {}};
  facet.color =
      ColorFacet{1U, ChannelGroupId{9U}, ColorTransferFunction::SceneLinear,
                 ColorPrimaries::AcesAp1};
  const StridedLayout layout{{16, 6, 2}};
  std::vector<std::byte> storage(28U, std::byte{0xA5});
  const std::uint16_t samples[12] = {1U, 2U, 3U, 4U,  5U,  6U,
                                     7U, 8U, 9U, 10U, 11U, 12U};
  std::memcpy(storage.data(), samples, 6U * sizeof(std::uint16_t));
  std::memcpy(storage.data() + 16U, samples + 6U, 6U * sizeof(std::uint16_t));
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::move(facet),
                                      layout, std::move(storage));
}

/**
 * @brief Publishes one padded, aligned built-in FP4 Blocked artifact fixture.
 * @return Ready Blocked DenseTensor with nonzero bit offset and exact 4096-byte
 *         reconstruction alignment.
 * @throws Value validation and allocation failures unchanged.
 * @note Sentinel nibbles outside the addressed elements make full-payload
 *       preservation independently observable after recapture.
 */
Value make_blocked_artifact_value() {
  DenseTensorDescriptor descriptor{
      {2U, 8U},
      ElementSemantics::FloatingPoint,
      StorageEncoding{4U, StorageEncodingKind::Fp4E2M1},
      QuantizationSchema{{1U, 4U}, {1.0F, 2.0F, 3.0F, 4.0F}}};
  BlockedLayout layout{1U,
                       {1U, 4U},
                       {32U, 16U},
                       4U,
                       PackedBitOrder::MostSignificantFirst};
  const std::vector<std::byte> storage{
      std::byte{0xa0}, std::byte{0x12}, std::byte{0x34},
      std::byte{0x56}, std::byte{0x78}, std::byte{0x9a},
      std::byte{0xbc}, std::byte{0xde}, std::byte{0xfa}};
  return Value::from_cpu_blocked_dense_tensor(std::move(descriptor),
                                              std::move(layout), storage,
                                              kMaximumValueArtifactAlignment);
}

TEST(ValueArtifact, RichDenseImageArchiveRoundTripsExactPortableFacts) {
  const Value original = make_rich_artifact_image();
  ValueArtifact artifact = capture_value_artifact("image", original);
  artifact.envelope.joins.artifact_identity = "artifact-17";
  artifact.envelope.joins.commit_identity = "commit-4";
  artifact.envelope.joins.slot_identity = "image";
  ASSERT_TRUE(artifact.envelope.content_digest.has_value());
  artifact.envelope.statistics_references.push_back(
      ValueArtifactStatisticsReference{
          *artifact.envelope.content_digest,
          compute_artifact_payload_digest(
              std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}}),
          "histogram", 3U, "statistics-88"});

  const std::vector<std::byte> encoded =
      encode_named_value_artifact_set(NamedValueArtifactSet{{artifact}});
  const NamedValueArtifactSet decoded =
      decode_named_value_artifact_set(encoded);
  ASSERT_EQ(decoded.values.size(), 1U);
  const ValueArtifactEnvelope& envelope = decoded.values[0].envelope;
  EXPECT_EQ(envelope.structural_version, kValueArtifactEnvelopeVersion);
  EXPECT_EQ(envelope.output_name, "image");
  EXPECT_EQ(envelope.dense_descriptor, std::optional<DenseTensorDescriptor>(
                                           original.dense_tensor_descriptor()));
  EXPECT_EQ(envelope.image_facet, original.image_facet());
  EXPECT_EQ(envelope.strided_layout,
            std::optional<StridedLayout>(original.strided_layout()));
  EXPECT_EQ(envelope.joins.artifact_identity,
            std::optional<std::string>("artifact-17"));
  ASSERT_EQ(envelope.statistics_references.size(), 1U);
  EXPECT_EQ(envelope.statistics_references[0].algorithm, "histogram");
  EXPECT_EQ(envelope.statistics_references[0].algorithm_version, 3U);
  EXPECT_EQ(envelope.statistics_references[0].artifact_identity,
            "statistics-88");

  const NamedValueResult reconstructed_result =
      reconstruct_named_value_artifact_set(decoded);
  const std::vector<NamedValueInspection> inspection =
      reconstructed_result.inspect();
  ASSERT_EQ(inspection.size(), 1U);
  EXPECT_TRUE(inspection[0].revision.valid());
  EXPECT_TRUE(inspection[0].producer.valid());
  EXPECT_EQ(inspection[0].descriptor_digest,
            std::optional<DescriptorDigest>(envelope.descriptor_digest));
  EXPECT_EQ(inspection[0].content_digest, envelope.content_digest);
  EXPECT_EQ(inspection[0].statistics_references.size(), 1U);

  const Value reconstructed = reconstruct_value_artifact(decoded.values[0]);
  EXPECT_NE(reconstructed.revision_id(), original.revision_id());
  EXPECT_NE(reconstructed.storage_binding().allocation,
            original.storage_binding().allocation);
  EXPECT_EQ(reconstructed.dense_tensor_descriptor(),
            original.dense_tensor_descriptor());
  EXPECT_EQ(reconstructed.image_facet(), original.image_facet());
  EXPECT_EQ(reconstructed.strided_layout(), original.strided_layout());
  const ContentDigestResult reconstructed_digest =
      compute_content_digest(reconstructed);
  const ContentDigestResult original_digest = compute_content_digest(original);
  EXPECT_EQ(reconstructed_digest.state, ContentDigestState::Available);
  EXPECT_EQ(reconstructed_digest.digest, original_digest.digest);
  const ImageView view(reconstructed);
  std::uint16_t last = 0U;
  std::memcpy(&last, view.channel_data_at(-6, 12, 2U), sizeof(last));
  EXPECT_EQ(last, 12U);
}

TEST(ValueArtifact, NamedResultRoundTripIsTransactionalAndCanonical) {
  const Value first = make_rich_artifact_image();
  const Value second = make_rich_artifact_image();
  const NamedValueResult result({{"image", first}, {"preview", second}});

  const NamedValueArtifactSet artifacts =
      capture_named_value_artifact_set(result);
  const NamedValueResult reconstructed =
      reconstruct_named_value_artifact_set(artifacts);

  ASSERT_EQ(reconstructed.values().size(), 2U);
  ASSERT_NE(reconstructed.find("image"), nullptr);
  ASSERT_NE(reconstructed.find("preview"), nullptr);
  EXPECT_EQ(compute_content_digest(*reconstructed.find("image")).digest,
            compute_content_digest(first).digest);
  EXPECT_EQ(compute_content_digest(*reconstructed.find("preview")).digest,
            compute_content_digest(second).digest);
}

TEST(ValueArtifact, RejectsVersionPayloadDigestAndTrailingByteTampering) {
  const Value value = make_rich_artifact_image();
  ValueArtifact artifact = capture_value_artifact("image", value);

  ValueArtifact wrong_version = artifact;
  wrong_version.envelope.structural_version += 1U;
  EXPECT_THROW(validate_value_artifact(wrong_version), std::invalid_argument);

  ValueArtifact wrong_payload = artifact;
  wrong_payload.payloads[0][0] ^= std::byte{0x01};
  EXPECT_THROW(validate_value_artifact(wrong_payload), std::invalid_argument);

  std::vector<std::byte> archive =
      encode_named_value_artifact_set(NamedValueArtifactSet{{artifact}});
  archive.push_back(std::byte{0x00});
  EXPECT_THROW((void)decode_named_value_artifact_set(archive),
               std::invalid_argument);
}

TEST(ValueArtifact,
     RejectsInvalidAlignmentAndNonzeroArchivePaddingBeforePublication) {
  const Value value = make_rich_artifact_image();
  const ValueArtifact artifact = capture_value_artifact("image", value);

  const std::array<std::uint64_t, 3U> invalid_alignments{
      0U, 3U, kMaximumValueArtifactAlignment * 2U};
  for (const std::uint64_t invalid_alignment : invalid_alignments) {
    ValueArtifact malformed = artifact;
    malformed.envelope.buffers[0].required_alignment = invalid_alignment;
    EXPECT_THROW(validate_value_artifact(malformed), std::invalid_argument);
  }
  ValueArtifact misaligned = artifact;
  misaligned.envelope.buffers[0].required_alignment = 64U;
  misaligned.envelope.buffers[0].artifact_offset = 1U;
  EXPECT_THROW(validate_value_artifact(misaligned), std::invalid_argument);

  const std::array<std::uint64_t, 6U> valid_alignments{
      1U, 2U, 4U, 8U, 64U, kMaximumValueArtifactAlignment};
  for (const std::uint64_t valid_alignment : valid_alignments) {
    ValueArtifact allowed = artifact;
    allowed.envelope.buffers[0].required_alignment = valid_alignment;
    const Value rebuilt = reconstruct_value_artifact(allowed);
    EXPECT_EQ(rebuilt.storage_binding().required_alignment, valid_alignment);
    const ValueArtifact recaptured = capture_value_artifact("image", rebuilt);
    EXPECT_EQ(recaptured.envelope.buffers[0].required_alignment,
              valid_alignment);
  }

  ValueArtifact aligned = artifact;
  aligned.envelope.buffers[0].required_alignment =
      kMaximumValueArtifactAlignment;
  std::vector<std::byte> archive =
      encode_named_value_artifact_set(NamedValueArtifactSet{{aligned}});
  const NamedValueArtifactSet decoded =
      decode_named_value_artifact_set(archive);
  ASSERT_EQ(decoded.values.size(), 1U);
  ASSERT_EQ(decoded.values[0].envelope.buffers.size(), 1U);
  const ValueArtifactBuffer& decoded_buffer =
      decoded.values[0].envelope.buffers[0];
  EXPECT_EQ(decoded_buffer.required_alignment, kMaximumValueArtifactAlignment);
  EXPECT_EQ(decoded_buffer.artifact_offset % decoded_buffer.required_alignment,
            0U);

  const Value reconstructed = reconstruct_value_artifact(decoded.values[0]);
  const ReadLease read = reconstructed.buffer_handle().acquire_read();
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(read.data()) %
                kMaximumValueArtifactAlignment,
            0U);
  EXPECT_EQ(reconstructed.storage_binding().required_alignment,
            kMaximumValueArtifactAlignment);

  const std::size_t payload_offset =
      static_cast<std::size_t>(decoded_buffer.artifact_offset);
  ASSERT_GT(payload_offset, 0U);
  ASSERT_EQ(archive[payload_offset - 1U], std::byte{0U});
  archive[payload_offset - 1U] = std::byte{0x01};
  EXPECT_THROW((void)decode_named_value_artifact_set(archive),
               std::invalid_argument);
}

TEST(ValueArtifact,
     BlockedArchiveRoundTripPreservesPaddingAlignmentAndFreshIdentity) {
  const Value original = make_blocked_artifact_value();
  const ValueArtifact captured = capture_value_artifact("packed", original);
  ASSERT_EQ(captured.envelope.buffers.size(), 1U);
  ASSERT_EQ(captured.payloads.size(), 1U);
  EXPECT_EQ(captured.envelope.layout_kind, StorageLayoutKind::Blocked);
  EXPECT_EQ(captured.envelope.buffers[0].required_alignment,
            kMaximumValueArtifactAlignment);

  const std::vector<std::byte> local_envelope =
      encode_value_artifact_envelope(captured.envelope);
  const std::size_t metadata_end = 8U + 4U + 4U + 4U + local_envelope.size();
  const std::vector<std::byte> archive =
      encode_named_value_artifact_set(NamedValueArtifactSet{{captured}});
  const NamedValueArtifactSet decoded =
      decode_named_value_artifact_set(archive);
  ASSERT_EQ(decoded.values.size(), 1U);
  const ValueArtifact& artifact = decoded.values[0];
  ASSERT_EQ(artifact.envelope.buffers.size(), 1U);
  const std::size_t payload_offset =
      static_cast<std::size_t>(artifact.envelope.buffers[0].artifact_offset);
  EXPECT_GT(payload_offset, metadata_end);
  EXPECT_EQ(payload_offset % kMaximumValueArtifactAlignment, 0U);
  EXPECT_TRUE(
      std::all_of(archive.begin() + static_cast<std::ptrdiff_t>(metadata_end),
                  archive.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                  [](std::byte value) { return value == std::byte{0U}; }));

  EXPECT_EQ(
      artifact.envelope.dense_descriptor,
      std::optional<DenseTensorDescriptor>(original.dense_tensor_descriptor()));
  EXPECT_EQ(artifact.envelope.blocked_layout,
            std::optional<BlockedLayout>(original.blocked_layout()));
  EXPECT_EQ(artifact.payloads, captured.payloads);
  EXPECT_EQ(artifact.envelope.content_digest, captured.envelope.content_digest);
  EXPECT_EQ(artifact.envelope.descriptor_digest,
            captured.envelope.descriptor_digest);
  EXPECT_EQ(artifact.envelope.storage_layout_digest,
            captured.envelope.storage_layout_digest);

  const Value reconstructed = reconstruct_value_artifact(artifact);
  EXPECT_NE(reconstructed.allocation_identity(),
            original.allocation_identity());
  EXPECT_NE(reconstructed.revision_id(), original.revision_id());
  EXPECT_EQ(reconstructed.storage_binding().required_alignment,
            kMaximumValueArtifactAlignment);
  const ReadLease read = reconstructed.buffer_handle().acquire_read();
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(read.data()) %
                kMaximumValueArtifactAlignment,
            0U);

  const ValueArtifact recaptured =
      capture_value_artifact("packed", reconstructed);
  EXPECT_EQ(recaptured.envelope.dense_descriptor,
            captured.envelope.dense_descriptor);
  EXPECT_EQ(recaptured.envelope.blocked_layout,
            captured.envelope.blocked_layout);
  EXPECT_EQ(recaptured.envelope.content_digest,
            captured.envelope.content_digest);
  EXPECT_EQ(recaptured.envelope.descriptor_digest,
            captured.envelope.descriptor_digest);
  EXPECT_EQ(recaptured.envelope.storage_layout_digest,
            captured.envelope.storage_layout_digest);
  EXPECT_EQ(recaptured.envelope.buffers[0].required_alignment,
            kMaximumValueArtifactAlignment);
  EXPECT_EQ(recaptured.envelope.buffers[0].digest,
            captured.envelope.buffers[0].digest);
  EXPECT_EQ(recaptured.payloads, captured.payloads);
}

TEST(ValueArtifact, RejectsNoncanonicalNamesAndPartialNamedSets) {
  const Value value = make_rich_artifact_image();
  const ValueArtifact image = capture_value_artifact("image", value);
  const ValueArtifact duplicate = capture_value_artifact("image", value);
  EXPECT_THROW((void)encode_named_value_artifact_set(
                   NamedValueArtifactSet{{image, duplicate}}),
               std::invalid_argument);

  ValueArtifact malformed = capture_value_artifact("preview", value);
  malformed.payloads[0].pop_back();
  EXPECT_THROW((void)reconstruct_named_value_artifact_set(
                   NamedValueArtifactSet{{image, malformed}}),
               std::invalid_argument);
}

TEST(NamedValueInspection, PendingMetadataNeverGrantsPayloadAccess) {
  DenseTensorDescriptor descriptor{{1U, 2U, 1U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet facet;
  facet.y_axis = 0U;
  facet.x_axis = 1U;
  facet.channel_axis = 2U;
  facet.data_window = ImageBounds{-4, 9, -2, 10};
  facet.display_window = ImageBounds{-8, -7, 12, 13};
  PendingValuePublication pending =
      PendingValuePublisher::allocate_cpu_dense_tensor(
          descriptor, facet, StridedLayout{{2, 1, 1}}, 2U);
  const NamedValueResult result({NamedValue{"image", pending.value}}, false);

  const std::vector<NamedValueInspection> inspection = result.inspect();

  ASSERT_EQ(inspection.size(), 1U);
  EXPECT_EQ(inspection[0].readiness, ReadyFenceState::Pending);
  EXPECT_EQ(inspection[0].dense_descriptor,
            std::optional<DenseTensorDescriptor>(descriptor));
  EXPECT_EQ(inspection[0].image_facet, std::optional<ImageFacet>(facet));
  EXPECT_TRUE(inspection[0].revision.valid());
  EXPECT_TRUE(inspection[0].producer.valid());
  ASSERT_EQ(inspection[0].buffers.size(), 1U);
  EXPECT_EQ(inspection[0].buffers[0].byte_size, 2U);
  EXPECT_THROW((void)ImageView(pending.value), ReadyFenceAccessError);
}

}  // namespace
}  // namespace ps
