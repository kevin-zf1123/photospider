#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/checked_math.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::gpu_internal {
// Fixed device bootstrap state. Dynamic blocks retain this explicit root;
// unmanaged devices never inherit a caller's thread-local allocation scope.
struct MetadataAccount final {
  explicit MetadataAccount(std::shared_ptr<ResourceBudget> root)
      : budget(std::move(root)) {}
  template <class F>
  auto retry(F&& operation) const -> decltype(operation()) {
    if (!budget)
      return operation();
    try {
      ResourceAllocationScope recoverable(*budget);
      return operation();
    } catch (const std::bad_alloc&) {
      if (reclaim)
        reclaim();
    }
    return operation();
  }
  std::shared_ptr<ResourceBudget> budget;
  std::function<void()> reclaim;
};
template <class T>
class NativeAllocator {
 public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  NativeAllocator() = default;
  explicit NativeAllocator(std::shared_ptr<MetadataAccount> account,
                           bool recover = true)
      : account_(std::move(account)), recover_(recover) {}
  template <class U>
  NativeAllocator(const NativeAllocator<U>& other) noexcept
      : account_(other.account_), recover_(other.recover_) {}
  T* allocate(std::size_t count) {
    if (!account_ || !account_->budget)
      return std::allocator<T>{}.allocate(count);
    auto operation = [&] {
      return ResourceAllocator<T>(*account_->budget).allocate(count);
    };
    return recover_ ? account_->retry(operation) : operation();
  }
  void deallocate(T* pointer, std::size_t count) noexcept {
    if (account_ && account_->budget)
      ResourceAllocator<T>(*account_->budget).deallocate(pointer, count);
    else
      std::allocator<T>{}.deallocate(pointer, count);
  }
  NativeAllocator select_on_container_copy_construction() const noexcept {
    return *this;
  }
  template <class U>
  bool operator==(const NativeAllocator<U>& other) const noexcept {
    return account_ == other.account_ && recover_ == other.recover_;
  }
  template <class U>
  bool operator!=(const NativeAllocator<U>& other) const noexcept {
    return !(*this == other);
  }

 private:
  template <class U>
  friend class NativeAllocator;
  std::shared_ptr<MetadataAccount> account_;
  bool recover_ = true;
};
// NOLINTBEGIN(whitespace/indent_namespace)
using NativeString =
    std::basic_string<char, std::char_traits<char>, NativeAllocator<char>>;
template <class K, class V>
using NativeMap =
    std::map<K, V, std::less<K>, NativeAllocator<std::pair<const K, V>>>;
// NOLINTEND
// A borrowed query has the same byte ordering as its concatenated owning key.
// Views survive only the locked lookup; cache entries always own their bytes.
struct PipelineKeyView final {
  std::array<std::string_view, 5> parts{};
  int compare(const NativeString& owned) const noexcept {
    std::size_t offset = 0;
    for (const auto part : parts) {
      const auto count = std::min(part.size(), owned.size() - offset);
      if (count) {
        const int order = std::char_traits<char>::compare(
            part.data(), owned.data() + offset, count);
        if (order)
          return order;
      }
      if (part.size() > count)
        return 1;
      offset += count;
    }
    return offset < owned.size() ? -1 : 0;
  }
  NativeString own(const std::shared_ptr<MetadataAccount>& account) const {
    NativeString key(NativeAllocator<char>(account, false));
    std::size_t size = 0;
    for (const auto part : parts) {
      if (!core_internal::can_add(size, part.size(), key.max_size()))
        throw std::bad_alloc();
      size += part.size();
    }
    key.reserve(size);
    for (const auto part : parts)
      if (!part.empty())
        key.append(part.data(), part.size());
    return key;
  }
};
struct PipelineKeyLess final {
  using is_transparent = void;
  bool operator()(const NativeString& left,
                  const NativeString& right) const noexcept {
    return left < right;
  }
  bool operator()(const PipelineKeyView& left,
                  const NativeString& right) const noexcept {
    return left.compare(right) < 0;
  }
  bool operator()(const NativeString& left,
                  const PipelineKeyView& right) const noexcept {
    return right.compare(left) > 0;
  }
};
template <class T>
struct PipelineCache final {
  explicit PipelineCache(const std::shared_ptr<MetadataAccount>& account)
      : entries(
            PipelineKeyLess{},
            NativeAllocator<std::pair<const NativeString, T>>(account, false)) {
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
  }
  std::mutex mutex;
  std::map<NativeString, T, PipelineKeyLess,
           NativeAllocator<std::pair<const NativeString, T>>>
      entries;
};
template <class T>
std::shared_ptr<PipelineCache<T>> make_pipeline_cache(
    const std::shared_ptr<MetadataAccount>& account) {
  auto cache = std::make_shared<PipelineCache<T>>(account);
  account->reclaim = [weak = std::weak_ptr<PipelineCache<T>>(cache)] {
    if (auto retained = weak.lock())
      retained->clear();
  };
  return cache;
}
}  // namespace ps::gpu_internal
