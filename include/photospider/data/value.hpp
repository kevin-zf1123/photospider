#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "photospider/core/status.hpp"
#include "photospider/data/region.hpp"

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
};

/**
 * @brief Logical type and shape of one dense runtime Value.
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
 * @brief Explicit byte layout for one dense runtime Value.
 *
 * @note Signed strides permit reversed/broadcast views when bounds validation
 * proves every addressed element remains inside the retained buffer.
 */
struct PHOTOSPIDER_API StridedLayout final {
  /** @brief Byte offset of logical coordinate zero from buffer start. */
  std::uint64_t byte_offset = 0;
  /** @brief Signed byte stride for each descriptor axis. */
  std::vector<std::int64_t> byte_strides;
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
 * @brief Immutable validated dense runtime Value with bounded facets.
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
   * @note Publication is atomic; failure retains no partial Value.
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
   * @return True when immutable byte storage is present.
   * @throws Nothing.
   * @note A valid Value may contain zero bytes only when validation permits it;
   * current dense descriptors always require at least one element.
   */
  [[nodiscard]] bool valid() const noexcept { return bytes_ != nullptr; }

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
   * @return Shared byte vector reference.
   * @throws std::logic_error If this Value is invalid/default.
   * @note Callers cannot mutate the retained allocation through this API.
   */
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const;

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
  /** @brief Shared immutable allocation; null marks the default state. */
  std::shared_ptr<const std::vector<std::uint8_t>> bytes_;
};

}  // namespace ps
