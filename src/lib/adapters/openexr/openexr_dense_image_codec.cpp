#include "adapters/openexr/openexr_dense_image_codec.hpp"

#include <IexBaseExc.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfPartType.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "photospider/core/graph_error.hpp"
#include "photospider/data/image_view.hpp"

/**
 * @file openexr_dense_image_codec.cpp
 * @brief Optional ordinary OpenEXR scanline codec over DenseImage Values.
 */

namespace ps::openexr_dense {
namespace Imf = OPENEXR_IMF_INTERNAL_NAMESPACE;
namespace Imath = IMATH_NAMESPACE;

namespace {

/**
 * @brief Complete uniform channel/storage classification for one input part.
 * @throws std::bad_alloc when diagnostic channel-name ownership allocates.
 */
struct DecodedFormat final {
  /** @brief Exact common file pixel type. */
  Imf::PixelType file_type = Imf::NUM_PIXELTYPES;
  /** @brief Logical semantics of the decoded DenseTensor storage. */
  ElementSemantics semantics = ElementSemantics::UnsignedInteger;
  /** @brief Concrete decoded DenseTensor storage encoding. */
  StorageEncoding storage{8U};
  /** @brief Channel names in deterministic OpenEXR channel-list order. */
  std::vector<std::string> channel_names;
};

/**
 * @brief Multiplies two sizes with exact overflow rejection.
 * @param left First factor.
 * @param right Second factor.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds `std::size_t`.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error(
        "OpenEXR DenseImage size multiplication overflows.");
  }
  return left * right;
}

/**
 * @brief Validates a filesystem path before crossing a C-string codec API.
 * @param path Caller-selected path.
 * @return Exact owned native path bytes.
 * @throws std::invalid_argument for an empty path or embedded NUL.
 * @throws std::bad_alloc when native path conversion allocates.
 */
std::string checked_path(const std::filesystem::path& path) {
  std::string native = path.string();
  if (native.empty() || native.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "OpenEXR DenseImage path must be nonempty and NUL-free.");
  }
  return native;
}

/**
 * @brief Converts an inclusive OpenEXR integer box to signed half-open bounds.
 * @param window Valid file window.
 * @return Exact signed bounds.
 * @throws std::invalid_argument for an inverted window.
 */
ImageBounds to_image_bounds(const Imath::Box2i& window) {
  if (window.max.x < window.min.x || window.max.y < window.min.y) {
    throw std::invalid_argument("OpenEXR DenseImage window is inverted.");
  }
  return {window.min.x, window.min.y,
          static_cast<std::int64_t>(window.max.x) + 1,
          static_cast<std::int64_t>(window.max.y) + 1};
}

/**
 * @brief Converts validated signed half-open bounds to one OpenEXR box.
 * @param bounds Nonempty candidate bounds.
 * @return Exact inclusive integer window.
 * @throws std::invalid_argument when an endpoint exceeds OpenEXR's int range.
 * @throws std::overflow_error from checked extent validation.
 */
Imath::Box2i to_openexr_box(const ImageBounds& bounds) {
  (void)image_bounds_width(bounds);
  (void)image_bounds_height(bounds);
  constexpr std::int64_t minimum = std::numeric_limits<int>::min();
  constexpr std::int64_t maximum = std::numeric_limits<int>::max();
  const std::int64_t inclusive_x = bounds.x_end - 1;
  const std::int64_t inclusive_y = bounds.y_end - 1;
  if (bounds.x_begin < minimum || bounds.x_begin > maximum ||
      bounds.y_begin < minimum || bounds.y_begin > maximum ||
      inclusive_x < minimum || inclusive_x > maximum || inclusive_y < minimum ||
      inclusive_y > maximum) {
    throw std::invalid_argument(
        "OpenEXR DenseImage window exceeds the file coordinate range.");
  }
  return {{static_cast<int>(bounds.x_begin), static_cast<int>(bounds.y_begin)},
          {static_cast<int>(inclusive_x), static_cast<int>(inclusive_y)}};
}

/**
 * @brief Resolves one exact caller-declared decoded-storage rule.
 * @param request Bounded strictly ordered rule table.
 * @param semantics Actual decoded element semantics.
 * @param encoding Actual decoded physical storage.
 * @return Borrowed unique rule.
 * @throws std::invalid_argument for empty, oversized, unordered, duplicate, or
 *         missing rules.
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
        "Decoded OpenEXR storage has no explicit semantic rule.");
  }
  return *found;
}

/**
 * @brief Classifies one bounded unit-sampled uniform ordinary channel list.
 * @param channels Borrowed file channel declarations.
 * @return Exact file type, decoded DenseTensor storage, and ordered names.
 * @throws std::invalid_argument for empty, oversized, sampled, mixed, or
 *         unsupported channel declarations.
 * @throws std::bad_alloc when name ownership allocates.
 * @note HALF is decoded exactly into FP32; UINT and FLOAT retain 32-bit
 *       storage. Mixed types are rejected to avoid an implicit lossy union.
 */
DecodedFormat classify_channels(const Imf::ChannelList& channels) {
  DecodedFormat result;
  for (Imf::ChannelList::ConstIterator iterator = channels.begin();
       iterator != channels.end(); ++iterator) {
    if (result.channel_names.size() >= kMaximumImageChannels) {
      throw std::invalid_argument(
          "OpenEXR DenseImage channel count exceeds the frozen bound.");
    }
    const Imf::Channel& channel = iterator.channel();
    if (channel.xSampling != 1 || channel.ySampling != 1) {
      throw std::invalid_argument(
          "OpenEXR DenseImage requires unit-sampled channels.");
    }
    if (result.channel_names.empty()) {
      result.file_type = channel.type;
    } else if (channel.type != result.file_type) {
      throw std::invalid_argument(
          "OpenEXR DenseImage requires one uniform channel storage type.");
    }
    result.channel_names.emplace_back(iterator.name());
  }
  if (result.channel_names.empty()) {
    throw std::invalid_argument("OpenEXR DenseImage has no channels.");
  }
  switch (result.file_type) {
    case Imf::UINT:
      result.semantics = ElementSemantics::UnsignedInteger;
      result.storage = StorageEncoding{32U};
      break;
    case Imf::HALF:
    case Imf::FLOAT:
      result.semantics = ElementSemantics::FloatingPoint;
      result.storage = StorageEncoding{32U};
      break;
    default:
      throw std::invalid_argument(
          "OpenEXR DenseImage channel storage is unsupported.");
  }
  return result;
}

/**
 * @brief Creates complete ordinary-image metadata from one OpenEXR header.
 * @param descriptor Valid y/x/channel DenseTensor descriptor.
 * @param header Borrowed classified file header.
 * @param channel_names Exact deterministic channel order.
 * @param endpoint Caller-declared sample meaning for decoded storage.
 * @return Complete axes, independent windows, channel IDs, and sample domain.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         std::bad_alloc from metadata construction and validation.
 * @note File names remain diagnostic; stable IDs are deterministic ordinal
 *       identities and do not infer channel roles or groups.
 */
ImageFacet make_image_facet(const DenseTensorDescriptor& descriptor,
                            const Imf::Header& header,
                            const std::vector<std::string>& channel_names,
                            const SampleEndpoint& endpoint) {
  ImageFacet facet;
  facet.x_axis = 1U;
  facet.y_axis = 0U;
  facet.channel_axis = 2U;
  facet.data_window = to_image_bounds(header.dataWindow());
  facet.display_window = to_image_bounds(header.displayWindow());
  ChannelSchema schema;
  schema.channels.reserve(channel_names.size());
  for (std::size_t index = 0U; index < channel_names.size(); ++index) {
    schema.channels.push_back(
        ChannelDescription{ChannelId{index + 1U}, channel_names[index]});
  }
  facet.channel_schema = std::move(schema);
  facet.sample_domain =
      SampleDomainFacet{1U, endpoint.encoding, endpoint.domain, {}};
  validate_dense_tensor_image_metadata(descriptor, facet);
  return facet;
}

/**
 * @brief Builds one tight positive HWC layout and exact byte envelope.
 * @param descriptor Valid y/x/channel descriptor.
 * @return Layout and exact allocation size.
 * @throws std::overflow_error when byte arithmetic overflows.
 * @throws std::invalid_argument when a stride exceeds `std::ptrdiff_t`.
 */
std::pair<StridedLayout, std::size_t> make_interleaved_layout(
    const DenseTensorDescriptor& descriptor) {
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  const std::size_t channels = descriptor.shape.at(2U);
  const std::size_t pixel_bytes = checked_multiply(channels, element_bytes);
  const std::size_t row_bytes =
      checked_multiply(descriptor.shape.at(1U), pixel_bytes);
  const std::size_t storage_size =
      checked_multiply(descriptor.shape.at(0U), row_bytes);
  const std::size_t maximum_stride =
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
  if (element_bytes > maximum_stride || pixel_bytes > maximum_stride ||
      row_bytes > maximum_stride) {
    throw std::invalid_argument(
        "OpenEXR DenseImage layout exceeds signed stride range.");
  }
  return {StridedLayout{{static_cast<std::ptrdiff_t>(row_bytes),
                         static_cast<std::ptrdiff_t>(pixel_bytes),
                         static_cast<std::ptrdiff_t>(element_bytes)},
                        0U},
          storage_size};
}

/**
 * @brief Reads all file channels into channel-major native planes.
 * @tparam Scalar UINT32 or FP32 decoded storage type.
 * @param input Open complete scanline input.
 * @param data_window Exact inclusive file data window.
 * @param channel_names Exact file channel order.
 * @param slice_type OpenEXR conversion target matching `Scalar`.
 * @param width Positive checked row width.
 * @param site_count Exact checked pixel count per channel.
 * @return Channel-major planes of `channel_count * site_count` elements.
 * @throws OpenEXR/Iex, overflow, or allocation exceptions.
 */
template <typename Scalar>
std::vector<Scalar> read_planes(Imf::InputFile* input,
                                const Imath::Box2i& data_window,
                                const std::vector<std::string>& channel_names,
                                Imf::PixelType slice_type, std::size_t width,
                                std::size_t site_count) {
  std::vector<Scalar> planes(
      checked_multiply(channel_names.size(), site_count));
  Imf::FrameBuffer frame_buffer;
  const std::size_t row_bytes = checked_multiply(width, sizeof(Scalar));
  for (std::size_t channel = 0U; channel < channel_names.size(); ++channel) {
    frame_buffer.insert(
        channel_names[channel],
        Imf::Slice::Make(slice_type, planes.data() + channel * site_count,
                         data_window, sizeof(Scalar), row_bytes));
  }
  input->setFrameBuffer(frame_buffer);
  input->readPixels(data_window.min.y, data_window.max.y);
  return planes;
}

/**
 * @brief Publishes channel-major decoded planes as one tight immutable Value.
 * @tparam Scalar Exact decoded native scalar.
 * @param descriptor Complete y/x/channel descriptor.
 * @param facet Complete independent-window image metadata.
 * @param planes Exact channel-major payload.
 * @return Fresh Ready Host-readable DenseImage Value.
 * @throws Value validation, overflow, allocation, or publication exceptions.
 */
template <typename Scalar>
Value publish_planes(DenseTensorDescriptor descriptor, ImageFacet facet,
                     const std::vector<Scalar>& planes) {
  const std::size_t height = descriptor.shape.at(0U);
  const std::size_t width = descriptor.shape.at(1U);
  const std::size_t channels = descriptor.shape.at(2U);
  const std::size_t site_count = checked_multiply(width, height);
  if (planes.size() != checked_multiply(channels, site_count) ||
      sizeof(Scalar) != dense_tensor_element_bytes(descriptor)) {
    throw std::invalid_argument(
        "OpenEXR DenseImage decoded planes disagree with metadata.");
  }
  const auto [layout, storage_size] = make_interleaved_layout(descriptor);
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      std::move(descriptor), std::move(facet), layout, storage_size);
  {
    WriteLease write = builder.acquire_write();
    for (std::size_t site = 0U; site < site_count; ++site) {
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        const Scalar& value = planes[channel * site_count + site];
        std::memcpy(write.data() + (site * channels + channel) * sizeof(Scalar),
                    &value, sizeof(Scalar));
      }
    }
  }
  return builder.seal();
}

/**
 * @brief Requires complete uniform sample metadata for direct file encode.
 * @param image Valid ordinary image Value.
 * @return Borrowed complete sample-domain facet.
 * @throws std::invalid_argument when metadata is absent or per-channel.
 */
const SampleDomainFacet& require_direct_sample_metadata(const Value& image) {
  if (!image.image_facet().has_value() ||
      !image.image_facet()->sample_domain.has_value() ||
      !image.image_facet()->sample_domain->per_channel.empty()) {
    throw std::invalid_argument(
        "Direct OpenEXR encode requires one explicit default sample domain.");
  }
  return *image.image_facet()->sample_domain;
}

/**
 * @brief Resolves diagnostic output names without assigning semantic roles.
 * @param facet Complete image facet.
 * @param channel_count Positive Value channel count.
 * @return Unique OpenEXR channel names in Value channel-axis order.
 * @throws std::invalid_argument for inconsistent, empty, NUL-containing, or
 *         duplicate declared names.
 * @throws std::bad_alloc when output ownership allocates.
 * @note When no channel schema exists, deterministic `channel.N` diagnostics
 *       are generated; these names carry no inferred role or stable identity.
 */
std::vector<std::string> output_channel_names(const ImageFacet& facet,
                                              std::size_t channel_count) {
  std::vector<std::string> names;
  names.reserve(channel_count);
  if (facet.channel_schema.has_value()) {
    if (facet.channel_schema->channels.size() != channel_count) {
      throw std::invalid_argument(
          "OpenEXR channel schema disagrees with the channel axis.");
    }
    for (const ChannelDescription& channel : facet.channel_schema->channels) {
      if (channel.diagnostic_name.empty() ||
          channel.diagnostic_name.find('\0') != std::string::npos) {
        throw std::invalid_argument(
            "OpenEXR declared channel names must be nonempty and NUL-free.");
      }
      names.push_back(channel.diagnostic_name);
    }
  } else {
    for (std::size_t channel = 0U; channel < channel_count; ++channel) {
      names.push_back("channel." + std::to_string(channel));
    }
  }
  const std::set<std::string> unique(names.begin(), names.end());
  if (unique.size() != names.size()) {
    throw std::invalid_argument("OpenEXR channel names must be unique.");
  }
  return names;
}

/**
 * @brief Copies one Value into channel-major native planes.
 * @tparam Scalar UINT32 or FP32 output storage.
 * @param view Retaining validated ordinary-image view.
 * @return Exact channel-major payload.
 * @throws std::out_of_range, std::bad_alloc, or overflow exceptions.
 */
template <typename Scalar>
std::vector<Scalar> copy_planes(const ImageView& view) {
  const std::size_t site_count = checked_multiply(view.width(), view.height());
  std::vector<Scalar> planes(checked_multiply(view.channels(), site_count));
  for (std::size_t y = 0U; y < view.height(); ++y) {
    for (std::size_t x = 0U; x < view.width(); ++x) {
      const std::size_t site = y * view.width() + x;
      for (std::size_t channel = 0U; channel < view.channels(); ++channel) {
        Scalar value{};
        std::memcpy(&value, view.channel_data(x, y, channel), sizeof(value));
        planes[channel * site_count + site] = value;
      }
    }
  }
  return planes;
}

/**
 * @brief Writes channel-major planes to one new ordinary scanline file.
 * @tparam Scalar UINT32 or FP32 input storage.
 * @param native_path Valid codec path.
 * @param view Retaining Value view with complete independent windows.
 * @param names Exact unique channel names in Value channel order.
 * @param file_type Exact OpenEXR channel type matching `Scalar`.
 * @param planes Exact channel-major payload.
 * @throws OpenEXR/Iex, invalid geometry, overflow, or allocation exceptions.
 */
template <typename Scalar>
void write_file(const std::string& native_path, const ImageView& view,
                const std::vector<std::string>& names, Imf::PixelType file_type,
                std::vector<Scalar>* planes) {
  if (!view.image_facet().display_window.has_value()) {
    throw std::invalid_argument(
        "OpenEXR encode requires an explicit independent display window.");
  }
  const Imath::Box2i data_window =
      to_openexr_box(view.image_facet().data_window);
  const Imath::Box2i display_window =
      to_openexr_box(*view.image_facet().display_window);
  if (view.height() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument(
        "OpenEXR DenseImage scan-line count exceeds int range.");
  }
  const std::size_t site_count = checked_multiply(view.width(), view.height());
  if (planes == nullptr ||
      planes->size() != checked_multiply(names.size(), site_count)) {
    throw std::invalid_argument(
        "OpenEXR DenseImage output planes disagree with metadata.");
  }
  Imf::Header header(display_window, data_window, 1.0F, Imath::V2f(0, 0), 1.0F,
                     Imf::INCREASING_Y, Imf::ZIP_COMPRESSION);
  header.setType(Imf::SCANLINEIMAGE);
  for (const std::string& name : names) {
    header.channels().insert(name, Imf::Channel(file_type, 1, 1));
  }
  Imf::FrameBuffer frame_buffer;
  const std::size_t row_bytes = checked_multiply(view.width(), sizeof(Scalar));
  for (std::size_t channel = 0U; channel < names.size(); ++channel) {
    frame_buffer.insert(
        names[channel],
        Imf::Slice::Make(file_type, planes->data() + channel * site_count,
                         data_window, sizeof(Scalar), row_bytes));
  }
  Imf::OutputFile output(native_path.c_str(), header, 0);
  output.setFrameBuffer(frame_buffer);
  output.writePixels(static_cast<int>(view.height()));
}

}  // namespace

/** @copydoc OpenExrDenseImageCodec::decode */
Value OpenExrDenseImageCodec::decode(
    const std::filesystem::path& path,
    const ImageArtifactDecodeRequest& request) const {
  const std::string native_path = checked_path(path);
  try {
    Imf::MultiPartInputFile classification(native_path.c_str(), 0, false);
    if (classification.parts() != 1 || !classification.header(0).hasType() ||
        classification.header(0).type() != Imf::SCANLINEIMAGE) {
      throw std::invalid_argument(
          "OpenEXR ordinary codec requires one scanline image part.");
    }
    Imf::InputFile input(native_path.c_str(), 0);
    if (!input.isComplete()) {
      throw GraphError(GraphErrc::Io,
                       "OpenEXR DenseImage file is incomplete: " + native_path);
    }
    const Imf::Header& header = input.header();
    const DecodedFormat format = classify_channels(header.channels());
    const ImageArtifactDecodeRule& rule =
        select_decode_rule(request, format.semantics, format.storage);
    const ImageBounds data_bounds = to_image_bounds(header.dataWindow());
    const std::size_t width = image_bounds_width(data_bounds);
    const std::size_t height = image_bounds_height(data_bounds);
    const std::size_t site_count = checked_multiply(width, height);
    DenseTensorDescriptor descriptor{
        {height, width, format.channel_names.size()},
        format.semantics,
        format.storage};
    ImageFacet facet = make_image_facet(
        descriptor, header, format.channel_names, rule.encoded_samples);
    Value decoded;
    if (format.file_type == Imf::UINT) {
      decoded =
          publish_planes(descriptor, facet,
                         read_planes<std::uint32_t>(
                             &input, header.dataWindow(), format.channel_names,
                             Imf::UINT, width, site_count));
    } else {
      decoded = publish_planes(
          descriptor, facet,
          read_planes<float>(&input, header.dataWindow(), format.channel_names,
                             Imf::FLOAT, width, site_count));
    }
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
  } catch (const std::invalid_argument&) {
    throw;
  } catch (const std::domain_error&) {
    throw;
  } catch (const std::overflow_error&) {
    throw;
  } catch (const std::length_error&) {
    throw;
  } catch (const IEX_NAMESPACE::BaseExc& error) {
    throw GraphError(GraphErrc::Io, "OpenEXR DenseImage decode failed for '" +
                                        native_path + "': " + error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Io, "OpenEXR DenseImage decode failed for '" +
                                        native_path + "': " + error.what());
  }
}

/** @copydoc OpenExrDenseImageCodec::encode */
void OpenExrDenseImageCodec::encode(
    const std::filesystem::path& path, const Value& image,
    const ImageArtifactEncodeRequest& request) const {
  const std::string native_path = checked_path(path);
  try {
    Value converted;
    const Value* selected = &image;
    if (request.conversion.has_value()) {
      converted = convert_dense_image_samples(image, *request.conversion);
      selected = &converted;
    } else {
      (void)require_direct_sample_metadata(image);
    }
    const ImageView view(*selected);
    const DenseTensorDescriptor& descriptor = view.descriptor();
    if (descriptor.quantization.has_value() ||
        descriptor.storage_encoding.kind != StorageEncodingKind::NativeScalar ||
        descriptor.storage_encoding.bit_width != 32U) {
      throw std::invalid_argument(
          "OpenEXR ordinary encode supports only unquantized UINT32 or FP32 "
          "storage.");
    }
    const std::vector<std::string> names =
        output_channel_names(view.image_facet(), view.channels());
    if (descriptor.element_semantics == ElementSemantics::UnsignedInteger) {
      std::vector<std::uint32_t> planes = copy_planes<std::uint32_t>(view);
      write_file(native_path, view, names, Imf::UINT, &planes);
    } else if (descriptor.element_semantics ==
               ElementSemantics::FloatingPoint) {
      std::vector<float> planes = copy_planes<float>(view);
      write_file(native_path, view, names, Imf::FLOAT, &planes);
    } else {
      throw std::invalid_argument(
          "OpenEXR ordinary encode does not support signed integer storage.");
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const std::invalid_argument&) {
    throw;
  } catch (const std::domain_error&) {
    throw;
  } catch (const std::overflow_error&) {
    throw;
  } catch (const std::length_error&) {
    throw;
  } catch (const IEX_NAMESPACE::BaseExc& error) {
    throw GraphError(GraphErrc::Io, "OpenEXR DenseImage encode failed for '" +
                                        native_path + "': " + error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Io, "OpenEXR DenseImage encode failed for '" +
                                        native_path + "': " + error.what());
  }
}

}  // namespace ps::openexr_dense
