/**
 * @file dense_tensor_content_digest.cpp
 * @brief Implements canonical logical content identity for built-in tensors.
 */
#include "core/dense_tensor_content_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/extension_internal.hpp"
#include "photospider/data/packed_dense_tensor_view.hpp"
#include "photospider/data/value.hpp"

namespace ps::internal {
namespace {

/** @brief Permanent built-in canonical descriptor identity for DenseTensor. */
constexpr ExtensionIdentity kDenseTensorCanonicalSchemaIdentity{
    0x70686f746f737069ULL,
    0x6465722d64656e73ULL};  // NOLINT(whitespace/indent_namespace)

/** @brief Permanent built-in canonical facet identity for ImageFacet. */
constexpr ExtensionIdentity kImageFacetCanonicalIdentity{
    0x70686f746f737069ULL,
    0x6465722d696d6167ULL};  // NOLINT(whitespace/indent_namespace)

/** @brief Exact structural version of both built-in canonical records. */
constexpr std::uint32_t kDenseCanonicalRecordVersion = 1U;

/**
 * @brief Appends one byte to a bounded canonical record payload.
 * @param output Destination payload.
 * @param value Exact byte value.
 * @return Nothing.
 * @throws std::bad_alloc when vector growth cannot allocate.
 */
void append_u8(std::vector<std::byte>* output, std::uint8_t value) {
  output->push_back(static_cast<std::byte>(value));
}

/**
 * @brief Appends one unsigned 32-bit value in little-endian order.
 * @param output Destination payload.
 * @param value Exact scalar value.
 * @return Nothing.
 * @throws std::bad_alloc when vector growth cannot allocate.
 */
void append_u32(std::vector<std::byte>* output, std::uint32_t value) {
  for (unsigned int byte = 0U; byte < 4U; ++byte) {
    append_u8(output,
              static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
  }
}

/**
 * @brief Appends one unsigned 64-bit value in little-endian order.
 * @param output Destination payload.
 * @param value Exact scalar value.
 * @return Nothing.
 * @throws std::bad_alloc when vector growth cannot allocate.
 */
void append_u64(std::vector<std::byte>* output, std::uint64_t value) {
  for (unsigned int byte = 0U; byte < 8U; ++byte) {
    append_u8(output,
              static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
  }
}

/**
 * @brief Converts one size_t metadata scalar to the frozen uint64 domain.
 * @param value Valid in-memory extent or axis index.
 * @return Exact uint64 value.
 * @throws std::overflow_error on platforms where size_t can exceed uint64.
 */
std::uint64_t size_to_u64(std::size_t value) {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(
          "DenseTensor canonical metadata exceeds uint64.");
    }
  }
  return static_cast<std::uint64_t>(value);
}

/**
 * @brief Converts one bounded metadata count to the frozen uint32 domain.
 * @param value Rank/count already representable by size_t.
 * @return Exact uint32 value.
 * @throws std::overflow_error when the count exceeds uint32.
 */
std::uint32_t size_to_u32(std::size_t value) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error(
        "DenseTensor canonical metadata count exceeds uint32.");
  }
  return static_cast<std::uint32_t>(value);
}

/**
 * @brief Multiplies two uint64 logical sizes without wraparound.
 * @param left First nonnegative factor.
 * @param right Second nonnegative factor.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds uint64.
 */
std::uint64_t checked_u64_multiply(std::uint64_t left, std::uint64_t right) {
  if (right != 0U && left > std::numeric_limits<std::uint64_t>::max() / right) {
    throw std::overflow_error(
        "DenseTensor canonical logical byte count overflowed.");
  }
  return left * right;
}

/**
 * @brief Returns the exact number of logical tensor elements.
 * @param descriptor Valid positive-rank DenseTensor descriptor.
 * @return Checked product of every shape extent.
 * @throws std::overflow_error when the product exceeds uint64.
 */
std::uint64_t logical_element_count(const DenseTensorDescriptor& descriptor) {
  std::uint64_t count = 1U;
  for (const std::size_t extent : descriptor.shape) {
    count = checked_u64_multiply(count, size_to_u64(extent));
  }
  return count;
}

/**
 * @brief Encodes the built-in logical DenseTensor descriptor record.
 * @param descriptor Valid immutable tensor descriptor.
 * @return Version-1 Schema record with exact little-endian payload fields.
 * @throws std::overflow_error when metadata exceeds frozen scalar domains.
 * @throws std::bad_alloc when payload ownership cannot allocate.
 * @note Field order is rank, shapes, element semantics, encoding kind/width,
 * quantization presence, then optional block shapes and binary32 scale bits.
 */
ExtensionRecord dense_tensor_schema_record(
    const DenseTensorDescriptor& descriptor) {
  ExtensionRecord record;
  record.kind = ExtensionDefinitionKind::Schema;
  record.identity = kDenseTensorCanonicalSchemaIdentity;
  record.structural_version = kDenseCanonicalRecordVersion;
  append_u32(&record.payload, size_to_u32(descriptor.shape.size()));
  for (const std::size_t extent : descriptor.shape) {
    append_u64(&record.payload, size_to_u64(extent));
  }
  append_u32(&record.payload,
             static_cast<std::uint32_t>(descriptor.element_semantics));
  append_u32(&record.payload,
             static_cast<std::uint32_t>(descriptor.storage_encoding.kind));
  append_u32(&record.payload, descriptor.storage_encoding.bit_width);
  append_u8(&record.payload, descriptor.quantization.has_value() ? 1U : 0U);
  if (descriptor.quantization.has_value()) {
    const QuantizationSchema& quantization = *descriptor.quantization;
    append_u32(&record.payload, size_to_u32(quantization.block_shape.size()));
    for (const std::size_t extent : quantization.block_shape) {
      append_u64(&record.payload, size_to_u64(extent));
    }
    append_u64(&record.payload, size_to_u64(quantization.scales.size()));
    for (const float scale : quantization.scales) {
      std::uint32_t bits = 0U;
      static_assert(sizeof(bits) == sizeof(scale),
                    "binary32 canonical encoding requires 32-bit float");
      std::memcpy(&bits, &scale, sizeof(bits));
      append_u32(&record.payload, bits);
    }
  }
  return record;
}

/**
 * @brief Encodes the optional built-in logical ImageFacet record.
 * @param facet Valid explicit image-axis assignment.
 * @return Version-1 Facet record with exact little-endian payload fields.
 * @throws std::overflow_error when an axis exceeds uint64.
 * @throws std::bad_alloc when payload ownership cannot allocate.
 */
ExtensionRecord image_facet_record(const ImageFacet& facet) {
  ExtensionRecord record;
  record.kind = ExtensionDefinitionKind::Facet;
  record.identity = kImageFacetCanonicalIdentity;
  record.structural_version = kDenseCanonicalRecordVersion;
  append_u64(&record.payload, size_to_u64(facet.x_axis));
  append_u64(&record.payload, size_to_u64(facet.y_axis));
  append_u8(&record.payload, facet.channel_axis.has_value() ? 1U : 0U);
  append_u64(&record.payload, facet.channel_axis.has_value()
                                  ? size_to_u64(*facet.channel_axis)
                                  : 0U);
  return record;
}

/**
 * @brief Builds the provider-independent canonical descriptor envelope.
 * @param value Valid DenseTensor value.
 * @return Built-in Schema plus optional ImageFacet record.
 * @throws Descriptor encoding errors unchanged.
 * @note No physical layout, binding, readiness, or revision enters the value.
 */
DataDescriptorEnvelope dense_descriptor_envelope(const Value& value) {
  DataDescriptorEnvelope envelope;
  envelope.schema = dense_tensor_schema_record(value.dense_tensor_descriptor());
  if (value.image_facet().has_value()) {
    envelope.facets.push_back(image_facet_record(*value.image_facet()));
  }
  return envelope;
}

/**
 * @brief Detects the process native byte order without type punning.
 * @return True when the least-significant byte is stored first.
 * @throws Nothing.
 */
bool host_is_little_endian() noexcept {
  const std::uint16_t marker = 1U;
  std::uint8_t first = 0U;
  std::memcpy(&first, &marker, sizeof(first));
  return first == 1U;
}

/**
 * @brief Advances a row-major coordinate prefix and resets wrapped axes.
 * @param coordinates Mutable complete coordinate vector.
 * @param shape Positive complete tensor shape.
 * @param prefix_axes Number of leading axes to advance.
 * @return True after advancing, false after the final prefix wraps.
 * @throws Nothing when ranks and coordinates are already valid.
 */
bool advance_coordinate_prefix(std::vector<std::size_t>* coordinates,
                               const std::vector<std::size_t>& shape,
                               std::size_t prefix_axes) noexcept {
  for (std::size_t reverse = prefix_axes; reverse > 0U; --reverse) {
    const std::size_t axis = reverse - 1U;
    ++(*coordinates)[axis];
    if ((*coordinates)[axis] < shape[axis]) {
      return true;
    }
    (*coordinates)[axis] = 0U;
  }
  return false;
}

/**
 * @brief Finds the maximal physically contiguous canonical logical suffix.
 * @param descriptor Valid tensor descriptor.
 * @param layout Valid signed strided layout.
 * @param element_bytes Positive native element width.
 * @return Pair of first suffix axis and total contiguous suffix byte count.
 * @throws std::overflow_error when suffix bytes exceed size_t.
 * @note A return first-axis equal to rank means one-element chunks.
 */
std::pair<std::size_t, std::size_t> contiguous_suffix(
    const DenseTensorDescriptor& descriptor, const StridedLayout& layout,
    std::size_t element_bytes) {
  std::size_t first_axis = descriptor.shape.size();
  std::size_t bytes = element_bytes;
  for (std::size_t reverse = descriptor.shape.size(); reverse > 0U; --reverse) {
    const std::size_t axis = reverse - 1U;
    if (bytes > static_cast<std::size_t>(
                    std::numeric_limits<std::ptrdiff_t>::max()) ||
        layout.byte_strides[axis] != static_cast<std::ptrdiff_t>(bytes)) {
      break;
    }
    first_axis = axis;
    if (descriptor.shape[axis] != 0U &&
        bytes >
            std::numeric_limits<std::size_t>::max() / descriptor.shape[axis]) {
      throw std::overflow_error(
          "DenseTensor canonical contiguous suffix overflowed.");
    }
    bytes *= descriptor.shape[axis];
  }
  return {first_axis, bytes};
}

/**
 * @brief Hashes one Strided tensor in row-major logical-axis order.
 * @param value Valid Ready host-visible Strided DenseTensor.
 * @param descriptor_digest Canonical built-in logical descriptor identity.
 * @param logical_elements Checked number of logical elements.
 * @return Canonical SHA-256 content identity.
 * @throws View, arithmetic, digest, and allocation errors unchanged.
 * @note Little-endian hosts stream maximal contiguous suffixes; big-endian
 * hosts reverse each native scalar into the frozen little-endian byte order.
 */
ContentDigest digest_strided_tensor(const Value& value,
                                    const DescriptorDigest& descriptor_digest,
                                    std::uint64_t logical_elements) {
  DenseTensorView view(value);
  const DenseTensorDescriptor& descriptor = view.descriptor();
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  const std::uint64_t content_bytes = checked_u64_multiply(
      logical_elements, static_cast<std::uint64_t>(element_bytes));
  CanonicalContentDigestWriter writer(descriptor_digest, content_bytes);
  std::vector<std::size_t> coordinates(descriptor.shape.size(), 0U);

  if (host_is_little_endian()) {
    const auto [suffix_axis, suffix_bytes] =
        contiguous_suffix(descriptor, view.layout(), element_bytes);
    do {
      writer.append(view.element_data(coordinates), suffix_bytes);
    } while (
        advance_coordinate_prefix(&coordinates, descriptor.shape, suffix_axis));
  } else {
    std::array<std::byte, 8U> canonical{};
    do {
      const std::byte* source = view.element_data(coordinates);
      std::reverse_copy(source, source + element_bytes, canonical.begin());
      writer.append(canonical.data(), element_bytes);
    } while (advance_coordinate_prefix(&coordinates, descriptor.shape,
                                       descriptor.shape.size()));
  }
  return writer.finish();
}

/**
 * @brief Hashes one Blocked FP4 tensor as row-major logical nibble codes.
 * @param value Valid Ready host-visible Blocked FP4 DenseTensor.
 * @param descriptor_digest Canonical built-in logical descriptor identity.
 * @param logical_elements Checked number of logical elements.
 * @return Canonical SHA-256 content identity.
 * @throws View, arithmetic, digest, and allocation errors unchanged.
 * @note Physical block strides, bit order, offsets, and unused bits are
 * excluded; descriptor-bound block shape/scales remain in descriptor identity.
 */
ContentDigest digest_blocked_tensor(const Value& value,
                                    const DescriptorDigest& descriptor_digest,
                                    std::uint64_t logical_elements) {
  PackedDenseTensorView view(value);
  CanonicalContentDigestWriter writer(descriptor_digest, logical_elements);
  std::vector<std::size_t> coordinates(view.descriptor().shape.size(), 0U);
  do {
    const std::byte code =
        static_cast<std::byte>(view.encoded_element(coordinates));
    writer.append(&code, 1U);
  } while (advance_coordinate_prefix(&coordinates, view.descriptor().shape,
                                     view.descriptor().shape.size()));
  return writer.finish();
}

}  // namespace

/** @copydoc compute_dense_tensor_content_digest */
ContentDigestResult compute_dense_tensor_content_digest(const Value& value) {
  if (!value.valid() ||
      value.representation_kind() != ValueRepresentationKind::DenseTensor) {
    return {ContentDigestState::InvalidDescriptor, std::nullopt,
            "DenseTensor ContentDigest requires a valid built-in Value."};
  }
  if (!value.ready_fence().poll().ready()) {
    return {ContentDigestState::PayloadUnavailable, std::nullopt,
            "DenseTensor ContentDigest requires a Ready payload."};
  }
  try {
    const DescriptorDigest descriptor_digest =
        compute_descriptor_digest(dense_descriptor_envelope(value));
    const std::uint64_t elements =
        logical_element_count(value.dense_tensor_descriptor());
    ContentDigest digest;
    switch (value.storage_layout_kind()) {
      case StorageLayoutKind::Strided:
        digest = digest_strided_tensor(value, descriptor_digest, elements);
        break;
      case StorageLayoutKind::Blocked:
        digest = digest_blocked_tensor(value, descriptor_digest, elements);
        break;
      case StorageLayoutKind::ProviderDefined:
        return {ContentDigestState::InvalidDescriptor, std::nullopt,
                "DenseTensor ContentDigest received a provider Layout."};
    }
    return {ContentDigestState::Available, digest, {}};
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const BufferAccessError& error) {
    return {ContentDigestState::PayloadUnavailable, std::nullopt, error.what()};
  } catch (const ReadyFenceAccessError& error) {
    return {ContentDigestState::PayloadUnavailable, std::nullopt, error.what()};
  } catch (const std::exception& error) {
    return {ContentDigestState::InvalidDescriptor, std::nullopt, error.what()};
  }
}

}  // namespace ps::internal
