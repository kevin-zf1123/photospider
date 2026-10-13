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
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, true, false, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    const unsigned channels = mode == 1 ? 2 : 3;
    const unsigned rows = mode == 2 ? 1 : 2;
    for (unsigned row = 0; row < rows; ++row) {
      std::array<double, 3> values{};
      for (unsigned channel = 0; channel < channels; ++channel)
        check(phase.read_tensor(0, 0, {row, channel}, &values[channel], 8));
      check(builder.publish_tensor(
          0, Region({{row, 1}, {0, channels}}),
          ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                   channels * sizeof(double))));
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
  for (unsigned mode = 0; mode < 3; ++mode) {
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
  for (unsigned mode = 0; mode < 3; ++mode) {
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
    const TileRequest query{true, {}};
    auto result = demand.request({{"y", query}});
    if (mode == 0) {
      PS_REQUIRE_OK(result);
      PS_CHECK(multi_result::number(result.value().results.at("y"), {0, 2}) ==
               1);
    } else {
      PS_CHECK(
          result.status().code ==
          (mode == 1 ? ErrorCode::TypeMismatch : ErrorCode::InvalidArgument));
      PS_CHECK(
          result.status().detail.origin ==
          (mode == 1 ? FailureOrigin::Unspecified : FailureOrigin::Protocol));
      PS_CHECK(result.status().detail.scope == FailureScope::Group);
    }
  }
  return 0;
}
