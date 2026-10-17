#include "execution/tile_binding_changes.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace ps::execution_internal {
namespace {
struct Comparison {
  const ResourceBudget& root;
  const SnapshotAccessOptions& options;
  std::uint64_t remaining;
  Status work(std::uint64_t count) const {
    return options.cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                            : root.consume({count});
  }
  Status sample(std::uint64_t bytes) {
    auto stopped = work(0);
    if (!stopped.ok())
      return stopped;
    if (!remaining)
      return {ErrorCode::ResourceExhausted, "binding comparison sample limit"};
    --remaining;
    return work(bytes);
  }
  Result<bool> tensor(const ResultRef& a, const ResultDescriptor& left,
                      const ResultRef& b, const ResultDescriptor& right,
                      std::uint32_t slot, const Region& region) {
    using Answer = Result<bool>;
    const auto width = TensorSpan::element_size(
        a.schema().tensors[slot].descriptor.element_type);
    auto admitted = root.reserve(
        ResourceCapacity::host(region.rank() * sizeof(std::uint64_t),
                               region.rank() * sizeof(std::uint64_t)));
    if (!admitted.ok())
      return Answer(admitted.status());
    auto lease = admitted.take_value();
    std::vector<std::uint64_t> at;
    at.reserve(region.rank());
    for (const auto dimension : region.dimensions())
      at.push_back(dimension.offset);
    for (;;) {
      auto status = sample(2 * width);
      if (!status.ok())
        return Answer(status);
      std::array<std::uint8_t, 8> first{}, second{};
      status = a.read_tensor(left, slot, at, first.data(), width,
                             options.cancellation);
      if (status.ok())
        status = b.read_tensor(right, slot, at, second.data(), width,
                               options.cancellation);
      if (!status.ok())
        return Answer(status);
      if (std::memcmp(first.data(), second.data(), width))
        return Answer(true);
      std::size_t axis = at.size();
      while (axis) {
        --axis;
        const auto dimension = region.dimensions()[axis];
        if (++at[axis] < dimension.offset + dimension.extent)
          break;
        at[axis] = dimension.offset;
      }
      if (!axis && at[0] == region.dimensions()[0].offset)
        return Answer(false);
    }
  }
  Result<bool> whole(const ResultRef& a, const ResultDescriptor& left,
                     const ResultRef& b, const ResultDescriptor& right) {
    using Answer = Result<bool>;

    for (std::uint32_t slot = 0; slot < a.schema().tensors.size(); ++slot) {
      auto changed =
          tensor(a, left, b, right, slot,
                 Region::whole(a.schema().tensors[slot].sample_shape()));
      if (!changed.ok() || changed.value())
        return changed;
    }
    return Answer(false);
  }
};
Result<const ResultRef*> binding(const ExecutionBindings& bindings,
                                 std::string_view name,
                                 const Comparison& compare) {
  const ResultRef* result = nullptr;
  for (const auto& item : bindings.inputs) {
    auto status = compare.work(1 + std::min(item.name.size(), name.size()));
    if (!status.ok())
      return Result<const ResultRef*>(status);
    if (item.name != name)
      continue;
    if (result)
      return Result<const ResultRef*>(
          Status{ErrorCode::InvalidArgument, "duplicate source binding"});
    result = &item.result;
  }
  return Result<const ResultRef*>(result);
}
}  // namespace
Result<ResourceVector<TileChange>> tile_binding_changes(
    const ExecutionPlan& plan, const ExecutionBindings& before,
    const ExecutionBindings& after,
    const ResourceVector<TileDependency>& observed, const ResourceBudget& root,
    const SnapshotAccessOptions& options) try {
  using Answer = Result<ResourceVector<TileChange>>;
  ResourceAllocationScope allocation(root);
  Comparison compare{root, options, options.maximum_samples};
  auto status = compare.work(observed.size() + 1);
  if (!status.ok())
    return Answer(status);
  TileRecord sources(root);
  for (const auto& dependency : observed) {
    if (!std::holds_alternative<PlanWorkflowInput>(dependency.producer))
      return Answer(
          Status{ErrorCode::InvalidArgument, "expected workflow input"});
    const auto input =
        std::get<PlanWorkflowInput>(dependency.producer).declaration_index;
    if (input >= plan.input_declarations().size() || dependency.producer_output)
      return Answer(
          Status{ErrorCode::InvalidArgument, "invalid source dependency"});
    const auto& declaration = plan.input_declarations()[input];
    if (!declaration.result_schema)
      return Answer(
          Status{ErrorCode::InvalidArgument, "missing source schema"});
    status = compare.work(dependency.tiles.intervals().size() + 1);
    if (!status.ok())
      return Answer(status);
    auto grid = TileGrid::from_schema(
        *declaration.result_schema, plan.tile_extent(), dependency.tensor_slot);
    if (!grid.ok())
      return Answer(grid.status());
    if (!dependency.tiles.empty() &&
        *(dependency.tiles.end() - 1) >= grid.value().tile_count())
      return Answer(Status{ErrorCode::InvalidArgument, "invalid source tiles"});
    status = sources.include(dependency, root);
    if (!status.ok())
      return Answer(status);
  }
  ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
  for (const auto& source : sources.inputs) {
    const auto input =
        std::get<PlanWorkflowInput>(source.producer).declaration_index;
    if (input >= plan.input_declarations().size())
      return Answer(Status{ErrorCode::InvalidArgument, "invalid source index"});
    const auto& declaration = plan.input_declarations()[input];
    status = compare.work(before.inputs.size() + after.inputs.size() +
                          declaration.name.size());
    if (!status.ok())
      return Answer(status);
    auto first = binding(before, declaration.name, compare);
    auto second = binding(after, declaration.name, compare);
    if (!first.ok() || !second.ok())
      return Answer(!first.ok() ? first.status() : second.status());
    const auto* a = first.value();
    const auto* b = second.value();
    if (!a || !b || !a->valid() || !b->valid() || !declaration.result_schema)
      return Answer(
          Status{ErrorCode::InvalidArgument, "missing source binding"});
    const auto& schema = *declaration.result_schema;
    status = compare.work(2 * schema.canonical_size());
    if (!status.ok())
      return Answer(status);
    if (!schema.same_schema(a->schema()) || !schema.same_schema(b->schema()))
      return Answer(Status{ErrorCode::TypeMismatch, "source schema changed"});
    auto grid =
        TileGrid::from_schema(schema, plan.tile_extent(), source.tensor_slot);
    if (!grid.ok())
      return Answer(grid.status());
    if (!source.tiles.empty() &&
        *(source.tiles.end() - 1) >= grid.value().tile_count())
      return Answer(Status{ErrorCode::InvalidArgument, "invalid source tiles"});
    auto left = a->descriptor(), right = b->descriptor();
    if (!left.ok() || !right.ok())
      return Answer(!left.ok() ? left.status() : right.status());
    status = compare.work(1);
    if (!status.ok())
      return Answer(status);
    TileChange change{source.producer, 0, source.tensor_slot, {}, false};
    ResourceVector<TileIndex> changed{ResourceAllocator<TileIndex>(root)};
    if (a->object_id() != b->object_id() ||
        left.value().revision() != right.value().revision()) {
      for (const auto tile : source.tiles) {
        status = compare.work(1);
        if (!status.ok())
          return Answer(status);
        auto region = grid.value().region(tile);
        if (!region.ok())
          return Answer(region.status());
        auto different = compare.tensor(*a, left.value(), *b, right.value(),
                                        source.tensor_slot, region.value());
        if (!different.ok())
          return Answer(different.status());
        if (different.value())
          changed.push_back(tile);
      }
    }
    auto tiles = TileSet::from_indices(root, changed.data(), changed.size());
    if (!tiles.ok())
      return Answer(tiles.status());
    change.tiles = tiles.take_value();
    if (change.descriptor_changed || !change.tiles.empty())
      changes.push_back(std::move(change));
  }
  return Answer(std::move(changes));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<TileChange>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
}  // namespace ps::execution_internal
