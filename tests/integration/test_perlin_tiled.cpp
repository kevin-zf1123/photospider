#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/perlin_workflow/result_fixture.hpp"
#include "../../examples/unified_result_workflow/minimal_ops.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
constexpr unsigned height = 17, width = 35;
struct SourceData {
  Value value;
  std::vector<Region> reads;
  std::uint64_t bytes = 0;
};
struct Source {
  std::shared_ptr<SourceData> data;
  explicit Source(std::shared_ptr<SourceData> input) : data(std::move(input)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    auto builder = numeric_result_fixture::take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key));
    auto status = builder.bind_descriptor_relation(
        ResultRelation::cartesian(phase.resources, 1, {}).take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    for (const auto& region : phase.query.tensor_outputs->boxes()) {
      data->reads.push_back(region);
      const auto sample_width =
          Value::element_size(data->value.descriptor().element_type);
      const auto count = region.element_count().take_value();
      auto allocated =
          phase.resources.allocator().allocate(count * sample_width);
      if (!allocated.ok())
        return Result<ResultProgramPoll>(allocated.status());
      auto output = allocated.take_value();
      auto covered =
          Footprint::from_regions(data->value.descriptor().shape, {region})
              .take_value();
      std::uint64_t offset = 0;
      auto copied = covered.visit(
          [&](const auto& at) {
            const auto index = (at[0] * width + at[1]) * 3 + at[2];
            std::memcpy(output.data() + offset,
                        data->value.bytes().data() + index * sample_width,
                        sample_width);
            offset += sample_width;
            return Status::success();
          },
          count, phase.query.cancellation);
      if (!copied.ok())
        return Result<ResultProgramPoll>(copied);
      data->bytes += offset;
      StridedLayout layout;
      layout.origin.resize(region.rank());
      layout.byte_strides.resize(region.rank());
      std::int64_t stride = sample_width;
      for (std::size_t axis = region.rank(); axis-- > 0;) {
        layout.origin[axis] = region.dimensions()[axis].offset;
        layout.byte_strides[axis] = stride;
        stride *= region.dimensions()[axis].extent;
      }
      status = builder.publish_tensor(
          0, region, layout, std::move(output).freeze(),
          ResultRelation::cartesian(
              phase.resources, schema.tensors[0].sample_count().take_value(),
              {})
              .take_value(),
          {true, true, true, true});
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Result<ResultProgramPoll>(
                             ResultPublication{sealed.take_value(), true})
                       : Result<ResultProgramPoll>(sealed.status());
  }
};
std::shared_ptr<OperationRegistry> registry(
    const std::shared_ptr<SourceData>& data) {
  auto operations = make_default_operation_registry(false);
  OperationDefinition source;
  source.key = "test.coordinates";
  source.traits = unified_example::traits(0, sizeof(Source));
  source.traits.cacheable = false;
  unified_example::result_output(
      &source.traits.outputs[0],
      numeric_result_fixture::source_schema(data->value));
  source.start_result = [data](const auto&, const auto& allocator) {
    return ResultContinuation::make<Source>(allocator, data);
  };
  numeric_result_fixture::require(
      operations->register_operation(std::move(source)).ok(),
      "source registration");
  numeric_result_fixture::require(operations->freeze().ok(), "registry freeze");
  return operations;
}
WorkflowDocument document() {
  WorkflowDocument doc;
  doc.nodes = {{1, "test.coordinates", {}, {}},
               {2,
                "noise.perlin2002_3d_v1_strict_cpu_tiled",
                {WorkflowNodeOutput{1, "value"}},
                {}}};
  doc.outputs = {{"values", 2, "values"}};
  return doc;
}
}  // namespace
int main() {
  std::vector<double> samples(height * width * 3);
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      samples[(y * width + x) * 3] =
          static_cast<double>((y * 31 + x * 17) % 257) / 64. - 2.;
      samples[(y * width + x) * 3 + 1] = .25 + y / 32.;
      samples[(y * width + x) * 3 + 2] = -.125;
    }
  std::vector<std::uint8_t> bytes(samples.size() * 8);
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  const ValueDescriptor descriptor{ElementType::Float64, {height, width, 3}};
  auto input = Value::create(descriptor, Region::whole(descriptor.shape),
                             {0, {width * 24, 24, 8}}, bytes)
                   .take_value();
  auto data = std::make_shared<SourceData>();
  data->value = input;
  auto operations = registry(data);
  ExecutionContextConfig config;
  config.cpu_workers = 4;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(operations, config);
  WorkflowDocument whole_doc;
  perlin_fixture::declare(&whole_doc, input);
  whole_doc.nodes = {{1,
                      "noise.perlin2002_3d_v1_strict_cpu_whole",
                      {WorkflowInputReference{1}},
                      {}}};
  whole_doc.outputs = {{"values", 1, "values"}};
  GraphContext whole_graph(whole_doc);
  auto whole_plan = Compiler(operations).compile(whole_graph).take_value().plan;
  auto whole = context.execute(
      whole_plan,
      perlin_fixture::bind(context.resource_budget().take_value(), input));
  PS_CHECK(whole.ok());
  const auto reference =
      numeric_result_fixture::bytes(whole.value().results.at("values"));
  const std::uint64_t nan = UINT64_C(0x7ff0000000000001);
  std::memcpy(bytes.data(), &nan, 8);
  data->value = Value::create(descriptor, Region::whole(descriptor.shape),
                              {0, {width * 24, 24, 8}}, bytes)
                    .take_value();
  GraphContext graph(document());
  PlanningOptions planning;
  planning.tile_width = 8;
  planning.tile_height = 4;
  const Region roi({{3, 11}, {5, 27}});
  planning.output_regions.emplace("values", roi);
  auto compiled = Compiler(operations).compile(graph);
  PS_CHECK(compiled.ok());
  auto plan = Compiler(operations).compile(graph, planning).take_value().plan;
  unsigned deliveries = 0;
  std::uint64_t computed = 0;
  auto seen = Footprint::none({height, width}).take_value();
  ExecutionOptions options;
  options.result_publication = [&](ValueRef ref, const ResultRef& object) {
    if (ref.node_id != 2)
      return Status::success();
    const auto facts = object.descriptor(false).take_value();
    auto added = facts.tensor_coverage(0).subtract(seen).take_value();
    auto check = added.visit(
        [&](const auto& at) {
          std::uint64_t bits = 0;
          auto status = object.read_tensor(facts, 0, at, &bits, 8);
          if (!status.ok())
            return status;
          return std::memcmp(&bits,
                             reference.data() + (at[0] * width + at[1]) * 8, 8)
                     ? Status{ErrorCode::Internal, "tile differs from Whole"}
                     : Status::success();
        },
        1024);
    if (!check.ok())
      return check;
    for (const auto& box : added.boxes())
      if (box.dimensions()[0].extent > 4 || box.dimensions()[1].extent > 8)
        return Status{ErrorCode::Internal, "unbounded Result tile"};
    computed += added.element_count().take_value();
    seen = facts.tensor_coverage(0);
    ++deliveries;
    return Status::success();
  };
  auto streamed = context.execute(plan, {}, {}, options);
  if (!streamed.ok())
    std::cerr << streamed.status().message << '\n';
  PS_CHECK(streamed.ok() && deliveries == 12 && computed == 297);
  PS_CHECK(data->reads.size() == 12 && data->bytes == 7128);
  for (const auto& read : data->reads) {
    const auto& d = read.dimensions();
    PS_CHECK(d[0].offset >= 3 && d[0].offset + d[0].extent <= 14 &&
             d[0].extent <= 4);
    PS_CHECK(d[1].offset >= 5 && d[1].offset + d[1].extent <= 32 &&
             d[1].extent <= 8);
    PS_CHECK(d[2].offset == 0 && d[2].extent == 3);
  }
  data->reads.clear();
  data->bytes = 0;
  auto collected = context.execute(plan);
  PS_CHECK(collected.ok() && data->reads.size() == 12 && data->bytes == 7128);
  PS_CHECK(
      numeric_result_fixture::bytes(collected.value().results.at("values")) ==
      numeric_result_fixture::bytes(streamed.value().results.at("values")));
  auto frozen = context.freeze(plan).take_value();
  data->reads.clear();
  auto empty = context.execute_fragments(
      frozen, {{"values", Footprint::none({height, width}).take_value()}});
  PS_CHECK(empty.ok() && data->reads.empty() &&
           empty.value()
               .results.at("values")
               .descriptor()
               .value()
               .tensor_coverage(0)
               .empty());
  const auto requested =
      Footprint::from_regions(
          {height, width}, {Region({{2, 3}, {2, 2}}), Region({{2, 3}, {8, 3}})})
          .take_value();
  const auto run = [&](const Value& source, bool low_work, bool low_capacity) {
    auto inputs = std::make_shared<SourceData>();
    inputs->value = source;
    auto ops = registry(inputs);
    auto cfg = config;
    if (low_work)
      cfg.managed_resources->maximum_work = 1000;
    if (low_capacity)
      cfg.managed_resources->capacity[ResourceKind::Payload] = 1079;
    ExecutionContext execution(ops, cfg);
    GraphContext source_graph(document());
    auto p = Compiler(ops).compile(source_graph).take_value().plan;
    auto capture = execution.freeze(p).take_value();
    auto result = execution.execute_fragments(capture, {{"values", requested}});
    if (!result.ok())
      numeric_result_fixture::require(
          execution.resource_budget()
                  .value()
                  .statistics()
                  .live[ResourceKind::Payload] == 0,
          "failed Result execution released payload");
    return result;
  };
  std::vector<uint8_t> narrow_bytes(samples.size() * 4);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const float value = samples[i];
    std::memcpy(narrow_bytes.data() + i * 4, &value, 4);
  }
  const ValueDescriptor f32{ElementType::Float32, descriptor.shape};
  auto narrow = Value::create(f32, Region::whole(f32.shape),
                              {0, {width * 12, 12, 4}}, narrow_bytes)
                    .take_value();
  auto boxes = run(narrow, false, false);
  PS_CHECK(boxes.ok());
  const auto& output = boxes.value().results.at("values");
  PS_CHECK(requested
               .visit(
                   [&](const auto& at) {
                     std::uint64_t actual = 0;
                     auto status = output.read_tensor(
                         output.descriptor().take_value(), 0, at, &actual, 8);
                     return status.ok() && !std::memcmp(
                                               &actual,
                                               reference.data() +
                                                   (at[0] * width + at[1]) * 8,
                                               8)
                                ? Status::success()
                                : Status{ErrorCode::Internal,
                                         "multi-box result"};
                   },
                   15)
               .ok());
  PS_CHECK(run(narrow, true, false).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(run(narrow, false, true).status().code ==
           ErrorCode::ResourceExhausted);
  const std::uint32_t narrow_nan = UINT32_C(0x7f800001);
  std::memcpy(narrow_bytes.data() + (2 * width + 8) * 12, &narrow_nan, 4);
  auto poisoned = Value::create(f32, Region::whole(f32.shape),
                                {0, {width * 12, 12, 4}}, narrow_bytes)
                      .take_value();
  PS_CHECK(run(poisoned, false, false).status().code ==
           ErrorCode::InvalidArgument);
  return 0;
}
