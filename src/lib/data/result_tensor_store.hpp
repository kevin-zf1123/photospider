#include <memory>
#pragma once

#include "photospider/data/result.hpp"

namespace ps::data_internal {
// Owns affine/spatial backings, retained source Results, and backing leases.
// Initialization borrows static schema and budget; retained vectors and planar
// config keep their allocation owners. Publication serializes access with the
// owning Result mutex and commits coverage only after backing admission.
struct ResultTensorView {
  Region region;
  PlanarImage backing;
  ResourceVector<ResultRef> owners;
};
struct ResultSpatialBacking {
  ResourceVector<std::uint64_t> batch;
  PlanarImage image;
};
struct ResultTensorBacking {
  PlanarImageConfig config;
  ResourceVector<ResultTensorView> views;
  ResourceVector<ResultSpatialBacking> backing;
  ResourceVector<Value> affine;
  ResourceVector<ResultRef> affine_owners;
  ResourceVector<ResourceLease> affine_metadata;
  ResourceVector<std::shared_ptr<void>> affine_growth;
  Footprint coverage;
  ResultRelation relation;
  Status initialize(const ResourceBudget& budget, const ResultTensorSpec& spec,
                    const std::shared_ptr<PlanarPageBudget>& page_budget,
                    const ResultGrowthLimits& limits, std::uint32_t tile_height,
                    std::uint32_t tile_width);
};
}  // namespace ps::data_internal
