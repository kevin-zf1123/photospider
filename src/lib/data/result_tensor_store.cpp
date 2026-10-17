#include "data/result_tensor_store.hpp"

#include <memory>
#include <utility>

#include "data/tensor_access.hpp"

namespace ps::data_internal {
Status ResultTensorBacking::initialize(const ResourceBudget& budget,
                                       const ResultTensorSpec& spec,
                                       std::uint64_t tile_extent) {
  affine_owners =
      ResourceVector<ResultRef>(ResourceAllocator<ResultRef>(budget));
  TensorAccess::edit(tensor) =
      ResourceVector<TensorSpan>(ResourceAllocator<TensorSpan>(budget));
  affine_growth = ResourceVector<std::shared_ptr<void>>(
      ResourceAllocator<std::shared_ptr<void>>(budget));
  auto initial_coverage = Footprint::none(spec.sample_shape());
  if (!initial_coverage.ok())
    return initial_coverage.status();
  TensorAccess::describe(tensor,
                         {spec.descriptor.element_type, spec.sample_shape()},
                         initial_coverage.take_value());
  this->tile_extent = tile_extent;
  return Status::success();
}
}  // namespace ps::data_internal
