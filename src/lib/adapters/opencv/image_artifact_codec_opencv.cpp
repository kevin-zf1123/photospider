#include "adapters/opencv/image_artifact_codec_opencv.hpp"

#include <new>
#include <opencv2/imgcodecs.hpp>
#include <string>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "photospider/core/graph_error.hpp"

namespace ps::adapters::opencv {
namespace {

/**
 * @brief Converts an OpenCV codec exception to the kernel exception surface.
 * @param operation Human-readable decode or encode phase.
 * @param path Artifact path associated with the failed call.
 * @param error OpenCV exception caught inside the adapter.
 * @return Never returns.
 * @throws std::bad_alloc when OpenCV reports `cv::Error::StsNoMem`.
 * @throws GraphError with `GraphErrc::Io` for every other OpenCV failure.
 * @note The translated diagnostic owns its text and no OpenCV dynamic exception
 * type crosses the adapter boundary.
 */
[[noreturn]] void throw_translated_codec_exception(
    const char* operation, const std::filesystem::path& path,
    const cv::Exception& error) {
  if (error.code == cv::Error::StsNoMem) {
    throw std::bad_alloc();
  }
  throw GraphError(GraphErrc::Io, std::string("OpenCV image codec failed to ") +
                                      operation + " '" + path.string() +
                                      "': " + error.what());
}

}  // namespace

/** @copydoc OpenCvImageArtifactCodec::decode */
ImageBuffer OpenCvImageArtifactCodec::decode(
    const std::filesystem::path& path) const {
  try {
    cv::Mat encoded = cv::imread(path.string(), cv::IMREAD_UNCHANGED);
    if (encoded.empty()) {
      throw GraphError(GraphErrc::Io,
                       "Failed to decode image artifact: " + path.string());
    }

    const double scale =
        encoded.depth() == CV_8U
            ? 1.0 / 255.0
            : (encoded.depth() == CV_16U ? 1.0 / 65535.0 : 1.0);
    cv::Mat decoded;
    encoded.convertTo(decoded, CV_32F, scale);
    return fromCvMat(decoded);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const cv::Exception& error) {
    throw_translated_codec_exception("decode", path, error);
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Io, "Failed to decode image artifact '" +
                                        path.string() + "': " + error.what());
  }
}

/** @copydoc OpenCvImageArtifactCodec::encode */
void OpenCvImageArtifactCodec::encode(const std::filesystem::path& path,
                                      const ImageBuffer& image,
                                      ImageArtifactPrecision precision) const {
  try {
    cv::Mat source = toCvMat(image);
    if (source.empty()) {
      throw GraphError(
          GraphErrc::Io,
          "Cannot encode an empty image artifact: " + path.string());
    }

    cv::Mat encoded;
    if (precision == ImageArtifactPrecision::UInt16) {
      source.convertTo(encoded, CV_16U, 65535.0);
    } else {
      source.convertTo(encoded, CV_8U, 255.0);
    }
    if (!cv::imwrite(path.string(), encoded)) {
      throw GraphError(GraphErrc::Io,
                       "OpenCV rejected image artifact path: " + path.string());
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const cv::Exception& error) {
    throw_translated_codec_exception("encode", path, error);
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Io, "Failed to encode image artifact '" +
                                        path.string() + "': " + error.what());
  }
}

}  // namespace ps::adapters::opencv
