#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "photospider/data/value.hpp"

namespace ps {

/** @brief Closed semantic kinds; generic Values need no semantic descriptor. */
enum class SemanticKind : std::uint32_t {
  Scalar = 1,
  Image = 2,
  Mask = 3,
  ScalarField = 4,
  VectorField = 5,
  ComplexField = 6,
  SampledSignal = 7,
  Lut = 8,
  ByteResource = 9,
};

/** @brief Owned channel interpretation, independent of storage dtype. */
struct PHOTOSPIDER_API SemanticChannel final {
  std::string name;
  std::string role;
  std::string unit;
};

/**
 * @brief Owned typed interpretation encoded by the canonical facet helpers.
 * @note Text fields are strict UTF-8, at most 128 bytes. Unused fields are
 * empty or positive zero. Metadata is bounded to 4096 encoded bytes. Image
 * samples are Float32 HWC; generic Value floating bits remain unrestricted.
 * Immutable descriptors may be shared across threads; callers synchronize
 * mutation.
 */
struct PHOTOSPIDER_API SemanticDescriptor final {
  SemanticKind kind = SemanticKind::Scalar;
  std::vector<SemanticChannel> channels;
  std::string model;
  std::string primaries;
  std::array<double, 3> white = {};
  std::string transfer;
  std::string reference;
  std::string unit = "dimensionless";
  std::string association;
  /** @brief Vector: pixel/normalized displacement/position. Complex:
   * frequency_unshifted, with full spectrum and DC at index zero.
   */
  std::string coordinate_space;
  /** @brief Vector: forward/inverse. Complex: forward_negative_inverse_1n,
   * denoting unnormalized negative-sign forward transform and inverse /N.
   */
  std::string direction;
  double sample_origin = 0;
  double sample_step = 0;
  std::string sample_axis_unit;
  std::string media_type;
};

/** @brief Creates the linear-sRGB D65 relative RGBA coverage descriptor.
 * @return An owned descriptor with signed/HDR finite RGB and bounded alpha.
 * @throws std::bad_alloc On metadata allocation.
 */
PHOTOSPIDER_API SemanticDescriptor rgba_semantics();
/** @brief Creates a dimensionless HW coverage-mask descriptor.
 * @return An owned descriptor for finite Float32 samples in [0,1].
 * @throws std::bad_alloc On metadata allocation.
 */
PHOTOSPIDER_API SemanticDescriptor coverage_semantics();

/** @brief Validates and encodes a typed descriptor as image-v2 or semantic-v1.
 * @param semantic Owned caller descriptor; not retained or modified.
 * @return Canonical facet, or InvalidArgument for malformed/oversized metadata.
 * @throws std::bad_alloc On metadata allocation. No sample buffer is allocated.
 * @note Pure and thread-safe. Canonicalization normalizes metadata zero only.
 */
PHOTOSPIDER_API Result<ValueFacet> encode_semantic(
    const SemanticDescriptor& semantic);
/** @brief Decodes a current canonical typed facet without compatibility
 * readers.
 * @param facet Borrowed facet; its bytes are copied into the returned value.
 * @return Owned descriptor or InvalidArgument for malformed/old/noncanonical
 * data.
 * @throws std::bad_alloc On metadata allocation. Pure and thread-safe.
 */
PHOTOSPIDER_API Result<SemanticDescriptor> decode_semantic(
    const ValueFacet& facet);
/** @brief Converts typed metadata to a static semantic String parameter.
 * @param semantic Descriptor to encode; not retained.
 * @return Lowercase hexadecimal, at most 8192 bytes, or InvalidArgument.
 * @throws std::bad_alloc On allocation. Pure and thread-safe; no manual hex
 * needed.
 */
PHOTOSPIDER_API Result<std::string> semantic_parameter(
    const SemanticDescriptor& semantic);
/** @brief Parses the canonical static semantic parameter emitted by the helper.
 * @param parameter Borrowed lowercase hexadecimal text, at most 8192 bytes.
 * @return Owned typed descriptor or InvalidArgument; no coercion or defaulting.
 * @throws std::bad_alloc On allocation. Pure and thread-safe.
 */
PHOTOSPIDER_API Result<SemanticDescriptor> semantic_from_parameter(
    const std::string& parameter);
/** @brief Checks a typed description against dtype and nonzero logical shape.
 * @param semantic Borrowed descriptor.
 * @param descriptor Borrowed Value descriptor.
 * @return InvalidArgument for malformed semantics; TypeMismatch for
 * shape/dtype.
 * @throws std::bad_alloc On metadata allocation. Pure and thread-safe.
 */
PHOTOSPIDER_API Status validate_semantic_descriptor(
    const SemanticDescriptor& semantic, const ValueDescriptor& descriptor);
/** @brief Validates only the present regional samples of a typed Value.
 * @param semantic Validated interpretation; not retained.
 * @param value Immutable Value, which must match the semantic descriptor.
 * @param numeric_failure Failure category for invalid sample values.
 * @param stop Optional periodic cancellation/currentness probe, never retained.
 * @return Metadata error, numeric_failure, or the probe's stop code.
 * @throws std::bad_alloc On metadata/address allocation.
 * @note Thread-safe for immutable inputs. No conversion, clamp or epsilon.
 */
PHOTOSPIDER_API Status
validate_semantic_value(const SemanticDescriptor& semantic, const Value& value,
                        ErrorCode numeric_failure = ErrorCode::InvalidArgument,
                        const std::function<ErrorCode()>& stop = {});

}  // namespace ps
