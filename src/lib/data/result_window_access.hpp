#pragma once

#include <functional>

#include "photospider/data/result.hpp"

namespace ps::execution_internal {
class ResultWindowAccess final {
 public:
  // Combines windows for one exact, fully covered Region with matching Result
  // schema, slot, and accounting Root. It charges that Root for coverage
  // validation and piece indexing, and observes cancellation. The returned
  // window retains each original owner/lease and exposes their backing pieces
  // without copying payload samples; it does not create a synthetic Result
  // identity. Callers must still hold authorization for every source piece.
  static Result<ResultTensorReadWindow> compose(
      const ResourceBudget& budget, const Region& region,
      ResourceVector<ResultTensorReadWindow> windows,
      const CancellationToken& cancellation);
  static Result<std::uint64_t> read_work(const ResultTensorReadWindow& window);
  // Visits disjoint Need-authorized pieces without reading samples. False
  // means there is no smaller backing partition; borrowed regions cannot
  // escape.
  static Result<bool> visit_backing_regions(
      const ResultTensorReadWindow& window, const ResourceBudget& budget,
      const std::function<Status(const Region&)>& visitor);
  static Result<ResultTensorReadWindow> replace_backing(
      ResultTensorReadWindow window, Value backing,
      ResourceLease metadata = {});
  static Result<Value> affine(const ResultTensorReadWindow& window);
  static Result<std::optional<Value>> input_view(
      const ResultTensorReadWindow& window, const FootprintLimits& limits);
};
}  // namespace ps::execution_internal
