#include "photospider/data/icc_profile.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "data/content_digest.hpp"
#include "data/icc_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps {
namespace {
// RFC 1321 arithmetic for the ICC header's optional Profile ID only. Resource
// identity always uses SHA-256 of the actual complete bytes, without masking.
class Md5 {
 public:
  void bytes(const std::uint8_t* data, std::size_t size) noexcept {
    length_ += size;
    while (size) {
      const auto count = std::min(size, 64 - used_);
      std::memcpy(block_.data() + used_, data, count);
      used_ += count;
      data += count;
      size -= count;
      if (used_ == 64) {
        compress();
        used_ = 0;
      }
    }
  }
  std::array<std::uint8_t, 16> finish() const noexcept {
    auto copy = *this;
    const std::uint64_t bits = length_ * 8;
    const std::uint8_t one = 128, zero = 0;
    copy.bytes(&one, 1);
    while (copy.used_ != 56)
      copy.bytes(&zero, 1);
    std::array<std::uint8_t, 8> tail{};
    for (unsigned i = 0; i < 8; ++i)
      tail[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    copy.bytes(tail.data(), tail.size());
    std::array<std::uint8_t, 16> result{};
    for (unsigned i = 0; i < 16; ++i)
      result[i] =
          static_cast<std::uint8_t>(copy.state_[i / 4] >> (8 * (i % 4)));
    return result;
  }

 private:
  void compress() noexcept {
    constexpr std::array<std::uint32_t, 64> constants{
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    constexpr std::array<unsigned, 16> shifts{7, 12, 17, 22, 5, 9,  14, 20,
                                              4, 11, 16, 23, 6, 10, 15, 21};
    std::array<std::uint32_t, 16> words{};
    for (unsigned i = 0; i < 64; ++i)
      words[i / 4] |= static_cast<std::uint32_t>(block_[i]) << (8 * (i % 4));
    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (unsigned i = 0; i < 64; ++i) {
      const auto round = i / 16;
      const auto function = round == 0   ? (b & c) | (~b & d)
                            : round == 1 ? (d & b) | (~d & c)
                            : round == 2 ? b ^ c ^ d
                                         : c ^ (b | ~d);
      const unsigned index = round == 0   ? i
                             : round == 1 ? (5 * i + 1) % 16
                             : round == 2 ? (3 * i + 5) % 16
                                          : (7 * i) % 16;
      const auto sum = a + function + constants[i] + words[index];
      const auto shift = shifts[4 * round + i % 4];
      const auto next = b + ((sum << shift) | (sum >> (32 - shift)));
      a = d;
      d = c;
      c = b;
      b = next;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
  }
  std::array<std::uint32_t, 4> state_{0x67452301, 0xefcdab89, 0x98badcfe,
                                      0x10325476};
  std::array<std::uint8_t, 64> block_{};
  std::size_t used_ = 0;
  std::uint64_t length_ = 0;
};
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message, FailureReason::InvalidDomain};
}
Status profile_id(ByteView bytes,
                  const std::function<Status(std::uint64_t)>& consume) {
  if (bytes[8] != 4 || std::all_of(bytes.begin() + 84, bytes.begin() + 100,
                                   [](auto byte) { return byte == 0; }))
    return Status::success();
  const bool legacy = (bytes[9] >> 4) == 0;
  // ICC v4.0 named Header attributes in its MD5 framing; later editions name
  // profile flags. Accept either documented framing for v4.0 only. The actual
  // resource identity still distinguishes every byte, including these fields.
  for (unsigned variant = 0; variant < (legacy ? 2U : 1U); ++variant) {
    Md5 hash;
    std::array<std::uint8_t, 128> header;
    std::memcpy(header.data(), bytes.data(), 128);
    if (!variant)
      std::fill(header.begin() + 44, header.begin() + 48, 0);
    else
      std::fill(header.begin() + 56, header.begin() + 64, 0);
    std::fill(header.begin() + 64, header.begin() + 68, 0);
    std::fill(header.begin() + 84, header.begin() + 100, 0);
    auto status = consume(128);
    if (!status.ok())
      return status;
    hash.bytes(header.data(), header.size());
    for (std::size_t offset = 128; offset < bytes.size();) {
      const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
      status = consume(count);
      if (!status.ok())
        return status;
      hash.bytes(bytes.data() + offset, count);
      offset += count;
    }
    const auto result = hash.finish();
    if (std::equal(result.begin(), result.end(), bytes.begin() + 84))
      return Status::success();
  }
  return invalid("ICC Profile ID checksum mismatch");
}
}  // namespace
struct IccProfile::Impl {
  std::shared_ptr<const CpuStorage> storage;
  ColorProfileIdentity identity;
};
Result<IccProfile> IccProfile::import(ByteView bytes,
                                      const ResourceBudget& resources,
                                      const CancellationToken& cancellation,
                                      std::uint64_t maximum_work) {
  const auto consume = [&](std::uint64_t count) -> Status {
    if (cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    if (count > maximum_work)
      return {ErrorCode::ResourceExhausted, "ICC import work limit",
              FailureReason::WorkLimit};
    maximum_work -= count;
    return resources.consume({count});
  };
  auto status = consume(0);
  if (!status.ok())
    return Result<IccProfile>(status);
  if (!bytes.data() || bytes.size() < 132 || bytes.size() > UINT32_MAX)
    return Result<IccProfile>(invalid("invalid ICC byte range"));
  std::shared_ptr<Impl> impl;
  try {
    impl = std::allocate_shared<Impl>(ResourceAllocator<Impl>(resources));
  } catch (const std::bad_alloc&) {
    return Result<IccProfile>(Status{ErrorCode::ResourceExhausted,
                                     "ICC owner metadata capacity",
                                     FailureReason::CapacityLimit});
  }
  // The temporary SHA-256 hexadecimal string has at most 128 bytes of capacity
  // on supported Clang libraries. Its lease is retired after the string.
  auto scratch = resources.reserve(ResourceCapacity::host(128, 128));
  if (!scratch.ok())
    return Result<IccProfile>(scratch.status());
  auto buffer = resources.allocator().allocate(bytes.size());
  if (!buffer.ok())
    return Result<IccProfile>(buffer.status());
  auto writer = buffer.take_value();
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
    status = consume(count);
    if (!status.ok())
      return Result<IccProfile>(status);
    std::memcpy(writer.data() + offset, bytes.data() + offset, count);
    offset += count;
  }
  auto storage = std::move(writer).freeze();
  bytes = storage->bytes();
  status = color_internal::validate_icc(bytes, resources, consume);
  if (!status.ok())
    return Result<IccProfile>(status);
  status = profile_id(bytes, consume);
  if (!status.ok())
    return Result<IccProfile>(status);
  content_internal::Sha256 hash;
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
    status = consume(count);
    if (!status.ok())
      return Result<IccProfile>(status);
    hash.bytes(bytes.data() + offset, count);
    offset += count;
  }
  const auto digest = hash.finish();
  impl->storage = std::move(storage);
  impl->identity.byte_length = bytes.size();
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (unsigned i = 0; i < 32; ++i)
    impl->identity.sha256[i] = static_cast<std::uint8_t>(
        16 * digit(digest[2 * i]) + digit(digest[2 * i + 1]));
  status = consume(1);
  if (!status.ok())
    return Result<IccProfile>(status);
  return Result<IccProfile>(IccProfile(std::move(impl)));
}
const ColorProfileIdentity& IccProfile::identity() const {
  if (!impl_)
    throw std::logic_error("invalid ICC resource");
  return impl_->identity;
}
ByteView IccProfile::bytes() const {
  if (!impl_)
    throw std::logic_error("invalid ICC resource");
  return impl_->storage->bytes();
}
std::string IccProfile::model() const {
  auto data = bytes();
  const std::string signature(reinterpret_cast<const char*>(data.data() + 16),
                              4);
  if (signature == "RGB ")
    return "rgb";
  if (signature == "GRAY")
    return "gray";
  if (signature == "XYZ ")
    return "xyz";
  if (signature == "Lab ")
    return "cielab";
  return "cmyk";
}
const std::shared_ptr<const CpuStorage>& IccProfile::storage() const {
  if (!impl_)
    throw std::logic_error("invalid ICC resource");
  return impl_->storage;
}
Result<IccProfile> IccProfile::reference(
    const ResourceBudget& resources) const {
  if (!impl_)
    return Result<IccProfile>(invalid("invalid ICC resource"));
  std::shared_ptr<Impl> impl;
  try {
    impl = std::allocate_shared<Impl>(ResourceAllocator<Impl>(resources));
  } catch (const std::bad_alloc&) {
    return Result<IccProfile>(Status{ErrorCode::ResourceExhausted,
                                     "ICC owner metadata capacity",
                                     FailureReason::CapacityLimit});
  }
  auto reference = resources.reference(impl_->storage);
  if (!reference.ok())
    return Result<IccProfile>(reference.status());
  impl->storage = reference.take_value();
  impl->identity = impl_->identity;
  return Result<IccProfile>(IccProfile(std::move(impl)));
}
}  // namespace ps
