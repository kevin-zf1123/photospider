#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "execution/memory_budget.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
ps::Value scalar(float number) {
  std::vector<std::uint8_t> bytes(4);
  std::memcpy(bytes.data(), &number, 4);
  return ps::Value::create({ps::ElementType::Float32, {1}},
                           ps::Region::whole({1}), {0, {4}}, std::move(bytes))
      .take_value();
}
std::uint64_t calls(const ps::ExecutionResult& result, std::uint64_t id) {
  std::uint64_t n = 0;
  for (const auto& t : result.diagnostics.operation_timings)
    if (t.node_id == id)
      n += t.invocation_count;
  return n;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  const std::vector<std::uint64_t> shape{5, 7, 4};
  std::vector<std::uint8_t> bytes(5 * 7 * 16);
  float half = .5F;
  for (std::size_t i = 0; i < bytes.size(); i += 4)
    std::memcpy(bytes.data() + i, &half, 4);
  const std::string profile = "rgba;linear-srgb;premultiplied;hwc";
  auto image = Value::create(
                   {ElementType::Float32, shape}, Region::whole(shape),
                   {0, {112, 16, 4}}, bytes,
                   {{"photospider.image", 1, {profile.begin(), profile.end()}}})
                   .take_value();
  InputSnapshotStore store({8192, 2});
  auto snapshot = store.import_value(image).take_value();
  WorkflowDocument document;
  document.inputs = {
      {1, "image", image.descriptor(), image.region(), image.layout(),
       image.facets()},
      {2, "gain", scalar(2).descriptor(), Region::whole({1}), {0, {4}}, {}}};
  document.nodes = {
      {1,
       "image.gaussian_blur",
       {WorkflowInputReference{1}},
       {{"radius", INT64_C(1)}, {"sigma", 1.0}}},
      {2,
       "image.exposure_gain",
       {WorkflowNodeOutput{1, "value"}, WorkflowInputReference{2}},
       {}}};
  document.outputs = {{"result", 2, "value"}};
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  GraphContext graph(document);
  PlanningOptions options;
  options.tile_height = 2;
  options.tile_width = 3;
  auto plan = compiler.compile(graph, options).take_value().plan;
  ExecutionContext execution(registry, {2, false, 16, 1024 * 1024, 65536});
  ExecutionContext uncached(registry);
  ExecutionBindings bindings{
      {{"image", {}, {}, std::make_shared<InputSnapshot>(snapshot)},
       {"gain", scalar(2)}}};
  auto first = execution.execute(plan, bindings);
  PS_CHECK(first.ok() && calls(first.value(), 1) == 9);
  PS_CHECK(first.value().diagnostics.shared_peak_live_bytes > 0);
  bindings.inputs[1].value = scalar(3);
  auto second = execution.execute(plan, bindings);
  PS_CHECK(second.ok());
  PS_CHECK(calls(second.value(), 1) == 0 && calls(second.value(), 2) == 9);
  auto reference = uncached.execute(plan, bindings);
  PS_CHECK(reference.ok());
  PS_CHECK(second.value().values.at("result").copy_bytes() ==
           reference.value().values.at("result").copy_bytes());
  std::vector<std::uint8_t> pixel(16);
  float quarter = .25F;
  for (std::size_t i = 0; i < 16; i += 4)
    std::memcpy(pixel.data() + i, &quarter, 4);
  auto patch =
      Value::create(image.descriptor(), Region({{0, 1}, {0, 1}, {0, 4}}),
                    {0, {16, 16, 4}, {0, 0, 0}}, pixel, image.facets())
          .take_value();
  bindings.inputs[0].snapshot = std::make_shared<InputSnapshot>(
      store.patch(snapshot, patch).take_value());
  auto changed = execution.execute(plan, bindings);
  PS_CHECK(changed.ok());
  PS_CHECK(calls(changed.value(), 1) == 1 && calls(changed.value(), 2) == 1);
  auto oracle = uncached.execute(plan, bindings);
  PS_CHECK(oracle.ok() && changed.value().values.at("result").copy_bytes() ==
                              oracle.value().values.at("result").copy_bytes());
  document.nodes.push_back({99, "core.constant", {}, {{"value", 9.0}}});
  graph.replace(document);
  auto edited = compiler.compile(graph, options).take_value().plan;
  auto unrelated = execution.execute(edited, bindings);
  PS_CHECK(unrelated.ok() && calls(unrelated.value(), 1) == 0 &&
           calls(unrelated.value(), 2) == 0);
  PS_CHECK(execution.cache_statistics().retained_bytes <= 65536);
  execution.clear_result_cache();
  PS_CHECK(execution.cache_statistics().retained_bytes == 0);
  PS_CHECK(execution.execute(edited, bindings).ok());

  auto gated = std::make_shared<OperationRegistry>();
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, release = false;
  std::atomic<unsigned> invocations{0};
  CancellationToken observed;
  OperationTraits traits;
  traits.input_count = 1;
  traits.output_element_type = ElementType::Float32;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.region_rule = OperationRegionRule::Elementwise;
  traits.input_schema = {
      {OperationPortKind::LinearPremultipliedRgbaFloat32, 0, 0}};
  traits.output_schema = traits.input_schema[0];
  PS_CHECK(gated
               ->register_operation(
                   {"gated", traits,
                    [&](const OperationInvocation& invocation) {
                      ++invocations;
                      {
                        std::unique_lock<std::mutex> lock(mutex);
                        entered = true;
                        observed = invocation.cancellation;
                        cv.notify_all();
                        cv.wait(lock, [&] { return release; });
                      }
                      if (invocation.cancellation.cancelled())
                        return Result<Value>(Status::failure(
                            ErrorCode::Cancelled, "gate cancelled"));
                      return Result<Value>(invocation.inputs[0]);
                    }})
               .ok());
  auto gain_traits = traits;
  gain_traits.input_count = 2;
  gain_traits.input_schema.push_back({OperationPortKind::Float32Scalar, 0, 8});
  PS_CHECK(
      gated
          ->register_operation(
              {"gain", gain_traits,
               [](const OperationInvocation& invocation) {
                 auto output = MutableValue::allocate(
                     invocation.inputs[0].descriptor(),
                     invocation.output_region, invocation.allocator);
                 if (!output.ok())
                   return Result<Value>(output.status());
                 auto value = output.take_value();
                 float gain = 0;
                 std::memcpy(&gain, invocation.inputs[1].bytes().data(), 4);
                 auto bytes = invocation.inputs[0].copy_bytes();
                 for (std::size_t i = 0; i < bytes.size(); i += 4) {
                   float input = 0;
                   std::memcpy(&input, bytes.data() + i, 4);
                   if ((i / 4) % 4 != 3)
                     input *= gain;
                   std::memcpy(value.data() + i, &input, 4);
                 }
                 return std::move(value).publish(invocation.inputs[0].facets());
               }})
          .ok());
  PS_CHECK(gated->freeze().ok());
  WorkflowDocument d;
  d.inputs = {document.inputs[0]};
  d.nodes = {{1, "gated", {WorkflowInputReference{1}}, {}}};
  d.outputs = {{"result", 1, "value"}};
  GraphContext g(d);
  Compiler c(gated);
  auto p = c.compile(g).take_value().plan;
  ExecutionContext context(gated, {1, false, 8, 65536, 8192});
  ExecutionBindings b{{bindings.inputs[0]}};
  CancellationSource cancel;
  auto a = std::async(std::launch::async,
                      [&] { return context.execute(p, b, cancel.token()); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return entered; });
  }
  auto follower =
      std::async(std::launch::async, [&] { return context.execute(p, b); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (context.cache_statistics().shared_computations == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  PS_CHECK(context.cache_statistics().shared_computations == 1);
  cancel.cancel();
  PS_CHECK(a.get().status().code == ErrorCode::Cancelled);
  context.clear_result_cache();
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  PS_CHECK(follower.get().ok() && invocations == 1);
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  PS_CHECK(context.execute(p, b).ok());
  context.clear_result_cache();
  {
    std::lock_guard<std::mutex> lock(mutex);
    entered = false;
    release = false;
  }
  CancellationSource last_cancel;
  auto last = std::async(std::launch::async, [&] {
    return context.execute(p, b, last_cancel.token());
  });
  CancellationToken producer_token;
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return entered; });
    producer_token = observed;
  }
  last_cancel.cancel();
  const auto stop_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!producer_token.cancelled() &&
         std::chrono::steady_clock::now() < stop_deadline)
    std::this_thread::yield();
  PS_CHECK(producer_token.cancelled());
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  PS_CHECK(last.get().status().code == ErrorCode::Cancelled);
  PS_CHECK(context.cache_statistics().in_flight == 0);
  // Distinct terminal computations share a common running ancestor, including
  // independent cancellation and subscriber-local node IDs / output names.
  context.clear_result_cache();
  {
    std::lock_guard<std::mutex> lock(mutex);
    entered = false;
    release = false;
  }
  d.inputs.push_back(document.inputs[1]);
  d.nodes.push_back(
      {2,
       "gain",
       {WorkflowNodeOutput{1, "value"}, WorkflowInputReference{2}},
       {}});
  d.outputs = {{"scaled", 2, "value"}};
  GraphContext ga(d);
  auto pa = c.compile(ga).take_value().plan;
  d.nodes[0].id = 11;
  d.nodes[1].id = 12;
  d.nodes[1].inputs[0] = WorkflowNodeOutput{11, "value"};
  d.outputs = {{"other", 12, "value"}};
  GraphContext gb(d);
  auto pb = c.compile(gb).take_value().plan;
  auto ba = b, bb = b;
  ba.inputs.push_back({"gain", scalar(2)});
  bb.inputs.push_back({"gain", scalar(3)});
  const auto before = invocations.load();
  const auto shares_before = context.cache_statistics().shared_computations;
  CancellationSource cancel_a;
  auto running = std::async(std::launch::async, [&] {
    return context.execute(pa, ba, cancel_a.token());
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return entered; });
  }
  auto following =
      std::async(std::launch::async, [&] { return context.execute(pb, bb); });
  const auto shared_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (context.cache_statistics().shared_computations == shares_before &&
         std::chrono::steady_clock::now() < shared_deadline)
    std::this_thread::yield();
  PS_CHECK(context.cache_statistics().shared_computations == shares_before + 1);
  cancel_a.cancel();
  PS_CHECK(running.get().status().code == ErrorCode::Cancelled);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  auto shared_result = following.get();
  PS_CHECK(shared_result.ok() && invocations == before + 1);
  PS_CHECK(shared_result.value().diagnostics.selected_backends.count(1) == 0);
  PS_CHECK(shared_result.value().diagnostics.selected_backends.count(11) == 1);
  ExecutionContext plain(gated);
  auto expected = plain.execute(pb, bb);
  PS_CHECK(expected.ok());
  for (const auto& output : expected.value().values)
    PS_CHECK(output.second.copy_bytes() ==
             shared_result.value().values.at(output.first).copy_bytes());

  d.outputs.push_back({"original", 11, "value"});
  gb.replace(d);
  auto multi = c.compile(gb).take_value().plan;
  auto multi_result = context.execute(multi, bb);
  PS_CHECK(multi_result.ok() && multi_result.value().values.size() == 2);
  PS_CHECK(multi_result.value().values.at("other").copy_bytes() ==
           expected.value().values.at("other").copy_bytes());

  // Admission must reclaim entries created while it was waiting. Use exact
  // accounted bytes and a barrier in the reclaimer, without timing sleeps.
  auto budget = std::make_shared<execution_internal::MemoryBudget>(16);
  auto a_work = budget->reserve(8).take_value();
  auto make_mask = [](const BufferAllocator& allocator) {
    return MutableValue::allocate({ElementType::Float32, {1, 1}},
                                  Region::whole({1, 1}), allocator)
        .take_value();
  };
  auto retained_a = budget->reserve(4).take_value();
  auto a_result = make_mask(retained_a->allocator());
  retained_a->seal();
  auto retained_b = budget->reserve(4).take_value();
  auto b_result = make_mask(retained_b->allocator());
  retained_b->seal();
  auto cached_value = make_mask(a_work->allocator());
  std::atomic<unsigned> reclamations{0};
  bool first_reclaim = false, admission_continue = false;
  auto waiting = std::async(std::launch::async, [&] {
    return budget->reserve(
        8, [] { return ErrorCode::Ok; }, {},
        [&] {
          if (++reclamations == 1) {
            std::unique_lock<std::mutex> lock(mutex);
            first_reclaim = true;
            cv.notify_all();
            cv.wait(lock, [&] { return admission_continue; });
          } else {
            cached_value = {};
          }
        });
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return first_reclaim; });
    a_work->seal();
    admission_continue = true;
  }
  cv.notify_all();
  auto admitted = waiting.get();
  PS_CHECK(admitted.ok() && reclamations >= 2);
  admitted.value()->seal();

  auto tiny_mask =
      Value::create({ElementType::Float32, {1, 1}}, Region::whole({1, 1}),
                    {0, {4, 4}}, scalar(.5F).copy_bytes())
          .take_value();
  WorkflowDocument tiny;
  tiny.inputs = {{1,
                  "mask",
                  tiny_mask.descriptor(),
                  tiny_mask.region(),
                  tiny_mask.layout(),
                  {}}};
  tiny.nodes = {{1,
                 "mask.downsample_box",
                 {WorkflowInputReference{1}},
                 {{"factor", INT64_C(1)}}}};
  tiny.outputs = {{"mask", 1, "value"}};
  GraphContext tiny_graph(tiny);
  auto tiny_plan = compiler.compile(tiny_graph).take_value().plan;
  ExecutionBindings tiny_bindings{
      {{"mask",
        {},
        {},
        std::make_shared<InputSnapshot>(
            store.import_value(tiny_mask).take_value())}}};
  ExecutionContext exact(registry, {1, false, 8, 12, 4});
  for (int repeat = 0; repeat < 3; ++repeat) {
    auto tiny_result = exact.execute(tiny_plan, tiny_bindings);
    PS_CHECK(tiny_result.ok() &&
             tiny_result.value().values.at("mask").copy_bytes() ==
                 tiny_mask.copy_bytes());
    exact.clear_result_cache();
  }
  ExecutionContext insufficient(registry, {1, false, 8, 11, 4});
  PS_CHECK(insufficient.execute(tiny_plan, tiny_bindings).status().code ==
           ErrorCode::ResourceExhausted);

  // Freeze owns the handle value, even when the caller replaces its pointee.
  auto mutable_snapshot = std::make_shared<InputSnapshot>(snapshot);
  ExecutionBindings frozen_bindings{
      {{"image", {}, {}, mutable_snapshot}, {"gain", scalar(2)}}};
  auto frozen_input = execution.freeze(edited, frozen_bindings).take_value();
  auto frozen_before = execution.execute(frozen_input).take_value();
  *mutable_snapshot = store.patch(snapshot, patch).take_value();
  auto frozen_after = execution.execute(frozen_input).take_value();
  PS_CHECK(frozen_before.values.at("result").copy_bytes() ==
           frozen_after.values.at("result").copy_bytes());
  PS_CHECK(execution.execute(edited, frozen_bindings)
               .take_value()
               .values.at("result")
               .copy_bytes() != frozen_before.values.at("result").copy_bytes());
  return 0;
}
