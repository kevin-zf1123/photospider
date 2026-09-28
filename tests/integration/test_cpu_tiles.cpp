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
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Probe {
  std::atomic<unsigned> active{0}, peak{0}, alive{0};
  std::atomic<bool> paired{false};
  bool overlap = false, dynamic = false;
  unsigned mode = 0;
  CancellationSource cancel;
  std::function<void()> invalidate;
};
struct Tile {
  Probe* probe;
  unsigned requests = 0;
  explicit Tile(Probe* value) : probe(value) { ++probe->alive; }
  ~Tile() { --probe->alive; }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (requests < (probe->dynamic ? 2U : 1U)) {
      ++requests;
      if (!probe->dynamic) {
        DependencyNeedBatch batch;
        batch.static_mapping = true;
        return Answer(std::move(batch));
      }
      if (phase.query.observations.element_count().value() != 1)
        return Answer(Status{ErrorCode::Internal, "unbounded dynamic tile"});
      AtomCertificate row;
      row.output = {phase.query.observations.boxes()[0].dimensions()[0].offset};
      // A no-payload stage exercises suspend/supply/resume before the data
      // Need.
      if (requests == 2)
        row.inputs.push_back({0, 1, phase.query.outputs, {}});
      return Answer(DependencyNeedBatch({std::move(row)}));
    }
    struct Active {
      Probe* p;
      ~Active() { --p->active; }
    } active{probe};
    const auto count = ++probe->active;
    auto peak = probe->peak.load();
    while (peak < count && !probe->peak.compare_exchange_weak(peak, count)) {
    }
    if (count > 1)
      probe->paired = true;
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (probe->overlap && !probe->paired &&
           std::chrono::steady_clock::now() < until)
      std::this_thread::yield();
    if (probe->overlap && !probe->paired)
      return Answer(Status{ErrorCode::Internal, "tile polls did not overlap"});
    const auto& region = phase.query.outputs.boxes()[0];
    const auto index = region.dimensions()[0].offset;
    if (index == 0) {
      if (probe->mode == 1)
        probe->cancel.cancel();
      if (probe->mode == 2)
        return Answer(Status{ErrorCode::OperationFailed, "tile failure"});
      if (probe->mode == 3)
        probe->invalidate();
    }
    auto checked = phase.consume_work(1);
    if (!checked.ok())
      return Answer(checked);
    auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                            region, phase.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    const unsigned components = region.rank() == 2 ? 3 : 1;
    if (components == 3 && (region.dimensions()[1].offset != 0 ||
                            region.dimensions()[1].extent != 3))
      return Answer(Status{ErrorCode::Internal, "split atomic tuple"});
    for (unsigned component = 0; component < components; ++component) {
      std::int64_t value = 0;
      const auto at = components == 1
                          ? std::vector<std::uint64_t>{index}
                          : std::vector<std::uint64_t>{index, component};
      auto read = phase.read(0, at, &value, 8);
      if (!read.ok())
        return Answer(read);
      value += 7;
      std::memcpy(output.data() + component * 8, &value, 8);
    }
    auto published = std::move(output).publish();
    if (!published.ok())
      return Answer(published.status());
    auto fragments = ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs,
        {published.take_value()}, phase.sets);
    return fragments.ok() ? Answer(fragments.take_value())
                          : Answer(fragments.status());
  }
};
int check(unsigned workers, unsigned grant, unsigned mode, unsigned layers = 1,
          unsigned components = 1, bool dynamic = false) {
  Probe probe;
  probe.overlap = grant > 1;
  probe.dynamic = dynamic;
  probe.mode = mode;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "test.tile";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(Tile);
  output.maximum_dependency_stages = dynamic ? 3 : 2;
  operation.specialize_metadata = [components, dynamic](const auto& inputs,
                                                        const auto&) {
    OperationOutputSpecialization result;
    result.metadata = inputs[0];
    result.regional_atomic = true;
    DependencyMappedNeed need;
    need.port = 0;
    need.roles = 1;
    need.axes = {{0, {}, 0}};
    if (components == 3) {
      result.metadata.atomic_trailing_axes = 1;
      need.axes.push_back({-1, {0, 3}, 0});
    }
    if (!dynamic)
      result.static_dependency_pieces = std::vector<DependencyMapPiece>{
          {Footprint::all({12}).take_value(), {need}}};
    return Result<std::vector<OperationOutputSpecialization>>(
        {std::move(result)});
  };
  operation.start_dependency = [&probe](const DependencyQuery&,
                                        const BufferAllocator& allocator) {
    return DependencyContinuation::make<Tile>(allocator, &probe);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  OperationDefinition whole;
  whole.key = "test.whole";
  whole.traits.input_count = 1;
  whole.traits.input_schema.resize(1);
  whole.traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  whole.traits.outputs[0].output_element_type = ElementType::Int64;
  whole.callback = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
  };
  PS_CHECK(registry->register_operation(std::move(whole)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  const ValueDescriptor descriptor{
      ElementType::Int64, components == 1 ? std::vector<std::uint64_t>{12}
                                          : std::vector<std::uint64_t>{12, 3}};
  const StridedLayout layout =
      components == 1 ? StridedLayout{0, {8}} : StridedLayout{0, {24, 8}};
  document.inputs = {
      {1, "input", descriptor, Region::whole(descriptor.shape), layout, {}}};
  document.nodes = {{1, "test.tile", {WorkflowInputReference{1}}, {}}};
  for (unsigned i = 1; i < layers; ++i)
    document.nodes.push_back(
        {i + 1, "test.tile", {WorkflowNodeOutput{i, "value"}}, {}});
  document.outputs = {{"values", layers, "value"}};
  GraphContext graph(document);
  probe.invalidate = [&] { graph.replace(document); };
  PlanningOptions planning;
  planning.tile_width = 1;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  auto allocation = MutableValue::allocate(
      descriptor, Region::whole(descriptor.shape), BufferAllocator{});
  PS_CHECK(allocation.ok());
  auto writer = allocation.take_value();
  for (std::int64_t i = 0; i < 12 * components; ++i)
    std::memcpy(writer.data() + i * 8, &i, 8);
  auto input = std::move(writer).publish().take_value();
  ExecutionContextConfig config;
  config.cpu_workers = workers;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  ExecutionOptions options;
  options.maximum_parallelism = grant;
  unsigned delivered = 0;
  auto result = context.execute_stream(
      compiled.value().plan, {{{"input", input}}},
      [&](const std::string&, ValueView value) {
        if (mode == 4)
          return Status{ErrorCode::InvalidArgument, "sink failure"};
        std::int64_t observed = 0;
        std::memcpy(&observed, value.bytes().data(), 8);
        if (observed != delivered * components + 7 * layers ||
            value.region().dimensions()[0].offset != delivered)
          return Status{ErrorCode::Internal, "tile publication order"};
        for (unsigned component = 0; component < components; ++component) {
          std::memcpy(&observed, value.bytes().data() + component * 8, 8);
          if (observed != delivered * components + component + 7 * layers)
            return Status{ErrorCode::Internal, "tuple component result"};
        }
        ++delivered;
        return Status::success();
      },
      probe.cancel.token(), options);
  PS_CHECK(probe.active == 0 && probe.alive == 0);
  PS_CHECK(probe.peak <= grant);
  if (!mode) {
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && delivered == 12);
    PS_CHECK(result.value().peak_active_tasks <= grant);
    if (grant > 1)
      PS_CHECK(probe.paired && result.value().peak_active_tasks > 1);
  } else {
    PS_CHECK(!result.ok() && delivered == 0);
    PS_CHECK(result.status().code == (mode == 1   ? ErrorCode::Cancelled
                                      : mode == 2 ? ErrorCode::OperationFailed
                                      : mode == 3
                                          ? ErrorCode::Stale
                                          : ErrorCode::InvalidArgument));
  }
  if (!mode && components == 3) {
    probe.overlap = false;
    for (bool downstream : {false, true}) {
      auto current = document;
      if (downstream) {
        current.nodes.push_back({layers + 1,
                                 "test.whole",
                                 {WorkflowNodeOutput{layers, "value"}},
                                 {}});
        current.outputs = {{"values", layers + 1, "value"}};
      }
      GraphContext tuple_graph(current);
      auto tuple_plan = Compiler(registry).compile(tuple_graph, planning);
      PS_CHECK(tuple_plan.ok());
      auto result =
          context.execute(tuple_plan.value().plan, {{{"input", input}}});
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      const auto& bytes = result.value().values.at("values").bytes();
      for (std::int64_t i = 0; i < 36; ++i) {
        std::int64_t actual;
        std::memcpy(&actual, bytes.data() + i * 8, 8);
        PS_CHECK(actual == i + 7 * layers);
      }
      if (!downstream) {
        auto certificate = result.value().dependencies.certificate({layers, 0});
        PS_CHECK(certificate.ok());
        PS_CHECK(certificate.value().coverage() ==
                 Footprint::all({12}).take_value());
        auto support = result.value().dependencies.source_support();
        PS_CHECK(support.ok() && support.value().at("input") ==
                                     Footprint::all({12, 3}).take_value());
      }
      PS_CHECK(probe.active == 0 && probe.alive == 0);
    }
  }
  if (!mode && grant == 2 && layers == 2) {
    probe.peak = 0;
    probe.overlap = false;
    std::vector<std::future<Result<ExecutionDiagnostics>>> runs;
    for (unsigned i = 0; i < 4; ++i)
      runs.push_back(std::async(std::launch::async, [&] {
        unsigned next = 0;
        return context.execute_stream(
            compiled.value().plan, {{{"input", input}}},
            [&](const std::string&, ValueView value) {
              std::int64_t actual = 0;
              std::memcpy(&actual, value.bytes().data(), 8);
              if (actual != next + 7 * layers ||
                  value.region().dimensions()[0].offset != next)
                return Status{ErrorCode::Internal, "concurrent stream order"};
              ++next;
              return Status::success();
            },
            {}, options);
      }));
    for (auto& future : runs) {
      PS_CHECK(future.wait_for(std::chrono::seconds(10)) ==
               std::future_status::ready);
      PS_CHECK(future.get().ok());
    }
    PS_CHECK(probe.active == 0 && probe.alive == 0 && probe.peak <= workers);
  }
  return 0;
}
}  // namespace
int main() {
  for (bool dynamic : {false, true}) {
    for (unsigned grant : {1, 2, 4})
      if (check(4, grant, 0, 1, 1, dynamic))
        return 1;
    for (unsigned mode : {1, 2, 3, 4})
      if (check(4, 2, mode, 1, 1, dynamic))
        return 1;
    if (check(4, 2, 0, 2, 1, dynamic))
      return 1;
    if (check(4, 2, 0, 1, 3, dynamic))
      return 1;
  }
  return 0;
}
