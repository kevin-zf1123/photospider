#include <memory>
#include <optional>
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
      plan.plan.value(), {{{"input", take(builder.seal())}}}, {}, options);
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
  definition.map_dependencies = [](const ResultStaticQuery& query) {
    ResultStaticMapping mapped;
    if (query.shared_stage == UINT32_MAX)
      mapped.shared_stages.push_back(0);
    return Result<ResultStaticMapping>(std::move(mapped));
  };
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
  auto result = Compiler(registry).compile(graph);
  PS_CHECK(!result.ok() &&
           result.status().code == ErrorCode::BackendUnavailable);
  PS_CHECK(starts == 0);
  return 0;
}
struct BundleProducer {
  std::optional<ResultBuilder> builder;
  unsigned member = 0;
  unsigned* polls;
  explicit BundleProducer(unsigned* count) : polls(count) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++*polls;
    if (!builder)
      builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {}, phase.query.tile_extent));
    const double value = member ? 7 : 5;
    multi_result::check(builder->publish_tensor(
        member++, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)}));
    return Result<ResultProgramPoll>(
        ResultPublication{member == 2 ? take(builder->seal())
                                      : take(builder->reference().capture()),
                          member == 2});
  }
};
struct BundleConsumer {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto& shared = phase.shared_stages->at(0);
    const auto facts = take(shared.descriptor());
    double sum = 0;
    for (unsigned member = 0; member < 2; ++member) {
      double value = 0;
      multi_result::check(
          shared.read_tensor(facts, member, {0}, &value, sizeof(value)));
      sum += value;
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    multi_result::check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&sum), sizeof(sum)}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int shared_bundle_validation() {
  auto registry = std::make_shared<OperationRegistry>();
  unsigned validations = 0, polls = 0;
  ValidationDefinition validator;
  validator.identity = {"test.shared_bundle", 1, "complete/1"};
  validator.requires_complete = true;
  validator.prepare = [](const ValidationPreparation&) {
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  validator.validate = [&](const void*, const ValidationContext& context) {
    ++validations;
    if (!context.complete)
      return Status{ErrorCode::OperationFailed, "premature shared validation"};
    for (unsigned member = 0; member < 2; ++member) {
      auto window = context.acquire_tensor(member, Region::whole({1}));
      if (!window.ok())
        return window.status();
    }
    return Status::success();
  };
  multi_result::check(registry->register_validator(std::move(validator)));
  OperationDefinition operation;
  operation.key = "test.shared_bundle";
  operation.traits.outputs = {multi_result::output("value")};
  OperationSharedStage stage;
  stage.key = "bundle";
  stage.result_schema = multi_result::schema();
  auto second = stage.result_schema.tensors[0];
  second.key = "second";
  stage.result_schema.tensors.push_back(second);
  stage.result_schema.publication_scopes = {
      {ResultPublicationPolicy::CompleteBundle, {0, 1}}};
  stage.validators = {{"test.shared_bundle", 1, "complete/1"}};
  stage.continuation_bytes = 4096;
  stage.workspace_bytes = 4096;
  stage.maximum_work = 1000000;
  stage.maximum_output_payload_bytes = 4096;
  stage.maximum_dependency_stages = 8;
  operation.traits.shared_stages = {stage};
  operation.map_dependencies = [&](const ResultStaticQuery& query) {
    ResultStaticMapping result;
    if (query.shared_stage == UINT32_MAX)
      result.shared_stages.push_back(0);
    return Result<ResultStaticMapping>(std::move(result));
  };
  operation.start_result = [&](const auto& query, const auto& allocator) {
    return query.shared_stage == UINT32_MAX
               ? ResultContinuation::make<BundleConsumer>(allocator)
               : ResultContinuation::make<BundleProducer>(allocator, &polls);
  };
  multi_result::check(registry->register_operation(std::move(operation)));
  multi_result::check(registry->freeze());
  WorkflowDocument doc;
  doc.nodes = {{1, "test.shared_bundle", {}, {}}};
  doc.outputs = {{"value", 1, "value"}};
  Compiler compiler(registry);
  GraphContext graph(doc);
  auto plan = take(compiler.compile(graph)).plan.value();
  ExecutionContext execution(registry);
  auto result = execution.execute(plan);
  PS_REQUIRE_OK(result);
  PS_CHECK(multi_result::number(result.value().results.at("value")) == 12);
  PS_CHECK(validations == 1 && polls == 2);
  return 0;
}
}  // namespace
int main() {
  return c_projection() || cpu_stage_admission() || shared_bundle_validation();
}
