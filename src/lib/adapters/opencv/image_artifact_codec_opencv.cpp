#include "adapters/opencv/image_artifact_codec_opencv.hpp"

#include <algorithm>
#include <cctype>
#include <new>
#include <opencv2/imgcodecs.hpp>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "adapters/opencv/value_adapter_opencv.hpp"
#include "photospider/core/graph_error.hpp"

namespace ps::adapters::opencv {
namespace {

/**
 * @brief Reports whether a path selects the dedicated OpenEXR boundary.
 * @param path Caller-selected artifact path.
 * @return True for a case-insensitive `.exr` extension.
 * @throws std::bad_alloc when native path-string conversion allocates.
 * @note OpenCV must not consume these paths because it cannot expose signed
 *       data/display-window metadata independently.
 */
bool is_openexr_path(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return extension == ".exr";
}

/**
 * @brief Translates one OpenCV codec exception into the kernel error surface.
 * @param operation Stable decode or encode verb.
 * @param path Artifact path associated with the failed call.
 * @param error OpenCV exception caught at the dependency boundary.
 * @throws std::bad_alloc for `cv::Error::StsNoMem`.
 * @throws GraphError with `GraphErrc::Io` for every other OpenCV error.
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

/**
 * @brief Builds explicit zero-origin metadata for one decoded matrix.
 * @param matrix Nonempty decoded matrix.
 * @param endpoint Caller-declared exact file sample meaning.
 * @return Complete y/x/channel image facet.
 * @throws std::invalid_argument or std::overflow_error for invalid extent or
 *         sample metadata.
 * @note OpenCV does not expose signed data/display-window authority; display
 *       remains absent rather than guessed.
 */
ImageFacet decoded_image_facet(const cv::Mat& matrix,
                               const SampleEndpoint& endpoint) {
  DenseTensorDescriptor shape_only{
      {static_cast<std::size_t>(matrix.rows),
       static_cast<std::size_t>(matrix.cols),
       static_cast<std::size_t>(matrix.channels())},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{8U}};
  ImageFacet facet = make_zero_origin_image_facet(shape_only, 1U, 0U, 2U);
  facet.sample_domain =
      SampleDomainFacet{1U, endpoint.encoding, endpoint.domain, {}};
  return facet;
}

/**
 * @brief Maps decoded OpenCV depth to an exact storage rule key.
 * @param matrix Nonempty decoded matrix.
 * @return DenseTensor scalar semantics and native storage encoding.
 * @throws std::invalid_argument for an unsupported OpenCV depth.
 */
std::pair<ElementSemantics, StorageEncoding> decoded_storage(
    const cv::Mat& matrix) {
  switch (matrix.depth()) {
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
      throw std::invalid_argument("Decoded OpenCV storage is unsupported.");
  }
}

/**
 * @brief Resolves one exact caller-declared rule for decoded storage.
 * @param request Bounded strictly ordered rule table.
 * @param semantics Decoded logical scalar category.
 * @param encoding Decoded physical scalar encoding.
 * @return Borrowed unique matching rule.
 * @throws std::invalid_argument for empty, noncanonical, duplicate, or missing
 *         rules.
 */
const ImageArtifactDecodeRule& select_decode_rule(
    const ImageArtifactDecodeRequest& request, ElementSemantics semantics,
    const StorageEncoding& encoding) {
  if (request.rules.empty() || request.rules.size() > 16U) {
    throw std::invalid_argument(
        "Image decode requires one to sixteen explicit storage rules.");
  }
  auto key = [](const ImageArtifactDecodeRule& rule) {
    return std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>{
        static_cast<std::uint32_t>(rule.element_semantics),
        static_cast<std::uint32_t>(rule.storage_encoding.kind),
        rule.storage_encoding.bit_width};
  };
  for (std::size_t index = 1U; index < request.rules.size(); ++index) {
    if (!(key(request.rules[index - 1U]) < key(request.rules[index]))) {
      throw std::invalid_argument(
          "Image decode storage rules must be strictly ordered.");
    }
  }
  const auto found =
      std::find_if(request.rules.begin(), request.rules.end(),
                   [&](const ImageArtifactDecodeRule& rule) {
                     return rule.element_semantics == semantics &&
                            rule.storage_encoding == encoding;
                   });
  if (found == request.rules.end()) {
    throw std::invalid_argument(
        "Decoded image storage has no explicit semantic rule.");
  }
  return *found;
}

/**
 * @brief Requires complete default sample metadata for direct file encode.
 * @param image Valid ordinary image Value.
 * @return Borrowed complete sample-domain facet.
 * @throws std::invalid_argument when metadata is absent or per-channel.
 */
const SampleDomainFacet& require_direct_sample_metadata(const Value& image) {
  if (!image.image_facet().has_value() ||
      !image.image_facet()->sample_domain.has_value() ||
      !image.image_facet()->sample_domain->per_channel.empty()) {
    throw std::invalid_argument(
        "Direct image encode requires one explicit default sample domain.");
  }
  return *image.image_facet()->sample_domain;
}

}  // namespace

/** @copydoc OpenCvImageArtifactCodec::decode */
Value OpenCvImageArtifactCodec::decode(
    const std::filesystem::path& path,
    const ImageArtifactDecodeRequest& request) const {
  try {
    if (is_openexr_path(path)) {
      throw std::invalid_argument(
          "OpenEXR decode requires the dedicated signed-window codec.");
    }
    const cv::Mat encoded = cv::imread(path.string(), cv::IMREAD_UNCHANGED);
    if (encoded.empty()) {
      throw GraphError(GraphErrc::Io,
                       "Failed to decode image artifact: " + path.string());
    }
    const auto [semantics, encoding] = decoded_storage(encoded);
    const ImageArtifactDecodeRule& rule =
        select_decode_rule(request, semantics, encoding);
    Value decoded =
        fromCvMat(encoded, decoded_image_facet(encoded, rule.encoded_samples));
    if (!rule.conversion.has_value()) {
      return decoded;
    }
    if (!(rule.conversion->source.encoding == rule.encoded_samples.encoding) ||
        !(rule.conversion->source.domain == rule.encoded_samples.domain)) {
      throw std::invalid_argument(
          "Decode conversion source disagrees with encoded sample meaning.");
    }
    return convert_dense_image_samples(decoded, *rule.conversion);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const cv::Exception& error) {
    throw_translated_codec_exception("decode", path, error);
  } catch (const std::invalid_argument&) {
    throw;
  } catch (const std::domain_error&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Io, "Failed to decode image artifact '" +
                                        path.string() + "': " + error.what());
  }
}

/** @copydoc OpenCvImageArtifactCodec::encode */
void OpenCvImageArtifactCodec::encode(
    const std::filesystem::path& path, const Value& image,
    const ImageArtifactEncodeRequest& request) const {
  try {
    if (is_openexr_path(path)) {
      throw std::invalid_argument(
          "OpenEXR encode requires the dedicated signed-window codec.");
    }
    Value converted;
    const Value* selected = &image;
    if (request.conversion.has_value()) {
      converted = convert_dense_image_samples(image, *request.conversion);
      selected = &converted;
    } else {
      (void)require_direct_sample_metadata(image);
    }
    const cv::Mat encoded = toCvMat(*selected);
    if (encoded.empty()) {
      throw std::invalid_argument("Cannot encode an empty image Value.");
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
  } catch (const std::invalid_argument&) {
    throw;
  } catch (const std::domain_error&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Io, "Failed to encode image artifact '" +
                                        path.string() + "': " + error.what());
  }
}

}  // namespace ps::adapters::opencv
