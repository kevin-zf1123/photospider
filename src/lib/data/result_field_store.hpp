#pragma once

#include <cstdint>
#include <memory>

#include "photospider/data/result.hpp"

namespace ps::data_internal {
// Owns immutable byte chunks and certified row counts.
// The publication mutex is borrowed by every call; failure is returned before
// the coordinator commits its total bytes or revision. Physical storage retires
// with this owner, independently of a captured descriptor or read plan.
struct ResultFieldBacking {
  struct Chunk {
    std::uint64_t offset;
    std::shared_ptr<const CpuStorage> bytes;
  };
  struct Storage {
    explicit Storage(const ResourceBudget& budget)
        : chunks(ResourceAllocator<Chunk>(budget)) {}
    ResourceVector<Chunk> chunks;
    std::uint64_t size = 0;
  };
  std::shared_ptr<Storage> storage;
  Result<std::shared_ptr<const CpuStorage>> read(
      const ResourceBudget& budget, std::uint64_t offset, std::uint64_t bytes,
      std::uint64_t maximum_window,
      const CancellationToken& cancellation) const;
  std::uint64_t written = 0, certified = 0, row_bytes = 0;
  Status append(const ResourceBudget& budget, std::uint64_t rows,
                ByteView bytes, const CancellationToken& cancellation);
};
}  // namespace ps::data_internal
