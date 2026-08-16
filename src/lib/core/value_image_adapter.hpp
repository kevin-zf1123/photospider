#pragma once

#include <optional>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/region.hpp"

namespace ps::value_image_adapter {

/**
 * @brief Copies one current nonempty CPU ImageBuffer into a sealed image Value.
 *
 * @param buffer Valid current CPU image descriptor and payload to snapshot.
 * @return Value with shape `[height, width, channels]`, explicit y/x/channel
 * axes, an explicit `[0,width) x [0,height)` data window, the same positive
 * row stride, fresh allocation identity, and fresh Value revision.
 * @throws std::invalid_argument for malformed, empty, non-CPU, or
 * unrepresentable ImageBuffer state.
 * @throws std::overflow_error when envelope arithmetic cannot be represented.
 * @throws std::bad_alloc when snapshot or immutable publication allocation
 * fails.
 * @note Only active row bytes are copied. Inter-row padding inside the exact
 * Value envelope is initialized independently. ImageBuffer supplies no stable
 * channel/group, sample-domain, color, or display-window authority, so those
 * facts remain absent. No ImageBuffer owner, context, deleter, or
 * plugin-library lifetime is retained.
 */
Value snapshot_cpu_image_value(const ImageBuffer& buffer);

/**
 * @brief Validates that one Value can enter the current ImageBuffer boundary.
 *
 * @param value Candidate Ready host-readable image Value.
 * @throws std::invalid_argument for invalid, packed, quantized, latent,
 * positive-int-unrepresentable extents, or otherwise unsupported image facts.
 * @throws ReadyFenceAccessError when producer completion is not Ready.
 * @throws BufferAccessError when the retained binding is not host-readable.
 * @throws std::bad_alloc when retaining view state cannot allocate.
 * @note Success establishes Strided layout, absent quantization, explicit
 * ImageFacet, supported whole-byte element encoding, singleton unassigned
 * axes, positive-int-compatible extents, and direct host visibility.
 * Validation reads no payload byte, allocates no ImageBuffer, performs no
 * conversion, and invokes no codec or filesystem mechanism.
 */
void validate_image_buffer_compatible_value(const Value& value);

/**
 * @brief Copies one sealed image Value into a current CPU ImageBuffer snapshot.
 *
 * @param value Valid DenseTensor Value with an explicit ImageFacet.
 * @return Independently allocated aligned CPU ImageBuffer containing all active
 * logical image elements.
 * @throws std::invalid_argument for a non-image Value or an element encoding
 * unsupported by the current ImageBuffer contract.
 * @throws std::out_of_range when checked view coordinates are invalid.
 * @throws std::overflow_error when image-byte arithmetic is unrepresentable.
 * @throws std::bad_alloc when the output allocation or view coordinates cannot
 * allocate.
 * @note Arbitrary validated positive, zero, or negative source strides are read
 * through ImageView; source padding is never copied as active pixels. Because
 * ImageBuffer cannot represent signed origin or richer image interpretation,
 * the returned compatibility snapshot deliberately projects those facts away.
 */
ImageBuffer snapshot_cpu_image_buffer(const Value& value);

/**
 * @brief Ensures a formal CPU NodeOutput has sealed Value identity authority.
 *
 * @param output Mutable result about to cross an HP or disk-load publication
 * boundary.
 * @return Nothing.
 * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc from
 * CPU ImageBuffer snapshot and Value publication.
 * @note A valid `image_value` is preserved exactly. An invalid member is
 * populated only for a nonempty owned CPU `image_buffer`; empty or opaque
 * non-CPU results remain unnormalized.
 */
void normalize_node_output_image_value(NodeOutput* output);

/**
 * @brief Derives exact full-validity Region metadata for one complete output.
 *
 * @param output Complete output about to enter the formal HP cache.
 * @return Exact data-window ImageRect for an image Value, zero-origin full
 *         ImageRect for a current ImageBuffer, full
 *         TensorSlice for a non-image DenseTensor Value, or Whole for a
 *         data-only/opaque output without finite logical dimensions.
 * @throws std::logic_error for an invalid Value accessor.
 * @throws std::invalid_argument when retained image/tensor facts violate their
 *         declared contracts.
 * @throws std::overflow_error when a logical extent exceeds Region bounds.
 * @throws std::bad_alloc when TensorSlice or Region storage cannot allocate.
 * @note This function derives validity metadata only. Display windows never
 *       authorize payload validity. The function preserves allocation
 *       identity, Value revision, bytes, and ownership.
 */
RegionSet full_node_output_region(const NodeOutput& output);

/**
 * @brief Tests whether exact Region metadata covers one complete output.
 *
 * @param output Formal HP output whose finite logical bounds are derived.
 * @param region Exact normalized validity claimed for `output`.
 * @return True when `region` contains the complete compatibility output
 * Region, or the complete rank-general TensorSlice for a sealed Value.
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
 * @throws std::bad_alloc when snapshot, coordinate, or output storage cannot
 *         allocate.
 * @note This source-private bridge changes neither operation ABI v2 nor the
 *       immutable Value contract. Empty returns `existing`; Whole returns
 *       `update`.
 */
std::optional<NodeOutput> merge_node_output_region(
    const NodeOutput& existing, const NodeOutput& update,
    const RegionSet& updated_region);

}  // namespace ps::value_image_adapter
