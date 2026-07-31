#pragma once

#include <cstddef>

#include "photospider/data/value.hpp"

/**
 * @file value_validation.hpp
 * @brief Source-private shared validation for writable Value producers.
 */

namespace ps {

/**
 * @brief Validates one exact positive writable DenseTensor envelope.
 *
 * The validation requires a zero logical-origin offset, positive strides, an
 * exact storage envelope, and a checked non-overlap proof for every active
 * axis. It performs no allocation publication, identity minting, fence
 * creation, payload access, or owner retention.
 *
 * @param descriptor Valid positive-shape DenseTensor descriptor.
 * @param layout Candidate writable producer layout.
 * @param storage_size Proposed exact allocation byte length.
 * @return Nothing after successful validation.
 * @throws std::invalid_argument for unsupported element encoding, nonzero
 * offset, non-positive stride, storage-size mismatch, or unprovable
 * non-overlap.
 * @throws std::overflow_error when envelope or non-overlap arithmetic is
 * unrepresentable.
 * @throws std::bad_alloc when rank-bounded non-overlap proof state cannot
 * allocate.
 * @note This is the shared authority for ordinary builders and explicit
 * transfer preparation. General immutable Value publishers remain free to
 * publish checked signed or zero-stride aliases over sealed storage.
 */
void validate_dense_tensor_producer_envelope(
    const DenseTensorDescriptor& descriptor, const StridedLayout& layout,
    std::size_t storage_size);

}  // namespace ps
