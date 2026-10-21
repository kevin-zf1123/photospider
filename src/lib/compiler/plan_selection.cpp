#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "compiler/identity.hpp"
#include "photospider/compiler/compiler.hpp"

namespace ps {
Status Compiler::select_plan(ExecutionPlan* plan) const {
  using Id = physical::Id;
  auto& p = plan->edit().program_;
  std::map<std::tuple<Id, std::uint32_t, TileIndex>, Id> locations;
  for (Id ti = 0; ti < p.tiles.size(); ++ti)
    locations.emplace(std::make_tuple(p.tiles[ti].value, p.tiles[ti].member,
                                      p.tiles[ti].tile_index),
                      ti);
  std::vector<bool> tasks(p.tasks.size()), tiles(p.tiles.size());
  std::vector<Id> task_queue, tile_queue;
  auto keep_task = [&](Id ti) {
    if (!tasks[ti]) {
      tasks[ti] = true;
      task_queue.push_back(ti);
    }
  };
  auto keep_tile = [&](Id ti) {
    if (!tiles[ti]) {
      tiles[ti] = true;
      tile_queue.push_back(ti);
    }
  };
  auto keep_value = [&](Id vi) {
    for (auto ti : p.values[vi].tiles)
      keep_tile(ti);
  };
  std::vector<std::vector<Id>> incoming(p.values.size());
  for (const auto& block : p.blocks) {
    if (block.condition != physical::none)
      keep_value(block.condition);
    for (const auto& edge : block.edges)
      for (std::size_t i = 0; i < edge.arguments.size(); ++i) {
        incoming[p.blocks[edge.target].arguments[i]].push_back(
            edge.arguments[i]);
        keep_value(edge.arguments[i]);
      }
    if (block.end == physical::EndKind::Return) {
      for (const auto& output : plan->edit().outputs_)
        for (const auto& member :
             plan->edit().requests_.at(output.first).members)
          for (auto index : member.second) {
            auto found = locations.find(
                {block.return_values[output.second], member.first, index});
            if (found == locations.end())
              return {ErrorCode::InvalidArgument,
                      "selected tile is not compiled"};
            keep_tile(found->second);
          }
    }
  }
  for (const auto& call : p.calls) {
    if (call.effect_task != physical::none)
      keep_task(call.effect_task);
    for (auto vi : call.returns)
      keep_value(vi);
  }
  while (!task_queue.empty() || !tile_queue.empty()) {
    while (!tile_queue.empty()) {
      auto ti = tile_queue.back();
      tile_queue.pop_back();
      const auto& tile = p.tiles[ti];
      if (tile.producer != physical::none)
        keep_task(tile.producer);
      for (auto source : incoming[tile.value]) {
        auto found = locations.find({source, tile.member, tile.tile_index});
        if (found == locations.end())
          return {ErrorCode::InvalidArgument, "missing incoming tile"};
        keep_tile(found->second);
      }
    }
    if (task_queue.empty())
      break;
    auto ti = task_queue.back();
    task_queue.pop_back();
    const auto& task = p.tasks[ti];
    for (auto predecessor : task.predecessors)
      keep_task(predecessor);
    for (const auto& read : task.reads)
      keep_tile(read.tile);
    for (auto tile : task.writes)
      keep_tile(tile);
  }
  std::vector<Id> task_map(p.tasks.size(), physical::none),
      tile_map(p.tiles.size(), physical::none);
  std::vector<physical::TaskDesc> selected_tasks;
  std::vector<physical::TileDesc> selected_tiles;
  for (Id ti = 0; ti < p.tasks.size(); ++ti)
    if (tasks[ti]) {
      task_map[ti] = static_cast<Id>(selected_tasks.size());
      selected_tasks.push_back(std::move(p.tasks[ti]));
    }
  for (Id ti = 0; ti < p.tiles.size(); ++ti)
    if (tiles[ti]) {
      tile_map[ti] = static_cast<Id>(selected_tiles.size());
      selected_tiles.push_back(p.tiles[ti]);
    }
  auto filter = [](std::vector<Id>* ids, const std::vector<Id>& mapping) {
    std::vector<Id> result;
    for (auto id : *ids)
      if (mapping[id] != physical::none)
        result.push_back(mapping[id]);
    *ids = std::move(result);
  };
  for (auto& task : selected_tasks) {
    for (auto& read : task.reads)
      read.tile = tile_map[read.tile];
    for (auto& write : task.writes)
      write = tile_map[write];
    for (auto& predecessor : task.explicit_dependencies)
      predecessor = task_map[predecessor];
    task.predecessors.clear();
    task.successors.clear();
  }
  for (auto& tile : selected_tiles)
    if (tile.producer != physical::none)
      tile.producer = task_map[tile.producer];
  for (auto& value : p.values)
    filter(&value.tiles, tile_map);
  for (auto& call : p.calls) {
    filter(&call.tasks, task_map);
    if (call.effect_task != physical::none)
      call.effect_task = task_map[call.effect_task];
  }
  for (auto& block : p.blocks)
    filter(&block.tasks, task_map);
  p.tasks = std::move(selected_tasks);
  p.tiles = std::move(selected_tiles);
  return finalize_plan(plan);
}

Result<ExecutionPlan> ExecutionPlan::select_outputs(
    const std::map<std::string, TileRequest>& requests) const {
  using Answer = Result<ExecutionPlan>;
  auto operations = data().operation_registry_.lock();
  if (!current() || !operations)
    return Answer({ErrorCode::Stale, "physical parent plan is stale"});
  try {
    ExecutionPlan result = *this;
    result.edit().outputs_.clear();
    result.edit().requests_.clear();
    for (const auto& requested : requests) {
      auto found = data().outputs_.find(requested.first);
      if (found == data().outputs_.end())
        return Answer({ErrorCode::InvalidArgument, "unknown output"});
      auto normalized =
          requested.second.resolve(output(requested.first).tile_grids);
      if (!normalized.ok())
        return Answer(normalized.status());
      if (!data().requests_.at(requested.first).contains(normalized.value()))
        return Answer({ErrorCode::InvalidArgument,
                       "output request exceeds compiled coverage"});
      result.edit().outputs_.emplace(*found);
      result.edit().requests_.emplace(requested.first, normalized.take_value());
    }
    Compiler compiler(std::move(operations));
    auto status = compiler.select_plan(&result);
    if (!status.ok())
      return Answer(status);
    result.edit().digest_.value = compiler_internal::physical_digest(result);
    if (!current())
      return Answer({ErrorCode::Stale, "source changed during selection"});
    return Answer(std::move(result));
  } catch (const std::bad_alloc&) {
    return Answer(
        {ErrorCode::ResourceExhausted, "physical selection allocation failed"});
  }
}
}  // namespace ps
