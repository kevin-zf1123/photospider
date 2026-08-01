#include "adapters/openexr/openexr_deep_scanline_adapter.hpp"

#include <IexBaseExc.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfDeepScanLineInputFile.h>
#include <OpenEXR/ImfDeepScanLineOutputFile.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfTestFile.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @file openexr_deep_scanline_adapter.cpp
 * @brief Optional OpenEXR deep-scanline codec and bounded I/O submissions.
 */

namespace ps::openexr_deep {

namespace Imf = OPENEXR_IMF_INTERNAL_NAMESPACE;
namespace Imath = IMATH_NAMESPACE;

/**
 * @brief Host-owned result state written once by an admitted read callback.
 * @throws Standard synchronization/allocation exceptions from explicit use.
 */
struct OpenExrDeepReadState final {
  /** @brief Serializes decoded result publication and observation. */
  std::mutex mutex;
  /** @brief Exact immutable result, present only after successful callback
   * work. */
  std::optional<Value> value;
};

namespace {

/**
 * @brief Reports whether one path can cross OpenEXR C-string filename APIs.
 * @param path Caller-owned path argument.
 * @return True exactly when the path is nonempty and contains no embedded NUL.
 * @throws Nothing.
 * @note Read/write submission admission and direct write preflight share this
 * validation before path capture, executor work, filesystem access, or codec
 * entry.
 */
bool is_openexr_path_argument_valid(const std::string& path) noexcept {
  return !path.empty() && path.find('\0') == std::string::npos;
}

/**
 * @brief Adds two uint64 values with overflow rejection.
 * @param left First value.
 * @param right Second value.
 * @return Exact sum.
 * @throws std::overflow_error when the sum is unrepresentable.
 */
std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw std::overflow_error("OpenEXR deep uint64 addition overflows.");
  }
  return left + right;
}

/**
 * @brief Multiplies two uint64 values with overflow rejection.
 * @param left First value.
 * @param right Second value.
 * @return Exact product.
 * @throws std::overflow_error when the product is unrepresentable.
 */
std::uint64_t checked_multiply(std::uint64_t left, std::uint64_t right) {
  if (right != 0U && left > std::numeric_limits<std::uint64_t>::max() / right) {
    throw std::overflow_error("OpenEXR deep uint64 multiplication overflows.");
  }
  return left * right;
}

/**
 * @brief Converts one checked uint64 count to size_t.
 * @param value Value to convert.
 * @return Exact size_t value.
 * @throws std::overflow_error when the platform size cannot represent it.
 */
std::size_t checked_size(std::uint64_t value) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error(
        "OpenEXR deep allocation size is unrepresentable.");
  }
  return static_cast<std::size_t>(value);
}

/**
 * @brief Converts one OpenEXR inclusive integer window to signed half-open
 * form.
 * @param window Borrowed OpenEXR box.
 * @return Exact signed half-open bounds.
 * @throws std::invalid_argument for an inverted window.
 */
SignedBounds to_signed_bounds(const Imath::Box2i& window) {
  if (window.max.x < window.min.x || window.max.y < window.min.y) {
    throw std::invalid_argument("OpenEXR window is inverted.");
  }
  return {window.min.x, window.min.y,
          static_cast<std::int64_t>(window.max.x) + 1,
          static_cast<std::int64_t>(window.max.y) + 1};
}

/**
 * @brief Returns the checked width of one valid signed bounds record.
 * @param bounds Valid nonempty bounds.
 * @return Positive width.
 * @throws std::invalid_argument for malformed bounds.
 */
std::uint64_t bounds_width(const SignedBounds& bounds) {
  (void)checked_site_count(bounds);
  return static_cast<std::uint64_t>(bounds.max_x) -
         static_cast<std::uint64_t>(bounds.min_x);
}

/**
 * @brief Requires one signed half-open window to map exactly to Box2i.
 * @param bounds Valid nonempty bounds to check.
 * @throws std::invalid_argument when an inclusive endpoint is outside int.
 * @note checked_site_count must validate ordering before this helper runs.
 */
void require_openexr_window_representable(const SignedBounds& bounds) {
  const std::int64_t minimum = std::numeric_limits<int>::min();
  const std::int64_t maximum = std::numeric_limits<int>::max();
  const std::int64_t inclusive_max_x = bounds.max_x - 1;
  const std::int64_t inclusive_max_y = bounds.max_y - 1;
  if (bounds.min_x < minimum || bounds.min_x > maximum ||
      bounds.min_y < minimum || bounds.min_y > maximum ||
      inclusive_max_x < minimum || inclusive_max_x > maximum ||
      inclusive_max_y < minimum || inclusive_max_y > maximum) {
    throw std::invalid_argument(
        "OpenEXR deep signed bounds exceed the file coordinate range.");
  }
}

/**
 * @brief Validates and converts the complete write-side file geometry.
 * @param preflight Dependency-neutral signed windows.
 * @return Geometry safe for Box2i and writePixels(int) construction.
 * @throws OpenExrDeepError with UnsupportedFileShape for invalid geometry.
 * @throws std::bad_alloc when owned diagnostic storage cannot allocate.
 */
OpenExrDeepWriteGeometry make_checked_write_geometry(
    const OpenExrDeepWritePreflight& preflight) {
  try {
    (void)checked_site_count(preflight.data_window);
    (void)checked_site_count(preflight.display_window);
    require_openexr_window_representable(preflight.data_window);
    require_openexr_window_representable(preflight.display_window);
    const std::uint64_t height =
        static_cast<std::uint64_t>(preflight.data_window.max_y) -
        static_cast<std::uint64_t>(preflight.data_window.min_y);
    if (height > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      throw OpenExrDeepError(OpenExrDeepErrorCode::UnsupportedFileShape,
                             "OpenEXR deep scan-line count exceeds int range.");
    }
    return {preflight.data_window, preflight.display_window,
            bounds_width(preflight.data_window), static_cast<int>(height)};
  } catch (const OpenExrDeepError&) {
    throw;
  } catch (const std::invalid_argument& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::UnsupportedFileShape,
                           error.what());
  } catch (const std::overflow_error& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::UnsupportedFileShape,
                           error.what());
  }
}

/**
 * @brief Appends one FP32 bit pattern in private little-endian framing.
 * @param output Destination byte vector.
 * @param value Floating-point sample.
 * @throws std::bad_alloc when destination growth fails.
 */
void append_float(std::vector<std::byte>* output, float value) {
  static_assert(sizeof(float) == sizeof(std::uint32_t),
                "OpenEXR deep adapter requires 32-bit float");
  std::uint32_t bits = 0U;
  std::memcpy(&bits, &value, sizeof(bits));
  append_u32(output, bits);
}

/**
 * @brief Reads one little-endian uint32 from an already checked byte range.
 * @param data First of at least four bytes.
 * @return Decoded scalar.
 * @throws Nothing.
 */
std::uint32_t read_u32(const std::byte* data) noexcept {
  std::uint32_t value = 0U;
  for (std::uint32_t index = 0U; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (index * 8U);
  }
  return value;
}

/**
 * @brief Reads one little-endian uint64 from an already checked byte range.
 * @param data First of at least eight bytes.
 * @return Decoded scalar.
 * @throws Nothing.
 */
std::uint64_t read_u64(const std::byte* data) noexcept {
  std::uint64_t value = 0U;
  for (std::uint32_t index = 0U; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
  }
  return value;
}

/**
 * @brief Reads one little-endian FP32 sample from checked bytes.
 * @param data First of at least four bytes.
 * @return Exact floating-point bit pattern.
 * @throws Nothing.
 */
float read_float(const std::byte* data) noexcept {
  const std::uint32_t bits = read_u32(data);
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/**
 * @brief Allocates one sealed host-visible BufferHandle from exact bytes.
 * @param bytes Nonempty immutable payload.
 * @return Copyable checked allocation range.
 * @throws std::invalid_argument for empty input.
 * @throws Exceptions from Value::from_cpu_dense_tensor.
 */
BufferHandle make_buffer(std::vector<std::byte> bytes) {
  if (bytes.empty()) {
    throw std::invalid_argument(
        "OpenEXR deep semantic buffer cannot be empty.");
  }
  DenseTensorDescriptor descriptor;
  descriptor.shape = {bytes.size()};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = {8U, StorageEncodingKind::NativeScalar};
  Value owner =
      Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                   StridedLayout{{1}, 0U}, std::move(bytes));
  return owner.buffer_handle();
}

/**
 * @brief Validates logical source cardinality and derives normalized metadata.
 * @param image Dependency-neutral source data.
 * @return Complete metadata with checked site/sample counts.
 * @throws std::invalid_argument or std::overflow_error for malformed source.
 * @throws std::bad_alloc when mapping normalization allocates.
 */
DeepMetadata metadata_from_image(const OpenExrDeepImage& image) {
  DeepMetadata metadata;
  metadata.data_window = image.data_window;
  metadata.display_window = image.display_window;
  metadata.channels = normalize_channels(image.channels);
  metadata.logical_site_count = checked_site_count(image.data_window);
  (void)checked_site_count(image.display_window);
  if (image.sample_counts.size() != checked_size(metadata.logical_site_count) ||
      image.channel_samples.size() != metadata.channels.size()) {
    throw std::invalid_argument(
        "OpenEXR deep source vector cardinality is inconsistent.");
  }
  if (metadata.channels != image.channels) {
    throw std::invalid_argument(
        "OpenEXR deep source channels must use permanent-identity order.");
  }
  for (std::uint32_t count : image.sample_counts) {
    metadata.sample_count = checked_add(metadata.sample_count, count);
  }
  for (const std::vector<float>& samples : image.channel_samples) {
    if (samples.size() != checked_size(metadata.sample_count)) {
      throw std::invalid_argument(
          "OpenEXR deep channel sample length is inconsistent.");
    }
  }
  return metadata;
}

/**
 * @brief Returns one extension with an exact typed key from a descriptor.
 * @param descriptor Provider-defined descriptor.
 * @param identity Required Facet identity.
 * @return Unique matching Facet.
 * @throws std::invalid_argument when missing or duplicate.
 */
const ExtensionRecord& find_facet(const DataDescriptorEnvelope& descriptor,
                                  ExtensionIdentity identity) {
  const ExtensionRecord* result = nullptr;
  for (const ExtensionRecord& facet : descriptor.facets) {
    if (facet.identity == identity) {
      if (result != nullptr) {
        throw std::invalid_argument("OpenEXR deep Facet is duplicated.");
      }
      result = &facet;
    }
  }
  if (result == nullptr) {
    throw std::invalid_argument("OpenEXR deep required Facet is missing.");
  }
  return *result;
}

/**
 * @brief Requires one exact C++ extension key.
 * @param extension Record to check.
 * @param kind Expected typed namespace.
 * @param identity Expected permanent identity.
 * @throws std::invalid_argument for any mismatch.
 */
void require_extension(const ExtensionRecord& extension,
                       ExtensionDefinitionKind kind,
                       ExtensionIdentity identity) {
  if (extension.kind != kind || extension.identity != identity ||
      extension.structural_version != kStructuralVersion) {
    throw std::invalid_argument(
        "OpenEXR deep extension definition key is invalid.");
  }
}

/**
 * @brief Parses and cross-checks one generic Value's private metadata.
 * @param value Valid provider-defined Value.
 * @return Complete normalized metadata.
 * @throws std::invalid_argument or std::overflow_error for any mismatch.
 * @throws std::bad_alloc when mappings allocate.
 */
DeepMetadata metadata_from_value(const Value& value) {
  if (!value.valid() ||
      value.representation_kind() != ValueRepresentationKind::ProviderDefined ||
      value.storage_layout_kind() != StorageLayoutKind::ProviderDefined) {
    throw std::invalid_argument(
        "OpenEXR deep adapter requires a provider-defined Value.");
  }
  const DataDescriptorEnvelope& descriptor =
      value.provider_defined_descriptor();
  const ProviderDefinedLayout& layout = value.provider_defined_layout();
  require_extension(descriptor.schema, ExtensionDefinitionKind::Schema,
                    kVariableSampleFieldSchemaIdentity);
  if (descriptor.facets.size() != 2U) {
    throw std::invalid_argument("OpenEXR deep descriptor Facets are invalid.");
  }
  const ExtensionRecord& image_facet =
      find_facet(descriptor, kImageFacetIdentity);
  const ExtensionRecord& deep_facet =
      find_facet(descriptor, kDeepSampleFacetIdentity);
  require_extension(image_facet, ExtensionDefinitionKind::Facet,
                    kImageFacetIdentity);
  require_extension(deep_facet, ExtensionDefinitionKind::Facet,
                    kDeepSampleFacetIdentity);
  require_extension(layout.definition, ExtensionDefinitionKind::Layout,
                    kLayoutIdentity);
  DeepMetadata schema = decode_schema_payload(descriptor.schema.payload.data(),
                                              descriptor.schema.payload.size());
  const DeepMetadata image = decode_image_facet_payload(
      image_facet.payload.data(), image_facet.payload.size());
  std::vector<ChannelMapping> channels = decode_deep_facet_payload(
      deep_facet.payload.data(), deep_facet.payload.size());
  DeepMetadata layout_metadata = decode_layout_payload(
      layout.definition.payload.data(), layout.definition.payload.size());
  if (!(schema.data_window == image.data_window) ||
      !(schema.display_window == image.display_window) ||
      schema.channels.size() != channels.size() ||
      layout_metadata.logical_site_count != schema.logical_site_count ||
      layout_metadata.channels != channels) {
    throw std::invalid_argument(
        "OpenEXR deep descriptor/Layout metadata is inconsistent.");
  }
  const std::size_t expected_buffer_count =
      layout_metadata.sample_count == 0U ? 2U : channels.size() + 2U;
  if (layout.buffers.size() != expected_buffer_count ||
      value.buffer_count() != expected_buffer_count) {
    throw std::invalid_argument(
        "OpenEXR deep descriptor/Layout buffer count is inconsistent.");
  }
  schema.channels = std::move(channels);
  schema.sample_count = layout_metadata.sample_count;
  return schema;
}

/**
 * @brief Retaining exact semantic range within one provider Value buffer.
 * @throws Nothing for moves/destruction.
 */
struct RetainedSemanticBuffer final {
  /** @brief Provider read lease retaining buffer and generation. */
  ProviderReadLease lease;
  /** @brief Byte offset inside the complete lease. */
  std::size_t offset = 0U;
  /** @brief Exact semantic length. */
  std::size_t size = 0U;
};

/**
 * @brief Resolves one unique exact Layout role to a retaining read lease.
 * @param value Provider-defined Value.
 * @param role Required logical role.
 * @param expected_size Exact semantic byte length.
 * @return Retaining checked semantic range.
 * @throws std::invalid_argument for missing/duplicate/wrong-size role.
 * @throws std::out_of_range or BufferAccessError from payload access.
 */
RetainedSemanticBuffer acquire_semantic_buffer(const Value& value,
                                               std::uint32_t role,
                                               std::uint64_t expected_size) {
  const ProviderDefinedLayout& layout = value.provider_defined_layout();
  const BufferEnvelope* selected = nullptr;
  for (const BufferEnvelope& envelope : layout.buffers) {
    if (envelope.logical_role == role) {
      if (selected != nullptr) {
        throw std::invalid_argument("OpenEXR deep buffer role is duplicate.");
      }
      selected = &envelope;
    }
  }
  if (selected == nullptr || selected->length != expected_size ||
      selected->buffer_index >= value.buffer_count()) {
    throw std::invalid_argument("OpenEXR deep semantic buffer is invalid.");
  }
  ProviderReadLease lease = value.acquire_provider_read(selected->buffer_index);
  if (selected->offset > lease.size() ||
      selected->length > lease.size() - selected->offset) {
    throw std::invalid_argument("OpenEXR deep semantic range escapes storage.");
  }
  return {std::move(lease), checked_size(selected->offset),
          checked_size(selected->length)};
}

/**
 * @brief Returns the first semantic byte retained by one range.
 * @param buffer Retaining checked range.
 * @return Pointer valid for the range lifetime.
 * @throws Nothing under construction invariants.
 */
const std::byte* semantic_data(const RetainedSemanticBuffer& buffer) noexcept {
  return buffer.lease.data() + buffer.offset;
}

/**
 * @brief Validates file channels against the exact explicit mapping set.
 * @param header Borrowed OpenEXR header.
 * @param channels Normalized explicit mapping entries.
 * @throws OpenExrDeepError for missing/extra/type/sampling mismatches.
 */
void validate_file_channels(const Imf::Header& header,
                            const std::vector<ChannelMapping>& channels) {
  std::size_t declared_count = 0U;
  for (Imf::ChannelList::ConstIterator iterator = header.channels().begin();
       iterator != header.channels().end(); ++iterator) {
    ++declared_count;
  }
  if (declared_count != channels.size()) {
    throw OpenExrDeepError(
        OpenExrDeepErrorCode::UnsupportedChannel,
        "OpenEXR deep file channel count does not match explicit metadata.");
  }
  for (const ChannelMapping& mapping : channels) {
    const Imf::Channel* channel =
        header.channels().findChannel(mapping.diagnostic_name);
    if (channel == nullptr) {
      throw OpenExrDeepError(
          OpenExrDeepErrorCode::MalformedMappingMetadata,
          "OpenEXR deep explicit mapping names a missing file channel.");
    }
    if (channel->type != Imf::FLOAT || channel->xSampling != 1 ||
        channel->ySampling != 1) {
      throw OpenExrDeepError(OpenExrDeepErrorCode::UnsupportedChannel,
                             "OpenEXR deep first vertical supports only "
                             "unit-sampled FP32 channels.");
    }
  }
}

/**
 * @brief Creates a deep frame-buffer count slice with checked signed origin.
 * @param counts Nonempty native sample-count storage.
 * @param data_window OpenEXR inclusive data window.
 * @param width Positive checked pixel width.
 * @return OpenEXR slice whose absolute coordinates address counts.
 * @throws OpenEXR/Iex exceptions for unsupported addressing.
 */
Imf::Slice make_count_slice(std::vector<std::uint32_t>* counts,
                            const Imath::Box2i& data_window,
                            std::uint64_t width) {
  return Imf::Slice::Make(
      Imf::UINT, counts->data(), data_window, sizeof(std::uint32_t),
      checked_size(checked_multiply(width, sizeof(std::uint32_t))));
}

/**
 * @brief Creates one DeepSlice over a row-major grid of sample pointers.
 * @param pointers Nonempty site-sized pointer grid.
 * @param data_window OpenEXR inclusive data window.
 * @param width Positive checked pixel width.
 * @return FP32 DeepSlice with one native float per sample.
 * @throws OpenEXR/Iex exceptions for unsupported addressing.
 */
Imf::DeepSlice make_deep_slice(std::vector<float*>* pointers,
                               const Imath::Box2i& data_window,
                               std::uint64_t width) {
  const Imf::Slice pointer_grid =
      Imf::Slice::Make(Imf::UINT, pointers->data(), data_window, sizeof(float*),
                       checked_size(checked_multiply(width, sizeof(float*))));
  return {Imf::FLOAT, pointer_grid.base, pointer_grid.xStride,
          pointer_grid.yStride, sizeof(float)};
}

/**
 * @brief Builds per-site sample pointers for all normalized channels.
 * @param offsets Native prefix offsets with site_count+1 entries.
 * @param samples One equal-length native sample stream per channel.
 * @param empty_sentinels One callback-local addressable float per channel.
 * @return Channel-major site pointer grids.
 * @throws std::invalid_argument for inconsistent ranges.
 * @throws std::bad_alloc when pointer grids cannot allocate.
 * @note Empty-stream sentinels remain callback-local and OpenEXR never reads
 * them because every corresponding site count is zero.
 */
std::vector<std::vector<float*>> make_sample_pointers(
    const std::vector<std::uint64_t>& offsets,
    std::vector<std::vector<float>>* samples,
    std::vector<float>* empty_sentinels) {
  if (offsets.size() < 2U || samples == nullptr || samples->empty() ||
      empty_sentinels == nullptr ||
      empty_sentinels->size() != samples->size()) {
    throw std::invalid_argument(
        "OpenEXR deep sample pointer input is invalid.");
  }
  const std::size_t site_count = offsets.size() - 1U;
  for (std::size_t site = 0U; site < site_count; ++site) {
    if (offsets[site] > offsets[site + 1U]) {
      throw std::invalid_argument(
          "OpenEXR deep prefix offsets are not monotonic.");
    }
  }
  for (const std::vector<float>& channel_samples : *samples) {
    if (offsets.back() != channel_samples.size()) {
      throw std::invalid_argument(
          "OpenEXR deep terminal offset disagrees with channel samples.");
    }
  }
  std::vector<std::vector<float*>> pointers(samples->size(),
                                            std::vector<float*>(site_count));
  for (std::size_t channel = 0U; channel < samples->size(); ++channel) {
    for (std::size_t site = 0U; site < site_count; ++site) {
      if (offsets[site] > (*samples)[channel].size()) {
        throw std::invalid_argument(
            "OpenEXR deep prefix offset escapes channel samples.");
      }
      pointers[channel][site] =
          offsets.back() == 0U
              ? &(*empty_sentinels)[channel]
              : (*samples)[channel].data() + checked_size(offsets[site]);
    }
  }
  return pointers;
}

/**
 * @brief Decodes one already-classified single-part deep-scanline file.
 * @param registry Injected data-definition authority.
 * @param path Nonempty, embedded-NUL-free input path validated at submission.
 * @param hooks Optional deterministic I/O-worker hooks.
 * @return Validated provider-defined Value.
 * @throws OpenExrDeepError with only Host-owned types.
 * @note The submission boundary rejects malformed paths before this worker
 * helper can invoke hooks, access the filesystem, or enter OpenEXR.
 */
Value read_file(DataDefinitionRegistry& registry, const std::string& path,
                const OpenExrDeepIoHooks& hooks) {
  try {
    if (hooks.before_codec) {
      hooks.before_codec();
    }
    if (!Imf::isOpenExrFile(path.c_str())) {
      throw OpenExrDeepError(OpenExrDeepErrorCode::UnsupportedFileShape,
                             "Input is not an OpenEXR file.");
    }
    {
      Imf::MultiPartInputFile classification(path.c_str(), 0, false);
      if (classification.parts() != 1 || !classification.header(0).hasType() ||
          classification.header(0).type() != Imf::DEEPSCANLINE) {
        throw OpenExrDeepError(
            OpenExrDeepErrorCode::UnsupportedFileShape,
            "Only single-part deep-scanline OpenEXR input is supported.");
      }
    }
    Imf::DeepScanLineInputFile input(path.c_str(), 0);
    if (!input.isComplete()) {
      throw OpenExrDeepError(OpenExrDeepErrorCode::CorruptOrIncompleteFile,
                             "OpenEXR deep-scanline file is incomplete.");
    }
    const Imf::Header& header = input.header();
    if (!header.hasType() || header.type() != Imf::DEEPSCANLINE) {
      throw OpenExrDeepError(OpenExrDeepErrorCode::UnsupportedFileShape,
                             "OpenEXR part type is not deep scanline.");
    }
    const Imf::StringAttribute* mapping_attribute =
        header.findTypedAttribute<Imf::StringAttribute>(kMappingAttributeName);
    if (mapping_attribute == nullptr) {
      if (header.find(kMappingAttributeName) == header.end()) {
        throw OpenExrDeepError(
            OpenExrDeepErrorCode::MissingMappingMetadata,
            "OpenEXR deep file has no explicit Photospider channel mapping.");
      }
      throw OpenExrDeepError(
          OpenExrDeepErrorCode::MalformedMappingMetadata,
          "OpenEXR deep mapping attribute has the wrong type.");
    }
    std::vector<ChannelMapping> channels;
    try {
      channels = decode_mapping_attribute(mapping_attribute->value());
    } catch (const std::exception& error) {
      throw OpenExrDeepError(OpenExrDeepErrorCode::MalformedMappingMetadata,
                             error.what());
    }
    validate_file_channels(header, channels);

    const Imath::Box2i data_window = header.dataWindow();
    const SignedBounds signed_data_window = to_signed_bounds(data_window);
    const std::uint64_t site_count = checked_site_count(signed_data_window);
    const std::uint64_t width = bounds_width(signed_data_window);
    std::vector<std::uint32_t> counts(checked_size(site_count), 0U);
    Imf::DeepFrameBuffer frame_buffer;
    frame_buffer.insertSampleCountSlice(
        make_count_slice(&counts, data_window, width));
    input.setFrameBuffer(frame_buffer);
    input.readPixelSampleCounts(data_window.min.y, data_window.max.y);

    std::vector<std::uint64_t> offsets(counts.size() + 1U, 0U);
    for (std::size_t site = 0U; site < counts.size(); ++site) {
      offsets[site + 1U] = checked_add(offsets[site], counts[site]);
    }
    const std::uint64_t sample_count = offsets.back();
    std::vector<std::vector<float>> samples(
        channels.size(), std::vector<float>(checked_size(sample_count), 0.0F));
    std::vector<float> empty_sentinels(channels.size(), 0.0F);
    std::vector<std::vector<float*>> pointers =
        make_sample_pointers(offsets, &samples, &empty_sentinels);
    for (std::size_t channel = 0U; channel < channels.size(); ++channel) {
      frame_buffer.insert(
          channels[channel].diagnostic_name,
          make_deep_slice(&pointers[channel], data_window, width));
    }
    input.setFrameBuffer(frame_buffer);
    input.readPixels(data_window.min.y, data_window.max.y);

    OpenExrDeepImage image;
    image.data_window = signed_data_window;
    image.display_window = to_signed_bounds(header.displayWindow());
    image.channels = std::move(channels);
    image.sample_counts = std::move(counts);
    image.channel_samples = std::move(samples);
    return make_openexr_deep_value(registry, image);
  } catch (const OpenExrDeepError&) {
    throw;
  } catch (const ExtensionContractError& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::ProviderContractFailure,
                           error.what());
  } catch (const std::bad_alloc&) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::AllocationFailure,
                           "OpenEXR deep read allocation failed.");
  } catch (const IEX_NAMESPACE::BaseExc& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::CorruptOrIncompleteFile,
                           error.what());
  } catch (const std::exception& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::MalformedMappingMetadata,
                           error.what());
  } catch (...) {
    throw OpenExrDeepError(
        OpenExrDeepErrorCode::CodecFailure,
        "OpenEXR deep read caught an unknown codec failure.");
  }
}

/**
 * @brief Encodes one validated generic deep Value to a new OpenEXR file.
 * @param value Exact provider-defined deep Value.
 * @param path Nonempty, embedded-NUL-free output path validated at submission.
 * @param hooks Optional deterministic I/O-worker hooks.
 * @throws OpenExrDeepError with only Host-owned types.
 * @note The submission boundary rejects malformed paths before this worker
 * helper can inspect the Value, invoke hooks, or enter OpenEXR; the production
 * preflight revalidates the same path contract before output preparation.
 */
void write_file(const Value& value, const std::string& path,
                const OpenExrDeepIoHooks& hooks) {
  try {
    OpenExrDeepImage image = inspect_openexr_deep_value(value);
    const DeepMetadata metadata = metadata_from_image(image);
    run_openexr_deep_write_preflight(
        {image.data_window, image.display_window}, path,
        [&image, &metadata, &hooks](const OpenExrDeepWriteGeometry& geometry,
                                    const std::string& output_path) {
          const Imath::Box2i data_window{
              {static_cast<int>(geometry.data_window.min_x),
               static_cast<int>(geometry.data_window.min_y)},
              {static_cast<int>(geometry.data_window.max_x - 1),
               static_cast<int>(geometry.data_window.max_y - 1)}};
          const Imath::Box2i display_window{
              {static_cast<int>(geometry.display_window.min_x),
               static_cast<int>(geometry.display_window.min_y)},
              {static_cast<int>(geometry.display_window.max_x - 1),
               static_cast<int>(geometry.display_window.max_y - 1)}};
          Imf::Header header(display_window, data_window, 1.0F,
                             Imath::V2f(0, 0), 1.0F, Imf::INCREASING_Y,
                             Imf::ZIPS_COMPRESSION);
          header.setType(Imf::DEEPSCANLINE);
          for (const ChannelMapping& channel : metadata.channels) {
            header.channels().insert(channel.diagnostic_name,
                                     Imf::Channel(Imf::FLOAT, 1, 1));
          }
          header.insert(kMappingAttributeName,
                        Imf::StringAttribute(
                            encode_mapping_attribute(metadata.channels)));

          std::vector<std::uint64_t> offsets(image.sample_counts.size() + 1U,
                                             0U);
          for (std::size_t site = 0U; site < image.sample_counts.size();
               ++site) {
            offsets[site + 1U] =
                checked_add(offsets[site], image.sample_counts[site]);
          }
          std::vector<float> empty_sentinels(metadata.channels.size(), 0.0F);
          std::vector<std::vector<float*>> pointers = make_sample_pointers(
              offsets, &image.channel_samples, &empty_sentinels);
          Imf::DeepFrameBuffer frame_buffer;
          frame_buffer.insertSampleCountSlice(make_count_slice(
              &image.sample_counts, data_window, geometry.width));
          for (std::size_t channel = 0U; channel < metadata.channels.size();
               ++channel) {
            frame_buffer.insert(metadata.channels[channel].diagnostic_name,
                                make_deep_slice(&pointers[channel], data_window,
                                                geometry.width));
          }
          if (hooks.before_codec) {
            hooks.before_codec();
          }
          Imf::DeepScanLineOutputFile output(output_path.c_str(), header, 0);
          output.setFrameBuffer(frame_buffer);
          output.writePixels(geometry.scan_line_count);
        });
  } catch (const OpenExrDeepError&) {
    throw;
  } catch (const ExtensionContractError& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::ProviderContractFailure,
                           error.what());
  } catch (const std::bad_alloc&) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::AllocationFailure,
                           "OpenEXR deep write allocation failed.");
  } catch (const IEX_NAMESPACE::BaseExc& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::CodecFailure, error.what());
  } catch (const std::exception& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::MalformedMappingMetadata,
                           error.what());
  } catch (...) {
    throw OpenExrDeepError(
        OpenExrDeepErrorCode::CodecFailure,
        "OpenEXR deep write caught an unknown codec failure.");
  }
}

}  // namespace

/** @copydoc run_openexr_deep_write_preflight */
void run_openexr_deep_write_preflight(
    const OpenExrDeepWritePreflight& preflight, const std::string& path,
    const OpenExrDeepWriteOperation& operation) {
  if (!is_openexr_path_argument_valid(path) || !operation) {
    throw OpenExrDeepError(
        OpenExrDeepErrorCode::InvalidRequest,
        "OpenEXR deep write preflight requires a nonempty, embedded-NUL-free "
        "path and a continuation.");
  }
  const OpenExrDeepWriteGeometry geometry =
      make_checked_write_geometry(preflight);
  operation(geometry, path);
}

/** @copydoc make_openexr_deep_value */
Value make_openexr_deep_value(DataDefinitionRegistry& registry,
                              const OpenExrDeepImage& image) {
  try {
    const DeepMetadata metadata = metadata_from_image(image);
    std::vector<std::byte> counts;
    counts.reserve(checked_size(
        checked_multiply(metadata.logical_site_count, sizeof(std::uint32_t))));
    std::vector<std::byte> offsets;
    offsets.reserve(checked_size(checked_multiply(
        checked_add(metadata.logical_site_count, 1U), sizeof(std::uint64_t))));
    append_u64(&offsets, 0U);
    std::uint64_t running = 0U;
    for (std::uint32_t count : image.sample_counts) {
      append_u32(&counts, count);
      running = checked_add(running, count);
      append_u64(&offsets, running);
    }
    std::vector<BufferHandle> buffers;
    buffers.reserve(metadata.channels.size() + 2U);
    buffers.push_back(make_buffer(std::move(counts)));
    buffers.push_back(make_buffer(std::move(offsets)));
    if (metadata.sample_count != 0U) {
      for (const std::vector<float>& channel_samples : image.channel_samples) {
        std::vector<std::byte> samples;
        samples.reserve(checked_size(
            checked_multiply(metadata.sample_count, sizeof(float))));
        for (float sample : channel_samples) {
          append_float(&samples, sample);
        }
        buffers.push_back(make_buffer(std::move(samples)));
      }
    }
    return Value::from_provider_defined(registry, make_descriptor(metadata),
                                        make_layout(metadata),
                                        std::move(buffers));
  } catch (const OpenExrDeepError&) {
    throw;
  } catch (const ExtensionContractError& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::ProviderContractFailure,
                           error.what());
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::InvalidRequest, error.what());
  }
}

/** @copydoc inspect_openexr_deep_value */
OpenExrDeepImage inspect_openexr_deep_value(const Value& value) {
  try {
    const DeepMetadata metadata = metadata_from_value(value);
    const std::uint64_t count_bytes =
        checked_multiply(metadata.logical_site_count, sizeof(std::uint32_t));
    const std::uint64_t offset_bytes = checked_multiply(
        checked_add(metadata.logical_site_count, 1U), sizeof(std::uint64_t));
    const std::uint64_t sample_bytes =
        checked_multiply(metadata.sample_count, sizeof(float));
    const RetainedSemanticBuffer counts =
        acquire_semantic_buffer(value, kCountsBufferRole, count_bytes);
    const RetainedSemanticBuffer offsets =
        acquire_semantic_buffer(value, kOffsetsBufferRole, offset_bytes);

    OpenExrDeepImage image;
    image.data_window = metadata.data_window;
    image.display_window = metadata.display_window;
    image.channels = metadata.channels;
    image.sample_counts.resize(checked_size(metadata.logical_site_count));
    const std::byte* count_data = semantic_data(counts);
    const std::byte* offset_data = semantic_data(offsets);
    if (read_u64(offset_data) != 0U) {
      throw std::invalid_argument("OpenEXR deep offsets do not begin at zero.");
    }
    std::uint64_t running = 0U;
    for (std::size_t site = 0U; site < image.sample_counts.size(); ++site) {
      image.sample_counts[site] = read_u32(count_data + site * 4U);
      running = checked_add(running, image.sample_counts[site]);
      if (read_u64(offset_data + (site + 1U) * 8U) != running) {
        throw std::invalid_argument(
            "OpenEXR deep offsets disagree with sample counts.");
      }
    }
    if (running != metadata.sample_count) {
      throw std::invalid_argument(
          "OpenEXR deep terminal sample count is inconsistent.");
    }
    image.channel_samples.reserve(metadata.channels.size());
    for (const ChannelMapping& channel : metadata.channels) {
      if (metadata.sample_count == 0U) {
        image.channel_samples.emplace_back();
        continue;
      }
      RetainedSemanticBuffer samples =
          acquire_semantic_buffer(value, channel.buffer_role, sample_bytes);
      const std::byte* sample_data = semantic_data(samples);
      std::vector<float> values(checked_size(metadata.sample_count));
      for (std::size_t index = 0U; index < values.size(); ++index) {
        values[index] = read_float(sample_data + index * 4U);
      }
      image.channel_samples.push_back(std::move(values));
    }
    return image;
  } catch (const OpenExrDeepError&) {
    throw;
  } catch (const ExtensionContractError& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::ProviderContractFailure,
                           error.what());
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::MalformedMappingMetadata,
                           error.what());
  }
}

/** @copydoc OpenExrDeepReadSubmission::wait */
Value OpenExrDeepReadSubmission::wait() const {
  if (!io_submission_.accepted() || state_ == nullptr) {
    throw std::logic_error("OpenEXR deep read submission was not accepted.");
  }
  const execution::ComputeIoTaskResult result =
      io_submission_.completion().wait();
  if (result.status() == execution::ComputeIoCompletionStatus::Cancelled) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::Cancelled,
                           "OpenEXR deep read was cancelled.");
  }
  result.rethrow_if_failed();
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->value.has_value()) {
    throw OpenExrDeepError(OpenExrDeepErrorCode::MissingResult,
                           "OpenEXR deep read completed without a Value.");
  }
  return *state_->value;
}

/** @copydoc submit_openexr_deep_read */
OpenExrDeepReadSubmission submit_openexr_deep_read(
    execution::ComputeIoExecutor& executor,
    std::shared_ptr<DataDefinitionRegistry> registry, const std::string& path,
    std::uint64_t planned_bytes,
    const std::shared_ptr<const void>& transaction_lifetime,
    const OpenExrDeepIoHooks& hooks) {
  if (registry == nullptr || !is_openexr_path_argument_valid(path)) {
    return {};
  }
  std::shared_ptr<OpenExrDeepReadState> state;
  const auto factory = [&registry, &path, &hooks,
                        &state]() -> execution::ComputeIoExecutor::Task {
    state = std::make_shared<OpenExrDeepReadState>();
    const std::shared_ptr<DataDefinitionRegistry> retained_registry = registry;
    const std::string retained_path = path;
    const OpenExrDeepIoHooks retained_hooks = hooks;
    const std::shared_ptr<OpenExrDeepReadState> retained_state = state;
    return
        [retained_registry, retained_path, retained_hooks, retained_state]() {
          Value value =
              read_file(*retained_registry, retained_path, retained_hooks);
          if (retained_hooks.before_read_publication) {
            retained_hooks.before_read_publication();
          }
          std::lock_guard<std::mutex> lock(retained_state->mutex);
          retained_state->value = std::move(value);
        };
  };
  execution::ComputeIoSubmission submission =
      executor.try_submit(planned_bytes, transaction_lifetime, factory);
  return OpenExrDeepReadSubmission(std::move(submission), std::move(state));
}

/** @copydoc submit_openexr_deep_write */
execution::ComputeIoSubmission submit_openexr_deep_write(
    execution::ComputeIoExecutor& executor, const Value& value,
    const std::string& path, std::uint64_t planned_bytes,
    const std::shared_ptr<const void>& transaction_lifetime,
    const OpenExrDeepIoHooks& hooks) {
  if (!value.valid() || !is_openexr_path_argument_valid(path)) {
    return {};
  }
  const auto factory = [&value, &path,
                        &hooks]() -> execution::ComputeIoExecutor::Task {
    const Value retained_value = value;
    const std::string retained_path = path;
    const OpenExrDeepIoHooks retained_hooks = hooks;
    return [retained_value, retained_path, retained_hooks]() {
      write_file(retained_value, retained_path, retained_hooks);
    };
  };
  return executor.try_submit(planned_bytes, transaction_lifetime, factory);
}

}  // namespace ps::openexr_deep
