/**
 * @file job_contract.cpp
 * @brief Implements canonical Issue #99 Job values and dependency-free SHA-256.
 */
#include "server/job_contract.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace ps::server {
namespace {

/** @brief Exact version token that begins every supported canonical JobSpec. */
constexpr char kJobSpecVersion[] = "jobspec-v2";

/**
 * @brief Rotates one 32-bit SHA-256 word right.
 * @param value Word to rotate.
 * @param bits Rotation count in `[1,31]`.
 * @return Rotated word.
 * @throws Nothing.
 */
constexpr std::uint32_t rotate_right(std::uint32_t value,
                                     std::uint32_t bits) noexcept {
  return (value >> bits) | (value << (32U - bits));
}

}  // namespace

/** @copydoc ps::server::ArtifactContentHasher::ArtifactContentHasher */
ArtifactContentHasher::ArtifactContentHasher() noexcept
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ps::server::ArtifactContentHasher::update */
void ArtifactContentHasher::update(const std::byte* data, std::size_t size) {
  if (finished_) {
    throw std::invalid_argument("SHA-256 is already finalized");
  }
  if (size != 0U && data == nullptr) {
    throw std::invalid_argument("SHA-256 input is null and nonempty");
  }
  if (size > (std::numeric_limits<std::uint64_t>::max() / 8U) - total_bytes_) {
    throw std::overflow_error("SHA-256 bit length overflowed");
  }
  total_bytes_ += static_cast<std::uint64_t>(size);
  while (size != 0U) {
    const std::size_t copied = std::min(size, buffer_.size() - buffered_);
    std::memcpy(buffer_.data() + buffered_, data, copied);
    buffered_ += copied;
    data += copied;
    size -= copied;
    if (buffered_ == buffer_.size()) {
      compress(buffer_.data());
      buffered_ = 0U;
    }
  }
}

/** @copydoc ps::server::ArtifactContentHasher::finish */
ArtifactContentDigest ArtifactContentHasher::finish() {
  if (finished_) {
    throw std::logic_error("SHA-256 was finalized more than once");
  }
  finished_ = true;
  const std::uint64_t bit_length = total_bytes_ * 8U;
  buffer_[buffered_++] = std::byte{0x80};
  if (buffered_ > 56U) {
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
              buffer_.end(), std::byte{0});
    compress(buffer_.data());
    buffered_ = 0U;
  }
  std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
            buffer_.begin() + 56, std::byte{0});
  for (std::size_t index = 0U; index < 8U; ++index) {
    buffer_[56U + index] =
        static_cast<std::byte>((bit_length >> ((7U - index) * 8U)) & 0xffU);
  }
  compress(buffer_.data());

  ArtifactContentDigest digest;
  for (std::size_t word = 0U; word < state_.size(); ++word) {
    for (std::size_t octet = 0U; octet < 4U; ++octet) {
      digest.bytes[word * 4U + octet] =
          static_cast<std::byte>((state_[word] >> ((3U - octet) * 8U)) & 0xffU);
    }
  }
  return digest;
}

/** @copydoc ps::server::ArtifactContentHasher::compress */
void ArtifactContentHasher::compress(const std::byte* block) noexcept {
  static constexpr std::array<std::uint32_t, 64U> kRoundConstants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  std::array<std::uint32_t, 64U> schedule{};
  for (std::size_t index = 0U; index < 16U; ++index) {
    schedule[index] =
        (static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(block[index * 4U]))
         << 24U) |
        (static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(block[index * 4U + 1U]))
         << 16U) |
        (static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(block[index * 4U + 2U]))
         << 8U) |
        static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(block[index * 4U + 3U]));
  }
  for (std::size_t index = 16U; index < schedule.size(); ++index) {
    const std::uint32_t before15 = schedule[index - 15U];
    const std::uint32_t before2 = schedule[index - 2U];
    const std::uint32_t sigma0 = rotate_right(before15, 7U) ^
                                 rotate_right(before15, 18U) ^ (before15 >> 3U);
    const std::uint32_t sigma1 = rotate_right(before2, 17U) ^
                                 rotate_right(before2, 19U) ^ (before2 >> 10U);
    schedule[index] =
        schedule[index - 16U] + sigma0 + schedule[index - 7U] + sigma1;
  }

  std::uint32_t a = state_[0U];
  std::uint32_t b = state_[1U];
  std::uint32_t c = state_[2U];
  std::uint32_t d = state_[3U];
  std::uint32_t e = state_[4U];
  std::uint32_t f = state_[5U];
  std::uint32_t g = state_[6U];
  std::uint32_t h = state_[7U];
  for (std::size_t index = 0U; index < schedule.size(); ++index) {
    const std::uint32_t sum1 =
        rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temporary1 =
        h + sum1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t sum0 =
        rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  state_[0U] += a;
  state_[1U] += b;
  state_[2U] += c;
  state_[3U] += d;
  state_[4U] += e;
  state_[5U] += f;
  state_[6U] += g;
  state_[7U] += h;
}

namespace {

/**
 * @brief Returns the exact canonical execution-profile token.
 * @param profile Candidate closed enum.
 * @return Closed execution-profile literal.
 * @throws std::invalid_argument for an invalid enum representation.
 */
std::string_view execution_profile_token(JobExecutionProfile profile) {
  switch (profile) {
    case JobExecutionProfile::EmbeddedCpuV1:
      return "embedded-cpu-v1";
  }
  throw std::invalid_argument("Job execution profile is invalid");
}

/**
 * @brief Returns the exact canonical durability token.
 * @param durability Candidate closed enum.
 * @return Crash-durable literal.
 * @throws std::invalid_argument for an invalid enum representation.
 */
std::string_view durability_token(ArtifactDurability durability) {
  switch (durability) {
    case ArtifactDurability::CrashDurable:
      return "crash-durable";
  }
  throw std::invalid_argument("artifact durability is invalid");
}

/**
 * @brief Appends one decimal-length-framed canonical field.
 * @param field Borrowed exact field bytes.
 * @param output Non-null canonical output owner.
 * @return Nothing.
 * @throws std::invalid_argument when `output` is null.
 * @throws std::bad_alloc when growing output exhausts memory.
 */
void append_frame(std::string_view field, std::string* output) {
  if (output == nullptr) {
    throw std::invalid_argument("canonical JobSpec output is null");
  }
  output->append(std::to_string(field.size()));
  output->push_back(':');
  output->append(field.data(), field.size());
}

/**
 * @brief Builds the exact supported canonical JobSpec byte sequence.
 * @param graph Immutable graph artifact identity.
 * @param target_node Nonnegative node selector.
 * @param output_slot Required image output slot.
 * @param resources Complete canonical quota demand.
 * @param checkpoint Optional durable checkpoint identity.
 * @param execution_profile Closed execution profile.
 * @param requested_durability Closed requested durability.
 * @return Version token followed by all scalar, device, and checkpoint fields
 * using decimal-length framing and canonical integer spellings.
 * @throws std::invalid_argument for any invalid contract field.
 * @throws std::bad_alloc when output construction exhausts memory.
 */
std::string canonical_job_spec(const GraphArtifactId& graph, int target_node,
                               const OutputSlotId& output_slot,
                               const JobResourceRequest& resources,
                               const std::optional<ArtifactId>& checkpoint,
                               JobExecutionProfile execution_profile,
                               ArtifactDurability requested_durability) {
  if (!graph.valid() || !output_slot.valid() || target_node < 0) {
    throw std::invalid_argument("JobSpec contains an invalid field");
  }
  validate_job_resource_request(resources);
  if (checkpoint.has_value() && !checkpoint->valid()) {
    throw std::invalid_argument("JobSpec checkpoint identity is invalid");
  }
  std::string result{kJobSpecVersion};
  append_frame(graph.value(), &result);
  append_frame(std::to_string(target_node), &result);
  append_frame(output_slot.value(), &result);
  append_frame(execution_profile_token(execution_profile), &result);
  append_frame(durability_token(requested_durability), &result);
  append_frame(std::to_string(resources.cpu_slots), &result);
  append_frame(std::to_string(resources.host_memory_bytes), &result);
  append_frame(std::to_string(resources.output_bytes), &result);
  append_frame(std::to_string(resources.staging_bytes), &result);
  append_frame(std::to_string(resources.retention_bytes), &result);
  append_frame(std::to_string(resources.devices.size()), &result);
  for (const DeviceResourceRequest& device : resources.devices) {
    append_frame(device.device_id, &result);
    append_frame(std::to_string(device.bytes), &result);
  }
  append_frame(checkpoint.has_value() ? "1" : "0", &result);
  append_frame(
      checkpoint.has_value() ? checkpoint->value() : std::string_view{},
      &result);
  return result;
}

/**
 * @brief Validates one configured device label without allocating.
 * @param text Candidate bounded opaque ASCII token.
 * @return True only when the token uses the Job identity alphabet and is not a
 * dot path component.
 * @throws Nothing.
 */
bool valid_device_id(std::string_view text) noexcept {
  if (text.empty() || text == "." || text == ".." ||
      text.size() > kMaximumOpaqueIdentityBytes) {
    return false;
  }
  for (const unsigned char character : text) {
    const bool alpha_numeric = (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z') ||
                               (character >= '0' && character <= '9');
    if (!alpha_numeric && character != '-' && character != '_' &&
        character != '.' && character != ':') {
      return false;
    }
  }
  return true;
}

/**
 * @brief Hashes one borrowed byte range using the internal SHA-256 engine.
 * @param bytes Borrowed bytes, null only when size is zero.
 * @param size Number of bytes.
 * @return Exact 32-byte digest.
 * @throws std::invalid_argument for null nonempty input.
 * @throws std::overflow_error when the encoded bit length would overflow.
 * @throws std::logic_error if the internal single-use lifecycle is violated.
 */
std::array<std::byte, 32U> sha256(const std::byte* bytes, std::size_t size) {
  ArtifactContentHasher hash;
  hash.update(bytes, size);
  return hash.finish().bytes;
}

}  // namespace

/** @copydoc ps::server::AttemptIdentity::operator== */
bool AttemptIdentity::operator==(const AttemptIdentity& other) const noexcept {
  return tenant_id == other.tenant_id && job_id == other.job_id &&
         job_spec_digest == other.job_spec_digest &&
         attempt_id == other.attempt_id &&
         worker_instance_id == other.worker_instance_id &&
         worker_lease_generation == other.worker_lease_generation;
}

/** @copydoc ps::server::JobSpec::JobSpec */
JobSpec::JobSpec(GraphArtifactId graph_artifact_id, int target_node,
                 OutputSlotId output_slot_id,
                 JobResourceRequest resource_request,
                 std::optional<ArtifactId> checkpoint_artifact_id,
                 JobExecutionProfile execution_profile,
                 ArtifactDurability requested_durability)
    : graph_artifact_id_(std::move(graph_artifact_id)),
      target_node_(target_node),
      output_slot_id_(std::move(output_slot_id)),
      resource_request_(std::move(resource_request)),
      checkpoint_artifact_id_(std::move(checkpoint_artifact_id)),
      execution_profile_(execution_profile),
      requested_durability_(requested_durability),
      canonical_bytes_(canonical_job_spec(
          graph_artifact_id_, target_node_, output_slot_id_, resource_request_,
          checkpoint_artifact_id_, execution_profile_, requested_durability_)),
      digest_(hash_job_spec_bytes(
          reinterpret_cast<const std::byte*>(canonical_bytes_.data()),
          canonical_bytes_.size())) {}

/** @copydoc ps::server::ArtifactImageDescriptor::operator== */
bool ArtifactImageDescriptor::operator==(
    const ArtifactImageDescriptor& other) const noexcept {
  return width == other.width && height == other.height &&
         channels == other.channels && type == other.type &&
         row_bytes == other.row_bytes && payload_bytes == other.payload_bytes;
}

/** @copydoc ps::server::hash_job_spec_bytes */
JobSpecDigest hash_job_spec_bytes(const std::byte* bytes, std::size_t size) {
  JobSpecDigest digest;
  digest.bytes = sha256(bytes, size);
  return digest;
}

/** @copydoc ps::server::hash_artifact_content */
ArtifactContentDigest hash_artifact_content(const std::byte* bytes,
                                            std::size_t size) {
  ArtifactContentDigest digest;
  digest.bytes = sha256(bytes, size);
  return digest;
}

/** @copydoc ps::server::hash_image_artifact_content */
ArtifactContentDigest hash_image_artifact_content(const ImageBuffer& image) {
  validate_image_buffer(image);
  if (image.device != Device::CPU || image.width <= 0 || image.height <= 0 ||
      image.channels <= 0 || image.data == nullptr) {
    throw std::invalid_argument(
        "artifact image hashing requires nonempty CPU data");
  }
  const std::size_t row_bytes = image_buffer_row_bytes(image);
  if (row_bytes > std::numeric_limits<std::size_t>::max() /
                      static_cast<std::size_t>(image.height)) {
    throw std::overflow_error("artifact image hash size overflowed");
  }
  ArtifactContentHasher hash;
  for (int row = 0; row < image.height; ++row) {
    hash.update(image_buffer_row_data(image, row), row_bytes);
  }
  return hash.finish();
}

/** @copydoc ps::server::validate_attempt_identity */
void validate_attempt_identity(const AttemptIdentity& identity) {
  if (!identity.tenant_id.valid() || !identity.job_id.valid() ||
      !identity.attempt_id.valid() || !identity.worker_instance_id.valid() ||
      identity.worker_lease_generation.value == 0U) {
    throw std::invalid_argument("attempt identity tuple is incomplete");
  }
}

/** @copydoc ps::server::validate_job_spec */
void validate_job_spec(const JobSpec& spec) {
  const std::string canonical = canonical_job_spec(
      spec.graph_artifact_id(), spec.target_node(), spec.output_slot_id(),
      spec.resource_request(), spec.checkpoint_artifact_id(),
      spec.execution_profile(), spec.requested_durability());
  if (canonical != spec.canonical_bytes()) {
    throw std::invalid_argument("JobSpec canonical bytes do not match fields");
  }
  const JobSpecDigest digest = hash_job_spec_bytes(
      reinterpret_cast<const std::byte*>(canonical.data()), canonical.size());
  if (digest != spec.digest()) {
    throw std::invalid_argument(
        "JobSpec digest does not match canonical bytes");
  }
}

/** @copydoc ps::server::validate_job_resource_request */
void validate_job_resource_request(const JobResourceRequest& request) {
  if (request.cpu_slots == 0U || request.host_memory_bytes == 0U ||
      request.output_bytes == 0U || request.staging_bytes == 0U ||
      request.retention_bytes == 0U) {
    throw std::invalid_argument(
        "Job resource request contains a zero required bound");
  }
  if (request.devices.size() > kMaximumConfiguredDevicesPerJob) {
    throw std::invalid_argument(
        "Job configured-device request count exceeds the supported maximum");
  }
  std::string_view previous;
  for (const DeviceResourceRequest& device : request.devices) {
    if (!valid_device_id(device.device_id) || device.bytes == 0U ||
        (!previous.empty() && previous >= device.device_id)) {
      throw std::invalid_argument(
          "Job configured-device requests are invalid or not canonical");
    }
    previous = device.device_id;
  }
}

}  // namespace ps::server
