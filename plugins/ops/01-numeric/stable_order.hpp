#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "photospider/data/value.hpp"
#include "photospider/execution/resource_allocator.hpp"

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
// Fixed comparison scratch belongs to the Whole callback workspace. Iterative
// heapsort retains a metadata-ledger-owned permutation and classified keys
// (16*N element bytes plus allocator overhead), without recursive stack or raw
// value copies. Tuple (numerical key, original index) gives stable order. Keys
// are read once; comparisons avoid repeated strided input address calculations.
struct StableOrderWorkspace final {
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  Result<ResourceVector<std::uint64_t>> build(
      std::uint64_t count, ElementType type, SequenceProfile profile,
      const std::function<Status(std::uint64_t)>& consume,
      const std::function<Status(std::uint64_t, std::uint64_t*)>& read) {
    using Answer = Result<ResourceVector<std::uint64_t>>;
    auto status = consume(count + 16);
    if (!status.ok())
      return Answer(status);
    if (!count || count > UINT64_MAX / 8)
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "stable order capacity",
                           FailureReason::CapacityLimit});
    ResourceVector<std::uint64_t> output(count), keys(count);
    for (std::uint64_t i = 0; i < count; ++i) {
      status = consume(1);
      if (!status.ok())
        return Answer(status);
      output[i] = i;
      std::uint64_t bits = 0;
      status = read(i, &bits);
      if (!status.ok())
        return Answer(status);
      keys[i] = stable_order_key(bits, type);
    }
    const auto at = [&](std::uint64_t i) { return output[i]; };
    const auto swap = [&](std::uint64_t a, std::uint64_t b) {
      std::swap(output[a], output[b]);
    };
    const auto before = [&](std::uint64_t a, std::uint64_t b) -> Result<bool> {
      auto work = consume(16);
      if (!work.ok())
        return Result<bool>(work);
      left.fill(keys[a]);
      right.fill(keys[b]);
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
    return Answer(std::move(output));
  }
};
}  // namespace ps::plugin_internal::numeric_ops
