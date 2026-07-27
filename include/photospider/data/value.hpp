#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "photospider/memory/strided_layout.hpp"

/**
 * @file value.hpp
 * @brief Immutable CPU DenseTensor Value and checked tensor-view contracts.
 */

namespace ps {

/**
 * @brief Identifies the logical interpretation of one tensor element.
 *
 * @throws Nothing.
 * @note Element semantics are independent from physical bit width. V-2 does
 *       not yet model quantization, packing, byte order, or vector lanes.
 */
enum class ElementSemantics : std::uint32_t {
  /** @brief Unsigned integer element. */
  UnsignedInteger = 0U,
  /** @brief Signed integer element. */
  SignedInteger = 1U,
  /** @brief IEEE-style floating-point element. */
  FloatingPoint = 2U,
};

/**
 * @brief Describes the byte-addressed storage width of one tensor element.
 *
 * @throws Nothing for ordinary value operations.
 * @note V-2 accepts unsigned/signed 8- or 16-bit integers and 32- or 64-bit
 *       floating-point values. Packed sub-byte encodings remain unsupported.
 */
struct StorageEncoding {
  /** @brief Number of physical bits occupied by one logical element. */
  std::uint32_t bit_width = 0U;

  /**
   * @brief Compares the complete physical encoding.
   *
   * @param other Encoding to compare.
   * @return True when bit widths match.
   * @throws Nothing.
   * @note Logical element semantics are stored by DenseTensorDescriptor.
   */
  bool operator==(const StorageEncoding& other) const noexcept {
    return bit_width == other.bit_width;
  }
};

/**
 * @brief Concrete logical descriptor for one CPU DenseTensor.
 *
 * @throws std::bad_alloc when copying the shape vector allocates and fails.
 * @note Shape and logical element facts do not describe physical strides,
 *       storage ownership, image axes, device routing, or readiness.
 */
struct DenseTensorDescriptor {
  /** @brief Positive concrete extent for every logical axis. */
  std::vector<std::size_t> shape;

  /** @brief Logical interpretation of each stored element. */
  ElementSemantics element_semantics = ElementSemantics::UnsignedInteger;

  /** @brief Physical byte-addressed encoding of each stored element. */
  StorageEncoding storage_encoding;

  /**
   * @brief Compares all logical DenseTensor facts.
   *
   * @param other Descriptor to compare.
   * @return True when shape, semantics, and encoding all match.
   * @throws Nothing under vector equality for size_t elements.
   * @note Physical strides and bytes are intentionally excluded.
   */
  bool operator==(const DenseTensorDescriptor& other) const noexcept {
    return shape == other.shape &&
           element_semantics == other.element_semantics &&
           storage_encoding == other.storage_encoding;
  }
};

/**
 * @brief Maps explicit image coordinates onto distinct DenseTensor axes.
 *
 * @throws Nothing for ordinary value operations.
 * @note An absent channel axis means one channel. Axis names, color roles,
 *       alpha semantics, and packing are not inferred.
 */
struct ImageFacet {
  /** @brief Logical axis used as the image x coordinate. */
  std::size_t x_axis = 0U;

  /** @brief Logical axis used as the image y coordinate. */
  std::size_t y_axis = 0U;

  /** @brief Optional logical axis used as the channel coordinate. */
  std::optional<std::size_t> channel_axis;

  /**
   * @brief Compares every explicit image-axis assignment.
   *
   * @param other Facet to compare.
   * @return True when x, y, and optional channel axes match.
   * @throws Nothing.
   * @note Tensor shape and storage layout are intentionally excluded.
   */
  bool operator==(const ImageFacet& other) const noexcept {
    return x_axis == other.x_axis && y_axis == other.y_axis &&
           channel_axis == other.channel_axis;
  }
};

/**
 * @brief Returns the validated byte width of one DenseTensor element.
 *
 * @param descriptor Descriptor whose semantics and encoding are inspected.
 * @return One, two, four, or eight bytes for a supported V-2 element.
 * @throws std::invalid_argument for an unknown semantic category or an
 *         unsupported semantic/bit-width combination.
 * @note Shape, facet, strides, and storage are not inspected.
 */
std::size_t dense_tensor_element_bytes(const DenseTensorDescriptor& descriptor);

/**
 * @brief Immutable owning handle for one validated CPU DenseTensor payload.
 *
 * Value uses a shared immutable PImpl. Copies share the same descriptor,
 * layout, facet, and owned byte envelope; no public API exposes writable
 * payload access.
 *
 * @throws Nothing for default, copy, move, assignment, and destruction.
 * @note V-2 owns CPU bytes directly. BufferHandle identity, offsets, leases,
 *       replicas, readiness, and mutable aliases remain later contracts.
 */
class Value final {
 public:
  /**
   * @brief Creates an invalid empty handle.
   *
   * @throws Nothing.
   * @note An empty handle is a transport sentinel, not a valid DenseTensor.
   */
  Value() noexcept = default;

  /**
   * @brief Publishes one exclusively owned immutable CPU DenseTensor.
   *
   * Validation checks nonempty positive shape, supported element encoding,
   * optional distinct in-rank image axes, one positive stride per axis,
   * checked envelope arithmetic, and exact storage size before publication.
   *
   * @param descriptor Concrete logical tensor descriptor.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Physical signed byte strides.
   * @param storage Exclusively owned bytes consumed by the returned Value.
   * @return Valid immutable Value sharing no mutable input storage.
   * @throws std::invalid_argument for malformed descriptors, facets, layouts,
   *         or a storage-size mismatch.
   * @throws std::overflow_error when the required address envelope cannot be
   *         represented by std::size_t.
   * @throws std::bad_alloc when immutable state allocation fails.
   * @note Validation finishes before the immutable PImpl is published.
   */
  static Value from_cpu_dense_tensor(DenseTensorDescriptor descriptor,
                                     std::optional<ImageFacet> image_facet,
                                     StridedLayout layout,
                                     std::vector<std::byte> storage);

  /**
   * @brief Reports whether this handle owns a published DenseTensor.
   *
   * @return True when immutable state is present.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the immutable logical DenseTensor descriptor.
   *
   * @return Borrowed descriptor retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   * @note The reference remains valid while this Value or one of its copies
   *       retains the shared immutable state.
   */
  const DenseTensorDescriptor& dense_tensor_descriptor() const;

  /**
   * @brief Returns the optional explicit image-axis mapping.
   *
   * @return Borrowed optional facet retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   */
  const std::optional<ImageFacet>& image_facet() const;

  /**
   * @brief Returns the immutable physical byte strides.
   *
   * @return Borrowed validated layout retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   */
  const StridedLayout& strided_layout() const;

  /**
   * @brief Returns the exact owned byte-envelope size.
   *
   * @return Number of immutable bytes retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   */
  std::size_t storage_size() const;

  /**
   * @brief Returns the start of the immutable owned CPU byte envelope.
   *
   * @return Read-only pointer retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   * @note The pointer remains valid while this Value or a retaining view lives.
   */
  const std::byte* data() const;

 private:
  /** @brief Immutable implementation containing descriptor, layout, and bytes.
   */
  struct Impl;

  /**
   * @brief Creates a handle from already validated immutable state.
   *
   * @param impl Shared state published by from_cpu_dense_tensor.
   * @throws Nothing.
   * @note The constructor is private so unvalidated state cannot be published.
   */
  explicit Value(std::shared_ptr<const Impl> impl) noexcept;

  /** @brief Shared immutable DenseTensor state, or null for an invalid handle.
   */
  std::shared_ptr<const Impl> impl_;
};

/**
 * @brief Retaining read-only, bounds-checked view of one DenseTensor Value.
 *
 * @throws std::bad_alloc only when the retained Value itself is copied by a
 *         caller-defined allocator implementation; current shared_ptr copies
 *         do not allocate.
 * @note The view stores a complete Value, so addresses outlive a caller's
 *       separate handle but never outlive the view.
 */
class DenseTensorView final {
 public:
  /**
   * @brief Retains a valid CPU DenseTensor Value.
   *
   * @param value Value copied or moved into the view.
   * @throws std::invalid_argument when value is invalid.
   * @note Value construction already validated descriptor, layout, and bytes.
   */
  explicit DenseTensorView(Value value);

  /**
   * @brief Returns the retained immutable Value.
   *
   * @return Borrowed Value handle.
   * @throws Nothing.
   */
  const Value& value() const noexcept;

  /**
   * @brief Returns the retained logical descriptor.
   *
   * @return Borrowed validated DenseTensor descriptor.
   * @throws Nothing after successful view construction.
   */
  const DenseTensorDescriptor& descriptor() const noexcept;

  /**
   * @brief Returns the retained physical layout.
   *
   * @return Borrowed validated byte strides.
   * @throws Nothing after successful view construction.
   */
  const StridedLayout& layout() const noexcept;

  /**
   * @brief Returns the exact immutable storage-envelope size.
   *
   * @return Number of retained payload bytes.
   * @throws Nothing after successful view construction.
   */
  std::size_t storage_size() const noexcept;

  /**
   * @brief Returns the immutable storage-envelope base address.
   *
   * @return Read-only retained payload pointer.
   * @throws Nothing after successful view construction.
   */
  const std::byte* data() const noexcept;

  /**
   * @brief Returns one logical element address after complete bounds checks.
   *
   * @param coordinates One coordinate for every logical axis.
   * @return Read-only pointer to the requested element's first byte.
   * @throws std::invalid_argument when coordinate rank differs from tensor
   *         rank.
   * @throws std::out_of_range when any coordinate is outside its extent.
   * @note Successful Value validation proves the computed address remains
   *       inside the retained byte envelope.
   */
  const std::byte* element_data(
      const std::vector<std::size_t>& coordinates) const;

 private:
  /** @brief Complete retained Value establishing descriptor and byte lifetime.
   */
  Value value_;
};

}  // namespace ps
