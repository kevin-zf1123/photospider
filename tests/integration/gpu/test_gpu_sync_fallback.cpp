#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/gpu_result_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
constexpr std::uint64_t length = 5000;
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char shader[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void increment(device const float* x [[buffer(0)]],"
    "device float* y [[buffer(1)]], uint i [[thread_position_in_grid]])"
    "{y[i]=x[i]+1.0f;}";
// NOLINTEND
std::atomic<unsigned> cpu_calls{0}, gpu_calls{0}, starts{0};
std::atomic<bool> unavailable{false};
int check(bool ok, const char* message) {
  if (!ok)
    std::cerr << message << '\n';
  return ok ? 0 : 1;
}
Footprint point(std::uint64_t at) {
  return Footprint::from_regions({length}, {Region({{at, 1}})}).take_value();
}
template <bool Whole>
Result<ResultProgramPoll> increment(const ResultProgramPhase& phase) try {
  const auto demand = gpu_result::demand(phase);
  if (!phase.tensors || phase.tensors->empty())
    return gpu_result::need(demand);
  auto result = gpu_result::builder(phase);
  auto support = gpu_result::identity(phase, Whole);
  if (phase.query.backend == Backend::Cpu)
    ++cpu_calls;
  else
    ++gpu_calls;
  for (const auto& box : demand.boxes()) {
    const auto count = gpu_result::take(box.element_count());
    auto output = gpu_result::take(phase.allocator.allocate(count * 4));
    if (phase.query.backend == Backend::Cpu) {
      for (std::uint64_t i = 0; i < count; ++i) {
        float x = 0;
        gpu_result::check(
            phase.read_tensor(0, 0, {box.dimensions()[0].offset + i}, &x, 4));
        x += 1;
        std::memcpy(output.data() + i * 4, &x, 4);
      }
    } else {
      auto window = gpu_result::take(phase.acquire_native_tensor(0, 0, box));
      auto row = gpu_result::take(window.row_run({box.dimensions()[0].offset}));
      if (row.samples != count || row.sample_stride_bytes != 4)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed, "native source layout"});
      if (Whole && unavailable)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::BackendUnavailable,
                   "example native implementation unavailable"});
      if (!phase.gpu)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::BackendUnavailable, "missing native service"});
      const auto* api = phase.gpu;
      std::uint64_t source = 0, destination = 0;
      gpu_result::check(gpu_result::gpu_status(
          phase, api->buffer(api->context, row.data, count * 4, 0, &source)));
      gpu_result::check(gpu_result::gpu_status(
          phase, api->buffer(api->context, output.data(), output.size(), 1,
                             &destination)));
      ps_gpu_buffer_binding_v1 bindings[] = {
          {sizeof(ps_gpu_buffer_binding_v1), 0, source, 0, count * 4, 0},
          {sizeof(ps_gpu_buffer_binding_v1), 1, destination, 0, output.size(),
           1}};
      ps_gpu_dispatch_v1 command{};
      command.struct_size = sizeof(command);
      command.source = shader;
      command.source_size = sizeof(shader) - 1;
      command.entry = "increment";
      command.entry_size = 9;
      command.buffers = bindings;
      command.buffer_count = 2;
      command.grid[0] = count;
      command.grid[1] = command.grid[2] = 1;
      gpu_result::check(gpu_result::gpu_status(
          phase, api->execute(api->context, &command, 1)));
      gpu_result::check(
          gpu_result::gpu_status(phase, api->release(api->context, source)));
      gpu_result::check(gpu_result::gpu_status(
          phase, api->release(api->context, destination)));
    }
    gpu_result::check(result.publish_tensor(
        0, box, StridedLayout{0, {4}, {box.dimensions()[0].offset}},
        std::move(output).freeze(), support, {true, true, true, true},
        phase.query.cancellation));
  }
  return Result<ResultProgramPoll>(
      ResultPublication{gpu_result::take(result.seal()), true});
} catch (const gpu_result::Failure& failed) {
  return Result<ResultProgramPoll>(failed.status);
}
struct Stage {
  unsigned stage = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (stage++ == 0)
      return gpu_result::need(gpu_result::demand(phase));
    if (stage != 2)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::Internal, "state not restarted"});
    if (phase.query.backend == Backend::Gpu && unavailable) {
      auto scratch = phase.allocator.allocate(4);
      if (!scratch.ok())
        return Result<ResultProgramPoll>(scratch.status());
      return Result<ResultProgramPoll>(
          Status{ErrorCode::BackendUnavailable,
                 "staged native implementation unavailable"});
    }
    return gpu_result::view(phase);
  }
};
WorkflowDocument document(const std::string& first, bool chain) {
  WorkflowDocument doc;
  doc.inputs = {gpu_result::declaration(
      1, "x", gpu_result::schema(ElementType::Float32, length))};
  doc.nodes = {{1, first, {WorkflowInputReference{1}}, {}}};
  if (chain)
    doc.nodes.push_back(
        {2, "example.increment", {WorkflowNodeOutput{1, "value"}}, {}});
  doc.outputs = {{"y", chain ? 2U : 1U, "value"}};
  return doc;
}
int verify(const Result<DemandResult>& result, std::uint64_t at,
           float expected) {
  if (!result.ok())
    std::cerr << static_cast<int>(result.status().code) << ": "
              << result.status().message << '\n';
  return check(result.ok() && gpu_result::number(result.value().results.at("y"),
                                                 at) == expected,
               "independent increment oracle failed");
}
struct FallbackReads {
  bool supplied = false, cpu_read;
  explicit FallbackReads(bool read) : cpu_read(read) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    const bool gpu = phase.query.backend == Backend::Gpu;
    if (!supplied && (gpu || cpu_read)) {
      supplied = true;
      return gpu_result::need(point(gpu ? 1 : 2));
    }
    if (gpu)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::BackendUnavailable, "after ancestor completion"});
    auto result = gpu_result::builder(phase, cpu_read);
    float value = 7;
    if (cpu_read)
      gpu_result::check(phase.read_tensor(0, 0, {2}, &value, 4));
    auto relation = gpu_result::take(ResultRelation::cartesian(
        phase.resources, length,
        cpu_read ? ResultSupport{0, 1, 2, 1, ResultSupportTarget::Tensor, 0}
                 : ResultSupport{}));
    for (const auto& box : gpu_result::demand(phase).boxes())
      gpu_result::check(result.publish_tensor(
          0, box, {reinterpret_cast<const uint8_t*>(&value), 4}, relation,
          {true, true, true, true}, phase.query.cancellation));
    return Result<ResultProgramPoll>(
        ResultPublication{gpu_result::take(result.seal()), true});
  } catch (const gpu_result::Failure& error) {
    return Result<ResultProgramPoll>(error.status);
  }
};
struct Ancestor {
  unsigned* calls;
  bool whole;
  Ancestor(unsigned* calls, bool whole) : calls(calls), whole(whole) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!phase.tensors || phase.tensors->empty())
      return gpu_result::need(gpu_result::demand(phase));
    ++*calls;
    return gpu_result::view(phase, whole);
  }
};
ExecutionBindings make_bindings(const ResourceBudget& root) {
  const auto source_schema = gpu_result::schema(ElementType::Float32, length);
  auto source_builder =
      gpu_result::take(ResultBuilder::start(root, source_schema, "gpu.input"));
  gpu_result::check(source_builder.bind_descriptor_relation(
      gpu_result::take(ResultRelation::cartesian(root, 1, {}))));
  std::vector<float> values(length);
  for (std::uint64_t i = 0; i < length; ++i)
    values[i] = static_cast<float>(i);
  gpu_result::check(source_builder.publish_tensor(
      0, Region::whole({length}),
      {reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * 4},
      gpu_result::take(ResultRelation::cartesian(root, length, {})),
      {true, true, true, true}));
  ExecutionBinding source;
  source.name = "x";
  source.result = gpu_result::take(source_builder.seal());
  return ExecutionBindings{{source}};
}
int fallback_records() {
  for (unsigned mode = 0; mode < 5; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    unsigned ancestors = 0;
    OperationDefinition ancestor;
    ancestor.key = "ancestor";
    ancestor.traits.input_count = 1;
    ancestor.traits.input_schema.resize(1);
    ancestor.traits.input_schema[0].kind = OperationPortKind::Result;
    ancestor.traits.input_schema[0].result_schema_id = "example.gpu.tensor";
    ancestor.traits.input_schema[0].result_schema_version = 1;
    ancestor.traits.outputs = {
        gpu_result::output(gpu_result::schema(ElementType::Float32, length))};
    ancestor.traits.outputs[0].continuation_bytes = sizeof(Ancestor);
    ancestor.traits.outputs[0].region_rule =
        (mode == 2 || mode == 3) ? OperationRegionRule::Whole
                                 : OperationRegionRule::Dependency;
    ancestor.start_result = [&ancestors, mode](
                                const ResultProgramQuery&,
                                const BufferAllocator& allocator) {
      return ResultContinuation::make<Ancestor>(allocator, &ancestors,
                                                (mode == 2 || mode == 3));
    };
    if (check(registry->register_operation(ancestor).ok(), "ancestor"))
      return 1;
    auto child = ancestor;
    child.key = "fallback";
    child.traits.outputs[0].region_rule = OperationRegionRule::Dependency;
    child.traits.supports_gpu = true;
    child.traits.allows_cpu_fallback = true;
    child.traits.outputs[0].continuation_bytes = sizeof(FallbackReads);
    child.traits.outputs[0].maximum_dependency_stages = 4;
    child.start_result = [mode](const ResultProgramQuery&,
                                const BufferAllocator& allocator) {
      return ResultContinuation::make<FallbackReads>(
          allocator, mode == 1 || mode == 2 || mode == 4);
    };
    if (check(
            registry->register_operation(child).ok() && registry->freeze().ok(),
            "fallback registration"))
      return 1;
    auto doc = document("ancestor", false);
    doc.nodes.push_back({2, "fallback", {WorkflowNodeOutput{1, "value"}}, {}});
    doc.outputs = {{"y", 2, "value"}};
    DemandQuery query{{"y", point(0)}};
    if (mode == 1 || mode == 4) {
      doc.outputs.push_back({"a", 1, "value"});
      query.emplace("a", point(0));
      if (mode == 4) {
        doc.outputs.push_back({"b", 1, "value"});
        query.emplace("b", point(3));
      }
    }
    GraphContext graph(doc);
    PlanningOptions planning;
    planning.execution_mode = ExecutionMode::NativeGpu;
    auto plan = Compiler(registry).compile(graph, planning).take_value().plan;
    ExecutionContext context(registry, {1, true, 8, 1 << 20, 0});
    const auto root = context.resource_budget().take_value();
    const auto bindings = make_bindings(root);
    auto frozen = context.freeze(plan, bindings).take_value();
    ExecutionOptions options;
    options.dependencies.sets.maximum_boxes = mode == 0   ? 16
                                              : mode == 3 ? 12
                                                          : 64;
    auto result = context.execute_fragments(frozen, query, {}, options);
    if (verify(result, 0, mode == 0 || mode == 3 ? 7 : 2))
      return 1;
    const auto& evidence = result.value().dependencies;
    if (mode == 0 || mode == 3) {
      if (check(evidence.record_count() == 2 &&
                    evidence.source_support().value().empty(),
                "abandoned GPU ancestor retained"))
        return 1;
      auto tile = plan.tile_plan("y", Region({{0, 1}})).take_value();
      auto ordinary = context.execute(tile, bindings, {}, options);
      if (check(ordinary.ok() &&
                    ordinary.value().dependencies.record_count() == 2,
                "ordinary fallback retained abandoned records"))
        return 1;
    } else if (mode == 1 || mode == 4) {
      auto expected_support = point(0).unite(point(2)).take_value();
      if (mode == 4)
        expected_support = expected_support.unite(point(3)).take_value();
      if (check(evidence.source_support().value().at("x") == expected_support &&
                    ancestors == (mode == 4 ? 4U : 3U),
                "rollback lost prior rows or retained abandoned rows"))
        return 1;
      for (const auto& name : {"a", "y"}) {
        auto restricted =
            gpu_result::take(evidence.restrict({{name, point(0)}}));
        if (check(restricted.source_support().value().at("x") ==
                      point(std::string(name) == "a" ? 0 : 2),
                  "rollback mixed distinct output support"))
          return 1;
      }
      if (mode == 4) {
        auto restricted =
            gpu_result::take(evidence.restrict({{"b", point(3)}}));
        if (check(restricted.source_support().value().at("x") == point(3),
                  "rollback preserves a second named query of the same step"))
          return 1;
      }
      for (std::uint64_t edited : {0, 1, 2}) {
        auto dirty = gpu_result::take(evidence.potential_dirty(
            "x", point(edited), 1, {}, ResultSupportTarget::Tensor, 0));
        if (check(
                dirty.at("a") ==
                        (edited == 0
                             ? point(0)
                             : Footprint::none({length}).take_value()) &&
                    dirty.at("y") ==
                        (edited == 2 ? point(0)
                                     : Footprint::none({length}).take_value()),
                "rollback lost typed dirty subscriptions"))
          return 1;
      }
    } else if (check(
                   ancestors == 1 && evidence.record_count() == 4 &&
                       evidence.source_support().value().at("x") ==
                           Footprint::all({length}).take_value(),
                   "Whole owner/evidence must survive fallback exactly once")) {
      return 1;
    }
    if (mode == 2) {
      auto tile = plan.tile_plan("y", Region({{0, 1}})).take_value();
      auto ordinary = context.execute(tile, bindings, {}, options);
      if (check(ordinary.ok() && ancestors == 2 &&
                    ordinary.value().dependencies.source_support().value().at(
                        "x") == Footprint::all({length}).take_value(),
                "ordinary Whole fallback must restore complete evidence once"))
        return 1;
    }
  }
  std::cout << "fallback record rollback: bounded retry, prior rows and Whole "
               "evidence passed\n";
  return 0;
}
}  // namespace
int main() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "example.increment";
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.input_schema[0].kind = OperationPortKind::Result;
  op.traits.input_schema[0].result_schema_id = "example.gpu.tensor";
  op.traits.input_schema[0].result_schema_version = 1;
  op.traits.outputs = {
      gpu_result::output(gpu_result::schema(ElementType::Float32, length))};
  op.traits.supports_gpu = true;
  op.traits.allows_cpu_fallback = true;
  op.traits.workspace_bytes = 32768;
  op.start_result = [](const ResultProgramQuery&, const BufferAllocator&) {
    return ResultContinuation::stateless<increment<false>>();
  };
  if (check(registry->register_operation(op).ok(), "register increment"))
    return 1;
  op.key = "example.whole";
  op.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  op.start_result = [](const ResultProgramQuery&, const BufferAllocator&) {
    return ResultContinuation::stateless<increment<true>>();
  };
  if (check(registry->register_operation(op).ok(), "register Whole"))
    return 1;
  op.key = "example.staged";
  op.traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  op.traits.outputs[0].data_movement = DataMovementKind::BitwiseMapped;
  op.traits.outputs[0].continuation_bytes = sizeof(Stage);
  op.traits.outputs[0].maximum_dependency_stages = 4;
  op.traits.workspace_bytes = 4;
  op.start_result = [](const ResultProgramQuery&,
                       const BufferAllocator& allocator) {
    ++starts;
    return ResultContinuation::make<Stage>(allocator);
  };
  if (check(registry->register_operation(op).ok(), "register staged"))
    return 1;
  op.key = "example.start_rejected";
  op.start_result = [](const ResultProgramQuery& query,
                       const BufferAllocator& allocator) {
    ++starts;
    if (query.backend == Backend::Gpu && unavailable)
      return Result<ResultContinuation>(
          Status{ErrorCode::BackendUnavailable, "start rejected"});
    return ResultContinuation::make<Stage>(allocator);
  };
  if (check(registry->register_operation(op).ok() && registry->freeze().ok(),
            "register start rejection"))
    return 1;
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  for (bool enabled : {false, true}) {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.gpu_enabled = enabled;
    config.maximum_live_bytes = 1 << 20;
    config.result_cache_bytes = 1 << 18;
    ExecutionContext context(registry, config);
    if (enabled && !context.gpu_enabled()) {
      std::cout << "missing-device CPU fallback passed; native skipped\n";
      return 77;
    }
    GraphContext graph(document("example.increment", false));
    auto plan = Compiler(registry).compile(graph, planning).take_value().plan;
    const auto root = context.resource_budget().take_value();
    const auto bindings = make_bindings(root);
    auto frozen = context.freeze(plan, bindings).take_value();
    const auto q = point(0).unite(point(2)).take_value();
    auto result = context.execute_fragments(frozen, {{"y", q}});
    if (verify(result, 0, 1) || verify(result, 2, 3) ||
        check((enabled ? result.value().diagnostics.native_dispatch_count > 0
                       : result.value().diagnostics.native_dispatch_count == 0),
              "dispatch count") ||
        check(result.value().diagnostics.selected_backends.at({1, 0}) ==
                  (enabled ? Backend::Gpu : Backend::Cpu),
              "actual backend"))
      return 1;
    if (!enabled) {
      const auto& diagnostics = result.value().diagnostics;
      if (check(diagnostics.operation_timings.size() == 2 &&
                    diagnostics.fallback_reasons.size() == 1,
                "missing-device rejection/CPU attempt pairs"))
        return 1;
      for (unsigned i = 0; i < 2; ++i)
        if (check(
                diagnostics.operation_timings[i].backend ==
                        (i % 2 ? Backend::Cpu : Backend::Gpu) &&
                    diagnostics.operation_timings[i].outcome ==
                        (i % 2 ? ErrorCode::Ok : ErrorCode::BackendUnavailable),
                "missing-device attempt outcome"))
          return 1;
    }
    result = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
    auto warm_scope = context.freeze(plan, bindings).take_value();
    auto warm = context.execute_fragments(warm_scope, {{"y", q}});
    if (verify(warm, 2, 3) ||
        check((enabled ? warm.value().diagnostics.cache_hits > 0
                       : warm.value().diagnostics.cache_hits == 0) &&
                  warm.value().diagnostics.native_dispatch_count == 0 &&
                  (!enabled ||
                   warm.value().diagnostics.operation_timings.empty()),
              "completed Result cache reuse/fallback isolation"))
      return 1;
    for (const auto& key :
         {"example.whole", "example.staged", "example.start_rejected"}) {
      context.clear_result_cache();
      GraphContext chain(document(key, true));
      auto chain_plan =
          Compiler(registry).compile(chain, planning).take_value().plan;
      auto captured = context.freeze(chain_plan, bindings).take_value();
      unavailable = true;
      const auto before = starts.load();
      auto fallback = context.execute_fragments(captured, {{"y", point(0)}});
      const bool staged = std::string(key) != "example.whole";
      const float expected = staged ? 1 : 2;
      const auto cache = context.cache_statistics();
      if (cache.retained_bytes != cache.native_retained_bytes)
        std::cerr << "fallback " << key << " enabled=" << enabled
                  << " retained=" << cache.retained_bytes
                  << " native=" << cache.native_retained_bytes << '\n';
      if (verify(fallback, 0, expected) ||
          check(fallback.value().diagnostics.selected_backends.at({1, 0}) ==
                    Backend::Cpu,
                "producer fallback backend") ||
          check(fallback.value().diagnostics.selected_backends.at({2, 0}) ==
                    (enabled ? Backend::Gpu : Backend::Cpu),
                "descendant backend") ||
          check(cache.retained_bytes == cache.native_retained_bytes,
                "fallback Result ancestry retained") ||
          check(!staged || starts == before + (enabled ? 2 : 1),
                "continuation restart count after device admission"))
        return 1;
      if (enabled) {
        unsigned rejected = 0;
        for (const auto& attempt :
             fallback.value().diagnostics.operation_timings)
          if (attempt.output.node_id == 1 && attempt.backend == Backend::Gpu &&
              attempt.outcome == ErrorCode::BackendUnavailable)
            ++rejected;
        if (check(rejected == 1, "native rejection attempt missing"))
          return 1;
      }
      fallback = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      unavailable = false;
      auto retry = context.execute_fragments(captured, {{"y", point(0)}});
      if (verify(retry, 0, expected) ||
          check(retry.value().diagnostics.cache_hits == 0,
                "fallback ancestry reused"))
        return 1;
      auto proof = retry.value().dependencies.source_support();
      if (check(proof.ok() &&
                    proof.value().at("x") ==
                        (staged ? point(0)
                                : Footprint::all({length}).take_value()),
                "exact dependency support"))
        return 1;
    }
  }
  if (fallback_records())
    return 1;
  // Whole dense input/output allocations each round independently to 32 KiB.
  GraphContext whole(document("example.whole", false));
  auto plan = Compiler(registry).compile(whole, planning).take_value().plan;
  for (std::uint64_t native_limit : {65535, 65536}) {
    const auto limit = length * 4 + native_limit;
    ExecutionContext context(registry, {1, true, 8, limit, 0});
    const auto root = context.resource_budget().take_value();
    const auto bindings = make_bindings(root);
    auto frozen = context.freeze(plan, bindings).take_value();
    if (check(root.statistics().live[ResourceKind::Payload] == length * 4,
              "Result source payload admission"))
      return 1;
    auto result = context.execute_fragments(frozen, {{"y", point(0)}});
    if (native_limit == 65535) {
      if (check(result.status().code == ErrorCode::ResourceExhausted &&
                    root.statistics().live[ResourceKind::Payload] == length * 4,
                "one-less admission and failed owner retirement"))
        return 1;
    } else {
      if (verify(result, 0, 1) ||
          check(root.statistics().peak[ResourceKind::Payload] == limit &&
                    result.value().diagnostics.native_dispatch_count > 0,
                "actual native admission"))
        return 1;
      // Release the successful output before admitting another Whole. A GPU
      // rejection must retire both rounded allocations before CPU retry.
      result = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      unavailable = true;
      auto fallback = context.execute_fragments(frozen, {{"y", point(0)}});
      if (verify(fallback, 0, 1) ||
          check(
              root.statistics().peak[ResourceKind::Payload] == limit &&
                  root.statistics().live[ResourceKind::Payload] == length * 8 &&
                  fallback.value().diagnostics.native_dispatch_count == 0 &&
                  fallback.value().diagnostics.selected_backends.at({1, 0}) ==
                      Backend::Cpu,
              "fallback native owner retirement"))
        return 1;
      fallback = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      if (check(root.statistics().live[ResourceKind::Payload] == length * 4,
                "released Whole Result owner"))
        return 1;
      unavailable = false;
      auto retry = context.execute_fragments(frozen, {{"y", point(0)}});
      if (verify(retry, 0, 1) ||
          check(retry.value().diagnostics.native_dispatch_count > 0,
                "fresh native retry"))
        return 1;
    }
  }
  std::cout << "synchronous GPU fragments, Whole/staged fallback, cache "
               "isolation and 85536-byte Result admission passed\n";
  return 0;
}
