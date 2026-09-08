#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/status.hpp"
#include "photospider/data/region.hpp"
#include "photospider/data/storage.hpp"

namespace ps {

/**
 * @brief Closed built-in scalar element vocabulary.
 *
 * @note Provider-defined semantic facets may refine meaning without changing
 * the physical scalar width.
 */
enum class ElementType : std::uint32_t {
  UInt8 = 1,
  Int64 = 2,
  Float64 = 3,
  /** @brief IEEE-754 binary32; generic Values preserve all bit patterns. */
  Float32 = 4,
};

/**
 * @brief Logical type and full shape of one runtime Value.
 *
 * @note Shape rank is 1..8 and every extent is nonzero.
 */
struct PHOTOSPIDER_API ValueDescriptor final {
  /** @brief Built-in physical scalar representation. */
  ElementType element_type = ElementType::UInt8;
  /** @brief Nonzero logical extents in axis order. */
  std::vector<std::uint64_t> shape;
};

/**
 * @brief Origin-relative byte layout for one regional runtime Value.
 *
 * @note Signed strides permit reversed/broadcast views when bounds validation
 * proves every addressed element remains inside the retained buffer. A
 * zero-stride axis contributes no address span regardless of logical extent;
 * a singleton axis likewise does not use its stride value.
 */
struct PHOTOSPIDER_API StridedLayout final {
  /** @brief Creates an empty layout, invalid until strides are supplied. */
  StridedLayout() = default;
  /** @brief Creates an origin-relative view; empty origin denotes zero. */
  StridedLayout(std::uint64_t offset, std::vector<std::int64_t> strides,
                std::vector<std::uint64_t> logical_origin = {})
      : byte_offset(offset),
        byte_strides(std::move(strides)),
        origin(std::move(logical_origin)) {}
  /** @brief Byte offset of origin from buffer start. */
  std::uint64_t byte_offset = 0;
  /** @brief Signed byte stride for each descriptor axis. */
  std::vector<std::int64_t> byte_strides;
  /** @brief Logical coordinate at byte_offset; empty means all-zero origin. */
  std::vector<std::uint64_t> origin;
};

/**
 * @brief One bounded versioned semantic refinement attached to a Value.
 *
 * @note Facets are opaque to the core byte layout. Keys are printable ASCII
 * identifiers; payload interpretation belongs to operation/data definitions.
 */
struct PHOTOSPIDER_API ValueFacet final {
  /** @brief Nonempty unique printable-ASCII semantic key. */
  std::string key;
  /** @brief Positive schema version interpreted by the key owner. */
  std::uint32_t version = 1U;
  /** @brief Bounded immutable opaque semantic payload. */
  std::vector<std::uint8_t> payload;
};

/**
 * @brief Immutable validated regional runtime Value with bounded facets.
 *
 * @note Copies share immutable bytes and copied facet records. Logical
 * identity is independent from allocation address, residency, and optional
 * content digests.
 */
class PHOTOSPIDER_API Value final {
 public:
  /**
   * @brief Constructs an empty invalid/default Value.
   * @throws Nothing.
   * @note Default Values are useful only as container placeholders.
   */
  Value() noexcept = default;

  /**
   * @brief Validates and publishes an immutable Value.
   * @param descriptor Logical scalar type and shape.
   * @param region Logical valid coverage contained by descriptor shape.
   * @param layout Explicit byte layout.
   * @param bytes Owned allocation bytes copied into immutable shared storage.
   * @param facets Bounded unique semantic refinements copied into the Value.
   * @return Complete Value or a typed validation failure.
   * @throws std::bad_alloc If owned storage cannot be allocated.
   * @note Publication is atomic; failure retains no partial Value. Layout
   * span validation ignores zero-stride and singleton axes before converting
   * logical extents into signed address arithmetic.
   */
  [[nodiscard]] static Result<Value> create(
      ValueDescriptor descriptor, Region region, StridedLayout layout,
      std::vector<std::uint8_t> bytes, std::vector<ValueFacet> facets = {});

  /**
   * @brief Creates one rank-one Float64 scalar Value.
   * @param value Finite or non-finite binary64 payload preserved exactly.
   * @return Complete scalar Value.
   * @throws std::bad_alloc If storage allocation fails.
   * @note The descriptor shape is `{1}` and Region is whole.
   */
  [[nodiscard]] static Value from_float64(double value);

  /**
   * @brief Reads a rank-one Float64 scalar.
   * @return Scalar or `TypeMismatch` for another descriptor/layout/storage or
   * any Region other than exact scalar coverage `{offset=0, extent=1}`.
   * @throws std::bad_alloc If a diagnostic allocation fails.
   * @note The method copies bytes and never exposes writable storage. Empty,
   * partial, and offset Regions remain valid general Value coverage but cannot
   * be read through this scalar accessor.
   */
  [[nodiscard]] Result<double> as_float64() const;

  /**
   * @brief Returns whether this object contains a published Value.
   * @return True when immutable storage is present.
   * @throws Nothing.
   * @note A valid Value may contain zero bytes only when validation permits it;
   * empty coverage may own an empty caller allocation.
   */
  [[nodiscard]] bool valid() const noexcept { return storage_ != nullptr; }

  /**
   * @brief Returns the immutable descriptor.
   * @return Descriptor reference.
   * @throws std::logic_error If this Value is invalid/default.
   * @note Reference lifetime is bounded by this Value.
   */
  [[nodiscard]] const ValueDescriptor& descriptor() const;

  /**
   * @brief Returns logical valid coverage.
   * @return Region reference.
   * @throws std::logic_error If this Value is invalid/default.
   * @note Region coordinates are not byte offsets.
   */
  [[nodiscard]] const Region& region() const;

  /**
   * @brief Returns the immutable byte layout.
   * @return Layout reference.
   * @throws std::logic_error If this Value is invalid/default.
   * @note The layout was range-checked against `bytes()` at construction.
   */
  [[nodiscard]] const StridedLayout& layout() const;

  /**
   * @brief Returns bounded immutable semantic facets in canonical key order.
   * @return Facet vector reference.
   * @throws std::logic_error If this Value is invalid/default.
   * @note Keys are unique; callers must interpret payloads by key/version.
   */
  [[nodiscard]] const std::vector<ValueFacet>& facets() const;

  /**
   * @brief Returns immutable storage bytes.
   * @return Borrowed read-only byte view.
   * @throws std::logic_error If this Value is invalid/default.
   * @note Callers cannot mutate the retained allocation through this API.
   */
  [[nodiscard]] ByteView bytes() const;

  /** @brief Copies allocation bytes into caller-owned memory; may throw
   * bad_alloc. */
  [[nodiscard]] std::vector<std::uint8_t> copy_bytes() const;
  /** @brief Returns shared immutable storage, valid independently of this
   * Value. */
  [[nodiscard]] const std::shared_ptr<const CpuStorage>& storage() const;
  /**
   * @brief Validates a regional view and retains immutable storage without
   * copying.
   * @param descriptor Complete logical descriptor.
   * @param region Valid nonempty or empty logical coverage inside descriptor.
   * @param layout Origin-relative byte layout of that coverage.
   * @param storage Immutable owner; null is InvalidArgument.
   * @param facets Semantic metadata, canonicalized before publication.
   * @return Validated Value or bounds/type/facet failure; no partial
   * publication.
   * @throws std::bad_alloc For metadata allocation failure.
   */
  [[nodiscard]] static Result<Value> from_storage(
      ValueDescriptor descriptor, Region region, StridedLayout layout,
      std::shared_ptr<const CpuStorage> storage,
      std::vector<ValueFacet> facets = {});
  /** @brief Creates a shared read-only subview; rejects coverage outside this
   * Value. */
  [[nodiscard]] Result<Value> view(const Region& region) const;
  /**
   * @brief Returns an element byte offset for a logical coordinate in coverage.
   * @return Checked offset or InvalidArgument for a coordinate outside
   * coverage.
   * @note The offset addresses bytes(); no mutable pointer or storage copy.
   */
  [[nodiscard]] Result<std::size_t> byte_address(
      const std::vector<std::uint64_t>& coordinate) const;

  /**
   * @brief Returns the physical scalar width.
   * @param type Closed built-in element type.
   * @return Width in bytes.
   * @throws std::invalid_argument For an unknown enum representation.
   * @note This function performs no allocation.
   */
  [[nodiscard]] static std::size_t element_size(ElementType type);

 private:
  /** @brief Published logical descriptor. */
  ValueDescriptor descriptor_;
  /** @brief Published logical valid coverage. */
  Region region_;
  /** @brief Published byte layout. */
  StridedLayout layout_;
  /** @brief Published facets sorted by key for deterministic observation. */
  std::vector<ValueFacet> facets_;
  /** @brief Shared immutable CPU owner; null marks the default state. */
  std::shared_ptr<const CpuStorage> storage_;
};

/**
 * @brief Borrowed logical Value view with no allocation ownership.
 * @note The referenced Value must outlive this view. Stream sinks may use it
 * only until callback return; retaining this view or its pointers is invalid.
 */
class PHOTOSPIDER_API ValueView final {
 public:
  explicit ValueView(const Value& value) : value_(&value) {}
  const ValueDescriptor& descriptor() const { return value_->descriptor(); }
  const Region& region() const { return value_->region(); }
  const StridedLayout& layout() const { return value_->layout(); }
  const std::vector<ValueFacet>& facets() const { return value_->facets(); }
  ByteView bytes() const { return value_->bytes(); }
  Result<std::size_t> byte_address(
      const std::vector<std::uint64_t>& coordinate) const {
    return value_->byte_address(coordinate);
  }

 private:
  const Value* value_;
};

/**
 * @brief Move-only tightly packed writable region allocated by the host.
 * @note Valid until publish/destruction; callbacks must not retain data().
 */
class PHOTOSPIDER_API MutableValue final {
 public:
  MutableValue() = default;
  MutableValue(MutableValue&&) noexcept = default;
  MutableValue& operator=(MutableValue&&) noexcept = default;
  MutableValue(const MutableValue&) = delete;
  MutableValue& operator=(const MutableValue&) = delete;
  /** @brief Allocates nonempty regional coverage, checking all byte products.
   */
  static Result<MutableValue> allocate(const ValueDescriptor& descriptor,
                                       const Region& region,
                                       const BufferAllocator& allocator);
  /** @brief Borrowed writable region bytes; invalidated by publication. */
  std::uint8_t* data() noexcept { return buffer_.data(); }
  /** @brief Exact regional allocation size. */
  std::size_t size() const noexcept { return buffer_.size(); }
  /** @brief Regional origin and row-major strides, borrowed until destruction.
   */
  const StridedLayout& layout() const noexcept { return layout_; }
  /** @brief Freezes this writer and validates facets; consumes even on failure.
   */
  Result<Value> publish(std::vector<ValueFacet> facets = {}) &&;

 private:
  ValueDescriptor descriptor_;
  Region region_;
  StridedLayout layout_;
  MutableBuffer buffer_;
};

}  // namespace ps
