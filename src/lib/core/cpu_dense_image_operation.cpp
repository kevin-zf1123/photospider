#include "core/cpu_dense_image_operation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/graph_error.hpp"

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
 * @brief Extracts one logical image descriptor without exposing payload bytes.
 *
 * @param view Valid retaining checked image view.
 * @return Complete logical DenseTensor and ImageFacet facts.
 * @throws std::bad_alloc when copying the complete DenseTensor descriptor or
 *         allocation-owning ImageFacet metadata cannot allocate.
 * @note Physical strides and Value ownership are intentionally omitted.
 */
DenseImageDescriptor logical_descriptor(const ImageView& view) {
  return DenseImageDescriptor{view.descriptor(), view.image_facet()};
}

/**
 * @brief Validates a descriptor-only image contract for pure inference.
 *
 * @param descriptor Logical image descriptor to inspect.
 * @throws std::invalid_argument for unsupported element facts, malformed
 *         shape/image metadata, non-singleton unassigned axes, or
 *         current-adapter extent overflow.
 * @throws std::overflow_error or std::length_error for unrepresentable or
 *         over-limit image metadata.
 * @throws std::bad_alloc when bounded validation state cannot allocate.
 * @note The function reads no payload, layout, graph, registry, or device
 *       state.
 */
void validate_logical_image_descriptor(const DenseImageDescriptor& descriptor) {
  validate_dense_tensor_image_metadata(descriptor.tensor, descriptor.image);
  (void)dense_tensor_element_bytes(descriptor.tensor);
  const std::vector<std::size_t>& shape = descriptor.tensor.shape;
  const ImageFacet& image = descriptor.image;
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
 * @brief Publishes one validated immutable result as named Value authority.
 *
 * @param value Execute result to validate and publish.
 * @param inferred Exact logical descriptor returned by inference.
 * @return NodeOutput retaining the exact Value under the permanent image name.
 * @throws std::invalid_argument or std::overflow_error for descriptor, facet,
 *         layout, or current-adapter mismatches.
 * @throws std::bad_alloc when retaining complete descriptor/ImageFacet facts
 * or named-output storage fails.
 * @note The exact sealed Value is the sole output identity and payload
 * authority; compatibility consumers project it at their own use boundary.
 */
NodeOutput publish_image_value(Value value,
                               const DenseImageDescriptor& inferred) {
  ImageView view(value);
  if (!(logical_descriptor(view) == inferred)) {
    throw std::invalid_argument(
        "Dense image execute result disagrees with inference.");
  }

  NodeOutput output;
  output.publish_image_value(std::move(value));
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
 * @brief Validates one exact Region against an inferred dense image.
 *
 * @param region Canonical logical work selection.
 * @param inferred Exact output tensor and image descriptor.
 * @throws std::invalid_argument for unsupported atom count/domain/kind,
 *         out-of-data-window image coordinates, tensor
 *         rank mismatch, or out-of-bounds tensor axes.
 * @note Whole and Empty are always valid. ImageRect constrains only explicit
 *       x/y axes; TensorSlice constrains every logical tensor axis directly.
 */
void validate_dense_region(const RegionSet& region,
                           const DenseImageDescriptor& inferred) {
  if (region.is_whole() || region.is_empty()) {
    return;
  }
  if (region.atoms().size() != 1U) {
    throw std::invalid_argument(
        "Dense operation accepts one exact Region atom.");
  }
  const RegionAtom& atom = region.atoms().front();
  if (const auto* image = std::get_if<ImageRect>(&atom)) {
    if (!(image->domain == image_region_domain())) {
      throw std::invalid_argument(
          "Dense ImageRect uses an unsupported logical domain.");
    }
    const ImageBounds& bounds = inferred.image.data_window;
    if (image->x_begin < bounds.x_begin || image->x_end > bounds.x_end ||
        image->y_begin < bounds.y_begin || image->y_end > bounds.y_end) {
      throw std::invalid_argument(
          "Dense ImageRect exceeds the inferred image data window.");
    }
    return;
  }
  const TensorSlice& tensor = std::get<TensorSlice>(atom);
  if (!(tensor.domain == dense_tensor_region_domain())) {
    throw std::invalid_argument(
        "Dense TensorSlice uses an unsupported logical domain.");
  }
  if (tensor.axes.size() != inferred.tensor.shape.size()) {
    throw std::invalid_argument(
        "Dense TensorSlice rank differs from the inferred tensor.");
  }
  for (std::size_t axis = 0U; axis < tensor.axes.size(); ++axis) {
    if (tensor.axes[axis].end > inferred.tensor.shape[axis]) {
      throw std::invalid_argument(
          "Dense TensorSlice exceeds the inferred tensor bounds.");
    }
  }
}

/**
 * @brief Tests whether one logical coordinate is selected by a valid Region.
 *
 * @param region Region validated by validate_dense_region().
 * @param inferred Exact tensor and complete ordinary-image interpretation.
 * @param coordinates Complete logical tensor coordinates in axis order.
 * @return True when the coordinate belongs to Whole or the exact atom.
 * @throws Nothing under validated rank and bounds.
 */
bool dense_coordinate_selected(
    const RegionSet& region, const DenseImageDescriptor& inferred,
    const std::vector<std::size_t>& coordinates) noexcept {
  if (region.is_whole()) {
    return true;
  }
  if (region.is_empty()) {
    return false;
  }
  const RegionAtom& atom = region.atoms().front();
  if (const auto* image = std::get_if<ImageRect>(&atom)) {
    const ImageBounds& bounds = inferred.image.data_window;
    const std::int64_t x =
        bounds.x_begin +
        static_cast<std::int64_t>(coordinates[inferred.image.x_axis]);
    const std::int64_t y =
        bounds.y_begin +
        static_cast<std::int64_t>(coordinates[inferred.image.y_axis]);
    return x >= image->x_begin && x < image->x_end && y >= image->y_begin &&
           y < image->y_end;
  }
  const TensorSlice& tensor = std::get<TensorSlice>(atom);
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    if (coordinates[axis] < tensor.axes[axis].begin ||
        coordinates[axis] >= tensor.axes[axis].end) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Infers the exact logical descriptor of unsigned-8 dense inversion.
 *
 * @param configuration Request-effective parameters and logical Region.
 * @param inputs Exactly one logical image input.
 * @return Unchanged complete input logical descriptor.
 * @throws std::invalid_argument for wrong arity, malformed image facts, or an
 *         element type other than unsigned 8-bit.
 * @throws std::overflow_error or std::length_error for unrepresentable or
 *         over-limit image metadata.
 * @throws std::bad_alloc when bounded validation state or the returned complete
 *         DenseTensor/ImageFacet copy cannot allocate.
 * @note The callback accepts no payload object and performs no IO or mutation.
 *       Returning by value preserves every rich ImageFacet field.
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
 * @param output_plan Frozen output descriptor/layout/allocation envelope.
 * @param output_grant Active whole-output write capability.
 * @return Nothing; successful return leaves retirement and seal to the Host.
 * @throws std::invalid_argument for wrong arity or descriptor disagreement.
 * @throws std::overflow_error for unrepresentable output layout arithmetic.
 * @throws std::bad_alloc when complete descriptor/ImageFacet copies, layout or
 *         coordinate storage, or grant validation cannot allocate.
 * @note Every active x/y/channel element is addressed through ImageView;
 *       selected elements are inverted, unselected elements are copied, and
 *       input padding is never inspected.
 */
void execute_dense_invert(const CpuDenseImageConfiguration& configuration,
                          const std::vector<ImageView>& inputs,
                          const DenseImageDescriptor& inferred,
                          const DenseImageOutputPlan& output_plan,
                          HostOutputWriteGrant& output_grant) {
  if (inputs.size() != 1U ||
      !(logical_descriptor(inputs.front()) == inferred)) {
    throw std::invalid_argument(
        "image_process:invert_dense input disagrees with inference.");
  }
  validate_dense_region(configuration.region, inferred);
  if (!(output_plan.descriptor() == inferred.tensor) ||
      !(output_plan.image_facet() == inferred.image) ||
      output_grant.span_count() != 1U ||
      output_grant.span(0U).allocation_offset != 0U ||
      output_grant.span(0U).byte_size != output_plan.storage_size()) {
    throw std::invalid_argument(
        "image_process:invert_dense output grant disagrees with its plan.");
  }

  const StridedLayout& layout = output_plan.layout();
  std::byte* storage = output_grant.data(0U);
  const ImageFacet& image = inferred.image;
  std::vector<std::size_t> coordinates(inferred.tensor.shape.size(), 0U);
  for (std::size_t y = 0U; y < inputs.front().height(); ++y) {
    coordinates[image.y_axis] = y;
    for (std::size_t x = 0U; x < inputs.front().width(); ++x) {
      coordinates[image.x_axis] = x;
      for (std::size_t channel = 0U; channel < inputs.front().channels();
           ++channel) {
        if (image.channel_axis.has_value()) {
          coordinates[*image.channel_axis] = channel;
        }
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
            dense_coordinate_selected(configuration.region, inferred,
                                      coordinates)
                ? std::byte{static_cast<std::uint8_t>(255U - input_value)}
                : std::byte{input_value};
      }
    }
  }
}

}  // namespace

/** @copydoc ps::ops::execute_cpu_dense_image_operation */
NodeOutput execute_cpu_dense_image_operation(
    const Node& node, const std::vector<const NodeOutput*>& inputs,
    const CpuDenseImageOperation& operation, const RegionSet& region) {
  if (!operation.infer || !operation.execute) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image operation definition is incomplete.");
  }

  CpuDenseImageConfiguration configuration;
  try {
    configuration.parameters = node.runtime_parameters;
    configuration.region = region;
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
      if (!input->has_image_value()) {
        throw std::invalid_argument(
            "CPU dense image operation input has no canonical image Value.");
      }
      input_views.emplace_back(input->image_value());
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

  StridedLayout output_layout;
  std::size_t output_storage_size = 0U;
  try {
    output_layout = make_interleaved_layout(inferred);
    output_storage_size = dense_storage_size(inferred.tensor, output_layout);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(
        GraphErrc::ComputeError,
        "CPU dense image output planning failed: " + std::string(error.what()));
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image output planning failed.");
  }

  HostOutputBinding binding =
      HostOutputBinding::allocate(DenseImageOutputPlan::create(
          std::string(NodeOutput::kImageOutputName), inferred.tensor,
          inferred.image, std::move(output_layout), output_storage_size, 64U));
  HostOutputWriteGrant grant = binding.grant_whole();
  try {
    operation.execute(configuration, input_views, inferred, binding.plan(),
                      grant);
  } catch (const std::bad_alloc&) {
    grant.retire_failure("CPU dense image execution exhausted resources.");
    throw;
  } catch (const std::exception& error) {
    grant.retire_failure("CPU dense image execution failed.");
    throw GraphError(
        GraphErrc::ComputeError,
        "CPU dense image execution failed: " + std::string(error.what()));
  } catch (...) {
    grant.retire_failure("CPU dense image execution failed.");
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image execution failed.");
  }

  try {
    grant.retire_success();
    return publish_image_value(binding.seal(), inferred);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image output publication failed: " +
                         std::string(error.what()));
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "CPU dense image output publication failed.");
  }
}

/** @copydoc ps::ops::make_dense_invert_operation */
CpuDenseImageOperation make_dense_invert_operation() {
  return CpuDenseImageOperation{CpuDenseImageInferFunc(infer_dense_invert),
                                CpuDenseImageExecuteFunc(execute_dense_invert)};
}

}  // namespace ps::ops
