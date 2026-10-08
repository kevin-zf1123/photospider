#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
struct TupleProgram {
  unsigned mode;
  bool requested = false;
  explicit TupleProgram(unsigned value) : mode(value) {}
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    using namespace ps;  // NOLINT(build/namespaces)
    using multi_result::check;
    using multi_result::take;
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, take(Footprint::all(shape)), 13});
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.bind_descriptor_relation(take(ResultRelation::cartesian(
        phase.resources, 1,
        {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))));
    for (const auto& box : phase.query.tensor_outputs->boxes()) {
      const std::vector<ResultMappedAxis> data{{0, 0, 1, 1}, {1, 0, 1, 1}};
      auto witness = take(
          ResultRelation::mapped(phase.resources, shape, box, shape, data,
                                 {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
      if (mode) {
        auto validation = data;
        validation[1] = {-1, 0, 0, mode == 2 ? 2U : 3U};
        if (mode == 1)
          validation[0] = {-1, 1, 0, 1};
        auto tuples = take(ResultRelation::mapped(
            phase.resources, shape, box, shape, validation,
            {0, 4, 0, 0, ResultSupportTarget::Tensor, 0}));
        witness =
            take(ResultRelation::unite(phase.resources, {witness, tuples}));
      }
      std::array<double, 3> values{};
      for (unsigned channel = 0; channel < 3; ++channel)
        check(phase.read_tensor(0, 0, {0, channel}, &values[channel], 8));
      check(builder.publish_tensor(
          0, box,
          ByteView(reinterpret_cast<const std::uint8_t*>(values.data()), 24),
          std::move(witness), {true, true, true, true}));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return ps::Result<ps::ResultProgramPoll>(failure.status);
  }
};
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto schema = multi_result::schema(ElementType::Float64, {2, 3});
  schema.tensors[0].facets = {
      encode_color_array(ColorArrayDescriptor{}).take_value()};
  PS_REQUIRE_OK(schema.validate());
  auto registry = std::make_shared<OperationRegistry>();
  for (unsigned mode = 0; mode < 4; ++mode) {
    OperationDefinition definition;
    definition.key = "test.tuple." + std::to_string(mode);
    definition.traits.input_count = 1;
    definition.traits.input_schema.resize(1);
    definition.traits.input_schema[0].kind = OperationPortKind::Result;
    definition.traits.input_schema[0].result_schema_id = std::string(schema.id);
    definition.traits.input_schema[0].result_schema_version = schema.version;
    definition.traits.outputs = {multi_result::output("value", schema)};
    definition.traits.outputs[0].continuation_bytes = sizeof(TupleProgram);
    definition.start_result = [mode](const ResultProgramQuery&,
                                     const BufferAllocator& allocator) {
      return ResultContinuation::make<TupleProgram>(allocator, mode);
    };
    PS_REQUIRE_OK(registry->register_operation(std::move(definition)));
  }
  PS_REQUIRE_OK(registry->freeze());
  ExecutionContext context(registry);
  auto root = context.resource_budget().take_value();
  auto input = multi_result::binding(root, "x", 1, schema);
  for (unsigned mode = 0; mode < 4; ++mode) {
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "x", schema)};
    document.nodes = {{1,
                       "test.tuple." + std::to_string(mode),
                       {WorkflowInputReference{1}},
                       {}}};
    document.outputs = {{"y", 1, "value"}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph);
    PS_REQUIRE_OK(plan);
    auto demand =
        context.open_demand(plan.value().plan, {{input}}).take_value();
    auto query = Footprint::from_regions({2, 3}, {Region({{0, 1}, {0, 3}})})
                     .take_value();
    auto result = demand.request({{"y", query}});
    if (mode == 3) {
      PS_REQUIRE_OK(result);
      PS_CHECK(multi_result::number(result.value().results.at("y"), {0, 2}) ==
               1);
    } else {
      PS_CHECK(result.status().code == ErrorCode::InvalidArgument);
      PS_CHECK(result.status().detail.origin == FailureOrigin::Protocol);
      PS_CHECK(result.status().detail.scope == FailureScope::Group);
    }
  }
  return 0;
}
