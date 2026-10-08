#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "../../examples/unified_result_workflow/minimal_ops.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
constexpr unsigned height = 7, width = 13;
using Point = std::pair<unsigned, unsigned>;
using Parameters = std::map<std::string, ParameterValue>;
Parameters parameters(const std::string& boundary) {
  return {{"radius_x", std::int64_t{2}},
          {"radius_y", std::int64_t{1}},
          {"sigma_x", 1.},
          {"sigma_y", 1.},
          {"x_axis", std::int64_t{1}},
          {"y_axis", std::int64_t{0}},
          {"cval", -3.},
          {"boundary", boundary}};
}
// Independent tap-by-tap reflection; no closed support interval calculation.
int extension(int index, int size, const std::string& mode) {
  if (mode == "constant" && (index < 0 || index >= size))
    return -1;
  if (mode == "clamp")
    return std::max(0, std::min(index, size - 1));
  if (mode == "wrap") {
    while (index < 0)
      index += size;
    while (index >= size)
      index -= size;
    return index;
  }
  if (size == 1)
    return 0;
  while (index < 0 || index >= size) {
    if (index < 0)
      index = -index - (mode == "reflect_half" ? 1 : 0);
    if (index >= size)
      index = 2 * size - index - (mode == "reflect_half" ? 1 : 2);
  }
  return index;
}
std::set<Point> needed(unsigned y, unsigned x, unsigned ry, unsigned rx,
                       const std::string& mode) {
  std::set<Point> result;
  for (int j = -static_cast<int>(ry); j <= static_cast<int>(ry); ++j)
    for (int i = -static_cast<int>(rx); i <= static_cast<int>(rx); ++i) {
      const auto yy = extension(static_cast<int>(y) + j, height, mode);
      const auto xx = extension(static_cast<int>(x) + i, width, mode);
      if (yy >= 0 && xx >= 0)
        result.emplace(yy, xx);
    }
  return result;
}
std::set<Point> points(const Footprint& value) {
  std::set<Point> result;
  auto status = value.visit(
      [&](const auto& at) {
        result.emplace(at[0], at[1]);
        return Status::success();
      },
      100000);
  if (!status.ok())
    throw std::runtime_error(status.message);
  return result;
}
WorkflowDocument document(const Value& input, const Parameters& p, bool tiled) {
  WorkflowDocument result;
  numeric_result_fixture::declare_sources(&result, {input});
  result.nodes = {{1,
                   tiled ? "filter.gaussian_baked64_v1_strict_cpu_tiled"
                         : "filter.gaussian_baked64_v1_strict_cpu_whole",
                   {WorkflowInputReference{1}},
                   p}};
  result.outputs = {{"output", 1, "output"}};
  return result;
}
ExecutionOptions options() {
  ExecutionOptions result;
  result.dependencies.maximum_work = UINT64_C(1000000000000);
  result.maximum_dependency_work = UINT64_C(1000000000000);
  return result;
}
struct SourceData {
  Value input;
  std::set<Point> allowed, observed;
  bool restrict = false;
};
struct Source {
  std::shared_ptr<SourceData> data;
  explicit Source(std::shared_ptr<SourceData> source)
      : data(std::move(source)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    auto builder = numeric_result_fixture::take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key));
    auto descriptor =
        ResultRelation::cartesian(phase.resources, 1, {}).take_value();
    auto status = builder.bind_descriptor_relation(descriptor);
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto relation =
        ResultRelation::cartesian(phase.resources, height * width, {})
            .take_value();
    for (const auto& region : phase.query.tensor_outputs->boxes()) {
      const auto count = region.element_count().take_value();
      auto allocated = phase.resources.allocator().allocate(count * 8);
      if (!allocated.ok())
        return Result<ResultProgramPoll>(allocated.status());
      auto output = allocated.take_value();
      auto requested =
          Footprint::from_regions({height, width}, {region}).take_value();
      uint64_t next = 0;
      auto copied = requested.visit(
          [&](const auto& at) {
            const Point point{at[0], at[1]};
            if (data->restrict && !data->allowed.count(point))
              return Status{ErrorCode::Internal,
                            "source read outside exact Gaussian halo"};
            data->observed.insert(point);
            std::memcpy(
                output.data() + next++ * 8,
                data->input.bytes().data() + (at[0] * width + at[1]) * 8, 8);
            return Status::success();
          },
          count, phase.query.cancellation);
      if (!copied.ok())
        return Result<ResultProgramPoll>(copied);
      StridedLayout layout{
          0,
          {static_cast<int64_t>(region.dimensions()[1].extent * 8), 8},
          {region.dimensions()[0].offset, region.dimensions()[1].offset}};
      status =
          builder.publish_tensor(0, region, layout, std::move(output).freeze(),
                                 relation, {true, true, true, true});
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto result = builder.seal();
    return result.ok() ? Result<ResultProgramPoll>(
                             ResultPublication{result.take_value(), true})
                       : Result<ResultProgramPoll>(result.status());
  }
};
std::shared_ptr<OperationRegistry> source_registry(
    const std::shared_ptr<SourceData>& source) {
  auto registry = make_default_operation_registry(false);
  OperationDefinition definition;
  definition.key = "test.gaussian_source";
  definition.traits = unified_example::traits(0, sizeof(Source));
  definition.traits.cacheable = false;
  unified_example::result_output(
      &definition.traits.outputs[0],
      numeric_result_fixture::source_schema(source->input));
  definition.start_result = [source](const auto&, const auto& allocator) {
    return ResultContinuation::make<Source>(allocator, source);
  };
  numeric_result_fixture::require(
      registry->register_operation(std::move(definition)).ok(),
      "register Result source");
  numeric_result_fixture::require(registry->freeze().ok(),
                                  "freeze source registry");
  return registry;
}
WorkflowDocument source_document(const Value& input, const Parameters& p) {
  auto result = document(input, p, true);
  result.inputs.clear();
  result.nodes[0].inputs = {WorkflowNodeOutput{2, "value"}};
  result.nodes.push_back({2, "test.gaussian_source", {}, {}});
  return result;
}
Result<DemandResult> direct(const Value& input, const Parameters& p,
                            const Footprint& q, ResourceLimits limits = {}) {
  auto source = std::make_shared<SourceData>();
  source->input = input;
  auto registry = source_registry(source);
  GraphContext graph(source_document(input, p));
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config;
  config.managed_resources = limits;
  ExecutionContext context(registry, config);
  auto frozen = context.freeze(plan).take_value();
  auto result =
      context.execute_fragments(frozen, {{"output", q}}, {}, options());
  if (!result.ok())
    numeric_result_fixture::require(context.resource_budget()
                                            .value()
                                            .statistics()
                                            .live[ResourceKind::Payload] == 0,
                                    "failed Gaussian run releases payload");
  return result;
}
int check(const std::string& boundary, const Region& roi) {
  auto writer =
      MutableValue::allocate({ElementType::Float64, {height, width}},
                             Region::whole({height, width}), BufferAllocator{})
          .take_value();
  for (unsigned i = 0; i < height * width; ++i) {
    const double value = (static_cast<int>(i * 19 % 83) - 41) / 16.;
    std::memcpy(writer.data() + i * 8, &value, 8);
  }
  const auto input = std::move(writer).publish().take_value();
  const auto p = parameters(boundary);
  std::set<Point> required;
  auto query = Footprint::from_regions({height, width}, {roi}).take_value();
  for (const auto& at : points(query)) {
    auto support = needed(at.first, at.second, 1, 2, boundary);
    required.insert(support.begin(), support.end());
  }
  auto source = std::make_shared<SourceData>();
  source->input = input;
  source->allowed = required;
  source->restrict = true;
  auto registry = source_registry(source);
  ExecutionContextConfig config;
  config.cpu_workers = 4;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  GraphContext whole_graph(document(input, p, false));
  auto whole_plan = Compiler(registry).compile(whole_graph).take_value().plan;
  auto whole =
      context.execute(whole_plan,
                      numeric_result_fixture::bind_sources(
                          context.resource_budget().take_value(), {input}),
                      {}, options());
  PS_CHECK(whole.ok());
  const auto reference =
      numeric_result_fixture::bytes(whole.value().results.at("output"));
  GraphContext graph(source_document(input, p));
  PlanningOptions planning;
  planning.tile_width = planning.tile_height = 2;
  planning.output_regions.emplace("output", roi);
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  unsigned delivered = 0;
  auto seen = Footprint::none({height, width}).take_value();
  auto opts = options();
  opts.result_publication = [&](ValueRef ref, const ResultRef& result) {
    if (ref.node_id != 1)
      return Status::success();
    auto facts = result.descriptor(false).take_value();
    auto added = facts.tensor_coverage(0).subtract(seen).take_value();
    for (const auto& region : added.boxes()) {
      if (region.dimensions()[0].extent > 2 ||
          region.dimensions()[1].extent > 2)
        return Status{ErrorCode::Internal, "unbounded Gaussian publication"};
    }
    auto status = added.visit(
        [&](const auto& at) {
          uint64_t bits = 0;
          auto read = result.read_tensor(facts, 0, at, &bits, 8);
          return read.ok() &&
                         !std::memcmp(
                             &bits,
                             reference.data() + (at[0] * width + at[1]) * 8, 8)
                     ? Status::success()
                     : Status{ErrorCode::Internal,
                              "Gaussian tile differs from Whole"};
        },
        1000);
    seen = facts.tensor_coverage(0);
    ++delivered;
    return status;
  };
  auto streamed = context.execute(compiled.value().plan, {}, {}, opts);
  if (!streamed.ok())
    std::cerr << streamed.status().message << '\n';
  PS_CHECK(streamed.ok() && delivered > 1 && source->observed == required);
  GraphContext bound_graph(document(input, p, true));
  auto bound_plan =
      Compiler(registry).compile(bound_graph, planning).take_value().plan;
  auto collected =
      context.execute(bound_plan,
                      numeric_result_fixture::bind_sources(
                          context.resource_budget().take_value(), {input}),
                      {}, options());
  PS_CHECK(collected.ok());
  auto relation =
      collected.value().results.at("output").tensor_relation(0).take_value();
  for (const auto& at : points(query)) {
    unsigned data_needs = 0;
    auto sample =
        Footprint::from_regions({height, width},
                                {Region({{at.first, 1}, {at.second, 1}})})
            .take_value();
    PS_CHECK(relation
                 .project(sample,
                          [&](ResultSupport support, const Footprint* samples) {
                            if (support.roles & 1) {
                              ++data_needs;
                              if (!samples || points(*samples) !=
                                                  needed(at.first, at.second, 1,
                                                         2, boundary))
                                return Status{
                                    ErrorCode::Internal,
                                    "incorrect Gaussian Data support"};
                            }
                            return Status::success();
                          })
                 .ok());
    PS_CHECK(data_needs == 1);
  }
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      auto dirty =
          Footprint::from_regions({height, width}, {Region({{y, 1}, {x, 1}})})
              .take_value();
      auto affected = collected.value().dependencies.potential_dirty(
          "input0", dirty, 1, {}, ResultSupportTarget::Tensor, 0);
      PS_CHECK(affected.ok());
      std::set<Point> expected;
      for (const auto& at : points(query))
        if (needed(at.first, at.second, 1, 2, boundary).count({y, x}))
          expected.insert(at);
      PS_CHECK(points(affected.value().at("output")) == expected);
    }
  return 0;
}
}  // namespace
int main() {
  for (const auto* boundary :
       {"constant", "clamp", "wrap", "reflect_half", "reflect_whole"}) {
    PS_CHECK(check(boundary, Region({{2, 3}, {3, 4}})) == 0);
    PS_CHECK(check(boundary, Region({{0, 3}, {11, 2}})) == 0);
  }
  std::vector<std::uint8_t> bytes(height * width * 8, 0);
  const std::uint64_t poison = UINT64_C(0x7ff0000000000001);
  std::memcpy(bytes.data() + 5 * 8, &poison, 8);
  auto input =
      Value::create({ElementType::Float64, {height, width}},
                    Region::whole({height, width}), {0, {width * 8, 8}}, bytes)
          .take_value();
  auto p = parameters("constant");
  p["radius_x"] = std::int64_t{5};
  p["radius_y"] = std::int64_t{0};
  p["sigma_x"] = .125;
  p["cval"] = 0.;
  auto q = Footprint::from_regions({height, width}, {Region({{0, 1}, {0, 1}})})
               .take_value();
  auto result = direct(input, p, q);
  PS_CHECK(result.ok());
  auto output = result.value().results.at("output");
  uint64_t value = 1;
  PS_CHECK(output.read_tensor(output.descriptor().value(), 0, {0, 0}, &value, 8)
               .ok() &&
           value == 0);
  unsigned data_needs = 0;
  PS_CHECK(output.tensor_relation(0)
               .value()
               .project(q,
                        [&](ResultSupport source, const Footprint* samples) {
                          if (source.roles & 1) {
                            ++data_needs;
                            if (!samples || points(*samples) !=
                                                needed(0, 0, 0, 4, "constant"))
                              return Status{
                                  ErrorCode::Internal,
                                  "zero coefficient contributed support"};
                          }
                          return Status::success();
                        })
               .ok());
  PS_CHECK(data_needs == 1);
  ResourceLimits work;
  work.maximum_work = 1000;
  PS_CHECK(direct(input, p, q, work).status().code ==
           ErrorCode::ResourceExhausted);
  ResourceLimits low;
  low.capacity[ResourceKind::Payload] = 4096;
  PS_CHECK(direct(input, p, q, low).status().code ==
           ErrorCode::ResourceExhausted);
  low.capacity[ResourceKind::Payload] = 0;
  auto empty =
      direct(input, p, Footprint::none({height, width}).take_value(), low);
  PS_CHECK(empty.ok() && empty.value()
                             .results.at("output")
                             .descriptor()
                             .value()
                             .tensor_coverage(0)
                             .empty());
  return 0;
}
