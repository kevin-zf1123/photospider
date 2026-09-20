#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/dependency.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps::dependency_internal {
struct MetadataOwner final {
  ResourceBudget root;
  ResourceLease lease;
  mutable std::uint64_t heap_capacity = 0;
  MetadataOwner(ResourceBudget budget, ResourceLease capacity,
                std::uint64_t bytes)
      : root(std::move(budget)),
        lease(std::move(capacity)),
        heap_capacity(bytes) {}
};
// The ledger covers declared STL element capacities; implementation-private
// heap headers and shared_ptr control blocks retain the legacy exclusion.
struct MetadataBytes final {
  std::uint64_t bytes = 0;
  void add(std::uint64_t count, std::uint64_t width = 1) {
    if (count > (UINT64_MAX - bytes) / width)
      throw std::bad_alloc();
    bytes += count * width;
  }
  template <class T>
  void block(const std::vector<T>& items, bool copy) {
    add(copy ? items.size() : items.capacity(), sizeof(T));
  }
  void footprint(const Footprint& value, bool copy) {
    block(value.shape(), copy);
    block(value.boxes(), copy);
    for (const auto& box : value.boxes())
      block(box.dimensions(), copy);
  }
  void needs(const std::vector<DependencyNeed>& values, bool copy) {
    block(values, copy);
    for (const auto& need : values) {
      footprint(need.samples, copy);
      block(need.tags, copy);
    }
  }
  void rows(const std::vector<AtomCertificate>& values, bool copy) {
    block(values, copy);
    for (const auto& row : values) {
      block(row.output, copy);
      needs(row.inputs, copy);
    }
  }
  void pieces(const std::vector<DependencyMapPiece>& values, bool copy) {
    block(values, copy);
    for (const auto& piece : values) {
      footprint(piece.coverage, copy);
      block(piece.inputs, copy);
      for (const auto& need : piece.inputs) {
        block(need.axes, copy);
        block(need.tags, copy);
      }
    }
  }
};
inline std::shared_ptr<const MetadataOwner> metadata_owner(
    std::uint64_t bytes,
    const std::shared_ptr<const MetadataOwner>& source = {}) {
  std::optional<ResourceBudget> root;
  if (const auto* active = resource_internal::metadata_budget())
    root = *active;
  else if (source)
    root = source->root;
  if (!root)
    return {};
  try {
    if (bytes > UINT64_MAX - sizeof(MetadataOwner))
      throw std::bad_alloc();
    const auto total = bytes + sizeof(MetadataOwner);
    auto admitted = root->reserve(ResourceCapacity::host(total, total));
    if (!admitted.ok())
      throw std::bad_alloc();
    return std::make_shared<MetadataOwner>(*root, admitted.take_value(), bytes);
  } catch (const std::bad_alloc&) {
    resource_internal::metadata_failure(*root, ErrorCode::ResourceExhausted);
    throw;
  }
}
inline std::uint64_t certificate_bytes(
    const std::string& identity, const Footprint& coverage,
    const std::vector<std::vector<std::uint64_t>>& shapes,
    const std::vector<AtomCertificate>& rows,
    const std::vector<DependencyMapPiece>& pieces, bool copy) {
  MetadataBytes count;
  count.add(identity.capacity() + 1);
  count.footprint(coverage, copy);
  count.block(shapes, copy);
  for (const auto& shape : shapes)
    count.block(shape, copy);
  count.rows(rows, copy);
  count.pieces(pieces, copy);
  return count.bytes;
}
}  // namespace ps::dependency_internal
