#include "core/cpu_dense_image_operation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/graph_error.hpp"
#include "photospider/core/image_buffer.hpp"

namespace ps::ops {
namespace {

/**
 * @brief Multiplies operation byte counts with explicit overflow checking.
 *
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @param diagnostic Stable overflow diagnostic.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds std::size_t.
 * @note The helper performs no allocation and accepts zero operands.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right,
                             const char* diagnostic) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error(diagnostic);
  }
  return left * right;
}

/**
 * @brief Adds operation byte counts with explicit overflow checking.
 *
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @param diagnostic Stable overflow diagnostic.
 * @return Exact sum.
 * @throws std::overflow_error when the sum exceeds std::size_t.
 * @note The helper performs no allocation.
 */
std::size_t checked_add(std::size_t left, std::size_t right,
                        const char* diagnostic) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error(diagnostic);
  }
  return left + right;
}

/**
 * @brief Converts one current ImageBuffer storage type to V-2 element facts.
 *
 * @param type Valid current ImageBuffer channel storage type.
 * @return Matching logical semantics and physical bit width.
 * @throws std::invalid_argument for an unknown DataType value.
 * @note The conversion is private to the bounded current-product edge.
 */
std::pair<ElementSemantics, StorageEncoding> dense_element_from_image_type(
    DataType type) {
  switch (type) {
    case DataType::UINT8:
      return {ElementSemantics::UnsignedInteger, StorageEncoding{8U}};
    case DataType::INT8:
      return {ElementSemantics::SignedInteger, StorageEncoding{8U}};
    case DataType::UINT16:
      return {ElementSemantics::UnsignedInteger, StorageEncoding{16U}};
    case DataType::INT16:
      return {ElementSemantics::SignedInteger, StorageEncoding{16U}};
    case DataType::FLOAT32:
      return {ElementSemantics::FloatingPoint, StorageEncoding{32U}};
    case DataType::FLOAT64:
      return {ElementSemantics::FloatingPoint, StorageEncoding{64U}};
  }
  throw std::invalid_argument("ImageBuffer DataType is not declared.");
}

/**
 * @brief Converts supported V-2 element facts to a current ImageBuffer type.
 *
 * @param descriptor Valid V-2 DenseTensor descriptor.
 * @return Equivalent current ImageBuffer channel type.
 * @throws std::invalid_argument when the element combination is unsupported.
 * @note The current type vocabulary exactly covers the V-2 element matrix.
 */
DataType image_type_from_dense_element(
    const DenseTensorDescriptor& descriptor) {
  const std::uint32_t bit_width = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (bit_width == 8U) {
        return DataType::UINT8;
      }
      if (bit_width == 16U) {
        return DataType::UINT16;
      }
      break;
    case ElementSemantics::SignedInteger:
      if (bit_width == 8U) {
        return DataType::INT8;
      }
      if (bit_width == 16U) {
        return DataType::INT16;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (bit_width == 32U) {
        return DataType::FLOAT32;
      }
      if (bit_width == 64U) {
        return DataType::FLOAT64;
      }
      break;
  }
  throw std::invalid_argument(
      "DenseTensor element cannot be adapted to ImageBuffer.");
}

/**
 * @brief Copies one current CPU image into an immutable DenseTensor snapshot.
 *
 * @param buffer Current image descriptor and payload to snapshot.
 * @return Value with shape [height, width, channels], explicit y/x/channel
 *         axes, and the same positive row stride.
 * @throws std::invalid_argument for malformed, non-CPU, or unrepresentable
 *         current descriptors.
 * @throws std::overflow_error for unrepresentable envelope arithmetic.
 * @throws std::bad_alloc when snapshot allocation fails.
 * @note Only active row bytes are read. Inter-row padding in the exact Value
 *       envelope is initialized independently, and trailing last-row padding
 *       is neither represented nor read.
 */
Value snapshot_image_buffer(const ImageBuffer& buffer) {
  validate_image_buffer(buffer);
  if (buffer.device != Device::CPU || buffer.width <= 0 || buffer.height <= 0 ||
      buffer.channels <= 0) {
    throw std::invalid_argument(
        "Dense image operation requires a nonempty CPU ImageBuffer.");
  }
  if (buffer.step >
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::invalid_argument(
        "ImageBuffer row stride exceeds the V-2 signed stride domain.");
  }

  const std::size_t element_bytes = image_buffer_bytes_per_channel(buffer.type);
  const std::size_t pixel_bytes =
      checked_multiply(static_cast<std::size_t>(buffer.channels), element_bytes,
                       "ImageBuffer pixel size exceeds size_t.");
  if (pixel_bytes > static_cast<std::size_t>(
                        std::numeric_limits<std::ptrdiff_t>::max()) ||
      element_bytes > static_cast<std::size_t>(
                          std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::invalid_argument(
        "ImageBuffer element stride exceeds the V-2 signed stride domain.");
  }
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  const std::size_t preceding_rows =
      checked_multiply(static_cast<std::size_t>(buffer.height - 1), buffer.step,
                       "ImageBuffer snapshot row offsets exceed size_t.");
  const std::size_t storage_size =
      checked_add(preceding_rows, row_bytes,
                  "ImageBuffer snapshot envelope exceeds size_t.");
  std::vector<std::byte> storage(storage_size, std::byte{0});
  for (int row = 0; row < buffer.height; ++row) {
    std::memcpy(storage.data() + static_cast<std::size_t>(row) * buffer.step,
                image_buffer_row_data(buffer, row), row_bytes);
  }

  const auto [semantics, encoding] = dense_element_from_image_type(buffer.type);
  DenseTensorDescriptor descriptor{{static_cast<std::size_t>(buffer.height),
                                    static_cast<std::size_t>(buffer.width),
                                    static_cast<std::size_t>(buffer.channels)},
                                   semantics,
                                   encoding};
  ImageFacet image;
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;
  StridedLayout layout{{static_cast<std::ptrdiff_t>(buffer.step),
                        static_cast<std::ptrdiff_t>(pixel_bytes),
                        static_cast<std::ptrdiff_t>(element_bytes)}};
  return Value::from_cpu_dense_tensor(std::move(descriptor), image,
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Extracts one logical image descriptor without exposing payload bytes.
 *
 * @param view Valid retaining checked image view.
 * @return Complete logical DenseTensor and ImageFacet facts.
 * @throws std::bad_alloc when copying tensor shape allocates and fails.
 * @note Physical strides and Value ownership are intentionally omitted.
 */
DenseImageDescriptor logical_descriptor(const ImageView& view) {
  return DenseImageDescriptor{view.descriptor(), view.image_facet()};
}

/**
 * @brief Validates a descriptor-only image contract for pure inference.
 *
 * @param descriptor Logical image descriptor to inspect.
 * @throws std::invalid_argument for unsupported element facts, malformed shape
 *         or facet axes, non-singleton unassigned axes, or current-adapter
 *         extent overflow.
 * @note The function reads no payload, layout, graph, registry, or device
 *       state.
 */
void validate_logical_image_descriptor(const DenseImageDescriptor& descriptor) {
  (void)dense_tensor_element_bytes(descriptor.tensor);
  const std::vector<std::size_t>& shape = descriptor.tensor.shape;
  if (shape.empty()) {
    throw std::invalid_argument(
        "Dense image descriptor requires positive rank.");
  }
  for (const std::size_t extent : shape) {
    if (extent == 0U) {
      throw std::invalid_argument(
          "Dense image descriptor extents must be positive.");
    }
  }
  const ImageFacet& image = descriptor.image;
  if (image.x_axis >= shape.size() || image.y_axis >= shape.size() ||
      image.x_axis == image.y_axis) {
    throw std::invalid_argument("Dense image descriptor has invalid x/y axes.");
  }
  if (image.channel_axis.has_value() && (*image.channel_axis >= shape.size() ||
                                         *image.channel_axis == image.x_axis ||
                                         *image.channel_axis == image.y_axis)) {
    throw std::invalid_argument(
        "Dense image descriptor has invalid channel axis.");
  }
  const std::size_t maximum_extent =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  for (std::size_t axis = 0U; axis < shape.size(); ++axis) {
    const bool assigned =
        axis == image.x_axis || axis == image.y_axis ||
        (image.channel_axis.has_value() && axis == *image.channel_axis);
    if (!assigned && shape[axis] != 1U) {
      throw std::invalid_argument(
          "Dense image descriptor unassigned axes must be singleton.");
    }
  }
  const std::size_t channels =
      image.channel_axis.has_value() ? shape[*image.channel_axis] : 1U;
  if (shape[image.x_axis] > maximum_extent ||
      shape[image.y_axis] > maximum_extent || channels > maximum_extent) {
    throw std::invalid_argument(
        "Dense image descriptor exceeds current adapter extent limits.");
  }
}

/**
 * @brief Reports whether an ImageView layout is current-buffer adaptable.
 *
 * @param view Valid immutable result view.
 * @return True for positive row stride and contiguous interleaved x/channel
 *         element strides.
 * @throws std::overflow_error when active-row multiplication exceeds size_t.
 * @note Singleton unassigned axes were already checked by ImageView.
 */
bool has_interleaved_image_layout(const ImageView& view) {
  const StridedLayout& layout = view.layout();
  const ImageFacet& image = view.image_facet();
  const std::size_t element_bytes = view.element_bytes();
  const std::size_t pixel_bytes = checked_multiply(
      view.channels(), element_bytes, "Dense image pixel size exceeds size_t.");
  const std::size_t row_bytes = checked_multiply(
      view.width(), pixel_bytes, "Dense image row size exceeds size_t.");
  if (layout.byte_strides[image.x_axis] !=
          static_cast<std::ptrdiff_t>(pixel_bytes) ||
      view.row_stride() < 0 ||
      static_cast<std::size_t>(view.row_stride()) < row_bytes) {
    return false;
  }
  if (image.channel_axis.has_value() &&
      layout.byte_strides[*image.channel_axis] !=
          static_cast<std::ptrdiff_t>(element_bytes)) {
    return false;
  }
  return true;
}

/**
 * @brief Copies a validated immutable result into a current NodeOutput.
 *
 * @param value Execute result to validate and publish.
 * @param inferred Exact logical descriptor returned by inference.
 * @return Newly allocated validated CPU ImageBuffer result.
 * @throws std::invalid_argument or std::overflow_error for descriptor, facet,
 *         layout, or current-adapter mismatches.
 * @throws std::bad_alloc when current output allocation fails.
 * @note The output Value is retained by ImageView until all active bytes have
 *       been copied; no shared owner crosses into NodeOutput.
 */
NodeOutput publish_image_value(Value value,
                               const DenseImageDescriptor& inferred) {
  ImageView view(std::move(value));
  if (!(logical_descriptor(view) == inferred)) {
    throw std::invalid_argument(
        "Dense image execute result disagrees with inference.");
  }
  if (!has_interleaved_image_layout(view)) {
    throw std::invalid_argument(
        "Dense image execute result is not interleaved-adaptable.");
  }

  const DataType type = image_type_from_dense_element(view.descriptor());
  NodeOutput output;
  output.image_buffer = make_aligned_cpu_image_buffer(
      static_cast<int>(view.width()), static_cast<int>(view.height()),
      static_cast<int>(view.channels()), type);
  const std::size_t row_bytes = image_buffer_row_bytes(output.image_buffer);
  auto* output_base = static_cast<std::byte*>(output.image_buffer.data.get());
  for (std::size_t row = 0U; row < view.height(); ++row) {
    std::memcpy(output_base + row * output.image_buffer.step,
                view.channel_data(0U, row, 0U), row_bytes);
  }
  validate_image_buffer(output.image_buffer);
  return output;
}

/**
 * @brief Rounds an active row size up to a fixed power-of-two alignment.
 *
 * @param row_bytes Positive active row size.
 * @param alignment Positive power-of-two alignment.
 * @return Aligned row stride.
 * @throws std::invalid_argument when alignment is not a power of two.
 * @throws std::overflow_error when rounding exceeds std::size_t.
 * @note The dense invert operation uses 64-byte rows to exercise padding.
 */
std::size_t aligned_row_stride(std::size_t row_bytes, std::size_t alignment) {
  if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
    throw std::invalid_argument(
        "Dense image row alignment must be a power of two.");
  }
  return checked_add(row_bytes, alignment - 1U,
                     "Dense image aligned row size exceeds size_t.") &
         ~(alignment - 1U);
}

/**
 * @brief Builds an interleaved padded layout for one logical image descriptor.
 *
 * @param descriptor Valid logical output descriptor.
 * @return Positive signed strides with contiguous channel/x axes and a
 *         64-byte-aligned y axis.
 * @throws std::invalid_argument when a stride exceeds ptrdiff_t.
 * @throws std::overflow_error when row arithmetic exceeds size_t.
 * @throws std::bad_alloc when stride-vector allocation fails.
 * @note Unassigned singleton axes receive one element-byte stride.
 */
StridedLayout make_interleaved_layout(const DenseImageDescriptor& descriptor) {
  const std::size_t element_bytes =
      dense_tensor_element_bytes(descriptor.tensor);
  const std::vector<std::size_t>& shape = descriptor.tensor.shape;
  const ImageFacet& image = descriptor.image;
  const std::size_t channels =
      image.channel_axis.has_value() ? shape[*image.channel_axis] : 1U;
  const std::size_t pixel_bytes = checked_multiply(
      channels, element_bytes, "Dense image pixel size exceeds size_t.");
  const std::size_t row_bytes = checked_multiply(
      shape[image.x_axis], pixel_bytes, "Dense image row size exceeds size_t.");
  const std::size_t row_stride = aligned_row_stride(row_bytes, 64U);
  const std::size_t maximum_stride =
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
  if (element_bytes > maximum_stride || pixel_bytes > maximum_stride ||
      row_stride > maximum_stride) {
    throw std::invalid_argument(
        "Dense image layout exceeds signed stride representation.");
  }

  StridedLayout layout;
  layout.byte_strides.assign(shape.size(),
                             static_cast<std::ptrdiff_t>(element_bytes));
  layout.byte_strides[image.x_axis] = static_cast<std::ptrdiff_t>(pixel_bytes);
  layout.byte_strides[image.y_axis] = static_cast<std::ptrdiff_t>(row_stride);
  if (image.channel_axis.has_value()) {
    layout.byte_strides[*image.channel_axis] =
        static_cast<std::ptrdiff_t>(element_bytes);
  }
  return layout;
}

/**
 * @brief Computes the exact envelope size for a validated positive layout.
 *
 * @param descriptor Logical tensor descriptor.
 * @param layout Positive physical layout with matching rank.
 * @return Highest addressable element end.
 * @throws std::overflow_error when offset arithmetic exceeds size_t.
 * @note make_interleaved_layout and descriptor validation establish all shape,
 *       rank, and stride preconditions.
 */
std::size_t dense_storage_size(const DenseTensorDescriptor& descriptor,
                               const StridedLayout& layout) {
  std::size_t size = dense_tensor_element_bytes(descriptor);
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    const std::size_t offset =
        checked_multiply(descriptor.shape[axis] - 1U,
                         static_cast<std::size_t>(layout.byte_strides[axis]),
                         "Dense image output offset exceeds size_t.");
    size = checked_add(size, offset,
                       "Dense image output envelope exceeds size_t.");
  }
  return size;
}

/**
 * @brief Infers the exact logical descriptor of unsigned-8 dense inversion.
 *
 * @param configuration Unused request-effective parameter snapshot.
 * @param inputs Exactly one logical image input.
 * @return Unchanged complete input logical descriptor.
 * @throws std::invalid_argument for wrong arity, malformed image facts, or an
 *         element type other than unsigned 8-bit.
 * @note The callback accepts no payload object and performs no IO or mutation.
 */
DenseImageDescriptor infer_dense_invert(
    const CpuDenseImageConfiguration& configuration,
    const std::vector<DenseImageDescriptor>& inputs) {
  (void)configuration;
  if (inputs.size() != 1U) {
    throw std::invalid_argument(
        "image_process:invert_dense requires exactly one image input.");
  }
  validate_logical_image_descriptor(inputs.front());
  if (inputs.front().tensor.element_semantics !=
          ElementSemantics::UnsignedInteger ||
      inputs.front().tensor.storage_encoding.bit_width != 8U) {
    throw std::invalid_argument(
        "image_process:invert_dense supports only unsigned 8-bit elements.");
  }
  return inputs.front();
}

/**
 * @brief Executes unsigned-8 inversion through explicit ImageView strides.
 *
 * @param configuration Unused request-effective parameter snapshot.
 * @param inputs Exactly one checked immutable ImageView.
 * @param inferred Exact logical output descriptor from infer_dense_invert.
 * @return Independently owned padded immutable output Value.
 * @throws std::invalid_argument for wrong arity or descriptor disagreement.
 * @throws std::overflow_error for unrepresentable output layout arithmetic.
 * @throws std::bad_alloc when output storage allocation fails.
 * @note Every active x/y/channel element is addressed through ImageView;
 *       input padding is never inspected.
 */
Value execute_dense_invert(const CpuDenseImageConfiguration& configuration,
                           const std::vector<ImageView>& inputs,
                           const DenseImageDescriptor& inferred) {
  (void)configuration;
  if (inputs.size() != 1U ||
      !(logical_descriptor(inputs.front()) == inferred)) {
    throw std::invalid_argument(
        "image_process:invert_dense input disagrees with inference.");
  }

  StridedLayout layout = make_interleaved_layout(inferred);
  std::vector<std::byte> storage(dense_storage_size(inferred.tensor, layout),
                                 std::byte{0});
  const ImageFacet& image = inferred.image;
  for (std::size_t y = 0U; y < inputs.front().height(); ++y) {
    for (std::size_t x = 0U; x < inputs.front().width(); ++x) {
      for (std::size_t channel = 0U; channel < inputs.front().channels();
           ++channel) {
        std::size_t offset =
            y * static_cast<std::size_t>(layout.byte_strides[image.y_axis]) +
            x * static_cast<std::size_t>(layout.byte_strides[image.x_axis]);
        if (image.channel_axis.has_value()) {
          offset += channel * static_cast<std::size_t>(
                                  layout.byte_strides[*image.channel_axis]);
        }
        const std::uint8_t input_value = std::to_integer<std::uint8_t>(
            *inputs.front().channel_data(x, y, channel));
        storage[offset] =
            std::byte{static_cast<std::uint8_t>(255U - input_value)};
      }
    }
  }

  return Value::from_cpu_dense_tensor(inferred.tensor, inferred.image,
                                      std::move(layout), std::move(storage));
}

}  // namespace

/** @copydoc ps::ops::execute_cpu_dense_image_operation */
NodeOutput execute_cpu_dense_image_operation(
    const Node& node, const std::vector<const NodeOutput*>& inputs,
    const CpuDenseImageOperation& operation) {
  if (!operation.infer || !operation.execute) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image operation definition is incomplete.");
  }

  CpuDenseImageConfiguration configuration;
  try {
    configuration.parameters = node.runtime_parameters;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "CPU dense image configuration snapshot failed: " +
                         std::string(error.what()));
  } catch (...) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "CPU dense image configuration snapshot failed.");
  }

  std::vector<ImageView> input_views;
  std::vector<DenseImageDescriptor> input_descriptors;
  try {
    input_views.reserve(inputs.size());
    input_descriptors.reserve(inputs.size());
    for (const NodeOutput* input : inputs) {
      if (input == nullptr) {
        throw std::invalid_argument(
            "CPU dense image operation input is missing.");
      }
      input_views.emplace_back(snapshot_image_buffer(input->image_buffer));
      input_descriptors.push_back(logical_descriptor(input_views.back()));
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "CPU dense image input validation failed: " +
                         std::string(error.what()));
  } catch (...) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "CPU dense image input validation failed.");
  }

  DenseImageDescriptor inferred;
  try {
    inferred = operation.infer(configuration, input_descriptors);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "CPU dense image inference failed: " + std::string(error.what()));
  } catch (...) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "CPU dense image inference failed.");
  }
  try {
    validate_logical_image_descriptor(inferred);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image inferred descriptor is invalid: " +
                         std::string(error.what()));
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image inferred descriptor is invalid.");
  }

  Value result;
  try {
    result = operation.execute(configuration, input_views, inferred);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(
        GraphErrc::ComputeError,
        "CPU dense image execution failed: " + std::string(error.what()));
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image execution failed.");
  }

  try {
    return publish_image_value(std::move(result), inferred);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image output validation failed: " +
                         std::string(error.what()));
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image output validation failed.");
  }
}

/** @copydoc ps::ops::make_dense_invert_operation */
CpuDenseImageOperation make_dense_invert_operation() {
  return CpuDenseImageOperation{CpuDenseImageInferFunc(infer_dense_invert),
                                CpuDenseImageExecuteFunc(execute_dense_invert)};
}

}  // namespace ps::ops
