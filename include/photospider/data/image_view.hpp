#pragma once

#include <cstddef>

#include "photospider/data/value.hpp"

/**
 * @file image_view.hpp
 * @brief Explicit-axis checked image facade over an immutable DenseTensor.
 */

namespace ps {

/**
 * @brief Retaining read-only image view over one DenseTensor Value.
 *
 * ImageView requires an explicit ImageFacet, rejects guessed axes, and derives
 * every address from the validated tensor layout. Axes not named by the facet
 * must be singleton in V-2.
 *
 * @throws std::invalid_argument when construction receives an invalid or
 *         non-image Value.
 * @note Width, height, and channel count are bounded to the current
 *       ImageBuffer adapter's positive int domain for this implementation
 *       slice. No mutable access, color role, or channel role is implied.
 */
class ImageView final {
 public:
  /**
   * @brief Retains and validates one Value-backed image.
   *
   * @param value CPU DenseTensor Value with an explicit valid ImageFacet.
   * @throws std::invalid_argument for a missing facet, non-singleton unassigned
   *         axes, or extents outside the current image-adapter domain.
   * @note Descriptor, facet, layout, and byte-envelope validation has already
   *       completed at Value publication.
   */
  explicit ImageView(Value value);

  /**
   * @brief Copies an image view retaining the same immutable Value.
   *
   * @param other Valid image view whose retained Value and image metadata are
   *        shared or copied.
   * @throws Nothing.
   * @note Both views remain complete, valid, and independently assignable.
   */
  ImageView(const ImageView& other) noexcept = default;

  /**
   * @brief Copy-assigns another image view's retained Value and metadata.
   *
   * @param other Valid image view replacing the current retained state.
   * @return This complete retaining image view.
   * @throws Nothing.
   * @note Both views remain complete and valid, including during
   *       self-assignment.
   */
  ImageView& operator=(const ImageView& other) noexcept = default;

  /**
   * @brief Move-constructs an image view without invalidating the source.
   *
   * @param other Valid image view whose retained state is shared or copied.
   * @throws Nothing.
   * @note Move is intentionally copy-like because ImageView has no public
   *       invalid state. Source and destination both remain fully readable.
   */
  ImageView(ImageView&& other) noexcept : ImageView(other) {}

  /**
   * @brief Move-assigns an image view without invalidating the source.
   *
   * @param other Valid image view replacing the current retained state.
   * @return This complete retaining image view.
   * @throws Nothing.
   * @note Move assignment intentionally delegates to copy assignment. Source
   *       and destination both remain fully readable, including for self-move.
   */
  ImageView& operator=(ImageView&& other) noexcept { return *this = other; }

  /**
   * @brief Returns the retained immutable Value.
   *
   * @return Borrowed Value handle.
   * @throws Nothing.
   */
  const Value& value() const noexcept;

  /**
   * @brief Returns the retained logical tensor descriptor.
   *
   * @return Borrowed validated descriptor.
   * @throws Nothing.
   */
  const DenseTensorDescriptor& descriptor() const noexcept;

  /**
   * @brief Returns the retained explicit image-axis mapping.
   *
   * @return Borrowed validated facet.
   * @throws Nothing.
   */
  const ImageFacet& image_facet() const noexcept;

  /**
   * @brief Returns the retained physical tensor layout.
   *
   * @return Borrowed validated byte strides.
   * @throws Nothing.
   */
  const StridedLayout& layout() const noexcept;

  /**
   * @brief Returns image width in pixels.
   *
   * @return Positive x-axis extent.
   * @throws Nothing.
   */
  std::size_t width() const noexcept;

  /**
   * @brief Returns image height in pixels.
   *
   * @return Positive y-axis extent.
   * @throws Nothing.
   */
  std::size_t height() const noexcept;

  /**
   * @brief Returns the number of elements per pixel.
   *
   * @return Positive channel-axis extent, or one when no channel axis exists.
   * @throws Nothing.
   */
  std::size_t channels() const noexcept;

  /**
   * @brief Returns the physical byte width of one channel element.
   *
   * @return Validated V-2 element byte width.
   * @throws Nothing.
   */
  std::size_t element_bytes() const noexcept;

  /**
   * @brief Returns the signed byte stride of the explicit y axis.
   *
   * @return Positive row stride retained by the tensor layout.
   * @throws Nothing.
   */
  std::ptrdiff_t row_stride() const noexcept;

  /**
   * @brief Returns one channel element address after coordinate checks.
   *
   * @param x Zero-based x coordinate.
   * @param y Zero-based y coordinate.
   * @param channel Zero-based channel coordinate; must be zero when the facet
   *        has no channel axis.
   * @return Read-only pointer to the requested element's first byte.
   * @throws std::out_of_range when any coordinate is outside its explicit
   *         image extent.
   * @note Successful Value and ImageView validation prove the address remains
   *       inside the retained immutable byte envelope.
   */
  const std::byte* channel_data(std::size_t x, std::size_t y,
                                std::size_t channel) const;

 private:
  /** @brief Retaining checked tensor view that owns the Value lifetime. */
  DenseTensorView tensor_;

  /** @brief Explicit validated image-axis assignment. */
  ImageFacet image_facet_;

  /** @brief Cached positive x-axis extent. */
  std::size_t width_ = 0U;

  /** @brief Cached positive y-axis extent. */
  std::size_t height_ = 0U;

  /** @brief Cached positive channel count. */
  std::size_t channels_ = 0U;

  /** @brief Cached validated physical bytes per channel element. */
  std::size_t element_bytes_ = 0U;
};

}  // namespace ps
