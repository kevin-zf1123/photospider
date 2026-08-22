/**
 * @file test_isolated_cpu_invocation_protocol.cpp
 * @brief Verifies the pointer-free isolated CPU invocation wire contract.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "execution/isolation/isolated_cpu_invocation_protocol.hpp"  // NOLINT(build/include_subdir)
#include "support/isolated_cpu_conformance_fixture.hpp"

namespace ps::execution {
namespace {

/**
 * @brief Creates one deterministic nonzero opaque protocol identity.
 * @param seed First nonzero byte in the deterministic sequence.
 * @return Complete 128-bit comparison key.
 * @throws Nothing.
 */
IsolatedCpuOpaqueId test_opaque_id(std::uint8_t seed) noexcept {
  IsolatedCpuOpaqueId id;
  for (std::size_t index = 0U; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::byte>(seed + index);
  }
  return id;
}

/**
 * @brief Creates four deterministic nonzero operation-descriptor digest words.
 * @param seed First word in the deterministic sequence.
 * @return Exact opaque digest suitable for round-trip comparisons.
 * @throws Nothing.
 */
IsolatedCpuSha256Digest test_operation_digest(std::uint64_t seed) noexcept {
  IsolatedCpuSha256Digest digest;
  for (std::size_t index = 0U; index < digest.words.size(); ++index) {
    digest.words[index] = seed + index;
  }
  return digest;
}

/**
 * @brief Creates one complete deterministic identity tuple.
 * @return Valid comparison-only identity with nonzero generations.
 * @throws Nothing.
 */
IsolatedCpuInvocationIdentity test_identity() noexcept {
  IsolatedCpuInvocationIdentity identity;
  identity.tenant_id = test_opaque_id(1U);
  identity.job_id = test_opaque_id(17U);
  identity.attempt_id = test_opaque_id(33U);
  identity.worker_id = test_opaque_id(49U);
  identity.worker_lease_generation = 7U;
  identity.plugin_package_id = test_opaque_id(65U);
  identity.plugin_generation = 11U;
  identity.invocation_id = test_opaque_id(81U);
  return identity;
}

/**
 * @brief Creates one structurally complete two-by-three u8 tensor descriptor.
 * @param capability_id Invocation-local capability selector.
 * @param access Input or output direction.
 * @return Descriptor with exact six-byte positive layout.
 * @throws std::bad_alloc when vector construction fails.
 */
IsolatedCpuTensorDescriptor test_tensor(std::uint64_t capability_id,
                                        IsolatedCpuTensorAccess access) {
  IsolatedCpuTensorDescriptor tensor;
  tensor.access = access;
  tensor.port_identity = test_opaque_id(
      access == IsolatedCpuTensorAccess::InputReadOnly ? 97U : 113U);
  tensor.binding_identity = test_opaque_id(
      access == IsolatedCpuTensorAccess::InputReadOnly ? 98U : 114U);
  tensor.schema_identity = test_opaque_id(99U);
  tensor.layout_identity = test_opaque_id(100U);
  tensor.schema_version = 7U;
  tensor.layout_version = 11U;
  tensor.descriptor_digest = test_operation_digest(0x101U);
  tensor.logical_content_digest = test_operation_digest(0x201U);
  tensor.layout_digest = test_operation_digest(0x301U);
  tensor.capability_id = capability_id;
  tensor.capability_length = 6U;
  tensor.element_semantics = IsolatedCpuElementSemantics::UnsignedInteger;
  tensor.bit_width = 8U;
  tensor.extents = {2U, 3U};
  tensor.byte_strides = {3, 1};
  if (access == IsolatedCpuTensorAccess::InputReadOnly) {
    tensor.readiness = IsolatedCpuTensorReadiness::ReadyInput;
    tensor.ownership = IsolatedCpuTensorOwnership::HostInput;
  } else {
    tensor.readiness = IsolatedCpuTensorReadiness::WritableOutput;
    tensor.ownership = IsolatedCpuTensorOwnership::RuntimeOutput;
    tensor.allocation_alignment = 1U;
  }
  return tensor;
}

/**
 * @brief Creates one complete valid request and binds deterministic input
 * bytes.
 * @return One-input/one-output request within default limits.
 * @throws Protocol, digest, or allocation errors unchanged.
 */
IsolatedCpuInvocationRequest test_request() {
  IsolatedCpuInvocationRequest request;
  request.identity = test_identity();
  request.operation = "test.copy";
  request.operation_identity = test_opaque_id(121U);
  request.implementation_identity = test_opaque_id(137U);
  request.configuration_schema_identity = test_opaque_id(153U);
  IsolatedCpuScalarParameter enabled;
  enabled.name = "alpha";
  enabled.kind = IsolatedCpuScalarKind::Boolean;
  enabled.boolean_value = true;
  IsolatedCpuScalarParameter scale;
  scale.name = "bravo";
  scale.kind = IsolatedCpuScalarKind::UnsignedInteger;
  scale.unsigned_value = 2U;
  request.parameters = {enabled, scale};
  request.capabilities = {
      IsolatedCpuCapability{1U, IsolatedCpuCapabilityAccess::ReadOnly, 6U},
      IsolatedCpuCapability{2U, IsolatedCpuCapabilityAccess::ReadWrite, 6U}};
  request.tensors = {test_tensor(1U, IsolatedCpuTensorAccess::InputReadOnly),
                     test_tensor(2U, IsolatedCpuTensorAccess::OutputWriteOnly)};
  const std::array<std::byte, 6U> input{std::byte{1}, std::byte{2},
                                        std::byte{3}, std::byte{4},
                                        std::byte{5}, std::byte{6}};
  request.tensors[0].content_binding = compute_isolated_cpu_content_binding(
      request.identity, request.tensors[0], input.data(), input.size());
  request.input_count = 1U;
  request.output_count = 1U;
  request.resources.shared_memory_bytes = 12U;
  request.resources.descriptor_count = 2U;
  request.resources.cpu_slots = 1U;
  validate_isolated_cpu_invocation_request(request, {});
  return request;
}

/**
 * @brief Encodes two operation-ABI identity words into protocol byte order.
 * @param word0 First opaque 64-bit identity word.
 * @param word1 Second opaque 64-bit identity word.
 * @return Exact network-order protocol identity.
 * @throws Nothing.
 */
IsolatedCpuOpaqueId opaque_id_from_words(std::uint64_t word0,
                                         std::uint64_t word1) noexcept {
  IsolatedCpuOpaqueId result;
  const std::array<std::uint64_t, 2U> words{word0, word1};
  for (std::size_t word = 0U; word < words.size(); ++word) {
    for (std::size_t byte = 0U; byte < 8U; ++byte) {
      result.bytes[word * 8U + byte] =
          static_cast<std::byte>((words[word] >> ((7U - byte) * 8U)) & 0xffU);
    }
  }
  return result;
}

/**
 * @brief Creates one exact conformance callback-local tensor.
 * @param facet_free True for the two-by-three facet-free representation;
 * false for the two-by-two single-channel Image representation.
 * @param access Callback direction and authority state to encode.
 * @param fixture_metadata True for the fixture's non-default versions and
 * digests; false for the ordinary built-in fallback metadata.
 * @return Complete synthetic mapped tensor suitable for pure fixture checks.
 * @throws Region validation or allocation failures unchanged.
 * @note Static backing bytes outlive every returned borrowed pointer. They are
 * never inspected or mutated by the pure conformance validator.
 */
IsolatedCpuRuntimeTensor conformance_runtime_tensor(
    bool facet_free, IsolatedCpuTensorAccess access,
    bool fixture_metadata = true) {
  static const std::array<std::byte, 6U> kInputBytes{};
  static std::array<std::byte, 6U> output_bytes{};
  IsolatedCpuRuntimeTensor tensor;
  IsolatedCpuTensorDescriptor& descriptor = tensor.descriptor;
  descriptor.access = access;
  descriptor.readiness = access == IsolatedCpuTensorAccess::InputReadOnly
                             ? IsolatedCpuTensorReadiness::ReadyInput
                             : IsolatedCpuTensorReadiness::WritableOutput;
  descriptor.ownership = access == IsolatedCpuTensorAccess::InputReadOnly
                             ? IsolatedCpuTensorOwnership::HostInput
                             : IsolatedCpuTensorOwnership::RuntimeOutput;
  descriptor.port_identity =
      access == IsolatedCpuTensorAccess::InputReadOnly
          ? opaque_id_from_words(0x5053434F4E46494EULL, 0x0001ULL)
          : opaque_id_from_words(0x5053434F4E464F55ULL, 0x0001ULL);
  descriptor.binding_identity = test_opaque_id(
      access == IsolatedCpuTensorAccess::InputReadOnly ? 201U : 217U);
  descriptor.schema_identity = opaque_id_from_words(
      PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_IDENTITY_WORD0_V1,
      PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_IDENTITY_WORD1_V1);
  descriptor.facet_identity =
      facet_free ? IsolatedCpuOpaqueId{}
                 : opaque_id_from_words(
                       PS_OPERATION_BUILTIN_IMAGE_FACET_IDENTITY_WORD0_V1,
                       PS_OPERATION_BUILTIN_IMAGE_FACET_IDENTITY_WORD1_V1);
  descriptor.layout_identity = opaque_id_from_words(
      PS_OPERATION_BUILTIN_STRIDED_LAYOUT_IDENTITY_WORD0_V1,
      PS_OPERATION_BUILTIN_STRIDED_LAYOUT_IDENTITY_WORD1_V1);
  if (fixture_metadata) {
    descriptor.schema_version = 7U;
    descriptor.layout_version = 11U;
    descriptor.descriptor_digest.words = {0x0102030405060708ULL, 0U, 0U,
                                          0x1112131415161718ULL};
    descriptor.logical_content_digest.words = {0U, 0x2122232425262728ULL, 0U,
                                               0U};
    descriptor.layout_digest.words = {0U, 0U, 0x3132333435363738ULL, 0U};
  } else {
    descriptor.schema_version =
        PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_VERSION_V1;
    descriptor.layout_version = PS_OPERATION_BUILTIN_STRIDED_LAYOUT_VERSION_V1;
  }
  descriptor.capability_id =
      access == IsolatedCpuTensorAccess::InputReadOnly ? 1U : 2U;
  descriptor.capability_offset = 40U;
  descriptor.capability_length = facet_free ? 6U : 4U;
  descriptor.element_semantics = IsolatedCpuElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = IsolatedCpuStorageEncoding::NativeScalar;
  descriptor.bit_width = 8U;
  descriptor.extents = facet_free ? std::vector<std::uint64_t>{2U, 3U}
                                  : std::vector<std::uint64_t>{2U, 2U, 1U};
  descriptor.byte_strides = facet_free ? std::vector<std::int64_t>{3, 1}
                                       : std::vector<std::int64_t>{2, 1, 1};
  if (facet_free) {
    descriptor.region = RegionSet::from_tensor_slice(
        TensorSlice{dense_tensor_region_domain(), {{0U, 2U}, {0U, 3U}}});
  } else {
    IsolatedCpuImageFacet facet;
    facet.x_axis = 1U;
    facet.y_axis = 0U;
    facet.channel_axis = 2U;
    facet.data_window = ImageBounds{0, 0, 2, 2};
    descriptor.image_facet = facet;
    descriptor.region = RegionSet::from_image_rect(
        ImageRect{image_region_domain(), 0, 2, 0, 2});
  }
  if (access == IsolatedCpuTensorAccess::InputReadOnly) {
    descriptor.content_binding = ContentDigest{};
    tensor.input_data = kInputBytes.data();
  } else {
    descriptor.allocation_alignment = 64U;
    tensor.output_data = output_bytes.data();
  }
  tensor.size = static_cast<std::size_t>(descriptor.capability_length);
  return tensor;
}

/**
 * @brief Creates one exact one-input/one-output conformance invocation.
 * @param facet_free_input Whether the input is a generic DenseTensor.
 * @param facet_free_output Whether the output plan is a generic DenseTensor.
 * @param fixture_input_metadata Whether the input retains prior fixture
 * publisher metadata instead of ordinary built-in fallback metadata.
 * @return Complete callback-local synthetic invocation.
 * @throws Region validation or allocation failures unchanged.
 */
IsolatedCpuRuntimeInvocation conformance_runtime_invocation(
    bool facet_free_input, bool facet_free_output,
    bool fixture_input_metadata = false) {
  IsolatedCpuRuntimeInvocation invocation;
  invocation.operation = "operation_conformance:supervised_tile";
  invocation.inputs.push_back(conformance_runtime_tensor(
      facet_free_input, IsolatedCpuTensorAccess::InputReadOnly,
      fixture_input_metadata));
  invocation.outputs.push_back(conformance_runtime_tensor(
      facet_free_output, IsolatedCpuTensorAccess::OutputWriteOnly));
  return invocation;
}

/**
 * @brief Creates a negative-origin rich-image protocol-v2 request.
 * @return One input/output request carrying complete channel/sample/color,
 * padded layout, Region, identity, and immutable-plan facts.
 * @throws Protocol, digest, Region, or allocation errors unchanged.
 */
IsolatedCpuInvocationRequest rich_image_request() {
  IsolatedCpuInvocationRequest request = test_request();
  IsolatedCpuImageFacet facet;
  facet.x_axis = 1U;
  facet.y_axis = 0U;
  facet.channel_axis = 2U;
  facet.data_window = ImageBounds{-3, -2, 0, 0};
  facet.display_window = ImageBounds{-5, -4, 5, 4};
  ChannelSchema schema;
  schema.channels = {
      ChannelDescription{ChannelId{10U}, "red"},
      ChannelDescription{ChannelId{20U}, "green"},
      ChannelDescription{ChannelId{30U}, "blue"},
      ChannelDescription{ChannelId{40U}, "alpha"},
  };
  schema.groups = {ChannelGroupDescription{
      ChannelGroupId{100U}, "display color",
      std::vector<ChannelId>{ChannelId{10U}, ChannelId{20U}, ChannelId{30U}}}};
  facet.channel_schema = std::move(schema);
  SampleDomainFacet sample;
  sample.encoding.kind = SampleEncodingKind::Normalized;
  sample.default_domain = SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0};
  sample.per_channel = {ChannelSampleDomain{
      ChannelId{40U}, SampleDomain{SampleDomainKind::Legal, 0.1, 0.9}}};
  facet.sample_domain = std::move(sample);
  facet.color = ColorFacet{1U, ChannelGroupId{100U},
                           ColorTransferFunction::Srgb, ColorPrimaries::Rec709};
  const RegionSet full_region = RegionSet::from_image_rect(
      ImageRect{image_region_domain(), -3, 0, -2, 0});
  for (IsolatedCpuTensorDescriptor& tensor : request.tensors) {
    tensor.extents = {2U, 3U, 4U};
    tensor.byte_strides = {16, 4, 1};
    tensor.capability_length = 28U;
    tensor.image_facet = facet;
    tensor.facet_identity = test_opaque_id(169U);
    tensor.region = full_region;
  }
  request.tensors[1].allocation_alignment = 16U;
  request.capabilities[0].byte_size = 28U;
  request.capabilities[1].byte_size = 28U;
  request.resources.shared_memory_bytes = 56U;
  const std::array<std::byte, 28U> input{};
  request.tensors[0].content_binding = compute_isolated_cpu_content_binding(
      request.identity, request.tensors[0], input.data(), input.size());
  validate_isolated_cpu_invocation_request(request, {});
  return request;
}

/**
 * @brief Creates one successful exact-plan response for a valid test request.
 * @param request Exact retained request.
 * @return Digest-bound six-byte output candidate.
 * @throws Protocol, digest, or allocation errors unchanged.
 */
IsolatedCpuInvocationResponse test_response(
    const IsolatedCpuInvocationRequest& request) {
  IsolatedCpuInvocationResponse response;
  response.identity = request.identity;
  response.operation = request.operation;
  response.resources = request.resources;
  response.outcome = IsolatedCpuInvocationOutcome::Succeeded;
  IsolatedCpuTensorDescriptor output = request.tensors[request.input_count];
  output.readiness = IsolatedCpuTensorReadiness::ReadyOutputCandidate;
  output.ownership = IsolatedCpuTensorOwnership::HostOutputCandidate;
  output.written_offset = 0U;
  output.written_length = output.capability_length;
  const std::vector<std::byte> output_bytes(
      static_cast<std::size_t>(output.capability_length), std::byte{7});
  output.content_binding = compute_isolated_cpu_content_binding(
      request.identity, output, output_bytes.data(), output_bytes.size());
  response.outputs.push_back(std::move(output));
  validate_isolated_cpu_invocation_response(request, response, {});
  return response;
}

/**
 * @brief Requires one readable byte range inside a test packet.
 * @param packet Complete encoded packet.
 * @param offset First requested byte.
 * @param size Requested byte count.
 * @return Nothing when the range is present.
 * @throws std::runtime_error when the test fixture layout is inconsistent.
 */
void require_wire_range(const std::vector<std::byte>& packet,
                        std::size_t offset, std::size_t size) {
  if (offset > packet.size() || size > packet.size() - offset) {
    throw std::runtime_error("isolated CPU test packet layout is truncated");
  }
}

/**
 * @brief Reads one test-only wire byte and advances a checked cursor.
 * @param packet Complete encoded packet.
 * @param cursor Non-null next-byte cursor.
 * @return Exact byte value.
 * @throws std::invalid_argument for a null cursor.
 * @throws std::runtime_error when the fixture packet is truncated.
 */
std::uint8_t read_wire_u8(const std::vector<std::byte>& packet,
                          std::size_t* cursor) {
  if (cursor == nullptr) {
    throw std::invalid_argument("isolated CPU test wire cursor is null");
  }
  require_wire_range(packet, *cursor, 1U);
  return std::to_integer<std::uint8_t>(packet[(*cursor)++]);
}

/**
 * @brief Reads one big-endian test-only uint32 and advances a checked cursor.
 * @param packet Complete encoded packet.
 * @param cursor Non-null next-byte cursor.
 * @return Exact decoded value.
 * @throws std::invalid_argument for a null cursor.
 * @throws std::runtime_error when the fixture packet is truncated.
 */
std::uint32_t read_wire_u32(const std::vector<std::byte>& packet,
                            std::size_t* cursor) {
  std::uint32_t value = 0U;
  for (std::size_t index = 0U; index < 4U; ++index) {
    value = (value << 8U) | read_wire_u8(packet, cursor);
  }
  return value;
}

/**
 * @brief Advances a checked test cursor over fixed wire bytes.
 * @param packet Complete encoded packet.
 * @param size Byte count to skip.
 * @param cursor Non-null next-byte cursor.
 * @return Nothing after advancing.
 * @throws std::invalid_argument for a null cursor.
 * @throws std::runtime_error when the fixture packet is truncated.
 */
void skip_wire_bytes(const std::vector<std::byte>& packet, std::size_t size,
                     std::size_t* cursor) {
  if (cursor == nullptr) {
    throw std::invalid_argument("isolated CPU test wire cursor is null");
  }
  require_wire_range(packet, *cursor, size);
  *cursor += size;
}

/**
 * @brief Locates and skips one length-prefixed test wire string.
 * @param packet Complete encoded packet.
 * @param cursor Non-null next-byte cursor.
 * @param length_out Optional exact decoded length output.
 * @return Offset of the first string byte.
 * @throws std::invalid_argument for a null cursor.
 * @throws std::runtime_error when the fixture packet is truncated.
 */
std::size_t skip_wire_string(const std::vector<std::byte>& packet,
                             std::size_t* cursor,
                             std::uint32_t* length_out = nullptr) {
  const std::uint32_t length = read_wire_u32(packet, cursor);
  const std::size_t bytes_offset = *cursor;
  skip_wire_bytes(packet, length, cursor);
  if (length_out != nullptr) {
    *length_out = length;
  }
  return bytes_offset;
}

/**
 * @brief Mutable canonical-field offsets in the deterministic request packet.
 * @throws std::bad_alloc when bounded offset vectors cannot allocate.
 */
struct RequestWireOffsets final {
  /** @brief First byte of each parameter name. */
  std::vector<std::size_t> parameter_name_offsets;
  /** @brief Exact byte length of each parameter name. */
  std::vector<std::uint32_t> parameter_name_lengths;
  /** @brief Scalar-kind byte for each parameter. */
  std::vector<std::size_t> parameter_kind_offsets;
  /** @brief First byte of each capability id. */
  std::vector<std::size_t> capability_id_offsets;
  /** @brief Sign byte of the first tensor's first signed stride. */
  std::size_t first_stride_sign_offset = 0U;
};

/**
 * @brief Locates canonical request fields used by direct wire rejection tests.
 * @param packet Canonical packet produced from `test_request()`.
 * @return Checked offsets for parameter, capability, and stride mutations.
 * @throws std::runtime_error when the deterministic fixture shape changes.
 * @throws std::bad_alloc when bounded offset storage cannot allocate.
 * @note This parser is test-only and intentionally follows protocol-v2 field
 * order so mutations exercise the production top-level decoder.
 */
RequestWireOffsets locate_request_wire_offsets(
    const std::vector<std::byte>& packet) {
  constexpr std::size_t kEncodedIdentityBytes = 112U;
  std::size_t cursor = kIsolatedCpuPacketHeaderBytes;
  skip_wire_bytes(packet, kEncodedIdentityBytes, &cursor);
  static_cast<void>(skip_wire_string(packet, &cursor));
  skip_wire_bytes(packet, 3U * 16U, &cursor);

  RequestWireOffsets offsets;
  const std::uint32_t parameter_count = read_wire_u32(packet, &cursor);
  offsets.parameter_name_offsets.reserve(parameter_count);
  offsets.parameter_name_lengths.reserve(parameter_count);
  offsets.parameter_kind_offsets.reserve(parameter_count);
  for (std::uint32_t index = 0U; index < parameter_count; ++index) {
    std::uint32_t name_length = 0U;
    offsets.parameter_name_offsets.push_back(
        skip_wire_string(packet, &cursor, &name_length));
    offsets.parameter_name_lengths.push_back(name_length);
    offsets.parameter_kind_offsets.push_back(cursor);
    const auto kind =
        static_cast<IsolatedCpuScalarKind>(read_wire_u8(packet, &cursor));
    switch (kind) {
      case IsolatedCpuScalarKind::Boolean:
        skip_wire_bytes(packet, 1U, &cursor);
        break;
      case IsolatedCpuScalarKind::SignedInteger:
        skip_wire_bytes(packet, 9U, &cursor);
        break;
      case IsolatedCpuScalarKind::UnsignedInteger:
      case IsolatedCpuScalarKind::FloatingPoint:
        skip_wire_bytes(packet, 8U, &cursor);
        break;
      case IsolatedCpuScalarKind::String:
        static_cast<void>(skip_wire_string(packet, &cursor));
        break;
      default:
        throw std::runtime_error(
            "isolated CPU test parameter kind unexpectedly changed");
    }
  }

  if (read_wire_u32(packet, &cursor) != 0U) {
    throw std::runtime_error(
        "isolated CPU scalar fixture unexpectedly has recursive config");
  }

  const std::uint32_t capability_count = read_wire_u32(packet, &cursor);
  offsets.capability_id_offsets.reserve(capability_count);
  for (std::uint32_t index = 0U; index < capability_count; ++index) {
    offsets.capability_id_offsets.push_back(cursor);
    skip_wire_bytes(packet, 8U + 1U + 8U, &cursor);
  }
  static_cast<void>(read_wire_u32(packet, &cursor));
  static_cast<void>(read_wire_u32(packet, &cursor));
  const std::uint32_t tensor_count = read_wire_u32(packet, &cursor);
  if (tensor_count == 0U) {
    throw std::runtime_error("isolated CPU test request has no tensor");
  }
  skip_wire_bytes(packet,
                  2U + 5U + (5U * 16U) + (2U * 8U) + (3U * 4U * 8U) + 8U + 8U +
                      8U + 1U + 1U + 4U,
                  &cursor);
  const std::uint32_t extent_count = read_wire_u32(packet, &cursor);
  skip_wire_bytes(packet, static_cast<std::size_t>(extent_count) * 8U, &cursor);
  const std::uint32_t stride_count = read_wire_u32(packet, &cursor);
  if (stride_count == 0U) {
    throw std::runtime_error("isolated CPU test tensor has no stride");
  }
  offsets.first_stride_sign_offset = cursor;
  require_wire_range(packet, offsets.first_stride_sign_offset, 9U);
  return offsets;
}

/**
 * @brief Proves a zero-Facet request row selects the six-byte generic profile.
 * @throws Region or allocation failures unchanged.
 */
TEST(IsolatedCpuInvocationProtocol,
     ConformanceFixtureDerivesFacetFreeOutputFromRuntimeDescriptor) {
  const IsolatedCpuRuntimeInvocation invocation =
      conformance_runtime_invocation(false, true);
  const auto profile = test_support::classify_isolated_cpu_conformance_tensor(
      invocation.outputs[0]);
  ASSERT_TRUE(profile.has_value());
  EXPECT_EQ(
      profile->representation,
      test_support::IsolatedCpuConformanceRepresentation::FacetFreeDenseTensor);
  EXPECT_EQ(profile->metadata,
            test_support::IsolatedCpuConformanceMetadataProfile::Fixture);
  EXPECT_EQ(profile->storage_size, 6U);
  EXPECT_FALSE(invocation.outputs[0].descriptor.facet_identity.valid());
  EXPECT_FALSE(invocation.outputs[0].descriptor.image_facet.has_value());
  EXPECT_TRUE(
      test_support::conformance_runtime_invocation_is_exact(invocation));
}

/**
 * @brief Proves a built-in Image Facet selects the four-byte image profile.
 * @throws Region or allocation failures unchanged.
 */
TEST(IsolatedCpuInvocationProtocol,
     ConformanceFixtureAcceptsImageOutputFromRuntimeDescriptor) {
  const IsolatedCpuRuntimeInvocation invocation =
      conformance_runtime_invocation(false, false);
  const auto profile = test_support::classify_isolated_cpu_conformance_tensor(
      invocation.outputs[0]);
  ASSERT_TRUE(profile.has_value());
  EXPECT_EQ(profile->representation,
            test_support::IsolatedCpuConformanceRepresentation::Image);
  EXPECT_EQ(profile->metadata,
            test_support::IsolatedCpuConformanceMetadataProfile::Fixture);
  EXPECT_EQ(profile->storage_size, 4U);
  EXPECT_TRUE(invocation.outputs[0].descriptor.facet_identity.valid());
  EXPECT_TRUE(invocation.outputs[0].descriptor.image_facet.has_value());
  EXPECT_TRUE(
      test_support::conformance_runtime_invocation_is_exact(invocation));
}

/**
 * @brief Proves every input/output representation and metadata source is
 * derived without monolithic/tiled mode state.
 * @throws Region or allocation failures unchanged.
 * @note A nested configuration is retained on every synthetic request to show
 * that configuration shape cannot select or corrupt the tensor profile.
 */
TEST(IsolatedCpuInvocationProtocol,
     ConformanceFixtureAcceptsEveryRequestDerivedProfileCombination) {
  for (const bool facet_free_input : {false, true}) {
    for (const bool facet_free_output : {false, true}) {
      for (const bool fixture_input_metadata : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "facet_free_input=" << facet_free_input
                     << " facet_free_output=" << facet_free_output
                     << " fixture_input_metadata=" << fixture_input_metadata);
        IsolatedCpuRuntimeInvocation invocation =
            conformance_runtime_invocation(facet_free_input, facet_free_output,
                                           fixture_input_metadata);
        IsolatedCpuConfigurationNode root;
        root.kind = IsolatedCpuConfigurationKind::Object;
        root.first_child = 1U;
        root.child_count = 1U;
        IsolatedCpuConfigurationNode empty_array;
        empty_array.kind = IsolatedCpuConfigurationKind::Array;
        empty_array.key = "items";
        empty_array.first_child = 0U;
        invocation.configuration = {root, empty_array};
        EXPECT_TRUE(
            test_support::conformance_runtime_invocation_is_exact(invocation));
      }
    }
  }
}

/**
 * @brief Proves exact range, Facet, Region, version, digest, and output-source
 * checks remain fail closed.
 * @throws Region or allocation failures unchanged.
 */
TEST(IsolatedCpuInvocationProtocol,
     ConformanceFixtureRejectsChangedDescriptorFacts) {
  const IsolatedCpuRuntimeInvocation facet_free =
      conformance_runtime_invocation(false, true);

  IsolatedCpuRuntimeTensor changed = facet_free.outputs[0];
  changed.size = 4U;
  EXPECT_FALSE(test_support::classify_isolated_cpu_conformance_tensor(changed)
                   .has_value());

  changed = facet_free.outputs[0];
  changed.descriptor.capability_length = 4U;
  EXPECT_FALSE(test_support::classify_isolated_cpu_conformance_tensor(changed)
                   .has_value());

  changed = facet_free.outputs[0];
  changed.descriptor.facet_identity =
      opaque_id_from_words(PS_OPERATION_BUILTIN_IMAGE_FACET_IDENTITY_WORD0_V1,
                           PS_OPERATION_BUILTIN_IMAGE_FACET_IDENTITY_WORD1_V1);
  EXPECT_FALSE(test_support::classify_isolated_cpu_conformance_tensor(changed)
                   .has_value());

  changed = facet_free.outputs[0];
  changed.descriptor.schema_version += 1U;
  EXPECT_FALSE(test_support::classify_isolated_cpu_conformance_tensor(changed)
                   .has_value());

  changed = facet_free.outputs[0];
  changed.descriptor.layout_digest.words[2] ^= 1U;
  EXPECT_FALSE(test_support::classify_isolated_cpu_conformance_tensor(changed)
                   .has_value());

  changed = facet_free.outputs[0];
  changed.descriptor.region = RegionSet::whole();
  EXPECT_FALSE(test_support::classify_isolated_cpu_conformance_tensor(changed)
                   .has_value());

  IsolatedCpuRuntimeInvocation fallback_output = facet_free;
  IsolatedCpuTensorDescriptor& descriptor =
      fallback_output.outputs[0].descriptor;
  descriptor.schema_version =
      PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_VERSION_V1;
  descriptor.layout_version = PS_OPERATION_BUILTIN_STRIDED_LAYOUT_VERSION_V1;
  descriptor.descriptor_digest = {};
  descriptor.logical_content_digest = {};
  descriptor.layout_digest = {};
  EXPECT_FALSE(
      test_support::conformance_runtime_invocation_is_exact(fallback_output));
}

TEST(IsolatedCpuInvocationProtocol, RequestAndResponseRoundTripExactly) {
  const IsolatedCpuInvocationRequest request = test_request();
  const std::vector<std::byte> request_packet =
      encode_isolated_cpu_invocation_request(request, {});
  const IsolatedCpuInvocationRequest decoded_request =
      decode_isolated_cpu_invocation_request(request_packet, {});
  EXPECT_EQ(decoded_request, request);
  EXPECT_EQ(encode_isolated_cpu_invocation_request(decoded_request, {}),
            request_packet);

  const IsolatedCpuInvocationResponse response = test_response(request);
  const std::vector<std::byte> response_packet =
      encode_isolated_cpu_invocation_response(request, response, {});
  const IsolatedCpuInvocationResponse decoded_response =
      decode_isolated_cpu_invocation_response(request, response_packet, {});
  EXPECT_EQ(decoded_response, response);
  EXPECT_EQ(encode_isolated_cpu_invocation_response(decoded_request,
                                                    decoded_response, {}),
            response_packet);
}

TEST(IsolatedCpuInvocationProtocol,
     RichImageMetadataAndImmutablePlanRoundTripExactly) {
  const IsolatedCpuInvocationRequest request = rich_image_request();
  const std::vector<std::byte> request_packet =
      encode_isolated_cpu_invocation_request(request, {});
  const IsolatedCpuInvocationRequest decoded_request =
      decode_isolated_cpu_invocation_request(request_packet, {});
  ASSERT_EQ(decoded_request, request);
  ASSERT_TRUE(decoded_request.tensors[0].image_facet.has_value());
  EXPECT_EQ(decoded_request.tensors[0]
                .image_facet->channel_schema->channels[0]
                .diagnostic_name,
            "red");
  EXPECT_EQ(decoded_request.tensors[0].image_facet->data_window.x_begin, -3);
  EXPECT_EQ(decoded_request.tensors[1].allocation_alignment, 16U);
  EXPECT_EQ(decoded_request.tensors[1].region, request.tensors[1].region);
  EXPECT_EQ(decoded_request.tensors[1].schema_identity,
            request.tensors[1].schema_identity);
  EXPECT_EQ(decoded_request.tensors[1].facet_identity,
            request.tensors[1].facet_identity);
  EXPECT_EQ(decoded_request.tensors[1].layout_identity,
            request.tensors[1].layout_identity);
  EXPECT_EQ(decoded_request.tensors[1].schema_version, 7U);
  EXPECT_EQ(decoded_request.tensors[1].layout_version, 11U);
  EXPECT_EQ(decoded_request.tensors[1].descriptor_digest,
            test_operation_digest(0x101U));
  EXPECT_EQ(decoded_request.tensors[1].logical_content_digest,
            test_operation_digest(0x201U));
  EXPECT_EQ(decoded_request.tensors[1].layout_digest,
            test_operation_digest(0x301U));

  const IsolatedCpuInvocationResponse valid = test_response(request);
  EXPECT_NO_THROW(
      validate_isolated_cpu_invocation_response(request, valid, {}));

  IsolatedCpuInvocationResponse hostile = valid;
  hostile.outputs[0].binding_identity = test_opaque_id(201U);
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, hostile, {}),
               IsolatedCpuProtocolError);

  hostile = valid;
  hostile.outputs[0].image_facet->data_window.x_begin -= 1;
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, hostile, {}),
               IsolatedCpuProtocolError);

  hostile = valid;
  hostile.outputs[0].written_length -= 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, hostile, {}),
               IsolatedCpuProtocolError);

  hostile = valid;
  hostile.outputs[0].schema_identity = test_opaque_id(202U);
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, hostile, {}),
               IsolatedCpuProtocolError);

  hostile = valid;
  hostile.outputs[0].schema_version += 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, hostile, {}),
               IsolatedCpuProtocolError);

  hostile = valid;
  hostile.outputs[0].descriptor_digest.words[0] ^= 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, hostile, {}),
               IsolatedCpuProtocolError);

  hostile = valid;
  hostile.outputs[0].logical_content_digest.words[1] ^= 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, hostile, {}),
               IsolatedCpuProtocolError);

  hostile = valid;
  hostile.outputs[0].layout_digest.words[2] ^= 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, hostile, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     RecursiveConfigurationRoundTripsAndRejectsHostileRelationships) {
  IsolatedCpuInvocationRequest request = test_request();
  request.parameters.clear();
  IsolatedCpuConfigurationNode root;
  root.kind = IsolatedCpuConfigurationKind::Object;
  root.first_child = 1U;
  root.child_count = 2U;
  IsolatedCpuConfigurationNode array;
  array.kind = IsolatedCpuConfigurationKind::Array;
  array.key = "items";
  array.first_child = 3U;
  array.child_count = 2U;
  IsolatedCpuConfigurationNode threshold;
  threshold.kind = IsolatedCpuConfigurationKind::FloatingPoint;
  threshold.key = "threshold";
  threshold.floating_value = 0.5;
  IsolatedCpuConfigurationNode item;
  item.kind = IsolatedCpuConfigurationKind::SignedInteger;
  item.signed_value = -7;
  IsolatedCpuConfigurationNode null_item;
  request.configuration = {root, array, threshold, item, null_item};

  const std::vector<std::byte> packet =
      encode_isolated_cpu_invocation_request(request, {});
  EXPECT_EQ(decode_isolated_cpu_invocation_request(packet, {}), request);

  IsolatedCpuInvocationRequest hostile = request;
  hostile.configuration[2].key = "items";
  EXPECT_THROW(validate_isolated_cpu_invocation_request(hostile, {}),
               IsolatedCpuProtocolError);

  hostile = request;
  hostile.configuration[1].first_child = 2U;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(hostile, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     WireDecodeRejectsDuplicateParametersCapabilitiesAndUnknownKind) {
  const IsolatedCpuInvocationRequest request = test_request();
  const std::vector<std::byte> valid =
      encode_isolated_cpu_invocation_request(request, {});
  const RequestWireOffsets offsets = locate_request_wire_offsets(valid);
  ASSERT_EQ(offsets.parameter_name_offsets.size(), 2U);
  ASSERT_EQ(offsets.parameter_name_lengths[0],
            offsets.parameter_name_lengths[1]);
  ASSERT_EQ(offsets.capability_id_offsets.size(), 2U);

  std::vector<std::byte> duplicate_parameter = valid;
  std::copy_n(
      duplicate_parameter.begin() +
          static_cast<std::ptrdiff_t>(offsets.parameter_name_offsets[0]),
      offsets.parameter_name_lengths[0],
      duplicate_parameter.begin() +
          static_cast<std::ptrdiff_t>(offsets.parameter_name_offsets[1]));
  EXPECT_THROW(decode_isolated_cpu_invocation_request(duplicate_parameter, {}),
               IsolatedCpuProtocolError);

  std::vector<std::byte> duplicate_capability = valid;
  std::copy_n(
      duplicate_capability.begin() +
          static_cast<std::ptrdiff_t>(offsets.capability_id_offsets[0]),
      8U,
      duplicate_capability.begin() +
          static_cast<std::ptrdiff_t>(offsets.capability_id_offsets[1]));
  EXPECT_THROW(decode_isolated_cpu_invocation_request(duplicate_capability, {}),
               IsolatedCpuProtocolError);

  std::vector<std::byte> unknown_kind = valid;
  unknown_kind[offsets.parameter_kind_offsets[0]] = std::byte{0xff};
  EXPECT_THROW(decode_isolated_cpu_invocation_request(unknown_kind, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     WireDecodeRejectsSignPlusMagnitudeNegativeZeroStride) {
  const IsolatedCpuInvocationRequest request = test_request();
  std::vector<std::byte> malformed =
      encode_isolated_cpu_invocation_request(request, {});
  const RequestWireOffsets offsets = locate_request_wire_offsets(malformed);
  malformed[offsets.first_stride_sign_offset] = std::byte{1};
  std::fill_n(malformed.begin() + static_cast<std::ptrdiff_t>(
                                      offsets.first_stride_sign_offset + 1U),
              8U, std::byte{0});
  EXPECT_THROW(decode_isolated_cpu_invocation_request(malformed, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     RejectsWrongMagicVersionKindLengthAndTrailingBytes) {
  const IsolatedCpuInvocationRequest request = test_request();
  const std::vector<std::byte> valid =
      encode_isolated_cpu_invocation_request(request, {});
  for (const std::size_t offset : {0U, 5U, 7U, 11U}) {
    std::vector<std::byte> malformed = valid;
    malformed[offset] ^= std::byte{0x7f};
    EXPECT_THROW(decode_isolated_cpu_invocation_request(malformed, {}),
                 IsolatedCpuProtocolError);
  }
  std::vector<std::byte> trailing = valid;
  trailing.push_back(std::byte{0});
  EXPECT_THROW(decode_isolated_cpu_invocation_request(trailing, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     RejectsStaleIdentityPermissionsReadinessAndUnsortedParameters) {
  IsolatedCpuInvocationRequest request = test_request();
  request.identity.worker_lease_generation = 0U;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);

  request = test_request();
  request.capabilities[0].access = IsolatedCpuCapabilityAccess::ReadWrite;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);

  request = test_request();
  request.tensors[1].readiness =
      IsolatedCpuTensorReadiness::ReadyOutputCandidate;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);

  request = test_request();
  std::swap(request.parameters[0], request.parameters[1]);
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     RejectsRankRangeStrideOverlapAndResourceWidening) {
  IsolatedCpuInvocationRequest request = test_request();
  request.tensors[1].byte_strides = {1, 1};
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);

  request = test_request();
  request.tensors[0].capability_length = 5U;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);

  request = test_request();
  request.tensors[0].extents.push_back(1U);
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);

  request = test_request();
  request.resources.shared_memory_bytes += 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     AcceptsCheckedZeroAndNegativeReadOnlyStridesButRejectsUnderflow) {
  IsolatedCpuInvocationRequest request = test_request();
  request.tensors[0].byte_strides = {0, -1};
  request.tensors[0].byte_offset = 2U;
  const std::array<std::byte, 6U> input{std::byte{1}, std::byte{2},
                                        std::byte{3}, std::byte{4},
                                        std::byte{5}, std::byte{6}};
  request.tensors[0].content_binding = compute_isolated_cpu_content_binding(
      request.identity, request.tensors[0], input.data(), input.size());
  EXPECT_NO_THROW(validate_isolated_cpu_invocation_request(request, {}));

  request.tensors[0].byte_offset = 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     RejectsOverlappingWritableRangesAcrossTensorRecords) {
  IsolatedCpuInvocationRequest request = test_request();
  request.tensors.push_back(request.tensors[1]);
  request.output_count = 2U;
  request.resources.descriptor_count = 3U;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     RejectsOutputPlanMutationFailureFramingAndStaleGeneration) {
  const IsolatedCpuInvocationRequest request = test_request();
  IsolatedCpuInvocationResponse response = test_response(request);
  response.outputs[0].byte_strides[0] += 1;
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, response, {}),
               IsolatedCpuProtocolError);

  response = test_response(request);
  response.identity.plugin_generation += 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, response, {}),
               IsolatedCpuProtocolError);

  response = test_response(request);
  response.outcome = IsolatedCpuInvocationOutcome::PluginFailed;
  response.outputs.clear();
  response.diagnostic.clear();
  EXPECT_THROW(validate_isolated_cpu_invocation_response(request, response, {}),
               IsolatedCpuProtocolError);
}

TEST(IsolatedCpuInvocationProtocol,
     PhysicalBindingIncludesPaddingAndDescriptorIdentity) {
  IsolatedCpuInvocationRequest request = test_request();
  IsolatedCpuTensorDescriptor descriptor = request.tensors[0];
  descriptor.extents = {2U, 2U};
  descriptor.byte_strides = {3, 1};
  descriptor.capability_length = 5U;
  std::array<std::byte, 5U> bytes{std::byte{1}, std::byte{2}, std::byte{0},
                                  std::byte{3}, std::byte{4}};
  const ContentDigest initial = compute_isolated_cpu_content_binding(
      request.identity, descriptor, bytes.data(), bytes.size());
  bytes[2] = std::byte{99};
  const ContentDigest changed_padding = compute_isolated_cpu_content_binding(
      request.identity, descriptor, bytes.data(), bytes.size());
  EXPECT_FALSE(initial == changed_padding);

  descriptor.byte_strides[0] = 2;
  const ContentDigest changed_descriptor = compute_isolated_cpu_content_binding(
      request.identity, descriptor, bytes.data(), bytes.size());
  EXPECT_FALSE(changed_padding == changed_descriptor);
}

TEST(IsolatedCpuInvocationProtocol, EnforcesEndpointHardLimitsBeforeUse) {
  const IsolatedCpuInvocationRequest request = test_request();
  IsolatedCpuInvocationLimits limits;
  limits.maximum_shared_memory_bytes = 11U;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, limits),
               IsolatedCpuProtocolError);

  limits = {};
  limits.maximum_capabilities = 1U;
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, limits),
               IsolatedCpuProtocolError);

  limits = {};
  limits.maximum_descriptors = 0U;
  EXPECT_THROW(validate_isolated_cpu_invocation_limits(limits),
               std::invalid_argument);
}

TEST(IsolatedCpuInvocationProtocol,
     AllowsZeroParameterLimitAndRejectsAnyParameter) {
  IsolatedCpuInvocationLimits limits;
  limits.maximum_parameters = 0U;
  EXPECT_NO_THROW(validate_isolated_cpu_invocation_limits(limits));

  IsolatedCpuInvocationRequest request = test_request();
  request.parameters.clear();
  EXPECT_NO_THROW(validate_isolated_cpu_invocation_request(request, limits));
  const std::vector<std::byte> packet =
      encode_isolated_cpu_invocation_request(request, limits);
  EXPECT_EQ(decode_isolated_cpu_invocation_request(packet, limits), request);

  request = test_request();
  EXPECT_THROW(validate_isolated_cpu_invocation_request(request, limits),
               IsolatedCpuProtocolError);
}

/**
 * @brief Exercises bounded deterministic request/response decode mutations.
 * @return Nothing; GoogleTest reports noncanonical success or open failure.
 * @throws Allocation failures from bounded packet copies unchanged.
 * @note Successful mutations must round-trip byte-for-byte; every strict
 * prefix and malformed mutation otherwise fails through the closed protocol
 * exception without acquiring invocation resources.
 */
TEST(IsolatedCpuInvocationProtocol,
     BoundedMutationCorpusIsCanonicalOrFailsWithProtocolError) {
  const IsolatedCpuInvocationRequest request = test_request();
  const std::vector<std::byte> request_packet =
      encode_isolated_cpu_invocation_request(request, {});
  for (std::size_t retained = 0U; retained < request_packet.size();
       ++retained) {
    std::vector<std::byte> truncated(request_packet.begin(),
                                     request_packet.begin() + retained);
    EXPECT_THROW(decode_isolated_cpu_invocation_request(truncated, {}),
                 IsolatedCpuProtocolError)
        << "request retained bytes=" << retained;
  }
  for (std::size_t offset = 0U; offset < request_packet.size(); ++offset) {
    std::vector<std::byte> mutated = request_packet;
    mutated[offset] ^= static_cast<std::byte>(
        static_cast<std::uint8_t>(0x01U << (offset % 8U)));
    try {
      const IsolatedCpuInvocationRequest decoded =
          decode_isolated_cpu_invocation_request(mutated, {});
      EXPECT_EQ(encode_isolated_cpu_invocation_request(decoded, {}), mutated)
          << "request mutation offset=" << offset;
    } catch (const IsolatedCpuProtocolError&) {
    }
  }

  const IsolatedCpuInvocationResponse response = test_response(request);
  const std::vector<std::byte> response_packet =
      encode_isolated_cpu_invocation_response(request, response, {});
  for (std::size_t retained = 0U; retained < response_packet.size();
       ++retained) {
    std::vector<std::byte> truncated(response_packet.begin(),
                                     response_packet.begin() + retained);
    EXPECT_THROW(
        decode_isolated_cpu_invocation_response(request, truncated, {}),
        IsolatedCpuProtocolError)
        << "response retained bytes=" << retained;
  }
  for (std::size_t offset = 0U; offset < response_packet.size(); ++offset) {
    std::vector<std::byte> mutated = response_packet;
    mutated[offset] ^= static_cast<std::byte>(
        static_cast<std::uint8_t>(0x80U >> (offset % 8U)));
    try {
      const IsolatedCpuInvocationResponse decoded =
          decode_isolated_cpu_invocation_response(request, mutated, {});
      EXPECT_EQ(encode_isolated_cpu_invocation_response(request, decoded, {}),
                mutated)
          << "response mutation offset=" << offset;
    } catch (const IsolatedCpuProtocolError&) {
    }
  }
}

}  // namespace
}  // namespace ps::execution
