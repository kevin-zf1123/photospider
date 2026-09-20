#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

#include "01-numeric/array_publication.hpp"
#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "photospider/data/value.hpp"

namespace ps::plugin_internal::numeric_ops {
// Canonical keys preserve equality of signed zeros and place every NaN after
// numerical values. The original index breaks ties, giving stable ordering
// independently of the in-place comparison sorting algorithm.
inline std::uint64_t stable_order_key(std::uint64_t bits, ElementType type) {
  if (type == ElementType::UInt8)
    return bits;
  if (type == ElementType::Int64)
    return bits ^ (UINT64_C(1) << 63);
  const auto value = BinaryParts::decode(bits, type == ElementType::Float32);
  return value.nan ? UINT64_MAX : value.order_key();
}
// Fixed comparison scratch belongs to a host continuation. Iterative heapsort
// retains only an allocator-owned Int64 permutation (8*N bytes), no recursive
// stack or raw-value copy. Comparisons read the already supplied immutable
// line. Tuple (numerical key, original index) is unique, so ascending tuple
// order is exactly the specified stable numerical order, including zero and NaN
// ties.
struct StableOrderWorkspace final {
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  Result<Value> build(
      std::uint64_t count, ElementType type, SequenceProfile profile,
      const BufferAllocator& allocator,
      const std::function<Status(std::uint64_t)>& consume,
      const std::function<Status(std::uint64_t, std::uint64_t*)>& read) {
    using Answer = Result<Value>;
    auto status = consume(count + 16);
    if (!status.ok())
      return Answer(status);
    if (!count || count > UINT64_MAX / 8)
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "stable order capacity",
                           FailureReason::CapacityLimit});
    ArrayPublication publication(1, 1);
    auto allocation = MutableValue::allocate({ElementType::Int64, {count}},
                                             Region::whole({count}), allocator);
    if (!allocation.ok())
      return Answer(allocation.status());
    auto output = allocation.take_value();
    for (std::uint64_t i = 0; i < count; ++i) {
      status = consume(1);
      if (!status.ok())
        return Answer(status);
      std::memcpy(output.data() + i * 8, &i, 8);
    }
    const auto at = [&](std::uint64_t i) {
      std::uint64_t index = 0;
      std::memcpy(&index, output.data() + i * 8, 8);
      return index;
    };
    const auto swap = [&](std::uint64_t a, std::uint64_t b) {
      const auto x = at(a), y = at(b);
      std::memcpy(output.data() + a * 8, &y, 8);
      std::memcpy(output.data() + b * 8, &x, 8);
    };
    const auto before = [&](std::uint64_t a, std::uint64_t b) -> Result<bool> {
      auto work = consume(16);
      if (!work.ok())
        return Result<bool>(work);
      std::uint64_t x = 0, y = 0;
      work = read(a, &x);
      if (work.ok())
        work = read(b, &y);
      if (!work.ok())
        return Result<bool>(work);
      left.fill(stable_order_key(x, type));
      right.fill(stable_order_key(y, type));
      compare_keys(left.data(), right.data(), greater.data(), less.data(),
                   profile);
      return Result<bool>(less[0] || (!greater[0] && a < b));
    };
    const auto sift = [&](std::uint64_t root, std::uint64_t end) -> Status {
      while (root < end / 2) {
        auto child = root * 2 + 1;
        if (child + 1 < end) {
          auto order = before(at(child), at(child + 1));
          if (!order.ok())
            return order.status();
          if (order.value())
            ++child;
        }
        auto order = before(at(root), at(child));
        if (!order.ok())
          return order.status();
        if (!order.value())
          break;
        swap(root, child);
        root = child;
      }
      return Status::success();
    };
    for (auto i = count / 2; i; --i) {
      status = sift(i - 1, count);
      if (!status.ok())
        return Answer(status);
    }
    for (auto i = count; i > 1; --i) {
      status = consume(4);
      if (!status.ok())
        return Answer(status);
      swap(0, i - 1);
      status = sift(0, i - 1);
      if (!status.ok())
        return Answer(status);
    }
    auto value = std::move(output).publish();
    return value.ok() ? publication.retain(value.take_value())
                      : Answer(value.status());
  }
};
}  // namespace ps::plugin_internal::numeric_ops
