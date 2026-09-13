#pragma once

#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <scoped_allocator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/execution/resources.hpp"

namespace ps {
namespace resource_internal {
PHOTOSPIDER_API const ResourceBudget* metadata_budget() noexcept;
PHOTOSPIDER_API void metadata_failure(const ResourceBudget&,
                                      ErrorCode) noexcept;
}  // namespace resource_internal
/** @brief Thread-local default for metadata containers created by a callback.
 * Explicit allocators retain their chosen root. Nested scopes restore the
 * previous default; escaped containers retain their own allocator ownership.
 */
class PHOTOSPIDER_API ResourceAllocationScope final {
 public:
  explicit ResourceAllocationScope(const ResourceBudget& budget,
                                   ErrorCode* failure = nullptr) noexcept;
  ~ResourceAllocationScope() noexcept;
  ResourceAllocationScope(const ResourceAllocationScope&) = delete;
  ResourceAllocationScope& operator=(const ResourceAllocationScope&) = delete;

 private:
  const ResourceBudget* previous_ = nullptr;
  ErrorCode* previous_failure_ = nullptr;
};
/** @brief Ledger role of the requested STL element block. */
enum class ResourceAllocationKind : std::uint32_t { Metadata = 0, Payload = 1 };
/** @brief STL allocator with admission before physical allocation.
 * Each allocated block owns its lease through deallocation. Copying a
 * container propagates the root and separately admits its real new capacity;
 * moving an allocator-aware container transfers that ownership. A default
 * allocator is ordinary caller-owned storage outside a managed root. The
 * Payload mode also charges the Payload sublimit and preserves that role on
 * copy/rebind; Metadata remains the default. The allocator's block header is
 * included in Metadata; implementation-private heap/control
 * blocks remain outside the managed-capacity model. Exhaustion throws
 * std::bad_alloc, including checked size overflow. No RSS guarantee is made.
 */
template <class T>
class ResourceAllocator {
 public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  ResourceAllocator() noexcept {
    if (auto* budget = resource_internal::metadata_budget())
      budget_ = *budget;
  }
  ResourceAllocator(const ResourceAllocator&) noexcept = default;
  ResourceAllocator& operator=(const ResourceAllocator&) noexcept = default;
  ResourceAllocator(ResourceAllocator&& other) noexcept
      : budget_(other.budget_), kind_(other.kind_) {}
  ResourceAllocator& operator=(ResourceAllocator&& other) noexcept {
    budget_ = other.budget_;
    kind_ = other.kind_;
    return *this;
  }
  explicit ResourceAllocator(
      ResourceBudget budget,
      ResourceAllocationKind kind = ResourceAllocationKind::Metadata) noexcept
      : budget_(std::move(budget)), kind_(kind) {}
  template <class U>
  ResourceAllocator(const ResourceAllocator<U>& other) noexcept
      : budget_(other.budget_), kind_(other.kind_) {}
  T* allocate(std::size_t count) {
    if (!budget_)
      return std::allocator<T>{}.allocate(count);
    if (count > (std::numeric_limits<std::size_t>::max() - offset) / sizeof(T))
      fail();
    const auto bytes = offset + count * sizeof(T);
    auto capacity = ResourceCapacity::host(
        bytes, kind_ == ResourceAllocationKind::Payload ? offset : bytes);
    if (kind_ == ResourceAllocationKind::Payload)
      capacity[ResourceKind::Payload] = count * sizeof(T);
    capacity[ResourceKind::Entries] = 1;
    auto admitted = budget_->reserve(capacity);
    if (!admitted.ok())
      fail();
    auto lease = admitted.take_value();
    void* storage = nullptr;
    try {
      storage = ::operator new(bytes, std::align_val_t(alignment));
    } catch (const std::bad_alloc&) {
      fail();
    }
    new (storage) Header{std::move(lease)};
    return reinterpret_cast<T*>(static_cast<unsigned char*>(storage) + offset);
  }
  void deallocate(T* pointer, std::size_t count) noexcept {
    if (!budget_) {
      std::allocator<T>{}.deallocate(pointer, count);
      return;
    }
    auto* storage = reinterpret_cast<unsigned char*>(pointer) - offset;
    auto* header = reinterpret_cast<Header*>(storage);
    auto lease = std::move(header->lease);
    header->~Header();
    ::operator delete(storage, std::align_val_t(alignment));
    // The local lease retires after physical storage, including its header.
  }
  ResourceAllocator select_on_container_copy_construction() const noexcept {
    if (auto* budget = resource_internal::metadata_budget())
      return ResourceAllocator(*budget, kind_);
    return *this;
  }
  bool owned_by(const ResourceBudget& budget) const noexcept {
    return budget_ && budget_->same_owner(budget);
  }
  template <class U>
  bool operator==(const ResourceAllocator<U>& other) const noexcept {
    return kind_ == other.kind_ &&
           ((!budget_ && !other.budget_) ||
            (budget_ && other.budget_ && budget_->same_owner(*other.budget_)));
  }
  template <class U>
  bool operator!=(const ResourceAllocator<U>& other) const noexcept {
    return !(*this == other);
  }

 private:
  template <class U>
  friend class ResourceAllocator;
  [[noreturn]] void fail() const {
    resource_internal::metadata_failure(*budget_, ErrorCode::ResourceExhausted);
    throw std::bad_alloc();
  }
  struct Header {
    ResourceLease lease;
  };
  static constexpr std::size_t alignment = alignof(T) > alignof(Header)
                                               ? alignof(T)
                                               : alignof(Header);
  static constexpr std::size_t offset =
      (sizeof(Header) + alignment - 1) / alignment * alignment;
  std::optional<ResourceBudget> budget_;
  ResourceAllocationKind kind_ = ResourceAllocationKind::Metadata;
};
/** @brief Allocator-aware owned metadata strings and sequences. */
// NOLINTBEGIN(whitespace/indent_namespace)
using ResourceString =
    std::basic_string<char, std::char_traits<char>, ResourceAllocator<char>>;
template <class T>
using ResourceVector = std::vector<T, ResourceAllocator<T>>;
struct ResourceStringLess {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a < b;
  }
};
template <class T>
using ResourceMapAllocator = std::scoped_allocator_adaptor<
    ResourceAllocator<std::pair<const ResourceString, T>>,
    ResourceAllocator<char>>;
template <class T>
using ResourceMap =
    std::map<ResourceString, T, ResourceStringLess, ResourceMapAllocator<T>>;
// NOLINTEND
template <class T>
ResourceMap<T> make_resource_map(const ResourceBudget& budget) {
  return ResourceMap<T>(
      ResourceStringLess{},
      ResourceMapAllocator<T>(
          ResourceAllocator<std::pair<const ResourceString, T>>(budget),
          ResourceAllocator<char>(budget)));
}
}  // namespace ps
