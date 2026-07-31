#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/data/packed_dense_tensor_view.hpp"

namespace ps {
namespace {

/**
 * @brief Multiplies packed-address components with overflow checking.
 * @param left First non-negative factor.
 * @param right Second non-negative factor.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds std::size_t.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("Packed DenseTensor arithmetic overflowed.");
  }
  return left * right;
}

/**
 * @brief Adds packed-address components with overflow checking.
 * @param left First non-negative term.
 * @param right Second non-negative term.
 * @return Exact sum.
 * @throws std::overflow_error when the sum exceeds std::size_t.
 */
std::size_t checked_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("Packed DenseTensor arithmetic overflowed.");
  }
  return left + right;
}

/**
 * @brief Validates coordinate rank/bounds and returns logical block grid shape.
 * @param descriptor Valid packed descriptor.
 * @param coordinates Candidate complete logical coordinate.
 * @return Positive block-grid extent for every axis.
 * @throws std::invalid_argument when coordinate rank is wrong.
 * @throws std::out_of_range when any coordinate exceeds its extent.
 * @throws std::logic_error if retained quantization facts are absent.
 * @throws std::bad_alloc when grid storage cannot allocate.
 * @note Value publication already proved exact divisibility.
 */
std::vector<std::size_t> validate_coordinates_and_grid(
    const DenseTensorDescriptor& descriptor,
    const std::vector<std::size_t>& coordinates) {
  if (!descriptor.quantization.has_value()) {
    throw std::logic_error("Packed Value lost quantization metadata.");
  }
  if (coordinates.size() != descriptor.shape.size()) {
    throw std::invalid_argument(
        "Packed DenseTensor coordinate rank must match tensor rank.");
  }
  std::vector<std::size_t> grid;
  grid.reserve(descriptor.shape.size());
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    if (coordinates[axis] >= descriptor.shape[axis]) {
      throw std::out_of_range(
          "Packed DenseTensor coordinate is outside its extent.");
    }
    grid.push_back(descriptor.shape[axis] /
                   descriptor.quantization->block_shape[axis]);
  }
  return grid;
}

/**
 * @brief Converts block coordinates to one row-major block index.
 * @param coordinates Logical element coordinates.
 * @param block_shape Positive element extents per block.
 * @param block_grid_shape Positive number of blocks per tensor axis.
 * @return Exact row-major logical block index.
 * @throws std::overflow_error when checked linearization overflows.
 * @note All vector ranks and coordinate bounds are prevalidated.
 */
std::size_t block_linear_index(
    const std::vector<std::size_t>& coordinates,
    const std::vector<std::size_t>& block_shape,
    const std::vector<std::size_t>& block_grid_shape) {
  std::size_t index = 0U;
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    index = checked_add(checked_multiply(index, block_grid_shape[axis]),
                        coordinates[axis] / block_shape[axis]);
  }
  return index;
}

/**
 * @brief Converts within-block coordinates to row-major element index.
 * @param coordinates Logical element coordinates.
 * @param block_shape Positive element extents per block.
 * @return Exact row-major index inside one complete block.
 * @throws std::overflow_error when checked linearization overflows.
 * @note Rank and element bounds are prevalidated.
 */
std::size_t within_block_linear_index(
    const std::vector<std::size_t>& coordinates,
    const std::vector<std::size_t>& block_shape) {
  std::size_t index = 0U;
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    index = checked_add(checked_multiply(index, block_shape[axis]),
                        coordinates[axis] % block_shape[axis]);
  }
  return index;
}

/**
 * @brief Resolves one logical coordinate to its allocation-relative bit.
 * @param coordinates Complete in-bounds logical coordinate.
 * @param layout Valid version-1 Blocked layout.
 * @return Bit position of the element's first packed value bit.
 * @throws std::overflow_error when coordinate arithmetic overflows.
 * @note Publication has proved the returned nibble lies in the byte envelope.
 */
std::size_t element_bit_position(const std::vector<std::size_t>& coordinates,
                                 const BlockedLayout& layout) {
  std::size_t position = layout.bit_offset;
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    position = checked_add(
        position, checked_multiply(coordinates[axis] / layout.block_shape[axis],
                                   layout.block_bit_strides[axis]));
  }
  return checked_add(
      position,
      checked_multiply(
          within_block_linear_index(coordinates, layout.block_shape), 4U));
}

/**
 * @brief Converts a bit position to the nibble shift for one packed order.
 * @param bit_position Nibble-aligned allocation-relative bit position.
 * @param order Valid V-13 packed bit order.
 * @return Zero or four bit shift inside the selected byte.
 * @throws std::logic_error if a retained layout invariant is violated.
 */
std::size_t nibble_shift(std::size_t bit_position, PackedBitOrder order) {
  const std::size_t bit_in_byte = bit_position % 8U;
  if (bit_in_byte != 0U && bit_in_byte != 4U) {
    throw std::logic_error("Packed FP4 position is not nibble-aligned.");
  }
  switch (order) {
    case PackedBitOrder::LeastSignificantFirst:
      return bit_in_byte;
    case PackedBitOrder::MostSignificantFirst:
      return 4U - bit_in_byte;
  }
  throw std::logic_error("Packed FP4 bit order is invalid.");
}

/**
 * @brief Writes one four-bit code through an exclusive byte-envelope lease.
 * @param bytes Writable complete allocation start.
 * @param layout Valid output Blocked layout.
 * @param coordinates Complete in-bounds logical coordinate.
 * @param code Four-bit value in the inclusive range zero through fifteen.
 * @throws std::overflow_error from checked coordinate addressing.
 * @throws std::logic_error for a retained bit-alignment/order violation.
 * @note The caller initializes the complete envelope before the first write.
 */
void write_encoded_element(std::byte* bytes, const BlockedLayout& layout,
                           const std::vector<std::size_t>& coordinates,
                           std::uint8_t code) {
  const std::size_t bit_position = element_bit_position(coordinates, layout);
  const std::size_t shift = nibble_shift(bit_position, layout.bit_order);
  const std::size_t byte_index = bit_position / 8U;
  const std::uint8_t prior = std::to_integer<std::uint8_t>(bytes[byte_index]);
  const std::uint8_t mask = static_cast<std::uint8_t>(0x0FU << shift);
  const std::uint8_t replacement = static_cast<std::uint8_t>(
      (prior & static_cast<std::uint8_t>(~mask)) |
      static_cast<std::uint8_t>((code & 0x0FU) << shift));
  bytes[byte_index] = static_cast<std::byte>(replacement);
}

/**
 * @brief Advances one row-major coordinate through a fixed positive shape.
 * @param coordinates Mutable current coordinate.
 * @param shape Positive extent for every coordinate axis.
 * @return True when advanced to another coordinate; false after final wrap.
 * @throws Nothing under rank/bounds preconditions.
 */
bool advance_coordinates(std::vector<std::size_t>* coordinates,
                         const std::vector<std::size_t>& shape) noexcept {
  for (std::size_t reverse = coordinates->size(); reverse > 0U; --reverse) {
    const std::size_t axis = reverse - 1U;
    ++(*coordinates)[axis];
    if ((*coordinates)[axis] < shape[axis]) {
      return true;
    }
    (*coordinates)[axis] = 0U;
  }
  return false;
}

/**
 * @brief Decodes one signed E2M1 four-bit storage code.
 * @param code Four-bit code in the inclusive range zero through fifteen.
 * @return Exact representable E2M1 value before block scaling.
 * @throws Nothing.
 * @note Positive magnitudes are 0, 0.5, 1, 1.5, 2, 3, 4, and 6; bit three
 * supplies the sign, including a representable negative zero.
 */
float decode_fp4_e2m1(std::uint8_t code) noexcept {
  constexpr std::array<float, 8U> kMagnitude = {0.0F, 0.5F, 1.0F, 1.5F,
                                                2.0F, 3.0F, 4.0F, 6.0F};
  const float magnitude = kMagnitude[code & 0x07U];
  return (code & 0x08U) == 0U ? magnitude : -magnitude;
}

/**
 * @brief Builds canonical contiguous block bit strides and exact storage size.
 * @param shape Positive output logical tensor shape.
 * @param block_shape Positive matching quantization block shape.
 * @param bit_offset Preserved nibble-aligned output bit offset.
 * @param order Preserved packed bit order.
 * @return Canonical layout and exact complete byte envelope size.
 * @throws std::overflow_error when block or bit products overflow.
 * @throws std::bad_alloc when layout vectors cannot allocate.
 * @note The final block-grid axis is contiguous at one complete block span.
 */
std::pair<BlockedLayout, std::size_t> canonical_blocked_layout_and_size(
    const std::vector<std::size_t>& shape,
    const std::vector<std::size_t>& block_shape, std::size_t bit_offset,
    PackedBitOrder order) {
  std::size_t block_elements = 1U;
  for (const std::size_t extent : block_shape) {
    block_elements = checked_multiply(block_elements, extent);
  }
  const std::size_t block_bits = checked_multiply(block_elements, 4U);

  BlockedLayout layout;
  layout.block_shape = block_shape;
  layout.block_bit_strides.resize(shape.size());
  layout.bit_offset = bit_offset;
  layout.bit_order = order;
  std::size_t stride = block_bits;
  for (std::size_t reverse = shape.size(); reverse > 0U; --reverse) {
    const std::size_t axis = reverse - 1U;
    layout.block_bit_strides[axis] = stride;
    stride = checked_multiply(stride, shape[axis] / block_shape[axis]);
  }
  const std::size_t exclusive_bit_end = checked_add(bit_offset, stride);
  const std::size_t storage_size = checked_add(exclusive_bit_end, 7U) / 8U;
  return {std::move(layout), storage_size};
}

/**
 * @brief Converts one Region endpoint to host shape size without truncation.
 * @param value Unsigned Region endpoint.
 * @return Exact std::size_t value.
 * @throws std::overflow_error when std::size_t is narrower than uint64_t.
 */
std::size_t region_endpoint_size(std::uint64_t value) {
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::size_t>::max()) {
      throw std::overflow_error(
          "Packed TensorSlice endpoint exceeds host size domain.");
    }
  }
  return static_cast<std::size_t>(value);
}

}  // namespace

/** @copydoc PackedDenseTensorView::PackedDenseTensorView */
PackedDenseTensorView::PackedDenseTensorView(Value value)
    : value_(std::move(value)) {
  if (!value_.valid()) {
    throw std::invalid_argument(
        "PackedDenseTensorView requires a valid Value.");
  }
  if (value_.storage_layout_kind() != StorageLayoutKind::Blocked ||
      value_.image_facet().has_value()) {
    throw std::invalid_argument(
        "PackedDenseTensorView requires a non-image Blocked Value.");
  }
  const DenseTensorDescriptor& tensor = value_.dense_tensor_descriptor();
  if (tensor.element_semantics != ElementSemantics::FloatingPoint ||
      tensor.storage_encoding.kind != StorageEncodingKind::Fp4E2M1 ||
      dense_tensor_element_bits(tensor) != 4U ||
      !tensor.quantization.has_value()) {
    throw std::invalid_argument(
        "PackedDenseTensorView requires quantized FP4 E2M1 elements.");
  }
  read_lease_ = value_.buffer_handle().acquire_read();
}

/** @copydoc PackedDenseTensorView::value */
const Value& PackedDenseTensorView::value() const noexcept {
  return value_;
}

/** @copydoc PackedDenseTensorView::descriptor */
auto PackedDenseTensorView::descriptor() const noexcept
    -> const DenseTensorDescriptor& {  // NOLINT(whitespace/indent_namespace)
  return value_.dense_tensor_descriptor();
}

/** @copydoc PackedDenseTensorView::layout */
const BlockedLayout& PackedDenseTensorView::layout() const noexcept {
  return value_.blocked_layout();
}

/** @copydoc PackedDenseTensorView::encoded_element */
std::uint8_t PackedDenseTensorView::encoded_element(
    const std::vector<std::size_t>& coordinates) const {
  (void)validate_coordinates_and_grid(descriptor(), coordinates);
  const std::size_t bit_position = element_bit_position(coordinates, layout());
  const std::size_t shift = nibble_shift(bit_position, layout().bit_order);
  const std::uint8_t byte =
      std::to_integer<std::uint8_t>(read_lease_.data()[bit_position / 8U]);
  return static_cast<std::uint8_t>((byte >> shift) & 0x0FU);
}

/** @copydoc PackedDenseTensorView::dequantized_element */
float PackedDenseTensorView::dequantized_element(
    const std::vector<std::size_t>& coordinates) const {
  const std::vector<std::size_t> block_grid_shape =
      validate_coordinates_and_grid(descriptor(), coordinates);
  const QuantizationSchema& quantization = *descriptor().quantization;
  const std::size_t scale_index = block_linear_index(
      coordinates, quantization.block_shape, block_grid_shape);
  return decode_fp4_e2m1(encoded_element(coordinates)) *
         quantization.scales[scale_index];
}

/** @copydoc ps::copy_packed_dense_tensor_slice */
Value copy_packed_dense_tensor_slice(const Value& source,
                                     const TensorSlice& slice) {
  PackedDenseTensorView source_view(source);
  const DenseTensorDescriptor& source_descriptor = source_view.descriptor();
  const QuantizationSchema& source_quantization =
      *source_descriptor.quantization;
  if (!(slice.domain == dense_tensor_region_domain()) ||
      slice.axes.size() != source_descriptor.shape.size()) {
    throw std::invalid_argument(
        "Packed TensorSlice requires the dense domain and matching rank.");
  }

  std::vector<std::size_t> begins;
  std::vector<std::size_t> output_shape;
  begins.reserve(slice.axes.size());
  output_shape.reserve(slice.axes.size());
  for (std::size_t axis = 0U; axis < slice.axes.size(); ++axis) {
    const std::size_t begin = region_endpoint_size(slice.axes[axis].begin);
    const std::size_t end = region_endpoint_size(slice.axes[axis].end);
    const std::size_t block_extent = source_quantization.block_shape[axis];
    if (begin >= end || end > source_descriptor.shape[axis]) {
      throw std::invalid_argument(
          "Packed TensorSlice intervals must be nonempty and in bounds.");
    }
    if (begin % block_extent != 0U || end % block_extent != 0U) {
      throw std::invalid_argument(
          "Packed TensorSlice endpoints must align to quantization blocks.");
    }
    begins.push_back(begin);
    output_shape.push_back(end - begin);
  }

  std::vector<std::size_t> source_block_grid;
  std::vector<std::size_t> output_block_grid;
  source_block_grid.reserve(output_shape.size());
  output_block_grid.reserve(output_shape.size());
  std::size_t output_scale_count = 1U;
  for (std::size_t axis = 0U; axis < output_shape.size(); ++axis) {
    const std::size_t block_extent = source_quantization.block_shape[axis];
    source_block_grid.push_back(source_descriptor.shape[axis] / block_extent);
    const std::size_t grid_extent = output_shape[axis] / block_extent;
    output_block_grid.push_back(grid_extent);
    output_scale_count = checked_multiply(output_scale_count, grid_extent);
  }

  QuantizationSchema output_quantization;
  output_quantization.block_shape = source_quantization.block_shape;
  output_quantization.scales.reserve(output_scale_count);
  std::vector<std::size_t> output_block_coordinates(output_shape.size(), 0U);
  do {
    std::vector<std::size_t> source_block_element(output_shape.size(), 0U);
    for (std::size_t axis = 0U; axis < output_shape.size(); ++axis) {
      source_block_element[axis] =
          begins[axis] + output_block_coordinates[axis] *
                             source_quantization.block_shape[axis];
    }
    const std::size_t source_scale_index =
        block_linear_index(source_block_element,
                           source_quantization.block_shape, source_block_grid);
    output_quantization.scales.push_back(
        source_quantization.scales[source_scale_index]);
  } while (advance_coordinates(&output_block_coordinates, output_block_grid));

  DenseTensorDescriptor output_descriptor{
      output_shape, source_descriptor.element_semantics,
      source_descriptor.storage_encoding, std::move(output_quantization)};
  auto [output_layout, output_storage_size] = canonical_blocked_layout_and_size(
      output_shape, source_quantization.block_shape,
      source_view.layout().bit_offset, source_view.layout().bit_order);
  ValueBuilder builder = ValueBuilder::allocate_cpu_blocked_dense_tensor(
      output_descriptor, output_layout, output_storage_size);
  {
    WriteLease output_write = builder.acquire_write();
    std::fill(output_write.data(), output_write.data() + output_write.size(),
              std::byte{0});
    std::vector<std::size_t> output_coordinates(output_shape.size(), 0U);
    do {
      std::vector<std::size_t> source_coordinates(output_shape.size(), 0U);
      for (std::size_t axis = 0U; axis < output_shape.size(); ++axis) {
        source_coordinates[axis] = begins[axis] + output_coordinates[axis];
      }
      write_encoded_element(output_write.data(), output_layout,
                            output_coordinates,
                            source_view.encoded_element(source_coordinates));
    } while (advance_coordinates(&output_coordinates, output_shape));
  }
  return builder.seal();
}

}  // namespace ps
