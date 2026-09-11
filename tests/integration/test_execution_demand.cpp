#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"
#include "support/typed_images.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Footprint point(std::uint64_t at, std::uint64_t size = 5) {
  return Footprint::from_regions({size}, {Region({{at, 1}})}).take_value();
}
template <class T>
Value values(ElementType type, const std::vector<T>& numbers) {
  auto writer =
      MutableValue::allocate({type, {numbers.size()}},
                             Region::whole({numbers.size()}), BufferAllocator{})
          .take_value();
  std::memcpy(writer.data(), numbers.data(), writer.size());
  return std::move(writer).publish().take_value();
}
WorkflowDocument scatter_document() {
  WorkflowDocument document;
  document.inputs = {{1,
                      "data",
                      {ElementType::Float64, {5}},
                      Region::whole({5}),
                      {0, {8}},
                      {}},
                     {2,
                      "radius",
                      {ElementType::Int64, {5}},
                      Region::whole({5}),
                      {0, {8}},
                      {}}};
  document.nodes = {{1,
                     "numeric.radius_scatter",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  return document;
}
int generations() {
  auto registry = make_default_operation_registry();
  GraphContext graph(scatter_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096});
  ExecutionBindings bindings{
      {{"data", values<double>(ElementType::Float64, {1, 2, 3, 0, 5})},
       {"radius", values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0})}}};
  auto opened = context.open_demand(plan, bindings);
  PS_CHECK(opened.ok());
  auto demand = opened.take_value();
  PS_CHECK(demand.generation().value() == 1);
  const auto q = point(0).unite(point(4)).take_value();
  DemandQuery query{{"sum", q}};
  auto before = demand.request(query);
  if (!before.ok())
    std::cerr << before.status().message << '\n';
  PS_CHECK(before.ok() && before.value().generation == 1);
  PS_CHECK(before.value().values.at("sum").coverage() == q);
  double result = 0;
  PS_CHECK(before.value().values.at("sum").read({0}, &result, 8).ok() &&
           result == 1);
  PS_CHECK(!before.value().values.at("sum").read({1}, &result, 8).ok());
  auto frozen = demand.freeze().take_value();
  bindings.inputs[1].value =
      values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0});
  auto edit = demand.replace_bindings(bindings);
  if (!edit.ok())
    std::cerr << edit.status().message << '\n';
  PS_CHECK(edit.ok() && edit.value().generation == 2 &&
           edit.value().coverage.at("sum") == q);
  PS_CHECK(edit.value().potential_dirty.at("sum") == q);
  auto same_value = demand.request(query);
  PS_CHECK(same_value.ok() &&
           same_value.value().values.at("sum").read({0}, &result, 8).ok() &&
           result == 1);
  PS_CHECK(same_value.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum") == q);
  auto old = context.execute_fragments(frozen, query);
  PS_CHECK(old.ok() && old.value().generation == 0);
  PS_CHECK(old.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum")
               .empty());
  bindings.inputs[0].value =
      values<double>(ElementType::Float64, {1, 2, 3, 9, 5});
  auto changed = demand.replace_bindings(bindings);
  PS_CHECK(changed.ok() && changed.value().potential_dirty.at("sum") == q);
  auto fresh = demand.request(query);
  PS_CHECK(fresh.ok() &&
           fresh.value().values.at("sum").read({4}, &result, 8).ok() &&
           result == 14);
  // Only current witness samples are compared: radius[0..4] and data{0,3,4}.
  bindings.inputs[0].value =
      values<double>(ElementType::Float64, {1, 2, 777, 9, 5});
  auto unrelated =
      demand.replace_bindings(bindings, SnapshotAccessOptions{8, {}});
  PS_CHECK(unrelated.ok() &&
           unrelated.value().potential_dirty.at("sum").empty());
  auto failed = demand.replace_bindings(bindings, SnapshotAccessOptions{7, {}});
  PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(demand.generation().value() == unrelated.value().generation);
  auto bad_bindings = bindings;
  bad_bindings.inputs[0].value = values<double>(ElementType::Float64, {1});
  PS_CHECK(demand.replace_bindings(bad_bindings).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(demand.release(query).ok());
  PS_CHECK(demand.release(query).code == ErrorCode::NotFound);
  auto no_subscriptions = demand.replace_bindings(bindings);
  PS_CHECK(no_subscriptions.ok() && no_subscriptions.value().coverage.empty());
  auto empty = demand.request({{"sum", Footprint::none({5}).take_value()}});
  PS_CHECK(empty.ok() && empty.value().values.at("sum").coverage().empty());
  PS_CHECK(empty.value().diagnostics.operation_timings.empty());
  PS_CHECK(empty.value().dependencies.coverage().at("sum").empty());
  PS_CHECK(demand.request({}).ok());
  PS_CHECK(demand.cancel() && !demand.cancel());
  PS_CHECK(demand.request(query).status().code == ErrorCode::Cancelled);
  PS_CHECK(context.execute_fragments(frozen, query).ok());
  return 0;
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered = 0;
  bool release = false;
  bool ignore_cancellation = false;
  unsigned released = 0;
  std::atomic<unsigned> active{0};
  bool await(unsigned count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(3),
                            [&] { return entered >= count; });
  }
  void open() {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
    changed.notify_all();
  }
  void allow(unsigned count) {
    std::lock_guard<std::mutex> lock(mutex);
    released = count;
    changed.notify_all();
  }
};
struct StagedGate {
  std::function<Result<Value>(const OperationInvocation&)> callback;
  bool terminal = false;
  bool requested = false;
  explicit StagedGate(
      std::function<Result<Value>(const OperationInvocation&)> callback,
      bool terminal = false)
      : callback(std::move(callback)), terminal(terminal) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto at =
        terminal ? 0 : phase.query.outputs.boxes()[0].dimensions()[0].offset;
    if (!requested) {
      requested = true;
      if (terminal)
        return Result<DependencyPoll>(DependencyNeedBatch{
            {},
            {{0, 1, point(0, phase.query.inputs[0].descriptor.shape[0]), {}}}});
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{at}, {{0, 1, phase.query.outputs, {}}}}}, {}});
    }
    double sample = 0;
    auto status = phase.read(0, {at}, &sample, 8);
    if (!status.ok())
      return Result<DependencyPoll>(status);
    const std::vector<Value> inputs{phase.inputs[0].fragments()[0]};
    const std::vector<Region> regions{
        terminal ? Region({{0, 1}}) : phase.query.outputs.boxes()[0]};
    const std::map<std::string, ParameterValue> parameters;
    OperationInvocation call{inputs,
                             regions,
                             parameters,
                             Backend::Cpu,
                             phase.query.cancellation,
                             regions[0],
                             phase.allocator};
    auto output = callback(call);
    if (!output.ok())
      return Result<DependencyPoll>(output.status());
    if (terminal) {
      output = Result<Value>(Status{ErrorCode::Cancelled, {}});
      const double value = sample + phase.query.outputs.element_count().value();
      std::vector<Value> fragments;
      for (const auto& region : phase.query.outputs.boxes()) {
        auto writer = MutableValue::allocate(phase.query.output.descriptor,
                                             region, phase.allocator)
                          .take_value();
        for (std::size_t i = 0; i < writer.size(); i += 8)
          std::memcpy(writer.data() + i, &value, 8);
        fragments.push_back(std::move(writer).publish().take_value());
      }
      return Result<DependencyPoll>(
          ValueFragments::create(phase.query.output.descriptor, {},
                                 phase.query.outputs, fragments)
              .take_value());
    }
    return Result<DependencyPoll>(
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {output.take_value()})
            .take_value());
  }
};
std::shared_ptr<OperationRegistry> gated_registry(
    const std::shared_ptr<Gate>& gate, bool staged = false,
    std::shared_ptr<std::atomic<unsigned>> effects = {}, bool terminal = false,
    bool whole = false, bool gpu_fallback = false) {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "wait";
  op.traits.supports_gpu = gpu_fallback;
  op.traits.allows_cpu_fallback = gpu_fallback;
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  op.traits.region_rule =
      whole ? OperationRegionRule::Whole : OperationRegionRule::Elementwise;
  op.callback = [gate](const OperationInvocation& call) -> Result<Value> {
    ++gate->active;
    struct Active {
      Gate* gate;
      ~Active() { --gate->active; }
    } active{gate.get()};
    {
      std::unique_lock<std::mutex> lock(gate->mutex);
      const auto ticket = ++gate->entered;
      gate->changed.notify_all();
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!gate->release && ticket > gate->released &&
             (!call.cancellation.cancelled() || gate->ignore_cancellation)) {
        if (std::chrono::steady_clock::now() >= deadline)
          break;
        gate->changed.wait_for(lock, std::chrono::milliseconds(2));
      }
      if (!gate->release && ticket > gate->released &&
          !call.cancellation.cancelled())
        return Result<Value>(
            Status{ErrorCode::OperationFailed, "gate deadline"});
    }
    if (call.cancellation.cancelled())
      return Result<Value>(Status{ErrorCode::Cancelled, {}});
    const auto at = call.output_region.dimensions()[0].offset;
    auto address = call.inputs[0].byte_address({at});
    if (!address.ok())
      return Result<Value>(address.status());
    double value = 0;
    std::memcpy(&value, call.inputs[0].bytes().data() + address.value(), 8);
    if (!std::isfinite(value))
      return Result<Value>(Status{ErrorCode::OperationFailed,
                                  "nonfinite sample " + std::to_string(at)});
    auto output = MutableValue::allocate(call.inputs[0].descriptor(),
                                         call.output_region, call.allocator)
                      .take_value();
    for (std::uint64_t i = 0; i < call.output_region.dimensions()[0].extent;
         ++i) {
      const auto source = call.inputs[0].byte_address({at + i}).take_value();
      std::memcpy(output.data() + i * 8, call.inputs[0].bytes().data() + source,
                  8);
    }
    return std::move(output).publish();
  };
  if (staged) {
    op.traits.region_rule = OperationRegionRule::Dependency;
    op.traits.dependency_version = 1;
    op.traits.continuation_bytes = sizeof(StagedGate);
    op.traits.maximum_dependency_stages = 4;
    if (terminal)
      op.traits.observation_kind = ObservationKind::RequestRecord;
    op.start_dependency = [callback = std::move(op.callback), terminal](
                              const DependencyQuery&,
                              const BufferAllocator& allocator) {
      return DependencyContinuation::make<StagedGate>(allocator, callback,
                                                      terminal);
    };
    op.callback = {};
  }
  if (effects) {
    OperationDefinition effect;
    effect.key = "effect";
    effect.traits.deterministic = false;
    effect.traits.side_effect_free = false;
    effect.traits.cacheable = false;
    effect.traits.shape_rule = OperationShapeRule::Fixed;
    effect.traits.fixed_output_shape = {2};
    effect.callback =
        [effects](const OperationInvocation& call) -> Result<Value> {
      const double value = ++*effects;
      auto writer = MutableValue::allocate({ElementType::Float64, {2}},
                                           Region::whole({2}), call.allocator)
                        .take_value();
      std::memcpy(writer.data(), &value, 8);
      std::memcpy(writer.data() + 8, &value, 8);
      return std::move(writer).publish();
    };
    if (!registry->register_operation(std::move(effect)).ok())
      throw std::runtime_error("effect registration");
  }
  if (!registry->register_operation(std::move(op)).ok() ||
      !registry->freeze().ok())
    throw std::runtime_error("gate registration");
  return registry;
}
WorkflowDocument gate_document() {
  WorkflowDocument document;
  document.inputs = {
      {1, "x", {ElementType::Float64, {2}}, Region::whole({2}), {0, {8}}, {}}};
  document.nodes = {{1, "wait", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"y", 1, "value"}};
  return document;
}
int edit_and_cancel() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto context = std::make_unique<ExecutionContext>(
      registry, ExecutionContextConfig{2, false, 8, 4096});
  ExecutionBindings bindings{
      {{"x", values<double>(ElementType::Float64, {1, 2})}}};
  auto demand = context->open_demand(plan, bindings).take_value();
  gate->open();
  PS_CHECK(demand.request({{"y", point(0, 2)}}).ok());
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  auto old = std::async(std::launch::async,
                        [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(1));
  bindings.inputs[0].value = values<double>(ElementType::Float64, {3, 4});
  auto replaced = demand.replace_bindings(bindings);
  PS_CHECK(replaced.ok() && replaced.value().generation == 2);
  PS_CHECK(replaced.value().potential_dirty.at("y") == point(0, 2));
  gate->open();
  PS_CHECK(old.get().status().code == ErrorCode::Stale && gate->active == 0);
  auto next = demand.request({{"y", point(0, 2)}});
  PS_CHECK(next.ok() && next.value().generation == 2);
  double value = 0;
  PS_CHECK(next.value().values.at("y").read({0}, &value, 8).ok() && value == 3);
  // Each request owns its token; cancelling one does not stop the other.
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  CancellationSource cancel;
  auto a = std::async(std::launch::async,
                      [&] { return demand.request({{"y", point(0, 2)}}); });
  auto b = std::async(std::launch::async, [&] {
    return demand.request({{"y", point(1, 2)}}, cancel.token());
  });
  PS_CHECK(gate->await(2));
  cancel.cancel();
  gate->changed.notify_all();
  PS_CHECK(b.get().status().code == ErrorCode::Cancelled);
  gate->open();
  PS_CHECK(a.get().ok() && gate->active == 0);
  // Context shutdown propagates cancellation and waits for callback retirement.
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  auto pending = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(1));
  context.reset();
  PS_CHECK(pending.get().status().code == ErrorCode::Cancelled &&
           gate->active == 0);
  PS_CHECK(demand.request({{"y", point(0, 2)}}).status().code ==
           ErrorCode::Cancelled);
  return 0;
}
int isolation_and_limits() {
  auto gate = std::make_shared<Gate>();
  gate->open();
  auto registry = gated_registry(gate);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config{1, false, 4, 4096};
  config.maximum_demands = 1;
  ExecutionContext context(registry, config);
  ExecutionBindings bindings{
      {{"x", values<double>(ElementType::Float64,
                            {1, std::numeric_limits<double>::infinity()})}}};
  auto demand = context.open_demand(plan, bindings).take_value();
  PS_CHECK(context.open_demand(plan, bindings).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(demand.request({{"y", point(0, 2)}}).ok());
  PS_CHECK(demand.request({{"y", point(1, 2)}}).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(
      demand.request({{"y", Footprint::all({2}).take_value()}}).status().code ==
      ErrorCode::OperationFailed);
  PS_CHECK(demand.request({{"y", point(0, 2)}}).ok());
  auto frozen = demand.freeze().take_value();
  PS_CHECK(graph.replace(gate_document()) > 0);
  PS_CHECK(demand.request({{"y", point(0, 2)}}).ok());
  PS_CHECK(demand.cancel());
  auto fresh = context.open_demand(frozen.plan(), bindings, DemandConfig{1});
  PS_CHECK(fresh.ok());
  PS_CHECK(fresh.value().request({{"y", point(0, 2)}}).status().code ==
           ErrorCode::ResourceExhausted);
  return 0;
}
int combined_tokens() {
  CancellationSource a, b;
  auto both = CancellationToken::combine({a.token(), b.token()}).take_value();
  auto nested = CancellationToken::combine({both, a.token(), {}}).take_value();
  PS_CHECK(!nested.cancelled());
  b.cancel();
  PS_CHECK(both.cancelled() && nested.cancelled() && !a.token().cancelled());
  std::vector<CancellationSource> sources(65);
  std::vector<CancellationToken> tokens;
  for (auto& source : sources)
    tokens.push_back(source.token());
  PS_CHECK(CancellationToken::combine(tokens).status().code ==
           ErrorCode::ResourceExhausted);
  tokens.pop_back();
  auto all = CancellationToken::combine(tokens).take_value();
  PS_CHECK(!all.cancelled());
  sources[32].cancel();
  PS_CHECK(all.cancelled());
  return 0;
}
int snapshot_replacement() {
  auto registry = make_default_operation_registry();
  ExecutionContext context(registry, {1, false, 8, 16384});
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    auto writer = MutableValue::allocate({type, {3}}, Region::whole({3}),
                                         BufferAllocator{})
                      .take_value();
    std::memset(writer.data(), 0, writer.size());
    auto initial = std::move(writer).publish().take_value();
    InputSnapshotStore store({256, 1});
    auto snapshot = store.import_value(initial).take_value();
    GraphContext graph(typed_images::document(initial));
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionBindings bindings{
        {{"image", {}, {}, std::make_shared<const InputSnapshot>(snapshot)}}};
    auto demand = context.open_demand(plan, bindings).take_value();
    const auto q = point(0, 3).unite(point(2, 3)).take_value();
    PS_CHECK(demand.request({{"result", q}}).ok());
    auto frozen = demand.freeze().take_value();
    for (auto at : {0U, 2U}) {
      auto patch = MutableValue::allocate(initial.descriptor(),
                                          Region({{at, 1}}), BufferAllocator{})
                       .take_value();
      // Includes nonfinite generic float payloads; dirty compares bits.
      std::memset(patch.data(), 0xff, patch.size());
      snapshot = store.patch(snapshot, std::move(patch).publish().take_value())
                     .take_value();
      bindings.inputs[0].snapshot =
          std::make_shared<const InputSnapshot>(snapshot);
      auto changed = demand.replace_bindings(bindings, {2, {}});
      PS_CHECK(changed.ok());
      PS_CHECK(changed.value().potential_dirty.at("result") ==
               (at == 0 ? point(0, 3) : q));
    }
    auto final = demand.request({{"result", q}});
    PS_CHECK(final.ok());
    std::uint8_t bytes[8]{};
    const auto width = Value::element_size(type);
    PS_CHECK(final.value().values.at("result").read({2}, bytes, width).ok());
    for (unsigned i = 0; i < width; ++i)
      PS_CHECK(bytes[i] == 0xff);
    auto old = context.execute_fragments(frozen, {{"result", q}});
    PS_CHECK(old.ok() &&
             old.value().values.at("result").read({2}, bytes, width).ok());
    for (unsigned i = 0; i < width; ++i)
      PS_CHECK(bytes[i] == 0);
    auto same = demand.replace_bindings(bindings, {2, {}});
    PS_CHECK(same.ok() && same.value().potential_dirty.at("result").empty());
  }
  auto image = typed_images::value(rgba_semantics());
  InputSnapshotStore store({4096, 1});
  auto base = store.import_value(image).take_value();
  GraphContext graph(typed_images::document(image));
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionBindings bindings{
      {{"image", {}, {}, std::make_shared<const InputSnapshot>(base)}}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const Region pixel({{1, 1}, {2, 1}, {0, 4}});
  const auto q =
      Footprint::from_regions(image.descriptor().shape, {pixel}).take_value();
  PS_CHECK(demand.request({{"result", q}}).ok());
  auto patch =
      MutableValue::allocate(image.descriptor(), pixel, BufferAllocator{})
          .take_value();
  const float rgba[] = {-1, 2, 0, .75F};
  std::memcpy(patch.data(), rgba, sizeof(rgba));
  auto replacement = std::move(patch).publish(image.facets()).take_value();
  auto next = store.patch(base, replacement).take_value();
  bindings.inputs[0].snapshot = std::make_shared<const InputSnapshot>(next);
  PS_CHECK(demand.replace_bindings(bindings, {3, {}}).status().code ==
           ErrorCode::ResourceExhausted);
  auto changed = demand.replace_bindings(bindings, {4, {}});
  PS_CHECK(changed.ok() && changed.value().potential_dirty.at("result") == q);
  auto partial = Footprint::from_regions(image.descriptor().shape,
                                         {Region({{1, 1}, {2, 1}, {0, 1}})})
                     .take_value();
  PS_CHECK(!demand.request({{"result", partial}}).ok());
  return 0;
}
int owner_retirement() {
  auto gate = std::make_shared<Gate>();
  gate->open();
  auto registry = gated_registry(gate);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto context = std::make_unique<ExecutionContext>(registry);
  ExecutionBindings ordinary{
      {{"x", values<double>(ElementType::Float64, {1, 2})}}};
  auto observer = context->open_demand(plan, ordinary).take_value();
  unsigned retired = 0;
  const auto owned = [&] {
    BufferAllocator allocator([&](std::uint64_t) {
      return Result<std::shared_ptr<void>>(
          std::shared_ptr<void>(new int(0), [&, observer](void* pointer) {
            delete static_cast<int*>(pointer);
            // A legitimate reservation owner can reenter the same context.
            // All bundle retirement paths must therefore unlock first.
            const auto status = observer.generation();
            if (status.ok() || status.status().code == ErrorCode::Cancelled)
              ++retired;
          }));
    });
    auto writer = MutableValue::allocate({ElementType::Float64, {2}},
                                         Region::whole({2}), allocator)
                      .take_value();
    ExecutionBindings bindings{
        {{"x", std::move(writer).publish().take_value()}}};
    return context->open_demand(plan, std::move(bindings)).take_value();
  };
  auto cancelled = owned();
  PS_CHECK(cancelled.cancel() && retired == 1);
  auto replaced = owned();
  PS_CHECK(replaced.replace_bindings(ordinary).ok() && retired == 2);
  auto destroyed = owned();
  destroyed = DemandHandle{};
  PS_CHECK(retired == 3);
  auto closing = owned();
  context.reset();
  PS_CHECK(retired == 4 &&
           closing.generation().status().code == ErrorCode::Cancelled);
  return 0;
}
bool await_shared(const ExecutionContext& context, std::uint64_t previous) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (context.cache_statistics().shared_computations <= previous) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::yield();
  }
  return true;
}
int shared_ancestors() {
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    const auto cancelled = scenario % 3;
    auto gate = std::make_shared<Gate>();
    auto registry = gated_registry(gate, scenario >= 3);
    auto document = gate_document();
    document.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
    document.outputs = {{"inner", 1, "value"}, {"outer", 2, "value"}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, {1, false, 8, 4096});
    auto demand =
        context
            .open_demand(
                plan, {{{"x", values<double>(ElementType::Float64, {7, 9})}}})
            .take_value();
    CancellationSource stop_outer, stop_inner;
    auto outer = std::async(std::launch::async, [&] {
      return demand.request({{"outer", point(0, 2)}}, stop_outer.token());
    });
    PS_CHECK(gate->await(1));
    auto inner = std::async(std::launch::async, [&] {
      return demand.request({{"inner", point(0, 2)}}, stop_inner.token());
    });
    // Observe a real directory join before cancellation; no timing sleep.
    PS_CHECK(await_shared(context, 0));
    if (cancelled == 1)
      stop_outer.cancel();
    if (cancelled == 2)
      stop_inner.cancel();
    gate->open();
    auto outer_result = outer.get();
    auto inner_result = inner.get();
    if (outer_result.ok() != (cancelled != 1) ||
        inner_result.ok() != (cancelled != 2))
      std::cerr << "shared scenario " << scenario
                << ": outer=" << static_cast<int>(outer_result.status().code)
                << ' ' << outer_result.status().message
                << "; inner=" << static_cast<int>(inner_result.status().code)
                << ' ' << inner_result.status().message << '\n';
    PS_CHECK(outer_result.ok() == (cancelled != 1));
    PS_CHECK(inner_result.ok() == (cancelled != 2));
    if (cancelled == 1)
      PS_CHECK(outer_result.status().code == ErrorCode::Cancelled);
    if (cancelled == 2)
      PS_CHECK(inner_result.status().code == ErrorCode::Cancelled);
    if (outer_result.ok()) {
      double value = 0;
      PS_CHECK(
          outer_result.value().values.at("outer").read({0}, &value, 8).ok() &&
          value == 7);
      PS_CHECK(outer_result.value().dependencies.record_count() == 2);
      PS_CHECK(outer_result.value()
                   .dependencies.potential_dirty("x", point(0, 2))
                   .value()
                   .at("outer") == point(0, 2));
    }
    if (inner_result.ok()) {
      double value = 0;
      PS_CHECK(
          inner_result.value().values.at("inner").read({0}, &value, 8).ok() &&
          value == 7);
      PS_CHECK(inner_result.value().diagnostics.shared_computations == 1);
      PS_CHECK(inner_result.value().diagnostics.shared_peak_live_bytes >= 8);
      PS_CHECK(inner_result.value().dependencies.record_count() == 1);
      PS_CHECK(inner_result.value()
                   .dependencies.potential_dirty("x", point(0, 2))
                   .value()
                   .at("inner") == point(0, 2));
    }
    PS_CHECK(gate->entered == (cancelled == 1 ? 1U : 2U));
    PS_CHECK(context.cache_statistics().in_flight == 0);
  }
  return 0;
}
int auxiliary_cancellation() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096});
  auto demand =
      context
          .open_demand(plan,
                       {{{"x", values<double>(ElementType::Float64, {7, 9})}}})
          .take_value();
  CancellationSource auxiliary;
  ExecutionOptions options;
  options.dependencies.sets.cancellation = auxiliary.token();
  auto a = std::async(std::launch::async, [&] {
    return demand.request({{"y", point(0, 2)}}, {}, options);
  });
  PS_CHECK(gate->await(1));
  auto b = std::async(std::launch::async,
                      [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(await_shared(context, 0));
  auxiliary.cancel();
  gate->open();
  PS_CHECK(a.get().status().code == ErrorCode::Cancelled);
  auto survived = b.get();
  PS_CHECK(survived.ok() &&
           survived.value().diagnostics.shared_computations == 1);
  PS_CHECK(gate->entered == 1 && gate->active == 0);
  return 0;
}
int impure_ancestor() {
  auto gate = std::make_shared<Gate>();
  auto effects = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = gated_registry(gate, true, effects);
  WorkflowDocument document;
  document.nodes = {{1, "effect", {}, {}},
                    {2, "wait", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"y", 2, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  auto demand = context.open_demand(plan, {}).take_value();
  auto a = std::async(std::launch::async,
                      [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(1));
  auto b = std::async(std::launch::async,
                      [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(2));
  gate->open();
  auto first = a.get(), second = b.get();
  PS_CHECK(first.ok() && second.ok());
  double x = 0, y = 0;
  PS_CHECK(first.value().values.at("y").read({0}, &x, 8).ok());
  PS_CHECK(second.value().values.at("y").read({0}, &y, 8).ok());
  PS_CHECK(x == 1 && y == 2 && *effects == 2);
  PS_CHECK(context.cache_statistics().shared_computations == 0);
  PS_CHECK(first.value().dependencies.record_count() == 2 &&
           second.value().dependencies.record_count() == 2);
  return 0;
}
int shared_fallback() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, false, {}, false, true, true);
  auto doc = gate_document();
  doc.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
  doc.outputs = {{"y", 2, "value"}};
  GraphContext graph(doc);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::MetalFp32;
  auto plan = Compiler(registry).compile(graph, planning).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096, 128});
  auto demand =
      context
          .open_demand(plan,
                       {{{"x", values<double>(ElementType::Float64, {7, 9})}}})
          .take_value();
  CancellationSource stop;
  auto owner = std::async(std::launch::async, [&] {
    return demand.request({{"y", point(0, 2)}}, stop.token());
  });
  PS_CHECK(gate->await(1));
  auto other = std::async(std::launch::async,
                          [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(await_shared(context, 0));
  stop.cancel();
  gate->open();
  auto cancelled = owner.get();
  auto completed = other.get();
  double actual = 0;
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled);
  PS_CHECK(completed.ok() &&
           completed.value().values.at("y").read({0}, &actual, 8).ok() &&
           actual == 7);
  PS_CHECK(completed.value().diagnostics.shared_computations == 1 &&
           completed.value().diagnostics.selected_backends.at(2) ==
               Backend::Cpu);
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  PS_CHECK(
      demand.request({{"y", point(0, 2)}}).value().diagnostics.cache_hits == 0);
  return 0;
}
int late_flight_and_frozen() {
  auto gate = std::make_shared<Gate>();
  gate->ignore_cancellation = true;
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  ExecutionBindings bindings{
      {{"x", values<double>(ElementType::Float64, {7, 9})}}};
  auto demand = context.open_demand(plan, bindings).take_value();
  CancellationSource stop;
  DemandQuery query{{"y", point(0, 2)}};
  auto p0 = std::async(std::launch::async,
                       [&] { return demand.request(query, stop.token()); });
  PS_CHECK(gate->await(1));
  stop.cancel();
  context.clear_result_cache();
  auto p1 =
      std::async(std::launch::async, [&] { return demand.request(query); });
  PS_CHECK(gate->await(2));
  gate->allow(1);
  PS_CHECK(p0.get().status().code == ErrorCode::Cancelled);
  auto follower =
      std::async(std::launch::async, [&] { return demand.request(query); });
  PS_CHECK(await_shared(context, 0));
  gate->open();
  PS_CHECK(p1.get().ok());
  auto joined = follower.get();
  PS_CHECK(joined.ok() && joined.value().diagnostics.shared_computations == 1);
  PS_CHECK(gate->entered == 2 && context.cache_statistics().in_flight == 0);
  // A frozen waiter retains the old bundle while latest publication is stale.
  auto frozen = demand.freeze().take_value();
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->released = 0;
    gate->release = false;
    gate->ignore_cancellation = false;
  }
  auto latest =
      std::async(std::launch::async, [&] { return demand.request(query); });
  PS_CHECK(gate->await(1));
  const auto shared_before = context.cache_statistics().shared_computations;
  auto pinned = std::async(std::launch::async, [&] {
    return context.execute_fragments(frozen, query);
  });
  PS_CHECK(await_shared(context, shared_before));
  bindings.inputs[0].value = values<double>(ElementType::Float64, {8, 9});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  gate->open();
  PS_CHECK(latest.get().status().code == ErrorCode::Stale);
  auto old = pinned.get();
  double result = 0;
  PS_CHECK(old.ok() && old.value().values.at("y").read({0}, &result, 8).ok() &&
           result == 7);
  PS_CHECK(gate->entered == 1);
  auto current = demand.request(query);
  PS_CHECK(current.ok() &&
           current.value().values.at("y").read({0}, &result, 8).ok() &&
           result == 8);
  return 0;
}
int shared_terminal() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true, {}, true);
  auto document = gate_document();
  document.inputs[0].descriptor.shape = {3};
  document.inputs[0].region = Region::whole({3});
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096, 512});
  auto demand =
      context
          .open_demand(
              plan, {{{"x", values<double>(ElementType::Float64, {7, 8, 9})}}})
          .take_value();
  const auto wide = point(0, 3).unite(point(2, 3)).take_value();
  auto first = std::async(std::launch::async,
                          [&] { return demand.request({{"y", wide}}); });
  PS_CHECK(gate->await(1));
  auto same = std::async(std::launch::async,
                         [&] { return demand.request({{"y", wide}}); });
  PS_CHECK(await_shared(context, 0));
  auto subset = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(0, 3)}}); });
  PS_CHECK(gate->await(2));
  gate->open();
  auto a = first.get(), b = same.get(), c = subset.get();
  PS_CHECK(a.ok() && b.ok() && c.ok() && gate->entered == 2);
  double value = 0;
  for (const auto* result : {&a.value(), &b.value()}) {
    PS_CHECK(result->values.at("y").read({2}, &value, 8).ok() && value == 9);
    PS_CHECK(!result->values.at("y").read({1}, &value, 8).ok());
    PS_CHECK(!result->dependencies.restrict({{"y", point(0, 3)}}).ok());
    PS_CHECK(result->dependencies.coverage().at("y") == wide);
    PS_CHECK(result->dependencies.certificate(1).status().code ==
             ErrorCode::NotFound);
  }
  PS_CHECK(c.value().values.at("y").read({0}, &value, 8).ok() && value == 8);
  PS_CHECK(b.value().diagnostics.shared_computations == 1 &&
           c.value().diagnostics.shared_computations == 0);
  auto warm = demand.request({{"y", wide}});
  PS_CHECK(warm.ok() && warm.value().diagnostics.cache_hits == 1 &&
           gate->entered == 2);
  PS_CHECK(!warm.value().dependencies.restrict({{"y", point(0, 3)}}).ok());
  auto changed = demand.replace_bindings(
      {{{"x", values<double>(ElementType::Float64, {7, 8, 222})}}});
  PS_CHECK(changed.ok() && changed.value().potential_dirty.at("y").empty());
  auto content_hit = demand.request({{"y", wide}});
  PS_CHECK(content_hit.ok() &&
           content_hit.value().diagnostics.cache_hits == 1 &&
           gate->entered == 2);
  PS_CHECK(content_hit.value().values.at("y").read({2}, &value, 8).ok() &&
           value == 9);
  return 0;
}
int cache_work_and_epoch() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true);
  auto document = gate_document();
  for (std::uint64_t i = 2; i <= 64; ++i)
    document.nodes.push_back(
        {i, "wait", {WorkflowNodeOutput{i - 1, "value"}}, {}});
  document.outputs = {{"y", 64, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 65536, 32768});
  auto demand =
      context
          .open_demand(plan,
                       {{{"x", values<double>(ElementType::Float64, {7, 9})}}})
          .take_value();
  gate->open();
  ExecutionOptions options;
  options.maximum_dependency_cache_work = 1;
  auto bounded = demand.request({{"y", point(0, 2)}}, {}, options);
  PS_CHECK(bounded.ok() && gate->entered == 64);
  PS_CHECK(bounded.value().diagnostics.dependency_cache_records_visited == 0 &&
           bounded.value().diagnostics.dependency_cache_work == 1);
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  // A real outstanding producer captured the old epoch. Its successful late
  // completion cannot repopulate a cleared cache, even with a live waiter.
  GraphContext short_graph(gate_document());
  auto short_plan = Compiler(registry).compile(short_graph).take_value().plan;
  auto short_demand =
      context
          .open_demand(short_plan,
                       {{{"x", values<double>(ElementType::Float64, {7, 9})}}})
          .take_value();
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->release = false;
    gate->entered = 0;
  }
  auto pending = std::async(std::launch::async, [&] {
    return short_demand.request({{"y", point(0, 2)}});
  });
  PS_CHECK(gate->await(1));
  context.clear_result_cache();
  gate->open();
  PS_CHECK(pending.get().ok() &&
           context.cache_statistics().retained_bytes == 0);
  PS_CHECK(short_demand.request({{"y", point(0, 2)}})
               .value()
               .diagnostics.cache_hits == 0);
  PS_CHECK(short_demand.request({{"y", point(0, 2)}})
               .value()
               .diagnostics.cache_hits == 1);
  PS_CHECK(gate->entered == 2);
  return 0;
}
int cached_whole_ancestor() {
  auto gate = std::make_shared<Gate>();
  gate->open();
  auto registry = gated_registry(gate, false, {}, false, true);
  auto document = gate_document();
  document.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
  document.outputs = {{"y", 2, "value"}, {"ancestor", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096, 512});
  auto demand =
      context
          .open_demand(plan,
                       {{{"x", values<double>(ElementType::Float64, {7, 9})}}})
          .take_value();
  auto first = demand.request({{"y", point(0, 2)}});
  PS_CHECK(first.ok() && gate->entered == 2);
  auto warm = demand.request({{"y", point(0, 2)}});
  PS_CHECK(warm.ok() && gate->entered == 2 &&
           warm.value().diagnostics.cache_hits == 1);
  PS_CHECK(warm.value().diagnostics.operation_timings.empty());
  PS_CHECK(warm.value().dependencies.source_support().value().at("x") ==
           Footprint::all({2}).take_value());
  double value = 0;
  PS_CHECK(warm.value().values.at("y").read({0}, &value, 8).ok() && value == 7);
  // The root's 16-byte result survives while its Whole ancestor's pixels
  // are evicted. Importing its evidence must not block a later actual read.
  document.outputs = {{"a_desc", 2, "value"}, {"z_ancestor", 1, "value"}};
  GraphContext named_graph(document);
  auto named_plan = Compiler(registry).compile(named_graph).take_value().plan;
  ExecutionContext tight(registry, {1, false, 8, 4096, 16});
  auto query =
      tight
          .open_demand(named_plan,
                       {{{"x", values<double>(ElementType::Float64, {7, 9})}}})
          .take_value();
  PS_CHECK(query.request({{"a_desc", point(0, 2)}}).ok());
  // A -> B -> C: the 16-byte LRU retains C and evicts B pixels. The
  // active C subscription must still transpose an A edit through B evidence.
  const auto old_bundle = query.freeze().take_value();
  auto change = query.replace_bindings(
      {{{"x", values<double>(ElementType::Float64, {11, 9})}}});
  PS_CHECK(change.ok() &&
           change.value().potential_dirty.at("a_desc") == point(0, 2));
  auto updated = query.request({{"a_desc", point(0, 2)}});
  PS_CHECK(updated.ok() &&
           updated.value().values.at("a_desc").read({0}, &value, 8).ok() &&
           value == 11);
  auto pinned = tight.execute_fragments(old_bundle, {{"a_desc", point(0, 2)}});
  PS_CHECK(pinned.ok() &&
           pinned.value().values.at("a_desc").read({0}, &value, 8).ok() &&
           value == 7);
  // Restore the current descendant to the single-entry LRU before asking for
  // both nodes; the old frozen computation must not replace current evidence.
  PS_CHECK(query.request({{"a_desc", point(0, 2)}}).ok());
  const auto previous = gate->entered;
  auto both =
      query.request({{"a_desc", point(0, 2)}, {"z_ancestor", point(1, 2)}});
  PS_CHECK(both.ok() && both.value().diagnostics.cache_hits == 1 &&
           gate->entered == previous + 1);
  PS_CHECK(both.value().values.at("z_ancestor").read({1}, &value, 8).ok() &&
           value == 9);
  PS_CHECK(both.value().dependencies.source_support().value().at("x") ==
           Footprint::all({2}).take_value());
  return 0;
}
int cache_snapshot_bits() {
  auto registry = make_default_operation_registry();
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = Value::element_size(type);
    std::vector<std::uint8_t> bytes(width * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::uint8_t>(255 - i);
    auto reversed =
        Value::create({type, {3}}, Region::whole({3}),
                      {width * 2, {-static_cast<std::int64_t>(width)}}, bytes)
            .take_value();
    auto writer = MutableValue::allocate({type, {3}}, Region::whole({3}),
                                         BufferAllocator{})
                      .take_value();
    for (std::size_t i = 0; i < 3; ++i)
      std::memcpy(writer.data() + i * width, bytes.data() + (2 - i) * width,
                  width);
    auto packed = std::move(writer).publish().take_value();
    auto document = typed_images::document(packed);
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, {1, false, 8, 4096, 512});
    auto demand = context.open_demand(plan, {{{"image", packed}}}).take_value();
    const auto q = point(0, 3).unite(point(2, 3)).take_value();
    auto initial = demand.request({{"result", q}});
    PS_CHECK(initial.ok() && initial.value().diagnostics.cache_hits == 0);
    InputSnapshotStore store({1024, 1});
    auto snapshot = std::make_shared<const InputSnapshot>(
        store.import_value(reversed).take_value());
    auto edited = demand.replace_bindings({{{"image", {}, {}, snapshot}}});
    PS_CHECK(edited.ok() &&
             edited.value().potential_dirty.at("result").empty());
    auto same = demand.request({{"result", q}});
    PS_CHECK(same.ok() && same.value().diagnostics.cache_hits == 2);
    for (auto at : {0U, 2U}) {
      std::uint8_t observed[8]{};
      PS_CHECK(
          same.value().values.at("result").read({at}, observed, width).ok());
      PS_CHECK(std::memcmp(observed, bytes.data() + (2 - at) * width, width) ==
               0);
    }
  }
  return 0;
}
int content_cache() {
  auto registry = make_default_operation_registry();
  GraphContext graph(scatter_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config{1, false, 8, 4096};
  config.result_cache_bytes = 2048;
  ExecutionContext context(registry, config);
  ExecutionBindings bindings{
      {{"data", values<double>(ElementType::Float64, {1, 2, 3, 0, 5})},
       {"radius", values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0})}}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const auto q = point(0).unite(point(4)).take_value();
  DemandQuery query{{"sum", q}};
  auto first = demand.request(query);
  PS_CHECK(first.ok() && first.value().diagnostics.cache_hits == 0);
  auto frozen = demand.freeze().take_value();
  auto warm = demand.request(query);
  PS_CHECK(warm.ok() && warm.value().diagnostics.cache_hits == 2 &&
           warm.value().diagnostics.operation_timings.empty());
  bindings.inputs[0].value =
      values<double>(ElementType::Float64, {1, 2, 777, 0, 5});
  PS_CHECK(demand.replace_bindings(bindings)
               .value()
               .potential_dirty.at("sum")
               .empty());
  auto unchanged = demand.request(query);
  PS_CHECK(unchanged.ok() && unchanged.value().diagnostics.cache_hits == 2);
  PS_CHECK(unchanged.value().dependencies.certificate(1).value().identity() !=
           first.value().dependencies.certificate(1).value().identity());
  // Cached and freshly computed rows must merge under the current identity.
  auto mixed = demand.request({{"sum", point(0).unite(point(1)).take_value()}});
  PS_CHECK(mixed.ok() && mixed.value().diagnostics.cache_hits == 1);
  bindings.inputs[1].value =
      values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto changed_relation = demand.request(query);
  PS_CHECK(changed_relation.ok() &&
           changed_relation.value().diagnostics.cache_hits == 0);
  double answer = 0;
  PS_CHECK(
      changed_relation.value().values.at("sum").read({0}, &answer, 8).ok() &&
      answer == 1);
  PS_CHECK(changed_relation.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum") == q);
  PS_CHECK(demand.request(query).value().diagnostics.cache_hits == 2);
  auto old = context.execute_fragments(frozen, query);
  PS_CHECK(old.ok() && old.value().diagnostics.cache_hits == 2);
  PS_CHECK(old.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum")
               .empty());
  bindings.inputs[0].value =
      values<double>(ElementType::Float64, {1, 2, 777, 9, 5});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto latest = demand.request(query);
  PS_CHECK(latest.ok() && latest.value().diagnostics.cache_hits == 0);
  PS_CHECK(latest.value().values.at("sum").read({4}, &answer, 8).ok() &&
           answer == 14);
  context.clear_result_cache();
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  PS_CHECK(latest.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum") == q);
  PS_CHECK(demand.request(query).value().diagnostics.cache_hits == 0);
  config.result_cache_bytes = 16;
  ExecutionContext small(registry, config);
  auto limited = small.open_demand(plan, bindings).take_value();
  PS_CHECK(limited.request({{"sum", point(0)}}).ok());
  auto all = limited.request({{"sum", Footprint::all({5}).take_value()}});
  PS_CHECK(all.ok() && small.cache_statistics().evictions > 0);
  PS_CHECK(all.value()
               .dependencies.potential_dirty("radius", point(3))
               .value()
               .at("sum") == Footprint::all({5}).take_value());
  bindings.inputs[0].value =
      values<double>(ElementType::Float64, {1, 2, 777, 10, 5});
  auto edit = limited.replace_bindings(bindings);
  PS_CHECK(edit.ok() && edit.value().potential_dirty.at("sum") ==
                            Footprint::all({5}).take_value());
  return 0;
}
int joint_failure_isolation() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  ExecutionBindings bindings{
      {{"x", values<double>(ElementType::Float64,
                            {1, std::numeric_limits<double>::infinity()})}}};
  auto demand = context.open_demand(plan, bindings).take_value();
  auto joint = std::async(std::launch::async, [&] {
    return demand.request({{"y", Footprint::all({2}).take_value()}});
  });
  PS_CHECK(gate->await(1));
  auto narrow = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(await_shared(context, 0));
  gate->open();
  auto failed = joint.get(), survived = narrow.get();
  PS_CHECK(failed.status().code == ErrorCode::OperationFailed &&
           failed.status().message == "nonfinite sample 1");
  double value = 0;
  PS_CHECK(survived.ok() &&
           survived.value().values.at("y").read({0}, &value, 8).ok() &&
           value == 1);
  PS_CHECK(gate->entered == 2 &&
           survived.value().diagnostics.shared_computations == 1);
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  bindings.inputs[0].value = values<double>(
      ElementType::Float64, {std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::infinity()});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto later_atom = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(1, 2)}}); });
  PS_CHECK(gate->await(1));
  auto ordered = std::async(std::launch::async, [&] {
    return demand.request({{"y", Footprint::all({2}).take_value()}});
  });
  PS_CHECK(gate->await(2));
  gate->allow(1);
  PS_CHECK(later_atom.get().status().message == "nonfinite sample 1");
  gate->open();
  PS_CHECK(ordered.get().status().message == "nonfinite sample 0");
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(combined_tokens() == 0);
  PS_CHECK(generations() == 0);
  PS_CHECK(edit_and_cancel() == 0);
  PS_CHECK(isolation_and_limits() == 0);
  PS_CHECK(snapshot_replacement() == 0);
  PS_CHECK(owner_retirement() == 0);
  PS_CHECK(shared_ancestors() == 0);
  PS_CHECK(auxiliary_cancellation() == 0);
  PS_CHECK(impure_ancestor() == 0);
  PS_CHECK(shared_fallback() == 0);
  PS_CHECK(late_flight_and_frozen() == 0);
  PS_CHECK(shared_terminal() == 0);
  PS_CHECK(joint_failure_isolation() == 0);
  PS_CHECK(content_cache() == 0);
  PS_CHECK(cache_work_and_epoch() == 0);
  PS_CHECK(cache_snapshot_bits() == 0);
  PS_CHECK(cached_whole_ancestor() == 0);
  return 0;
}
