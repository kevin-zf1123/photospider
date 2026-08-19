#pragma once

#include <optional>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/region.hpp"

namespace ps::value_region {

/**
 * @brief Derives exact full-validity Region metadata for one complete output.
 *
 * @param output Complete output about to enter the formal HP cache.
 * @return Exact data-window ImageRect for an image Value, full TensorSlice for
 *         a non-image DenseTensor Value, or Whole for a data-only output
 *         without finite logical dimensions.
 * @throws std::logic_error for an invalid Value accessor.
 * @throws std::invalid_argument when retained image/tensor facts violate their
 *         declared contracts.
 * @throws std::overflow_error when a logical extent exceeds Region bounds.
 * @throws std::bad_alloc when TensorSlice or Region storage cannot allocate.
 * @note This function derives validity metadata only. Display windows never
 * authorize payload validity. The function preserves allocation identity,
 * Value revision, bytes, and ownership.
 */
RegionSet full_node_output_region(const NodeOutput& output);

/**
 * @brief Tests whether exact Region metadata covers one complete output.
 *
 * @param output Formal HP output whose finite logical bounds are derived.
 * @param region Exact normalized validity claimed for `output`.
 * @return True when `region` contains the complete image Region or the
 * complete rank-general TensorSlice for a sealed Value.
 * @throws std::logic_error for an invalid Value accessor.
 * @throws std::invalid_argument when retained image/tensor facts violate their
 *         declared contracts.
 * @throws std::overflow_error when a logical extent exceeds Region bounds.
 * @throws std::bad_alloc when full TensorSlice storage cannot allocate.
 * @note Image-faceted Values accept either their complete ImageRect or their
 *       complete TensorSlice as proof. Partial validity remains useful for
 *       Region-aware dirty staging but must not authorize whole-output cache
 *       reuse or persistence.
 */
bool node_output_region_is_complete(const NodeOutput& output,
                                    const RegionSet& region);

/**
 * @brief Merges one exact dense update into prior output bytes by Region.
 *
 * The result takes selected logical elements from `update`, unselected
 * elements from `existing`, and publishes a fresh contiguous sealed Value.
 * Named data and other non-image metadata come from `update`.
 *
 * @param existing Prior staged output whose valid bytes must be retained.
 * @param update Fresh complete-shape dense result carrying changed bytes.
 * @param updated_region Exact normalized ImageRect or TensorSlice selection.
 * @return Fresh merged output, or nullopt when either dense payload is absent
 *         or their descriptors/ImageFacets differ.
 * @throws std::invalid_argument when updated_region is unsupported, has the
 *         wrong domain/rank, or exceeds the shared descriptor.
 * @throws std::overflow_error when contiguous output arithmetic cannot be
 *         represented.
 * @throws std::bad_alloc when complete descriptor/ImageFacet snapshots,
 *         coordinate or output storage, or immutable Value publication cannot
 *         allocate.
 * @note Empty returns `existing`; Whole returns `update`. The helper preserves
 * the immutable Value contract and introduces no secondary image surface.
 */
std::optional<NodeOutput> merge_node_output_region(
    const NodeOutput& existing, const NodeOutput& update,
    const RegionSet& updated_region);

}  // namespace ps::value_region
