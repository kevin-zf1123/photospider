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

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "execution/memory_budget.hpp"
#include "execution/result_cache.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
enum class CacheMode { Constant, First, Sum };
struct CacheProgram {
  CacheMode mode;
  std::shared_ptr<std::atomic<unsigned>> calls;
  bool requested = false;
  CacheProgram(CacheMode selected, std::shared_ptr<std::atomic<unsigned>> count)
      : mode(selected), calls(std::move(count)) {}
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    using namespace ps;  // NOLINT(build/namespaces)
    using multi_result::check;
    using multi_result::take;
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      for (std::uint32_t i = 0; i < phase.query.inputs.size(); ++i)
        need.tensors.push_back(
            {i, 0,
             take(Footprint::all(phase.query.inputs[i]
                                     .result_schema->tensors[0]
                                     .sample_shape())),
             9});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (calls)
      ++*calls;
    const auto& schema = *phase.query.output.result_schema;
    auto builder = take(ResultBuilder::start(phase.resources, schema,
                                             phase.query.semantic_key));
    std::vector<ResultRelation> descriptor, data;
    const auto count = take(schema.tensors[0].sample_count());
    for (std::uint32_t i = 0; i < phase.query.inputs.size(); ++i) {
      descriptor.push_back(take(ResultRelation::cartesian(
          phase.resources, 1,
          {i, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
      data.push_back(take(ResultRelation::cartesian(
          phase.resources, count,
          {i, 1, 0,
           take(phase.query.inputs[i].result_schema->tensors[0].sample_count()),
           ResultSupportTarget::Tensor, 0})));
    }
    check(builder.bind_descriptor_relation(
        take(ResultRelation::unite(phase.resources, descriptor))));
    auto relation = take(ResultRelation::unite(phase.resources, data));
    const auto region = Region::whole(schema.tensors[0].sample_shape());
    if (mode == CacheMode::First) {
      auto window = take(phase.tensors->at({0, 0}).acquire(region));
      ResultTensorViewTransform identity;
      identity.source_axes = {{0, 0, 1, 1}};
      check(builder.publish_tensor_view(0, region, window, identity,
                                        std::move(relation),
                                        {true, true, true, true}));
    } else {
      double value = mode == CacheMode::Constant ? 2 : 0;
      for (std::uint32_t i = 0; i < phase.query.inputs.size(); ++i) {
        const auto type = phase.query.inputs[i]
                              .result_schema->tensors[0]
                              .descriptor.element_type;
        if (type == ElementType::Float32) {
          float input = 0;
          check(phase.read_tensor(i, 0, {0}, &input, sizeof(input)));
          if (mode == CacheMode::Sum)
            value += input;
        } else {
          double input = 0;
          check(phase.read_tensor(i, 0, {0}, &input, sizeof(input)));
          if (mode == CacheMode::Sum)
            value += input;
        }
      }
      check(phase.consume_work(phase.query.inputs.size()));
      auto bytes = take(phase.allocator.allocate(8));
      std::memcpy(bytes.data(), &value, sizeof(value));
      check(builder.publish_tensor(
          0, region, {0, {8}}, std::move(bytes).freeze(), std::move(relation),
          {true, true, true, true}));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return ps::Result<ps::ResultProgramPoll>(failure.status);
  }
};
ps::OperationDefinition cache_operation(
    std::string key, const ps::SchemaTemplate& output,
    CacheMode mode = CacheMode::First, unsigned inputs = 1,
    std::shared_ptr<std::atomic<unsigned>> calls = {},
    ps::ElementType input_type = ps::ElementType::Float64) {
  ps::OperationDefinition operation;
  operation.key = std::move(key);
  operation.traits.input_count = inputs;
  operation.traits.input_schema.resize(inputs);
  for (auto& port : operation.traits.input_schema) {
    port.kind = ps::OperationPortKind::Result;
    port.element_type = static_cast<std::uint32_t>(input_type);
    port.rank = 1;
  }
  auto result = multi_result::output("value", output);
  result.region_rule = ps::OperationRegionRule::Whole;
  result.continuation_bytes = sizeof(CacheProgram);
  operation.traits.outputs = {std::move(result)};
  operation.traits.workspace_bytes = 8;
  operation.start_result = [mode, calls](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<CacheProgram>(allocator, mode, calls);
  };
  return operation;
}
ps::ExecutionContextConfig configuration(std::uint64_t bytes = 65536) {
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = bytes;
  config.managed_resources = ps::ResourceLimits{};
  return config;
}
int opaque_result_cache_preservation() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto operations = std::make_shared<OperationRegistry>();
  auto input_schema = multi_result::schema(ElementType::Float32);
  auto output_schema = multi_result::schema();
  output_schema.tensors[0].facets = {{"vendor.test", 1, {42}}};
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  PS_CHECK(operations
               ->register_operation(
                   cache_operation("source", output_schema, CacheMode::Constant,
                                   1, calls, ElementType::Float32))
               .ok());
  PS_CHECK(
      operations->register_operation(cache_operation("identity", output_schema))
          .ok());
  PS_CHECK(operations->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", input_schema)};
  document.nodes = {{1, "source", {WorkflowInputReference{1}}, {}},
                    {2, "identity", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(operations).compile(graph).take_value().plan;
  ExecutionContext execution(operations, configuration());
  auto root = execution.resource_budget().take_value();
  ExecutionBindings bindings{
      {multi_result::binding(root, "input", 1, input_schema)}};
  auto copied_document = document;
  copied_document.outputs = {{"result", 2, "value"}};
  GraphContext copied_graph(copied_document);
  auto copied_plan =
      Compiler(operations).compile(copied_graph).take_value().plan;
  for (bool warm : {false, true}) {
    auto result = execution.execute(plan, bindings);
    PS_CHECK(result.ok());
    const auto& output = result.value().results.at("result");
    PS_CHECK(multi_result::number(output) == 2);
    const auto& facets = output.schema().tensors[0].facets;
    PS_CHECK(facets.size() == 1 && facets[0].key == "vendor.test" &&
             facets[0].version == 1 &&
             facets[0].payload == std::vector<uint8_t>{42});
    PS_CHECK(warm ? result.value().diagnostics.cache_hits > 0
                  : result.value().diagnostics.cache_hits == 0);
    auto copied = execution.execute(copied_plan, bindings);
    PS_CHECK(copied.ok() &&
             multi_result::number(copied.value().results.at("result")) == 2);
    PS_CHECK(copied.value()
                 .results.at("result")
                 .schema()
                 .tensors[0]
                 .facets[0]
                 .payload == facets[0].payload);
    PS_CHECK(*calls == 1);
  }
  return 0;
}
int result_content_bits() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = Value::element_size(type);
    auto schema = multi_result::schema(type, {3});
    auto registry = std::make_shared<OperationRegistry>();
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    PS_CHECK(registry
                 ->register_operation(cache_operation(
                     "bits.identity", schema, CacheMode::First, 1, calls, type))
                 .ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument doc;
    doc.inputs = {multi_result::declaration(1, "x", schema)};
    doc.nodes = {{1, "bits.identity", {WorkflowInputReference{1}}, {}}};
    doc.outputs = {{"y", 1, "value"}};
    GraphContext graph(doc);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, configuration());
    auto root = context.resource_budget().take_value();
    std::vector<std::uint8_t> backing(width * 3), logical(width * 3);
    for (std::size_t i = 0; i < backing.size(); ++i)
      backing[i] = static_cast<std::uint8_t>(255 - i);
    for (std::size_t i = 0; i < 3; ++i)
      std::memcpy(logical.data() + i * width, backing.data() + (2 - i) * width,
                  width);
    auto reversed =
        Value::create({type, {3}}, Region::whole({3}),
                      {2 * width, {-static_cast<std::int64_t>(width)}}, backing)
            .take_value();
    const auto bind = [&](const Value& value) {
      return ExecutionBindings{
          {{"x", numeric_result_fixture::source(root, value, &schema)}}};
    };
    auto first = context.execute(plan, bind(reversed));
    PS_CHECK(first.ok() && *calls == 1 &&
             numeric_result_fixture::bytes(first.value().results.at("y")) ==
                 logical);
    auto dense = Value::create({type, {3}}, Region::whole({3}),
                               {0, {static_cast<std::int64_t>(width)}}, logical)
                     .take_value();
    auto equivalent = context.execute(plan, bind(dense));
    PS_CHECK(equivalent.ok() && *calls == 1 &&
             equivalent.value().diagnostics.cache_hits > 0 &&
             numeric_result_fixture::bytes(
                 equivalent.value().results.at("y")) == logical);
    logical[width] ^= 1;
    auto edited =
        Value::create({type, {3}}, Region::whole({3}),
                      {0, {static_cast<std::int64_t>(width)}}, logical)
            .take_value();
    auto changed = context.execute(plan, bind(edited));
    PS_CHECK(changed.ok() && *calls == 2 &&
             numeric_result_fixture::bytes(changed.value().results.at("y")) ==
                 logical);
    // Optional verification fuel never prevents the normal identity operation.
    ExecutionOptions bounded;
    bounded.maximum_dependency_cache_work = 1;
    auto uncached = context.execute(plan, bind(edited), {}, bounded);
    PS_CHECK(uncached.ok() && *calls == 3 &&
             uncached.value().diagnostics.cache_hits == 0 &&
             numeric_result_fixture::bytes(uncached.value().results.at("y")) ==
                 logical);
  }
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
  const auto schema = multi_result::schema();
  auto leaf = cache_operation("cache.whole", schema);
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto merge = cache_operation("cache.merge", schema, CacheMode::Sum, 2, calls);
  auto branch1 = cache_operation("cache.branch1", schema);
  auto branch2 = cache_operation("cache.branch2", schema);
  PS_CHECK(registry->register_operation(leaf).ok());
  PS_CHECK(registry->register_operation(branch1).ok());
  PS_CHECK(registry->register_operation(branch2).ok());
  PS_CHECK(registry->register_operation(merge).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "x", schema)};
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
  ExecutionContext execution(registry, configuration());
  auto resource_root = execution.resource_budget().take_value();
  auto frozen =
      execution
          .freeze(plan,
                  {{multi_result::binding(resource_root, "x", 7, schema)}})
          .take_value();
  const DemandQuery query{{"y", Footprint::all({1}).take_value()}};
  PS_CHECK(execution.execute_fragments(frozen, query).ok() && *calls == 1);
  // Split the old shared Whole owner into two equivalent new source nodes.
  // The unchanged root content may reuse cached pixels. Each corresponding
  // ancestry copy retains its own metadata admission for its new route.
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
      execution
          .freeze(changed_plan,
                  {{multi_result::binding(resource_root, "x", 7, schema)}})
          .take_value();
  auto result = execution.execute_fragments(changed_frozen, query);
  PS_CHECK(result.ok() && *calls == 1 &&
           result.value().diagnostics.cache_hits > 0);
  double actual = 0;
  PS_CHECK(numeric_result_fixture::read(result.value().results.at("y"), {0},
                                        &actual, sizeof(actual))
               .ok() &&
           actual == 14);
  auto dirty = result.value().dependencies.potential_dirty(
      "x", query.at("y"), 7, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(dirty.ok() && dirty.value().at("y") == query.at("y"));
  return 0;
}
int dependency_cache_proof_limits() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  const auto schema = multi_result::schema(ElementType::Float32);
  auto leaf = cache_operation("leaf", schema, CacheMode::First, 1, {},
                              ElementType::Float32);
  auto merge = cache_operation("merge", schema, CacheMode::First, 4, {},
                               ElementType::Float32);
  PS_CHECK(registry->register_operation(leaf).ok());
  PS_CHECK(registry->register_operation(merge).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "x", schema)};
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
  root->upstream.assign(branches.begin(), branches.end());
  std::uint64_t work = 100000, visits = 0;
  auto proof = dependency_cache_proof(plan, root, 10000, &work, &visits, {});
  PS_CHECK(proof.ok() && visits == 9 && proof.value().metadata_entries > 1200);
  PS_CHECK(proof.value().support.at("x") == q);
  work = 100000;
  visits = 0;
  PS_CHECK(!dependency_cache_proof(plan, root, 1200, &work, &visits, {}).ok());
  // Compare the projected source set with public execution's independently
  // assembled evidence for this diamond. No hidden implementation exports.
  ExecutionContext context(registry, configuration());
  auto frozen =
      context
          .freeze(plan,
                  {{multi_result::binding(
                      context.resource_budget().take_value(), "x", 7, schema)}})
          .take_value();
  auto computed = context.execute_fragments(frozen, {{"y", q}});
  PS_CHECK(computed.ok());
  const auto support =
      computed.value().dependencies.source_support().take_value();
  PS_CHECK(support.size() == proof.value().support.size());
  for (const auto& source : proof.value().support) {
    auto actual = support.find(source.first);
    PS_CHECK(actual != support.end() && actual->second == source.second);
  }
  work = 1;
  visits = 0;
  for (unsigned i = 0; i < 100; ++i)
    PS_CHECK(
        !dependency_cache_proof(plan, root, 10000, &work, &visits, {}).ok());
  PS_CHECK(work == 0 && visits == 0);
  return 0;
}
int concurrent_reclamation() {
  using namespace ps;  // NOLINT(build/namespaces)
  // Admission must reclaim entries created while it was waiting. Use exact
  // accounted bytes and a barrier in the reclaimer, without timing sleeps.
  std::mutex mutex;
  std::condition_variable cv;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto budget = std::make_shared<execution_internal::MemoryBudget>(16);
  auto a_work = budget->reserve(8).take_value();
  auto make_value = [](const BufferAllocator& allocator) {
    return MutableValue::allocate({ElementType::Float32, {1, 1}},
                                  Region::whole({1, 1}), allocator)
        .take_value();
  };
  auto retained_a = budget->reserve(4).take_value();
  auto a_result = make_value(retained_a->allocator());
  retained_a->seal();
  auto retained_b = budget->reserve(4).take_value();
  auto b_result = make_value(retained_b->allocator());
  retained_b->seal();
  auto cached_value = make_value(a_work->allocator());
  std::atomic<unsigned> reclamations{0};
  bool first_reclaim = false, admission_continue = false;
  auto waiting = std::async(std::launch::async, [&] {
    return budget->reserve(
        8,
        [&] {
          return std::chrono::steady_clock::now() < deadline
                     ? ErrorCode::Ok
                     : ErrorCode::Cancelled;
        },
        {},
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
    cv.wait_until(lock, deadline, [&] { return first_reclaim; });
    a_work->seal();
    admission_continue = true;
  }
  cv.notify_all();
  auto admitted = waiting.get();
  PS_CHECK(admitted.ok() && reclamations >= 2);
  admitted.value()->seal();

  return 0;
}
}  // namespace
int main() {
  PS_CHECK(opaque_result_cache_preservation() == 0);
  PS_CHECK(concurrent_reclamation() == 0);
  PS_CHECK(result_content_bits() == 0);
  PS_CHECK(dependency_cache_storage() == 0);
  PS_CHECK(cache_shared_route_budget() == 0);
  PS_CHECK(dependency_cache_proof_limits() == 0);
  return 0;
}
