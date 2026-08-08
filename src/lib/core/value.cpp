#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "core/dense_tensor_content_digest.hpp"
#include "core/pending_value.hpp"
#include "core/value_validation.hpp"
#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

/** @brief One and only one physical layout retained by a Value publication. */
// NOLINTBEGIN(whitespace/indent_namespace)
using ValueLayout =
    std::variant<StridedLayout, BlockedLayout, ProviderDefinedLayout>;
// NOLINTEND

/**
 * @brief Owns process-wide allocation, revision, and producer sequences.
 *
 * This authority is instantiated exactly once inside the shared
 * `Photospider::operation_runtime`. The static Host product and every
 * Value-using operation DSO dynamically depend on that same runtime image, so
 * allocation, revision, and producer tokens cannot restart at a DSO boundary.
 *
 * @throws Nothing during construction and destruction.
 * @note The three sequences remain distinct identity domains. The
 * shared-runtime ownership, rather than symbol interposition or a probabilistic
 * token, establishes their process-wide uniqueness.
 */
class ProcessIdentityAuthority final {
 public:
  /**
   * @brief Mints one allocation identity for this process runtime.
   *
   * @return Unique nonzero allocation token.
   * @throws std::overflow_error when allocation identity space is exhausted.
   * @note Calls are thread-safe and never reuse a previously returned token.
   */
  std::uint64_t mint_allocation_identity() {
    return mint_from(&next_allocation_identity_,
                     "Allocation identity space is exhausted.");
  }

  /**
   * @brief Mints one Value revision identity for this process runtime.
   *
   * @return Unique nonzero Value revision token.
   * @throws std::overflow_error when Value revision space is exhausted.
   * @note Calls are thread-safe and never reuse a previously returned token.
   */
  std::uint64_t mint_value_revision() {
    return mint_from(&next_value_revision_,
                     "Value revision identity space is exhausted.");
  }

  /**
   * @brief Mints one producer identity for this process runtime.
   * @return Unique nonzero producer token.
   * @throws std::overflow_error when producer identity space is exhausted.
   * @note Calls are thread-safe and never reuse a previously returned token.
   */
  std::uint64_t mint_producer_identity() {
    return mint_from(&next_producer_identity_,
                     "Producer identity space is exhausted.");
  }

 private:
  /**
   * @brief Mints one nonzero token without wraparound reuse.
   *
   * @param source Atomic next-token source owned by this authority.
   * @param diagnostic Stable exhaustion diagnostic.
   * @return Unique nonzero token from the selected identity domain.
   * @throws std::overflow_error when the source reaches its terminal value.
   * @note The terminal maximum value is reserved so increment never wraps to
   *       zero and later calls can never reuse an earlier token.
   */
  static std::uint64_t mint_from(std::atomic<std::uint64_t>* source,
                                 const char* diagnostic) {
    std::uint64_t current = source->load(std::memory_order_relaxed);
    while (true) {
      if (current == 0U ||
          current == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(diagnostic);
      }
      if (source->compare_exchange_weak(current, current + 1U,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
        return current;
      }
    }
  }

  /** @brief Next allocation token; zero is permanently invalid. */
  std::atomic<std::uint64_t> next_allocation_identity_{1U};

  /** @brief Next Value revision token; zero is permanently invalid. */
  std::atomic<std::uint64_t> next_value_revision_{1U};

  /** @brief Next producer token; zero is permanently invalid. */
  std::atomic<std::uint64_t> next_producer_identity_{1U};
};

/**
 * @brief Returns the shared runtime's sole identity-minting authority.
 *
 * @return Process-lifetime authority owned by `operation_runtime`.
 * @throws Nothing.
 * @note Function-local initialization is thread-safe. The enclosing shared
 *       runtime supplies one storage instance to the Host and every linked
 *       operation DSO.
 */
ProcessIdentityAuthority& process_identity_authority() noexcept {
  static ProcessIdentityAuthority authority;
  return authority;
}

/**
 * @brief Multiplies address-envelope components with overflow checking.
 *
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds std::size_t.
 * @note The helper performs no allocation and accepts zero operands.
 */
std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("DenseTensor byte envelope overflows size_t.");
  }
  return left * right;
}

/**
 * @brief Adds address-envelope components with overflow checking.
 *
 * @param left First non-negative component.
 * @param right Second non-negative component.
 * @return Exact sum.
 * @throws std::overflow_error when the sum exceeds std::size_t.
 * @note The helper performs no allocation.
 */
std::size_t checked_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("DenseTensor byte envelope overflows size_t.");
  }
  return left + right;
}

/**
 * @brief Returns the unsigned magnitude of a signed byte stride.
 *
 * @param stride Signed stride, including the minimum ptrdiff_t value.
 * @return Exact non-negative magnitude representable by std::size_t.
 * @throws std::overflow_error when size_t cannot represent the magnitude.
 * @note Conversion avoids negating the minimum signed value.
 */
std::size_t stride_magnitude(std::ptrdiff_t stride) {
  if (stride >= 0) {
    return static_cast<std::size_t>(stride);
  }
  const auto magnitude =
      static_cast<std::uintmax_t>(-(stride + 1)) + std::uintmax_t{1U};
  if (magnitude > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error("DenseTensor stride magnitude exceeds size_t.");
  }
  return static_cast<std::size_t>(magnitude);
}

/**
 * @brief Validates the common logical DenseTensor descriptor and image facet.
 *
 * @param descriptor Logical descriptor to validate.
 * @param facet Optional explicit image-axis mapping.
 * @throws std::invalid_argument for empty/zero shape or malformed image axes.
 * @note Element encoding, quantization, layout, and allocation ranges are
 * validated separately.
 */
void validate_shape_and_facet(const DenseTensorDescriptor& descriptor,
                              const std::optional<ImageFacet>& facet) {
  if (descriptor.shape.empty()) {
    throw std::invalid_argument("DenseTensor rank must be positive.");
  }
  for (const std::size_t extent : descriptor.shape) {
    if (extent == 0U) {
      throw std::invalid_argument("DenseTensor extents must all be positive.");
    }
  }

  if (!facet.has_value()) {
    return;
  }
  if (facet->x_axis >= descriptor.shape.size() ||
      facet->y_axis >= descriptor.shape.size()) {
    throw std::invalid_argument("ImageFacet axis is outside tensor rank.");
  }
  if (facet->x_axis == facet->y_axis) {
    throw std::invalid_argument("ImageFacet x and y axes must be distinct.");
  }
  if (facet->channel_axis.has_value() &&
      (*facet->channel_axis >= descriptor.shape.size() ||
       *facet->channel_axis == facet->x_axis ||
       *facet->channel_axis == facet->y_axis)) {
    throw std::invalid_argument(
        "ImageFacet channel axis must be distinct and in rank.");
  }
}

/**
 * @brief Validates one currently supported whole-byte Strided descriptor.
 * @param descriptor Logical descriptor to validate.
 * @param facet Optional explicit image-axis mapping.
 * @return Positive whole-byte element width.
 * @throws std::invalid_argument for malformed shape/facet, quantization on a
 * Strided V-13 value, or unsupported whole-byte encoding.
 * @note Packed FP4 uses the separate Blocked descriptor authority.
 */
std::size_t validate_descriptor_and_facet(
    const DenseTensorDescriptor& descriptor,
    const std::optional<ImageFacet>& facet) {
  validate_shape_and_facet(descriptor, facet);
  if (descriptor.quantization.has_value()) {
    throw std::invalid_argument(
        "Strided DenseTensor does not support quantization in V-13.");
  }
  return dense_tensor_element_bytes(descriptor);
}

/**
 * @brief Complete reachable byte bounds for one signed tensor layout.
 *
 * @note `lower_extent` is the maximum distance below logical origin.
 * `upper_end` is the exclusive allocation-relative end of the highest element.
 */
struct AddressEnvelope {
  /** @brief Maximum byte magnitude addressed below logical coordinate zero. */
  std::size_t lower_extent = 0U;

  /** @brief Exclusive allocation-relative end of the highest element. */
  std::size_t upper_end = 0U;
};

/**
 * @brief Computes the checked reachable envelope of one signed layout.
 *
 * @param descriptor Valid positive-shape descriptor.
 * @param layout Layout with one signed byte stride per logical axis.
 * @param element_bytes Valid positive physical element width.
 * @return Checked lower extent and exclusive upper end.
 * @throws std::invalid_argument when shape and stride ranks differ.
 * @throws std::out_of_range when the layout offset is below its lower extent.
 * @throws std::overflow_error when any extent or address is unrepresentable.
 * @note The function derives no pointer and permits positive, zero, or negative
 * strides.
 */
AddressEnvelope compute_address_envelope(
    const DenseTensorDescriptor& descriptor, const StridedLayout& layout,
    std::size_t element_bytes) {
  if (layout.byte_strides.size() != descriptor.shape.size()) {
    throw std::invalid_argument(
        "DenseTensor shape and stride ranks must match.");
  }

  std::size_t lower_extent = 0U;
  std::size_t upper_extent = 0U;
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    const std::size_t axis_extent =
        checked_multiply(descriptor.shape[axis] - 1U,
                         stride_magnitude(layout.byte_strides[axis]));
    if (layout.byte_strides[axis] < 0) {
      lower_extent = checked_add(lower_extent, axis_extent);
    } else {
      upper_extent = checked_add(upper_extent, axis_extent);
    }
  }
  if (layout.byte_offset < lower_extent) {
    throw std::out_of_range(
        "DenseTensor signed layout underflows its BufferHandle.");
  }
  const std::size_t upper_address =
      checked_add(layout.byte_offset, upper_extent);
  return AddressEnvelope{lower_extent,
                         checked_add(upper_address, element_bytes)};
}

/**
 * @brief Proves that one positive producer layout has disjoint element bytes.
 *
 * Non-singleton axes are ordered by increasing byte stride. The running span
 * starts at one element width and encloses every byte reachable through the
 * axes already processed. Requiring each next stride to be at least that span
 * makes adjacent coordinate slabs disjoint; extending the span then preserves
 * the invariant inductively for the remaining axes.
 *
 * @param descriptor Valid positive-shape descriptor.
 * @param layout Rank-matched layout whose strides are already positive.
 * @param element_bytes Valid positive physical element width.
 * @throws std::invalid_argument when the stride-span proof cannot establish
 * non-overlapping writes.
 * @throws std::overflow_error when a covered span is unrepresentable.
 * @throws std::bad_alloc when the rank-bounded axis inventory cannot allocate.
 * @note Singleton axes add no alternative address and are omitted. The proof
 * is O(rank log rank), enumerates no logical elements, and accepts contiguous,
 * padded, transposed, and otherwise permuted monotonic layouts.
 */
void validate_non_overlapping_producer_layout(
    const DenseTensorDescriptor& descriptor, const StridedLayout& layout,
    std::size_t element_bytes) {
  std::vector<std::pair<std::size_t, std::size_t>> active_axes;
  active_axes.reserve(descriptor.shape.size());
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    if (descriptor.shape[axis] > 1U) {
      active_axes.emplace_back(stride_magnitude(layout.byte_strides[axis]),
                               descriptor.shape[axis]);
    }
  }
  std::sort(active_axes.begin(), active_axes.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });

  std::size_t covered_span = element_bytes;
  for (const auto& [byte_stride, extent] : active_axes) {
    if (byte_stride < covered_span) {
      throw std::invalid_argument(
          "DenseTensor producer layout cannot prove non-overlapping writes.");
    }
    covered_span =
        checked_add(covered_span, checked_multiply(extent - 1U, byte_stride));
  }
}

/**
 * @brief Validates the exact positive-stride producer allocation envelope.
 *
 * @param descriptor Valid positive-shape descriptor.
 * @param layout Producer layout to validate.
 * @param element_bytes Valid positive physical element width.
 * @param storage_size Proposed exact allocation byte length.
 * @throws std::invalid_argument for nonzero offset, non-positive stride,
 * storage-size mismatch, or a layout whose writes cannot be proven disjoint.
 * @throws std::overflow_error when envelope or non-overlap arithmetic is
 * unrepresentable.
 * @throws std::bad_alloc when rank-bounded non-overlap proof state cannot
 * allocate.
 * @note Producer behavior remains the positive-stride, exact-envelope,
 * non-overlapping writable subset. Signed aliases are published only over
 * already sealed handles.
 */
void validate_producer_envelope(const DenseTensorDescriptor& descriptor,
                                const StridedLayout& layout,
                                std::size_t element_bytes,
                                std::size_t storage_size) {
  if (layout.byte_offset != 0U) {
    throw std::invalid_argument(
        "DenseTensor producer layout byte offset must be zero.");
  }
  for (const std::ptrdiff_t stride : layout.byte_strides) {
    if (stride <= 0) {
      throw std::invalid_argument(
          "DenseTensor producer strides must be positive.");
    }
  }
  const AddressEnvelope envelope =
      compute_address_envelope(descriptor, layout, element_bytes);
  if (envelope.lower_extent != 0U || storage_size != envelope.upper_end) {
    throw std::invalid_argument(
        "DenseTensor storage size must equal its exact byte envelope.");
  }
  validate_non_overlapping_producer_layout(descriptor, layout, element_bytes);
}

/**
 * @brief Derived exact block-grid facts for one validated FP4 descriptor.
 * @note Values are logical metadata only and carry no allocation identity.
 */
struct BlockedDescriptorFacts {
  /** @brief Number of quantization blocks along each tensor axis. */
  std::vector<std::size_t> block_grid_shape;
  /** @brief Number of logical FP4 elements inside one complete block. */
  std::size_t block_element_count = 0U;
  /** @brief Number of active packed bits inside one complete block. */
  std::size_t block_bits = 0U;
};

/**
 * @brief Validates the bounded V-13 FP4 block-scale descriptor.
 *
 * @param descriptor Candidate logical descriptor and quantization schema.
 * @return Derived grid, element-count, and block-bit facts.
 * @throws std::invalid_argument for unsupported encoding, absent/malformed
 * quantization, non-divisible extents, or invalid scales.
 * @throws std::overflow_error when block/grid products cannot be represented.
 * @throws std::bad_alloc when derived rank storage cannot allocate.
 * @note ImageFacet validation is unnecessary because blocked publication
 * deliberately supplies no image facet.
 */
BlockedDescriptorFacts validate_blocked_descriptor(
    const DenseTensorDescriptor& descriptor) {
  validate_shape_and_facet(descriptor, std::nullopt);
  if (descriptor.element_semantics != ElementSemantics::FloatingPoint ||
      descriptor.storage_encoding.kind != StorageEncodingKind::Fp4E2M1 ||
      dense_tensor_element_bits(descriptor) != 4U) {
    throw std::invalid_argument(
        "Blocked DenseTensor requires floating-point FP4 E2M1 encoding.");
  }
  if (!descriptor.quantization.has_value()) {
    throw std::invalid_argument(
        "Blocked FP4 DenseTensor requires block-scale quantization.");
  }
  const QuantizationSchema& quantization = *descriptor.quantization;
  if (quantization.block_shape.size() != descriptor.shape.size()) {
    throw std::invalid_argument(
        "Quantization block shape rank must match DenseTensor rank.");
  }

  BlockedDescriptorFacts facts;
  facts.block_grid_shape.reserve(descriptor.shape.size());
  facts.block_element_count = 1U;
  std::size_t scale_count = 1U;
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    const std::size_t block_extent = quantization.block_shape[axis];
    if (block_extent == 0U || descriptor.shape[axis] % block_extent != 0U) {
      throw std::invalid_argument(
          "DenseTensor extents must be divisible by positive quantization "
          "block extents.");
    }
    const std::size_t grid_extent = descriptor.shape[axis] / block_extent;
    facts.block_grid_shape.push_back(grid_extent);
    facts.block_element_count =
        checked_multiply(facts.block_element_count, block_extent);
    scale_count = checked_multiply(scale_count, grid_extent);
  }
  if (quantization.scales.size() != scale_count) {
    throw std::invalid_argument(
        "Quantization scale count must match the logical block grid.");
  }
  for (const float scale : quantization.scales) {
    if (!std::isfinite(scale) || scale <= 0.0F) {
      throw std::invalid_argument(
          "Quantization scales must all be finite and positive.");
    }
  }
  facts.block_bits = checked_multiply(facts.block_element_count, 4U);
  return facts;
}

/**
 * @brief Validates one Blocked layout and computes its exact byte envelope.
 *
 * @param descriptor Valid candidate FP4 descriptor.
 * @param layout Candidate versioned Blocked layout.
 * @return Required byte count through the final active block bit.
 * @throws std::invalid_argument for invalid version, rank, shape, bit order,
 * alignment, strides, or overlapping block ranges.
 * @throws std::overflow_error when bit-span or byte-envelope arithmetic
 * overflows.
 * @throws std::bad_alloc when rank-bounded proof storage cannot allocate.
 * @note The returned envelope includes unused leading/trailing nibble bits.
 */
std::size_t blocked_required_storage_size(
    const DenseTensorDescriptor& descriptor, const BlockedLayout& layout) {
  const BlockedDescriptorFacts facts = validate_blocked_descriptor(descriptor);
  if (layout.version != 1U) {
    throw std::invalid_argument(
        "Blocked DenseTensor layout version must be 1.");
  }
  if (layout.block_shape != descriptor.quantization->block_shape ||
      layout.block_bit_strides.size() != descriptor.shape.size()) {
    throw std::invalid_argument(
        "Blocked layout rank and block shape must match quantization.");
  }
  switch (layout.bit_order) {
    case PackedBitOrder::LeastSignificantFirst:
    case PackedBitOrder::MostSignificantFirst:
      break;
    default:
      throw std::invalid_argument("Blocked layout bit order is invalid.");
  }
  if (layout.bit_offset % 4U != 0U) {
    throw std::invalid_argument(
        "Blocked FP4 layout bit offset must be nibble-aligned.");
  }

  std::vector<std::pair<std::size_t, std::size_t>> active_axes;
  active_axes.reserve(descriptor.shape.size());
  std::size_t maximum_block_start = layout.bit_offset;
  for (std::size_t axis = 0U; axis < descriptor.shape.size(); ++axis) {
    const std::size_t bit_stride = layout.block_bit_strides[axis];
    if (bit_stride == 0U || bit_stride % 4U != 0U) {
      throw std::invalid_argument(
          "Blocked FP4 bit strides must be positive and nibble-aligned.");
    }
    const std::size_t grid_extent = facts.block_grid_shape[axis];
    maximum_block_start = checked_add(
        maximum_block_start, checked_multiply(grid_extent - 1U, bit_stride));
    if (grid_extent > 1U) {
      active_axes.emplace_back(bit_stride, grid_extent);
    }
  }
  std::sort(active_axes.begin(), active_axes.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });
  std::size_t covered_span = facts.block_bits;
  for (const auto& [bit_stride, grid_extent] : active_axes) {
    if (bit_stride < covered_span) {
      throw std::invalid_argument(
          "Blocked producer layout has overlapping block bit ranges.");
    }
    covered_span = checked_add(covered_span,
                               checked_multiply(grid_extent - 1U, bit_stride));
  }

  const std::size_t exclusive_bit_end =
      checked_add(maximum_block_start, facts.block_bits);
  return checked_add(exclusive_bit_end, 7U) / 8U;
}

/**
 * @brief Validates an exact writable Blocked allocation envelope.
 * @param descriptor Candidate packed descriptor.
 * @param layout Candidate packed layout.
 * @param storage_size Proposed exact byte allocation length.
 * @throws std::invalid_argument when layout validation fails or size differs.
 * @throws std::overflow_error or std::bad_alloc from checked layout proof.
 * @note Validation performs no allocation publication or identity minting.
 */
void validate_blocked_producer_envelope(const DenseTensorDescriptor& descriptor,
                                        const BlockedLayout& layout,
                                        std::size_t storage_size) {
  if (storage_size == 0U ||
      storage_size != blocked_required_storage_size(descriptor, layout)) {
    throw std::invalid_argument(
        "Blocked DenseTensor storage must equal its exact byte envelope.");
  }
}

/**
 * @brief Reports whether one axis is explicitly assigned by an image facet.
 *
 * @param facet Valid explicit image-axis mapping.
 * @param axis Logical axis to inspect.
 * @return True when axis is x, y, or the optional channel axis.
 * @throws Nothing.
 */
bool is_image_axis(const ImageFacet& facet, std::size_t axis) noexcept {
  return axis == facet.x_axis || axis == facet.y_axis ||
         (facet.channel_axis.has_value() && axis == *facet.channel_axis);
}

/**
 * @brief Accumulates one coordinate's positive or negative byte displacement.
 *
 * @param coordinate Valid in-extent coordinate.
 * @param stride Signed byte stride for the coordinate axis.
 * @param positive Mutable accumulated positive displacement.
 * @param negative Mutable accumulated negative magnitude.
 * @throws std::overflow_error when coordinate displacement is unrepresentable.
 * @note Value publication has already proved the final address is in range.
 */
void accumulate_coordinate_displacement(std::size_t coordinate,
                                        std::ptrdiff_t stride,
                                        std::size_t* positive,
                                        std::size_t* negative) {
  const std::size_t displacement =
      checked_multiply(coordinate, stride_magnitude(stride));
  if (stride < 0) {
    *negative = checked_add(*negative, displacement);
  } else {
    *positive = checked_add(*positive, displacement);
  }
}

/**
 * @brief Resolves a checked signed displacement to an allocation-relative byte.
 *
 * @param layout Validated layout supplying logical-origin offset.
 * @param positive Accumulated positive coordinate displacement.
 * @param negative Accumulated negative coordinate magnitude.
 * @return Allocation-relative element byte offset.
 * @throws std::overflow_error when positive addition is unrepresentable.
 * @throws std::out_of_range only if an internal validated-layout invariant is
 * violated.
 * @note The returned byte is already proven to lie inside the BufferHandle.
 */
std::size_t resolve_coordinate_offset(const StridedLayout& layout,
                                      std::size_t positive,
                                      std::size_t negative) {
  const std::size_t above_origin = checked_add(layout.byte_offset, positive);
  if (negative > above_origin) {
    throw std::out_of_range(
        "DenseTensor coordinate escaped its validated BufferHandle.");
  }
  return above_origin - negative;
}

}  // namespace

/** @copydoc ps::validate_dense_tensor_producer_envelope */
void validate_dense_tensor_producer_envelope(
    const DenseTensorDescriptor& descriptor, const StridedLayout& layout,
    std::size_t storage_size) {
  validate_producer_envelope(
      descriptor, layout, dense_tensor_element_bytes(descriptor), storage_size);
}

/** @copydoc ps::validate_dense_tensor_producer_envelope */
void validate_dense_tensor_producer_envelope(
    const DenseTensorDescriptor& descriptor, const BlockedLayout& layout,
    std::size_t storage_size) {
  validate_blocked_producer_envelope(descriptor, layout, storage_size);
}

/**
 * @brief Private explicit-binding control block retained by BufferHandle
 * copies.
 *
 * @throws std::bad_alloc when allocating CPU bytes or retained ownership.
 * @note Exactly one of owned CPU storage or external ownership establishes the
 * allocation lifetime. Host visibility is independent from device/domain.
 */
struct BufferHandle::ControlBlock final {
  /** @brief Stable nonzero process-local physical allocation identity. */
  AllocationIdentity identity;

  /** @brief Complete concrete device identity. */
  DeviceId device{DeviceBackend::CPU};

  /** @brief Explicit allocation memory domain. */
  MemoryDomain memory_domain = MemoryDomain::Host;

  /** @brief Owned CPU bytes, empty for external/native bindings. */
  std::vector<std::byte> storage;

  /** @brief Shared external/native allocation owner, or null for CPU storage.
   */
  std::shared_ptr<void> external_owner;

  /** @brief Opaque native handle, or null for ordinary CPU storage. */
  void* native = nullptr;

  /** @brief Host-visible allocation start, or null when inaccessible. */
  std::byte* host_data = nullptr;

  /** @brief Positive complete allocation byte size. */
  std::size_t byte_size = 0U;

  /**
   * @brief Allocates one zero-initialized CPU byte range.
   *
   * @param identity_in Already minted nonzero allocation identity.
   * @param size Positive allocation size.
   * @throws std::bad_alloc when byte allocation fails.
   */
  ControlBlock(AllocationIdentity identity_in, std::size_t size)
      : identity(identity_in),
        storage(size, std::byte{0}),
        host_data(storage.data()),
        byte_size(size) {}

  /**
   * @brief Retains one external/native allocation and optional host pointer.
   * @param identity_in Fresh nonzero allocation identity.
   * @param owner_in Shared complete allocation owner.
   * @param native_in Non-null opaque native handle.
   * @param host_data_in Optional host-visible allocation start.
   * @param size Positive complete allocation byte size.
   * @param device_in Concrete device binding.
   * @param memory_domain_in Explicit allocation domain.
   * @throws Nothing under shared ownership movement.
   */
  ControlBlock(AllocationIdentity identity_in, std::shared_ptr<void> owner_in,
               void* native_in, std::byte* host_data_in, std::size_t size,
               DeviceId device_in, MemoryDomain memory_domain_in) noexcept
      : identity(identity_in),
        device(device_in),
        memory_domain(memory_domain_in),
        external_owner(std::move(owner_in)),
        native(native_in),
        host_data(host_data_in),
        byte_size(size) {}
};

/** @copydoc BufferHandle::BufferHandle */
BufferHandle::BufferHandle(std::shared_ptr<ControlBlock> control,
                           std::size_t offset, std::size_t length) noexcept
    : control_(std::move(control)), offset_(offset), length_(length) {}

/** @copydoc BufferHandle::allocate_for_builder */
BufferHandle BufferHandle::allocate_for_builder(std::size_t size) {
  if (size == 0U) {
    throw std::invalid_argument(
        "BufferHandle allocation size must be positive.");
  }
  const AllocationIdentity identity(
      process_identity_authority().mint_allocation_identity());
  return BufferHandle(std::make_shared<ControlBlock>(identity, size), 0U, size);
}

/** @copydoc BufferHandle::retain_external_binding */
BufferHandle BufferHandle::retain_external_binding(
    std::shared_ptr<void> owner, void* native_handle, std::byte* host_pointer,
    std::size_t size, DeviceId device, MemoryDomain memory_domain) {
  if (!owner || native_handle == nullptr || size == 0U) {
    throw std::invalid_argument(
        "External BufferHandle requires owner, native handle, and bytes.");
  }
  if ((memory_domain == MemoryDomain::Host ||
       memory_domain == MemoryDomain::HostPinned) &&
      host_pointer == nullptr) {
    throw std::invalid_argument(
        "Host-domain external BufferHandle requires a host pointer.");
  }
  const AllocationIdentity identity(
      process_identity_authority().mint_allocation_identity());
  return BufferHandle(
      std::make_shared<ControlBlock>(identity, std::move(owner), native_handle,
                                     host_pointer, size, device, memory_domain),
      0U, size);
}

/** @copydoc BufferHandle::valid */
bool BufferHandle::valid() const noexcept {
  return control_ != nullptr && length_ != 0U;
}

/** @copydoc BufferHandle::allocation_identity */
AllocationIdentity BufferHandle::allocation_identity() const {
  if (!valid()) {
    throw std::logic_error("Invalid BufferHandle has no allocation identity.");
  }
  return control_->identity;
}

/** @copydoc BufferHandle::allocation_offset */
std::size_t BufferHandle::allocation_offset() const {
  if (!valid()) {
    throw std::logic_error("Invalid BufferHandle has no allocation offset.");
  }
  return offset_;
}

/** @copydoc BufferHandle::size */
std::size_t BufferHandle::size() const {
  if (!valid()) {
    throw std::logic_error("Invalid BufferHandle has no byte length.");
  }
  return length_;
}

/** @copydoc BufferHandle::storage_binding */
StorageBinding BufferHandle::storage_binding() const {
  if (!valid()) {
    throw std::logic_error("Invalid BufferHandle has no storage binding.");
  }
  return StorageBinding{control_->identity, control_->device,
                        control_->memory_domain, control_->byte_size,
                        control_->host_data != nullptr};
}

/** @copydoc BufferHandle::host_visible */
bool BufferHandle::host_visible() const noexcept {
  return valid() && control_->host_data != nullptr;
}

/** @copydoc BufferHandle::subrange */
BufferHandle BufferHandle::subrange(std::size_t offset,
                                    std::size_t length) const {
  if (!valid()) {
    throw std::logic_error("Cannot subrange an invalid BufferHandle.");
  }
  if (length == 0U) {
    throw std::invalid_argument(
        "BufferHandle subrange length must be positive.");
  }
  if (offset > length_ || length > length_ - offset) {
    throw std::out_of_range(
        "BufferHandle subrange exceeds the retained range.");
  }
  return BufferHandle(control_, checked_add(offset_, offset), length);
}

/** @copydoc BufferHandle::acquire_read */
ReadLease BufferHandle::acquire_read() const {
  if (!valid()) {
    throw std::logic_error("Cannot read an invalid BufferHandle.");
  }
  if (!host_visible()) {
    throw BufferAccessError();
  }
  return ReadLease(*this);
}

/** @copydoc BufferHandle::read_pointer */
const std::byte* BufferHandle::read_pointer() const noexcept {
  return control_->host_data + offset_;
}

/** @copydoc BufferHandle::write_pointer */
std::byte* BufferHandle::write_pointer() const noexcept {
  return control_->host_data + offset_;
}

/** @copydoc BufferHandle::native_handle */
void* BufferHandle::native_handle() const noexcept {
  return valid() ? control_->native : nullptr;
}

/** @copydoc ReadLease::ReadLease */
ReadLease::ReadLease(BufferHandle handle) noexcept
    : handle_(std::move(handle)) {}

/** @copydoc ReadLease::valid */
bool ReadLease::valid() const noexcept {
  return handle_.valid();
}

/** @copydoc ReadLease::data */
const std::byte* ReadLease::data() const {
  if (!valid()) {
    throw std::logic_error("Invalid ReadLease has no readable pointer.");
  }
  return handle_.read_pointer();
}

/** @copydoc ReadLease::size */
std::size_t ReadLease::size() const {
  if (!valid()) {
    throw std::logic_error("Invalid ReadLease has no readable length.");
  }
  return handle_.size();
}

/** @copydoc ReadLease::allocation_identity */
AllocationIdentity ReadLease::allocation_identity() const {
  if (!valid()) {
    throw std::logic_error("Invalid ReadLease has no allocation identity.");
  }
  return handle_.allocation_identity();
}

/**
 * @brief Shared externally serialized producer-authority state.
 *
 * @note Atomics permit a moved lease to release on another thread without
 * racing diagnostic validity checks. ValueBuilder methods themselves remain
 * externally serialized.
 */
struct WriteLease::Authority final {
  /** @brief True until the builder seals successfully. */
  std::atomic<bool> builder_open{true};

  /** @brief True while exactly one WriteLease owns producer authority. */
  std::atomic<bool> lease_active{false};
};

/** @copydoc WriteLease::WriteLease */
WriteLease::WriteLease(BufferHandle handle,
                       std::shared_ptr<Authority> authority) noexcept
    : handle_(std::move(handle)), authority_(std::move(authority)) {}

/** @copydoc WriteLease::WriteLease */
WriteLease::WriteLease(WriteLease&& other) noexcept
    : handle_(std::move(other.handle_)),
      authority_(std::move(other.authority_)) {
  other.handle_ = BufferHandle{};
}

/** @copydoc WriteLease::operator= */
WriteLease& WriteLease::operator=(WriteLease&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = std::move(other.handle_);
    authority_ = std::move(other.authority_);
    other.handle_ = BufferHandle{};
  }
  return *this;
}

/** @copydoc WriteLease::~WriteLease */
WriteLease::~WriteLease() noexcept {
  release();
}

/** @copydoc WriteLease::release */
void WriteLease::release() noexcept {
  if (authority_) {
    authority_->lease_active.store(false, std::memory_order_release);
    authority_.reset();
  }
  handle_ = BufferHandle{};
}

/** @copydoc WriteLease::valid */
bool WriteLease::valid() const noexcept {
  return authority_ != nullptr && handle_.valid() &&
         authority_->builder_open.load(std::memory_order_acquire) &&
         authority_->lease_active.load(std::memory_order_acquire);
}

/** @copydoc WriteLease::data */
std::byte* WriteLease::data() const {
  if (!valid()) {
    throw std::logic_error("WriteLease has no active producer authority.");
  }
  return handle_.write_pointer();
}

/** @copydoc WriteLease::size */
std::size_t WriteLease::size() const {
  if (!valid()) {
    throw std::logic_error("WriteLease has no active producer range.");
  }
  return handle_.size();
}

/**
 * @brief Immutable implementation for one validated generic Value.
 *
 * @throws std::bad_alloc when copied descriptor/Layout/buffer storage cannot
 * allocate.
 * @note Instances are published only through shared_ptr<const Impl>. Ordinary
 * synchronous publication uses an already-Ready fence; source-private pending
 * publication closes ordinary builder authority before the Value escapes.
 * Provider buffers are destroyed before their retained generation lease.
 */
struct Value::Impl final {
  /** @brief Explicit logical representation discriminator. */
  ValueRepresentationKind representation = ValueRepresentationKind::DenseTensor;

  /** @brief Validated concrete logical descriptor. */
  DenseTensorDescriptor descriptor;

  /** @brief Optional validated explicit image-axis mapping. */
  std::optional<ImageFacet> image_facet;

  /** @brief Optional byte-preserved provider-defined logical descriptor. */
  std::optional<DataDescriptorEnvelope> provider_descriptor;

  /** @brief Exactly one validated physical Layout. */
  ValueLayout layout;

  /** @brief DenseTensor-only checked explicit storage-binding range. */
  BufferHandle buffer;

  /** @brief Provider generation declared before buffers so it dies last. */
  DataDefinitionLease provider_lease;

  /** @brief Provider-defined immutable checked storage-binding ranges. */
  std::vector<BufferHandle> provider_buffers;

  /** @brief Immutable observer of producer completion for this binding. */
  ReadyFence ready_fence;

  /** @brief Stable identity of this immutable logical publication. */
  ValueRevisionId revision;

  /** @brief Nonreused identity of the producer or transfer publication. */
  ProducerIdentity producer;

  /**
   * @brief Stores one completely validated immutable publication.
   *
   * @param descriptor_in Logical descriptor to retain.
   * @param image_facet_in Optional image facet to retain.
   * @param layout_in Tagged checked layout to retain.
   * @param buffer_in Sealed checked allocation range to retain.
   * @param ready_fence_in Valid producer-completion observer.
   * @param revision_in Fresh nonzero publication revision.
   * @param producer_in Fresh nonzero producer identity.
   * @throws std::bad_alloc when descriptor or layout vector moves must
   * allocate.
   * @note Inputs are already validated. Before the Value escapes, the caller
   *       either retires ordinary builder authority for an already-Ready
   *       publication or transfers the sole mutable path to the matching
   *       source-private pending producer.
   */
  Impl(DenseTensorDescriptor descriptor_in,
       std::optional<ImageFacet> image_facet_in, ValueLayout layout_in,
       BufferHandle buffer_in, ReadyFence ready_fence_in,
       ValueRevisionId revision_in, ProducerIdentity producer_in)
      : representation(ValueRepresentationKind::DenseTensor),
        descriptor(std::move(descriptor_in)),
        image_facet(std::move(image_facet_in)),
        layout(std::move(layout_in)),
        buffer(std::move(buffer_in)),
        ready_fence(std::move(ready_fence_in)),
        revision(revision_in),
        producer(producer_in) {}

  /**
   * @brief Stores one completely validated provider-defined publication.
   *
   * @param descriptor_in Byte-preserved Schema and ordered Facets.
   * @param layout_in Byte-preserved Layout and generic buffer envelopes.
   * @param buffers_in Valid sealed ranges in dense buffer-index order.
   * @param provider_lease_in Exact generation that validated all semantics.
   * @param ready_fence_in Already-Ready completion observer.
   * @param revision_in Fresh nonzero publication revision.
   * @param producer_in Fresh nonzero publication producer identity.
   * @throws std::bad_alloc when bounded metadata or buffer vectors cannot move.
   * @note Inputs are already generically and provider-semantically validated.
   *       Member order keeps provider code live until every retained buffer is
   *       destroyed.
   */
  Impl(DataDescriptorEnvelope descriptor_in, ProviderDefinedLayout layout_in,
       std::vector<BufferHandle> buffers_in,
       DataDefinitionLease provider_lease_in, ReadyFence ready_fence_in,
       ValueRevisionId revision_in, ProducerIdentity producer_in)
      : representation(ValueRepresentationKind::ProviderDefined),
        provider_descriptor(std::move(descriptor_in)),
        layout(std::move(layout_in)),
        provider_lease(std::move(provider_lease_in)),
        provider_buffers(std::move(buffers_in)),
        ready_fence(std::move(ready_fence_in)),
        revision(revision_in),
        producer(producer_in) {}
};

/**
 * @brief Exclusive producer implementation retained by ValueBuilder.
 *
 * @note Descriptor/layout state and allocation remain private until seal.
 */
struct ValueBuilder::Impl final {
  /** @brief Validated logical descriptor. */
  DenseTensorDescriptor descriptor;

  /** @brief Optional validated explicit image facet. */
  std::optional<ImageFacet> image_facet;

  /** @brief Validated exact Strided or Blocked producer layout. */
  ValueLayout layout;

  /** @brief Private complete allocation handle. */
  BufferHandle buffer;

  /** @brief Shared active/sealed producer authority state. */
  std::shared_ptr<WriteLease::Authority> authority;

  /** @brief True after this builder successfully publishes its Value. */
  bool sealed = false;

  /**
   * @brief Stores completely validated private producer state.
   *
   * @param descriptor_in Logical descriptor to own.
   * @param image_facet_in Optional image facet to own.
   * @param layout_in Tagged exact producer layout to own.
   * @param buffer_in Private complete allocation handle.
   * @param authority_in Fresh producer authority.
   * @throws Nothing under member move contracts.
   */
  Impl(DenseTensorDescriptor descriptor_in,
       std::optional<ImageFacet> image_facet_in, ValueLayout layout_in,
       BufferHandle buffer_in,
       std::shared_ptr<WriteLease::Authority> authority_in) noexcept
      : descriptor(std::move(descriptor_in)),
        image_facet(std::move(image_facet_in)),
        layout(std::move(layout_in)),
        buffer(std::move(buffer_in)),
        authority(std::move(authority_in)) {}
};

/**
 * @brief Exclusive source-private producer implementation for a pending Value.
 *
 * @note The BufferHandle remains mutable only while access_active is true.
 *       Terminal publication clears that handle before resolving completer.
 */
struct PendingValueProducer::Impl final {
  /** @brief Private mutable allocation range bound to one pending Value. */
  BufferHandle buffer;

  /** @brief Unique capability that resolves the matching Value fence. */
  FenceCompleter completer;

  /** @brief True while producer pointer access remains valid. */
  bool access_active = true;

  /**
   * @brief Stores one validated private producer capability.
   *
   * @param buffer_in Complete pending Value allocation range.
   * @param completer_in Matching unique fence completer.
   * @throws Nothing.
   */
  Impl(BufferHandle buffer_in, FenceCompleter completer_in) noexcept
      : buffer(std::move(buffer_in)), completer(std::move(completer_in)) {}
};

/** @copydoc ps::dense_tensor_element_bits */
std::size_t dense_tensor_element_bits(const DenseTensorDescriptor& descriptor) {
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
    case ElementSemantics::SignedInteger:
      if (descriptor.storage_encoding.kind ==
              StorageEncodingKind::NativeScalar &&
          (descriptor.storage_encoding.bit_width == 8U ||
           descriptor.storage_encoding.bit_width == 16U)) {
        return descriptor.storage_encoding.bit_width;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (descriptor.storage_encoding.kind ==
              StorageEncodingKind::NativeScalar &&
          (descriptor.storage_encoding.bit_width == 32U ||
           descriptor.storage_encoding.bit_width == 64U)) {
        return descriptor.storage_encoding.bit_width;
      }
      if (descriptor.storage_encoding.kind == StorageEncodingKind::Fp4E2M1 &&
          descriptor.storage_encoding.bit_width == 4U) {
        return 4U;
      }
      break;
  }
  throw std::invalid_argument(
      "DenseTensor element semantics and encoding are unsupported.");
}

/** @copydoc ps::dense_tensor_element_bytes */
std::size_t dense_tensor_element_bytes(
    const DenseTensorDescriptor& descriptor) {
  const std::size_t element_bits = dense_tensor_element_bits(descriptor);
  if (descriptor.storage_encoding.kind != StorageEncodingKind::NativeScalar ||
      element_bits % 8U != 0U) {
    throw std::invalid_argument(
        "DenseTensor element encoding is not whole-byte addressable.");
  }
  return element_bits / 8U;
}

/** @copydoc ValueBuilder::ValueBuilder */
ValueBuilder::ValueBuilder(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

/** @copydoc ValueBuilder::ValueBuilder */
ValueBuilder::ValueBuilder(ValueBuilder&& other) noexcept = default;

/** @copydoc ValueBuilder::operator= */
ValueBuilder& ValueBuilder::operator=(ValueBuilder&& other) noexcept = default;

/** @copydoc ValueBuilder::~ValueBuilder */
ValueBuilder::~ValueBuilder() noexcept = default;

/** @copydoc ValueBuilder::allocate_cpu_dense_tensor */
ValueBuilder ValueBuilder::allocate_cpu_dense_tensor(
    DenseTensorDescriptor descriptor, std::optional<ImageFacet> image_facet,
    StridedLayout layout, std::size_t storage_size) {
  (void)validate_descriptor_and_facet(descriptor, image_facet);
  validate_dense_tensor_producer_envelope(descriptor, layout, storage_size);

  DenseTensorDescriptor isolated_descriptor = descriptor;
  const std::optional<ImageFacet> isolated_image_facet = image_facet;
  StridedLayout isolated_layout = layout;
  BufferHandle buffer = BufferHandle::allocate_for_builder(storage_size);
  auto authority = std::make_shared<WriteLease::Authority>();
  return ValueBuilder(std::make_unique<Impl>(
      std::move(isolated_descriptor), isolated_image_facet,
      std::move(isolated_layout), std::move(buffer), std::move(authority)));
}

/** @copydoc ValueBuilder::allocate_cpu_blocked_dense_tensor */
ValueBuilder ValueBuilder::allocate_cpu_blocked_dense_tensor(
    DenseTensorDescriptor descriptor, BlockedLayout layout,
    std::size_t storage_size) {
  validate_dense_tensor_producer_envelope(descriptor, layout, storage_size);

  DenseTensorDescriptor isolated_descriptor = descriptor;
  BlockedLayout isolated_layout = layout;
  BufferHandle buffer = BufferHandle::allocate_for_builder(storage_size);
  auto authority = std::make_shared<WriteLease::Authority>();
  return ValueBuilder(
      std::make_unique<Impl>(std::move(isolated_descriptor), std::nullopt,
                             ValueLayout(std::move(isolated_layout)),
                             std::move(buffer), std::move(authority)));
}

/** @copydoc ValueBuilder::acquire_write */
WriteLease ValueBuilder::acquire_write() {
  if (!impl_) {
    throw std::logic_error("Moved-from ValueBuilder cannot issue a lease.");
  }
  if (impl_->sealed ||
      !impl_->authority->builder_open.load(std::memory_order_acquire)) {
    throw std::logic_error("Sealed ValueBuilder cannot issue a lease.");
  }
  bool expected = false;
  if (!impl_->authority->lease_active.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    throw std::logic_error("ValueBuilder already has an active WriteLease.");
  }
  return WriteLease(impl_->buffer, impl_->authority);
}

/** @copydoc ValueBuilder::seal */
Value ValueBuilder::seal() {
  if (!impl_) {
    throw std::logic_error("Moved-from ValueBuilder cannot seal.");
  }
  if (impl_->sealed ||
      !impl_->authority->builder_open.load(std::memory_order_acquire)) {
    throw std::logic_error("ValueBuilder has already sealed.");
  }
  if (impl_->authority->lease_active.load(std::memory_order_acquire)) {
    throw std::logic_error(
        "ValueBuilder cannot seal while a WriteLease is active.");
  }

  const ValueRevisionId revision(
      process_identity_authority().mint_value_revision());
  const ProducerIdentity producer(
      process_identity_authority().mint_producer_identity());
  auto published = std::make_shared<const Value::Impl>(
      impl_->descriptor, impl_->image_facet, impl_->layout, impl_->buffer,
      ReadyFence::already_ready(), revision, producer);

  impl_->authority->builder_open.store(false, std::memory_order_release);
  impl_->sealed = true;
  return Value(std::move(published));
}

/** @copydoc ValueBuilder::sealed */
bool ValueBuilder::sealed() const noexcept {
  return impl_ != nullptr && impl_->sealed;
}

/** @copydoc PendingValueProducer::PendingValueProducer */
PendingValueProducer::PendingValueProducer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

/** @copydoc PendingValueProducer::PendingValueProducer */
PendingValueProducer::PendingValueProducer(
    PendingValueProducer&& other) noexcept =
    default;  // NOLINT(whitespace/indent_namespace)

/** @copydoc PendingValueProducer::operator= */
PendingValueProducer& PendingValueProducer::operator=(
    PendingValueProducer&& other) noexcept {
  if (this != &other) {
    cancel();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

/** @copydoc PendingValueProducer::~PendingValueProducer */
PendingValueProducer::~PendingValueProducer() noexcept {
  cancel();
}

/** @copydoc PendingValueProducer::valid */
bool PendingValueProducer::valid() const noexcept {
  return impl_ != nullptr && impl_->completer.valid();
}

/** @copydoc PendingValueProducer::data */
std::byte* PendingValueProducer::data() const {
  if (!impl_ || !impl_->access_active || !impl_->buffer.valid()) {
    throw std::logic_error(
        "PendingValueProducer has no active write capability.");
  }
  return impl_->buffer.write_pointer();
}

/** @copydoc PendingValueProducer::size */
std::size_t PendingValueProducer::size() const {
  if (!impl_ || !impl_->access_active || !impl_->buffer.valid()) {
    throw std::logic_error(
        "PendingValueProducer has no active write capability.");
  }
  return impl_->buffer.size();
}

/** @copydoc PendingValueProducer::revoke_access */
void PendingValueProducer::revoke_access() noexcept {
  if (impl_) {
    impl_->access_active = false;
    impl_->buffer = BufferHandle{};
  }
}

/** @copydoc PendingValueProducer::complete_ready */
bool PendingValueProducer::complete_ready() noexcept {
  if (!impl_) {
    return false;
  }
  revoke_access();
  const bool published = impl_->completer.complete_ready();
  impl_.reset();
  return published;
}

/** @copydoc PendingValueProducer::complete_failed */
bool PendingValueProducer::complete_failed(ReadyFenceFailure failure) {
  if (!impl_) {
    return false;
  }
  revoke_access();
  const bool published = impl_->completer.complete_failed(std::move(failure));
  impl_.reset();
  return published;
}

/** @copydoc PendingValueProducer::cancel */
bool PendingValueProducer::cancel() noexcept {
  if (!impl_) {
    return false;
  }
  revoke_access();
  const bool published = impl_->completer.cancel();
  impl_.reset();
  return published;
}

/** @copydoc PendingValuePublisher::allocate_cpu_dense_tensor */
PendingValuePublication PendingValuePublisher::allocate_cpu_dense_tensor(
    DenseTensorDescriptor descriptor, std::optional<ImageFacet> image_facet,
    StridedLayout layout, std::size_t storage_size,
    std::optional<ValueRevisionId> replica_revision) {
  if (replica_revision.has_value() && !replica_revision->valid()) {
    throw std::invalid_argument(
        "Pending CPU Value replica revision must be valid.");
  }
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      std::move(descriptor), std::move(image_facet), std::move(layout),
      storage_size);
  if (!builder.impl_ || builder.impl_->sealed ||
      !builder.impl_->authority->builder_open.load(std::memory_order_acquire)) {
    throw std::logic_error(
        "Pending Value publication requires an open builder.");
  }
  if (builder.impl_->authority->lease_active.load(std::memory_order_acquire)) {
    throw std::logic_error(
        "Pending Value publication cannot retain an ordinary WriteLease.");
  }

  PendingReadyFence pending_fence = make_pending_ready_fence();
  const ValueRevisionId revision =
      replica_revision.has_value()
          ? *replica_revision
          : ValueRevisionId(process_identity_authority().mint_value_revision());
  const ProducerIdentity producer_identity(
      process_identity_authority().mint_producer_identity());
  auto published = std::make_shared<const Value::Impl>(
      builder.impl_->descriptor, builder.impl_->image_facet,
      builder.impl_->layout, builder.impl_->buffer, pending_fence.fence,
      revision, producer_identity);
  auto producer = std::make_unique<PendingValueProducer::Impl>(
      builder.impl_->buffer, std::move(pending_fence.completer));

  builder.impl_->authority->builder_open.store(false,
                                               std::memory_order_release);
  builder.impl_->sealed = true;
  return {Value(std::move(published)),
          PendingValueProducer(std::move(producer))};
}

/** @copydoc PendingValuePublisher::allocate_cpu_blocked_dense_tensor */
PendingValuePublication
PendingValuePublisher::allocate_cpu_blocked_dense_tensor(
    DenseTensorDescriptor descriptor, BlockedLayout layout,
    std::size_t storage_size, std::optional<ValueRevisionId> replica_revision) {
  if (replica_revision.has_value() && !replica_revision->valid()) {
    throw std::invalid_argument(
        "Pending blocked CPU Value replica revision must be valid.");
  }
  ValueBuilder builder = ValueBuilder::allocate_cpu_blocked_dense_tensor(
      std::move(descriptor), std::move(layout), storage_size);
  if (!builder.impl_ || builder.impl_->sealed ||
      !builder.impl_->authority->builder_open.load(std::memory_order_acquire)) {
    throw std::logic_error(
        "Pending blocked Value publication requires an open builder.");
  }
  if (builder.impl_->authority->lease_active.load(std::memory_order_acquire)) {
    throw std::logic_error(
        "Pending blocked Value cannot retain an ordinary WriteLease.");
  }

  PendingReadyFence pending_fence = make_pending_ready_fence();
  const ValueRevisionId revision =
      replica_revision.has_value()
          ? *replica_revision
          : ValueRevisionId(process_identity_authority().mint_value_revision());
  const ProducerIdentity producer_identity(
      process_identity_authority().mint_producer_identity());
  auto published = std::make_shared<const Value::Impl>(
      builder.impl_->descriptor, std::nullopt, builder.impl_->layout,
      builder.impl_->buffer, pending_fence.fence, revision, producer_identity);
  auto producer = std::make_unique<PendingValueProducer::Impl>(
      builder.impl_->buffer, std::move(pending_fence.completer));

  builder.impl_->authority->builder_open.store(false,
                                               std::memory_order_release);
  builder.impl_->sealed = true;
  return {Value(std::move(published)),
          PendingValueProducer(std::move(producer))};
}

/** @copydoc PendingDeviceValueProducer::operator= */
PendingDeviceValueProducer& PendingDeviceValueProducer::operator=(
    PendingDeviceValueProducer&& other) noexcept {
  if (this != &other) {
    (void)cancel();
    completer_ = std::move(other.completer_);
  }
  return *this;
}

/** @copydoc PendingDeviceValueProducer::~PendingDeviceValueProducer */
PendingDeviceValueProducer::~PendingDeviceValueProducer() noexcept {
  (void)cancel();
}

/** @copydoc PendingDeviceValueProducer::valid */
bool PendingDeviceValueProducer::valid() const noexcept {
  return completer_.valid();
}

/** @copydoc PendingDeviceValueProducer::complete_ready */
bool PendingDeviceValueProducer::complete_ready() noexcept {
  return completer_.complete_ready();
}

/** @copydoc PendingDeviceValueProducer::complete_failed */
bool PendingDeviceValueProducer::complete_failed(ReadyFenceFailure failure) {
  return completer_.complete_failed(std::move(failure));
}

/** @copydoc PendingDeviceValueProducer::cancel */
bool PendingDeviceValueProducer::cancel() noexcept {
  return completer_.cancel();
}

/** @copydoc PendingDeviceValuePublisher::publish_dense_tensor */
PendingDeviceValuePublication PendingDeviceValuePublisher::publish_dense_tensor(
    DenseTensorDescriptor descriptor, std::optional<ImageFacet> image_facet,
    StridedLayout layout, std::shared_ptr<void> owner, void* native_handle,
    std::byte* host_pointer, std::size_t storage_size, DeviceId device,
    MemoryDomain memory_domain,
    std::optional<ValueRevisionId> replica_revision) {
  const std::size_t element_bytes =
      validate_descriptor_and_facet(descriptor, image_facet);
  const AddressEnvelope envelope =
      compute_address_envelope(descriptor, layout, element_bytes);
  if (storage_size == 0U || envelope.upper_end > storage_size) {
    throw std::out_of_range(
        "Pending device Value layout exceeds its storage binding.");
  }
  if (replica_revision.has_value() && !replica_revision->valid()) {
    throw std::invalid_argument(
        "Pending device Value replica revision must be valid.");
  }

  BufferHandle buffer = BufferHandle::retain_external_binding(
      std::move(owner), native_handle, host_pointer, storage_size, device,
      memory_domain);
  PendingReadyFence pending_fence = make_pending_ready_fence();
  ValueRevisionId revision;
  if (replica_revision.has_value()) {
    revision = *replica_revision;
  } else {
    revision =
        ValueRevisionId(process_identity_authority().mint_value_revision());
  }
  const ProducerIdentity producer(
      process_identity_authority().mint_producer_identity());
  auto published = std::make_shared<const Value::Impl>(
      std::move(descriptor), std::move(image_facet), std::move(layout),
      std::move(buffer), pending_fence.fence, revision, producer);
  return {Value(std::move(published)),
          PendingDeviceValueProducer(std::move(pending_fence.completer))};
}

/** @copydoc PendingDeviceValuePublisher::publish_blocked_dense_tensor */
PendingDeviceValuePublication
PendingDeviceValuePublisher::publish_blocked_dense_tensor(
    DenseTensorDescriptor descriptor, BlockedLayout layout,
    std::shared_ptr<void> owner, void* native_handle, std::byte* host_pointer,
    std::size_t storage_size, DeviceId device, MemoryDomain memory_domain,
    std::optional<ValueRevisionId> replica_revision) {
  validate_dense_tensor_producer_envelope(descriptor, layout, storage_size);
  if (replica_revision.has_value() && !replica_revision->valid()) {
    throw std::invalid_argument(
        "Pending blocked device Value replica revision must be valid.");
  }

  BufferHandle buffer = BufferHandle::retain_external_binding(
      std::move(owner), native_handle, host_pointer, storage_size, device,
      memory_domain);
  PendingReadyFence pending_fence = make_pending_ready_fence();
  const ValueRevisionId revision =
      replica_revision.has_value()
          ? *replica_revision
          : ValueRevisionId(process_identity_authority().mint_value_revision());
  const ProducerIdentity producer(
      process_identity_authority().mint_producer_identity());
  auto published = std::make_shared<const Value::Impl>(
      std::move(descriptor), std::nullopt, ValueLayout(std::move(layout)),
      std::move(buffer), pending_fence.fence, revision, producer);
  return {Value(std::move(published)),
          PendingDeviceValueProducer(std::move(pending_fence.completer))};
}

/** @copydoc Value::Value */
Value::Value(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

/** @copydoc Value::from_cpu_dense_tensor */
Value Value::from_cpu_dense_tensor(DenseTensorDescriptor descriptor,
                                   std::optional<ImageFacet> image_facet,
                                   StridedLayout layout,
                                   std::vector<std::byte> storage) {
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      std::move(descriptor), std::move(image_facet), std::move(layout),
      storage.size());
  {
    WriteLease lease = builder.acquire_write();
    std::memcpy(lease.data(), storage.data(), storage.size());
  }
  return builder.seal();
}

/** @copydoc Value::from_cpu_dense_tensor */
Value Value::from_cpu_dense_tensor(DenseTensorDescriptor descriptor,
                                   std::optional<ImageFacet> image_facet,
                                   StridedLayout layout, BufferHandle buffer) {
  if (!buffer.valid()) {
    throw std::invalid_argument(
        "DenseTensor Value requires a valid BufferHandle.");
  }
  const std::size_t element_bytes =
      validate_descriptor_and_facet(descriptor, image_facet);
  const AddressEnvelope envelope =
      compute_address_envelope(descriptor, layout, element_bytes);
  if (envelope.upper_end > buffer.size()) {
    throw std::out_of_range(
        "DenseTensor signed layout exceeds its BufferHandle.");
  }

  const ValueRevisionId revision(
      process_identity_authority().mint_value_revision());
  const ProducerIdentity producer(
      process_identity_authority().mint_producer_identity());
  DenseTensorDescriptor isolated_descriptor = descriptor;
  const std::optional<ImageFacet> isolated_image_facet = image_facet;
  StridedLayout isolated_layout = layout;
  return Value(std::make_shared<const Impl>(
      std::move(isolated_descriptor), isolated_image_facet,
      std::move(isolated_layout), std::move(buffer),
      ReadyFence::already_ready(), revision, producer));
}

/** @copydoc Value::from_cpu_blocked_dense_tensor */
Value Value::from_cpu_blocked_dense_tensor(DenseTensorDescriptor descriptor,
                                           BlockedLayout layout,
                                           std::vector<std::byte> storage) {
  ValueBuilder builder = ValueBuilder::allocate_cpu_blocked_dense_tensor(
      std::move(descriptor), std::move(layout), storage.size());
  {
    WriteLease lease = builder.acquire_write();
    std::memcpy(lease.data(), storage.data(), storage.size());
  }
  return builder.seal();
}

/** @copydoc Value::from_cpu_blocked_dense_tensor */
Value Value::from_cpu_blocked_dense_tensor(DenseTensorDescriptor descriptor,
                                           BlockedLayout layout,
                                           BufferHandle buffer) {
  if (!buffer.valid()) {
    throw std::invalid_argument(
        "Blocked DenseTensor Value requires a valid BufferHandle.");
  }
  const std::size_t required_size =
      blocked_required_storage_size(descriptor, layout);
  if (required_size > buffer.size()) {
    throw std::out_of_range(
        "Blocked DenseTensor layout exceeds its BufferHandle.");
  }

  const ValueRevisionId revision(
      process_identity_authority().mint_value_revision());
  const ProducerIdentity producer(
      process_identity_authority().mint_producer_identity());
  DenseTensorDescriptor isolated_descriptor = descriptor;
  BlockedLayout isolated_layout = layout;
  return Value(std::make_shared<const Impl>(
      std::move(isolated_descriptor), std::nullopt,
      ValueLayout(std::move(isolated_layout)), std::move(buffer),
      ReadyFence::already_ready(), revision, producer));
}

/** @copydoc Value::from_provider_defined */
Value Value::from_provider_defined(DataDefinitionRegistry& registry,
                                   DataDescriptorEnvelope descriptor,
                                   ProviderDefinedLayout layout,
                                   std::vector<BufferHandle> buffers) {
  validate_data_descriptor_envelope(descriptor);
  std::vector<std::size_t> buffer_sizes;
  buffer_sizes.reserve(buffers.size());
  for (const BufferHandle& buffer : buffers) {
    if (!buffer.valid()) {
      throw ExtensionContractError(
          ExtensionErrorCode::InvalidBinding,
          "Provider-defined Value contains an invalid BufferHandle.");
    }
    buffer_sizes.push_back(buffer.size());
  }
  validate_provider_defined_layout(layout, buffer_sizes);

  DataDefinitionResolveResult resolved = registry.resolve(descriptor, layout);
  if (!resolved.ok()) {
    const ExtensionErrorCode code =
        resolved.status == DataDefinitionResolveStatus::UnsupportedSchemaVersion
            ? ExtensionErrorCode::UnsupportedSchemaVersion
            : ExtensionErrorCode::MissingProvider;
    throw ExtensionContractError(
        code, resolved.diagnostic.empty()
                  ? "No active provider generation owns the complete bundle."
                  : std::move(resolved.diagnostic));
  }
  DataDefinitionLease provider_lease = std::move(resolved.lease);
  provider_lease.validate(descriptor, layout, buffers);

  const ValueRevisionId revision(
      process_identity_authority().mint_value_revision());
  const ProducerIdentity producer(
      process_identity_authority().mint_producer_identity());
  return Value(std::make_shared<const Impl>(
      std::move(descriptor), std::move(layout), std::move(buffers),
      std::move(provider_lease), ReadyFence::already_ready(), revision,
      producer));
}

/** @copydoc Value::valid */
bool Value::valid() const noexcept {
  return impl_ != nullptr;
}

/** @copydoc Value::representation_kind */
ValueRepresentationKind Value::representation_kind() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no representation kind.");
  }
  return impl_->representation;
}

/** @copydoc Value::dense_tensor_descriptor */
const DenseTensorDescriptor& Value::dense_tensor_descriptor() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no DenseTensor descriptor.");
  }
  if (impl_->representation != ValueRepresentationKind::DenseTensor) {
    throw std::logic_error(
        "Provider-defined Value has no DenseTensor descriptor.");
  }
  return impl_->descriptor;
}

/** @copydoc Value::image_facet */
const std::optional<ImageFacet>& Value::image_facet() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no ImageFacet.");
  }
  if (impl_->representation != ValueRepresentationKind::DenseTensor) {
    throw std::logic_error("Provider-defined Value has no ImageFacet.");
  }
  return impl_->image_facet;
}

/** @copydoc Value::provider_defined_descriptor */
const DataDescriptorEnvelope& Value::provider_defined_descriptor() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no provider-defined descriptor.");
  }
  if (impl_->representation != ValueRepresentationKind::ProviderDefined ||
      !impl_->provider_descriptor.has_value()) {
    throw std::logic_error(
        "DenseTensor Value has no provider-defined descriptor.");
  }
  return *impl_->provider_descriptor;
}

/** @copydoc Value::storage_layout_kind */
StorageLayoutKind Value::storage_layout_kind() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no storage layout kind.");
  }
  if (std::holds_alternative<StridedLayout>(impl_->layout)) {
    return StorageLayoutKind::Strided;
  }
  if (std::holds_alternative<BlockedLayout>(impl_->layout)) {
    return StorageLayoutKind::Blocked;
  }
  return StorageLayoutKind::ProviderDefined;
}

/** @copydoc Value::strided_layout */
const StridedLayout& Value::strided_layout() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no StridedLayout.");
  }
  const auto* layout = std::get_if<StridedLayout>(&impl_->layout);
  if (layout == nullptr) {
    throw std::logic_error("Non-Strided Value has no StridedLayout.");
  }
  return *layout;
}

/** @copydoc Value::blocked_layout */
const BlockedLayout& Value::blocked_layout() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no BlockedLayout.");
  }
  const auto* layout = std::get_if<BlockedLayout>(&impl_->layout);
  if (layout == nullptr) {
    throw std::logic_error("Non-Blocked Value has no BlockedLayout.");
  }
  return *layout;
}

/** @copydoc Value::provider_defined_layout */
const ProviderDefinedLayout& Value::provider_defined_layout() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no provider-defined Layout.");
  }
  const auto* layout = std::get_if<ProviderDefinedLayout>(&impl_->layout);
  if (layout == nullptr) {
    throw std::logic_error("DenseTensor Value has no provider-defined Layout.");
  }
  return *layout;
}

/** @copydoc Value::buffer_count */
std::size_t Value::buffer_count() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no buffers.");
  }
  return impl_->representation == ValueRepresentationKind::ProviderDefined
             ? impl_->provider_buffers.size()
             : 1U;
}

/** @copydoc Value::storage_size */
std::size_t Value::storage_size() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no storage.");
  }
  if (impl_->representation != ValueRepresentationKind::DenseTensor) {
    throw std::logic_error(
        "Provider-defined Value requires indexed binding inspection.");
  }
  return impl_->buffer.size();
}

/** @copydoc Value::ready_fence */
ReadyFence Value::ready_fence() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no ReadyFence.");
  }
  return impl_->ready_fence;
}

/** @copydoc Value::storage_binding */
StorageBinding Value::storage_binding() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no storage binding.");
  }
  if (impl_->representation != ValueRepresentationKind::DenseTensor) {
    throw std::logic_error(
        "Provider-defined Value requires indexed binding inspection.");
  }
  return impl_->buffer.storage_binding();
}

/** @copydoc Value::storage_binding */
StorageBinding Value::storage_binding(std::size_t buffer_index) const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no indexed storage binding.");
  }
  if (impl_->representation == ValueRepresentationKind::DenseTensor) {
    if (buffer_index != 0U) {
      throw std::out_of_range("DenseTensor Value has exactly one buffer.");
    }
    return impl_->buffer.storage_binding();
  }
  if (buffer_index >= impl_->provider_buffers.size()) {
    throw std::out_of_range(
        "Provider-defined buffer index is outside the Value binding set.");
  }
  return impl_->provider_buffers[buffer_index].storage_binding();
}

/** @copydoc Value::producer_identity */
ProducerIdentity Value::producer_identity() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no producer identity.");
  }
  return impl_->producer;
}

/** @copydoc Value::plan_access */
AccessPlan Value::plan_access(AccessTarget target) const {
  return plan_value_access(*this, target);
}

/** @copydoc Value::buffer_handle */
const BufferHandle& Value::buffer_handle() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no BufferHandle.");
  }
  if (impl_->representation != ValueRepresentationKind::DenseTensor) {
    throw std::logic_error(
        "Provider-defined Value does not expose a naked BufferHandle.");
  }
  const ReadyFenceSnapshot snapshot = impl_->ready_fence.poll();
  if (!snapshot.ready()) {
    throw ReadyFenceAccessError(snapshot);
  }
  return impl_->buffer;
}

/** @copydoc Value::acquire_provider_read */
ProviderReadLease Value::acquire_provider_read(std::size_t buffer_index) const {
  if (!impl_ ||
      impl_->representation != ValueRepresentationKind::ProviderDefined ||
      !impl_->provider_lease.valid()) {
    throw std::logic_error(
        "Provider read access requires a valid provider-defined Value.");
  }
  if (buffer_index >= impl_->provider_buffers.size()) {
    throw std::out_of_range(
        "Provider-defined buffer index is outside the Value binding set.");
  }
  return ProviderReadLease(impl_->provider_buffers[buffer_index].acquire_read(),
                           impl_->provider_lease);
}

/** @copydoc Value::provider_generation */
std::uint64_t Value::provider_generation() const {
  if (!impl_ ||
      impl_->representation != ValueRepresentationKind::ProviderDefined ||
      !impl_->provider_lease.valid()) {
    throw std::logic_error(
        "Provider generation requires a valid provider-defined Value.");
  }
  return impl_->provider_lease.generation();
}

/** @copydoc Value::query_property */
PropertyQueryResult Value::query_property(PropertyQuery query) const {
  if (!impl_ ||
      impl_->representation != ValueRepresentationKind::ProviderDefined ||
      !impl_->provider_descriptor.has_value() ||
      !impl_->provider_lease.valid()) {
    throw std::logic_error(
        "Property query requires a valid provider-defined Value.");
  }
  return impl_->provider_lease.query(*impl_->provider_descriptor,
                                     provider_defined_layout(),
                                     impl_->provider_buffers, query);
}

/** @copydoc Value::evaluate_data_spec */
DataSpecResult Value::evaluate_data_spec(const DataSpec& spec) const {
  if (!impl_ ||
      impl_->representation != ValueRepresentationKind::ProviderDefined ||
      !impl_->provider_descriptor.has_value() ||
      !impl_->provider_lease.valid()) {
    throw std::logic_error(
        "DataSpec evaluation requires a valid provider-defined Value.");
  }
  return impl_->provider_lease.evaluate(*impl_->provider_descriptor,
                                        provider_defined_layout(),
                                        impl_->provider_buffers, spec);
}

/** @copydoc Value::evaluate_region */
ProviderRegionResult Value::evaluate_region(
    const RegionSet& region, RegionComplexityBudget budget) const {
  if (!impl_ ||
      impl_->representation != ValueRepresentationKind::ProviderDefined ||
      !impl_->provider_descriptor.has_value() ||
      !impl_->provider_lease.valid()) {
    throw std::logic_error(
        "Region evaluation requires a valid provider-defined Value.");
  }
  return impl_->provider_lease.evaluate(
      *impl_->provider_descriptor, provider_defined_layout(),
      impl_->provider_buffers, region, budget);
}

/** @copydoc Value::create_provider_owner */
ProviderOwner Value::create_provider_owner() const {
  if (!impl_ ||
      impl_->representation != ValueRepresentationKind::ProviderDefined ||
      !impl_->provider_lease.valid()) {
    throw std::logic_error(
        "Provider owner requires a valid provider-defined Value.");
  }
  return impl_->provider_lease.create_owner();
}

/** @copydoc Value::allocation_identity */
AllocationIdentity Value::allocation_identity() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no allocation identity.");
  }
  if (impl_->representation != ValueRepresentationKind::DenseTensor) {
    throw std::logic_error(
        "Provider-defined Value has no single allocation identity.");
  }
  return impl_->buffer.allocation_identity();
}

/** @copydoc Value::revision_id */
ValueRevisionId Value::revision_id() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no revision.");
  }
  return impl_->revision;
}

/** @copydoc ps::compute_content_digest */
ContentDigestResult compute_content_digest(const Value& value) {
  if (!value.impl_) {
    return {ContentDigestState::InvalidDescriptor, std::nullopt,
            "ContentDigest requires a valid Value."};
  }
  if (value.impl_->representation == ValueRepresentationKind::DenseTensor) {
    return internal::compute_dense_tensor_content_digest(value);
  }
  if (!value.impl_->provider_descriptor.has_value()) {
    return {ContentDigestState::InvalidDescriptor, std::nullopt,
            "Provider ContentDigest requires a valid descriptor."};
  }
  if (!value.impl_->provider_lease.valid()) {
    return {ContentDigestState::MissingProvider, std::nullopt,
            "ContentDigest requires a retained provider generation."};
  }
  try {
    return {ContentDigestState::Available,
            value.impl_->provider_lease.content_digest(
                *value.impl_->provider_descriptor,
                value.provider_defined_layout(), value.impl_->provider_buffers),
            {}};
  } catch (const ExtensionContractError& error) {
    ContentDigestState state = ContentDigestState::ProviderFailure;
    switch (error.code()) {
      case ExtensionErrorCode::MissingProvider:
        state = ContentDigestState::MissingProvider;
        break;
      case ExtensionErrorCode::UnsupportedSchemaVersion:
        state = ContentDigestState::UnsupportedSchemaVersion;
        break;
      case ExtensionErrorCode::PayloadUnavailable:
        state = ContentDigestState::PayloadUnavailable;
        break;
      case ExtensionErrorCode::InvalidEnvelope:
      case ExtensionErrorCode::InvalidBinding:
      case ExtensionErrorCode::InvalidSerialization:
        state = ContentDigestState::InvalidDescriptor;
        break;
      case ExtensionErrorCode::ProviderRejected:
      case ExtensionErrorCode::InvalidProviderOutput:
        state = ContentDigestState::ProviderFailure;
        break;
    }
    return {state, std::nullopt, error.what()};
  }
}

/** @copydoc ps::plan_value_access */
AccessPlan plan_value_access(const Value& value, AccessTarget target) {
  if (!value.valid()) {
    throw std::invalid_argument("Access planning requires a valid Value.");
  }
  const StorageBinding binding = value.storage_binding();
  const ReadyFenceState fence_state = value.ready_fence().poll().state();
  const bool same_binding = binding.device == target.device &&
                            binding.memory_domain == target.memory_domain;
  const bool direct_host_access = !target.host_read || binding.host_visible;
  if (same_binding && direct_host_access && !target.require_distinct_binding) {
    return AccessPlan(
        AccessPlanKind::Direct, value.revision_id().value(), binding, target,
        VisibilityObligations{fence_state == ReadyFenceState::Pending, false,
                              false},
        0U);
  }

  const DeviceBackend source_backend = binding.device.backend();
  const DeviceBackend target_backend = target.device.backend();
  const bool supported_direction = (source_backend == DeviceBackend::CPU &&
                                    (target_backend == DeviceBackend::CPU ||
                                     target_backend == DeviceBackend::Metal)) ||
                                   (source_backend == DeviceBackend::Metal &&
                                    (target_backend == DeviceBackend::CPU ||
                                     target_backend == DeviceBackend::Metal));
  if (!supported_direction) {
    return AccessPlan(
        AccessPlanKind::Unsupported, value.revision_id().value(), binding,
        target,
        VisibilityObligations{fence_state == ReadyFenceState::Pending, false,
                              false},
        0U);
  }

  const bool crosses_device = binding.device != target.device;
  const bool involves_metal = source_backend == DeviceBackend::Metal ||
                              target_backend == DeviceBackend::Metal;
  return AccessPlan(
      AccessPlanKind::Transfer, value.revision_id().value(), binding, target,
      VisibilityObligations{fence_state == ReadyFenceState::Pending,
                            involves_metal, crosses_device},
      value.storage_size());
}

/** @copydoc DenseTensorView::DenseTensorView */
DenseTensorView::DenseTensorView(Value value) : value_(std::move(value)) {
  if (!value_.valid()) {
    throw std::invalid_argument(
        "DenseTensorView requires a valid DenseTensor Value.");
  }
  if (value_.storage_layout_kind() != StorageLayoutKind::Strided) {
    throw std::invalid_argument(
        "DenseTensorView requires a byte-addressed Strided Value.");
  }
  read_lease_ = value_.buffer_handle().acquire_read();
}

/** @copydoc DenseTensorView::value */
const Value& DenseTensorView::value() const noexcept {
  return value_;
}

/** @copydoc DenseTensorView::descriptor */
const DenseTensorDescriptor& DenseTensorView::descriptor() const noexcept {
  return value_.dense_tensor_descriptor();
}

/** @copydoc DenseTensorView::layout */
const StridedLayout& DenseTensorView::layout() const noexcept {
  return value_.strided_layout();
}

/** @copydoc DenseTensorView::storage_size */
std::size_t DenseTensorView::storage_size() const noexcept {
  return value_.storage_size();
}

/** @copydoc DenseTensorView::data */
const std::byte* DenseTensorView::data() const noexcept {
  return read_lease_.data() + layout().byte_offset;
}

/** @copydoc DenseTensorView::element_data */
const std::byte* DenseTensorView::element_data(
    const std::vector<std::size_t>& coordinates) const {
  const DenseTensorDescriptor& tensor_descriptor = descriptor();
  if (coordinates.size() != tensor_descriptor.shape.size()) {
    throw std::invalid_argument(
        "DenseTensor coordinate rank must match tensor rank.");
  }

  std::size_t positive = 0U;
  std::size_t negative = 0U;
  for (std::size_t axis = 0U; axis < coordinates.size(); ++axis) {
    if (coordinates[axis] >= tensor_descriptor.shape[axis]) {
      throw std::out_of_range("DenseTensor coordinate is outside its extent.");
    }
    accumulate_coordinate_displacement(
        coordinates[axis], layout().byte_strides[axis], &positive, &negative);
  }
  return read_lease_.data() +
         resolve_coordinate_offset(layout(), positive, negative);
}

/** @copydoc ImageView::ImageView */
ImageView::ImageView(Value value) : tensor_(std::move(value)) {
  const std::optional<ImageFacet>& facet = tensor_.value().image_facet();
  if (!facet.has_value()) {
    throw std::invalid_argument("ImageView requires an explicit ImageFacet.");
  }
  image_facet_ = *facet;

  const DenseTensorDescriptor& tensor_descriptor = tensor_.descriptor();
  for (std::size_t axis = 0U; axis < tensor_descriptor.shape.size(); ++axis) {
    if (!is_image_axis(image_facet_, axis) &&
        tensor_descriptor.shape[axis] != 1U) {
      throw std::invalid_argument(
          "ImageView unassigned tensor axes must be singleton.");
    }
  }

  width_ = tensor_descriptor.shape[image_facet_.x_axis];
  height_ = tensor_descriptor.shape[image_facet_.y_axis];
  channels_ = image_facet_.channel_axis.has_value()
                  ? tensor_descriptor.shape[*image_facet_.channel_axis]
                  : 1U;
  const std::size_t maximum_image_extent =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (width_ > maximum_image_extent || height_ > maximum_image_extent ||
      channels_ > maximum_image_extent) {
    throw std::invalid_argument(
        "ImageView extent exceeds the current ImageBuffer adapter domain.");
  }
  element_bytes_ = dense_tensor_element_bytes(tensor_descriptor);
}

/** @copydoc ImageView::value */
const Value& ImageView::value() const noexcept {
  return tensor_.value();
}

/** @copydoc ImageView::descriptor */
const DenseTensorDescriptor& ImageView::descriptor() const noexcept {
  return tensor_.descriptor();
}

/** @copydoc ImageView::image_facet */
const ImageFacet& ImageView::image_facet() const noexcept {
  return image_facet_;
}

/** @copydoc ImageView::layout */
const StridedLayout& ImageView::layout() const noexcept {
  return tensor_.layout();
}

/** @copydoc ImageView::width */
std::size_t ImageView::width() const noexcept {
  return width_;
}

/** @copydoc ImageView::height */
std::size_t ImageView::height() const noexcept {
  return height_;
}

/** @copydoc ImageView::channels */
std::size_t ImageView::channels() const noexcept {
  return channels_;
}

/** @copydoc ImageView::element_bytes */
std::size_t ImageView::element_bytes() const noexcept {
  return element_bytes_;
}

/** @copydoc ImageView::row_stride */
std::ptrdiff_t ImageView::row_stride() const noexcept {
  return layout().byte_strides[image_facet_.y_axis];
}

/** @copydoc ImageView::channel_data */
const std::byte* ImageView::channel_data(std::size_t x, std::size_t y,
                                         std::size_t channel) const {
  if (x >= width_ || y >= height_ || channel >= channels_) {
    throw std::out_of_range("ImageView coordinate is outside its extent.");
  }
  std::vector<std::size_t> coordinates(descriptor().shape.size(), 0U);
  coordinates[image_facet_.x_axis] = x;
  coordinates[image_facet_.y_axis] = y;
  if (image_facet_.channel_axis.has_value()) {
    coordinates[*image_facet_.channel_axis] = channel;
  }
  return tensor_.element_data(coordinates);
}

}  // namespace ps
