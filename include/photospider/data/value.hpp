#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/memory/buffer_handle.hpp"
#include "photospider/memory/strided_layout.hpp"

/**
 * @file value.hpp
 * @brief Immutable CPU DenseTensor Value and checked tensor-view contracts.
 */

namespace ps {

/**
 * @brief Identifies the logical interpretation of one tensor element.
 *
 * @throws Nothing.
 * @note Element semantics are independent from physical bit width. V-2 does
 *       not yet model quantization, packing, byte order, or vector lanes.
 */
enum class ElementSemantics : std::uint32_t {
  /** @brief Unsigned integer element. */
  UnsignedInteger = 0U,
  /** @brief Signed integer element. */
  SignedInteger = 1U,
  /** @brief IEEE-style floating-point element. */
  FloatingPoint = 2U,
};

/**
 * @brief Describes the byte-addressed storage width of one tensor element.
 *
 * @throws Nothing for ordinary value operations.
 * @note V-2 accepts unsigned/signed 8- or 16-bit integers and 32- or 64-bit
 *       floating-point values. Packed sub-byte encodings remain unsupported.
 */
struct StorageEncoding {
  /** @brief Number of physical bits occupied by one logical element. */
  std::uint32_t bit_width = 0U;

  /**
   * @brief Compares the complete physical encoding.
   *
   * @param other Encoding to compare.
   * @return True when bit widths match.
   * @throws Nothing.
   * @note Logical element semantics are stored by DenseTensorDescriptor.
   */
  bool operator==(const StorageEncoding& other) const noexcept {
    return bit_width == other.bit_width;
  }
};

/**
 * @brief Concrete logical descriptor for one CPU DenseTensor.
 *
 * @throws std::bad_alloc when copying the shape vector allocates and fails.
 * @note Shape and logical element facts do not describe physical strides,
 *       storage ownership, image axes, device routing, or readiness.
 */
struct DenseTensorDescriptor {
  /** @brief Positive concrete extent for every logical axis. */
  std::vector<std::size_t> shape;

  /** @brief Logical interpretation of each stored element. */
  ElementSemantics element_semantics = ElementSemantics::UnsignedInteger;

  /** @brief Physical byte-addressed encoding of each stored element. */
  StorageEncoding storage_encoding;

  /**
   * @brief Compares all logical DenseTensor facts.
   *
   * @param other Descriptor to compare.
   * @return True when shape, semantics, and encoding all match.
   * @throws Nothing under vector equality for size_t elements.
   * @note Physical strides and bytes are intentionally excluded.
   */
  bool operator==(const DenseTensorDescriptor& other) const noexcept {
    return shape == other.shape &&
           element_semantics == other.element_semantics &&
           storage_encoding == other.storage_encoding;
  }
};

/**
 * @brief Maps explicit image coordinates onto distinct DenseTensor axes.
 *
 * @throws Nothing for ordinary value operations.
 * @note An absent channel axis means one channel. Axis names, color roles,
 *       alpha semantics, and packing are not inferred.
 */
struct ImageFacet {
  /** @brief Logical axis used as the image x coordinate. */
  std::size_t x_axis = 0U;

  /** @brief Logical axis used as the image y coordinate. */
  std::size_t y_axis = 0U;

  /** @brief Optional logical axis used as the channel coordinate. */
  std::optional<std::size_t> channel_axis;

  /**
   * @brief Compares every explicit image-axis assignment.
   *
   * @param other Facet to compare.
   * @return True when x, y, and optional channel axes match.
   * @throws Nothing.
   * @note Tensor shape and storage layout are intentionally excluded.
   */
  bool operator==(const ImageFacet& other) const noexcept {
    return x_axis == other.x_axis && y_axis == other.y_axis &&
           channel_axis == other.channel_axis;
  }
};

/**
 * @brief Returns the validated byte width of one DenseTensor element.
 *
 * @param descriptor Descriptor whose semantics and encoding are inspected.
 * @return One, two, four, or eight bytes for a supported V-2 element.
 * @throws std::invalid_argument for an unknown semantic category or an
 *         unsupported semantic/bit-width combination.
 * @note Shape, facet, strides, and storage are not inspected.
 */
std::size_t dense_tensor_element_bytes(const DenseTensorDescriptor& descriptor);

/**
 * @brief Opaque process-local identity of one immutable Value publication.
 *
 * @throws Nothing for default construction, copying, comparison, and
 * destruction.
 * @note A revision is distinct from allocation, descriptor, content, layout,
 * artifact, graph-version, scheduler, and persistent cache identities.
 */
class ValueRevisionId final {
 public:
  /**
   * @brief Creates an invalid revision sentinel.
   *
   * @throws Nothing.
   * @note Every successfully published Value has a nonzero revision.
   */
  constexpr ValueRevisionId() noexcept = default;

  /**
   * @brief Reports whether this token identifies a Value publication.
   *
   * @return True when the process-local token is nonzero.
   * @throws Nothing.
   */
  constexpr bool valid() const noexcept { return value_ != 0U; }

  /**
   * @brief Returns the opaque process-local token for diagnostics.
   *
   * @return Zero for an invalid sentinel; otherwise a process-local token.
   * @throws Nothing.
   * @note Callers must not serialize this token or use it as an artifact,
   * registry, task-graph, disk-cache, or cross-process identity.
   */
  constexpr std::uint64_t value() const noexcept { return value_; }

  /**
   * @brief Compares complete Value revision tokens.
   *
   * @param other Revision to compare.
   * @return True when both opaque process-local values match.
   * @throws Nothing.
   */
  constexpr bool operator==(const ValueRevisionId& other) const noexcept {
    return value_ == other.value_;
  }

  /**
   * @brief Compares Value revisions for inequality.
   *
   * @param other Revision to compare.
   * @return True when opaque process-local values differ.
   * @throws Nothing.
   */
  constexpr bool operator!=(const ValueRevisionId& other) const noexcept {
    return !(*this == other);
  }

 private:
  /**
   * @brief Creates one valid token from the Value revision source.
   *
   * @param value Nonzero process-local token.
   * @throws Nothing.
   * @note Only successful Value publication may mint this type.
   */
  explicit constexpr ValueRevisionId(std::uint64_t value) noexcept
      : value_(value) {}

  /** @brief Opaque nonzero token, or zero for the invalid sentinel. */
  std::uint64_t value_ = 0U;

  friend class Value;
  friend class ValueBuilder;
};

class Value;

/**
 * @brief Exclusive producer for one future immutable CPU DenseTensor Value.
 *
 * ValueBuilder validates a positive-stride exact producer envelope before
 * allocation, issues at most one active move-only WriteLease, and closes all
 * producer authority at seal. No BufferHandle escapes before seal.
 *
 * @throws Nothing for move construction, move assignment, and destruction.
 * @note The builder is externally serialized. It provides raw allocation-byte
 * access only; no writable logical tensor view or aliasing layout is exposed.
 */
class ValueBuilder final {
 public:
  /** @brief Copy construction is forbidden for exclusive producer authority. */
  ValueBuilder(const ValueBuilder&) = delete;

  /** @brief Copy assignment is forbidden for exclusive producer authority. */
  ValueBuilder& operator=(const ValueBuilder&) = delete;

  /**
   * @brief Transfers complete unsealed or sealed builder state.
   *
   * @param other Builder to consume.
   * @throws Nothing.
   * @note The source becomes an invalid moved-from builder.
   */
  ValueBuilder(ValueBuilder&& other) noexcept;

  /**
   * @brief Replaces this builder with transferred state.
   *
   * @param other Builder to consume.
   * @return This builder after transfer.
   * @throws Nothing.
   * @note Existing unsealed state is abandoned only after any lease has
   * already released its separately retained authority.
   */
  ValueBuilder& operator=(ValueBuilder&& other) noexcept;

  /**
   * @brief Destroys an unpublished allocation or released sealed state.
   *
   * @throws Nothing.
   * @note An active WriteLease independently retains both allocation and
   * authority until that lease is destroyed.
   */
  ~ValueBuilder() noexcept;

  /**
   * @brief Allocates one exact positive-stride CPU DenseTensor producer.
   *
   * Validation checks positive shape, supported element encoding, optional
   * distinct in-rank image axes, one positive stride per axis, checked
   * envelope arithmetic, zero layout byte offset, and exact storage size.
   *
   * @param descriptor Logical descriptor copied into private builder state.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Positive producer layout copied into private builder state.
   * @param storage_size Exact positive allocation byte length.
   * @return Move-only exclusive builder ready to issue one WriteLease.
   * @throws std::invalid_argument for malformed descriptor, facet, layout, or
   * storage-size mismatch.
   * @throws std::overflow_error when envelope or identity arithmetic overflows.
   * @throws std::bad_alloc when state or CPU allocation cannot be created.
   * @note No caller allocation is retained.
   */
  static ValueBuilder allocate_cpu_dense_tensor(
      DenseTensorDescriptor descriptor, std::optional<ImageFacet> image_facet,
      StridedLayout layout, std::size_t storage_size);

  /**
   * @brief Acquires the builder's sole active exclusive write lease.
   *
   * @return Move-only lease over the complete private allocation.
   * @throws std::logic_error when the builder is moved-from, sealed, or already
   * has an active lease.
   * @note Seal remains forbidden until the returned lease is destroyed.
   */
  WriteLease acquire_write();

  /**
   * @brief Closes producer authority and publishes one immutable Value.
   *
   * @return Valid Value with a fresh revision and the builder allocation.
   * @throws std::logic_error when the builder is moved-from, already sealed, or
   * still has an active WriteLease.
   * @throws std::overflow_error when Value revision identity is exhausted.
   * @throws std::bad_alloc when immutable publication state cannot allocate.
   * @note A successful seal is irreversible. All bytes, including padding,
   * become immutable.
   */
  Value seal();

  /**
   * @brief Reports whether this builder has successfully sealed.
   *
   * @return True after seal; false for unsealed or moved-from state.
   * @throws Nothing.
   */
  bool sealed() const noexcept;

 private:
  /** @brief Private descriptor, allocation, and producer-authority state. */
  struct Impl;

  /**
   * @brief Creates a builder from completely validated private state.
   *
   * @param impl Exclusive implementation state.
   * @throws Nothing.
   */
  explicit ValueBuilder(std::unique_ptr<Impl> impl) noexcept;

  /** @brief Exclusive builder state, or null after move. */
  std::unique_ptr<Impl> impl_;
};

/**
 * @brief Immutable owning handle for one validated CPU DenseTensor payload.
 *
 * Value uses a shared immutable PImpl. Copies share one descriptor, layout,
 * facet, BufferHandle, allocation identity, and Value revision. Readable
 * pointers are available only through retaining checked views and ReadLease.
 *
 * @throws Nothing for default, copy, move, assignment, and destruction.
 * @note V-3 implements CPU ownership and runtime identity only. Replicas,
 * device routing, readiness, transfers, and fences remain later contracts.
 */
class Value final {
 public:
  /**
   * @brief Creates an invalid empty handle.
   *
   * @throws Nothing.
   * @note An empty handle is a transport sentinel, not a valid DenseTensor.
   */
  Value() noexcept = default;

  /**
   * @brief Publishes one exclusively owned immutable CPU DenseTensor.
   *
   * Validation checks nonempty positive shape, supported element encoding,
   * optional distinct in-rank image axes, one positive stride per axis, zero
   * producer byte offset, checked envelope arithmetic, and exact storage size
   * before publication.
   *
   * @param descriptor Concrete logical tensor descriptor copied into isolated
   *        immutable state after validation.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Physical signed byte strides copied into isolated immutable
   *        state after validation.
   * @param storage Exclusively owned input bytes consumed as the source for an
   *        isolated immutable allocation.
   * @return Valid immutable Value whose shape, strides, and payload allocations
   *         are distinct from every caller-owned input allocation.
   * @throws std::invalid_argument for malformed descriptors, facets, layouts,
   *         or a storage-size mismatch.
   * @throws std::overflow_error when the required address envelope cannot be
   *         represented by std::size_t.
   * @throws std::bad_alloc when immutable state allocation fails.
   * @note Validation finishes before the immutable PImpl is published. The
   *       published state deep-copies shape, strides, and payload so pointers
   *       retained before an lvalue or rvalue call never alias the Value.
   */
  static Value from_cpu_dense_tensor(DenseTensorDescriptor descriptor,
                                     std::optional<ImageFacet> image_facet,
                                     StridedLayout layout,
                                     std::vector<std::byte> storage);

  /**
   * @brief Publishes an immutable logical view over a sealed CPU buffer.
   *
   * Validation checks the descriptor and facet, then computes the complete
   * lower/upper signed-stride envelope from `layout.byte_offset`. Positive,
   * zero, and negative read strides are accepted only when every addressed
   * element lies inside `buffer`.
   *
   * @param descriptor Concrete logical descriptor copied into immutable state.
   * @param image_facet Optional explicit image-axis mapping.
   * @param layout Signed byte strides and logical-origin byte offset.
   * @param buffer Sealed immutable range retained by the new Value.
   * @return Valid Value with a fresh revision and buffer allocation identity.
   * @throws std::invalid_argument for malformed descriptor, facet, layout rank,
   * or an invalid buffer.
   * @throws std::out_of_range when the signed layout envelope escapes buffer.
   * @throws std::overflow_error when envelope or revision arithmetic overflows.
   * @throws std::bad_alloc when immutable publication state cannot allocate.
   * @note Publishing another logical view over the same BufferHandle preserves
   * allocation identity but creates a distinct ValueRevisionId.
   */
  static Value from_cpu_dense_tensor(DenseTensorDescriptor descriptor,
                                     std::optional<ImageFacet> image_facet,
                                     StridedLayout layout, BufferHandle buffer);

  /**
   * @brief Reports whether this handle owns a published DenseTensor.
   *
   * @return True when immutable state is present.
   * @throws Nothing.
   */
  bool valid() const noexcept;

  /**
   * @brief Returns the immutable logical DenseTensor descriptor.
   *
   * @return Borrowed descriptor retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   * @note The reference remains valid while this Value or one of its copies
   *       retains the shared immutable state.
   */
  const DenseTensorDescriptor& dense_tensor_descriptor() const;

  /**
   * @brief Returns the optional explicit image-axis mapping.
   *
   * @return Borrowed optional facet retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   */
  const std::optional<ImageFacet>& image_facet() const;

  /**
   * @brief Returns the immutable physical byte strides.
   *
   * @return Borrowed validated layout retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   */
  const StridedLayout& strided_layout() const;

  /**
   * @brief Returns the exact owned byte-envelope size.
   *
   * @return Number of immutable bytes retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   */
  std::size_t storage_size() const;

  /**
   * @brief Returns the immutable checked CPU allocation range.
   *
   * @return Borrowed BufferHandle retained by this Value.
   * @throws std::logic_error when the handle is invalid.
   * @note Callers may copy, subrange, or acquire a ReadLease from the handle;
   * no raw pointer is exposed by Value.
   */
  const BufferHandle& buffer_handle() const;

  /**
   * @brief Returns this Value's physical allocation identity.
   *
   * @return Nonzero process-local identity shared by allocation aliases.
   * @throws std::logic_error when the handle is invalid.
   * @note This identity is neither a Value revision nor a persistent cache key.
   */
  AllocationIdentity allocation_identity() const;

  /**
   * @brief Returns this immutable publication's Value revision.
   *
   * @return Nonzero process-local revision shared by Value copies.
   * @throws std::logic_error when the handle is invalid.
   * @note Publishing a new view over the same BufferHandle mints a new
   * revision.
   */
  ValueRevisionId revision_id() const;

 private:
  /** @brief Immutable implementation containing descriptor, layout, and bytes.
   */
  struct Impl;

  /**
   * @brief Creates a handle from already validated immutable state.
   *
   * @param impl Shared state published by from_cpu_dense_tensor.
   * @throws Nothing.
   * @note The constructor is private so unvalidated state cannot be published.
   */
  explicit Value(std::shared_ptr<const Impl> impl) noexcept;

  /** @brief Shared immutable DenseTensor state, or null for an invalid handle.
   */
  std::shared_ptr<const Impl> impl_;

  friend class ValueBuilder;
};

/**
 * @brief Retaining read-only, bounds-checked view of one DenseTensor Value.
 *
 * @throws std::invalid_argument when construction receives an invalid Value.
 * @note Copy and copy-like move operations are noexcept. The view stores a
 *       complete Value, so addresses outlive a caller's separate handle but
 *       never outlive the view.
 */
class DenseTensorView final {
 public:
  /**
   * @brief Retains a valid CPU DenseTensor Value.
   *
   * @param value Value copied or moved into the view.
   * @throws std::invalid_argument when value is invalid.
   * @note Construction acquires one ReadLease retained with the Value.
   */
  explicit DenseTensorView(Value value);

  /**
   * @brief Copies a view that retains the same immutable Value.
   *
   * @param other Valid view whose retained Value is shared.
   * @throws Nothing.
   * @note Both views remain complete, valid, and independently assignable.
   */
  DenseTensorView(const DenseTensorView& other) noexcept = default;

  /**
   * @brief Copy-assigns the immutable Value retained by another view.
   *
   * @param other Valid view whose retained Value replaces the current Value.
   * @return This complete retaining view.
   * @throws Nothing.
   * @note Both views remain complete and valid, including during
   *       self-assignment.
   */
  DenseTensorView& operator=(const DenseTensorView& other) noexcept = default;

  /**
   * @brief Move-constructs a view without invalidating the source view.
   *
   * @param other Valid view whose immutable retained Value is shared.
   * @throws Nothing.
   * @note Move is intentionally copy-like because DenseTensorView has no public
   *       invalid state. Source and destination both remain fully readable.
   */
  DenseTensorView(DenseTensorView&& other) noexcept : DenseTensorView(other) {}

  /**
   * @brief Move-assigns a view without invalidating the source view.
   *
   * @param other Valid view whose immutable retained Value replaces the current
   *        Value.
   * @return This complete retaining view.
   * @throws Nothing.
   * @note Move assignment intentionally delegates to copy assignment. Source
   *       and destination both remain fully readable, including for self-move.
   */
  DenseTensorView& operator=(DenseTensorView&& other) noexcept {
    return *this = other;
  }

  /**
   * @brief Returns the retained immutable Value.
   *
   * @return Borrowed Value handle.
   * @throws Nothing.
   */
  const Value& value() const noexcept;

  /**
   * @brief Returns the retained logical descriptor.
   *
   * @return Borrowed validated DenseTensor descriptor.
   * @throws Nothing after successful view construction.
   */
  const DenseTensorDescriptor& descriptor() const noexcept;

  /**
   * @brief Returns the retained physical layout.
   *
   * @return Borrowed validated byte strides.
   * @throws Nothing after successful view construction.
   */
  const StridedLayout& layout() const noexcept;

  /**
   * @brief Returns the exact immutable storage-envelope size.
   *
   * @return Number of retained payload bytes.
   * @throws Nothing after successful view construction.
   */
  std::size_t storage_size() const noexcept;

  /**
   * @brief Returns the immutable logical coordinate-zero address.
   *
   * @return Read-only pointer at `layout().byte_offset` within the lease.
   * @throws Nothing after successful view construction.
   * @note The pointer remains valid only while this view or a copy retains the
   * ReadLease.
   */
  const std::byte* data() const noexcept;

  /**
   * @brief Returns one logical element address after complete bounds checks.
   *
   * @param coordinates One coordinate for every logical axis.
   * @return Read-only pointer to the requested element's first byte.
   * @throws std::invalid_argument when coordinate rank differs from tensor
   *         rank.
   * @throws std::out_of_range when any coordinate is outside its extent.
   * @note Successful Value validation proves the computed address remains
   *       inside the retained byte envelope.
   */
  const std::byte* element_data(
      const std::vector<std::size_t>& coordinates) const;

 private:
  /** @brief Complete retained Value establishing descriptor and byte lifetime.
   */
  Value value_;

  /**
   * @brief Retaining read lease establishing every exposed pointer lifetime.
   *
   * @note During assignment the old lease retains its allocation while Value
   * state changes; during destruction the lease releases before the Value.
   */
  ReadLease read_lease_;
};

}  // namespace ps
