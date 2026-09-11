#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "execution/dependency_content.hpp"
#include "execution/memory_budget.hpp"
#include "execution/result_cache.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"
#include "support/typed_images.hpp"

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
int dynamic_opaque_preservation() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto operations = std::make_shared<OperationRegistry>();
  OperationTraits producer;
  producer.input_count = 1;
  producer.input_schema.resize(1);
  const auto opaque =
      Value::create({ElementType::Float64, {1}}, Region::whole({1}), {0, {8}},
                    Value::from_float64(2).copy_bytes(),
                    {{"vendor.test", 1, {42}}})
          .take_value();
  PS_CHECK(operations
               ->register_operation({"source", producer,
                                     [opaque](const OperationInvocation&) {
                                       return Result<Value>(opaque);
                                     }})
               .ok());
  auto identity = make_default_operation_registry()
                      ->find_traits("core.identity")
                      .take_value();
  PS_CHECK(operations
               ->register_operation({"identity", identity,
                                     [](const OperationInvocation& call) {
                                       return Result<Value>(call.inputs[0]);
                                     }})
               .ok());
  PS_CHECK(operations->freeze().ok());
  auto image = typed_images::value(typed_images::descriptions().front());
  auto document = typed_images::document(image);
  document.nodes = {{1, "source", {WorkflowInputReference{1}}, {}},
                    {2, "identity", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"result", 2, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(operations).compile(graph).take_value().plan;
  InputSnapshotStore store;
  ExecutionBindings bindings{{{"image",
                               {},
                               {},
                               std::make_shared<InputSnapshot>(
                                   store.import_value(image).take_value())}}};
  ExecutionContext execution(operations, {1, false, 8, 65536, 8192});
  for (bool warm : {false, true}) {
    auto result = execution.execute(plan, bindings);
    PS_CHECK(result.ok());
    PS_CHECK(typed_images::same(result.value().values.at("result"), opaque));
    PS_CHECK(warm ? result.value().diagnostics.cache_hits > 0
                  : result.value().diagnostics.cache_hits == 0);
  }
  return 0;
}
int stored_contract() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto budget = std::make_shared<execution_internal::MemoryBudget>(65536);
  execution_internal::ResultCache cache(8192, budget, 1, 4);
  auto semantic = typed_images::descriptions().front();
  auto image = typed_images::value(semantic);
  PlanStep step;
  step.output_descriptor = image.descriptor();
  step.output_demand = image.region();
  step.output_facets = image.facets();
  step.traits.output_semantic_rule = OperationSemanticRule::PreserveInput;
  cache.put("image", image, cache.epoch());
  auto retained = cache.get_output("image", step, {});
  PS_CHECK(retained.ok() && typed_images::same(retained.value(), image));
  auto cancelled =
      cache.get_output("image", step, [] { return ErrorCode::Cancelled; });
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled);
  semantic.reference = "display";
  cache.put("wrong-facet", typed_images::value(semantic), cache.epoch());
  PS_CHECK(cache.get_output("wrong-facet", step, {}).status().code ==
           ErrorCode::OperationFailed);
  auto bytes = image.copy_bytes();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(bytes.data(), &nan, 4);
  auto invalid = Value::create(image.descriptor(), image.region(),
                               image.layout(), bytes, image.facets())
                     .take_value();
  cache.put("invalid-sample", invalid, cache.epoch());
  PS_CHECK(cache.get_output("invalid-sample", step, {}).status().code ==
           ErrorCode::OperationFailed);
  return 0;
}
int dependency_content_bits() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = Value::element_size(type);
    std::vector<std::uint8_t> bytes(width * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::uint8_t>(255 - i);
    auto reversed =
        Value::create({type, {3}}, Region::whole({3}),
                      {2 * width, {-static_cast<std::int64_t>(width)}}, bytes)
            .take_value();
    InputSnapshotStore store({1024, 1});
    const auto snapshot = store.import_value(reversed).take_value();
    for (const auto& box : {Region::whole({3}), Region({{2, 1}})})
      PS_CHECK(dependency_value_identity(reversed, box, {}).value() ==
               snapshot.content_identity(box).value());
    const std::map<std::string, Footprint> support{
        {"x", Footprint::all({3}).take_value()}};
    std::uint64_t work = 1000;
    auto a = dependency_content_identity({{"x", reversed}}, support, &work, {});
    work = 1000;
    auto b = dependency_content_identity(
        {{"x", {}, {}, std::make_shared<const InputSnapshot>(snapshot)}},
        support, &work, {});
    PS_CHECK(a.ok() && b.ok() && a.value() == b.value());
  }
  auto opaque =
      Value::create({ElementType::UInt8, {1}}, Region::whole({1}), {0, {1}},
                    {42},
                    {{"vendor.proof", 1, std::vector<std::uint8_t>(1000)}})
          .take_value();
  std::uint64_t work = 100;
  auto bounded = dependency_content_identity(
      {{"x", opaque}}, {{"x", Footprint::all({1}).take_value()}}, &work, {});
  PS_CHECK(bounded.status().code == ErrorCode::ResourceExhausted && work == 0);
  return 0;
}
int dependency_cache_storage() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  auto budget = std::make_shared<MemoryBudget>(256);
  ResultCache cache(24, budget, 1, 4);
  const ValueDescriptor descriptor{ElementType::Float32, {4}};
  const auto make = [&](Region region) {
    auto reservation =
        budget->reserve(region.element_count().value() * 4).take_value();
    auto writer =
        MutableValue::allocate(descriptor, region, reservation->allocator())
            .take_value();
    for (std::uint64_t i = 0; i < region.dimensions()[0].extent; ++i) {
      const float value =
          static_cast<float>(region.dimensions()[0].offset + i + 1);
      std::memcpy(writer.data() + i * 4, &value, 4);
    }
    reservation->seal();
    return std::move(writer).publish().take_value();
  };
  const auto manifest = [&](const std::vector<Value>& pixels) {
    auto entry = std::make_shared<DependencyCacheManifest>();
    entry->descriptor = descriptor;
    entry->outputs = Footprint::all({4}).take_value();
    entry->content_identity = "same bytes";
    entry->epoch = cache.epoch();
    entry->metadata_entries = 1;
    for (const auto& value : pixels)
      entry->fragment_keys.push_back(
          dependency_fragment_key("plan", "same bytes", value.region()));
    return entry;
  };
  // First fragment retains 16 backing bytes despite its 8-byte logical region.
  auto large = make(Region::whole({4}));
  const std::vector<Value> old{large.view(Region({{0, 2}})).take_value(),
                               make(Region({{2, 2}}))};
  auto previous = manifest(old);
  cache.put_dependency("plan", previous, old);
  cache.put("unrelated", make(Region({{2, 2}})), cache.epoch());
  PS_CHECK(!cache.get(previous->fragment_keys[0]).valid());
  PS_CHECK(cache.get(previous->fragment_keys[1]).valid());
  // Only old [2,4) survives. Recompute with [0,3)+[3,4), same logical result.
  const std::vector<Value> next{make(Region({{0, 3}})), make(Region({{3, 1}}))};
  auto current = manifest(next);
  PS_CHECK(current->fragment_keys[1] != previous->fragment_keys[1]);
  cache.put_dependency("plan", current, next);
  auto retained = cache.dependency_values(*current);
  auto result =
      ValueFragments::create(descriptor, {}, current->outputs, retained);
  PS_CHECK(result.ok());
  for (std::uint64_t i = 0; i < 4; ++i) {
    float value = 0;
    PS_CHECK(result.value().read({i}, &value, 4).ok() && value == i + 1);
  }
  const auto epoch = current->epoch;
  cache.clear();
  cache.put_dependency("plan", current, next);
  PS_CHECK(cache.epoch() != epoch &&
           cache.dependency_candidates("plan").empty());
  PS_CHECK(cache.statistics().retained_bytes == 0);
  return 0;
}
int dependency_cache_proof_limits() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition leaf;
  leaf.key = "leaf";
  leaf.traits.output_element_type = ElementType::Float32;
  leaf.traits.input_count = 1;
  leaf.traits.input_schema.resize(1);
  leaf.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  leaf.traits.region_rule = OperationRegionRule::Elementwise;
  leaf.callback = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
  };
  auto merge = leaf;
  merge.key = "merge";
  merge.traits.input_count = 4;
  merge.traits.input_schema.resize(4);
  PS_CHECK(registry->register_operation(leaf).ok());
  PS_CHECK(registry->register_operation(merge).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "x", {ElementType::Float32, {1}}, Region::whole({1}), {0, {4}}, {}}};
  document.nodes = {{1, "leaf", {WorkflowInputReference{1}}, {}}};
  std::vector<WorkflowInput> siblings;
  for (std::uint64_t i = 2; i <= 5; ++i) {
    document.nodes.push_back({i, "leaf", {WorkflowNodeOutput{1, "value"}}, {}});
    siblings.push_back(WorkflowNodeOutput{i, "value"});
  }
  document.nodes.push_back({6, "merge", siblings, {}});
  document.outputs = {{"y", 6, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  const auto q = Footprint::all({1}).take_value();
  std::vector<DependencyTag> tags;
  for (std::uint64_t i = 0; i < 100; ++i)
    tags.push_back({1, i});
  const auto certificate =
      DependencyCertificate::create("certificate", q, {{1}},
                                    {{{0}, {{0, 7, q, tags}}}})
          .take_value();
  std::vector<std::shared_ptr<const DependencyRecord>> branches;
  for (std::size_t i = 1; i <= 4; ++i) {
    // Distinct actual E owners with equal semantic identity, as recomputation
    // under a pixel cache too small for E's backing storage can produce.
    auto source = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                    DependencyRecord::retire);
    source->step = 0;
    source->identity = "equal observation";
    source->samples = q;
    source->certificate = certificate;
    auto branch = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                    DependencyRecord::retire);
    branch->step = i;
    branch->identity = "branch " + std::to_string(i);
    branch->samples = q;
    branch->certificate =
        DependencyCertificate::create(branch->identity, q, {{1}},
                                      {{{0}, {{0, 1, q, {}}}}})
            .take_value();
    branch->upstream.push_back(source);
    branches.push_back(branch);
  }
  auto root = std::shared_ptr<DependencyRecord>(new DependencyRecord(),
                                                DependencyRecord::retire);
  root->step = 5;
  root->identity = "root";
  root->samples = q;
  root->certificate =
      DependencyCertificate::create(
          "root", q, {{1}, {1}, {1}, {1}},
          {{{0}, {{0, 1, q, {}}, {1, 1, q, {}}, {2, 1, q, {}}, {3, 1, q, {}}}}})
          .take_value();
  root->upstream = branches;
  std::uint64_t work = 100000, visits = 0;
  auto proof = dependency_cache_proof(plan, root, 10000, &work, &visits, {});
  PS_CHECK(proof.ok() && visits == 9 && proof.value().metadata_entries > 1200);
  PS_CHECK(proof.value().support.at("x") == q);
  work = 100000;
  visits = 0;
  PS_CHECK(!dependency_cache_proof(plan, root, 1200, &work, &visits, {}).ok());
  // Compare the projected source set with public execution's independently
  // assembled evidence for this diamond. No hidden implementation exports.
  ExecutionContext context(registry);
  auto frozen = context.freeze(plan, {{{"x", scalar(7)}}}).take_value();
  auto computed = context.execute_fragments(frozen, {{"y", q}});
  PS_CHECK(computed.ok() &&
           computed.value().dependencies.source_support().value() ==
               proof.value().support);
  work = 1;
  visits = 0;
  for (unsigned i = 0; i < 100; ++i)
    PS_CHECK(
        !dependency_cache_proof(plan, root, 10000, &work, &visits, {}).ok());
  PS_CHECK(work == 0 && visits == 0);
  return 0;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(stored_contract() == 0);
  PS_CHECK(dependency_cache_storage() == 0);
  PS_CHECK(dependency_content_bits() == 0);
  PS_CHECK(dependency_cache_proof_limits() == 0);
  PS_CHECK(dynamic_opaque_preservation() == 0);
  const std::vector<std::uint64_t> shape{5, 7, 4};
  std::vector<std::uint8_t> bytes(5 * 7 * 16);
  float half = .5F;
  for (std::size_t i = 0; i < bytes.size(); i += 4)
    std::memcpy(bytes.data() + i, &half, 4);

  auto image =
      Value::create({ElementType::Float32, shape}, Region::whole(shape),
                    {0, {112, 16, 4}}, bytes,
                    {ps::encode_semantic(ps::rgba_semantics()).take_value()})
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
  traits.output_semantic_rule = OperationSemanticRule::PreserveInput;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.region_rule = OperationRegionRule::Elementwise;
  traits.input_schema = {{OperationPortKind::RgbaFloat32, 0, 0}};
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
                    {0, {4, 4}}, scalar(.5F).copy_bytes(),
                    {encode_semantic(coverage_semantics()).take_value()})
          .take_value();
  WorkflowDocument tiny;
  tiny.inputs = {{1, "mask", tiny_mask.descriptor(), tiny_mask.region(),
                  tiny_mask.layout(), tiny_mask.facets()}};
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
