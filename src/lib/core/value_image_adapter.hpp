#pragma once

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::value_image_adapter {

/**
 * @brief Copies one current nonempty CPU ImageBuffer into a sealed image Value.
 *
 * @param buffer Valid current CPU image descriptor and payload to snapshot.
 * @return Value with shape `[height, width, channels]`, explicit y/x/channel
 * axes, the same positive row stride, fresh allocation identity, and fresh
 * Value revision.
 * @throws std::invalid_argument for malformed, empty, non-CPU, or
 * unrepresentable ImageBuffer state.
 * @throws std::overflow_error when envelope arithmetic cannot be represented.
 * @throws std::bad_alloc when snapshot or immutable publication allocation
 * fails.
 * @note Only active row bytes are copied. Inter-row padding inside the exact
 * Value envelope is initialized independently; no ImageBuffer owner, context,
 * deleter, or plugin-library lifetime is retained.
 */
Value snapshot_cpu_image_value(const ImageBuffer& buffer);

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
 * through ImageView; source padding is never copied as active pixels.
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

}  // namespace ps::value_image_adapter
