#include <memory>
#include <utility>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::take;
int c_projection() {
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_SHARED_NEED_FIXTURE).ok());
  PS_CHECK(registry->freeze().ok());
  auto schema = multi_result::schema(ElementType::Float64, {1});
  schema.id = "test.shared";
  WorkflowDocument doc;
  doc.inputs = {multi_result::declaration(1, "input", schema)};
  doc.nodes = {{1, "test.shared", {WorkflowInputReference{1}}, {}}};
  doc.outputs = {{"first", 1, "first"}, {"second", 1, "second"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.result_cache_bytes = 0;
  ExecutionContext execution(registry, config);
  auto root = take(execution.resource_budget());
  auto builder = take(ResultBuilder::start(root, schema, "input"));
  const double value = 42;
  PS_CHECK(builder
               .publish_tensor(0, Region::whole({1}),
                               {reinterpret_cast<const std::uint8_t*>(&value),
                                sizeof(value)})
               .ok());
  ExecutionOptions options;
  options.maximum_parallelism = 1;
  auto result = execution.execute(
      plan.plan, {{{"input", take(builder.seal())}}}, {}, options);
  PS_REQUIRE_OK(result);
  PS_CHECK(multi_result::number(result.value().results.at("first")) == 1);
  PS_CHECK(multi_result::number(result.value().results.at("second")) == 42);
  return 0;
}
struct SharedRequest {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ResultProgramNeed need;
    need.shared_stages = ResourceVector<ResultSharedNeed>(
        ResourceAllocator<ResultSharedNeed>(phase.resources));
    need.shared_stages.push_back(0);
    return Result<ResultProgramPoll>(std::move(need));
  }
};
int cpu_stage_admission() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "test.gpu_stage";
  definition.traits.supports_gpu = true;
  auto schema = multi_result::schema(ElementType::Float64, {1});
  definition.traits.outputs = {multi_result::output("value", schema)};
  OperationSharedStage stage;
  stage.key = "gpu";
  stage.backend = Backend::Gpu;
  stage.result_schema = schema;
  stage.maximum_work = 100000;
  stage.maximum_dependency_stages = 2;
  stage.maximum_output_payload_bytes = 8;
  stage.continuation_bytes = 128;
  definition.traits.shared_stages = {stage};
  unsigned starts = 0;
  definition.start_result = [&](const auto& query, const auto& allocator) {
    if (query.shared_stage != UINT32_MAX)
      ++starts;
    return ResultContinuation::make<SharedRequest>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(definition)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument doc;
  doc.nodes = {{1, "test.gpu_stage", {}, {}}};
  doc.outputs = {{"value", 1, "value"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  ExecutionContext execution(registry, config);
  auto result = execution.execute(plan.plan, {});
  PS_CHECK(!result.ok() &&
           result.status().code == ErrorCode::BackendUnavailable);
  PS_CHECK(result.status().message ==
           "GPU shared stage is unavailable in CPU execution mode");
  PS_CHECK(starts == 0);
  return 0;
}
}  // namespace
int main() {
  return c_projection() || cpu_stage_admission();
}
