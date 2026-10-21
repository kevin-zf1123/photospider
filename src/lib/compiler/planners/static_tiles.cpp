#include "compiler/planners/static_tiles.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace ps::compiler_internal {
namespace {
using Id = physical::Id;
using Key = std::tuple<Id, std::uint32_t, TileIndex>;
Status invalid(const char* text) {
  return {ErrorCode::InvalidArgument, text};
}
Status exhausted() {
  return {ErrorCode::ResourceExhausted, "physical lowering work limit"};
}
std::uint64_t records(const physical::Program& p) {
  return std::uint64_t{p.schemas.size()} + p.layouts.size() + p.tiles.size() +
         p.values.size() + p.placements.size() + p.kernels.size() +
         p.tasks.size() + p.calls.size() + p.blocks.size();
}
bool room(const physical::Program& p, std::uint64_t added,
          const PlanningOptions& options) {
  const auto used = records(p);
  return used <= options.maximum_records &&
         added <= options.maximum_records - used;
}
Result<TileRequest> request_from_value(const physical::Program& p, Id vi) {
  TileRequest result;
  std::map<std::uint32_t, std::vector<TileIndex>> members;
  for (auto tile : p.values[vi].tiles)
    members[p.tiles[tile].member].push_back(p.tiles[tile].tile_index);
  for (const auto& member : members) {
    auto set = TileSet::from_indices(ResourceBudget{}, member.second);
    if (!set.ok())
      return Result<TileRequest>(set.status());
    result.members.emplace(member.first, set.take_value());
  }
  return Result<TileRequest>(std::move(result));
}

// Expand external dense views only after operation lowering has fixed its
// dense tile reads. Imports are local to each block activation; an edge binds
// their handles to the destination's canonical dense argument tiles.
Status import_inputs(PlanBuilder& builder, const PlanningOptions& options,
                     std::uint64_t* work) {
  auto& p = builder.program();
  auto charge = [&](std::uint64_t amount) {
    if (amount > *work)
      return false;
    *work -= amount;
    return true;
  };
  std::map<Id, physical::TileDesc> dense_tiles;
  for (auto vi : p.inputs) {
    if (!charge(1))
      return exhausted();
    if (p.values[vi].input_layout != physical::InputLayout::DenseHost)
      continue;
    for (auto ti : p.values[vi].tiles) {
      if (!*work)
        return exhausted();
      --*work;
      auto& tile = p.tiles[ti];
      dense_tiles.emplace(ti, tile);
      auto layout = p.layouts[tile.layout];
      auto shape =
          p.schemas[p.values[vi].schema]->tensors[tile.member].sample_shape();
      std::uint64_t bytes = TensorSpan::element_size(layout.element_type);
      layout.byte_offset = 0;
      for (unsigned axis = layout.rank; axis-- > 0;) {
        if (bytes > INT64_MAX || shape[axis] > UINT64_MAX / bytes ||
            layout.origin[axis] > (UINT64_MAX - layout.byte_offset) / bytes)
          return exhausted();
        layout.strides[axis] = static_cast<std::int64_t>(bytes);
        layout.byte_offset += layout.origin[axis] * bytes;
        bytes *= shape[axis];
      }
      if (!room(p, 1, options))
        return exhausted();
      tile.bytes = bytes;
      tile.layout = static_cast<Id>(p.layouts.size());
      p.layouts.push_back(layout);
    }
  }
  if (dense_tiles.empty())
    return Status::success();
  if (!room(p, 1, options))
    return exhausted();
  auto placement = static_cast<Id>(p.placements.size());
  p.placements.push_back({});
  for (Id bi = 0; bi < p.blocks.size(); ++bi) {
    if (!charge(1))
      return exhausted();
    std::map<Id, Id> imported_values, imported_tiles;
    auto imported_value = [&](Id source) -> Result<Id> {
      auto found = imported_values.find(source);
      if (found != imported_values.end())
        return Result<Id>(found->second);
      if (!charge(1) || !room(p, 1, options))
        return Result<Id>(exhausted());
      auto vi = static_cast<Id>(p.values.size());
      physical::ValueDesc value;
      value.schema = p.values[source].schema;
      value.owner = bi;
      value.kind = physical::ValueKind::Imported;
      value.import_source = source;
      p.values.push_back(std::move(value));
      imported_values.emplace(source, vi);
      return Result<Id>(vi);
    };
    auto import = [&](Id raw, Id ci, Id port) -> Result<Id> {
      auto found = imported_tiles.find(raw);
      if (found != imported_tiles.end())
        return Result<Id>(found->second);
      if (!*work || p.tasks.size() >= options.maximum_tasks)
        return Result<Id>(exhausted());
      --*work;
      auto tile = dense_tiles.at(raw);
      auto vi = imported_value(tile.value);
      if (!vi.ok())
        return vi;
      if (!room(p, 2, options))
        return Result<Id>(exhausted());
      tile.value = vi.value();
      tile.producer = static_cast<Id>(p.tasks.size());
      auto ti = static_cast<Id>(p.tiles.size());
      physical::TaskDesc copy;
      copy.kind = physical::TaskKind::Copy;
      copy.call = ci;
      copy.placement = placement;
      copy.reads.push_back({port, raw, p.tiles[raw].layout});
      copy.writes = {ti};
      p.blocks[bi].tasks.push_back(tile.producer);
      if (ci != physical::none)
        p.calls[ci].tasks.push_back(tile.producer);
      p.tasks.push_back(std::move(copy));
      p.values[tile.value].tiles.push_back(ti);
      p.tiles.push_back(tile);
      imported_tiles.emplace(raw, ti);
      return Result<Id>(ti);
    };
    for (auto ci : p.blocks[bi].calls) {
      if (!charge(1 + p.calls[ci].tasks.size()))
        return exhausted();
      const auto tasks = p.calls[ci].tasks;
      for (auto ti : tasks)
        for (std::size_t ri = 0; ri < p.tasks[ti].reads.size(); ++ri) {
          if (!charge(1))
            return exhausted();
          const auto read = p.tasks[ti].reads[ri];
          if (!dense_tiles.count(read.tile))
            continue;
          auto tile = import(read.tile, ci, read.port);
          if (!tile.ok())
            return tile.status();
          p.tasks[ti].reads[ri].tile = tile.value();
        }
    }
    auto boundary = [&](Id source, const TileRequest* selection) -> Result<Id> {
      if (!charge(1))
        return Result<Id>(exhausted());
      if (p.values[source].kind != physical::ValueKind::Input ||
          p.values[source].input_layout != physical::InputLayout::DenseHost)
        return Result<Id>(source);
      auto vi = imported_value(source);
      if (!vi.ok() || (selection && selection->empty()))
        return vi;
      // Charge the scan before allocating its stable snapshot. Adding imported
      // values can reallocate p.values, so references into it cannot survive.
      if (!charge(p.values[source].tiles.size()))
        return Result<Id>(exhausted());
      const auto tiles = p.values[source].tiles;
      for (auto raw : tiles) {
        if (selection) {
          auto member = selection->members.find(p.tiles[raw].member);
          if (member == selection->members.end() ||
              !member->second.contains(p.tiles[raw].tile_index))
            continue;
        }
        auto tile = import(raw, physical::none, 0);
        if (!tile.ok())
          return tile;
      }
      return vi;
    };
    auto& block = p.blocks[bi];
    if (block.condition != physical::none) {
      auto vi = boundary(block.condition, nullptr);
      if (!vi.ok())
        return vi.status();
      block.condition = vi.value();
    }
    for (auto& edge : block.edges)
      for (auto& argument : edge.arguments) {
        auto vi = boundary(argument, nullptr);
        if (!vi.ok())
          return vi.status();
        argument = vi.value();
      }
    for (std::size_t i = 0; i < block.return_values.size(); ++i) {
      TileRequest selection;
      if (!charge(builder.outputs().size()))
        return exhausted();
      for (const auto& output : builder.outputs())
        if (output.second == i)
          selection = builder.output_requests().at(output.first);
      auto vi = boundary(block.return_values[i], &selection);
      if (!vi.ok())
        return vi.status();
      block.return_values[i] = vi.value();
    }
  }
  return Status::success();
}

Status expand(PlanBuilder& builder, const PlanningOptions& options) {
  auto& p = builder.program();
  if (!room(p, 0, options))
    return exhausted();
  const auto value_count = p.values.size();
  const auto call_count = p.calls.size();
  std::vector<std::vector<TileGrid>> grids;
  for (const auto& value : p.values) {
    auto members = TileGrid::members(*p.schemas[value.schema], p.tile_extent);
    if (!members.ok())
      return members.status();
    grids.push_back(members.take_value());
  }
  physical::ValidationLimits limits{options.maximum_records,
                                    options.maximum_dependency_edges,
                                    options.maximum_work};
  auto local_limits = limits;
  local_limits.maximum_records = std::max(
      limits.maximum_records, physical::ValidationLimits{}.maximum_records);
  std::uint64_t work = options.maximum_work;
  auto charge = [&](std::uint64_t amount) {
    if (amount > work)
      return false;
    work -= amount;
    return true;
  };
  // Track the final merged records while retaining per-call lowerings. Local
  // input tiles are references to one global value, so only their copied
  // layouts contribute here. Incoming global tiles are counted by root().
  std::uint64_t projected_records = records(p);
  std::vector<std::uint64_t> contributions(call_count);
  std::vector<TileRequest> requests(value_count);
  std::vector<std::vector<Id>> incoming(value_count);
  for (const auto& block : p.blocks)
    for (const auto& edge : block.edges)
      for (std::size_t i = 0; i < edge.arguments.size(); ++i)
        incoming[p.blocks[edge.target].arguments[i]].push_back(
            edge.arguments[i]);
  std::deque<Id> values, calls;
  std::vector<bool> value_queued(value_count), call_queued(call_count);
  std::vector<std::optional<OperationProgram>> lowered(call_count);
  auto enqueue_call = [&](Id ci) {
    // A larger request may lower to a smaller, different graph. Discard the
    // obsolete contribution before admitting any newly discovered inputs.
    if (lowered[ci]) {
      projected_records -= contributions[ci];
      contributions[ci] = 0;
      lowered[ci].reset();
    }
    if (!call_queued[ci]) {
      call_queued[ci] = true;
      calls.push_back(ci);
    }
  };
  auto root = [&](Id vi, const TileRequest& request) -> Status {
    if (!charge(1 + request.interval_count()))
      return exhausted();
    auto united = requests[vi].unite(request, ResourceBudget{});
    if (!united.ok())
      return united.status();
    if (!(united.value() == requests[vi])) {
      if (p.values[vi].kind != physical::ValueKind::Produced) {
        const auto added = united.value().size() - requests[vi].size();
        if (added > (options.maximum_records - projected_records) / 2)
          return exhausted();
        projected_records += 2 * added;
      }
      requests[vi] = united.take_value();
      if (p.values[vi].kind == physical::ValueKind::Produced)
        enqueue_call(p.values[vi].owner);
      if (!value_queued[vi]) {
        value_queued[vi] = true;
        values.push_back(vi);
      }
    }
    return Status::success();
  };
  auto all = [&](Id vi) -> Status {
    auto request = TileRequest::all(grids[vi], ResourceBudget{});
    return request.ok() ? root(vi, request.value()) : request.status();
  };
  for (const auto& block : p.blocks) {
    if (block.condition != physical::none) {
      auto status = all(block.condition);
      if (!status.ok())
        return status;
    }
    for (const auto& edge : block.edges)
      for (auto vi : edge.arguments) {
        auto status = all(vi);
        if (!status.ok())
          return status;
      }
    if (block.end == physical::EndKind::Return) {
      for (const auto& output : builder.outputs()) {
        auto status = root(block.return_values[output.second],
                           builder.output_requests().at(output.first));
        if (!status.ok())
          return status;
      }
    }
  }
  for (Id ci = 0; ci < call_count; ++ci)
    if (!builder.calls()[ci].prepared->traits().side_effect_free)
      enqueue_call(ci);
  while (!values.empty() || !calls.empty()) {
    while (!values.empty()) {
      auto vi = values.front();
      values.pop_front();
      value_queued[vi] = false;
      if (!charge(1))
        return exhausted();
      if (p.values[vi].kind == physical::ValueKind::Produced) {
        enqueue_call(p.values[vi].owner);
      } else if (p.values[vi].kind == physical::ValueKind::Argument) {
        for (auto source : incoming[vi]) {
          auto status = root(source, requests[vi]);
          if (!status.ok())
            return status;
        }
      }
    }
    if (calls.empty())
      break;
    auto ci = calls.front();
    calls.pop_front();
    call_queued[ci] = false;
    if (!charge(1))
      return exhausted();
    std::vector<TileRequest> selected;
    for (auto vi : builder.calls()[ci].outputs)
      selected.push_back(requests[vi]);
    const auto& prepared = builder.calls()[ci].prepared;
    // Backend choice is static. A native graph can contain CPU-only calls;
    // unavailable GPU preparation never retries a CPU implementation.
    const auto backend = options.execution_mode == ExecutionMode::NativeGpu &&
                                 prepared->traits().supports_gpu
                             ? physical::Backend::Gpu
                             : physical::Backend::Cpu;
    auto graph =
        prepared->lower(selected, p.tile_extent, backend, local_limits);
    if (!graph.ok())
      return graph.status();
    const auto& local = graph.value().program;
    if (local.tasks.size() > options.maximum_tasks)
      return {ErrorCode::ResourceExhausted, "physical task limit"};
    if (!charge(local.tasks.size()) || !charge(local.tiles.size()))
      return exhausted();
    for (std::size_t port = 0; port < local.inputs.size(); ++port) {
      auto request = request_from_value(local, local.inputs[port]);
      if (!request.ok())
        return request.status();
      auto status = root(builder.calls()[ci].inputs[port], request.value());
      if (!status.ok())
        return status;
    }
    std::uint64_t contribution = local.layouts.size() +
                                 local.placements.size() +
                                 local.kernels.size() + local.tasks.size();
    for (const auto& tile : local.tiles)
      contribution += tile.producer != physical::none;
    contribution += 2 * (local.values.size() - local.inputs.size() -
                         graph.value().outputs.size());
    const auto other = projected_records - contributions[ci];
    if (contribution > options.maximum_records - other)
      return exhausted();
    projected_records = other + contribution;
    contributions[ci] = contribution;
    lowered[ci] = graph.take_value();
  }

  std::map<Key, Id> tiles;
  // Lower operations against canonical dense tiles. A separate compiler pass
  // expands requested external layouts into raw views and explicit COPYs.
  for (Id vi = 0; vi < value_count; ++vi) {
    if (p.values[vi].kind == physical::ValueKind::Produced)
      continue;
    if (requests[vi].size() > limits.maximum_records)
      return exhausted();
    for (const auto& member : requests[vi].members)
      for (auto index : member.second) {
        if (!charge(1))
          return exhausted();
        auto region = grids[vi][member.first].region(index);
        if (!region.ok())
          return region.status();
        physical::Layout layout;
        layout.element_type = p.schemas[p.values[vi].schema]
                                  ->tensors[member.first]
                                  .descriptor.element_type;
        layout.rank = static_cast<std::uint32_t>(region.value().rank());
        layout.strides.fill(0);
        std::uint64_t bytes = TensorSpan::element_size(layout.element_type);
        auto alignment = bytes;
        for (unsigned axis = layout.rank; axis-- > 0;) {
          const auto d = region.value().dimensions()[axis];
          if (bytes > INT64_MAX || d.extent > UINT64_MAX / bytes)
            return exhausted();
          layout.strides[axis] = static_cast<std::int64_t>(bytes);
          layout.origin[axis] = d.offset;
          layout.shape[axis] = d.extent;
          bytes *= d.extent;
        }
        physical::TileDesc tile;
        tile.value = vi;
        if (p.values[vi].kind == physical::ValueKind::Input) {
          const auto& declaration =
              builder.input_declarations()[p.values[vi].owner];
          auto domain = options.input_domains.find(declaration.name);
          if (domain != options.input_domains.end())
            tile.domain = domain->second;
        }
        tile.member = member.first;
        tile.tile_index = index;
        tile.bytes = bytes;
        tile.alignment = alignment;
        tile.layout = static_cast<Id>(p.layouts.size());
        auto ti = static_cast<Id>(p.tiles.size());
        p.layouts.push_back(layout);
        p.tiles.push_back(tile);
        p.values[vi].tiles.push_back(ti);
        tiles.emplace(Key{vi, member.first, index}, ti);
      }
  }

  struct Remap {
    std::vector<Id> values, tasks, layouts, placements, kernels;
  };
  std::vector<Remap> remaps(call_count);
  for (Id ci = 0; ci < call_count; ++ci) {
    if (!lowered[ci])
      continue;
    const auto& graph = *lowered[ci];
    const auto& local = graph.program;
    auto& map = remaps[ci];
    map.values.assign(local.values.size(), physical::none);
    for (std::size_t i = 0; i < local.inputs.size(); ++i)
      map.values[local.inputs[i]] = builder.calls()[ci].inputs[i];
    for (std::size_t i = 0; i < graph.outputs.size(); ++i)
      map.values[graph.outputs[i]] = builder.calls()[ci].outputs[i];
    for (Id vi = 0; vi < local.values.size(); ++vi) {
      if (map.values[vi] != physical::none)
        continue;
      auto v = local.values[vi];
      if (v.kind != physical::ValueKind::Produced)
        return invalid("unexpected private incoming value");
      v.owner = ci;
      v.schema = static_cast<Id>(p.schemas.size());
      v.source_value = 0;
      v.tiles.clear();
      map.values[vi] = static_cast<Id>(p.values.size());
      p.schemas.push_back(local.schemas[local.values[vi].schema]);
      p.values.push_back(std::move(v));
    }
    for (const auto& layout : local.layouts) {
      map.layouts.push_back(static_cast<Id>(p.layouts.size()));
      p.layouts.push_back(layout);
    }
    for (const auto& placement : local.placements) {
      map.placements.push_back(static_cast<Id>(p.placements.size()));
      p.placements.push_back(placement);
    }
    for (const auto& kernel : local.kernels) {
      map.kernels.push_back(static_cast<Id>(p.kernels.size()));
      p.kernels.push_back(kernel);
    }
    for (const auto& task : local.tasks) {
      if (p.tasks.size() >= options.maximum_tasks)
        return {ErrorCode::ResourceExhausted, "physical task limit"};
      auto ti = static_cast<Id>(p.tasks.size());
      map.tasks.push_back(ti);
      p.tasks.push_back(task);
      p.calls[ci].tasks.push_back(ti);
      p.blocks[p.calls[ci].block].tasks.push_back(ti);
    }
    if (local.calls[0].effect_task != physical::none)
      p.calls[ci].effect_task = map.tasks[local.calls[0].effect_task];
    for (auto exported : local.calls[0].returns)
      p.calls[ci].returns.push_back(map.values[exported]);
    for (const auto& source : local.tiles) {
      if (source.producer == physical::none)
        continue;
      auto tile = source;
      tile.value = map.values[source.value];
      tile.layout = map.layouts[source.layout];
      tile.producer = map.tasks[source.producer];
      const auto ti = static_cast<Id>(p.tiles.size());
      if (!tiles.emplace(Key{tile.value, tile.member, tile.tile_index}, ti)
               .second)
        return invalid("multiple physical tile producers during merge");
      p.values[tile.value].tiles.push_back(ti);
      p.tiles.push_back(tile);
    }
    if (!charge(local.values.size() + local.tasks.size() + local.tiles.size()))
      return exhausted();
  }
  // CFG arguments rename existing allocations. Resolve their domain from
  // incoming producers before checking operation reads, including loop edges.
  std::vector<std::vector<Id>> domain_users(p.tiles.size());
  std::vector<bool> known_domain(p.tiles.size());
  std::deque<Id> domain_ready;
  for (Id id = 0; id < p.tiles.size(); ++id)
    if (p.values[p.tiles[id].value].kind != physical::ValueKind::Argument) {
      known_domain[id] = true;
      domain_ready.push_back(id);
    }
  for (Id vi = 0; vi < value_count; ++vi) {
    if (p.values[vi].kind == physical::ValueKind::Argument) {
      for (auto id : p.values[vi].tiles)
        for (auto source : incoming[vi]) {
          if (!charge(1))
            return exhausted();
          const auto& tile = p.tiles[id];
          auto found = tiles.find({source, tile.member, tile.tile_index});
          if (found == tiles.end())
            return invalid("missing incoming argument tile");
          domain_users[found->second].push_back(id);
        }
    }
  }
  while (!domain_ready.empty()) {
    const auto source = domain_ready.front();
    domain_ready.pop_front();
    for (auto target : domain_users[source]) {
      if (!charge(1))
        return exhausted();
      if (known_domain[target]) {
        if (p.tiles[target].domain != p.tiles[source].domain)
          return invalid("block argument domain requires an explicit COPY");
      } else {
        known_domain[target] = true;
        p.tiles[target].domain = p.tiles[source].domain;
        domain_ready.push_back(target);
      }
    }
  }
  for (bool known : known_domain)
    if (!known)
      return invalid("unresolved block argument domain");
  for (Id ci = 0; ci < call_count; ++ci) {
    if (!lowered[ci])
      continue;
    const auto& local = lowered[ci]->program;
    const auto& map = remaps[ci];
    std::vector<Id> tile_map;
    for (const auto& tile : local.tiles) {
      auto found =
          tiles.find({map.values[tile.value], tile.member, tile.tile_index});
      if (found == tiles.end())
        return invalid("missing compiled producer tile");
      const auto& actual = p.tiles[found->second];
      const auto& actual_layout = p.layouts[actual.layout];
      const auto& required_layout = local.layouts[tile.layout];
      if (actual.domain != tile.domain)
        return invalid(
            "cross-domain operation input requires an explicit COPY");
      if (actual.bytes < tile.bytes || actual.alignment < tile.alignment ||
          actual_layout.element_type != required_layout.element_type ||
          actual_layout.rank != required_layout.rank ||
          actual_layout.byte_offset != required_layout.byte_offset ||
          actual_layout.strides != required_layout.strides ||
          actual_layout.origin != required_layout.origin ||
          actual_layout.shape != required_layout.shape)
        return invalid("producer allocation does not satisfy operation input");
      tile_map.push_back(found->second);
    }
    for (Id i = 0; i < local.tasks.size(); ++i) {
      auto& task = p.tasks[map.tasks[i]];
      task.call = ci;
      task.placement = map.placements[task.placement];
      if (task.kernel != physical::none)
        task.kernel = map.kernels[task.kernel];
      for (auto& read : task.reads) {
        read.tile = tile_map[read.tile];
        read.layout = map.layouts[read.layout];
      }
      for (auto& write : task.writes)
        write = tile_map[write];
      for (auto& vi : task.schema_reads)
        vi = map.values[vi];
      for (auto& dependency : task.explicit_dependencies)
        dependency = map.tasks[dependency];
      task.predecessors.clear();
      task.successors.clear();
    }
  }
  auto imported = import_inputs(builder, options, &work);
  if (!imported.ok())
    return imported;
  for (auto& value : p.values)
    std::sort(value.tiles.begin(), value.tiles.end(), [&](auto a, auto b) {
      return std::tie(p.tiles[a].member, p.tiles[a].tile_index) <
             std::tie(p.tiles[b].member, p.tiles[b].tile_index);
    });
  return physical::finalize_program(&p, limits);
}
}  // namespace

PlannerDefinition static_tile_planner() {
  return {
      "static-tiles", "2",
      "Fixed task graph expansion with ordered call retirement",
      [](const ValidatedIR&, PlanBuilder& builder,
         const PlanningOptions& options) { return expand(builder, options); }};
}
}  // namespace ps::compiler_internal
