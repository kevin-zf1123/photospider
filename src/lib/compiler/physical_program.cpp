#include "photospider/compiler/physical_program.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "photospider/data/tiles.hpp"

namespace ps::physical {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}
Status exhausted() {
  return {ErrorCode::ResourceExhausted, "physical program validation limit"};
}
bool add(std::uint64_t a, std::uint64_t b, std::uint64_t* result) {
  if (b > UINT64_MAX - a)
    return false;
  *result = a + b;
  return true;
}
bool multiply(std::uint64_t a, std::uint64_t b, std::uint64_t* result) {
  if (a && b > UINT64_MAX / a)
    return false;
  *result = a * b;
  return true;
}
bool power_of_two(std::uint64_t value) {
  return value && !(value & (value - 1));
}
std::uint64_t scalar_bytes(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
    case ElementType::Int8:
      return 1;
    case ElementType::UInt16:
    case ElementType::Int16:
      return 2;
    case ElementType::Float32:
      return 4;
    case ElementType::Int64:
    case ElementType::Float64:
      return 8;
  }
  return 0;
}
void canonicalize(std::vector<Id>* ids) {
  std::sort(ids->begin(), ids->end());
  ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
}
bool canonical(const std::vector<Id>& ids) {
  return std::is_sorted(ids.begin(), ids.end()) &&
         std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

struct Derived {
  std::vector<std::vector<Id>> predecessors, successors, live_ins;
  std::vector<Reservation> reservations;
};

class Checker {
 public:
  Checker(const Program& program, const ValidationLimits& limits)
      : p_(program), limits_(limits), work_(limits.maximum_work) {}

  Status run() {
    auto status = bounds();
    if (status.ok())
      status = structure();
    if (status.ok())
      status = cfg();
    if (status.ok())
      status = values();
    if (status.ok())
      status = tasks();
    if (status.ok())
      status = liveness();
    return status;
  }

  Derived derived;

 private:
  bool spend(std::uint64_t amount = 1) {
    if (amount > work_) {
      work_exhausted_ = true;
      return false;
    }
    work_ -= amount;
    return true;
  }
  Status bounds() {
    if (!limits_.maximum_records || !limits_.maximum_edges || !work_)
      return exhausted();
    std::uint64_t records = 0, references = 0;
    auto record = [&](std::size_t size) {
      return size < none && add(records, size, &records) &&
             records <= limits_.maximum_records;
    };
    if (!record(p_.schemas.size()) || !record(p_.layouts.size()) ||
        !record(p_.tiles.size()) || !record(p_.values.size()) ||
        !record(p_.placements.size()) || !record(p_.kernels.size()) ||
        !record(p_.tasks.size()) || !record(p_.calls.size()) ||
        !record(p_.blocks.size()))
      return exhausted();
    auto refs = [&](std::size_t size) {
      return add(references, size, &references) &&
             references <= limits_.maximum_edges && spend(size);
    };
    if (!refs(p_.inputs.size()) || !refs(p_.return_schemas.size()))
      return exhausted();
    for (const auto& v : p_.values)
      if (!refs(v.tiles.size()))
        return exhausted();
    for (const auto& t : p_.tasks)
      if (!refs(t.reads.size()) || !refs(t.writes.size()) ||
          !refs(t.schema_reads.size()) ||
          !refs(t.explicit_dependencies.size()) ||
          !refs(t.predecessors.size()) || !refs(t.successors.size()) ||
          !spend(t.parameters.size()))
        return exhausted();
    for (const auto& c : p_.calls)
      if (!refs(c.tasks.size()) || !refs(c.returns.size()))
        return exhausted();
    for (const auto& b : p_.blocks) {
      if (!refs(b.tasks.size()) || !refs(b.calls.size()) ||
          !refs(b.arguments.size()) || !refs(b.live_ins.size()) ||
          !refs(b.return_values.size()) || !refs(b.edges.size()))
        return exhausted();
      for (const auto& e : b.edges)
        if (!refs(e.arguments.size()) || !refs(e.live_values.size()))
          return exhausted();
    }
    if (!spend(records))
      return exhausted();
    derived.predecessors.resize(p_.tasks.size());
    derived.successors.resize(p_.tasks.size());
    derived.reservations.resize(p_.blocks.size());
    derived.live_ins.resize(p_.blocks.size());
    value_blocks_.assign(p_.values.size(), none);
    task_blocks_.assign(p_.tasks.size(), none);
    return Status::success();
  }

  Status structure() {
    if (p_.blocks.empty() || p_.entry >= p_.blocks.size() ||
        !p_.blocks[p_.entry].arguments.empty() || !power_of_two(p_.tile_extent))
      return invalid("invalid physical entry or tile extent");
    for (const auto& schema : p_.schemas) {
      if (!schema)
        return invalid("null physical schema");
      auto status = schema->validate();
      if (!status.ok())
        return status;
    }
    for (auto schema : p_.return_schemas)
      if (schema >= p_.schemas.size())
        return invalid("invalid return schema");
    for (const auto& layout : p_.layouts) {
      auto status = validate_layout(layout, UINT64_MAX, false);
      if (!status.ok())
        return status;
    }
    for (const auto& placement : p_.placements)
      if (static_cast<unsigned>(placement.backend) > 1 ||
          static_cast<unsigned>(placement.domain) > 2 ||
          (placement.backend == Backend::Cpu &&
           placement.domain == Domain::Device) ||
          placement.profile.empty() || placement.profile.size() > 1024 ||
          placement.queue.size() > 1024)
        return invalid("invalid physical placement");
    for (const auto& kernel : p_.kernels)
      if ((kernel.abi_version != 8 && kernel.abi_version != 9) ||
          kernel.operation.empty() || kernel.symbol.empty() ||
          kernel.contract.empty() || kernel.operation.size() > 1024 ||
          kernel.symbol.size() > 1024 || kernel.contract.size() > 1024)
        return invalid("invalid physical kernel identity");
    std::vector<bool> calls(p_.calls.size()), tasks(p_.tasks.size());
    std::vector<bool> block_tasks(p_.tasks.size());
    std::set<std::uint64_t> source_calls, source_blocks;
    for (Id bi = 0; bi < p_.blocks.size(); ++bi) {
      const auto& block = p_.blocks[bi];
      if (!block.source_block ||
          !source_blocks.insert(block.source_block).second)
        return invalid("duplicate physical source block");
      for (Id ordinal = 0; ordinal < block.calls.size(); ++ordinal) {
        auto ci = block.calls[ordinal];
        if (ci >= p_.calls.size() || calls[ci])
          return invalid("invalid call membership");
        calls[ci] = true;
        const auto& call = p_.calls[ci];
        if (call.block != bi || call.ordinal != ordinal || !call.source_call ||
            !source_calls.insert(call.source_call).second)
          return invalid("invalid call retirement ordinal or identity");
        for (auto ti : call.tasks) {
          if (ti >= p_.tasks.size() || tasks[ti] || p_.tasks[ti].call != ci)
            return invalid("invalid task call membership");
          tasks[ti] = true;
        }
      }
      for (auto ti : block.tasks) {
        if (ti >= p_.tasks.size() || block_tasks[ti] ||
            (p_.tasks[ti].call != none &&
             (p_.tasks[ti].call >= p_.calls.size() ||
              p_.calls[p_.tasks[ti].call].block != bi)))
          return invalid("invalid task block membership");
        block_tasks[ti] = true;
        task_blocks_[ti] = bi;
        if (p_.tasks[ti].call == none) {
          if (p_.tasks[ti].kind != TaskKind::Copy)
            return invalid("boundary task must be an import COPY");
          tasks[ti] = true;
        }
      }
      auto count = block.end == EndKind::Jump     ? 1U
                   : block.end == EndKind::Branch ? 2U
                                                  : 0U;
      if (static_cast<unsigned>(block.end) > 2 || block.edges.size() != count ||
          (block.end != EndKind::Branch && block.condition != none) ||
          (block.end != EndKind::Return && !block.return_values.empty()) ||
          (block.end == EndKind::Return &&
           block.return_values.size() != p_.return_schemas.size()))
        return invalid("invalid physical terminator");
      for (const auto& edge : block.edges)
        if (edge.target >= p_.blocks.size() ||
            edge.arguments.size() != p_.blocks[edge.target].arguments.size())
          return invalid("invalid physical edge");
    }
    if (std::find(calls.begin(), calls.end(), false) != calls.end() ||
        std::find(tasks.begin(), tasks.end(), false) != tasks.end() ||
        std::find(block_tasks.begin(), block_tasks.end(), false) !=
            block_tasks.end())
      return invalid("orphan call or task");
    return Status::success();
  }

  Status cfg() {
    const auto count = p_.blocks.size();
    block_predecessors_.resize(count);
    for (Id bi = 0; bi < count; ++bi)
      for (const auto& e : p_.blocks[bi].edges)
        block_predecessors_[e.target].push_back(bi);
    std::vector<bool> visited(count);
    std::vector<std::pair<Id, std::size_t>> stack{{p_.entry, 0}};
    std::vector<Id> order;
    visited[p_.entry] = true;
    while (!stack.empty()) {
      if (!spend())
        return exhausted();
      auto& frame = stack.back();
      const auto& edges = p_.blocks[frame.first].edges;
      if (frame.second == edges.size()) {
        order.push_back(frame.first);
        stack.pop_back();
      } else {
        auto child = edges[frame.second++].target;
        if (!visited[child]) {
          visited[child] = true;
          stack.emplace_back(child, 0);
        }
      }
    }
    if (order.size() != count)
      return invalid("unreachable physical block");
    std::reverse(order.begin(), order.end());
    std::vector<Id> position(count);
    for (Id i = 0; i < count; ++i)
      position[order[i]] = i;
    dominator_.assign(count, none);
    dominator_[p_.entry] = p_.entry;
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto bi : order) {
        if (!spend())
          return exhausted();
        if (bi == p_.entry)
          continue;
        Id parent = none;
        for (auto pred : block_predecessors_[bi]) {
          if (!spend())
            return exhausted();
          if (dominator_[pred] == none)
            continue;
          if (parent == none) {
            parent = pred;
            continue;
          }
          auto other = pred;
          while (parent != other) {
            if (!spend())
              return exhausted();
            if (position[parent] > position[other])
              parent = dominator_[parent];
            else
              other = dominator_[other];
          }
        }
        if (dominator_[bi] != parent) {
          dominator_[bi] = parent;
          changed = true;
        }
      }
    }
    return Status::success();
  }

  bool dominates(Id definition, Id use) {
    if (definition == none)
      return true;
    for (;;) {
      if (!spend())
        return false;
      if (definition == use)
        return true;
      if (use == p_.entry)
        return false;
      use = dominator_[use];
    }
  }
  Status available(Id vi, Id block, Id ordinal = none) {
    if (vi >= p_.values.size())
      return invalid("invalid physical value use");
    if (!dominates(value_blocks_[vi], block))
      return work_exhausted_ ? exhausted()
                             : invalid("physical value does not dominate use");
    const auto& value = p_.values[vi];
    if (value.kind == ValueKind::Imported && value.owner != block)
      return invalid("imported values must cross blocks through arguments");
    if (value.kind == ValueKind::Produced && value_blocks_[vi] == block &&
        p_.calls[value.owner].ordinal > ordinal)
      return invalid("physical value uses a later call");
    return Status::success();
  }

  bool raw_dense_payload(Id vi) const {
    const auto& value = p_.values[vi];
    return value.kind == ValueKind::Input &&
           value.input_layout == InputLayout::DenseHost && !value.tiles.empty();
  }

  Status values() {
    std::vector<bool> owned(p_.values.size()), tiles(p_.tiles.size());
    for (Id i = 0; i < p_.inputs.size(); ++i) {
      auto vi = p_.inputs[i];
      if (vi >= p_.values.size() || owned[vi] ||
          p_.values[vi].kind != ValueKind::Input || p_.values[vi].owner != i)
        return invalid("invalid physical input value");
      owned[vi] = true;
    }
    for (Id bi = 0; bi < p_.blocks.size(); ++bi)
      for (auto vi : p_.blocks[bi].arguments) {
        if (vi >= p_.values.size() || owned[vi] ||
            p_.values[vi].kind != ValueKind::Argument ||
            p_.values[vi].owner != bi)
          return invalid("invalid block argument value");
        value_blocks_[vi] = bi;
        owned[vi] = true;
      }
    std::vector<std::vector<TileGrid>> grids;
    for (const auto& schema : p_.schemas) {
      auto members = TileGrid::members(*schema, p_.tile_extent);
      if (!members.ok())
        return members.status();
      grids.push_back(members.take_value());
    }
    for (Id vi = 0; vi < p_.values.size(); ++vi) {
      const auto& value = p_.values[vi];
      if (value.schema >= p_.schemas.size() ||
          static_cast<unsigned>(value.kind) > 3 ||
          static_cast<unsigned>(value.input_layout) > 1 ||
          (value.kind != ValueKind::Input &&
           value.input_layout != InputLayout::DirectTiles) ||
          (value.kind != ValueKind::Imported && value.import_source != none))
        return invalid("invalid value schema or import contract");
      if (value.kind == ValueKind::Imported) {
        if (value.owner >= p_.blocks.size() || value.source_value ||
            value.import_source >= p_.values.size())
          return invalid("invalid imported value identity");
        const auto& source = p_.values[value.import_source];
        if (source.kind != ValueKind::Input ||
            source.input_layout != InputLayout::DenseHost ||
            source.schema >= p_.schemas.size() || !same_type(source, value))
          return invalid("invalid imported value provenance");
        value_blocks_[vi] = value.owner;
        owned[vi] = true;
      }
      if (value.kind == ValueKind::Produced) {
        if (value.owner >= p_.calls.size())
          return invalid("invalid value call");
        value_blocks_[vi] = p_.calls[value.owner].block;
      } else if (!owned[vi]) {
        return invalid("orphan physical input or argument");
      }
      std::pair<Id, std::uint64_t> previous{};
      bool first = true;
      for (auto ti : value.tiles) {
        if (ti >= p_.tiles.size() || tiles[ti])
          return invalid("invalid tile membership");
        tiles[ti] = true;
        const auto& tile = p_.tiles[ti];
        const auto key = std::make_pair(tile.member, tile.tile_index);
        if (tile.value != vi || tile.layout >= p_.layouts.size() ||
            tile.member >= grids[value.schema].size() ||
            static_cast<unsigned>(tile.domain) > 2 ||
            !power_of_two(tile.alignment) || !tile.bytes ||
            (!first && !(previous < key)))
          return invalid("invalid physical tile descriptor");
        first = false;
        previous = key;
        if (value.kind == ValueKind::Produced) {
          if (tile.producer >= p_.tasks.size() ||
              p_.tasks[tile.producer].call != value.owner)
            return invalid("invalid tile producer");
        } else if (value.kind == ValueKind::Imported) {
          if (tile.producer >= p_.tasks.size() ||
              task_blocks_[tile.producer] != value.owner)
            return invalid("invalid import producer block");
          const auto& copy = p_.tasks[tile.producer];
          if (copy.kind != TaskKind::Copy || copy.reads.size() != 1 ||
              copy.writes.size() != 1 || copy.writes[0] != ti ||
              copy.reads[0].tile >= p_.tiles.size())
            return invalid("import requires one explicit COPY");
          const auto& raw = p_.tiles[copy.reads[0].tile];
          if (raw.value != value.import_source || raw.member != tile.member ||
              raw.tile_index != tile.tile_index ||
              copy.reads[0].layout != raw.layout || tile.domain != Domain::Host)
            return invalid("import COPY does not preserve source identity");
        } else if (tile.producer != none) {
          return invalid("incoming tile has a producer");
        }
        auto status = validate_layout(p_.layouts[tile.layout], tile.bytes,
                                      value.kind == ValueKind::Produced ||
                                          value.kind == ValueKind::Imported);
        if (!status.ok())
          return status;
        const auto& layout = p_.layouts[tile.layout];
        auto region = grids[value.schema][tile.member].region(tile.tile_index);
        if (!region.ok())
          return region.status();
        if (layout.rank != region.value().rank() ||
            layout.element_type != p_.schemas[value.schema]
                                       ->tensors[tile.member]
                                       .descriptor.element_type)
          return invalid("tile layout disagrees with schema");
        if (value.kind == ValueKind::Input &&
            value.input_layout == InputLayout::DenseHost) {
          auto shape =
              p_.schemas[value.schema]->tensors[tile.member].sample_shape();
          std::uint64_t bytes = scalar_bytes(layout.element_type), offset = 0;
          for (unsigned axis = layout.rank; axis-- > 0;) {
            std::uint64_t step;
            if (bytes > INT64_MAX ||
                layout.strides[axis] != static_cast<std::int64_t>(bytes) ||
                !multiply(layout.origin[axis], bytes, &step) ||
                !add(offset, step, &offset) ||
                !multiply(bytes, shape[axis], &bytes))
              return invalid("invalid dense input layout");
          }
          if (tile.domain != Domain::Host || tile.bytes != bytes ||
              layout.byte_offset != offset)
            return invalid("invalid dense input allocation");
        }
        for (Id axis = 0; axis < layout.rank; ++axis)
          if (layout.origin[axis] != region.value().dimensions()[axis].offset ||
              layout.shape[axis] != region.value().dimensions()[axis].extent)
            return invalid("tile layout disagrees with fixed grid");
      }
    }
    if (std::find(tiles.begin(), tiles.end(), false) != tiles.end())
      return invalid("orphan physical tile");
    for (Id bi = 0; bi < p_.blocks.size(); ++bi) {
      const auto& block = p_.blocks[bi];
      for (const auto& edge : block.edges) {
        for (Id i = 0; i < edge.arguments.size(); ++i) {
          auto status = available(edge.arguments[i], bi);
          if (!status.ok())
            return status;
          if (raw_dense_payload(edge.arguments[i]))
            return invalid("dense input edge requires an import COPY");
          const auto& from = p_.values[edge.arguments[i]];
          const auto& to = p_.values[p_.blocks[edge.target].arguments[i]];
          if (!same_type(from, to))
            return invalid("edge argument schema or coverage mismatch");
          if (!covers(from, to))
            return work_exhausted_ ? exhausted()
                                   : invalid("edge argument coverage mismatch");
        }
      }
      for (Id i = 0; i < block.return_values.size(); ++i) {
        auto status = available(block.return_values[i], bi);
        if (!status.ok())
          return status;
        if (raw_dense_payload(block.return_values[i]))
          return invalid("dense input return requires an import COPY");
        if (!p_.schemas[p_.values[block.return_values[i]].schema]->same_schema(
                *p_.schemas[p_.return_schemas[i]]))
          return invalid("return schema mismatch");
      }
      if (block.end == EndKind::Branch) {
        auto status = available(block.condition, bi);
        if (!status.ok())
          return status;
        if (raw_dense_payload(block.condition))
          return invalid("dense input branch requires an import COPY");
        const auto& value = p_.values[block.condition];
        const auto& schema = *p_.schemas[value.schema];
        if (schema.tensors.size() != 1 || value.tiles.size() != 1 ||
            schema.tensors[0].descriptor.element_type != ElementType::UInt8)
          return invalid("branch requires a host UInt8 scalar");
        const auto& tile = p_.tiles[value.tiles[0]];
        const auto& layout = p_.layouts[tile.layout];
        if (tile.domain == Domain::Device)
          return invalid("device condition requires an explicit host copy");
        for (Id axis = 0; axis < layout.rank; ++axis)
          if (layout.shape[axis] != 1)
            return invalid("branch is not scalar");
        auto count = schema.tensors[0].sample_count();
        if (!count.ok() || count.value() != 1)
          return invalid("branch is not scalar");
      }
    }
    for (Id ci = 0; ci < p_.calls.size(); ++ci) {
      std::set<Id> exports;
      for (auto vi : p_.calls[ci].returns)
        if (vi >= p_.values.size() ||
            p_.values[vi].kind != ValueKind::Produced ||
            p_.values[vi].owner != ci || !exports.insert(vi).second)
          return invalid("invalid call export");
    }
    return Status::success();
  }

  bool same_type(const ValueDesc& a, const ValueDesc& b) const {
    return p_.schemas[a.schema]->same_schema(*p_.schemas[b.schema]);
  }
  bool covers(const ValueDesc& from, const ValueDesc& to) {
    std::size_t index = 0;
    for (auto target : to.tiles) {
      if (!spend())
        return false;
      const auto& tile = p_.tiles[target];
      while (index < from.tiles.size()) {
        if (!spend())
          return false;
        const auto& source = p_.tiles[from.tiles[index]];
        auto a = std::tie(source.member, source.tile_index);
        auto b = std::tie(tile.member, tile.tile_index);
        if (a < b) {
          ++index;
          continue;
        }
        const auto& source_layout = p_.layouts[source.layout];
        const auto& target_layout = p_.layouts[tile.layout];
        if (a != b || source.domain != tile.domain ||
            source.bytes < tile.bytes || source.alignment < tile.alignment ||
            source_layout.byte_offset != target_layout.byte_offset ||
            source_layout.strides != target_layout.strides ||
            source_layout.origin != target_layout.origin ||
            source_layout.shape != target_layout.shape)
          return false;
        break;
      }
      if (index == from.tiles.size())
        return false;
    }
    return true;
  }

  Status read_view(const ReadDesc& read) {
    if (read.tile >= p_.tiles.size() || read.layout >= p_.layouts.size())
      return invalid("invalid physical read");
    const auto& tile = p_.tiles[read.tile];
    const auto& base = p_.layouts[tile.layout];
    const auto& view = p_.layouts[read.layout];
    auto status = validate_layout(view, tile.bytes, false);
    if (!status.ok())
      return status;
    if (base.rank != view.rank || base.element_type != view.element_type)
      return invalid("read view type mismatch");
    auto offset = base.byte_offset;
    std::uint64_t positive = 0, negative = 0;
    for (Id axis = 0; axis < base.rank; ++axis) {
      if (view.origin[axis] < base.origin[axis])
        return invalid("read before tile");
      auto delta = view.origin[axis] - base.origin[axis];
      if (delta > base.shape[axis] ||
          view.shape[axis] > base.shape[axis] - delta ||
          (view.shape[axis] > 1 && view.strides[axis] != base.strides[axis]))
        return invalid("read outside tile");
      const auto stride = base.strides[axis];
      auto magnitude = stride < 0
                           ? static_cast<std::uint64_t>(-(stride + 1)) + 1
                           : static_cast<std::uint64_t>(stride);
      std::uint64_t step;
      if (!multiply(delta, magnitude, &step) ||
          !(stride < 0 ? add(negative, step, &negative)
                       : add(positive, step, &positive)))
        return invalid("read offset overflow");
    }
    if (negative > offset || !add(offset - negative, positive, &offset) ||
        offset != view.byte_offset)
      return invalid("read offset disagrees with logical extent");
    return Status::success();
  }

  Status tasks() {
    std::vector<bool> written(p_.tiles.size());
    std::vector<Id> effects(p_.calls.size(), none);
    for (Id ti = 0; ti < p_.tasks.size(); ++ti) {
      const auto& task = p_.tasks[ti];
      if (static_cast<unsigned>(task.kind) > 2 ||
          task.placement >= p_.placements.size() ||
          (task.kind != TaskKind::Copy && task.kernel >= p_.kernels.size()) ||
          (task.kind == TaskKind::Copy && task.kernel != none))
        return invalid("invalid task kind, kernel or placement");
      const auto block = task_blocks_[ti];
      const auto ordinal =
          task.call == none ? none : p_.calls[task.call].ordinal;
      if (task.call == none &&
          (task.writes.size() != 1 || task.writes[0] >= p_.tiles.size() ||
           p_.values[p_.tiles[task.writes[0]].value].kind !=
               ValueKind::Imported ||
           !task.explicit_dependencies.empty() || !task.schema_reads.empty()))
        return invalid("boundary task must materialize an external input");
      const auto& placement = p_.placements[task.placement];
      auto& pred = derived.predecessors[ti];
      for (const auto& read : task.reads) {
        auto status = read_view(read);
        if (!status.ok())
          return status;
        const auto& tile = p_.tiles[read.tile];
        if (raw_dense_payload(tile.value)) {
          if (task.kind != TaskKind::Copy || task.writes.size() != 1 ||
              task.writes[0] >= p_.tiles.size())
            return invalid("dense input read requires an import COPY");
          const auto& imported = p_.values[p_.tiles[task.writes[0]].value];
          if (imported.kind != ValueKind::Imported ||
              imported.import_source != tile.value)
            return invalid("dense input read bypasses import provenance");
        }
        status = available(tile.value, block, ordinal);
        if (!status.ok())
          return status;
        if (task.kind != TaskKind::Copy && tile.domain != placement.domain)
          return invalid("cross-domain read requires COPY");
        if (tile.producer != none && task_blocks_[tile.producer] == block)
          pred.push_back(tile.producer);
      }
      for (auto vi : task.schema_reads)
        if (vi >= p_.values.size())
          return invalid("invalid schema-only read");
      pred.insert(pred.end(), task.explicit_dependencies.begin(),
                  task.explicit_dependencies.end());
      canonicalize(&pred);
      for (auto dependency : pred) {
        if (dependency >= p_.tasks.size() || dependency == ti)
          return invalid("invalid task dependency");
        const auto source_call = p_.tasks[dependency].call;
        const auto source_ordinal =
            source_call == none ? none : p_.calls[source_call].ordinal;
        if (task_blocks_[dependency] != block || source_ordinal > ordinal)
          return invalid("dependency is outside block or in a later call");
        derived.successors[dependency].push_back(ti);
      }
      for (auto wi : task.writes) {
        if (wi >= p_.tiles.size() || written[wi] || p_.tiles[wi].producer != ti)
          return invalid("duplicate or mismatched task write");
        const auto& tile = p_.tiles[wi];
        if (tile.domain != placement.domain)
          return invalid("write domain differs from placement");
        written[wi] = true;
        std::uint64_t rounded;
        if (!add(tile.bytes, tile.alignment - 1, &rounded))
          return exhausted();
        rounded &= ~(tile.alignment - 1);
        auto& bytes = derived.reservations[block]
                          .outputs[static_cast<unsigned>(tile.domain)];
        if (!add(bytes, rounded, &bytes))
          return exhausted();
      }
      if (task.kind == TaskKind::Copy) {
        if (task.reads.size() != 1 || task.writes.size() != 1 ||
            !task.parameters.empty())
          return invalid("COPY requires one input and one output");
        const auto& input = p_.layouts[task.reads[0].layout];
        const auto& output = p_.layouts[p_.tiles[task.writes[0]].layout];
        if (input.rank != output.rank ||
            input.element_type != output.element_type ||
            input.shape != output.shape || input.origin != output.origin)
          return invalid("COPY changes logical tile shape or type");
        if ((p_.tiles[task.reads[0].tile].domain == Domain::Device ||
             placement.domain == Domain::Device) &&
            placement.backend != Backend::Gpu)
          return invalid("device COPY requires GPU submission");
        if (placement.backend == Backend::Gpu) {
          const auto host_ends =
              static_cast<std::uint64_t>(p_.tiles[task.reads[0].tile].domain ==
                                         Domain::Host) +
              static_cast<std::uint64_t>(placement.domain == Domain::Host);
          std::uint64_t staging;
          if (!multiply(p_.tiles[task.writes[0]].bytes, host_ends, &staging))
            return exhausted();
          if (task.scratch[static_cast<unsigned>(Domain::Shared)] < staging)
            return invalid("GPU COPY requires declared Shared staging");
        }
      }
      if (task.kind == TaskKind::Effect) {
        if (effects[task.call] != none)
          return invalid("multiple effects in one call");
        effects[task.call] = ti;
      }
      auto& scratch = derived.reservations[block]
                          .scratch[static_cast<unsigned>(placement.backend)];
      for (unsigned domain = 0; domain < 3; ++domain)
        scratch[domain] = std::max(scratch[domain], task.scratch[domain]);
    }
    for (Id ti = 0; ti < p_.tiles.size(); ++ti)
      if (written[ti] != (p_.tiles[ti].producer != none))
        return invalid("materialized tile has no write");
    std::vector<Id> pending, ready;
    for (Id ti = 0; ti < p_.tasks.size(); ++ti) {
      pending.push_back(static_cast<Id>(derived.predecessors[ti].size()));
      if (!pending.back())
        ready.push_back(ti);
    }
    for (std::size_t head = 0; head < ready.size(); ++head) {
      if (!spend())
        return exhausted();
      for (auto successor : derived.successors[ready[head]]) {
        if (!spend())
          return exhausted();
        if (--pending[successor] == 0)
          ready.push_back(successor);
      }
    }
    if (ready.size() != p_.tasks.size())
      return invalid("cyclic physical task graph");
    // With an acyclic same-call graph, one sink proves every preparation task
    // reaches that sink. This checks terminal effects in O(T + E).
    for (Id ci = 0; ci < p_.calls.size(); ++ci) {
      const auto& call = p_.calls[ci];
      if (effects[ci] != call.effect_task)
        return invalid("effect membership mismatch");
      if (call.effect_task == none)
        continue;
      for (auto ti : call.tasks) {
        bool local_successor = false;
        for (auto successor : derived.successors[ti]) {
          if (!spend())
            return exhausted();
          local_successor |= p_.tasks[successor].call == ci;
        }
        if (local_successor == (ti == call.effect_task))
          return invalid("effect must be the unique terminal task of its call");
      }
    }
    return Status::success();
  }

  Status liveness() {
    std::vector<std::set<Id>> live(p_.blocks.size());
    auto use = [&](Id vi, Id bi) {
      if (value_blocks_[vi] != bi)
        live[bi].insert(vi);
    };
    for (Id ti = 0; ti < p_.tasks.size(); ++ti)
      for (const auto& read : p_.tasks[ti].reads)
        use(p_.tiles[read.tile].value, task_blocks_[ti]);
    for (Id bi = 0; bi < p_.blocks.size(); ++bi) {
      const auto& block = p_.blocks[bi];
      if (block.condition != none)
        use(block.condition, bi);
      for (auto vi : block.return_values)
        use(vi, bi);
      for (const auto& edge : block.edges)
        for (auto vi : edge.arguments)
          use(vi, bi);
    }
    bool changed = true;
    while (changed) {
      changed = false;
      for (Id bi = 0; bi < p_.blocks.size(); ++bi) {
        for (const auto& edge : p_.blocks[bi].edges) {
          for (auto vi : live[edge.target]) {
            if (!spend())
              return exhausted();
            if (value_blocks_[vi] != bi && live[bi].insert(vi).second)
              changed = true;
          }
        }
      }
    }
    for (Id bi = 0; bi < p_.blocks.size(); ++bi) {
      for (auto vi : live[bi]) {
        auto status = available(vi, bi);
        if (!status.ok())
          return status;
      }
      derived.live_ins[bi].assign(live[bi].begin(), live[bi].end());
    }
    return Status::success();
  }

  const Program& p_;
  const ValidationLimits& limits_;
  std::uint64_t work_;
  bool work_exhausted_ = false;
  std::vector<Id> value_blocks_, task_blocks_, dominator_;
  std::vector<std::vector<Id>> block_predecessors_;
};
}  // namespace

Status validate_layout(const Layout& layout, std::uint64_t bytes,
                       bool writable) {
  const auto width = scalar_bytes(layout.element_type);
  if (!width || !layout.rank || layout.rank > 8 || !bytes)
    return invalid("invalid physical view type or rank");
  std::uint64_t positive = width, negative = 0, dense = width;
  for (unsigned axis = layout.rank; axis-- > 0;) {
    const auto extent = layout.shape[axis];
    const auto stride = layout.strides[axis];
    if (!extent || extent > UINT64_MAX - layout.origin[axis])
      return invalid("invalid physical view extent");
    if (writable) {
      if (dense > INT64_MAX || stride != static_cast<std::int64_t>(dense) ||
          !multiply(dense, extent, &dense))
        return invalid("output view must have dense positive strides");
    }
    const auto magnitude = stride < 0
                               ? static_cast<std::uint64_t>(-(stride + 1)) + 1
                               : static_cast<std::uint64_t>(stride);
    std::uint64_t span;
    if (!multiply(extent - 1, magnitude, &span) ||
        !(stride < 0 ? add(negative, span, &negative)
                     : add(positive, span, &positive)))
      return invalid("physical view address overflow");
  }
  for (unsigned axis = layout.rank; axis < 8; ++axis)
    if (layout.shape[axis] || layout.origin[axis] || layout.strides[axis])
      return invalid("noncanonical unused layout axis");
  if (negative > layout.byte_offset || layout.byte_offset > bytes ||
      positive > bytes - layout.byte_offset || (writable && layout.byte_offset))
    return invalid("physical view exceeds allocation");
  return Status::success();
}

Status finalize_program(Program* program, const ValidationLimits& limits) {
  if (!program)
    return invalid("null physical program");
  try {
    Checker checker(*program, limits);
    auto status = checker.run();
    if (!status.ok())
      return status;
    for (Id i = 0; i < program->tasks.size(); ++i) {
      program->tasks[i].predecessors =
          std::move(checker.derived.predecessors[i]);
      program->tasks[i].successors = std::move(checker.derived.successors[i]);
      canonicalize(&program->tasks[i].explicit_dependencies);
    }
    for (Id i = 0; i < program->blocks.size(); ++i) {
      auto& block = program->blocks[i];
      block.live_ins = checker.derived.live_ins[i];
      block.reservation = checker.derived.reservations[i];
      for (auto& edge : block.edges)
        edge.live_values = checker.derived.live_ins[edge.target];
    }
    // Derived adjacency and live-through sets also consume the table budget.
    // A successful builder must publish a program the checked decoder accepts
    // under exactly the same limits.
    return validate_program(*program, limits);
  } catch (const std::bad_alloc&) {
    return exhausted();
  }
}

Status validate_program(const Program& program,
                        const ValidationLimits& limits) {
  try {
    Checker checker(program, limits);
    auto status = checker.run();
    if (!status.ok())
      return status;
    for (Id i = 0; i < program.tasks.size(); ++i) {
      const auto& task = program.tasks[i];
      if (!canonical(task.explicit_dependencies) ||
          task.predecessors != checker.derived.predecessors[i] ||
          task.successors != checker.derived.successors[i])
        return invalid("stored task adjacency mismatch");
    }
    for (Id i = 0; i < program.blocks.size(); ++i) {
      const auto& block = program.blocks[i];
      const auto& bound = checker.derived.reservations[i];
      if (block.live_ins != checker.derived.live_ins[i] ||
          block.reservation.outputs != bound.outputs ||
          block.reservation.scratch != bound.scratch)
        return invalid("stored liveness or reservation mismatch");
      for (const auto& edge : block.edges)
        if (edge.live_values != checker.derived.live_ins[edge.target])
          return invalid("stored edge live values mismatch");
    }
    return Status::success();
  } catch (const std::bad_alloc&) {
    return exhausted();
  }
}
}  // namespace ps::physical
