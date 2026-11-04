// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "07-grade/statistical_common.hpp"

namespace ps::plugin_internal::grade::composition {
using namespace generation;  // NOLINT(build/namespaces)
using statistics::empty;
using Id = physical::Id;
struct Source {
  int stage = -1;
  unsigned port = 0;
};
struct Stage {
  OperationDefinition definition;
  Parameters parameters;
  std::vector<Source> sources;
  std::vector<OperationMetadata> inputs, outputs;
  OperationPreparation preparation;
};
/** Operator-local DAG with explicit Plan temporaries. Statistics are immutable
 * Run-owned results, shared by all application tiles. No pointer/content cache
 * survives an invocation, so replacing input frames cannot reuse stale data.
 */
struct Graph {
  std::vector<Stage> stages;
  Source result;
  int add(const std::vector<OperationMetadata>& external,
          OperationDefinition def, std::vector<Source> sources,
          Parameters parameters) {
    Stage s;
    s.definition = std::move(def);
    s.parameters = std::move(parameters);
    s.sources = std::move(sources);
    auto& schema = s.definition.traits.parameter_schema;
    std::sort(schema.begin(), schema.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    check(validate_operation_parameters(s.definition.traits, s.parameters));
    for (auto src : s.sources) {
      s.inputs.push_back(src.stage < 0
                             ? external.at(src.port)
                             : stages.at(src.stage).outputs.at(src.port));
    }
    s.preparation = take(s.definition.prepare_static(s.inputs, s.parameters));
    for (const auto& o : s.preparation.outputs) {
      s.outputs.push_back(o.metadata);
    }
    stages.push_back(std::move(s));
    return static_cast<int>(stages.size() - 1);
  }
};
inline void encode(std::vector<std::uint8_t>* bytes, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) {
    bytes->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
}
inline std::uint32_t decode(const std::vector<std::uint8_t>& b,
                            std::size_t* offset) {
  require(*offset <= b.size() && b.size() - *offset >= 4,
          "statistical DAG task encoding");
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(b[(*offset)++]) << (8 * i);
  }
  return v;
}
inline Result<OperationProgram> lower(const OperationLowering& q) {
  try {
    const auto& g = *static_cast<const Graph*>(q.static_state);
    ResourceLimits limits;
    limits.maximum_work = q.limits.maximum_work;
    const ResourceBudget budget(limits);
    std::vector<std::vector<TileRequest>> requests(g.stages.size());
    for (unsigned s = 0; s < g.stages.size(); ++s) {
      requests[s].resize(g.stages[s].outputs.size());
    }
    const auto request = [&](Source src, unsigned m, const TileSet& tiles) {
      auto& into = requests[src.stage][src.port].members[m];
      into = take(into.unite(tiles, budget));
    };
    for (const auto& m : q.requests[0].members) {
      request(g.result, m.first, m.second);
    }
    std::vector<OperationProgram> programs(g.stages.size());
    for (std::size_t s = g.stages.size(); s-- > 0;) {
      const auto& stage = g.stages[s];
      OperationLowering child;
      child.operation = stage.definition.key;
      child.inputs = stage.inputs;
      child.outputs = stage.outputs;
      child.parameters = stage.parameters;
      child.requests = requests[s];
      child.tile_extent = q.tile_extent;
      child.backend = q.backend;
      child.static_state = stage.preparation.state.get();
      child.limits = q.limits;
      programs[s] = take(stage.definition.lower(child));
      const auto& p = programs[s].program;
      for (unsigned port = 0; port < stage.sources.size(); ++port) {
        const auto src = stage.sources[port];
        if (src.stage < 0) {
          continue;
        }
        std::map<unsigned, std::vector<TileIndex>> members;
        for (auto id : p.values[p.inputs[port]].tiles) {
          const auto& t = p.tiles[id];
          members[t.member].push_back(t.tile_index);
        }
        for (const auto& m : members) {
          request(src, m.first, take(TileSet::from_indices(budget, m.second)));
        }
      }
    }
    OperationProgramBuilder b(q);
    using Key = std::tuple<unsigned, unsigned, TileIndex>;
    std::vector<std::map<Key, Id>> published(g.stages.size());
    std::map<std::string, std::uint32_t> versions;
    for (unsigned s = 0; s < g.stages.size(); ++s) {
      const auto& stage = g.stages[s];
      const auto& child = programs[s];
      const auto& p = child.program;
      std::vector<Id> values(p.values.size(), physical::none),
          tiles(p.tiles.size(), physical::none),
          tasks(p.tasks.size(), physical::none);
      std::map<Id, unsigned> inputs, outputs;
      for (unsigned i = 0; i < p.inputs.size(); ++i) {
        inputs[p.inputs[i]] = i;
      }
      for (unsigned i = 0; i < child.outputs.size(); ++i) {
        outputs[child.outputs[i]] = i;
      }
      for (Id v = 0; v < p.values.size(); ++v) {
        if (!inputs.count(v)) {
          values[v] = take(b.add_temporary(p.schemas[p.values[v].schema]));
        }
      }
      for (Id t = 0; t < p.tasks.size(); ++t) {
        const auto& task = p.tasks[t];
        require(task.kind == physical::TaskKind::Kernel,
                "statistical DAG requires fixed CPU stages");
        const auto& kernel = p.kernels[task.kernel];
        const std::string symbol =
            "stage" + std::to_string(s) + ":" + kernel.symbol;
        versions[symbol] = kernel.abi_version;
        tasks[t] = take(b.add_task(symbol, kernel.contract,
                                   p.placements[task.placement], task.scratch));
        auto& parameters = b.parameters(tasks[t]);
        encode(&parameters, s);
        encode(&parameters, task.reads.size());
        encode(&parameters, task.writes.size());
        encode(&parameters, task.parameters.size());
        for (const auto& read : task.reads) {
          const auto& tile = p.tiles[read.tile];
          encode(&parameters, read.port);
          if (inputs.count(tile.value)) {
            const auto src = stage.sources[inputs.at(tile.value)];
            if (src.stage < 0) {
              check(b.read_input(tasks[t], src.port, tile.member,
                                 tile.tile_index, tile.domain));
            } else {
              check(b.read_tile(tasks[t],
                                published[src.stage].at(Key{
                                    src.port, tile.member, tile.tile_index}),
                                read.port));
            }
          } else {
            check(b.read_tile(tasks[t], tiles[read.tile], read.port));
          }
        }
        for (auto id : task.writes) {
          const auto& tile = p.tiles[id];
          require(outputs.count(tile.value),
                  "nested statistical DAG private output");
          const auto port = outputs.at(tile.value);
          encode(&parameters, port);
          if (s == static_cast<unsigned>(g.result.stage) &&
              port == g.result.port) {
            check(b.write_output(tasks[t], 0, tile.member, tile.tile_index));
          } else {
            tiles[id] = take(b.write_temporary(tasks[t], values[tile.value],
                                               tile.member, tile.tile_index));
            published[s][Key{port, tile.member, tile.tile_index}] = tiles[id];
          }
        }
        parameters.insert(parameters.end(), task.parameters.begin(),
                          task.parameters.end());
        for (auto pred : task.explicit_dependencies) {
          check(b.depends_on(tasks[t], tasks[pred]));
        }
        for (auto v : task.schema_reads) {
          if (inputs.count(v)) {
            const auto src = stage.sources[inputs.at(v)];
            if (src.stage < 0) {
              check(b.read_input_schema(tasks[t], src.port));
            }
          }
        }
      }
    }
    auto out = take(b.finish());
    for (auto& k : out.program.kernels) {
      k.abi_version = versions.at(k.symbol);
    }
    check(physical::finalize_program(&out.program, q.limits));
    return Result<OperationProgram>(std::move(out));
  } catch (const Status& s) {
    return Result<OperationProgram>(s);
  }
}
struct Invocation {
  ps_prepared_kernel_v8 legacy{};
  ps_prepared_kernel_v9 budgeted{};
  std::vector<std::uint8_t> parameters;
  ~Invocation() {
    if (legacy.release) {
      legacy.release(legacy.context);
    }
    if (budgeted.release) {
      budgeted.release(budgeted.context);
    }
  }
};
template <bool V9>
auto prepare(const physical::KernelDesc& description,
             const physical::Placement& placement, const KernelPreparation& q,
             const ResourceSet& resources,
             const ps_native_gpu_service_v2* native) {
  using Prepared =
      std::conditional_t<V9, ps_prepared_kernel_v9, ps_prepared_kernel_v8>;
  try {
    const auto& g = *static_cast<const Graph*>(q.static_state);
    std::size_t cursor = 0;
    const auto index = decode(q.task_parameters, &cursor);
    require(index < g.stages.size(), "statistical DAG stage");
    const auto& stage = g.stages[index];
    const auto nr = decode(q.task_parameters, &cursor),
               nw = decode(q.task_parameters, &cursor);
    const auto size = decode(q.task_parameters, &cursor);
    require(nr == q.reads.size() && nw == q.writes.size(),
            "statistical DAG task arity");
    auto reads = q.reads, writes = q.writes;
    for (auto& r : reads) {
      r.port = decode(q.task_parameters, &cursor);
    }
    for (auto& w : writes) {
      w.port = decode(q.task_parameters, &cursor);
    }
    require(size == q.task_parameters.size() - cursor,
            "statistical DAG parameter size");
    auto owner = std::make_unique<Invocation>();
    owner->parameters.assign(q.task_parameters.begin() + cursor,
                             q.task_parameters.end());
    KernelPreparation child(stage.definition.key, stage.inputs,
                            stage.parameters, owner->parameters);
    child.outputs = stage.outputs;
    child.static_state = stage.preparation.state.get();
    child.tile_extent = q.tile_extent;
    child.reads = std::move(reads);
    child.writes = std::move(writes);
    child.scratch = q.scratch;
    const auto prefix = "stage" + std::to_string(index) + ":";
    require(description.symbol.compare(0, prefix.size(), prefix) == 0,
            "statistical DAG symbol");
    auto kernel = description;
    kernel.operation = stage.definition.key;
    kernel.symbol = description.symbol.substr(prefix.size());
    if constexpr (V9) {
      owner->budgeted = take(stage.definition.prepare_kernel_v9(
          kernel, placement, child, resources, native));
    } else {
      owner->legacy = take(stage.definition.prepare_kernel(
          kernel, placement, child, resources, native));
    }
    Prepared out{};
    out.struct_size = sizeof(out);
    out.abi_version = V9 ? 9 : 8;
    out.context = owner.release();
    out.release = [](const void* p) {
      delete static_cast<const Invocation*>(p);
    };
    if constexpr (V9) {
      out.invoke_cpu = [](const ps_kernel_invocation_v9* i,
                          ps_kernel_status_v8* s) {
        const auto& o =
            *static_cast<const Invocation*>(i->views.prepared_context);
        auto c = *i;
        c.views.prepared_context = o.budgeted.context;
        c.views.parameters = o.parameters.data();
        c.views.parameter_bytes = o.parameters.size();
        o.budgeted.invoke_cpu(&c, s);
      };
    } else {
      out.invoke_cpu = [](const ps_kernel_invocation_v8* i,
                          ps_kernel_status_v8* s) {
        const auto& o = *static_cast<const Invocation*>(i->prepared_context);
        auto c = *i;
        c.prepared_context = o.legacy.context;
        c.parameters = o.parameters.data();
        c.parameter_bytes = o.parameters.size();
        o.legacy.invoke_cpu(&c, s);
      };
    }
    return Result<Prepared>(out);
  } catch (const Status& s) {
    return Result<Prepared>(s);
  }
}
template <class Factory>
OperationDefinition operation(OperationDefinition prototype, Factory factory) {
  prototype.prepare_static =
      [factory](const auto& in, const auto& p) -> Result<OperationPreparation> {
    try {
      auto graph = std::make_shared<Graph>(factory(in, p));
      OperationPreparation out;
      out.outputs.push_back(
          {graph->stages.at(graph->result.stage).outputs.at(graph->result.port),
           {}});
      out.state = std::move(graph);
      return Result<OperationPreparation>(std::move(out));
    } catch (const Status& s) {
      return Result<OperationPreparation>(s);
    }
  };
  prototype.lower = lower;
  prototype.prepare_kernel = prepare<false>;
  prototype.prepare_kernel_v9 = prepare<true>;
  return prototype;
}
}  // namespace ps::plugin_internal::grade::composition
