#pragma once

#include <array>
#include <utility>

#include "data/result_field_store.hpp"
#include "data/result_publication_state.hpp"
#include "data/result_tensor_store.hpp"

namespace ps {
struct ResultRef::Impl : data_internal::ResultPublicationState {
  explicit Impl(ResourceBudget root)
      : ResultPublicationState(std::move(root)) {}
  std::array<data_internal::ResultFieldBacking, 16> fields;
  std::array<data_internal::ResultTensorBacking, 16> tensors;
};
struct ResultRef::Capture {
  ResourceLease lease;
  ResultDescriptor descriptor;
  std::array<ResultRelation, 16> fields, tensors;
  ResultRelation basis;
  data_internal::ResultEvidenceOwner dependencies;
};
}  // namespace ps
