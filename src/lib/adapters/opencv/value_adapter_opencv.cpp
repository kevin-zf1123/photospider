#include "adapters/opencv/value_adapter_opencv.hpp"

#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"
#include "photospider/plugin/opencv_adapter.hpp"

namespace ps {
namespace {

/**
 * @brief Multiplies OpenCV adapter byte factors without wraparound.
 * @param left First nonnegative factor.
 * @param right Second nonnegative factor.
 * @return Exact product.
 * @throws std::overflow_error when size_t cannot represent the product.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("OpenCV Value byte multiplication overflowed.");
  }
  return left * right;
}

/**
 * @brief Adds OpenCV adapter byte terms without wraparound.
 * @param left First nonnegative term.
 * @param right Second nonnegative term.
 * @return Exact sum.
 * @throws std::overflow_error when size_t cannot represent the sum.
 */
std::size_t checked_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("OpenCV Value byte addition overflowed.");
  }
  return left + right;
}

/**
 * @brief Maps validated DenseTensor scalar facts to one OpenCV depth.
 * @param descriptor Whole-byte native scalar descriptor.
 * @return OpenCV depth constant without channel bits.
 * @throws std::invalid_argument for unsupported semantics or encoding.
 */
int to_cv_depth(const DenseTensorDescriptor& descriptor) {
  if (descriptor.storage_encoding.kind != StorageEncodingKind::NativeScalar) {
    throw std::invalid_argument("OpenCV requires native scalar storage.");
  }
  const std::uint32_t bits = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (bits == 8U)
        return CV_8U;
      if (bits == 16U)
        return CV_16U;
      break;
    case ElementSemantics::SignedInteger:
      if (bits == 8U)
        return CV_8S;
      if (bits == 16U)
        return CV_16S;
      break;
    case ElementSemantics::FloatingPoint:
      if (bits == 32U)
        return CV_32F;
      if (bits == 64U)
        return CV_64F;
      break;
  }
  throw std::invalid_argument(
      "OpenCV does not support the DenseTensor scalar encoding.");
}

/**
 * @brief Maps one supported OpenCV depth to DenseTensor scalar facts.
 * @param matrix_type Complete OpenCV matrix type.
 * @return Logical semantics and native scalar storage encoding.
 * @throws std::invalid_argument when the depth has no maintained mapping.
 */
std::pair<ElementSemantics, StorageEncoding> from_cv_type(int matrix_type) {
  switch (CV_MAT_DEPTH(matrix_type)) {
    case CV_8U:
      return {ElementSemantics::UnsignedInteger, StorageEncoding{8U}};
    case CV_8S:
      return {ElementSemantics::SignedInteger, StorageEncoding{8U}};
    case CV_16U:
      return {ElementSemantics::UnsignedInteger, StorageEncoding{16U}};
    case CV_16S:
      return {ElementSemantics::SignedInteger, StorageEncoding{16U}};
    case CV_32F:
      return {ElementSemantics::FloatingPoint, StorageEncoding{32U}};
    case CV_64F:
      return {ElementSemantics::FloatingPoint, StorageEncoding{64U}};
    default:
      throw std::invalid_argument("OpenCV matrix depth is unsupported.");
  }
}

/**
 * @brief Validates OpenCV-compatible interleaving and returns its matrix type.
 * @param view Retaining validated ordinary-image view.
 * @return Complete OpenCV type including channel count.
 * @throws std::invalid_argument for unsupported channels or physical layout.
 * @throws std::overflow_error when active row bytes are unrepresentable.
 */
int validate_cv_image_view(const ImageView& view) {
  if (view.channels() == 0U ||
      view.channels() > static_cast<std::size_t>(CV_CN_MAX)) {
    throw std::invalid_argument("OpenCV channel count is unsupported.");
  }
  const std::size_t pixel_bytes =
      checked_multiply(view.channels(), view.element_bytes());
  const std::size_t row_bytes = checked_multiply(view.width(), pixel_bytes);
  const ImageFacet& facet = view.image_facet();
  const std::vector<std::ptrdiff_t>& strides = view.layout().byte_strides;
  if ((view.width() > 1U &&
       (strides[facet.x_axis] <= 0 ||
        static_cast<std::size_t>(strides[facet.x_axis]) != pixel_bytes)) ||
      (facet.channel_axis.has_value() && view.channels() > 1U &&
       (strides[*facet.channel_axis] <= 0 ||
        static_cast<std::size_t>(strides[*facet.channel_axis]) !=
            view.element_bytes())) ||
      (view.height() > 1U &&
       (view.row_stride() <= 0 ||
        static_cast<std::size_t>(view.row_stride()) < row_bytes))) {
    throw std::invalid_argument(
        "OpenCV requires positive interleaved image rows.");
  }
  return CV_MAKETYPE(to_cv_depth(view.descriptor()),
                     static_cast<int>(view.channels()));
}

/**
 * @brief Validates one zero-based ROI against a retained image view.
 * @param roi Candidate ROI.
 * @param view Enclosing image.
 * @return Nothing.
 * @throws std::out_of_range for negative or out-of-bounds endpoints.
 */
void validate_roi(const PixelRect& roi, const ImageView& view) {
  if (roi.x < 0 || roi.y < 0 || roi.width < 0 || roi.height < 0 ||
      static_cast<std::size_t>(roi.x) > view.width() ||
      static_cast<std::size_t>(roi.y) > view.height() ||
      static_cast<std::size_t>(roi.width) >
          view.width() - static_cast<std::size_t>(roi.x) ||
      static_cast<std::size_t>(roi.height) >
          view.height() - static_cast<std::size_t>(roi.y)) {
    throw std::out_of_range("OpenCV tile ROI is outside the Value extent.");
  }
}

/**
 * @brief Converts kernel geometry into the provider-local rectangle type.
 * @param roi Validated zero-based ROI.
 * @return Equivalent OpenCV rectangle.
 * @throws Nothing.
 */
cv::Rect to_cv_rect(const PixelRect& roi) noexcept {
  return cv::Rect(roi.x, roi.y, roi.width, roi.height);
}

}  // namespace

/** @copydoc toCvMat(const Value&) */
cv::Mat toCvMat(const Value& value) {
  ImageView view(value);
  const int matrix_type = validate_cv_image_view(view);
  const std::size_t row_bytes = checked_multiply(
      checked_multiply(view.width(), view.channels()), view.element_bytes());
  const std::size_t row_stride =
      view.height() == 1U && view.row_stride() <= 0
          ? row_bytes
          : static_cast<std::size_t>(view.row_stride());
  return cv::Mat(static_cast<int>(view.height()),
                 static_cast<int>(view.width()), matrix_type,
                 const_cast<std::byte*>(view.channel_data(0U, 0U, 0U)),
                 row_stride);
}

/** @copydoc toCvMat(const InputTile&) */
cv::Mat toCvMat(const InputTile& tile) {
  if (tile.value == nullptr) {
    throw std::runtime_error("OpenCV input tile has no Value.");
  }
  ImageView view(*tile.value);
  validate_roi(tile.roi, view);
  return toCvMat(*tile.value)(to_cv_rect(tile.roi));
}

/** @copydoc toCvUMat(const Value&) */
cv::UMat toCvUMat(const Value& value) {
  return toCvMat(value).getUMat(cv::ACCESS_READ);
}

/** @copydoc toCvUMat(const InputTile&) */
cv::UMat toCvUMat(const InputTile& tile) {
  return toCvMat(tile).getUMat(cv::ACCESS_READ);
}

/** @copydoc fromCvMat */
Value fromCvMat(const cv::Mat& matrix, ImageFacet image_facet) {
  if (matrix.empty() || matrix.dims != 2 || matrix.rows <= 0 ||
      matrix.cols <= 0 || matrix.channels() <= 0) {
    throw std::invalid_argument(
        "OpenCV Value publication requires a nonempty 2D matrix.");
  }
  const auto [semantics, encoding] = from_cv_type(matrix.type());
  DenseTensorDescriptor descriptor{
      {static_cast<std::size_t>(matrix.rows),
       static_cast<std::size_t>(matrix.cols),
       static_cast<std::size_t>(matrix.channels())},
      semantics,
      encoding};
  validate_dense_tensor_image_metadata(descriptor, image_facet);
  if (image_facet.y_axis != 0U || image_facet.x_axis != 1U ||
      !image_facet.channel_axis.has_value() ||
      *image_facet.channel_axis != 2U) {
    throw std::invalid_argument(
        "OpenCV publication requires explicit y/x/channel axes 0/1/2.");
  }
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  const std::size_t pixel_bytes = checked_multiply(
      static_cast<std::size_t>(matrix.channels()), element_bytes);
  const std::size_t row_bytes =
      checked_multiply(static_cast<std::size_t>(matrix.cols), pixel_bytes);
  if (matrix.step[0] < row_bytes ||
      matrix.step[0] > static_cast<std::size_t>(
                           std::numeric_limits<std::ptrdiff_t>::max()) ||
      pixel_bytes > static_cast<std::size_t>(
                        std::numeric_limits<std::ptrdiff_t>::max()) ||
      element_bytes > static_cast<std::size_t>(
                          std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::invalid_argument("OpenCV matrix row layout is unsupported.");
  }
  const std::size_t storage_size =
      checked_add(checked_multiply(static_cast<std::size_t>(matrix.rows - 1),
                                   matrix.step[0]),
                  row_bytes);
  StridedLayout layout{{static_cast<std::ptrdiff_t>(matrix.step[0]),
                        static_cast<std::ptrdiff_t>(pixel_bytes),
                        static_cast<std::ptrdiff_t>(element_bytes)}};
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      descriptor, image_facet, layout, storage_size);
  {
    WriteLease write = builder.acquire_write();
    std::memset(write.data(), 0, write.size());
    for (int row = 0; row < matrix.rows; ++row) {
      std::memcpy(write.data() + static_cast<std::size_t>(row) * matrix.step[0],
                  matrix.ptr(row), row_bytes);
    }
  }
  return builder.seal();
}

/** @copydoc fromCvUMat */
Value fromCvUMat(const cv::UMat& matrix, ImageFacet image_facet) {
  if (matrix.empty()) {
    throw std::invalid_argument(
        "OpenCV Value publication requires a nonempty UMat.");
  }
  const cv::Mat mapping = matrix.getMat(cv::ACCESS_READ);
  return fromCvMat(mapping, std::move(image_facet));
}

}  // namespace ps

namespace ps::plugin::opencv {

/** @copydoc to_mat */
cv::Mat to_mat(const Value& value) {
  return ps::toCvMat(value);
}

/** @copydoc to_umat */
cv::UMat to_umat(const Value& value) {
  return ps::toCvUMat(value);
}

/** @copydoc from_mat */
Value from_mat(const cv::Mat& matrix, ImageFacet image_facet) {
  return ps::fromCvMat(matrix, std::move(image_facet));
}

/** @copydoc from_umat */
Value from_umat(const cv::UMat& matrix, ImageFacet image_facet) {
  return ps::fromCvUMat(matrix, std::move(image_facet));
}

}  // namespace ps::plugin::opencv
