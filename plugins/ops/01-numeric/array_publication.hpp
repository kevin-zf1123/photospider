#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "data/dependency_metadata.hpp"
#include "photospider/data/value_fragments.hpp"

namespace ps::plugin_internal::numeric_ops {
// One bounded publication ledger shared by every output owner. Aliases retain
// each actual CpuStorage pointer (allocator and dedup identity) while the
// common guard survives coalescing. No owner holds a Value, so there is no
// cycle.
class ArrayPublication final {
  struct Owner {
    std::shared_ptr<const dependency_internal::MetadataOwner> metadata;
    std::shared_ptr<const CpuStorage> source;
  };
  std::shared_ptr<const dependency_internal::MetadataOwner> metadata_;

 public:
  ArrayPublication(std::uint64_t boxes, std::uint64_t rank) {
    const auto per_box = 4 * (sizeof(Value) + rank * 40 + sizeof(Owner)) +
                         sizeof(Region) + rank * sizeof(RegionDimension);
    dependency_internal::MetadataBytes bytes;
    bytes.add(4096);
    bytes.add(boxes, per_box);
    metadata_ = dependency_internal::metadata_owner(bytes.bytes);
  }
  Result<Value> retain(Value value) const {
    auto owner = std::make_shared<Owner>(Owner{metadata_, value.storage()});
    auto alias = std::shared_ptr<const CpuStorage>(owner, owner->source.get());
    return Value::from_storage(value.descriptor(), value.region(),
                               value.layout(), std::move(alias),
                               value.facets());
  }
  Result<ValueFragments> finish(const ValueDescriptor& descriptor,
                                const Footprint& outputs, const Value* values,
                                std::size_t count,
                                const FootprintLimits& limits) const {
    return ValueFragments::create_view(descriptor, {}, outputs, values, count,
                                       limits, metadata_);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
