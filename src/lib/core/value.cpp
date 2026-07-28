#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

/**
 * @brief Owns both process-wide runtime identity sequences.
 *
 * This authority is instantiated exactly once inside the shared
 * `Photospider::operation_runtime`. The static Host product and every
 * Value-using operation DSO dynamically depend on that same runtime image, so
 * allocation and revision tokens cannot restart at a DSO boundary.
 *
 * @throws Nothing during construction and destruction.
 * @note The two sequences remain distinct identity domains. The shared-runtime
 *       ownership, rather than symbol interposition or a probabilistic token,
 *       establishes their process-wide uniqueness.
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
 * @return Validated element byte width.
 * @throws std::invalid_argument for empty/zero shape, unsupported elements, or
 * malformed image axes.
 * @note Layout and allocation ranges are validated separately.
 */
std::size_t validate_descriptor_and_facet(
    const DenseTensorDescriptor& descriptor,
    const std::optional<ImageFacet>& facet) {
  if (descriptor.shape.empty()) {
    throw std::invalid_argument("DenseTensor rank must be positive.");
  }
  for (const std::size_t extent : descriptor.shape) {
    if (extent == 0U) {
      throw std::invalid_argument("DenseTensor extents must all be positive.");
    }
  }

  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  if (!facet.has_value()) {
    return element_bytes;
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
  return element_bytes;
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

/**
 * @brief Private CPU allocation control block retained by BufferHandle copies.
 *
 * @throws std::bad_alloc when allocating CPU byte storage.
 * @note The immutable identity is minted before construction. Only an active
 * pre-seal WriteLease may mutate `storage`.
 */
struct BufferHandle::ControlBlock final {
  /** @brief Stable nonzero process-local physical allocation identity. */
  AllocationIdentity identity;

  /** @brief CPU allocation bytes, immutable after ValueBuilder seal. */
  std::vector<std::byte> storage;

  /**
   * @brief Allocates one zero-initialized CPU byte range.
   *
   * @param identity_in Already minted nonzero allocation identity.
   * @param size Positive allocation size.
   * @throws std::bad_alloc when byte allocation fails.
   */
  ControlBlock(AllocationIdentity identity_in, std::size_t size)
      : identity(identity_in), storage(size, std::byte{0}) {}
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
  return ReadLease(*this);
}

/** @copydoc BufferHandle::read_pointer */
const std::byte* BufferHandle::read_pointer() const noexcept {
  return control_->storage.data() + offset_;
}

/** @copydoc BufferHandle::write_pointer */
std::byte* BufferHandle::write_pointer() const noexcept {
  return control_->storage.data() + offset_;
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
 * @brief Immutable implementation for one validated CPU DenseTensor Value.
 *
 * @throws std::bad_alloc when copied descriptor/layout storage cannot allocate.
 * @note Instances are published only through shared_ptr<const Impl>. The
 * BufferHandle control block is already sealed before publication returns.
 */
struct Value::Impl final {
  /** @brief Validated concrete logical descriptor. */
  DenseTensorDescriptor descriptor;

  /** @brief Optional validated explicit image-axis mapping. */
  std::optional<ImageFacet> image_facet;

  /** @brief Validated signed physical layout and logical-origin offset. */
  StridedLayout layout;

  /** @brief Immutable checked CPU allocation range. */
  BufferHandle buffer;

  /** @brief Stable identity of this immutable logical publication. */
  ValueRevisionId revision;

  /**
   * @brief Stores one completely validated immutable publication.
   *
   * @param descriptor_in Logical descriptor to retain.
   * @param image_facet_in Optional image facet to retain.
   * @param layout_in Signed checked layout to retain.
   * @param buffer_in Sealed checked allocation range to retain.
   * @param revision_in Fresh nonzero publication revision.
   * @throws std::bad_alloc when descriptor or layout vector moves must
   * allocate.
   * @note Inputs are already validated and no producer authority remains.
   */
  Impl(DenseTensorDescriptor descriptor_in,
       std::optional<ImageFacet> image_facet_in, StridedLayout layout_in,
       BufferHandle buffer_in, ValueRevisionId revision_in)
      : descriptor(std::move(descriptor_in)),
        image_facet(std::move(image_facet_in)),
        layout(std::move(layout_in)),
        buffer(std::move(buffer_in)),
        revision(revision_in) {}
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

  /** @brief Validated positive exact producer layout. */
  StridedLayout layout;

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
   * @param layout_in Positive exact producer layout to own.
   * @param buffer_in Private complete allocation handle.
   * @param authority_in Fresh producer authority.
   * @throws Nothing under member move contracts.
   */
  Impl(DenseTensorDescriptor descriptor_in,
       std::optional<ImageFacet> image_facet_in, StridedLayout layout_in,
       BufferHandle buffer_in,
       std::shared_ptr<WriteLease::Authority> authority_in) noexcept
      : descriptor(std::move(descriptor_in)),
        image_facet(std::move(image_facet_in)),
        layout(std::move(layout_in)),
        buffer(std::move(buffer_in)),
        authority(std::move(authority_in)) {}
};

/** @copydoc ps::dense_tensor_element_bytes */
std::size_t dense_tensor_element_bytes(
    const DenseTensorDescriptor& descriptor) {
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
    case ElementSemantics::SignedInteger:
      if (descriptor.storage_encoding.bit_width == 8U ||
          descriptor.storage_encoding.bit_width == 16U) {
        return descriptor.storage_encoding.bit_width / 8U;
      }
      break;
    case ElementSemantics::FloatingPoint:
      if (descriptor.storage_encoding.bit_width == 32U ||
          descriptor.storage_encoding.bit_width == 64U) {
        return descriptor.storage_encoding.bit_width / 8U;
      }
      break;
  }
  throw std::invalid_argument(
      "DenseTensor element semantics and encoding are unsupported.");
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
  const std::size_t element_bytes =
      validate_descriptor_and_facet(descriptor, image_facet);
  validate_producer_envelope(descriptor, layout, element_bytes, storage_size);

  DenseTensorDescriptor isolated_descriptor = descriptor;
  const std::optional<ImageFacet> isolated_image_facet = image_facet;
  StridedLayout isolated_layout = layout;
  BufferHandle buffer = BufferHandle::allocate_for_builder(storage_size);
  auto authority = std::make_shared<WriteLease::Authority>();
  return ValueBuilder(std::make_unique<Impl>(
      std::move(isolated_descriptor), isolated_image_facet,
      std::move(isolated_layout), std::move(buffer), std::move(authority)));
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
  auto published = std::make_shared<const Value::Impl>(
      impl_->descriptor, impl_->image_facet, impl_->layout, impl_->buffer,
      revision);

  impl_->authority->builder_open.store(false, std::memory_order_release);
  impl_->sealed = true;
  return Value(std::move(published));
}

/** @copydoc ValueBuilder::sealed */
bool ValueBuilder::sealed() const noexcept {
  return impl_ != nullptr && impl_->sealed;
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
  DenseTensorDescriptor isolated_descriptor = descriptor;
  const std::optional<ImageFacet> isolated_image_facet = image_facet;
  StridedLayout isolated_layout = layout;
  return Value(std::make_shared<const Impl>(
      std::move(isolated_descriptor), isolated_image_facet,
      std::move(isolated_layout), std::move(buffer), revision));
}

/** @copydoc Value::valid */
bool Value::valid() const noexcept {
  return impl_ != nullptr;
}

/** @copydoc Value::dense_tensor_descriptor */
const DenseTensorDescriptor& Value::dense_tensor_descriptor() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no DenseTensor descriptor.");
  }
  return impl_->descriptor;
}

/** @copydoc Value::image_facet */
const std::optional<ImageFacet>& Value::image_facet() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no ImageFacet.");
  }
  return impl_->image_facet;
}

/** @copydoc Value::strided_layout */
const StridedLayout& Value::strided_layout() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no StridedLayout.");
  }
  return impl_->layout;
}

/** @copydoc Value::storage_size */
std::size_t Value::storage_size() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no storage.");
  }
  return impl_->buffer.size();
}

/** @copydoc Value::buffer_handle */
const BufferHandle& Value::buffer_handle() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no BufferHandle.");
  }
  return impl_->buffer;
}

/** @copydoc Value::allocation_identity */
AllocationIdentity Value::allocation_identity() const {
  return buffer_handle().allocation_identity();
}

/** @copydoc Value::revision_id */
ValueRevisionId Value::revision_id() const {
  if (!impl_) {
    throw std::logic_error("Invalid Value has no revision.");
  }
  return impl_->revision;
}

/** @copydoc DenseTensorView::DenseTensorView */
DenseTensorView::DenseTensorView(Value value) : value_(std::move(value)) {
  if (!value_.valid()) {
    throw std::invalid_argument(
        "DenseTensorView requires a valid DenseTensor Value.");
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
