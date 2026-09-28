#include <cstdint>
#include <iostream>
#include <memory>

#ifndef PS_PLANAR_PUBLIC_CONSUMER
#include "execution/execution_test_hooks.hpp"
#endif
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

#ifndef PS_PLANAR_PUBLIC_CONSUMER
namespace {
ps::CancellationSource* active_cancellation = nullptr;
ps::GraphContext* active_graph = nullptr;
const ps::WorkflowDocument* active_document = nullptr;
void submitted() noexcept {
  if (active_cancellation)
    active_cancellation->cancel();
  if (active_graph)
    active_graph->replace(*active_document);
}
struct ClearHooks {
  ~ClearHooks() {
    ps::execution_testing::install_execution_test_hooks(nullptr);
  }
};
}  // namespace
#endif
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
#ifndef PS_PLANAR_PUBLIC_CONSUMER
  execution_testing::ExecutionTestHooks hooks;
  const ClearHooks clear_hooks;
  hooks.native_device = true;
  hooks.native_submitted = submitted;
  execution_testing::install_execution_test_hooks(&hooks);
#endif
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_PLANAR_FIXTURE).ok());
  PS_CHECK(registry->load_plugin(PS_PLANAR_GPU_FIXTURE).ok());
  PS_CHECK(registry->freeze().ok());
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.cpu_workers = 4;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  if (!context.gpu_enabled()) {
    std::cout << "native GPU unavailable\n";
    return 77;
  }
  const ValueDescriptor descriptor{ElementType::Float32, {1, 1, 1}};
  const auto whole = Region::whole(descriptor.shape);
  auto image = PlanarImage::create(descriptor, {}).take_value();
  const float sample = .75f;
  PS_CHECK(
      image.publish(whole, reinterpret_cast<const uint8_t*>(&sample), 4).ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "image", descriptor, whole, {}, {}, PlanarImageLayout{}}};
  document.outputs = {{"result", 1, "values"}};
  ExecutionBindings bindings;
  bindings.inputs.push_back(
      {"image", {}, {}, {}, std::make_shared<const PlanarImage>(image)});
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  std::uint64_t pair_native_capacity = 0;
  for (std::int64_t test = 0; test <= 9; ++test) {
    document.nodes = {
        {1, "test.planar_gpu", {WorkflowInputReference{1}}, {{"case", test}}}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph, planning);
    if (!plan.ok())
      std::cerr << plan.status().message << '\n';
    PS_CHECK(plan.ok());
    auto result = context.execute(plan.value().plan, bindings);
    if (test == 0 || test == 9) {
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      PS_CHECK(result.value().diagnostics.native_dispatch_count == 1);
      PS_CHECK(result.value().diagnostics.native_submission_count == 1);
      PS_CHECK(result.value().diagnostics.selected_backends.at({1, 0}) ==
               Backend::Gpu);
      PS_CHECK(result.value().diagnostics.fallback_reasons.empty());
      float value = 0;
      PS_CHECK(result.value()
                   .images.at("result")
                   .read(whole, reinterpret_cast<uint8_t*>(&value), 4)
                   .ok());
      PS_CHECK(value == .375f);
      if (test == 0)
        pair_native_capacity = context.resource_budget()
                                   .value()
                                   .statistics()
                                   .peak[ResourceKind::Device];
    } else {
      PS_CHECK(!result.ok());
      const auto expected = test == 5   ? ErrorCode::TypeMismatch
                            : test == 7 ? ErrorCode::BackendUnavailable
                            : test == 2 ? ErrorCode::OperationFailed
                            : test == 3 || test == 8
                                ? ErrorCode::ResourceExhausted
                                : ErrorCode::InvalidArgument;
      if (result.status().code != expected)
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.status().code == expected);
    }
  }
  document.nodes[0].parameters["case"] = std::int64_t{0};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph, planning).take_value();
#ifndef PS_PLANAR_PUBLIC_CONSUMER
  // Cancellation and staleness at actual native submission suppress commit.
  CancellationSource cancellation;
  active_cancellation = &cancellation;
  auto stopped = context.execute(compiled.plan, bindings, cancellation.token());
  active_cancellation = nullptr;
  PS_CHECK(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled);
  active_graph = &graph;
  active_document = &document;
  auto stale = context.execute(compiled.plan, bindings);
  active_graph = nullptr;
  active_document = nullptr;
  PS_CHECK(!stale.ok() && stale.status().code == ErrorCode::Stale);
  PS_CHECK(context.resource_budget()
               .value()
               .statistics()
               .live[ResourceKind::Payload] == 0);
  compiled = Compiler(registry).compile(graph, planning).take_value();
#endif
  config.gpu_enabled = false;
  ExecutionContext disabled(registry, config);
  PS_CHECK(disabled.execute(compiled.plan, bindings).status().code ==
           ErrorCode::BackendUnavailable);
  config.gpu_enabled = true;
  config.maximum_live_bytes = pair_native_capacity - 1;
  ExecutionContext limited(registry, config);
  auto exhausted = limited.execute(compiled.plan, bindings);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  for (auto kind : {ResourceKind::Device, ResourceKind::Shared}) {
    for (std::uint64_t bytes :
         {pair_native_capacity - 1, pair_native_capacity}) {
      auto capacity_config = config;
      capacity_config.maximum_live_bytes = 1 << 20;
      capacity_config.managed_resources = ResourceLimits{};
      capacity_config.managed_resources->capacity[kind] = bytes;
      ExecutionContext bounded(registry, capacity_config);
      auto result = bounded.execute(compiled.plan, bindings);
      PS_CHECK(result.ok() == (bytes == pair_native_capacity));
      if (!result.ok())
        PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
      const auto stats = bounded.resource_budget().value().statistics();
      PS_CHECK(stats.peak[kind] == (bytes == pair_native_capacity
                                        ? pair_native_capacity
                                        : pair_native_capacity / 2));
      PS_CHECK(stats.live[ResourceKind::Shared] == 0 &&
               stats.live[ResourceKind::Device] == 0);
    }
  }
  // A large retained pipeline key must yield Host capacity to scratch for a
  // later callback in the same context, before a sticky reserve failure.
  {
    auto reclaim_config = config;
    reclaim_config.maximum_live_bytes = 1 << 20;
    reclaim_config.managed_resources = ResourceLimits{};
    ExecutionContext reference(registry, reclaim_config);
    {
      auto measured = reference.execute(compiled.plan, bindings);
      PS_CHECK(measured.ok());
    }
    const auto required_host = reference.resource_budget()
                                   .value()
                                   .statistics()
                                   .peak[ResourceKind::Host];
    ExecutionContext reclaiming(registry, reclaim_config);
    auto warm_document = document;
    warm_document.nodes[0].parameters["case"] = std::int64_t{10};
    GraphContext warm_graph(warm_document);
    auto warm_plan =
        Compiler(registry).compile(warm_graph, planning).take_value();
    {
      auto warmed = reclaiming.execute(warm_plan.plan, bindings);
      PS_CHECK(warmed.ok());
    }
    auto root = reclaiming.resource_budget().value();
    const auto warm_metadata = root.statistics().live[ResourceKind::Metadata];
    const auto available = root.available_capacity()[ResourceKind::Host];
    PS_CHECK(required_host > 16384 && available > required_host);
    auto pressure = root.reserve(
        ResourceCapacity::host(available - (required_host - 16384) -
                               ResourceBudget::lease_metadata_bytes()));
    PS_CHECK(pressure.ok());
    auto result = reclaiming.execute(compiled.plan, bindings);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    PS_CHECK(result.value().diagnostics.native_dispatch_count == 1);
    PS_CHECK(result.value().diagnostics.fallback_reasons.empty());
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] < warm_metadata);
    float actual = 0;
    PS_CHECK(result.value()
                 .images.at("result")
                 .read(whole, reinterpret_cast<uint8_t*>(&actual), 4)
                 .ok());
    PS_CHECK(actual == .375f);
  }
  // CPU -> GPU -> GPU -> CPU, then independently requested named outputs.
  document.nodes = {{1,
                     "test.planar_plugin",
                     {WorkflowInputReference{1}},
                     {{"case", std::int64_t{0}}}},
                    {2,
                     "test.planar_gpu",
                     {WorkflowNodeOutput{1, "values"}},
                     {{"case", std::int64_t{0}}}},
                    {3,
                     "test.planar_gpu",
                     {WorkflowNodeOutput{2, "values"}},
                     {{"case", std::int64_t{0}}}},
                    {4,
                     "test.planar_plugin",
                     {WorkflowNodeOutput{3, "values"}},
                     {{"case", std::int64_t{0}}}}};
  document.outputs = {{"result", 4, "values"}};
  PlanarImage retained;
  for (bool multiple : {false, true}) {
    if (multiple)
      document.outputs.push_back({"tap", 2, "values"});
    GraphContext chain(document);
    auto plan = Compiler(registry).compile(chain, planning).take_value();
    auto result = context.execute(plan.plan, bindings);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    PS_CHECK(result.value().diagnostics.native_dispatch_count ==
             (multiple ? 3 : 2));
    std::uint64_t dispatches = 0;
    for (const auto& timing : result.value().diagnostics.operation_timings) {
      PS_CHECK(timing.native_dispatch_count ==
               (timing.backend == Backend::Gpu ? 1 : 0));
      dispatches += timing.native_dispatch_count;
    }
    PS_CHECK(dispatches == result.value().diagnostics.native_dispatch_count);
    PS_CHECK(result.value().diagnostics.selected_backends.at({4, 0}) ==
             Backend::Cpu);
    PS_CHECK(result.value().diagnostics.fallback_reasons.empty());
    retained = result.value().images.at("result");
    float value = 0;
    PS_CHECK(retained.read(whole, reinterpret_cast<uint8_t*>(&value), 4).ok());
    PS_CHECK(value == .1875f);
  }
  return 0;
}
