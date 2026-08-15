#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/extension.hpp"
#include "photospider/data/value.hpp"

namespace ps {
namespace {

/**
 * @brief Creates a Ready unsigned-byte tensor with caller-selected layout.
 * @param shape Positive logical shape in canonical axis order.
 * @param strides Positive byte strides for the producer allocation.
 * @param bytes Exact physical allocation envelope, including padding.
 * @param image_facet Optional explicit logical image-axis interpretation.
 * @return Fresh immutable CPU DenseTensor Value.
 * @throws Value publication and allocation errors unchanged.
 */
Value make_u8_tensor(std::vector<std::size_t> shape,
                     std::vector<std::ptrdiff_t> strides,
                     std::vector<std::byte> bytes,
                     std::optional<ImageFacet> image_facet = std::nullopt) {
  DenseTensorDescriptor descriptor;
  descriptor.shape = std::move(shape);
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(image_facet),
      StridedLayout{std::move(strides)}, std::move(bytes));
}

/**
 * @brief Converts one typed digest to conventional lowercase hexadecimal.
 * @param digest Exact SHA-256 canonical-v1 identity.
 * @return Sixty-four lowercase hexadecimal characters.
 * @throws std::bad_alloc when result storage cannot allocate.
 */
std::string digest_hex(const ContentDigest& digest) {
  constexpr std::array<char, 16U> kHexDigits{'0', '1', '2', '3', '4', '5',
                                             '6', '7', '8', '9', 'a', 'b',
                                             'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(kCanonicalDigestBytes * 2U);
  for (const std::byte value : digest.bytes) {
    const std::uint8_t byte = static_cast<std::uint8_t>(value);
    result.push_back(kHexDigits[byte >> 4U]);
    result.push_back(kHexDigits[byte & 0x0fU]);
  }
  return result;
}

/**
 * @brief Proves canonical DenseTensor content ignores physical row padding.
 * @throws Nothing when canonical traversal and typed state remain exact.
 */
TEST(DenseTensorContentDigest, LogicalRowMajorBytesIgnorePhysicalPadding) {
  const Value contiguous =
      make_u8_tensor({2U, 3U}, {3, 1},
                     {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                      std::byte{5}, std::byte{6}});
  const Value padded =
      make_u8_tensor({2U, 3U}, {4, 1},
                     {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{0xff},
                      std::byte{4}, std::byte{5}, std::byte{6}});

  const ContentDigestResult contiguous_digest =
      compute_content_digest(contiguous);
  const ContentDigestResult padded_digest = compute_content_digest(padded);
  ASSERT_EQ(contiguous_digest.state, ContentDigestState::Available)
      << contiguous_digest.diagnostic;
  ASSERT_EQ(padded_digest.state, ContentDigestState::Available)
      << padded_digest.diagnostic;
  ASSERT_TRUE(contiguous_digest.digest.has_value());
  ASSERT_TRUE(padded_digest.digest.has_value());
  EXPECT_EQ(contiguous_digest.digest->algorithm,
            CanonicalDigestAlgorithm::Sha256CanonicalV1);
  EXPECT_EQ(*contiguous_digest.digest, *padded_digest.digest);
  EXPECT_EQ(digest_hex(*contiguous_digest.digest),
            "f6afc72b6e52e27b991da46eac0013ff50e3966b1bff5b06468fc966986125d3");
}

/**
 * @brief Proves logical descriptor and ImageFacet facts enter the identity.
 * @throws Nothing when descriptor/facet distinctions yield distinct digests.
 */
TEST(DenseTensorContentDigest, DescriptorAndFacetRemainLogicalIdentity) {
  const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{3},
                                     std::byte{4}, std::byte{5}, std::byte{6}};
  const Value two_by_three = make_u8_tensor({2U, 3U}, {3, 1}, bytes);
  const Value three_by_two = make_u8_tensor({3U, 2U}, {2, 1}, bytes);
  const Value image =
      make_u8_tensor({2U, 3U}, {3, 1}, bytes,
                     ImageFacet{1U, 0U, std::optional<std::size_t>{}});

  const ContentDigestResult plain_digest = compute_content_digest(two_by_three);
  const ContentDigestResult reshaped_digest =
      compute_content_digest(three_by_two);
  const ContentDigestResult image_digest = compute_content_digest(image);
  ASSERT_TRUE(plain_digest.digest.has_value()) << plain_digest.diagnostic;
  ASSERT_TRUE(reshaped_digest.digest.has_value()) << reshaped_digest.diagnostic;
  ASSERT_TRUE(image_digest.digest.has_value()) << image_digest.diagnostic;
  EXPECT_FALSE(*plain_digest.digest == *reshaped_digest.digest);
  EXPECT_FALSE(*plain_digest.digest == *image_digest.digest);
}

/**
 * @brief Preserves fail-closed typed state for an invalid Value handle.
 * @throws Nothing.
 */
TEST(DenseTensorContentDigest, InvalidValueHasNoDigest) {
  const ContentDigestResult result = compute_content_digest(Value{});
  EXPECT_EQ(result.state, ContentDigestState::InvalidDescriptor);
  EXPECT_FALSE(result.digest.has_value());
}

}  // namespace
}  // namespace ps
