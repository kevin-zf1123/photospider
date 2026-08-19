#pragma once

#include <cstdint>

#include "photospider/data/image_metadata.hpp"
#include "photospider/data/value.hpp"

/**
 * @file sample_conversion.hpp
 * @brief Explicit deterministic ordinary-image sample conversion contracts.
 */

namespace ps {

/** @brief Policy for a finite source sample outside its declared domain. */
enum class OutOfDomainPolicy : std::uint32_t {
  /** @brief Reject the complete conversion before publication. */
  Reject = 0U,
  /** @brief Clamp to the nearest declared source-domain endpoint. */
  Clamp = 1U,
};

/** @brief Deterministic rounding selected for integral destination storage. */
enum class SampleRoundingMode : std::uint32_t {
  /** @brief Round to nearest with exact halfway cases to an even integer. */
  NearestEven = 0U,
  /** @brief Truncate toward zero. */
  TowardZero = 1U,
  /** @brief Round toward negative infinity. */
  Floor = 2U,
  /** @brief Round toward positive infinity. */
  Ceil = 3U,
};

/** @brief Policy for NaN and infinity at a conversion boundary. */
enum class NonFinitePolicy : std::uint32_t {
  /** @brief Reject every non-finite source value. */
  Reject = 0U,
  /** @brief Preserve exactly when destination storage supports the value. */
  Preserve = 1U,
};

/** @brief Policy for destination quantization or narrowing loss. */
enum class PrecisionLossPolicy : std::uint32_t {
  /** @brief Reject any value not exactly representable after conversion. */
  Reject = 0U,
  /** @brief Permit the explicitly selected rounding or floating narrowing. */
  Allow = 1U,
};

/**
 * @brief Complete explicit sample meaning at one conversion endpoint.
 * @throws Nothing for ordinary value operations.
 * @note Storage element semantics remain an independent field of the request.
 */
struct SampleEndpoint final {
  /** @brief Versioned storage-independent encoding classification. */
  SampleEncoding encoding;
  /** @brief Finite inclusive numeric interval. */
  SampleDomain domain;
};

/**
 * @brief Complete deterministic conversion request for an ordinary image.
 * @throws Nothing for ordinary value operations.
 * @note The source endpoint must exactly match the Value's declared default
 *       sample facts. Per-channel overrides require separate future policy.
 */
struct SampleConversion final {
  /** @brief Exact declared source interpretation. */
  SampleEndpoint source;
  /** @brief Exact declared destination interpretation. */
  SampleEndpoint destination;
  /** @brief Logical numeric semantics of destination storage. */
  ElementSemantics destination_element_semantics =
      ElementSemantics::UnsignedInteger;
  /** @brief Physical destination scalar encoding. */
  StorageEncoding destination_storage_encoding{8U};
  /** @brief Source-domain handling policy. */
  OutOfDomainPolicy out_of_domain = OutOfDomainPolicy::Reject;
  /** @brief Deterministic integral rounding policy. */
  SampleRoundingMode rounding = SampleRoundingMode::NearestEven;
  /** @brief Exceptional-value handling policy. */
  NonFinitePolicy non_finite = NonFinitePolicy::Reject;
  /** @brief Numeric narrowing/quantization loss policy. */
  PrecisionLossPolicy precision_loss = PrecisionLossPolicy::Reject;
};

/**
 * @brief Converts one Ready host-readable Strided ordinary DenseImage Value.
 *
 * The operation validates complete endpoint facts, performs the exact affine
 * domain mapping, applies the selected exceptional/out-of-domain/rounding/loss
 * policies, and publishes one fresh interleaved CPU Value only after every
 * sample succeeds.
 *
 * @param source Valid built-in Strided Value with complete ImageFacet and one
 *        default sample-domain record without per-channel overrides.
 * @param conversion Complete explicit source and destination policy.
 * @return Fresh Ready CPU Value preserving shape, axes, signed data/display
 *         windows, channel/group identities, and color while replacing sample
 *         meaning and storage encoding exactly as requested.
 * @throws std::invalid_argument for malformed/mismatched endpoint facts,
 *         unsupported storage, provider/Blocked input, or policy enum values.
 * @throws std::domain_error for forbidden out-of-domain, non-finite, overflow,
 *         or precision-loss samples.
 * @throws ReadyFenceAccessError or BufferAccessError when source bytes are not
 *         synchronously readable.
 * @throws std::overflow_error when layout/storage arithmetic is
 * unrepresentable.
 * @throws std::bad_alloc when output metadata or storage cannot allocate.
 * @note Failure publishes no partial Value and performs no implicit clamp,
 *       normalization, transfer function, channel-role, or file-type inference.
 */
Value convert_dense_image_samples(const Value& source,
                                  const SampleConversion& conversion);

}  // namespace ps
