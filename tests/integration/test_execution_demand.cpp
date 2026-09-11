#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
};
std::shared_ptr<OperationRegistry> gated_registry(
    const std::shared_ptr<Gate>& gate) {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "wait";
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  op.traits.region_rule = OperationRegionRule::Elementwise;
  op.callback = [gate](const OperationInvocation& call) -> Result<Value> {
    ++gate->active;
    struct Active {
      Gate* gate;
      ~Active() { --gate->active; }
    } active{gate.get()};
    {
      std::unique_lock<std::mutex> lock(gate->mutex);
      ++gate->entered;
      gate->changed.notify_all();
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!gate->release && !call.cancellation.cancelled()) {
        if (std::chrono::steady_clock::now() >= deadline)
          break;
        gate->changed.wait_for(lock, std::chrono::milliseconds(2));
      }
      if (!gate->release && !call.cancellation.cancelled())
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
      return Result<Value>(
          Status{ErrorCode::OperationFailed, "nonfinite sample"});
    auto output = MutableValue::allocate(call.inputs[0].descriptor(),
                                         call.output_region, call.allocator)
                      .take_value();
    std::memcpy(output.data(), &value, 8);
    return std::move(output).publish();
  };
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
}  // namespace
int main() {
  PS_CHECK(combined_tokens() == 0);
  PS_CHECK(generations() == 0);
  PS_CHECK(edit_and_cancel() == 0);
  PS_CHECK(isolation_and_limits() == 0);
  PS_CHECK(snapshot_replacement() == 0);
  PS_CHECK(owner_retirement() == 0);
  return 0;
}
