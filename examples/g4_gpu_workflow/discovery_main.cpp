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
WorkflowDocument document(std::int64_t capacity = 2, std::int64_t mode = 0) {
  WorkflowDocument result;
  result.inputs = {{1,
                    "data",
                    {ElementType::Float32, {length}},
                    Region::whole({length}),
                    {0, {4}},
                    {}},
                   {2,
                    "control",
                    {ElementType::Int64, {length}},
                    Region::whole({length}),
                    {0, {8}},
                    {}}};
  result.nodes = {{1,
                   "example.c_discovery",
                   {WorkflowInputReference{1}, WorkflowInputReference{2}},
                   {{"capacity", capacity}, {"mode", mode}}}};
  result.outputs = {{"sum", 1, "value"}};
  return result;
}
Value controls(std::int64_t first = 0) {
  auto writer =
      MutableValue::allocate({ElementType::Int64, {length}},
                             Region::whole({length}), BufferAllocator{})
          .take_value();
  std::memset(writer.data(), 0, writer.size());
  const std::int64_t second = 1;
  std::memcpy(writer.data(), &first, 8);
  std::memcpy(writer.data() + 8, &second, 8);
  return std::move(writer).publish().take_value();
}
int check(bool value, const char* message) {
  if (!value)
    std::cerr << message << '\n';
  return value ? 0 : 1;
}
}  // namespace
int main(int argc, char** argv) {
  auto registry = std::make_shared<OperationRegistry>();
  auto loaded =
      registry->load_plugin(argc > 1 ? argv[1] : PS_G4_DISCOVERY_PLUGIN);
  if (!loaded.ok())
    std::cerr << loaded.message << '\n';
  if (check(loaded.ok() && registry->freeze().ok(), "discovery module failed"))
    return 1;
  auto writer =
      MutableValue::allocate({ElementType::Float32, {length}},
                             Region::whole({length}), BufferAllocator{})
          .take_value();
  std::memset(writer.data(), 0, writer.size());
  const std::uint64_t indices[] = {0, 4096, 1, 4097};
  const float values[] = {3, 5, 11, 13};
  for (unsigned i = 0; i < 4; ++i)
    std::memcpy(writer.data() + indices[i] * 4, values + i, 4);
  const auto data = std::move(writer).publish().take_value();
  ExecutionBindings bindings{{{"data", data}, {"control", controls()}}};
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  ExecutionContext context(registry, config);
  for (bool gpu : {false, true}) {
    if (gpu && !context.gpu_enabled()) {
      std::cout << "CPU discovery oracle passed; native skipped\n";
      return 77;
    }
    PlanningOptions options;
    options.execution_mode =
        gpu ? ExecutionMode::MetalFp32 : ExecutionMode::CpuExact;
    GraphContext graph(document());
    auto compiled = Compiler(registry).compile(graph, options).take_value();
    auto frozen = context.freeze(compiled.plan, bindings).take_value();
    auto result = context.execute_fragments(
        frozen, {{"sum", point(0).unite(point(1)).take_value()}});
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    if (check(result.ok(), "discovery workflow failed"))
      return 1;
    for (std::uint64_t at : {0, 1}) {
      float value = 0;
      if (check(result.value().values.at("sum").read({at}, &value, 4).ok() &&
                    value == (at ? 24 : 8),
                "discovery arithmetic oracle failed"))
        return 1;
    }
    const auto support = result.value()
                             .dependencies.restrict({{"sum", point(0)}})
                             .take_value()
                             .source_support()
                             .take_value();
    if (check(support.at("data") == point(0).unite(point(4096)).value() &&
                  support.at("control") == point(0),
              "discovery relation is not exact"))
      return 1;
    if (check(result.value().diagnostics.native_dispatch_count == (gpu ? 4 : 0),
              "discovery/resume dispatch count failed"))
      return 1;
    auto demand = context.open_demand(compiled.plan, bindings).take_value();
    if (check(demand.request({{"sum", point(0)}}).ok() &&
                  demand
                      .replace_bindings(
                          {{{"data", data}, {"control", controls(1)}}})
                      .ok(),
              "dynamic binding update failed"))
      return 1;
    auto changed = demand.request({{"sum", point(0)}});
    if (check(changed.ok(), "changed discovery failed"))
      return 1;
    float value = 0;
    if (check(changed.value().values.at("sum").read({0}, &value, 4).ok() &&
                  value == 24,
              "new GPU address selection failed"))
      return 1;
    auto current = changed.value().dependencies.source_support().take_value();
    if (check(current.at("data") == point(1).unite(point(4097)).value(),
              "discovery retained old data edges"))
      return 1;
    auto clean = changed.value()
                     .dependencies.potential_dirty("data", point(0))
                     .take_value();
    auto dirty = changed.value()
                     .dependencies.potential_dirty("data", point(1))
                     .take_value();
    if (check(clean.at("sum").empty() && dirty.at("sum") == point(0),
              "discovery dirty transpose retained old edge"))
      return 1;
    auto bad_control =
        context
            .freeze(compiled.plan,
                    {{{"data", data}, {"control", controls(-1)}}})
            .take_value();
    if (check(context.execute_fragments(bad_control, {{"sum", point(0)}})
                      .status()
                      .code == ErrorCode::InvalidArgument,
              "signed control access contract differs"))
      return 1;
    auto old = context.execute_fragments(frozen, {{"sum", point(0)}});
    if (check(old.ok() &&
                  old.value().values.at("sum").read({0}, &value, 4).ok() &&
                  value == 8,
              "frozen discovery changed"))
      return 1;
    std::cout << (gpu ? "Metal" : "CPU")
              << " discovery: sums=8,24; exact controls/data; changed=24; "
                 "frozen=8; dispatches="
              << result.value().diagnostics.native_dispatch_count << '\n';
    if (!gpu)
      continue;
    for (unsigned mode = 0; mode < 3; ++mode) {
      GraphContext invalid(document(mode == 0 ? 1 : 2, mode == 1 ? 1 : 0));
      auto bad = Compiler(registry).compile(invalid, options).take_value();
      ExecutionOptions bounds;
      if (mode == 2)
        bounds.dependencies.maximum_gpu_requests = 0;
      auto denied = context.execute_fragments(
          context.freeze(bad.plan, bindings).take_value(), {{"sum", point(0)}},
          {}, bounds);
      if (check(denied.status().code == (mode == 1
                                             ? ErrorCode::InvalidArgument
                                             : ErrorCode::ResourceExhausted),
                "overflow/bypass/disabled discovery accepted"))
        return 1;
    }
    GraphContext large(document(128));
    auto large_plan = Compiler(registry).compile(large, options).take_value();
    // Table: 18448 logical bytes -> 32768 native capacity. Control atlas:
    // 8+160.
    const auto minimum =
        large_plan.plan.steps()[0].traits.continuation_bytes + 12 + 32768 + 168;
    for (auto bytes : {minimum - 1, minimum}) {
      ExecutionContextConfig tight;
      tight.gpu_enabled = true;
      tight.maximum_live_bytes = bytes;
      ExecutionContext small(registry, tight);
      auto query = small.freeze(large_plan.plan, bindings).take_value();
      auto run = small.execute_fragments(query, {{"sum", point(0)}});
      if (check(bytes == minimum
                    ? run.ok()
                    : run.status().code == ErrorCode::ResourceExhausted,
                "discovery actual admission frontier failed"))
        return 1;
      if (run.ok() && check(run.value().diagnostics.native_dispatch_count == 2,
                            "bounded discovery skipped native work"))
        return 1;
    }
    std::cout << "discovery native admission: " << minimum - 1 << " rejected, "
              << minimum << " passed\n";
  }
  return 0;
}
