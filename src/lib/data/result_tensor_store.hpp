#pragma once

#include <memory>

#include "data/device_tensor_span.hpp"
#include "photospider/data/result.hpp"

namespace ps::data_internal {
// Owns affine/spatial backings, retained source Results, and backing leases.
// Initialization borrows static schema and budget; retained vectors and planar
// config keep their allocation owners. Publication serializes access with the
// owning Result mutex and commits coverage only after backing admission.
struct ResultTensorBacking {
  std::uint64_t tile_extent = 128;
  Footprint coverage;
  ResourceVector<TensorSpan> affine;
  ResourceVector<DeviceTensorSpan> device;
  // True only when affine[i].region() equals coverage.boxes()[i].
  bool affine_canonical = false;
  ResourceVector<ResultRef> affine_owners;
  ResourceVector<std::shared_ptr<void>> affine_growth;
  Status initialize(const ResourceBudget& budget, const ResultTensorSpec& spec,
                    std::uint64_t tile_extent);
};
}  // namespace ps::data_internal
