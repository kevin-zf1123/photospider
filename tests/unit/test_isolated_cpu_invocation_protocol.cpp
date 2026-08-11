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
#include <string>
#include <utility>
#include <vector>

#include "execution/isolated_cpu_invocation_protocol.hpp"  // NOLINT(build/include_subdir)

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
  IsolatedCpuScalarParameter enabled;
  enabled.name = "enabled";
  enabled.kind = IsolatedCpuScalarKind::Boolean;
  enabled.boolean_value = true;
  IsolatedCpuScalarParameter scale;
  scale.name = "scale";
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
  const std::array<std::byte, 6U> output_bytes{std::byte{7},  std::byte{8},
                                               std::byte{9},  std::byte{10},
                                               std::byte{11}, std::byte{12}};
  output.content_binding = compute_isolated_cpu_content_binding(
      request.identity, output, output_bytes.data(), output_bytes.size());
  response.outputs.push_back(std::move(output));
  validate_isolated_cpu_invocation_response(request, response, {});
  return response;
}

TEST(IsolatedCpuInvocationProtocol, RequestAndResponseRoundTripExactly) {
  const IsolatedCpuInvocationRequest request = test_request();
  const std::vector<std::byte> request_packet =
      encode_isolated_cpu_invocation_request(request, {});
  EXPECT_EQ(decode_isolated_cpu_invocation_request(request_packet, {}),
            request);

  const IsolatedCpuInvocationResponse response = test_response(request);
  const std::vector<std::byte> response_packet =
      encode_isolated_cpu_invocation_response(request, response, {});
  EXPECT_EQ(
      decode_isolated_cpu_invocation_response(request, response_packet, {}),
      response);
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

}  // namespace
}  // namespace ps::execution
