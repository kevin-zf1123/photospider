#pragma once

/**
 * @file image_buffer_storage.hpp
 * @brief Declares source-private ImageBuffer storage-envelope inspection.
 */

#include "photospider/core/image_buffer.hpp"

namespace ps::detail {

/**
 * @brief Conservatively detects overlap between two active storage envelopes.
 * @param left Valid nonempty CPU descriptor with owned addressable data.
 * @param right Valid nonempty CPU descriptor with owned addressable data.
 * @return True when the descriptors share an owner, their checked half-open
 * address intervals overlap, or an interval endpoint is not representable.
 * @throws std::invalid_argument or std::overflow_error only when a caller
 * violates the requirement to validate both descriptors first.
 * @note The active envelope includes row gaps through the final active row but
 * excludes padding after that row. Address ordering uses `std::uintptr_t`, not
 * relational comparison between unrelated pointers. A shared control block or
 * endpoint overflow is rejected conservatively because independence cannot be
 * proven fail-closed.
 */
bool image_buffer_storage_envelopes_may_overlap(const ImageBuffer& left,
                                                const ImageBuffer& right);

}  // namespace ps::detail
