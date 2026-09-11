#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
constexpr std::uint64_t length = 8192;
Footprint point(std::uint64_t at) {
  return Footprint::from_regions({length}, {Region({{at, 1}})}).take_value();
}
WorkflowDocument document(std::int64_t mode) {
  WorkflowDocument result;
  result.inputs = {{1,
                    "x",
                    {ElementType::Float32, {length}},
                    Region::whole({length}),
                    {0, {4}},
                    {}}};
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
}  // namespace
int main(int argc, char** argv) {
  auto registry = std::make_shared<OperationRegistry>();
  const auto status =
      registry->load_plugin(argc > 1 ? argv[1] : PS_G4_C_GPU_PLUGIN);
  if (!status.ok())
    std::cerr << status.message << '\n';
  if (check(status.ok() && registry->freeze().ok(), "C plugin load failed"))
    return 1;
  auto writer =
      MutableValue::allocate({ElementType::Float32, {length}},
                             Region::whole({length}), BufferAllocator{})
          .take_value();
  std::memset(writer.data(), 0, writer.size());
  for (std::uint64_t i = 1; i <= 65; ++i) {
    const float value = static_cast<float>(i);
    std::memcpy(writer.data() + i * 64 * 4, &value, 4);
  }
  ExecutionBindings bindings{{{"x", std::move(writer).publish().take_value()}}};
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.result_cache_bytes = 1048576;
  ExecutionContext context(registry, config);
  for (bool gpu : {false, true}) {
    if (gpu && !context.gpu_enabled()) {
      std::cout << "C CPU oracle passed; native skipped\n";
      return 77;
    }
    GraphContext graph(document(0));
    PlanningOptions options;
    options.execution_mode =
        gpu ? ExecutionMode::MetalFp32 : ExecutionMode::CpuExact;
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
      float value = 0;
      if (check(result.value().values.at("sum").read({i}, &value, 4).ok() &&
                    value == 2145,
                "C independent sum failed"))
        return 1;
    }
    auto evidence =
        result.value().dependencies.restrict({{"sum", point(1)}}).take_value();
    auto support = evidence.source_support().take_value().at("x");
    if (check(support.contains({1}) && !support.contains({0}) &&
                  support.element_count().value() == 66,
              "C block hit imported obsolete control"))
      return 1;
    const auto& d = result.value().diagnostics;
    if (check(d.native_dispatch_count == (gpu ? 1 : 0) &&
                  d.block_cache_hits == (gpu ? 1 : 0),
              "C native computation/cache evidence failed"))
      return 1;
    if (gpu && check(context.cache_statistics().native_retained_bytes == 16,
                     "C native cache capacity failed"))
      return 1;
    std::cout << (gpu ? "C Metal" : "C CPU")
              << ": sums=2145,2145; dispatches=" << d.native_dispatch_count
              << "; block_hits=" << d.block_cache_hits << '\n';
    for (std::int64_t mode : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}) {
      if ((gpu && mode == 8) || (!gpu && mode != 8))
        continue;
      GraphContext bad_graph(document(mode));
      auto bad_plan =
          Compiler(registry).compile(bad_graph, options).take_value();
      ExecutionContextConfig tight;
      tight.gpu_enabled = gpu;
      tight.maximum_live_bytes =
          bad_plan.plan.steps()[0].traits.outputs[0].continuation_bytes + 4 +
          24 + 65 * 4 + 32768;
      ExecutionContext fresh(registry, tight);
      auto bad_frozen = fresh.freeze(bad_plan.plan, bindings).take_value();
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
          fresh.freeze(compiled.value().plan, bindings).take_value();
      auto retry = fresh.execute_fragments(retry_frozen, {{"sum", point(0)}});
      if (check(retry.ok() && retry.value().diagnostics.native_dispatch_count ==
                                  (gpu ? 1 : 0),
                "C failed stage retained owners or bypassed real retry"))
        return 1;
    }
  }
  std::cout << "C stale/forged tokens, malformed services, revoked writes and "
               "missing samples rejected\n";
  return 0;
}
