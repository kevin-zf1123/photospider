#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

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
Result<Value> increment(const OperationInvocation& call, bool can_fail) {
  auto allocated = MutableValue::allocate(call.inputs[0].descriptor(),
                                          call.output_region, call.allocator);
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto output = allocated.take_value();
  if (call.backend == Backend::Cpu) {
    ++cpu_calls;
    for (std::uint64_t i = 0; i < output.size() / 4; ++i) {
      float x = 0;
      std::memcpy(&x, call.inputs[0].bytes().data() + i * 4, 4);
      x += 1;
      std::memcpy(output.data() + i * 4, &x, 4);
    }
  } else {
    ++gpu_calls;
    if (can_fail && unavailable)
      return Result<Value>(Status{ErrorCode::BackendUnavailable,
                                  "example native implementation unavailable"});
    if (!call.gpu)
      return Result<Value>(
          Status{ErrorCode::Internal, "missing native service"});
    std::uint64_t source = 0, destination = 0;
    const auto* api = call.gpu;
    if (api->buffer(api->context, call.inputs[0].bytes().data(),
                    call.inputs[0].bytes().size(), 0, &source) ||
        api->buffer(api->context, output.data(), output.size(), 1,
                    &destination))
      return Result<Value>(Status{ErrorCode::OperationFailed, "native buffer"});
    ps_gpu_buffer_binding_v8 bindings[] = {
        {sizeof(ps_gpu_buffer_binding_v8), 0, source, 0,
         call.inputs[0].bytes().size(), 0},
        {sizeof(ps_gpu_buffer_binding_v8), 1, destination, 0, output.size(),
         1}};
    ps_gpu_dispatch_v8 command{};
    command.struct_size = sizeof(command);
    command.source = shader;
    command.source_size = sizeof(shader) - 1;
    command.entry = "increment";
    command.entry_size = 9;
    command.buffers = bindings;
    command.buffer_count = 2;
    command.grid[0] = output.size() / 4;
    command.grid[1] = command.grid[2] = 1;
    if (api->execute(api->context, &command, 1))
      return Result<Value>(
          Status{ErrorCode::OperationFailed, "native dispatch"});
  }
  return std::move(output).publish();
}
struct Stage {
  unsigned stage = 0;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (stage++ == 0) {
      const auto at = phase.query.outputs.boxes()[0].dimensions()[0].offset;
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{at}, {{0, 1, phase.query.outputs, {}}}}}, {}});
    }
    if (stage != 2)
      return Result<DependencyPoll>(
          Status{ErrorCode::Internal, "state not restarted"});
    if (phase.query.backend == Backend::Gpu && unavailable) {
      // The continuation has already read and retained an upstream stage.
      auto scratch = phase.allocator.allocate(4);
      if (!scratch.ok())
        return Result<DependencyPoll>(scratch.status());
      return Result<DependencyPoll>(
          Status{ErrorCode::BackendUnavailable,
                 "staged native implementation unavailable"});
    }
    auto allocated =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocated.ok())
      return Result<DependencyPoll>(allocated.status());
    auto output = allocated.take_value();
    const auto at = phase.query.outputs.boxes()[0].dimensions()[0].offset;
    auto status = phase.read(0, {at}, output.data(), 4);
    if (!status.ok())
      return Result<DependencyPoll>(status);
    auto fragments = ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs,
        {std::move(output).publish().take_value()}, phase.sets);
    if (!fragments.ok())
      return Result<DependencyPoll>(fragments.status());
    return Result<DependencyPoll>(fragments.take_value());
  }
};
WorkflowDocument document(const std::string& first, bool chain) {
  WorkflowDocument doc;
  doc.inputs = {{1,
                 "x",
                 {ElementType::Float32, {length}},
                 Region::whole({length}),
                 {0, {4}},
                 {}}};
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
    std::cerr << result.status().message << '\n';
  float actual = 0;
  return check(result.ok() &&
                   result.value().values.at("y").read({at}, &actual, 4).ok() &&
                   actual == expected,
               "independent increment oracle failed");
}
struct FallbackReads {
  bool supplied = false;
  bool cpu_read;
  explicit FallbackReads(bool read) : cpu_read(read) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const bool gpu = phase.query.backend == Backend::Gpu;
    if (!supplied && (gpu || cpu_read)) {
      supplied = true;
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{0}, {{0, 1, point(gpu ? 1 : 2), {}}}}}, {}});
    }
    if (gpu)
      return Result<DependencyPoll>(
          Status{ErrorCode::BackendUnavailable, "after ancestor completion"});
    auto writer =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator)
            .take_value();
    float value = 7;
    if (cpu_read) {
      auto status = phase.read(0, {2}, &value, 4);
      if (!status.ok())
        return Result<DependencyPoll>(status);
    }
    std::memcpy(writer.data(), &value, 4);
    return Result<DependencyPoll>(
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs,
                               {std::move(writer).publish().take_value()})
            .take_value());
  }
};
int fallback_records(const ExecutionBindings& bindings) {
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    unsigned ancestors = 0;
    OperationDefinition ancestor;
    ancestor.key = "ancestor";
    ancestor.traits.input_count = 1;
    ancestor.traits.input_schema.resize(1);
    ancestor.traits.output_element_type = ElementType::Float32;
    ancestor.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
    ancestor.traits.region_rule = mode >= 2 ? OperationRegionRule::Whole
                                            : OperationRegionRule::Elementwise;
    ancestor.callback = [&](const OperationInvocation& call) {
      ++ancestors;
      return Result<Value>(call.inputs[0]);
    };
    if (check(registry->register_operation(ancestor).ok(), "ancestor"))
      return 1;
    auto child = ancestor;
    child.key = "fallback";
    child.callback = {};
    child.traits.region_rule = OperationRegionRule::Dependency;
    child.traits.dependency_version = 1;
    child.traits.supports_gpu = true;
    child.traits.allows_cpu_fallback = true;
    child.traits.continuation_bytes = sizeof(FallbackReads);
    child.traits.maximum_dependency_stages = 4;
    child.start_dependency = [mode](const DependencyQuery&,
                                    const BufferAllocator& allocator) {
      return DependencyContinuation::make<FallbackReads>(
          allocator, mode == 1 || mode == 2);
    };
    if (check(
            registry->register_operation(child).ok() && registry->freeze().ok(),
            "fallback registration"))
      return 1;
    auto doc = document("ancestor", false);
    doc.nodes.push_back({2, "fallback", {WorkflowNodeOutput{1, "value"}}, {}});
    doc.outputs = {{"y", 2, "value"}};
    DemandQuery query{{"y", point(0)}};
    if (mode == 1) {
      doc.outputs.push_back({"a", 1, "value"});
      query.emplace("a", point(0));
    }
    GraphContext graph(doc);
    PlanningOptions planning;
    planning.execution_mode = ExecutionMode::MetalFp32;
    auto plan = Compiler(registry).compile(graph, planning).take_value().plan;
    ExecutionContext context(registry, {1, true, 8, 1 << 20, 0});
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
      if (check(evidence.record_count() == 1 &&
                    evidence.source_support().value().empty(),
                "abandoned GPU ancestor retained"))
        return 1;
      auto tile = plan.tile_plan("y", Region({{0, 1}})).take_value();
      auto ordinary = context.execute(tile, bindings, {}, options);
      if (check(ordinary.ok() &&
                    ordinary.value().dependencies.record_count() == 1,
                "ordinary fallback retained abandoned records"))
        return 1;
    } else if (mode == 1) {
      if (check(evidence.certificate(1).value().coverage() ==
                        point(0).unite(point(2)).take_value() &&
                    ancestors == 3,
                "rollback lost prior rows or retained abandoned rows"))
        return 1;
    } else if (check(
                   ancestors == 1 && evidence.record_count() == 2 &&
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
  op.traits.output_element_type = ElementType::Float32;
  op.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  op.traits.region_rule = OperationRegionRule::Elementwise;
  op.traits.supports_gpu = true;
  op.traits.allows_cpu_fallback = true;
  op.callback = [](const auto& call) { return increment(call, false); };
  if (check(registry->register_operation(op).ok(), "register increment"))
    return 1;
  op.key = "example.whole";
  op.traits.region_rule = OperationRegionRule::Whole;
  op.callback = [](const auto& call) { return increment(call, true); };
  if (check(registry->register_operation(op).ok(), "register Whole"))
    return 1;
  op.key = "example.staged";
  op.traits.region_rule = OperationRegionRule::Dependency;
  op.traits.dependency_version = 1;
  op.callback = {};
  op.traits.continuation_bytes = sizeof(Stage);
  op.traits.maximum_dependency_stages = 4;
  op.traits.workspace_bytes = 4;
  op.start_dependency = [](const DependencyQuery&,
                           const BufferAllocator& allocator) {
    ++starts;
    return DependencyContinuation::make<Stage>(allocator);
  };
  if (check(registry->register_operation(op).ok(), "register staged"))
    return 1;
  op.key = "example.start_rejected";
  op.start_dependency = [](const DependencyQuery& query,
                           const BufferAllocator& allocator) {
    ++starts;
    if (query.backend == Backend::Gpu && unavailable)
      return Result<DependencyContinuation>(
          Status{ErrorCode::BackendUnavailable, "start rejected"});
    return DependencyContinuation::make<Stage>(allocator);
  };
  if (check(registry->register_operation(op).ok() && registry->freeze().ok(),
            "register start rejection"))
    return 1;
  auto memory =
      MutableValue::allocate({ElementType::Float32, {length}},
                             Region::whole({length}), BufferAllocator{})
          .take_value();
  for (std::uint64_t i = 0; i < length; ++i) {
    const float value = static_cast<float>(i);
    std::memcpy(memory.data() + i * 4, &value, 4);
  }
  ExecutionBindings bindings{{{"x", std::move(memory).publish().take_value()}}};
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::MetalFp32;
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
    auto frozen = context.freeze(plan, bindings).take_value();
    const auto q = point(0).unite(point(2)).take_value();
    auto result = context.execute_fragments(frozen, {{"y", q}});
    if (verify(result, 0, 1) || verify(result, 2, 3) ||
        check(result.value().diagnostics.native_dispatch_count ==
                  (enabled ? 2 : 0),
              "dispatch count") ||
        check(result.value().diagnostics.selected_backends.at(1) ==
                  (enabled ? Backend::Gpu : Backend::Cpu),
              "actual backend"))
      return 1;
    if (!enabled) {
      const auto& diagnostics = result.value().diagnostics;
      if (check(diagnostics.operation_timings.size() == 4 &&
                    diagnostics.fallback_reasons.size() == 1,
                "missing-device rejection/CPU attempt pairs"))
        return 1;
      for (unsigned i = 0; i < 4; ++i)
        if (check(
                diagnostics.operation_timings[i].backend ==
                        (i % 2 ? Backend::Cpu : Backend::Gpu) &&
                    diagnostics.operation_timings[i].outcome ==
                        (i % 2 ? ErrorCode::Ok : ErrorCode::BackendUnavailable),
                "missing-device attempt outcome"))
          return 1;
    }
    auto warm = context.execute_fragments(frozen, {{"y", q}});
    if (verify(warm, 2, 3) ||
        check(warm.value().diagnostics.cache_hits == (enabled ? 2 : 0),
              "fallback pixels cached"))
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
      if (verify(fallback, 0, expected) ||
          check(fallback.value().diagnostics.selected_backends.at(1) ==
                    Backend::Cpu,
                "producer fallback backend") ||
          check(fallback.value().diagnostics.selected_backends.at(2) ==
                    (enabled ? Backend::Gpu : Backend::Cpu),
                "descendant backend") ||
          check(context.cache_statistics().retained_bytes == 0,
                "fallback ancestry retained") ||
          check(!staged || starts == before + (enabled ? 2 : 1),
                "continuation restart count"))
        return 1;
      if (enabled) {
        unsigned rejected = 0;
        for (const auto& attempt :
             fallback.value().diagnostics.operation_timings)
          if (attempt.node_id == 1 && attempt.backend == Backend::Gpu &&
              attempt.outcome == ErrorCode::BackendUnavailable)
            ++rejected;
        if (check(rejected == 1, "native rejection attempt missing"))
          return 1;
      }
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
  if (fallback_records(bindings))
    return 1;
  // Whole dense input/output allocations each round independently to 32 KiB.
  GraphContext whole(document("example.whole", false));
  auto plan = Compiler(registry).compile(whole, planning).take_value().plan;
  for (std::uint64_t limit : {65535, 65536}) {
    ExecutionContext context(registry, {1, true, 8, limit, 0});
    auto frozen = context.freeze(plan, bindings).take_value();
    auto result = context.execute_fragments(frozen, {{"y", point(0)}});
    if (limit == 65535) {
      if (check(result.status().code == ErrorCode::ResourceExhausted,
                "one-less admission"))
        return 1;
    } else {
      if (verify(result, 0, 1) ||
          check(result.value().diagnostics.peak_live_bytes == limit,
                "actual native admission"))
        return 1;
      // Release the successful output before admitting another Whole. A GPU
      // rejection must retire both rounded allocations before CPU retry.
      result = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      unavailable = true;
      auto fallback = context.execute_fragments(frozen, {{"y", point(0)}});
      if (verify(fallback, 0, 1) ||
          check(fallback.value().diagnostics.peak_live_bytes == limit,
                "fallback native owner retirement"))
        return 1;
      fallback = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      unavailable = false;
      if (verify(context.execute_fragments(frozen, {{"y", point(0)}}), 0, 1))
        return 1;
    }
  }
  std::cout << "synchronous GPU fragments, Whole/staged fallback, cache "
               "isolation and 65536-byte admission passed\n";
  return 0;
}
