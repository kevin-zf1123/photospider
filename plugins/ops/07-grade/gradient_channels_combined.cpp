// Copyright 2026 Photospider contributors
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "07-grade/gradient_channels.hpp"

namespace ps::plugin_internal {
OperationDefinition channel_extraction_for_composition();
OperationDefinition channel_assembly_for_composition();
namespace native_sampling {
OperationDefinition sampling(unsigned mode);
}
namespace native_curves {
OperationDefinition interpolation(bool cubic, bool multi);
}
namespace vector_lookup {
OperationDefinition operation();
}
}  // namespace ps::plugin_internal

namespace ps::plugin_internal::grade::gradient_channels {
using namespace generation;  // NOLINT(build/namespaces)
namespace {
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
// FMT-01A's raw exact-copy stage generalized to a scalar cell with batch axes.
// Its sample coordinates become ordinary axes for the downstream numeric node.
struct BatchExtract final : Program {
  unsigned axis;
  std::uint64_t channel;
  BatchExtract(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto& spec = tensor(in[0], true);
    axis = integer(p, "axis") + spec.batch_axes.size();
    channel = integer(p, "index");
    auto dims = spec.sample_shape();
    dims.erase(dims.begin() + axis);
    add_output(this, tensor_schema(spec.descriptor.element_type, dims));
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (r.dimensions().empty())
      return {};
    for (const auto& d : r.dimensions())
      if (!d.extent)
        return {};
    auto dims = r.dimensions();
    dims.insert(dims.begin() + axis, {channel, 1});
    return {{0, 0, Region(std::move(dims))}};
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    const mask::Reader reader{k, inv};
    for (unsigned o = 0; o < inv.output_count; ++o)
      samples(inv.outputs[o], [&](const Coordinate& at, auto i) {
        if (!(i % 4096) && inv.cancellation.is_cancelled &&
            inv.cancellation.is_cancelled(inv.cancellation.context))
          throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
        Coordinate source{};
        for (unsigned a = 0, b = 0; a <= inv.outputs[o].layout->rank; ++a)
          source[a] = a == axis ? channel : at[b++];
        store(inv.outputs[o], at, reader.raw(0, 0, source));
      });
  }
};
OperationDefinition batch_extraction() {
  using Type = OperationParameterType;
  return generation::operation(
      "grade.gradient_query_copy", 1,
      {{"axis", Type::Int64, true}, {"index", Type::Int64, true}},
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [](const auto& in, const auto& p) {
        return std::make_shared<BatchExtract>(in, p);
      });
}
struct Combined {
  Configuration config;
  std::vector<Stage> stages;
  std::array<Source, 3> results;
  Combined(const std::vector<OperationMetadata>& in, const Parameters& p)
      : config(in, p) {
    const auto add = [&](OperationDefinition definition,
                         std::vector<Source> sources, Parameters parameters) {
      Stage stage;
      stage.definition = std::move(definition);
      auto& schema = stage.definition.traits.parameter_schema;
      std::sort(schema.begin(), schema.end(),
                [](const auto& a, const auto& b) { return a.key < b.key; });
      stage.sources = std::move(sources);
      stage.parameters = std::move(parameters);
      if (p.count("max_work") &&
          std::any_of(stage.definition.traits.parameter_schema.begin(),
                      stage.definition.traits.parameter_schema.end(),
                      [](const auto& spec) { return spec.key == "max_work"; }))
        stage.parameters["max_work"] = p.at("max_work");
      check(validate_operation_parameters(stage.definition.traits,
                                          stage.parameters));
      for (const auto& s : stage.sources)
        stage.inputs.push_back(s.stage < 0 ? in[s.port]
                                           : stages[s.stage].outputs[s.port]);
      stage.preparation =
          take(stage.definition.prepare_static(stage.inputs, stage.parameters));
      for (const auto& output : stage.preparation.outputs)
        stage.outputs.push_back(output.metadata);
      stages.push_back(std::move(stage));
      return static_cast<int>(stages.size() - 1);
    };
    const auto grid = add(native_sampling::sampling(0), {{-1, 1}, {-1, 2}},
                          {{"count", static_cast<std::int64_t>(config.length)},
                           {"dtype", std::string("float64")}});
    const std::string dtype =
        config.type == ElementType::Float32 ? "float32" : "float64";
    std::vector<Source> columns;
    for (unsigned c = 0; c < config.columns; ++c) {
      const auto stage =
          add(native_curves::interpolation(config.cubic[c], false),
              {{-1, 3 + 2 * c}, {-1, 4 + 2 * c}, {grid, 0}},
              {{"dtype", dtype}, {"out_of_domain", config.curve_edges[c]}});
      columns.push_back({stage, 0});
    }
    const auto table =
        add(channel_assembly_for_composition(), std::move(columns),
            {{"axis", static_cast<std::int64_t>(1)},
             {"layout", std::string("materialize")}});
    const auto& tensor = *in[0].result_schema;
    const bool scalar_cell = tensor.tensors[0].descriptor.shape.size() == 1;
    Parameters extraction{{"axis", p.at("axis")},
                          {"index", p.at("query_channel")}};
    if (!scalar_cell) {
      extraction["keepdims"] = false;
      extraction["layout"] = std::string("materialize");
    }
    const auto query = add(
        scalar_cell ? batch_extraction() : channel_extraction_for_composition(),
        {{-1, 0}}, std::move(extraction));
    const auto image =
        add(vector_lookup::operation(), {{query, 0}, {table, 0}, {grid, 1}},
            {{"dtype", dtype}, {"out_of_domain", config.lookup_policy}});
    results = {Source{image, 0}, Source{table, 0}, Source{grid, 1}};
    for (unsigned o = 0; o < 3; ++o)
      require(stages[results[o].stage]
                  .outputs[results[o].port]
                  .result_schema->same_schema(*config.outputs[o].result_schema),
              "GRD06C composition output schema");
  }
};
void encode(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint32_t decode(const std::vector<std::uint8_t>& bytes,
                     std::size_t* offset) {
  require(*offset <= bytes.size() && bytes.size() - *offset >= 4,
          "GRD06C malformed composition task");
  std::uint32_t result = 0;
  for (unsigned i = 0; i < 4; ++i)
    result |= static_cast<std::uint32_t>(bytes[(*offset)++]) << (8 * i);
  return result;
}
Result<OperationProgram> lower_combined(const OperationLowering& q) {
  try {
    const auto& state = *static_cast<const Combined*>(q.static_state);
    ResourceLimits limits;
    limits.maximum_work = q.limits.maximum_work;
    const ResourceBudget budget(limits);
    std::vector<std::vector<TileRequest>> requests(state.stages.size());
    for (unsigned s = 0; s < state.stages.size(); ++s)
      requests[s].resize(state.stages[s].outputs.size());
    const auto request = [&](Source source, unsigned member,
                             const TileSet& tiles) {
      auto& into = requests[source.stage][source.port].members[member];
      into = take(into.unite(tiles, budget));
    };
    for (unsigned o = 0; o < q.requests.size(); ++o)
      for (const auto& m : q.requests[o].members)
        request(state.results[o], m.first, m.second);
    std::vector<OperationProgram> programs(state.stages.size());
    // Demand flows backwards over actual lowered input tiles, including schema-
    // only/singleton stages. No payload or mutable cross-Run state is
    // inspected.
    for (std::size_t s = state.stages.size(); s-- > 0;) {
      const auto& stage = state.stages[s];
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
        const auto source = stage.sources[port];
        if (source.stage < 0)
          continue;
        std::map<unsigned, std::vector<TileIndex>> members;
        for (auto id : p.values[p.inputs[port]].tiles) {
          const auto& t = p.tiles[id];
          members[t.member].push_back(t.tile_index);
        }
        for (const auto& m : members)
          request(source, m.first,
                  take(TileSet::from_indices(budget, m.second)));
      }
    }
    OperationProgramBuilder builder(q);
    using TileKey = std::tuple<unsigned, unsigned, TileIndex>;
    std::vector<std::map<TileKey, Id>> published(state.stages.size());
    std::map<std::string, std::uint32_t> versions;
    for (unsigned s = 0; s < state.stages.size(); ++s) {
      const auto& stage = state.stages[s];
      const auto& child = programs[s];
      const auto& p = child.program;
      std::vector<Id> values(p.values.size(), physical::none);
      std::vector<Id> tiles(p.tiles.size(), physical::none);
      std::vector<Id> tasks(p.tasks.size(), physical::none);
      std::map<Id, unsigned> inputs, outputs;
      for (unsigned i = 0; i < p.inputs.size(); ++i)
        inputs[p.inputs[i]] = i;
      for (unsigned i = 0; i < child.outputs.size(); ++i)
        outputs[child.outputs[i]] = i;
      for (Id v = 0; v < p.values.size(); ++v)
        if (!inputs.count(v))
          values[v] =
              take(builder.add_temporary(p.schemas[p.values[v].schema]));
      for (Id t = 0; t < p.tasks.size(); ++t) {
        const auto& task = p.tasks[t];
        require(task.kind == physical::TaskKind::Kernel,
                "GRD06C stage must use a fixed CPU kernel");
        const auto& kernel = p.kernels[task.kernel];
        const std::string symbol =
            "stage" + std::to_string(s) + ":" + kernel.symbol;
        versions[symbol] = kernel.abi_version;
        tasks[t] =
            take(builder.add_task(symbol, kernel.contract,
                                  p.placements[task.placement], task.scratch));
        auto& parameters = builder.parameters(tasks[t]);
        encode(&parameters, s);
        encode(&parameters, static_cast<std::uint32_t>(task.reads.size()));
        encode(&parameters, static_cast<std::uint32_t>(task.writes.size()));
        encode(&parameters, static_cast<std::uint32_t>(task.parameters.size()));
        for (const auto& read : task.reads) {
          const auto& tile = p.tiles[read.tile];
          encode(&parameters, read.port);
          if (inputs.count(tile.value)) {
            const auto source = stage.sources[inputs.at(tile.value)];
            if (source.stage < 0)
              check(builder.read_input(tasks[t], source.port, tile.member,
                                       tile.tile_index, tile.domain));
            else
              check(builder.read_tile(
                  tasks[t],
                  published[source.stage].at(
                      TileKey{source.port, tile.member, tile.tile_index}),
                  read.port));
          } else {
            check(builder.read_tile(tasks[t], tiles[read.tile], read.port));
          }
        }
        for (auto id : task.writes) {
          const auto& tile = p.tiles[id];
          require(outputs.count(tile.value),
                  "GRD06C unexpected nested private stage");
          encode(&parameters, outputs.at(tile.value));
          if (s == static_cast<unsigned>(state.results[0].stage) &&
              outputs.at(tile.value) == state.results[0].port) {
            // The terminal lookup owns the public values tile directly.
            check(builder.write_output(tasks[t], 0, tile.member,
                                       tile.tile_index));
          } else {
            tiles[id] = take(builder.write_temporary(
                tasks[t], values[tile.value], tile.member, tile.tile_index));
            published[s][TileKey{outputs.at(tile.value), tile.member,
                                 tile.tile_index}] = tiles[id];
          }
        }
        parameters.insert(parameters.end(), task.parameters.begin(),
                          task.parameters.end());
        for (auto predecessor : task.explicit_dependencies)
          check(builder.depends_on(tasks[t], tasks[predecessor]));
        // Every external port was structurally validated by static preparation.
        // Preserve schema-only references without manufacturing payload reads.
        for (auto value : task.schema_reads)
          if (inputs.count(value)) {
            const auto source = stage.sources[inputs.at(value)];
            if (source.stage < 0)
              check(builder.read_input_schema(tasks[t], source.port));
          }
      }
    }
    // Auxiliary exports own buffers independently of their internal consumers.
    for (unsigned o = 1; o < q.requests.size(); ++o) {
      const auto source = state.results[o];
      for (const auto& m : q.requests[o].members)
        for (auto tile : m.second) {
          const auto task = take(builder.add_copy());
          check(builder.read_tile(task, published[source.stage].at(TileKey{
                                            source.port, m.first, tile})));
          check(builder.write_output(task, o, m.first, tile));
        }
    }
    auto result = take(builder.finish());
    for (auto& kernel : result.program.kernels)
      kernel.abi_version = versions.at(kernel.symbol);
    check(physical::finalize_program(&result.program, q.limits));
    return Result<OperationProgram>(std::move(result));
  } catch (const Status& failure) {
    return Result<OperationProgram>(failure);
  }
}
struct Invocation final {
  ps_prepared_kernel_v8 legacy{};
  ps_prepared_kernel_v9 budgeted{};
  std::vector<std::uint8_t> parameters;
  ~Invocation() {
    if (budgeted.release)
      budgeted.release(budgeted.context);
    if (legacy.release)
      legacy.release(legacy.context);
  }
};
struct PreparedChild {
  std::unique_ptr<Invocation> invocation;
  const Stage* stage;
  physical::KernelDesc kernel;
};
PreparedChild prepare_child(const physical::KernelDesc& description,
                            const KernelPreparation& q) {
  const auto& state = *static_cast<const Combined*>(q.static_state);
  std::size_t cursor = 0;
  const auto index = decode(q.task_parameters, &cursor);
  require(index < state.stages.size(), "GRD06C stage index");
  const auto& stage = state.stages[index];
  const auto nr = decode(q.task_parameters, &cursor),
             nw = decode(q.task_parameters, &cursor);
  const auto bytes = decode(q.task_parameters, &cursor);
  require(nr == q.reads.size() && nw == q.writes.size(), "GRD06C stage arity");
  std::vector<KernelView> reads = q.reads, writes = q.writes;
  for (auto& read : reads)
    read.port = decode(q.task_parameters, &cursor);
  for (auto& write : writes)
    write.port = decode(q.task_parameters, &cursor);
  require(bytes == q.task_parameters.size() - cursor,
          "GRD06C stage parameter bytes");
  auto owner = std::make_unique<Invocation>();
  owner->parameters.assign(q.task_parameters.begin() + cursor,
                           q.task_parameters.end());
  KernelPreparation child(stage.definition.key, stage.inputs, stage.parameters,
                          owner->parameters);
  child.outputs = stage.outputs;
  child.tile_extent = q.tile_extent;
  child.static_state = stage.preparation.state.get();
  child.reads = std::move(reads);
  child.writes = std::move(writes);
  child.scratch = q.scratch;
  const auto prefix = "stage" + std::to_string(index) + ":";
  require(description.symbol.compare(0, prefix.size(), prefix) == 0,
          "GRD06C stage symbol");
  auto kernel = description;
  kernel.operation = stage.definition.key;
  kernel.symbol = description.symbol.substr(prefix.size());
  // Retain the child query only for the synchronous preparation callback below.
  // The returned fixed context owns/borrows immutable stage state.
  return {std::move(owner), &stage, std::move(kernel)};
}
template <bool V9>
auto prepare_composed(const physical::KernelDesc& description,
                      const physical::Placement& placement,
                      const KernelPreparation& q, const ResourceSet& resources,
                      const ps_native_gpu_service_v2* native) {
  using Prepared =
      std::conditional_t<V9, ps_prepared_kernel_v9, ps_prepared_kernel_v8>;
  try {
    auto ready = prepare_child(description, q);
    const auto& stage = *ready.stage;
    std::size_t cursor = 16;
    auto reads = q.reads, writes = q.writes;
    for (auto& read : reads)
      read.port = decode(q.task_parameters, &cursor);
    for (auto& write : writes)
      write.port = decode(q.task_parameters, &cursor);
    KernelPreparation child(stage.definition.key, stage.inputs,
                            stage.parameters, ready.invocation->parameters);
    child.outputs = stage.outputs;
    child.static_state = stage.preparation.state.get();
    child.tile_extent = q.tile_extent;
    child.reads = std::move(reads);
    child.writes = std::move(writes);
    child.scratch = q.scratch;
    if constexpr (V9)
      ready.invocation->budgeted = take(stage.definition.prepare_kernel_v9(
          ready.kernel, placement, child, resources, native));
    else
      ready.invocation->legacy = take(stage.definition.prepare_kernel(
          ready.kernel, placement, child, resources, native));
    Prepared result{};
    result.struct_size = sizeof(result);
    result.abi_version = V9 ? 9 : 8;
    result.context = ready.invocation.release();
    result.release = [](const void* pointer) {
      delete static_cast<const Invocation*>(pointer);
    };
    if constexpr (V9) {
      result.invoke_cpu = [](const ps_kernel_invocation_v9* inv,
                             ps_kernel_status_v8* status) {
        const auto& owner =
            *static_cast<const Invocation*>(inv->views.prepared_context);
        auto child = *inv;
        child.views.prepared_context = owner.budgeted.context;
        child.views.parameters = owner.parameters.data();
        child.views.parameter_bytes = owner.parameters.size();
        owner.budgeted.invoke_cpu(&child, status);
      };
    } else {
      result.invoke_cpu = [](const ps_kernel_invocation_v8* inv,
                             ps_kernel_status_v8* status) {
        const auto& owner =
            *static_cast<const Invocation*>(inv->prepared_context);
        auto child = *inv;
        child.prepared_context = owner.legacy.context;
        child.parameters = owner.parameters.data();
        child.parameter_bytes = owner.parameters.size();
        owner.legacy.invoke_cpu(&child, status);
      };
    }
    return Result<Prepared>(result);
  } catch (const Status& failure) {
    return Result<Prepared>(failure);
  }
}
}  // namespace

OperationDefinition combined_operation() {
  auto result = compiled_operation();
  result.key = "grade.gradient_map_channels_combined";
  result.prepare_static = [](const auto& inputs,
                             const auto& p) -> Result<OperationPreparation> {
    try {
      require(integer(p, "max_work", INT64_MAX) >= 0,
              "GRD06C max_work must be nonnegative");
      auto state = std::make_shared<Combined>(inputs, p);
      OperationPreparation ready;
      for (const auto& output : state->config.outputs)
        ready.outputs.push_back({output, {}});
      if (state->config.profile)
        ready.resources.push_back(*state->config.profile);
      ready.state = std::move(state);
      return Result<OperationPreparation>(std::move(ready));
    } catch (const Status& failure) {
      return Result<OperationPreparation>(failure);
    }
  };
  result.lower = lower_combined;
  result.prepare_kernel = prepare_composed<false>;
  result.prepare_kernel_v9 = prepare_composed<true>;
  result.validate_resources = [](const void* state,
                                 const ResourceSet& resources) {
    return validate_profile(static_cast<const Combined*>(state)->config,
                            resources);
  };
  return result;
}
}  // namespace ps::plugin_internal::grade::gradient_channels
