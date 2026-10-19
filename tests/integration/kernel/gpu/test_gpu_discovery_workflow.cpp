#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/gpu_result_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using gpu_result::take;
constexpr std::uint64_t length = 8192;
constexpr std::uint64_t source_bytes = length * 12;
SchemaTemplate schema(ElementType type) {
  auto value = gpu_result::schema(type, length);
  value.tensors[0].descriptor.shape = {1, length};
  value.tensors[0].layout.spatial = true;
  value.tensors[0].layout.channel_axis.reset();
  value.tensors[0].layout.height_axis = 0;
  value.tensors[0].layout.width_axis = 1;
  value.id =
      type == ElementType::Float32 ? "discovery.data" : "discovery.control";
  return value;
}
ResultRef source(const ResourceBudget& root, ElementType type,
                 std::int64_t first = 0) {
  auto builder =
      take(ResultBuilder::start(root, schema(type), "discovery.input"));

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
  gpu_result::check(builder.publish_tensor(
      0, Region::whole({1, length}),
      {0, {static_cast<std::int64_t>(length * (data ? 4 : 8)), data ? 4 : 8}},
      std::move(buffer).freeze()));
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
TileRequest point(const ResourceBudget& root, std::uint64_t at) {
  return {{{0, take(TileSet::from_indices(root, {at}))}}};
}
TileRequest pair(const ResourceBudget& root) {
  return {{{0, take(TileSet::from_indices(root, {0, 1}))}}};
}
float number(const ResultRef& result, std::uint64_t at) {
  float value = 0;
  gpu_result::check(
      result.read_tensor(take(result.descriptor()), 0, {0, at}, &value, 4));
  return value;
}
bool input_tiles(const DemandResult& result, std::size_t step, TileIndex output,
                 std::size_t input, const TileSet& expected) {
  auto record = take(result.dependencies.tile_record({step, 0, 0, output}));
  if (!record)
    return false;
  for (const auto& need : record->inputs) {
    const auto* source = std::get_if<PlanWorkflowInput>(&need.producer);
    if (source && source->declaration_index == input)
      return need.producer_output == 0 && need.tiles == expected;
  }
  return false;
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
    auto output = gpu_result::builder(phase);
    const float zero = 0;
    gpu_result::check(output.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&zero), 4)));
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
    for (auto budget : {8288, 8289}) {
      ExecutionContextConfig config;
      config.gpu_enabled = true;
      ExecutionContext context(registry, config);
      const auto root = take(context.resource_budget());
      ExecutionOptions bounds;
      bounds.dependencies.maximum_work = budget;
      entered->store(false);
      auto run = context.execute_fragments(
          compiled.plan.value(), {}, {{"empty", point(root, 0)}}, {}, bounds);
      const auto expected = wrong            ? ErrorCode::InvalidArgument
                            : budget == 8288 ? ErrorCode::ResourceExhausted
                                             : ErrorCode::Ok;
      if (run.status().code != expected || entered->load() == wrong) {
        std::cerr << "thread discovery wrong=" << wrong << " budget=" << budget
                  << " " << run.status().message << '\n';
        throw std::runtime_error("thread discovery admission failed");
      }
      if (run.ok() && run.value().diagnostics.native_dispatch_count == 0)
        throw std::runtime_error("parallel discovery lacked native submission");
      run = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      if (root.statistics().live[ResourceKind::Payload] != 0)
        throw std::runtime_error("thread discovery retained native table");
    }
  }
  std::cout << "Metal discovery: wrong thread rejected; concurrent work "
               "8288 rejected, 8289 passed\n";
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
    if (gpu && (!PS_TEST_HAS_METAL || !context.gpu_enabled())) {
      std::cout
          << "CPU discovery oracle passed; Metal unavailable, native skipped\n";
      return 77;
    }
    PlanningOptions options;
    options.tile_extent = 1;
    options.execution_mode =
        gpu ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
    GraphContext graph(document());
    auto compiled = Compiler(registry).compile(graph, options).take_value();
    auto run_plan = compiled.plan.value();
    ExecutionBindings run_bindings = bindings;
    auto result = context.execute_fragments(run_plan, run_bindings,
                                            {{"sum", pair(root)}});
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    if (check(result.ok(), "discovery workflow failed"))
      return 1;
    for (std::uint64_t at : {0, 1}) {
      if (check(number(result.value().results.at("sum"), at) == (at ? 24 : 8),
                "discovery arithmetic oracle failed"))
        return 1;
    }
    const auto step = compiled.plan.value()
                          .return_signature()
                          .at(compiled.plan.value().outputs().at("sum"))
                          .record_index;
    const TileInterval all{0, length};
    const auto fixed = take(TileSet::from_intervals(root, &all, 1));
    for (TileIndex output : {0, 1})
      for (std::size_t input : {0, 1})
        if (check(
                input_tiles(result.value(), step, output, input,
                            input ? point(root, output).members.at(0) : fixed),
                "discovery changed compiled tile grants"))
          return 1;
    if (check((gpu ? result.value().diagnostics.native_dispatch_count > 0
                   : result.value().diagnostics.native_dispatch_count == 0),
              "discovery/resume dispatch count failed"))
      return 1;
    auto demand =
        context.open_demand(compiled.plan.value(), bindings).take_value();
    if (check(demand.request({{"sum", point(root, 0)}}).ok() &&
                  demand
                      .replace_bindings(
                          bind(data, source(root, ElementType::Int64, 1)))
                      .ok(),
              "dynamic binding update failed"))
      return 1;
    auto changed = demand.request({{"sum", point(root, 0)}});
    if (check(changed.ok(), "changed discovery failed"))
      return 1;
    if (check(number(changed.value().results.at("sum"), 0) == 24,
              "new GPU address selection failed"))
      return 1;
    if (check(input_tiles(changed.value(), step, 0, 0, fixed) &&
                  input_tiles(changed.value(), step, 0, 1,
                              point(root, 0).members.at(0)),
              "control values changed compiled grants"))
      return 1;
    auto edit = [&](std::uint64_t at) {
      auto builder = take(
          ResultBuilder::start(root, schema(ElementType::Float32), "edited"));

      std::vector<float> values(length, 0);
      values[0] = 3;
      values[4096] = 5;
      values[1] = 11;
      values[4097] = 13;
      values[at] += 1;
      gpu_result::check(builder.publish_tensor(
          0, Region::whole({1, length}),
          ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                   length * 4)));
      return take(builder.seal());
    };
    auto changed_old_selection = take(demand.replace_bindings(
        bind(edit(0), source(root, ElementType::Int64, 1))));
    auto dirty = take(demand.replace_bindings(
        bind(edit(1), source(root, ElementType::Int64, 1))));
    if (check(
            changed_old_selection.potential_dirty.at("sum") == point(root, 0) &&
                dirty.potential_dirty.at("sum") == point(root, 0),
            "dirty evidence must cover the complete compiled grant"))
      return 1;
    auto bad_control_plan = compiled.plan.value();
    ExecutionBindings bad_control_bindings =
        bind(data, source(root, ElementType::Int64, -1));
    if (check(context.execute_fragments(bad_control_plan, bad_control_bindings,
                                        {{"sum", point(root, 0)}})
                      .status()
                      .code == ErrorCode::InvalidArgument,
              "signed control access contract differs"))
      return 1;
    std::cout << (gpu ? "Metal" : "CPU")
              << " discovery: sums=8,24; fixed controls/data; changed=24; "
                 "dispatches="
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
          bad.plan.value(), bindings, {{"sum", point(root, 0)}}, {}, bounds);
      if (check(denied.status().code == (mode == 1
                                             ? ErrorCode::InvalidArgument
                                             : ErrorCode::ResourceExhausted),
                "overflow/bypass/disabled discovery accepted"))
        return 1;
    }
    for (unsigned mode : {3, 4, 5, 6, 8, 9, 10, 11, 12, 14, 15, 19}) {
      ExecutionContext fresh(registry, config);
      const auto fresh_root = take(fresh.resource_budget());
      const auto inputs = bind(source(fresh_root, ElementType::Float32),
                               source(fresh_root, ElementType::Int64));
      GraphContext invalid(document(2, mode));
      auto plan = take(Compiler(registry).compile(invalid, options));
      auto failed = fresh.execute_fragments(plan.plan.value(), inputs,
                                            {{"sum", point(root, 0)}});
      if (mode == 19 &&
          check(failed.status().reason == FailureReason::UnauthorizedRead &&
                    failed.status().detail.origin == FailureOrigin::Protocol,
                "descriptor receipt granted payload"))
        return 1;
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
      auto retry = fresh.execute_fragments(compiled.plan.value(), inputs,
                                           {{"sum", point(root, 0)}});
      if (check(retry.ok() &&
                    retry.value().diagnostics.native_dispatch_count > 0 &&
                    number(retry.value().results.at("sum"), 0) == 8,
                "normal discovery after failure did not execute"))
        return 1;
    }
    {
      GraphContext mixed(document(2, 13));
      auto plan = take(Compiler(registry).compile(mixed, options));
      auto run = take(context.execute_fragments(plan.plan.value(), bindings,
                                                {{"sum", point(root, 0)}}));
      if (check(
              number(run.results.at("sum"), 0) == 8 &&
                  run.diagnostics.native_dispatch_count > 0 &&
                  input_tiles(run, plan.plan.value().output("sum").record_index,
                              0, 0, fixed),
              "unordered discovery tile normalization failed"))
        return 1;
      auto empty = take(context.execute_fragments(plan.plan.value(), bindings,
                                                  {{"sum", TileRequest{}}}));
      if (check(empty.diagnostics.native_dispatch_count == 0,
                "empty discovery dispatched native work"))
        return 1;
    }
    for (unsigned mode : {17, 18}) {
      GraphContext valid(document(2, mode));
      auto plan = take(Compiler(registry).compile(valid, options));
      auto run = take(context.execute_fragments(plan.plan.value(), bindings,
                                                {{"sum", point(root, 0)}}));
      if (check(
              number(run.results.at("sum"), 0) == 8 &&
                  run.diagnostics.native_dispatch_count == (mode == 17 ? 3 : 2),
              "repeated or full interval discovery failed"))
        return 1;
      if (mode == 18) {
        auto record = take(run.dependencies.tile_record(
            {plan.plan.value().output("sum").record_index, 0, 0, 0}));
        bool whole = false;
        for (const auto& need : record->inputs) {
          const auto* source = std::get_if<PlanWorkflowInput>(&need.producer);
          whole |= source && source->declaration_index == 0 &&
                   need.tensor_slot == 0 &&
                   need.tiles == take(TileSet::range(root, 0, 8192));
        }
        if (check(whole, "full interval receipt lost producer dependency"))
          return 1;
      }
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
        auto single = fresh.execute_fragments(
            plan.plan.value(), inputs, {{"sum", point(root, 0)}}, {}, bounds);
        if (!single.ok())
          std::cerr << "single run_limit=" << run_limit << " "
                    << single.status().message << '\n';
        if (check(single.ok() &&
                      single.value().diagnostics.native_dispatch_count > 0,
                  "one discovery rejected by cumulative work fixture"))
          return 1;
      }
      GraphContext repeated(document(2, 17));
      auto repeated_plan = take(Compiler(registry).compile(repeated, options));
      auto failed =
          fresh.execute_fragments(repeated_plan.plan.value(), inputs,
                                  {{"sum", point(fresh_root, 0)}}, {}, bounds);
      if (check(failed.status().code == ErrorCode::ResourceExhausted &&
                    fresh_root.statistics().live[ResourceKind::Payload] ==
                        source_bytes,
                "discovery work allowance reset between polls")) {
        std::cerr << "run_limit=" << run_limit << " " << failed.status().message
                  << '\n';
        return 1;
      }
    }
    std::cout << "Metal discovery: protocol failures, unordered tiles, empty, "
                 "cumulative work and owner retirement passed\n";
    thread_probes(registry, &entered);
    GraphContext large(document(128));
    auto large_plan = Compiler(registry).compile(large, options).take_value();
    // The control atlas (8 + 160 bytes) survives selection. The table retires
    // before computation allocates 8 scratch bytes and the full static data
    // atlas: 8192 float samples plus 256 directory slots for 128 atlas tiles.
    // The 20,480-byte directory has two 16 KiB native capacity pages.
    const auto directory_bytes = (length / 64) * 2 * kFragmentAtlasSlotBytes;
    const auto directory_capacity = ((directory_bytes + 16383) / 16384) * 16384;
    const auto data_atlas_bytes = length * 4 + directory_capacity;
    const auto minimum = source_bytes +
                         large_plan.plan.value()
                             .steps()[0]
                             .traits.outputs[0]
                             .continuation_bytes +
                         168 +
                         std::max<std::uint64_t>(4112, 8 + data_atlas_bytes);
    for (auto bytes : {minimum - 1, minimum}) {
      ExecutionContextConfig tight;
      tight.gpu_enabled = true;
      tight.maximum_live_bytes = bytes;
      ExecutionContext small(registry, tight);
      const auto small_root = take(small.resource_budget());
      const auto small_bindings = bind(source(small_root, ElementType::Float32),
                                       source(small_root, ElementType::Int64));
      auto query_plan = large_plan.plan.value();
      ExecutionBindings query_bindings = small_bindings;
      auto run = small.execute_fragments(query_plan, query_bindings,
                                         {{"sum", point(root, 0)}});
      if (!run.ok() && bytes == minimum)
        std::cerr << "frontier bytes=" << bytes << " peak="
                  << small_root.statistics().peak[ResourceKind::Payload] << " "
                  << run.status().message << '\n';
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
        auto retry = small.execute_fragments(query_plan, query_bindings,
                                             {{"sum", point(root, 0)}});
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
