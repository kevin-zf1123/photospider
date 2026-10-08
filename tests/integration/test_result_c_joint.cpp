#include <dlfcn.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/bad_result_table_fixture.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Module {
  explicit Module(const char* path = PS_RESULT_JOINT_FIXTURE)
      : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {}
  void* handle;
  ~Module() {
    if (handle)
      dlclose(handle);
  }
  std::array<std::uint64_t, 6> counts() const {
    auto function = reinterpret_cast<void (*)(std::uint64_t*)>(
        dlsym(handle, "ps_result_joint_counts"));
    std::array<std::uint64_t, 6> result{};
    if (!function)
      throw std::runtime_error("missing C joint counts");
    function(result.data());
    return result;
  }
};
int workflow(unsigned mode, bool disabled = false, bool pruned = false) {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_RESULT_JOINT_FIXTURE));
  check(registry->freeze());
  WorkflowDocument doc;
  doc.inputs = {multi_result::declaration(1, "source")};
  doc.nodes = {{10,
                "fixture.result_joint",
                {WorkflowInputReference{1}},
                {{"mode", static_cast<std::int64_t>(mode)}}}};
  doc.outputs = {{"left", 10, "left"}, {"right", 10, "right"}};
  if (mode == 11)
    doc.outputs = {{"a_right", 10, "right"}, {"z_left", 10, "left"}};
  if (pruned)
    doc.outputs.erase(doc.outputs.begin());
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 4096, 2048});
  auto root = take(context.resource_budget());
  auto binding = multi_result::binding(root, "source", 3);
  auto frozen = take(context.freeze(plan, ExecutionBindings{{binding}}));
  auto point = take(Footprint::all({1}));
  DemandQuery queries;
  for (const auto& output : doc.outputs)
    queries.emplace(output.name, point);
  std::vector<std::uint32_t> delivered;
  ExecutionOptions options;
  options.enable_joint = !disabled;
  options.maximum_dependency_work = mode == 5 ? 2000 : 1000000;
  options.result_publication = [&](ValueRef ref, const ResultRef&) {
    delivered.push_back(ref.output_index);
    return Status::success();
  };
  auto result = context.execute_fragments(frozen, queries, {}, options);
  const auto counts = module.counts();
  if (mode == 0 || mode == 6 || mode == 7) {
    if (!result.ok())
      std::cerr << "C joint mode " << mode << ": " << result.status().message
                << '\n';
    PS_CHECK(result.ok());
    PS_CHECK(multi_result::number(result.value().results.at("right")) == 11);
    if (!pruned)
      PS_CHECK(multi_result::number(result.value().results.at("left")) == 7);
    if (disabled || pruned) {
      PS_CHECK(counts[0] == 0 && counts[1] == 0 &&
               counts[3] == (pruned ? 1 : 2));
    } else if (mode == 0) {
      PS_CHECK(counts[0] == 1 && counts[1] == 1 && counts[2] == 2 &&
               counts[3] == 0);
      PS_CHECK(counts[4] == 1 && counts[5] == 1);
      auto dirty =
          take(result.value().dependencies.potential_dirty("source", point));
      PS_CHECK(dirty.at("left") == point && dirty.at("right").empty());
      auto identical = take(context.freeze(
          plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
      auto warm = take(context.execute_fragments(identical, queries));
      PS_CHECK(warm.diagnostics.cache_hits == 2 && module.counts() == counts);
      auto changed = take(context.freeze(
          plan,
          ExecutionBindings{{multi_result::binding(root, "source", 13)}}));
      auto partial = take(context.execute_fragments(changed, queries));
      PS_CHECK(multi_result::number(partial.results.at("left")) == 17 &&
               multi_result::number(partial.results.at("right")) == 11 &&
               partial.diagnostics.cache_hits == 1);
      auto after = module.counts();
      PS_CHECK(after[0] == 1 && after[1] == 1 && after[3] == 1);
    } else {
      PS_CHECK(counts[0] == 1 && counts[1] == 1 && counts[3] == 2);
      PS_CHECK(result.value().diagnostics.joint_fallbacks == 1);
    }
  } else {
    PS_CHECK(!result.ok() && counts[0] == 1 && counts[1] == 1 &&
             counts[3] == 0);
    if (mode == 5) {
      PS_CHECK(result.status().code == ErrorCode::ResourceExhausted &&
               result.status().detail.scope == FailureScope::Run);
    } else {
      PS_CHECK(result.status().detail.origin == FailureOrigin::Protocol);
    }
    PS_CHECK(delivered.empty());
  }
  PS_CHECK(context.cache_statistics().in_flight == 0 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int different_output_shapes(bool disabled) {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_RESULT_JOINT_FIXTURE));
  check(registry->freeze());
  WorkflowDocument doc;
  doc.inputs = {multi_result::declaration(1, "source")};
  doc.nodes = {{10,
                "fixture.result_joint_roi",
                {WorkflowInputReference{1}},
                {{"mode", std::int64_t{0}}}}};
  doc.outputs = {{"left", 10, "left"}, {"right", 10, "right"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 4096, 2048});
  auto root = take(context.resource_budget());
  auto frozen = take(context.freeze(
      plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
  auto left = take(Footprint::all({1}));
  auto right = take(Footprint::from_regions({2}, {Region({{1, 1}})}));
  ExecutionOptions options;
  options.enable_joint = !disabled;
  auto result = take(context.execute_fragments(
      frozen, {{"left", left}, {"right", right}}, {}, options));
  const auto& output = result.results.at("right");
  auto descriptor = take(output.descriptor());
  PS_CHECK(descriptor.tensor_coverage(0) == right &&
           multi_result::number(result.results.at("left")) == 7);
  double value = 0;
  check(output.read_tensor(descriptor, 0, {1}, &value, sizeof(value)));
  PS_CHECK(value == 11 &&
           !output.read_tensor(descriptor, 0, {0}, &value, sizeof(value)).ok());
  auto dirty = take(result.dependencies.potential_dirty("source", left));
  PS_CHECK(dirty.at("left") == left && dirty.at("right").empty());
  const auto counts = module.counts();
  PS_CHECK(result.diagnostics.joint_groups == (disabled ? 0 : 1) &&
           counts[0] == (disabled ? 0 : 1) && counts[0] == counts[1] &&
           counts[3] == (disabled ? 2 : 0) &&
           context.cache_statistics().in_flight == 0 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int wide_sparse_output() {
  const std::uint64_t extent = UINT64_C(1) << 61;
  ResourceBudget root;
  {
    auto registry = std::make_shared<OperationRegistry>();
    check(registry->load_plugin(PS_RESULT_JOINT_FIXTURE));
    check(registry->freeze());
    WorkflowDocument doc;
    doc.inputs = {multi_result::declaration(1, "source")};
    doc.nodes = {{10,
                  "fixture.result_joint_wide",
                  {WorkflowInputReference{1}},
                  {{"mode", std::int64_t{0}}}}};
    doc.outputs = {{"left", 10, "left"}, {"right", 10, "right"}};
    GraphContext graph(doc);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Payload] = 65536;
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    auto frozen = take(context.freeze(
        plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
    auto left = take(Footprint::all({1}));
    for (const auto coordinate : {std::uint64_t{0}, extent - 1}) {
      auto right =
          take(Footprint::from_regions({extent}, {Region({{coordinate, 1}})}));
      auto result = take(context.execute_fragments(
          frozen, {{"left", left}, {"right", right}}));
      const auto& output = result.results.at("right");
      auto descriptor = take(output.descriptor());
      PS_CHECK(output.schema().tensors[0].descriptor.shape[0] == extent &&
               descriptor.tensor_coverage(0) == right &&
               multi_result::number(output, {coordinate}) == 11 &&
               result.diagnostics.joint_groups == 1);
      double ignored = 0;
      PS_CHECK(!output
                    .read_tensor(descriptor, 0, {coordinate ? 0u : 1u},
                                 &ignored, sizeof(ignored))
                    .ok());
      auto window =
          take(output.acquire_tensor(descriptor, 0, Region({{coordinate, 1}})));
      PS_CHECK(take(window.row_run({coordinate})).bytes == sizeof(double));
    }
    PS_CHECK(root.statistics().peak[ResourceKind::Payload] <= 65536 &&
             context.cache_statistics().in_flight == 0);
  }
  for (auto bytes : root.statistics().live.values)
    PS_CHECK(bytes == 0);
  return 0;
}
int cancelled_start() {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_RESULT_JOINT_FIXTURE));
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  OperationMetadata source;
  source.result_schema =
      std::make_shared<const SchemaTemplate>(multi_result::schema());
  std::vector<OperationMetadata> inputs{source};
  const std::map<std::string, ParameterValue> parameters{
      {"mode", std::int64_t{12}}};
  auto prepared = take(
      registry->prepare_operation("fixture.result_joint", inputs, parameters));
  auto outputs =
      take(infer_operation_outputs(prepared->traits(), inputs, parameters));
  ResultProgramMetadata left{inputs, outputs[0]}, right{inputs, outputs[1]};
  ResourceVector<ResultProgramQuery> queries;
  queries.emplace_back(left, parameters);
  queries.emplace_back(right, parameters);
  CancellationSource cancellation;
  cancellation.cancel();
  for (unsigned i = 0; i < 2; ++i) {
    queries[i].prepared = prepared;
    queries[i].output_index = i;
    queries[i].tensor_outputs = take(Footprint::all({1}));
    queries[i].semantic_key = i ? "right" : "left";
    queries[i].snapshot_identity = "C cancelled start";
  }
  queries[0].cancellation = cancellation.token();
  auto state =
      take(registry->start_result_joint("fixture.result_joint", queries, root));
  auto allocator = root.allocator();
  auto work = [&](std::uint64_t count) { return root.consume({count}); };

  ResultObjectInputs results;
  ResourceVector<ResultIoReply> io;
  ResultProgramPhase phase_left{queries[0], results, io, allocator,
                                root,       work,    {}};
  ResultProgramPhase phase_right{queries[1], results, io, allocator,
                                 root,       work,    {}};
  ResourceVector<const ResultProgramPhase*> ready{&phase_left, &phase_right};
  auto outcomes = take(state.poll({ready, allocator, work}));
  PS_CHECK(outcomes.size() == 2);
  bool cancelled = false, healthy = false;
  for (const auto& outcome : outcomes)
    if (outcome.key.output_index == 0) {
      cancelled = !outcome.outcome.ok() &&
                  outcome.outcome.status().code == ErrorCode::Cancelled;
    } else {
      healthy =
          outcome.outcome.ok() &&
          multi_result::number(
              std::get<ResultPublication>(outcome.outcome.value()).result) ==
              11;
    }
  state = {};
  auto counts = module.counts();
  PS_CHECK(cancelled && healthy && counts[0] == 1 && counts[1] == 1 &&
           counts[2] == 1 && counts[3] == 0);
  return 0;
}

int first_cause(unsigned mode) {
  Module module(PS_RESULT_JOINT_THROW_FIXTURE);
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_RESULT_JOINT_THROW_FIXTURE));
  check(registry->freeze());
  WorkflowDocument doc;
  doc.nodes = {{10,
                "fixture.result_joint_throw",
                {},
                {{"mode", static_cast<std::int64_t>(mode)}}}};
  doc.outputs = {{"left", 10, "left"}, {"right", 10, "right"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 4096, 0});
  auto frozen = take(context.freeze(plan, {}));
  auto point = take(Footprint::all({1}));
  ExecutionOptions options;
  options.maximum_dependency_work = mode == 2 ? 2000 : 1000000;
  auto result = context.execute_fragments(
      frozen, {{"left", point}, {"right", point}}, {}, options);
  auto counts = module.counts();
  PS_CHECK(counts[0] == 1 && counts[1] == 1 &&
           counts[2] == (mode == 5 ? 0 : 1));
  if (mode < 2 || mode == 4 || mode == 6) {
    PS_CHECK(!result.ok() &&
             result.status().detail.origin == FailureOrigin::Protocol &&
             result.status().message ==
                 (mode == 6   ? "invalid C Result joint outcome member"
                  : mode == 4 ? "expired C joint services"
                              : "invalid C joint scratch destination") &&
             counts[3] == 0);
  } else if (mode == 2) {
    PS_CHECK(
        !result.ok() && result.status().code == ErrorCode::ResourceExhausted &&
        result.status().reason == FailureReason::WorkLimit &&
        result.status().detail.scope == FailureScope::Run && counts[3] == 0);
  } else {
    PS_CHECK(result.ok() && counts[3] == 2 &&
             result.value().diagnostics.joint_fallbacks == 1 &&
             multi_result::number(result.value().results.at("left")) == 7 &&
             multi_result::number(result.value().results.at("right")) == 11);
  }
  return 0;
}

}  // namespace
int main() try {
  PS_CHECK(ps::test::check_bad_result_tables(PS_BAD_JOINT_FIXTURE,
                                             {{1, "joint size"},
                                              {2, "joint contract"},
                                              {3, "destroy callback"},
                                              {4, "state bytes"},
                                              {5, "observation kind"},
                                              {6, "operation size"},
                                              {7, "query size"},
                                              {8, "member size"},
                                              {9, "outcome size"},
                                              {10, "services size"}}));
  for (unsigned mode = 0; mode <= 11; ++mode)
    PS_CHECK(workflow(mode) == 0);
  PS_CHECK(workflow(0, true) == 0);
  PS_CHECK(workflow(0, false, true) == 0);
  PS_CHECK(cancelled_start() == 0);
  PS_CHECK(different_output_shapes(false) == 0);
  PS_CHECK(different_output_shapes(true) == 0);
  PS_CHECK(wide_sparse_output() == 0);
  for (unsigned mode = 0; mode < 7; ++mode)
    PS_CHECK(first_cause(mode) == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
