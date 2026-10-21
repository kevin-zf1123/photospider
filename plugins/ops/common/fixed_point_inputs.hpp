#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/fixed_tensor.hpp"
#include "photospider/plugin/fixed_invocation_api.h"
#include "photospider/plugin/tile_geometry.hpp"

namespace ps::plugin_internal::fixed_tensor {
// Fixed pointwise kernels read each input through its own logical tile grid.
// Batch and atomic axes may differ even when logical shapes are identical.
template <std::size_t Ports>
struct PointInputs final {
  std::array<TileGrid, Ports> grids;
  std::array<std::vector<std::pair<TileIndex, std::uint32_t>>, Ports> slots;
};
inline Result<OperationProgram> lower_pointwise(const OperationLowering& query,
                                                const std::string& symbol,
                                                const std::string& contract,
                                                std::uint64_t scratch) {
  OperationProgramBuilder builder(query);
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Result<OperationProgram>(output.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  std::vector<TileGrid> inputs;
  for (const auto& input : query.inputs) {
    auto grid = TileGrid::from_schema(*input.result_schema, query.tile_extent);
    if (!grid.ok())
      return Result<OperationProgram>(grid.status());
    inputs.push_back(grid.take_value());
  }
  for (const auto& member : query.requests[0].members) {
    for (auto tile : member.second) {
      auto task = builder.add_task(symbol, contract, {}, {scratch, 0, 0});
      if (!task.ok())
        return Result<OperationProgram>(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Result<OperationProgram>(region.status());
      for (unsigned port = 0; port < query.inputs.size(); ++port) {
        auto reads =
            sdk::tiles_for_region(budget, inputs[port], region.value());
        if (!reads.ok())
          return Result<OperationProgram>(reads.status());
        for (auto index : reads.value()) {
          auto status = budget.consume({1});
          if (status.ok())
            status = builder.read_input(task.value(), port, 0, index);
          if (!status.ok())
            return Result<OperationProgram>(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Result<OperationProgram>(status);
    }
  }
  return builder.finish();
}
inline bool matches_region(const physical::Layout& layout,
                           const Region& region) {
  if (layout.rank != region.rank())
    return false;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    if (layout.origin[axis] != region.dimensions()[axis].offset ||
        layout.shape[axis] != region.dimensions()[axis].extent)
      return false;
  return true;
}
// Validate exact immutable read slots independently of the output task shape.
// Multi-output scan tasks and single-output pointwise tasks use the same proof.
template <std::size_t Ports>
Status prepare_input_regions(const KernelPreparation& query,
                             PointInputs<Ports>* state,
                             const std::array<Region, Ports>& input_regions) {
  const auto invalid = [] {
    return Status{ErrorCode::TypeMismatch,
                  "invalid fixed input tile signature"};
  };
  if (query.inputs.size() != Ports)
    return invalid();
  std::size_t slot = 0;
  for (unsigned port = 0; port < Ports; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return grid.status();
    state->grids[port] = grid.take_value();
    const auto& input_region = input_regions[port];
    auto tiles = sdk::tiles_for_region(ResourceBudget{}, state->grids[port],
                                       input_region);
    if (!tiles.ok())
      return tiles.status();
    if (tiles.value().size() > query.reads.size() - slot)
      return invalid();
    state->slots[port].reserve(tiles.value().size());
    for (auto index : tiles.value()) {
      const auto& read = query.reads[slot];
      auto expected = state->grids[port].region(index);
      if (!expected.ok())
        return expected.status();
      if (read.port != port || read.member || read.tile_index != index ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != query.inputs[port]
                                          .result_schema->tensors[0]
                                          .descriptor.element_type ||
          !matches_region(read.layout, expected.value()))
        return invalid();
      state->slots[port].push_back({index, static_cast<std::uint32_t>(slot++)});
    }
  }
  return slot == query.reads.size() ? Status::success() : invalid();
}
template <std::size_t Ports>
Status prepare_point_inputs(
    const KernelPreparation& query, PointInputs<Ports>* state,
    const std::array<Region, Ports>* input_regions = nullptr) {
  const auto invalid = [] {
    return Status{ErrorCode::TypeMismatch, "invalid pointwise tile signature"};
  };
  if (query.inputs.size() != Ports || query.outputs.size() != 1 ||
      query.writes.size() != 1)
    return invalid();
  const auto& write = query.writes[0];
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return output.status();
  auto region = output.value().region(write.tile_index);
  if (!region.ok())
    return region.status();
  const auto dtype =
      query.outputs[0].result_schema->tensors[0].descriptor.element_type;
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type != dtype ||
      !matches_region(write.layout, region.value()))
    return invalid();
  if (input_regions)
    return prepare_input_regions(query, state, *input_regions);
  std::array<Region, Ports> regions;
  regions.fill(region.value());
  return prepare_input_regions(query, state, regions);
}
template <std::size_t Ports>
struct PointReader final {
  const PointInputs<Ports>& state;
  const ps_kernel_invocation_v8& invocation;
  std::array<const ps_kernel_const_view_v8*, Ports> cached{};
  std::uint64_t bits(unsigned port, const std::array<std::uint64_t, 8>& at,
                     std::size_t width) noexcept {
    auto* view = cached[port];
    bool contained = view != nullptr;
    for (unsigned axis = 0; contained && axis < state.grids[port].rank();
         ++axis)
      contained =
          at[axis] >= view->layout->origin[axis] &&
          at[axis] - view->layout->origin[axis] < view->layout->shape[axis];
    if (!contained) {
      const auto tile = input_tile(state.grids[port], at);
      const auto& slots = state.slots[port];
      const auto found = std::lower_bound(
          slots.begin(), slots.end(), tile,
          [](const auto& item, TileIndex id) { return item.first < id; });
      // Preparation proved complete immutable tile membership.
      view = cached[port] = &invocation.inputs[found->second];
    }
    __int128 offset = view->byte_offset;
    for (unsigned axis = 0; axis < view->layout->rank; ++axis)
      offset += static_cast<__int128>(at[axis] - view->layout->origin[axis]) *
                view->layout->strides[axis];
    std::uint64_t result = 0;
    std::memcpy(&result,
                static_cast<const std::uint8_t*>(view->allocation) +
                    static_cast<std::ptrdiff_t>(offset),
                width);
    return result;
  }
};
}  // namespace ps::plugin_internal::fixed_tensor
