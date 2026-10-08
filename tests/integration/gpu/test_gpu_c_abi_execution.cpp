#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "support/gpu_result_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using gpu_result::take;
constexpr std::uint64_t length = 8192;
SchemaTemplate schema(std::uint64_t count = length) {
  auto value = gpu_result::schema(ElementType::Float32, count);
  value.id = "c.gpu.samples";
  value.tensors[0].key = "samples";
  return value;
}
ExecutionBindings inputs(const ResourceBudget& root,
                         std::uint64_t count = length) {
  auto builder = take(ResultBuilder::start(root, schema(count), "c.input"));
  gpu_result::check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  auto buffer = take(root.allocator().allocate(count * 4));
  std::memset(buffer.data(), 0, buffer.size());
  for (std::uint64_t i = 1; i <= 65; ++i) {
    const float value = static_cast<float>(i);
    std::memcpy(buffer.data() + i * 64 * 4, &value, 4);
  }
  gpu_result::check(builder.publish_tensor(
      0, Region::whole({count}), {0, {4}}, std::move(buffer).freeze(),
      take(ResultRelation::cartesian(root, count, {})),
      {true, true, true, true}));
  ExecutionBindings bindings;
  ExecutionBinding binding;
  binding.name = "x";
  binding.result = take(builder.seal());
  bindings.inputs.push_back(std::move(binding));
  return bindings;
}
Footprint point(std::uint64_t at) {
  return Footprint::from_regions({length}, {Region({{at, 1}})}).take_value();
}
WorkflowDocument document(std::int64_t mode, std::uint64_t count = length) {
  WorkflowDocument result;
  result.inputs = {gpu_result::declaration(1, "x", schema(count))};
  result.nodes = {{1,
                   "example.c_native_sum",
                   {WorkflowInputReference{1}},
                   {{"mode", mode}}}};
  result.outputs = {{"sum", 1, "value"}};
  return result;
}
int check(bool value, const char* message) {
  if (!value)
    std::cerr << message << '\n';
  return value ? 0 : 1;
}
bool whole_query(const OperationRegistry& registry,
                 const ResourceBudget& root) {
  ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(schema());
  metadata.inputs.push_back(metadata.output);
  const std::map<std::string, ParameterValue> parameters{
      {"mode", std::int64_t{0}}};
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "example.c_native_sum";
  auto continuation = take(registry.start_result(
      std::string(query.semantic_key), query, root.allocator()));

  ResultObjectInputs objects;
  ResourceVector<ResultIoReply> io;
  auto allocator = root.allocator();
  ResultProgramPhase phase{
      query,
      objects,
      io,
      allocator,
      root,
      [&](std::uint64_t work) { return root.consume({work}); },
      std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)};
  auto polled = take(continuation.poll(phase));
  const auto* need = std::get_if<ResultProgramNeed>(&polled);
  return need && need->tensors.size() == 1 && need->tensors[0].roles == 10 &&
         need->tensors[0].samples.contains({0}) &&
         take(need->tensors[0].samples.element_count()) == 1;
}
}  // namespace
int main(int argc, char** argv) try {
  auto registry = std::make_shared<OperationRegistry>();
  const auto status =
      registry->load_plugin(argc > 1 ? argv[1] : PS_GPU_DEPENDENCY_PLUGIN);
  if (!status.ok())
    std::cerr << status.message << '\n';
  if (check(status.ok() && registry->freeze().ok(), "C plugin load failed"))
    return 1;
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.result_cache_bytes = 1048576;
  ExecutionContext context(registry, config);
  if (check(whole_query(*registry, take(context.resource_budget())),
            "C complete-object query was treated as Empty"))
    return 1;
  const auto bindings = inputs(take(context.resource_budget()));
  for (bool gpu : {false, true}) {
    if (gpu && !context.gpu_enabled()) {
      std::cout << "C CPU oracle passed; native skipped\n";
      return 77;
    }
    GraphContext graph(document(0));
    PlanningOptions options;
    options.execution_mode =
        gpu ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
    auto compiled = Compiler(registry).compile(graph, options);
    if (check(compiled.ok(), "C workflow compile failed"))
      return 1;
    auto frozen = context.freeze(compiled.value().plan, bindings).take_value();
    auto result = context.execute_fragments(
        frozen, {{"sum", point(0).unite(point(1)).take_value()}});
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    if (check(result.ok(), "C workflow failed"))
      return 1;
    for (std::uint64_t i : {0, 1}) {
      if (check(gpu_result::number(result.value().results.at("sum"), i) == 2145,
                "C independent sum failed"))
        return 1;
    }
    auto evidence =
        result.value().dependencies.restrict({{"sum", point(1)}}).take_value();
    auto support = evidence.source_support().take_value().at("x");
    if (check(support.contains({1}) && !support.contains({0}) &&
                  support.element_count().value() == 66,
              "C block state imported obsolete control"))
      return 1;
    const auto& d = result.value().diagnostics;
    if (check(gpu ? d.native_dispatch_count > 0 : d.native_dispatch_count == 0,
              "C native computation evidence failed"))
      return 1;
    if (gpu && check(context.cache_statistics().native_retained_bytes == 16,
                     "C native cache capacity failed"))
      return 1;
    std::cout << (gpu ? "C Metal" : "C CPU")
              << ": sums=2145,2145; dispatches=" << d.native_dispatch_count
              << "; block_hits=" << d.block_cache_hits << '\n';
    auto empty = take(context.execute_fragments(
        frozen, {{"sum", take(Footprint::none({length}))}}));
    if (check(take(empty.results.at("sum").descriptor())
                      .tensor_coverage(0)
                      .empty() &&
                  !empty.diagnostics.native_dispatch_count,
              "C Empty Result performed native work"))
      return 1;
    GraphContext minimum_graph(document(0, 4225));
    auto minimum_plan =
        take(Compiler(registry).compile(minimum_graph, options));
    auto minimum_binding = inputs(take(context.resource_budget()), 4225);
    auto minimum_frozen =
        take(context.freeze(minimum_plan.plan, minimum_binding));
    auto minimum_output = take(context.execute_fragments(
        minimum_frozen,
        {{"sum", take(Footprint::from_regions({4225}, {Region({{0, 1}})}))}}));
    if (check(gpu_result::number(minimum_output.results.at("sum"), 0) == 2145,
              "C dynamic Result metadata changed minimum input"))
      return 1;
    GraphContext invalid_graph(document(0, 4224));
    if (check(!Compiler(registry).compile(invalid_graph, options).ok(),
              "C undersized tensor accepted"))
      return 1;
    if (gpu) {
      GraphContext renewed_graph(document(17));
      auto renewed_plan =
          take(Compiler(registry).compile(renewed_graph, options));
      auto renewed_frozen = take(context.freeze(renewed_plan.plan, bindings));
      auto renewed =
          take(context.execute_fragments(renewed_frozen, {{"sum", point(0)}}));
      if (check(gpu_result::number(renewed.results.at("sum"), 0) == 2145 &&
                    renewed.diagnostics.native_dispatch_count > 0,
                "C released atlas token could not be reacquired"))
        return 1;
    }
    for (std::int64_t mode :
         {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}) {
      if ((gpu && mode == 8) || (!gpu && mode != 8))
        continue;
      GraphContext bad_graph(document(mode));
      auto bad_plan =
          Compiler(registry).compile(bad_graph, options).take_value();
      ExecutionContextConfig tight;
      tight.gpu_enabled = gpu;
      tight.maximum_live_bytes =
          length * 4 +
          bad_plan.plan.steps()[0].traits.outputs[0].continuation_bytes + 4 +
          24 + 65 * 4 + 32768;
      ExecutionContext fresh(registry, tight);
      const auto fresh_bindings = inputs(take(fresh.resource_budget()));
      auto bad_frozen =
          fresh.freeze(bad_plan.plan, fresh_bindings).take_value();
      auto failed = fresh.execute_fragments(bad_frozen, {{"sum", point(0)}});
      const auto expected =
          mode == 7 ? ErrorCode::OperationFailed : ErrorCode::InvalidArgument;
      if (check(failed.status().code == expected,
                "C native negative case failed")) {
        std::cerr << "mode=" << mode
                  << ", code=" << static_cast<int>(failed.status().code)
                  << ", message=" << failed.status().message << '\n';
        return 1;
      }
      if (check(take(fresh.resource_budget())
                            .statistics()
                            .live[ResourceKind::Payload] == length * 4 &&
                    fresh.cache_statistics().entries == 0,
                "C failed Result retained payload or cached output"))
        return 1;
      if (mode == 3 &&
          check(failed.status().message == "invalid C native view destination",
                "C writable flag was not rejected by the bridge"))
        return 1;
      if ((mode == 1 || mode == 5) &&
          check(failed.status().message ==
                    "native token is not from current C poll",
                "C stale/forged token did not fail in bridge"))
        return 1;
      auto retry_frozen =
          fresh.freeze(compiled.value().plan, fresh_bindings).take_value();
      auto retry = fresh.execute_fragments(retry_frozen, {{"sum", point(0)}});
      if (check(
              retry.ok() &&
                  gpu_result::number(retry.value().results.at("sum"), 0) ==
                      2145 &&
                  (gpu ? retry.value().diagnostics.native_dispatch_count > 0
                       : retry.value().diagnostics.native_dispatch_count == 0),
              "C failed stage retained owners or bypassed real retry"))
        return 1;
    }
  }
  std::cout << "C stale/forged tokens, malformed services, revoked writes and "
               "missing samples rejected\n";
  return 0;
} catch (const gpu_result::Failure& failure) {
  std::cerr << "C Result failure: " << static_cast<int>(failure.status.code)
            << " " << failure.status.message << '\n';
  return 1;
} catch (const std::exception& failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
