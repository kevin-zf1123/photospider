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
int cache_shared_route_budget() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition leaf;
  leaf.key = "cache.whole";
  leaf.traits.input_count = 1;
  leaf.traits.input_schema.resize(1);
  leaf.traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  leaf.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  leaf.callback = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
  };
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto merge = leaf;
  merge.key = "cache.merge";
  merge.traits.input_count = 2;
  merge.traits.input_schema.resize(2);
  merge.callback = [calls](const OperationInvocation& call) {
    ++*calls;
    return Result<Value>(
        Value::from_float64(call.inputs[0].as_float64().value() +
                            call.inputs[1].as_float64().value()));
  };
  auto branch1 = leaf, branch2 = leaf;
  branch1.key = "cache.branch1";
  branch2.key = "cache.branch2";
  PS_CHECK(registry->register_operation(leaf).ok());
  PS_CHECK(registry->register_operation(branch1).ok());
  PS_CHECK(registry->register_operation(branch2).ok());
  PS_CHECK(registry->register_operation(merge).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "x", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}}};
  document.nodes = {
      {1, leaf.key, {WorkflowInputReference{1}}, {}},
      {2, branch1.key, {WorkflowNodeOutput{1, "value"}}, {}},
      {3, branch2.key, {WorkflowNodeOutput{1, "value"}}, {}},
      {4,
       merge.key,
       {WorkflowNodeOutput{2, "value"}, WorkflowNodeOutput{3, "value"}},
       {}}};
  document.outputs = {{"y", 4, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext execution(registry, {1, false, 64, 1048576, 65536});
  auto frozen =
      execution.freeze(plan, {{{"x", Value::from_float64(7)}}}).take_value();
  const DemandQuery query{{"y", Footprint::all({1}).take_value()}};
  PS_CHECK(execution.execute_fragments(frozen, query).ok() && *calls == 1);
  // Split the old shared Whole owner into two equivalent new source nodes.
  // The root content matches, but its proof must not be cloned twice against
  // a single prepaid metadata charge. Safe miss still reuses each branch.
  document.nodes = {
      {10, leaf.key, {WorkflowInputReference{1}}, {}},
      {11, leaf.key, {WorkflowInputReference{1}}, {}},
      {20, branch1.key, {WorkflowNodeOutput{10, "value"}}, {}},
      {21, branch2.key, {WorkflowNodeOutput{11, "value"}}, {}},
      {30,
       merge.key,
       {WorkflowNodeOutput{20, "value"}, WorkflowNodeOutput{21, "value"}},
       {}}};
  document.outputs = {{"y", 30, "value"}};
  GraphContext changed(document);
  auto changed_plan = Compiler(registry).compile(changed).take_value().plan;
  auto changed_frozen =
      execution.freeze(changed_plan, {{{"x", Value::from_float64(7)}}})
          .take_value();
  auto result = execution.execute_fragments(changed_frozen, query);
  PS_CHECK(result.ok() && *calls == 2 &&
           result.value().diagnostics.cache_hits > 0);
  double actual = 0;
  PS_CHECK(
      result.value().values.at("y").read({0}, &actual, sizeof(actual)).ok() &&
      actual == 14);
  auto dirty = result.value().dependencies.potential_dirty("x", query.at("y"));
  PS_CHECK(dirty.ok() && dirty.value().at("y") == query.at("y"));
  return 0;
}
int dependency_cache_proof_limits() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition leaf;
  leaf.key = "leaf";
  leaf.traits.outputs[0].output_element_type = ElementType::Float32;
  leaf.traits.input_count = 1;
  leaf.traits.input_schema.resize(1);
  leaf.traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  leaf.traits.outputs[0].region_rule = OperationRegionRule::Elementwise;
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
  PS_CHECK(dependency_content_bits() == 0);
  PS_CHECK(dependency_cache_storage() == 0);
  PS_CHECK(cache_shared_route_budget() == 0);
  PS_CHECK(dependency_cache_proof_limits() == 0);
  return 0;
}
