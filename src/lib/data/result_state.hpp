#pragma once

#include <array>
#include <functional>
#include <set>
#include <utility>

#include "data/result_publication_state.hpp"
#include "data/result_tensor_store.hpp"

namespace ps {
struct ResultRef::Impl : data_internal::ResultPublicationState {
  using StorageOwners = std::set<const void*, std::less<const void*>,
                                 ResourceAllocator<const void*>>;
  explicit Impl(ResourceBudget root)
      : ResultPublicationState(root),
        storage_owners(std::less<const void*>{},
                       ResourceAllocator<const void*>(root)) {}
  // Non-owning identities remain valid while tensor fragments retain storage.
  // Publication merges preallocated nodes only at its commit barrier.
  StorageOwners storage_owners;
  std::array<data_internal::ResultTensorBacking, 16> tensors;
};
struct ResultRef::Capture {
  ResourceLease lease;
  ResultDescriptor descriptor;
};
}  // namespace ps
