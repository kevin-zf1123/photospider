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
  return 0;
}
