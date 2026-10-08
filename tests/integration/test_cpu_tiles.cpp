#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Probe {
  std::atomic<unsigned> active{0}, peak{0}, alive{0}, callbacks{0},
      empty_queries{0};
  std::atomic<bool> paired{false};
  bool overlap = false, dynamic = false;
  unsigned mode = 0, components = 1;
  CancellationSource cancel;
  std::function<void()> invalidate;
};
struct Work {
  Probe* probe;
  const ResultTensorInput* input;
  std::uint8_t* output;
  std::array<std::atomic<unsigned>, 12> visited{};
  static int compute(void* raw, const ps_cpu_tile_v1* tile) {
    auto& self = *static_cast<Work*>(raw);
    auto& probe = *self.probe;
    struct Retire {
      Probe& probe;
      ~Retire() { --probe.active; }
    } retire{probe};
    const auto active = ++probe.active;
    auto peak = probe.peak.load();
    while (peak < active && !probe.peak.compare_exchange_weak(peak, active)) {
    }
    if (active > 1)
      probe.paired = true;
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (probe.overlap && !probe.paired &&
           std::chrono::steady_clock::now() < until)
      std::this_thread::yield();
    if (probe.overlap && !probe.paired)
      return 1;
    if (tile->begin[1] != 0 || tile->end[1] != 1 || tile->begin[2] != 0 ||
        tile->end[2] != 1 || tile->end[0] - tile->begin[0] != 1)
      return 6;
    ++probe.callbacks;
    const auto index = tile->begin[0];
    if (++self.visited.at(index) != 1)
      return 6;
    if (!index) {
      if (probe.mode == 1)
        probe.cancel.cancel();
      if (probe.mode == 2)
        return 1;
      if (probe.mode == 3)
        probe.invalidate();
    }
    if (probe.cancel.token().cancelled())
      return 2;
    for (unsigned component = 0; component < probe.components; ++component) {
      std::int64_t value = 0;
      const auto at = probe.components == 1
                          ? std::vector<std::uint64_t>{index}
                          : std::vector<std::uint64_t>{index, component};
      if (!self.input->read(at, &value, sizeof(value)).ok())
        return 1;
      value += 7;
      std::memcpy(self.output + (index * probe.components + component) * 8,
                  &value, 8);
    }
    return 0;
  }
};
struct Tile {
  Probe* probe;
  unsigned requests = 0;
  explicit Tile(Probe* probe) : probe(probe) { ++probe->alive; }
  ~Tile() { --probe->alive; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using Answer = Result<ResultProgramPoll>;
    const auto& spec = phase.query.output.result_schema->tensors[0];
    const auto shape = spec.sample_shape();
    if (requests < (probe->dynamic ? 2U : 1U)) {
      ++requests;
      ResultProgramNeed need;
      if (probe->dynamic && requests == 1)
        need.tensors.push_back(
            {0, 0, take(Footprint::from_regions(shape, {})), 2});
      else
        need.tensors.push_back({0, 0, take(Footprint::all(shape)), 1});
      return Answer(std::move(need));
    }
    if (!phase.cpu_tiles || phase.cpu_tiles->maximum_workers == 0)
      return Answer(Status{ErrorCode::Internal, "missing CPU tile service"});
    check(phase.consume_work(12 * probe->components));
    auto buffer =
        take(phase.resources.allocator().allocate(12 * probe->components * 8));
    Work work{probe, &phase.tensors->at({0, 0}), buffer.data(), {}};
    ps_cpu_tile_stage_v1 stage{};
    stage.struct_size = sizeof(stage);
    stage.extent[0] = 12;
    stage.extent[1] = stage.extent[2] = 1;
    stage.tile[0] = stage.tile[1] = stage.tile[2] = 1;
    const int code = phase.cpu_tiles->run(phase.cpu_tiles->context, &stage,
                                          Work::compute, &work);
    if (code)
      return Answer(Status{code == 2   ? ErrorCode::Cancelled
                           : code == 4 ? ErrorCode::ResourceExhausted
                                       : ErrorCode::OperationFailed,
                           "tile stage failed"});
    if (probe->mode == 5)
      check(phase.consume_work(1000000));
    if (phase.query.tensor_outputs && phase.query.tensor_outputs->empty())
      ++probe->empty_queries;
    for (const auto& visited : work.visited)
      if (visited != 1)
        return Answer(Status{ErrorCode::Internal, "tile partition coverage"});
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        std::vector<std::uint64_t>(phase.association->begin(),
                                   phase.association->end())));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    ResultTensorViewTransform identity;
    for (unsigned axis = 0; axis < shape.size(); ++axis)
      identity.source_axes.push_back(
          {static_cast<std::int32_t>(axis), 0, 1, 1});
    auto relation = take(ResultRelation::mapped(
        phase.resources, shape, Region::whole(shape), shape,
        identity.source_axes, {0, 1, 0, 1, ResultSupportTarget::Tensor, 0}));
    StridedLayout layout = probe->components == 1 ? StridedLayout{0, {8}}
                                                  : StridedLayout{0, {24, 8}};
    check(builder.publish_tensor(
        0, Region::whole(shape), layout, std::move(buffer).freeze(),
        std::move(relation), {true, true, true, true}));
    return Answer(ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
int run(unsigned grant, unsigned mode, unsigned layers = 1,
        unsigned components = 1, bool dynamic = false) {
  Probe probe;
  probe.overlap = grant > 1;
  probe.mode = mode;
  probe.components = components;
  probe.dynamic = dynamic;
  const auto shape = components == 1 ? std::vector<std::uint64_t>{12}
                                     : std::vector<std::uint64_t>{12, 3};
  auto schema = multi_result::schema(ElementType::Int64, shape);
  if (components == 3)
    schema.tensors[0].atomic_trailing_axes = 1;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "test.tile";
  operation.traits.cacheable = false;
  operation.traits.cpu_staged_tiles = true;
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.input_schema[0].kind = OperationPortKind::Result;
  operation.traits.input_schema[0].tensor_key = "number";
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].continuation_bytes = sizeof(Tile);
  operation.traits.outputs[0].maximum_dependency_stages = 3;
  operation.start_result = [&probe](const auto&, const auto& allocator) {
    return ResultContinuation::make<Tile>(allocator, &probe);
  };
  check(registry->register_operation(std::move(operation)));
  OperationDefinition whole;
  whole.key = "test.whole";
  whole.traits.cacheable = false;
  whole.traits.input_count = 1;
  whole.traits.input_schema.resize(1);
  whole.traits.input_schema[0].kind = OperationPortKind::Result;
  whole.traits.input_schema[0].tensor_key = "number";
  whole.traits.outputs = {multi_result::output("value", schema)};
  whole.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  whole.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  check(registry->register_operation(std::move(whole)));
  check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", schema)};
  document.nodes = {{1, "test.tile", {WorkflowInputReference{1}}, {}}};
  for (unsigned i = 1; i < layers; ++i)
    document.nodes.push_back(
        {i + 1, "test.tile", {WorkflowNodeOutput{i, "value"}}, {}});
  document.outputs = {{"values", layers, "value"}};
  GraphContext graph(document);
  probe.invalidate = [&] { graph.replace(document); };
  PlanningOptions planning;
  planning.tile_width = 1;
  auto plan = take(Compiler(registry).compile(graph, planning));
  ExecutionContextConfig config;
  config.cpu_workers = 4;
  config.gpu_enabled = false;
  config.result_cache_bytes = 0;
  if (mode == 5) {
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_work = 1000000;
  }
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto builder = take(ResultBuilder::start(root, schema, "input"));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  std::vector<std::int64_t> values(12 * components);
  for (unsigned i = 0; i < values.size(); ++i)
    values[i] = i;
  check(builder.publish_tensor(
      0, Region::whole(shape),
      {reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * 8},
      take(ResultRelation::cartesian(root, values.size(), {})),
      {true, true, true, true}));
  ExecutionBinding input;
  input.name = "input";
  input.result = take(builder.seal());
  ExecutionOptions options;
  options.maximum_parallelism = grant;
  unsigned delivered = 0;
  options.result_publication = [&](ValueRef output, const ResultRef& result) {
    if (output.node_id != layers)
      return Status::success();
    if (mode == 4)
      return Status{ErrorCode::InvalidArgument, "sink failure"};
    for (unsigned index = 0; index < 12; ++index)
      for (unsigned component = 0; component < components; ++component) {
        std::int64_t number = 0;
        const auto at = components == 1
                            ? std::vector<std::uint64_t>{index}
                            : std::vector<std::uint64_t>{index, component};
        check(result.read_tensor(take(result.descriptor()), 0, at, &number, 8));
        if (number != index * components + component + 7 * layers)
          return Status{ErrorCode::Internal, "tile result"};
      }
    ++delivered;
    return Status::success();
  };
  auto result =
      context.execute(plan.plan, {{input}}, probe.cancel.token(), options);
  PS_CHECK(probe.active == 0 && probe.alive == 0 && probe.peak <= grant);
  if (mode) {
    PS_CHECK(!result.ok() && delivered == 0);
    PS_CHECK(result.status().code == (mode == 1   ? ErrorCode::Cancelled
                                      : mode == 2 ? ErrorCode::OperationFailed
                                      : mode == 3 ? ErrorCode::Stale
                                      : mode == 4
                                          ? ErrorCode::InvalidArgument
                                          : ErrorCode::ResourceExhausted));
    if (mode == 1)
      PS_CHECK(probe.callbacks > 0);
    if (mode == 5) {
      PS_CHECK(probe.callbacks == 12);
      PS_CHECK(result.status().reason == FailureReason::WorkLimit);
    }
  } else {
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    // Empty metadata and full payload Needs have distinct producer queries.
    // A layered dynamic consumer therefore runs its upstream producer twice.
    const auto stages = layers + (dynamic && layers == 2 ? 1U : 0U);
    PS_CHECK(result.ok() && delivered == 1 && probe.callbacks == 12 * stages);
    PS_CHECK(probe.empty_queries == (dynamic && layers == 2 ? 1U : 0U));
    PS_CHECK(result.value().diagnostics.cpu_stage_count == stages);
    PS_CHECK(result.value().diagnostics.cpu_tile_callback_count == 12 * stages);
    if (grant > 1)
      PS_CHECK(probe.paired && probe.peak > 1);
    PS_CHECK(take(result.value().results.at("values").descriptor())
                 .tensor_coverage(0) == take(Footprint::all(shape)));
  }
  if (!mode && components == 3) {
    probe.overlap = false;
    document.nodes.push_back(
        {layers + 1, "test.whole", {WorkflowNodeOutput{layers, "value"}}, {}});
    document.outputs = {{"values", layers + 1, "value"}};
    GraphContext downstream(document);
    auto next = take(Compiler(registry).compile(downstream, planning));
    auto copied = context.execute(next.plan, {{input}});
    PS_CHECK(copied.ok());
    for (unsigned i = 0; i < 36; ++i) {
      std::int64_t number = 0;
      const auto& output = copied.value().results.at("values");
      check(output.read_tensor(take(output.descriptor()), 0, {i / 3, i % 3},
                               &number, 8));
      PS_CHECK(number == i + 7 * layers);
    }
  }
  if (!mode && grant == 2 && layers == 2) {
    probe.peak = 0;
    probe.overlap = false;
    options.result_publication = {};
    std::vector<std::future<Result<ExecutionResult>>> runs;
    for (unsigned i = 0; i < 4; ++i)
      runs.push_back(std::async(std::launch::async, [&] {
        return context.execute(plan.plan, {{input}}, {}, options);
      }));
    for (auto& future : runs) {
      PS_CHECK(future.wait_for(std::chrono::seconds(10)) ==
               std::future_status::ready);
      auto concurrent = future.get();
      PS_CHECK(concurrent.ok());
      std::int64_t number = 0;
      const auto& output = concurrent.value().results.at("values");
      check(output.read_tensor(take(output.descriptor()), 0, {11}, &number, 8));
      PS_CHECK(number == 25);
    }
    PS_CHECK(probe.active == 0 && probe.alive == 0 && probe.peak <= 4);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
}  // namespace
int main() try {
  for (bool dynamic : {false, true}) {
    for (unsigned grant : {1, 2, 4})
      PS_CHECK(run(grant, 0, 1, 1, dynamic) == 0);
    for (unsigned mode : {1, 2, 3, 4, 5})
      PS_CHECK(run(2, mode, 1, 1, dynamic) == 0);
    PS_CHECK(run(2, 0, 2, 1, dynamic) == 0);
    PS_CHECK(run(2, 0, 1, 3, dynamic) == 0);
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
