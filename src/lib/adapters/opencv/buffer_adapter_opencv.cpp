#include "adapters/opencv/buffer_adapter_opencv.hpp"

#include <memory>
#include <stdexcept>
#include <string>

#include "photospider/plugin/opencv_adapter.hpp"

namespace ps {
namespace {

/**
 * @brief Converts a Photospider DataType/channel pair to an OpenCV matrix type.
 *
 * @param type Channel scalar type from ImageBuffer.
 * @param channels Number of channels per pixel.
 * @return OpenCV CV_* type matching the input description.
 * @throws std::invalid_argument when channels is outside OpenCV's supported
 * range.
 * @throws std::runtime_error when type is not supported by the adapter.
 * @note Validation occurs before any CV_*C macro expands the channel count.
 */
int to_cv_type(DataType type, int channels) {
  if (channels <= 0 || channels > CV_CN_MAX) {
    throw std::invalid_argument(
        "OpenCV conversion requires a supported positive channel count");
  }
  switch (type) {
    case ps::DataType::UINT8:
      return CV_8UC(channels);
    case ps::DataType::INT8:
      return CV_8SC(channels);
    case ps::DataType::UINT16:
      return CV_16UC(channels);
    case ps::DataType::INT16:
      return CV_16SC(channels);
    case ps::DataType::FLOAT32:
      return CV_32FC(channels);
    case ps::DataType::FLOAT64:
      return CV_64FC(channels);
  }
  throw std::runtime_error("Unsupported data type for OpenCV conversion");
}

/**
 * @brief Rejects descriptors whose data pointer is not declared host memory.
 * @param buffer Descriptor about to be interpreted as a CPU address.
 * @param operation Adapter operation name used in the stable diagnostic.
 * @return Nothing.
 * @throws std::runtime_error when buffer.device is not Device::CPU.
 * @note A non-null data field on a non-CPU descriptor is not evidence that the
 * address is host-dereferenceable; the matching device adapter owns transfer.
 */
void require_cpu_device(const ImageBuffer& buffer, const char* operation) {
  if (buffer.device != Device::CPU) {
    throw std::runtime_error(std::string(operation) +
                             ": non-CPU images require a device adapter.");
  }
}

/**
 * @brief Converts an OpenCV matrix depth to a Photospider DataType.
 *
 * @param cv_type OpenCV matrix type code.
 * @return DataType representing the matrix scalar depth.
 * @throws std::runtime_error when the OpenCV depth has no ImageBuffer mapping.
 * @note Channel count is handled separately by fromCvMat/fromCvUMat.
 */
DataType from_cv_type(int cv_type) {
  switch (CV_MAT_DEPTH(cv_type)) {
    case CV_8U:
      return ps::DataType::UINT8;
    case CV_8S:
      return ps::DataType::INT8;
    case CV_16U:
      return ps::DataType::UINT16;
    case CV_16S:
      return ps::DataType::INT16;
    case CV_32F:
      return ps::DataType::FLOAT32;
    case CV_64F:
      return ps::DataType::FLOAT64;
    default:
      throw std::runtime_error(
          "Unsupported cv::Mat depth for ImageBuffer conversion");
  }
}

/**
 * @brief Owns one writable UMat mapping and its originating unified handle.
 * @throws cv::Exception when ACCESS_RW mapping fails.
 * @note Member order is intentional: reverse destruction retires the derived
 * Mat mapping before the UMat allocation regardless of ImageBuffer alias reset
 * order.
 */
struct RetainedUmatMapping {
  /**
   * @brief Copies a unified handle and opens its writable host mapping.
   * @param source UMat whose allocation must remain retained.
   * @throws cv::Exception when writable host mapping fails.
   */
  explicit RetainedUmatMapping(const cv::UMat& source)
      : unified(source), mapping(unified.getMat(cv::ACCESS_RW)) {}

  /** @brief Originating UMat destroyed after its derived mapping. */
  cv::UMat unified;
  /** @brief Writable host mapping destroyed before unified. */
  cv::Mat mapping;
};

/**
 * @brief Returns the ImageBuffer pointer carried by an input tile.
 *
 * @param tile Read-only tile view to inspect.
 * @return Borrowed ImageBuffer pointer.
 * @throws Nothing.
 * @note Used by templated tile adapters so input/output overloads share ROI
 * validation and slicing behavior.
 */
const ImageBuffer* tile_buffer(const InputTile& tile) noexcept {
  return tile.buffer;
}

/**
 * @brief Returns the ImageBuffer pointer carried by an output tile.
 *
 * @param tile Writable tile view to inspect.
 * @return Borrowed ImageBuffer pointer.
 * @throws Nothing.
 * @note Used by templated tile adapters so input/output overloads share ROI
 * validation and slicing behavior.
 */
ImageBuffer* tile_buffer(const OutputTile& tile) noexcept {
  return tile.buffer;
}

/**
 * @brief Converts any tile view to an ROI-scoped cv::Mat.
 *
 * @tparam TileView InputTile or OutputTile.
 * @param tile Tile view carrying a buffer pointer and ROI.
 * @param missing_message Error text used when the tile has no buffer.
 * @return cv::Mat view covering tile.roi.
 * @throws std::runtime_error when tile has no buffer or the ImageBuffer adapter
 * cannot expose a matrix.
 * @throws cv::Exception when backend mapping or ROI slicing fails.
 * @note InputTile pixel immutability is a caller contract because OpenCV
 * returns mutable cv::Mat handles even for const source buffers.
 */
template <typename TileView>
cv::Mat to_cv_mat_for_tile(const TileView& tile, const char* missing_message) {
  auto* buffer = tile_buffer(tile);
  if (!buffer) {
    throw std::runtime_error(missing_message);
  }
  require_cpu_device(*buffer, "toCvMat");
  cv::Mat full_mat;
  if (buffer->data) {
    full_mat = cv::Mat(buffer->height, buffer->width,
                       to_cv_type(buffer->type, buffer->channels),
                       buffer->data.get(), buffer->step);
  } else {
    throw std::runtime_error(
        "OpenCV adapter cannot interpret an opaque backend-only context");
  }
  return full_mat(tile.roi);
}

/**
 * @brief Converts any tile view to an ROI-scoped cv::UMat.
 *
 * @tparam TileView InputTile or OutputTile.
 * @param tile Tile view carrying a buffer pointer and ROI.
 * @param missing_message Error text used when the tile has no buffer.
 * @param access OpenCV access intent for upload/backend synchronization.
 * @return cv::UMat view covering tile.roi.
 * @throws std::runtime_error when tile has no buffer or the ImageBuffer adapter
 * cannot expose a UMat.
 * @throws cv::Exception when upload, backend mapping, or ROI slicing fails.
 * @note InputTile pixel immutability is a caller contract because OpenCV UMat
 * views can still be passed to mutating APIs.
 */
template <typename TileView>
cv::UMat to_cv_umat_for_tile(const TileView& tile, const char* missing_message,
                             cv::AccessFlag access) {
  auto* buffer = tile_buffer(tile);
  if (!buffer) {
    throw std::runtime_error(missing_message);
  }
  require_cpu_device(*buffer, "toCvUMat");
  cv::UMat full_umat;
  if (buffer->data) {
    cv::Mat view(buffer->height, buffer->width,
                 to_cv_type(buffer->type, buffer->channels), buffer->data.get(),
                 buffer->step);
    full_umat = view.getUMat(access);
  } else {
    throw std::runtime_error(
        "OpenCV adapter cannot interpret an opaque backend-only context");
  }
  return full_umat(tile.roi);
}

}  // namespace

/** @copydoc toCvMat(const ImageBuffer&) */
cv::Mat toCvMat(const ImageBuffer& buffer) {
  require_cpu_device(buffer, "toCvMat");
  // CPU storage is exposed as a zero-copy, source-lifetime-bound view.
  if (buffer.data) {
    int type = to_cv_type(buffer.type, buffer.channels);
    return cv::Mat(buffer.height, buffer.width, type, buffer.data.get(),
                   buffer.step);
  }

  if (buffer.context) {
    throw std::runtime_error(
        "toCvMat: opaque backend-only context requires a device adapter.");
  }

  throw std::runtime_error("toCvMat: CPU buffer has no pixel data.");
}

/** @copydoc toCvMat(const InputTile&) */
cv::Mat toCvMat(const InputTile& tile) {
  return to_cv_mat_for_tile(tile,
                            "toCvMat: InputTile has no associated buffer.");
}

/** @copydoc toCvMat(const OutputTile&) */
cv::Mat toCvMat(const OutputTile& tile) {
  return to_cv_mat_for_tile(tile,
                            "toCvMat: OutputTile has no associated buffer.");
}

/** @copydoc toCvUMat(const ImageBuffer&) */
cv::UMat toCvUMat(const ImageBuffer& buffer) {
  require_cpu_device(buffer, "toCvUMat");
  // CPU storage is uploaded through a source-lifetime-bound Mat view.
  if (buffer.data) {
    int type = to_cv_type(buffer.type, buffer.channels);
    cv::Mat temp_mat_view(buffer.height, buffer.width, type, buffer.data.get(),
                          buffer.step);
    return temp_mat_view.getUMat(cv::ACCESS_READ);
  }

  if (buffer.context) {
    throw std::runtime_error(
        "toCvUMat: opaque backend-only context requires a device adapter.");
  }

  throw std::runtime_error("toCvUMat: CPU buffer has no pixel data.");
}

/** @copydoc toCvUMat(const InputTile&) */
cv::UMat toCvUMat(const InputTile& tile) {
  return to_cv_umat_for_tile(
      tile, "toCvUMat: InputTile has no associated buffer.", cv::ACCESS_READ);
}

/** @copydoc toCvUMat(const OutputTile&) */
cv::UMat toCvUMat(const OutputTile& tile) {
  return to_cv_umat_for_tile(
      tile, "toCvUMat: OutputTile has no associated buffer.", cv::ACCESS_WRITE);
}

/**
 * @brief Converts a cv::Mat to a lifetime-safe CPU ImageBuffer.
 *
 * @param mat Matrix whose pixels and metadata are retained.
 * @return Descriptor sharing ordinary OpenCV-owned storage or owning a deep
 *         copy when `mat` only borrows external raw memory.
 * @throws std::runtime_error when the matrix depth is unsupported.
 * @throws std::bad_alloc when clone or shared lifetime storage cannot allocate.
 * @throws cv::Exception when cloning fails.
 * @note `cv::Mat::u == nullptr` identifies a header over external storage; a
 *       copied header cannot retain that owner, so this path clones first and
 *       publishes the clone's data pointer and step. Ordinary owning matrices
 *       remain zero-copy.
 */
ps::ImageBuffer fromCvMat(const cv::Mat& mat) {
  if (mat.empty()) {
    return {};
  }
  cv::Mat retained = mat.u != nullptr ? mat : mat.clone();
  ps::ImageBuffer buffer;
  buffer.width = retained.cols;
  buffer.height = retained.rows;
  buffer.channels = retained.channels();
  buffer.type = from_cv_type(retained.type());
  buffer.device = ps::Device::CPU;
  buffer.step = retained.step;

  // The retained header owns ordinary OpenCV storage. External raw-storage
  // headers were cloned above, so this captured value always owns the pixels.
  buffer.data = std::shared_ptr<void>(
      retained.data,
      [retained_owner = retained](void*) { (void)retained_owner; });

  return buffer;
}

/** @copydoc fromCvUMat */
ImageBuffer fromCvUMat(const cv::UMat& umat) {
  if (umat.empty()) {
    return {};
  }
  auto retained = std::make_shared<RetainedUmatMapping>(umat);
  ImageBuffer buffer;
  buffer.width = umat.cols;
  buffer.height = umat.rows;
  buffer.channels = umat.channels();
  buffer.type = from_cv_type(umat.type());
  buffer.device = ps::Device::CPU;
  buffer.step = retained->mapping.step;
  buffer.data = std::shared_ptr<void>(retained, retained->mapping.data);

  // Both aliases share one owner whose member order retires Mat before UMat.
  buffer.context = std::shared_ptr<void>(retained, &retained->unified);

  return buffer;
}

}  // namespace ps

namespace ps::plugin::opencv {
namespace {

/**
 * @brief Converts public geometry to the OpenCV adapter rectangle.
 * @param rectangle Public pixel rectangle.
 * @return Equivalent OpenCV rectangle.
 * @throws Nothing.
 */
cv::Rect to_opencv_rect(const PixelRect& rectangle) noexcept {
  return cv::Rect(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
}

/**
 * @brief Validates one public tile ROI without overflow-prone edge arithmetic.
 * @param buffer Image extent containing the tile.
 * @param rectangle Public ROI to validate.
 * @return Nothing.
 * @throws std::out_of_range when dimensions are negative or the ROI falls
 *         outside the image extent.
 * @note Empty edge-aligned ROIs are valid. Subtraction after origin validation
 *       avoids signed overflow from x + width and y + height.
 */
void validate_public_tile_roi(const ImageBuffer& buffer,
                              const PixelRect& rectangle) {
  if (buffer.width < 0 || buffer.height < 0 || rectangle.x < 0 ||
      rectangle.y < 0 || rectangle.width < 0 || rectangle.height < 0 ||
      rectangle.x > buffer.width || rectangle.y > buffer.height ||
      rectangle.width > buffer.width - rectangle.x ||
      rectangle.height > buffer.height - rectangle.y) {
    throw std::out_of_range("OpenCV tile ROI is outside the image extent");
  }
}

}  // namespace

/** @copydoc to_mat(const ImageBuffer&) */
cv::Mat to_mat(const ImageBuffer& buffer) {
  return ps::toCvMat(buffer);
}

/** @copydoc to_mat(const InputTileView&) */
cv::Mat to_mat(const InputTileView& tile) {
  if (!tile.buffer) {
    throw std::runtime_error("to_mat: input tile has no image buffer");
  }
  validate_public_tile_roi(*tile.buffer, tile.roi);
  return ps::toCvMat(InputTile{tile.buffer, to_opencv_rect(tile.roi), nullptr});
}

/** @copydoc to_mat(const OutputTileView&) */
cv::Mat to_mat(const OutputTileView& tile) {
  if (!tile.buffer) {
    throw std::runtime_error("to_mat: output tile has no image buffer");
  }
  validate_public_tile_roi(*tile.buffer, tile.roi);
  return ps::toCvMat(*tile.buffer)(to_opencv_rect(tile.roi));
}

/** @copydoc to_umat(const ImageBuffer&) */
cv::UMat to_umat(const ImageBuffer& buffer) {
  return ps::toCvUMat(buffer);
}

/** @copydoc to_umat(const InputTileView&) */
cv::UMat to_umat(const InputTileView& tile) {
  if (!tile.buffer) {
    throw std::runtime_error("to_umat: input tile has no image buffer");
  }
  validate_public_tile_roi(*tile.buffer, tile.roi);
  return ps::toCvUMat(
      InputTile{tile.buffer, to_opencv_rect(tile.roi), nullptr});
}

/** @copydoc to_umat(const OutputTileView&) */
cv::UMat to_umat(const OutputTileView& tile) {
  if (!tile.buffer) {
    throw std::runtime_error("to_umat: output tile has no image buffer");
  }
  validate_public_tile_roi(*tile.buffer, tile.roi);
  cv::Mat addressable = ps::toCvMat(*tile.buffer);
  cv::UMat writable = addressable.getUMat(cv::ACCESS_WRITE);
  return writable(to_opencv_rect(tile.roi));
}

/**
 * @brief Converts a public OpenCV matrix to a lifetime-safe ImageBuffer.
 * @param matrix Matrix whose ordinary storage is shared or whose external raw
 *        storage is deep-copied.
 * @return CPU descriptor that remains valid after the source handle and any
 *         external owner are destroyed.
 * @throws std::runtime_error for unsupported matrix depth.
 * @throws std::bad_alloc from clone or lifetime-control allocation.
 * @throws cv::Exception from clone or retained backend context creation.
 * @note This public wrapper preserves `fromCvMat`'s zero-copy owning path and
 *       external-storage safety policy unchanged.
 */
ImageBuffer from_mat(const cv::Mat& matrix) {
  return ps::fromCvMat(matrix);
}

/** @copydoc from_umat */
ImageBuffer from_umat(const cv::UMat& matrix) {
  return ps::fromCvUMat(matrix);
}

}  // namespace ps::plugin::opencv
