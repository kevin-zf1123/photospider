#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include "core/checked_math.hpp"
#include "photospider/core/cancellation.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::radix_internal {

// Fixed-width unsigned coordinates have a bounded number of digits. Stable
// radix passes avoid comparison sorting, including for adversarial coordinates.
// Scratch storage is reserved exactly once and shares the caller's root ledger.
template <class T, class Coordinate, class Charge>
Status sort(ResourceVector<T>* values, std::size_t rank, Coordinate coordinate,
            Charge charge, const CancellationToken& cancellation) {
  if (cancellation.cancelled())
    return {ErrorCode::Cancelled, {}};
  if (values->size() < 2)
    return charge(UINT64_C(1));
  try {
    std::array<std::uint64_t, 8> base{}, varying{};
    if (rank > base.size() ||
        !core_internal::can_multiply(values->size(), rank + 2))
      return {ErrorCode::ResourceExhausted, "radix coordinate count"};
    auto status = charge(values->size() * rank);
    if (!status.ok())
      return status;
    for (std::size_t axis = 0; axis < rank; ++axis)
      base[axis] = coordinate((*values)[0], axis);
    for (std::size_t i = 0; i < values->size(); ++i) {
      if (!(i & 1023U) && cancellation.cancelled())
        return {ErrorCode::Cancelled, {}};
      for (std::size_t axis = 0; axis < rank; ++axis)
        varying[axis] |= coordinate((*values)[i], axis) ^ base[axis];
    }
    ResourceVector<T> scratch(values->get_allocator());
    for (std::size_t axis = rank; axis-- > 0;) {
      for (unsigned shift = 0; shift < 64; shift += 8) {
        if (!((varying[axis] >> shift) & 255U))
          continue;
        status = charge(values->size() * 2 + 256);
        if (!status.ok())
          return status;
        if (scratch.empty())
          scratch.resize(values->size());
        std::array<std::size_t, 256> positions{};
        for (std::size_t i = 0; i < values->size(); ++i) {
          if (!(i & 1023U) && cancellation.cancelled())
            return {ErrorCode::Cancelled, {}};
          ++positions[(coordinate((*values)[i], axis) >> shift) & 255U];
        }
        std::size_t next = 0;
        for (auto& position : positions) {
          const auto count = position;
          position = next;
          next += count;
        }
        for (std::size_t i = 0; i < values->size(); ++i) {
          if (!(i & 1023U) && cancellation.cancelled())
            return {ErrorCode::Cancelled, {}};
          const auto digit = (coordinate((*values)[i], axis) >> shift) & 255U;
          scratch[positions[digit]++] = (*values)[i];
        }
        values->swap(scratch);
      }
    }
    return cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                    : Status::success();
  } catch (const std::bad_alloc&) {
    return {ErrorCode::ResourceExhausted, "radix scratch capacity"};
  }
}

}  // namespace ps::radix_internal
