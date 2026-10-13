#include "data/result_tensor_store.hpp"

#include <memory>
#include <utility>

namespace ps::data_internal {
Status ResultTensorBacking::initialize(
    const ResourceBudget& budget, const ResultTensorSpec& spec,
    const std::shared_ptr<PlanarPageBudget>& page_budget,
    const ResultGrowthLimits& limits, std::uint64_t tile_extent) {
  views = ResourceVector<ResultTensorView>(
      ResourceAllocator<ResultTensorView>(budget));
  backing = ResourceVector<ResultSpatialBacking>(
      ResourceAllocator<ResultSpatialBacking>(budget));
  affine_owners =
      ResourceVector<ResultRef>(ResourceAllocator<ResultRef>(budget));
  affine = ResourceVector<Value>(ResourceAllocator<Value>(budget));
  affine_metadata =
      ResourceVector<ResourceLease>(ResourceAllocator<ResourceLease>(budget));
  affine_growth = ResourceVector<std::shared_ptr<void>>(
      ResourceAllocator<std::shared_ptr<void>>(budget));
  auto initial_coverage = Footprint::none(spec.sample_shape());
  if (!initial_coverage.ok())
    return initial_coverage.status();
  coverage = initial_coverage.take_value();
  if (!spec.layout.spatial)
    return Status::success();
  config.order = spec.layout.order;
  config.height_axis = spec.layout.height_axis;
  config.width_axis = spec.layout.width_axis;
  config.channel_axis = spec.layout.channel_axis;
  config.row_pitch_bytes = spec.layout.row_pitch_bytes;
  config.tile_extent = tile_extent;
  config.maximum_backed_bytes = limits.maximum_bytes;
  config.aggregate_budget = page_budget;
  return Status::success();
}
}  // namespace ps::data_internal
