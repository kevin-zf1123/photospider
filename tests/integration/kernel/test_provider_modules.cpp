#include <memory>
#include <utility>

#include "photospider/compiler/artifact.hpp"
#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

int main() {
  auto operations = ps::test::kernel_registry();
  ps::GraphContext graph(ps::test::addition_document(4, 5));
  auto catalog = std::make_shared<ps::PlannerRegistry>();
  PS_REQUIRE_OK(catalog->load_plugin(PS_SCALAR_MODULE));
  PS_REQUIRE_OK(catalog->load_plugin(PS_DEFAULT_PLANNER));
  PS_CHECK(!catalog->load_plugin(PS_SCALAR_MODULE).ok());
  catalog->freeze();
  ps::Compiler compiler(operations, {}, catalog);
  auto ir = compiler.analyze(graph.snapshot());
  PS_REQUIRE_OK(ir);
  for (const char* planner : {"scalar", "static-tiles"}) {
    ps::PlanningOptions options;
    options.planner = planner;
    auto plan = compiler.plan(ir.value(), options);
    PS_REQUIRE_OK(plan);
    ps::ExecutorRegistry exec;
    PS_REQUIRE_OK(exec.load_plugin(PS_DEFAULT_EXECUTOR));
    exec.freeze();
    ps::ExecutionContext context(operations, {}, exec, "physical");
    auto r = multi_result::run(context, plan.value());
    PS_REQUIRE_OK(r);
    PS_CHECK(ps::test::named_scalar(r.value(), "sum") == 9);
  }
  // Both default planner assemblies must preserve operation-owned failures
  // from a lowering callback, including its diagnostic domain.
  auto rejecting = ps::test::kernel_registry(false);
  auto broken = ps::test::kernel_identity("fixture.reject-lowering");
  broken.lower = [](const ps::OperationLowering&) {
    ps::Status status{ps::ErrorCode::OperationFailed, "mapping rejected",
                      ps::FailureReason::InvalidDomain};
    status.detail.domain_code = 123;
    return ps::Result<ps::OperationProgram>(std::move(status));
  };
  PS_REQUIRE_OK(rejecting->register_operation(std::move(broken)));
  PS_REQUIRE_OK(rejecting->freeze());
  auto source = ps::test::addition_document(4, 5);
  source.nodes.push_back(
      {4, "fixture.reject-lowering", {ps::WorkflowNodeOutput{3, "value"}}, {}});
  source.outputs = {{"invalid", 4, "value"}};
  ps::GraphContext reject_graph(source);
  for (auto planners : {ps::make_default_planner_registry(),
                        std::shared_ptr<const ps::PlannerRegistry>(catalog)}) {
    auto rejected = ps::Compiler(rejecting, {}, planners).compile(reject_graph);
    PS_CHECK(!rejected.ok() &&
             rejected.status().code == ps::ErrorCode::OperationFailed &&
             rejected.status().reason == ps::FailureReason::InvalidDomain &&
             rejected.status().detail.domain_code == 123);
  }
  auto schema =
      std::make_shared<const ps::SchemaTemplate>(multi_result::schema());
  ps::IRProgram passthrough;
  passthrough.entry = 1;
  passthrough.input_declarations = {{1, "x", schema}};
  passthrough.input_bindings = {{1, 10}};
  passthrough.returns = {{"out", schema}};
  passthrough.blocks = {{1, {}, {}, ps::IRReturn{{10}}}};
  auto bound = compiler.bind_ir(passthrough);
  PS_REQUIRE_OK(bound);
  auto plan = compiler.plan(bound.value());
  PS_REQUIRE_OK(plan);
  ps::ExecutionObservation observation;
  ps::ResultRef retained;
  {
    ps::ExecutorRegistry exec;
    PS_REQUIRE_OK(exec.load_plugin(PS_IDENTITY_MODULE));
    exec.freeze();
    ps::ExecutionContext context(operations, {}, exec, "identity");
    PS_CHECK(context.native_device_description() == "independent");
    auto input = multi_result::binding(context.resource_budget().value(), "x",
                                       42, *schema);
    auto prepared = context.prepare(plan.value());
    PS_REQUIRE_OK(prepared);
    auto wrong = input;
    wrong.name = "wrong";
    PS_CHECK(!context.submit(prepared.value(), {{wrong}}).ok());
    auto run = context.submit(prepared.value(), {{input}});
    PS_REQUIRE_OK(run);
    observation = run.value().observation();
    auto output = run.value().join();
    PS_REQUIRE_OK(output);
    PS_CHECK(output.value().diagnostics.activations == 1);
    retained = output.value().results.at("out");
    auto rejected =
        context.prepare(compiler.compile(graph).value().plan.value());
    PS_CHECK(!rejected.ok());
  }
  PS_CHECK(observation.valid() && observation.snapshot().submitted_tasks == 0);
  PS_CHECK(multi_result::number(retained) == 42);
  // Returning the whole input for a narrowed Plan would expose unselected
  // samples, even though the requested samples are present.
  auto vector_schema = std::make_shared<const ps::SchemaTemplate>(
      multi_result::schema(ps::ElementType::Float64, {2}));
  passthrough.input_declarations[0].result_schema = vector_schema;
  passthrough.returns[0].result_schema = vector_schema;
  auto vector_ir = compiler.bind_ir(passthrough);
  PS_REQUIRE_OK(vector_ir);
  ps::PlanningOptions partial;
  partial.tile_extent = 1;
  partial.output_requests["out"].members[0] =
      multi_result::take(ps::TileSet::range(ps::ResourceBudget{}, 0, 1));
  auto partial_plan = compiler.plan(vector_ir.value(), partial);
  PS_REQUIRE_OK(partial_plan);
  ps::ExecutorRegistry partial_registry;
  PS_REQUIRE_OK(partial_registry.load_plugin(PS_IDENTITY_MODULE));
  partial_registry.freeze();
  ps::ExecutionContext forwarding(operations, {}, partial_registry, "identity");
  auto partial_prepared = forwarding.prepare(partial_plan.value());
  PS_REQUIRE_OK(partial_prepared);
  auto root = forwarding.resource_budget().take_value();
  auto bytes =
      multi_result::take(root.allocator().allocate(2 * sizeof(double)));
  auto source_builder = multi_result::take(
      ps::ResultBuilder::start(root, *vector_schema, "input", {}, 1));
  ps::StridedLayout layout;
  layout.byte_strides = {sizeof(double)};
  layout.origin = {0};
  PS_REQUIRE_OK(source_builder.publish_tensor(0, ps::Region::whole({2}), layout,
                                              std::move(bytes).freeze()));
  ps::ExecutionBinding input{"x", multi_result::take(source_builder.seal())};
  auto partial_run = forwarding.submit(partial_prepared.value(), {{input}});
  PS_REQUIRE_OK(partial_run);
  auto extra_coverage = partial_run.value().join();
  PS_CHECK(!extra_coverage.ok() &&
           extra_coverage.status().code == ps::ErrorCode::InvalidArgument);
  ps::OperationRegistry ops;
  PS_CHECK(ops.load_plugin(PS_OBSOLETE_MODULE).code ==
           ps::ErrorCode::TypeMismatch);
  PS_REQUIRE_OK(ops.load_plugin(PS_GEOMETRY_MODULE));
  PS_CHECK(ops.has_mapper("halo", "geometry") &&
           ops.has_mapper("plane", "geometry") && ops.keys().empty());
  PS_CHECK(!ops.load_plugin(PS_GEOMETRY_MODULE).ok());
  PS_REQUIRE_OK(ops.freeze());
  return 0;
}
