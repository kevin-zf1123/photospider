/**
 * @file fuzz_isolated_cpu_invocation_codec.cpp
 * @brief Manual bounded libFuzzer harness for isolated CPU product codecs.
 *
 * The harness constructs only pointer-free metadata and small fixed byte
 * arrays used for canonical content bindings. It creates no mapping, file
 * descriptor, callback, plugin process, lease, or execution authority.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "execution/isolated_cpu_invocation_protocol.hpp"  // NOLINT(build/include_subdir)

namespace ps::execution {
namespace {

/**
 * @brief Canonical request plus encoded request/response seed packets.
 * @throws Nothing after construction; the factory may allocate or throw.
 */
struct IsolatedCodecSeedCorpus final {
  /** @brief Valid retained request required to validate response packets. */
  IsolatedCpuInvocationRequest request;

  /** @brief Canonical encoded request packet. */
  std::vector<std::byte> request_packet;

  /** @brief Canonical encoded successful response packet. */
  std::vector<std::byte> response_packet;
};

/**
 * @brief Creates one deterministic nonzero opaque comparison identity.
 * @param seed First byte in the nonzero deterministic sequence.
 * @return Complete 128-bit opaque identity.
 * @throws Nothing.
 */
IsolatedCpuOpaqueId make_opaque_id(std::uint8_t seed) noexcept {
  IsolatedCpuOpaqueId identity;
  for (std::size_t index = 0U; index < identity.bytes.size(); ++index) {
    identity.bytes[index] = static_cast<std::byte>(seed + index);
  }
  return identity;
}

/**
 * @brief Creates one complete deterministic invocation identity tuple.
 * @return Valid comparison-only identity with nonzero generations.
 * @throws Nothing.
 */
IsolatedCpuInvocationIdentity make_identity() noexcept {
  IsolatedCpuInvocationIdentity identity;
  identity.tenant_id = make_opaque_id(1U);
  identity.job_id = make_opaque_id(17U);
  identity.attempt_id = make_opaque_id(33U);
  identity.worker_id = make_opaque_id(49U);
  identity.worker_lease_generation = 7U;
  identity.plugin_package_id = make_opaque_id(65U);
  identity.plugin_generation = 11U;
  identity.invocation_id = make_opaque_id(81U);
  return identity;
}

/**
 * @brief Creates one exact two-by-three unsigned-byte tensor descriptor.
 * @param capability_id Invocation-local capability selector.
 * @param access Input or output direction.
 * @return Six-byte canonical strided tensor plan.
 * @throws std::bad_alloc when bounded vector storage cannot allocate.
 */
IsolatedCpuTensorDescriptor make_tensor(std::uint64_t capability_id,
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
 * @brief Builds one valid pointer-free invocation request seed.
 * @return One-input/one-output request within default protocol limits.
 * @throws Protocol, digest, or allocation failures unchanged.
 */
IsolatedCpuInvocationRequest make_request() {
  IsolatedCpuInvocationRequest request;
  request.identity = make_identity();
  request.operation = "fuzz.copy";
  IsolatedCpuScalarParameter enabled;
  enabled.name = "enabled";
  enabled.kind = IsolatedCpuScalarKind::Boolean;
  enabled.boolean_value = true;
  request.parameters.push_back(std::move(enabled));
  request.capabilities = {
      IsolatedCpuCapability{1U, IsolatedCpuCapabilityAccess::ReadOnly, 6U},
      IsolatedCpuCapability{2U, IsolatedCpuCapabilityAccess::ReadWrite, 6U}};
  request.tensors = {make_tensor(1U, IsolatedCpuTensorAccess::InputReadOnly),
                     make_tensor(2U, IsolatedCpuTensorAccess::OutputWriteOnly)};
  const std::array<std::byte, 6U> input_bytes{std::byte{1}, std::byte{2},
                                              std::byte{3}, std::byte{4},
                                              std::byte{5}, std::byte{6}};
  request.tensors[0].content_binding = compute_isolated_cpu_content_binding(
      request.identity, request.tensors[0], input_bytes.data(),
      input_bytes.size());
  request.input_count = 1U;
  request.output_count = 1U;
  request.resources.shared_memory_bytes = 12U;
  request.resources.descriptor_count = 2U;
  request.resources.cpu_slots = 1U;
  validate_isolated_cpu_invocation_request(request, {});
  return request;
}

/**
 * @brief Builds one successful exact-plan response seed.
 * @param request Valid retained request.
 * @return Digest-bound six-byte output candidate response.
 * @throws Protocol, digest, or allocation failures unchanged.
 */
IsolatedCpuInvocationResponse make_response(
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

/**
 * @brief Creates canonical request and response packet seeds.
 * @return Complete immutable seed corpus.
 * @throws Protocol, digest, or allocation failures unchanged.
 */
IsolatedCodecSeedCorpus make_seed_corpus() {
  IsolatedCodecSeedCorpus corpus;
  corpus.request = make_request();
  corpus.request_packet =
      encode_isolated_cpu_invocation_request(corpus.request, {});
  corpus.response_packet = encode_isolated_cpu_invocation_response(
      corpus.request, make_response(corpus.request), {});
  return corpus;
}

/**
 * @brief Returns the process-lifetime isolated codec seed corpus.
 * @return Borrowed immutable corpus.
 * @throws Seed construction failures on first access.
 * @note Function-local initialization is thread-safe under C++17.
 */
const IsolatedCodecSeedCorpus& seed_corpus() {
  static const IsolatedCodecSeedCorpus corpus = make_seed_corpus();
  return corpus;
}

/**
 * @brief Stops the fuzz process for a noncanonical successful decode.
 * @return Never returns.
 * @throws Nothing.
 */
[[noreturn]] void fail_noncanonical_decode() noexcept {
  std::abort();
}

/**
 * @brief Copies one bounded arbitrary packet.
 * @param data First packet byte, null only when `size` is zero.
 * @param size Exact bounded byte count.
 * @return Independently owned candidate packet.
 * @throws std::bad_alloc when bounded vector storage cannot allocate.
 */
std::vector<std::byte> copy_packet(const std::uint8_t* data, std::size_t size) {
  const auto* bytes = reinterpret_cast<const std::byte*>(data);
  return size == 0U ? std::vector<std::byte>{}
                    : std::vector<std::byte>(bytes, bytes + size);
}

/**
 * @brief Applies arbitrary bytes to one canonical packet.
 * @param seed Canonical packet to copy.
 * @param data Mutation bytes, null only when `size` is zero.
 * @param size Mutation byte count.
 * @return Canonical-sized packet with deterministic XOR mutations.
 * @throws std::bad_alloc when bounded packet copying cannot allocate.
 * @note Raw modes independently cover arbitrary packet lengths.
 */
std::vector<std::byte> mutate_packet(const std::vector<std::byte>& seed,
                                     const std::uint8_t* data,
                                     std::size_t size) {
  std::vector<std::byte> mutated = seed;
  const std::size_t overlap = std::min(mutated.size(), size);
  for (std::size_t index = 0U; index < overlap; ++index) {
    mutated[index] ^= static_cast<std::byte>(data[index]);
  }
  return mutated;
}

/**
 * @brief Exercises request decode, full descriptor validation, and encoding.
 * @param packet Complete bounded candidate packet.
 * @return Nothing after rejection or exact canonical re-encoding.
 * @throws IsolatedCpuProtocolError for expected malformed input rejection.
 * @throws Any unexpected product exception unchanged as a fuzz finding.
 */
void exercise_request(const std::vector<std::byte>& packet) {
  const IsolatedCpuInvocationRequest decoded =
      decode_isolated_cpu_invocation_request(packet, {});
  if (encode_isolated_cpu_invocation_request(decoded, {}) != packet) {
    fail_noncanonical_decode();
  }
}

/**
 * @brief Exercises response decode, retained-plan validation, and encoding.
 * @param request Valid retained request authority plan.
 * @param packet Complete bounded candidate response packet.
 * @return Nothing after rejection or exact canonical re-encoding.
 * @throws IsolatedCpuProtocolError for expected malformed input rejection.
 * @throws Any unexpected product exception unchanged as a fuzz finding.
 */
void exercise_response(const IsolatedCpuInvocationRequest& request,
                       const std::vector<std::byte>& packet) {
  const IsolatedCpuInvocationResponse decoded =
      decode_isolated_cpu_invocation_response(request, packet, {});
  if (encode_isolated_cpu_invocation_response(request, decoded, {}) != packet) {
    fail_noncanonical_decode();
  }
}

}  // namespace
}  // namespace ps::execution

/**
 * @brief Runs one bounded isolated invocation codec fuzz iteration.
 * @param data Arbitrary libFuzzer bytes; the first byte selects raw request,
 * canonical request mutation, raw response, or canonical response mutation.
 * @param size Exact input size.
 * @return Zero after a successful canonical decode or closed protocol reject.
 * @throws Unexpected allocation, contract, or codec exceptions unchanged so
 * libFuzzer records them as findings.
 * @note Inputs above the product packet maximum plus selector are ignored
 * before copying. `IsolatedCpuProtocolError` is the expected rejection type.
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  using ps::execution::copy_packet;
  using ps::execution::exercise_request;
  using ps::execution::exercise_response;
  using ps::execution::IsolatedCodecSeedCorpus;
  using ps::execution::IsolatedCpuProtocolError;
  using ps::execution::kMaximumIsolatedCpuPacketBytes;
  using ps::execution::mutate_packet;
  using ps::execution::seed_corpus;
  if (size == 0U || data == nullptr ||
      size > kMaximumIsolatedCpuPacketBytes + 1U) {
    return 0;
  }
  const IsolatedCodecSeedCorpus& corpus = seed_corpus();
  const std::uint8_t mode = data[0] & 0x03U;
  const std::uint8_t* packet_data = data + 1U;
  const std::size_t packet_size = size - 1U;
  try {
    if (mode == 0U) {
      exercise_request(copy_packet(packet_data, packet_size));
    } else if (mode == 1U) {
      exercise_request(
          mutate_packet(corpus.request_packet, packet_data, packet_size));
    } else if (mode == 2U) {
      exercise_response(corpus.request, copy_packet(packet_data, packet_size));
    } else {
      exercise_response(
          corpus.request,
          mutate_packet(corpus.response_packet, packet_data, packet_size));
    }
  } catch (const IsolatedCpuProtocolError&) {
    return 0;
  }
  return 0;
}
