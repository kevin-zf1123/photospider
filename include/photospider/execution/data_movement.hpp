#pragma once

#include <cstdint>
#include <functional>

#include "photospider/data/dependency.hpp"
#include "photospider/data/planar_image.hpp"

namespace ps {

/** @brief Closed, compiler-visible VALUE relation, independent of read needs.
 * BitwiseMapped v1 declares that every output sample copies the exact bits at
 * its static dependency mapping (or broadcasts a fixed rank-one scalar).
 * It is not inferred from an operation name or from identity dependencies.
 * The host may bypass the sample callback, but never static validation.
 */
enum class DataMovementKind : std::uint8_t { None = 0, BitwiseMapped = 1 };
/** @brief Explicit physical result policy, independent of parameter spellings.
 */
enum class DataMovementViewPolicy : std::uint8_t {
  Auto = 0,
  RequireView = 1,
  Materialize = 2
};

/** @brief Copy/fill an affine spatial piece into an unpublished planar window.
 * Exactly one of image/value must be supplied. The mapped input must cover the
 * entire source rectangle; dtype, output axes and sample width are checked.
 * Spatial axes map one-to-one with translations; scalar {1} is broadcast.
 * Negative/zero generic strides are supported. No padding is read/written.
 * Source and destination windows may have different tile geometries.
 * A generic Value's immutable backing must not alias the unpublished writer;
 * this trusted caller precondition is not checked for external raw aliases.
 * No owner is allocated or committed here. Existing published planar samples
 * are immutable and cannot overlap an authorized writer. Raw run pointers are
 * borrowed for the duration of this call. Cancellation/currentness is checked
 * at most every 1024 samples; caller owns aggregate work/page admission.
 * Failure may leave an UNPUBLISHED prefix. Allocations may throw bad_alloc.
 */
PHOTOSPIDER_API Status copy_planar_region(
    const Region& region, const DependencyMappedNeed& mapping,
    const PlanarImageReadWindow* image, const Value* value,
    const PlanarImageWriteWindow& output, const PlanarImageLayout& layout,
    std::uint64_t sample_bytes, const CancellationToken& cancellation = {},
    const std::function<bool()>& current = {});

}  // namespace ps
