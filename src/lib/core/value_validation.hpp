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

/**
 * @brief Validates one exact writable V-13 Blocked FP4 envelope.
 *
 * Validation requires a valid FP4 E2M1 descriptor with independent block-scale
 * quantization, no image facet, layout version 1, matching rank/block shape,
 * nibble-aligned positive bit strides and offset, an exact byte envelope, and
 * a checked non-overlap proof for complete physical block bit ranges. It
 * performs no allocation, identity minting, fence creation, payload access, or
 * owner retention.
 *
 * @param descriptor Candidate packed logical descriptor.
 * @param layout Candidate physical Blocked layout.
 * @param storage_size Proposed exact retained allocation byte length.
 * @throws std::invalid_argument for unsupported encoding or quantization,
 * malformed ranks/shapes, invalid version/order/alignment/strides, overlapping
 * blocks, or an inexact storage size.
 * @throws std::overflow_error when block-grid, scale-count, bit-span, or byte
 * envelope arithmetic is unrepresentable.
 * @throws std::bad_alloc when rank-bounded validation storage cannot allocate.
 * @note This is the shared authority for blocked builders and explicit
 * transfer preparation. The V-13 exact producer envelope may have a nonzero
 * nibble offset because leading unused bits are part of its byte allocation.
 */
void validate_dense_tensor_producer_envelope(
    const DenseTensorDescriptor& descriptor, const BlockedLayout& layout,
    std::size_t storage_size);

}  // namespace ps
