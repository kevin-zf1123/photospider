#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "support/gpu_result_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using gpu_result::take;
constexpr std::uint64_t length = 8192;
constexpr std::uint64_t source_bytes = length * 12;
SchemaTemplate schema(ElementType type) {
  auto value = gpu_result::schema(type, length);
  value.id =
      type == ElementType::Float32 ? "discovery.data" : "discovery.control";
  return value;
}
ResultRef source(const ResourceBudget& root, ElementType type,
                 std::int64_t first = 0) {
  auto builder =
      take(ResultBuilder::start(root, schema(type), "discovery.input"));
  gpu_result::check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  const bool data = type == ElementType::Float32;
  auto buffer = take(root.allocator().allocate(length * (data ? 4 : 8)));
  std::memset(buffer.data(), 0, buffer.size());
  if (data) {
    const std::uint64_t indices[] = {0, 4096, 1, 4097};
    const float values[] = {3, 5, 11, 13};
    for (unsigned i = 0; i < 4; ++i)
      std::memcpy(buffer.data() + indices[i] * 4, values + i, 4);
  } else {
    const std::int64_t second = 1;
    std::memcpy(buffer.data(), &first, 8);
    std::memcpy(buffer.data() + 8, &second, 8);
  }
  gpu_result::check(
      builder.publish_tensor(0, Region::whole({length}), {0, {data ? 4 : 8}},
                             std::move(buffer).freeze(),
                             take(ResultRelation::cartesian(root, length, {})),
                             {true, true, true, true}));
  return take(builder.seal());
}
ExecutionBindings bind(const ResultRef& data, const ResultRef& control) {
  ExecutionBindings bindings;
  for (const bool is_data : {true, false}) {
    ExecutionBinding binding;
    binding.name = is_data ? "data" : "control";
    binding.result = is_data ? data : control;
    bindings.inputs.push_back(std::move(binding));
  }
  return bindings;
}
Footprint point(std::uint64_t at) {
  return Footprint::from_regions({length}, {Region({{at, 1}})}).take_value();
}
WorkflowDocument document(std::int64_t capacity = 2, std::int64_t mode = 0) {
  WorkflowDocument result;
  result.inputs = {
      gpu_result::declaration(1, "data", schema(ElementType::Float32)),
      gpu_result::declaration(2, "control", schema(ElementType::Int64))};
  result.nodes = {{1,
                   "example.c_discovery",
                   {WorkflowInputReference{1}, WorkflowInputReference{2}},
                   {{"capacity", capacity}, {"mode", mode}}}};
  result.outputs = {{"sum", 1, "value"}};
  return result;
}
int check(bool value, const char* message) {
  if (!value)
    std::cerr << message << '\n';
  return value ? 0 : 1;
}
struct ThreadDiscovery {
  bool wrong_thread;
  std::atomic<bool>* entered;
  ThreadDiscovery(bool wrong_thread, std::atomic<bool>* entered)
      : wrong_thread(wrong_thread), entered(entered) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto compute = [&](const ResultGpuRequestTable& table) {
      entered->store(true);
      Status workers[2];
      std::thread first([&] { workers[0] = phase.consume_work(4096); });
      std::thread second([&] { workers[1] = phase.consume_work(4096); });
      first.join();
      second.join();
      for (const auto& status : workers)
        if (!status.ok())
          return status;
      const char shader[] =
          "#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void clear_table(device uint* table [[buffer(0)]])"
          "{table[0]=0;}";
      const auto* api = phase.gpu;
      std::uint64_t token = 0;
      auto status = gpu_result::gpu_status(
          phase,
          api->buffer(api->context, table.bytes, table.byte_size, 1, &token));
      if (!status.ok())
        return status;
      const ps_gpu_buffer_binding_v1 binding{sizeof(binding), 0, token, 0,
                                             table.byte_size, 1};
      ps_gpu_dispatch_v1 command{};
      command.struct_size = sizeof(command);
      command.source = shader;
      command.source_size = sizeof(shader) - 1;
      command.entry = "clear_table";
      command.entry_size = 11;
      command.buffers = &binding;
      command.buffer_count = 1;
      command.grid[0] = command.grid[1] = command.grid[2] = 1;
      status = gpu_result::gpu_status(phase,
                                      api->execute(api->context, &command, 1));
      const auto released =
          gpu_result::gpu_status(phase, api->release(api->context, token));
      return status.ok() ? released : status;
    };
    if (wrong_thread) {
      Status status;
      std::thread worker(
          [&] { status = phase.discover(1, 1, compute).status(); });
      worker.join();
      return Result<ResultProgramPoll>(
          status.ok()
              ? Status{ErrorCode::OperationFailed, "wrong thread allowed"}
              : status);
    }
    const auto receipt = phase.discover(1, 1, compute);
    if (!receipt.ok())
      return Result<ResultProgramPoll>(receipt.status());
    auto output = gpu_result::builder(phase, false);
    return Result<ResultProgramPoll>(
        ResultPublication{take(output.seal()), true});
  }
};
void register_thread_probes(const std::shared_ptr<OperationRegistry>& registry,
                            std::atomic<bool>* entered) {
  for (const bool wrong : {false, true}) {
    OperationDefinition op;
    op.key = wrong ? "test.discovery_wrong_thread" : "test.discovery_workers";
    op.traits.supports_cpu = false;
    op.traits.supports_gpu = true;
    op.traits.outputs = {
        gpu_result::output(gpu_result::schema(ElementType::Float32, 1))};
    op.traits.outputs[0].continuation_bytes = sizeof(ThreadDiscovery);
    op.start_result = [wrong, entered](const ResultProgramQuery&,
                                       const BufferAllocator& allocator) {
      return ResultContinuation::make<ThreadDiscovery>(allocator, wrong,
                                                       entered);
    };
    gpu_result::check(registry->register_operation(op));
  }
}
void thread_probes(const std::shared_ptr<OperationRegistry>& registry,
                   std::atomic<bool>* entered) {
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  for (const bool wrong : {true, false}) {
    WorkflowDocument document;
    document.nodes = {
        {1,
         wrong ? "test.discovery_wrong_thread" : "test.discovery_workers",
         {},
         {}}};
    document.outputs = {{"empty", 1, "value"}};
    GraphContext graph(document);
    const auto compiled = take(Compiler(registry).compile(graph, options));
    for (auto budget : {8512, 8513}) {
      ExecutionContextConfig config;
      config.gpu_enabled = true;
      ExecutionContext context(registry, config);
      const auto root = take(context.resource_budget());
      ExecutionOptions bounds;
      bounds.dependencies.maximum_work = budget;
      entered->store(false);
      auto run = context.execute_fragments(
          take(context.freeze(compiled.plan)),
          {{"empty", take(Footprint::none({1}))}}, {}, bounds);
      const auto expected = wrong            ? ErrorCode::InvalidArgument
                            : budget == 8512 ? ErrorCode::ResourceExhausted
                                             : ErrorCode::Ok;
      if (run.status().code != expected || entered->load() == wrong) {
        std::cerr << "thread discovery wrong=" << wrong << " budget=" << budget
                  << " " << run.status().message << '\n';
        throw std::runtime_error("thread discovery admission failed");
      }
      if (run.ok() && run.value().diagnostics.native_dispatch_count != 1)
        throw std::runtime_error("parallel discovery lacked native submission");
      run = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      if (root.statistics().live[ResourceKind::Payload] != 0)
        throw std::runtime_error("thread discovery retained native table");
    }
  }
  std::cout << "Metal discovery: wrong thread rejected; concurrent work "
               "8512 rejected, 8513 passed\n";
}
}  // namespace
int main(int argc, char** argv) try {
  std::atomic<bool> entered{false};
  auto registry = std::make_shared<OperationRegistry>();
  register_thread_probes(registry, &entered);
  auto loaded =
      registry->load_plugin(argc > 1 ? argv[1] : PS_GPU_DISCOVERY_PLUGIN);
  if (!loaded.ok())
    std::cerr << loaded.message << '\n';
  if (check(loaded.ok() && registry->freeze().ok(), "discovery module failed"))
    return 1;
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  ExecutionContext context(registry, config);
  const auto root = take(context.resource_budget());
  const auto data = source(root, ElementType::Float32);
  const auto bindings = bind(data, source(root, ElementType::Int64));
  for (bool gpu : {false, true}) {
    if (gpu && !context.gpu_enabled()) {
      std::cout << "CPU discovery oracle passed; native skipped\n";
      return 77;
    }
    PlanningOptions options;
    options.execution_mode =
        gpu ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
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
      if (check(gpu_result::number(result.value().results.at("sum"), at) ==
                    (at ? 24 : 8),
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
    if (check((gpu ? result.value().diagnostics.native_dispatch_count > 0
                   : result.value().diagnostics.native_dispatch_count == 0),
              "discovery/resume dispatch count failed"))
      return 1;
    auto demand = context.open_demand(compiled.plan, bindings).take_value();
    if (check(demand.request({{"sum", point(0)}}).ok() &&
                  demand
                      .replace_bindings(
                          bind(data, source(root, ElementType::Int64, 1)))
                      .ok(),
              "dynamic binding update failed"))
      return 1;
    auto changed = demand.request({{"sum", point(0)}});
    if (check(changed.ok(), "changed discovery failed"))
      return 1;
    if (check(gpu_result::number(changed.value().results.at("sum"), 0) == 24,
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
                    bind(data, source(root, ElementType::Int64, -1)))
            .take_value();
    if (check(context.execute_fragments(bad_control, {{"sum", point(0)}})
                      .status()
                      .code == ErrorCode::InvalidArgument,
              "signed control access contract differs"))
      return 1;
    auto old = context.execute_fragments(frozen, {{"sum", point(0)}});
    if (check(old.ok() &&
                  gpu_result::number(old.value().results.at("sum"), 0) == 8,
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
    for (unsigned mode : {3, 4, 5, 6, 8, 9, 10, 11, 12, 14, 15}) {
      ExecutionContext fresh(registry, config);
      const auto fresh_root = take(fresh.resource_budget());
      const auto inputs = bind(source(fresh_root, ElementType::Float32),
                               source(fresh_root, ElementType::Int64));
      GraphContext invalid(document(2, mode));
      auto plan = take(Compiler(registry).compile(invalid, options));
      auto failed = fresh.execute_fragments(
          take(fresh.freeze(plan.plan, inputs)), {{"sum", point(0)}});
      const auto expected = mode == 10   ? ErrorCode::OperationFailed
                            : mode == 12 ? ErrorCode::ResourceExhausted
                                         : ErrorCode::InvalidArgument;
      if (failed.status().code != expected) {
        std::cerr << "discovery mode=" << mode
                  << " code=" << static_cast<int>(failed.status().code) << " "
                  << failed.status().message << '\n';
        return 1;
      }
      if (check(fresh_root.statistics().live[ResourceKind::Payload] ==
                        source_bytes &&
                    fresh.cache_statistics().entries == 0,
                "failed discovery retained payload or cached a result"))
        return 1;
      auto retry = fresh.execute_fragments(
          take(fresh.freeze(compiled.plan, inputs)), {{"sum", point(0)}});
      if (check(retry.ok() &&
                    retry.value().diagnostics.native_dispatch_count > 0 &&
                    gpu_result::number(retry.value().results.at("sum"), 0) == 8,
                "normal discovery after failure did not execute"))
        return 1;
    }
    {
      GraphContext mixed(document(2, 13));
      auto plan = take(Compiler(registry).compile(mixed, options));
      auto run = take(context.execute_fragments(
          take(context.freeze(plan.plan, bindings)), {{"sum", point(0)}}));
      const auto support = take(run.dependencies.source_support());
      if (check(gpu_result::number(run.results.at("sum"), 0) == 8 &&
                    run.diagnostics.native_dispatch_count > 0 &&
                    support.at("data") == point(0).unite(point(4096)).value(),
                "same-slot discovery role groups failed"))
        return 1;
      auto empty = take(context.execute_fragments(
          take(context.freeze(plan.plan, bindings)),
          {{"sum", take(Footprint::none({length}))}}));
      if (check(empty.diagnostics.native_dispatch_count == 0,
                "empty discovery dispatched native work"))
        return 1;
    }
    for (const bool run_limit : {false, true}) {
      ExecutionContext fresh(registry, config);
      const auto fresh_root = take(fresh.resource_budget());
      const auto inputs = bind(source(fresh_root, ElementType::Float32),
                               source(fresh_root, ElementType::Int64));
      GraphContext metered(document(2, 16));
      auto plan = take(Compiler(registry).compile(metered, options));
      ExecutionOptions bounds;
      if (run_limit)
        bounds.maximum_dependency_work = 8192;
      else
        bounds.dependencies.maximum_work = 8192;
      {
        auto single =
            fresh.execute_fragments(take(fresh.freeze(plan.plan, inputs)),
                                    {{"sum", point(0)}}, {}, bounds);
        if (check(single.ok() &&
                      single.value().diagnostics.native_dispatch_count > 0,
                  "one discovery rejected by cumulative work fixture"))
          return 1;
      }
      auto failed = fresh.execute_fragments(
          take(fresh.freeze(plan.plan, inputs)),
          {{"sum", take(point(0).unite(point(1)))}}, {}, bounds);
      if (check(failed.status().code == ErrorCode::ResourceExhausted &&
                    fresh_root.statistics().live[ResourceKind::Payload] ==
                        source_bytes,
                "discovery work allowance reset between polls")) {
        std::cerr << "run_limit=" << run_limit << " " << failed.status().message
                  << '\n';
        return 1;
      }
    }
    std::cout << "Metal discovery: protocol failures, mixed roles, empty, "
                 "cumulative work and owner retirement passed\n";
    thread_probes(registry, &entered);
    GraphContext large(document(128));
    auto large_plan = Compiler(registry).compile(large, options).take_value();
    // Table: 18448 logical bytes -> 32768 native capacity. Control atlas:
    // 8+160. Discovery allocates neither the output nor the declared scratch;
    // those allocations occur in the later compute phase after its table
    // retires.
    const auto minimum =
        source_bytes +
        large_plan.plan.steps()[0].traits.outputs[0].continuation_bytes +
        32768 + 168;
    for (auto bytes : {minimum - 1, minimum}) {
      ExecutionContextConfig tight;
      tight.gpu_enabled = true;
      tight.maximum_live_bytes = bytes;
      ExecutionContext small(registry, tight);
      const auto small_root = take(small.resource_budget());
      const auto small_bindings = bind(source(small_root, ElementType::Float32),
                                       source(small_root, ElementType::Int64));
      auto query = small.freeze(large_plan.plan, small_bindings).take_value();
      auto run = small.execute_fragments(query, {{"sum", point(0)}});
      if (check(bytes == minimum
                    ? run.ok()
                    : run.status().code == ErrorCode::ResourceExhausted,
                "discovery actual admission frontier failed"))
        return 1;
      if (run.ok() && check(run.value().diagnostics.native_dispatch_count > 0,
                            "bounded discovery skipped native work"))
        return 1;
      if (run.ok()) {
        run = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
        if (check(small_root.statistics().live[ResourceKind::Payload] ==
                          source_bytes &&
                      small_root.statistics().peak[ResourceKind::Payload] ==
                          minimum,
                  "discovery owners did not retire at the measured frontier"))
          return 1;
        auto retry = small.execute_fragments(query, {{"sum", point(0)}});
        if (check(retry.ok() &&
                      retry.value().diagnostics.native_dispatch_count > 0,
                  "discovery frontier retry retained an old result"))
          return 1;
      }
    }
    std::cout << "discovery native admission: " << minimum - 1 << " rejected, "
              << minimum << " passed\n";
  }
  return 0;
} catch (const gpu_result::Failure& error) {
  std::cerr << "discovery Result failure: "
            << static_cast<int>(error.status.code) << " "
            << error.status.message << '\n';
  return 1;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
