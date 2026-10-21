#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "photospider/data/tensor_span.hpp"

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
// Both word arrays borrow preallocated invocation scratch. Byte-wise access
// supports raw arena storage without an implicit typed-array lifetime.
struct OrderWords final {
  std::uint8_t* bytes;
  std::uint64_t get(std::uint64_t index) const {
    std::uint64_t value;
    std::memcpy(&value, bytes + index * 8, 8);
    return value;
  }
  void set(std::uint64_t index, std::uint64_t value) const {
    std::memcpy(bytes + index * 8, &value, 8);
  }
};
// Iterative heapsort needs 16*N bytes for permutation and classified keys.
// Tuple (numerical key, original index) gives stable order; keys are read once.
struct StableOrderWorkspace final {
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  template <class Consume, class Read>
  Status build(std::uint64_t count, ElementType type, SequenceProfile profile,
               OrderWords output, OrderWords keys, const Consume& consume,
               const Read& read) {
    auto status = consume(count + 16);
    if (!status.ok())
      return status;
    for (std::uint64_t i = 0; i < count; ++i) {
      status = consume(1);
      if (!status.ok())
        return status;
      output.set(i, i);
      std::uint64_t bits = 0;
      status = read(i, &bits);
      if (!status.ok())
        return status;
      keys.set(i, stable_order_key(bits, type));
    }
    const auto at = [&](std::uint64_t i) { return output.get(i); };
    const auto swap = [&](std::uint64_t a, std::uint64_t b) {
      const auto value = output.get(a);
      output.set(a, output.get(b));
      output.set(b, value);
    };
    const auto before = [&](std::uint64_t a, std::uint64_t b) -> Result<bool> {
      auto work = consume(16);
      if (!work.ok())
        return Result<bool>(work);
      left.fill(keys.get(a));
      right.fill(keys.get(b));
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
        return status;
    }
    for (auto i = count; i > 1; --i) {
      status = consume(4);
      if (!status.ok())
        return status;
      swap(0, i - 1);
      status = sift(0, i - 1);
      if (!status.ok())
        return status;
    }
    return Status::success();
  }
};
}  // namespace ps::plugin_internal::numeric_ops
