#pragma once

#include "photospider/data/result.hpp"

namespace ps::data_internal {
// Owns temporary bytes, frozen relation and monotonically certified row counts.
// The publication mutex is borrowed by every call; failure is returned before
// the coordinator commits its total bytes or revision. Physical storage retires
// with this owner, independently of a captured descriptor or read plan.
struct ResultFieldBacking {
  TemporaryStorage storage;
  ResultRelation relation;
  std::uint64_t written = 0, certified = 0, row_bytes = 0;
  Status append(const ResourceBudget& budget, std::uint64_t rows,
                ByteView bytes, const CancellationToken& cancellation);
};
}  // namespace ps::data_internal
