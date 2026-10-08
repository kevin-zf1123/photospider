#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "result_support.hpp"  // NOLINT(build/include_subdir)

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Follow {
  std::uint64_t position = 0, atom = 0;
  unsigned stage = 0;
  ResourceVector<std::uint64_t> controls;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using g4_result::check;
    using g4_result::take;
    const auto need = [&](std::uint32_t port) {
      auto samples = take(Footprint::from_regions(
          phase.query.inputs[port].result_schema->tensors[0].sample_shape(),
          {Region({{position, 1}})}));
      ResultProgramNeed batch;
      batch.tensors.push_back(
          {port, 0, std::move(samples), port == 0 ? 2U : 1U});
      return Result<ResultProgramPoll>(std::move(batch));
    };
    check(phase.consume_work(1));
    if (stage == 0) {
      if (!phase.query.tensor_outputs ||
          take(phase.query.tensor_outputs->element_count()) != 1)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::InvalidArgument,
                   "follow requires one requested sample"});
      controls = ResourceVector<std::uint64_t>(
          ResourceAllocator<std::uint64_t>(phase.resources));
      atom = phase.query.tensor_outputs->boxes()[0].dimensions()[0].offset;
      position = atom;
      stage = 1;
      return need(0);
    }
    if (stage == 1) {
      std::int64_t pointer = 0;
      check(phase.read_tensor(0, 0, {position}, &pointer, 8));
      controls.push_back(position);
      if (pointer >= 0) {
        position = static_cast<std::uint64_t>(pointer);
        return need(0);
      }
      position = static_cast<std::uint64_t>(-(pointer + 1));
      stage = 2;
      return need(1);
    }
    double sample = 0;
    check(phase.read_tensor(1, 0, {position}, &sample, 8));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{},
        phase.query.tile_height, phase.query.tile_width,
        phase.query.resources));
    check(builder.bind_descriptor_relation(take(ResultRelation::unite(
        phase.resources,
        {take(ResultRelation::cartesian(
             phase.resources, 1,
             {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0})),
         take(ResultRelation::cartesian(
             phase.resources, 1,
             {1, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))}))));
    auto relation = take(ResultRelation::sample_rows(
        phase.resources,
        take(phase.query.output.result_schema->tensors[0].sample_count()),
        controls.size() + 1, [&](std::uint64_t row) {
          return Result<ResultRelationRow>(ResultRelationRow{
              atom, row < controls.size()
                        ? ResultSupport{0, 2, controls[row], 1,
                                        ResultSupportTarget::Tensor, 0}
                        : ResultSupport{1, 1, position, 1,
                                        ResultSupportTarget::Tensor, 0}});
        }));
    check(builder.publish_tensor(
        0, Region({{atom, 1}}),
        {reinterpret_cast<const std::uint8_t*>(&sample), 8},
        std::move(relation), {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const g4_result::Failure& error) {
    return Result<ResultProgramPoll>(error.status);
  }
};
}  // namespace
void progressive_workflow() {
  using g4_result::check;
  using g4_result::take;
  constexpr std::uint64_t extent = 1000000000;
  const auto control_schema = g4_result::schema(ElementType::Int64, extent);
  const auto payload_schema = g4_result::schema(ElementType::Float64, extent);
  auto registry = std::make_shared<OperationRegistry>();
  std::vector<std::uint64_t> controls, payloads;
  check(registry->register_operation(g4_result::source(
      "example.controls", control_schema,
      [&](const ResultProgramQuery&, const Region& region, std::uint8_t* bytes,
          std::uint64_t size) {
        const auto i = region.dimensions()[0].offset;
        if (region.dimensions()[0].extent != 1 || size != 8 ||
            (i != 0 && i != 1 && i != 3))
          return Status{ErrorCode::OperationFailed,
                        "unexpected control generation"};
        controls.push_back(i);
        const std::int64_t value = i == 0 ? 1 : i == 1 ? 3 : -1000000000;
        std::memcpy(bytes, &value, 8);
        return Status::success();
      })));
  check(registry->register_operation(g4_result::source(
      "example.payload", payload_schema,
      [&](const ResultProgramQuery&, const Region& region, std::uint8_t* bytes,
          std::uint64_t size) {
        const auto i = region.dimensions()[0].offset;
        if (region.dimensions()[0].extent != 1 || size != 8 || i != extent - 1)
          return Status{ErrorCode::OperationFailed,
                        "unexpected payload generation"};
        payloads.push_back(i);
        const double value = 17.25;
        std::memcpy(bytes, &value, 8);
        return Status::success();
      })));
  OperationDefinition follow;
  follow.key = "example.follow";
  follow.traits.input_count = 2;
  follow.traits.input_schema.resize(2);
  for (unsigned i = 0; i < 2; ++i) {
    auto& input = follow.traits.input_schema[i];
    input.kind = OperationPortKind::Result;
    input.element_type = static_cast<std::uint32_t>(
        i == 0 ? ElementType::Int64 : ElementType::Float64);
    input.rank = 1;
  }
  follow.traits.outputs = {
      g4_result::output(payload_schema, sizeof(Follow), 8)};
  follow.start_result = [](const ResultProgramQuery&,
                           const BufferAllocator& allocator) {
    return ResultContinuation::make<Follow>(allocator);
  };
  check(registry->register_operation(std::move(follow)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {1, "example.controls", {}, {}},
      {2, "example.payload", {}, {}},
      {3,
       "example.follow",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}}};
  document.outputs = {{"sample", 3, "value"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.output_regions = {{"sample", Region({{0, 1}})}};
  auto compiled = take(Compiler(registry).compile(graph, options));
  ExecutionContext execution(registry, {1, false, 4, 128});
  auto result = take(execution.execute(compiled.plan));
  const auto peak = take(execution.resource_budget())
                        .statistics()
                        .peak[ResourceKind::Payload];
  if (g4_result::number(result.results.at("sample")) != 17.25 ||
      controls != std::vector<std::uint64_t>({0, 1, 3}) ||
      payloads != std::vector<std::uint64_t>({extent - 1}) || peak > 128)
    throw std::runtime_error("progressive independent oracle failed");
  std::vector<std::uint64_t> control_support, payload_support;
  auto relation = take(result.results.at("sample").tensor_relation(0));
  check(relation.visit(0, 16, [&](ResultSupport support) {
    if (support.target != ResultSupportTarget::Tensor || support.slot != 0 ||
        support.count != 1 || support.input > 1 ||
        support.roles != (support.input == 0 ? 2U : 1U))
      return Status{ErrorCode::OperationFailed,
                    "follow typed support mismatch"};
    (support.input == 0 ? control_support : payload_support)
        .push_back(support.first);
    return Status::success();
  }));
  if (control_support != controls || payload_support != payloads)
    throw std::runtime_error("follow lost historical Control support");
  std::cout
      << "progressive: value=17.25, controls=[0,1,3], payload=[999999999], "
         "generated_bytes=32, peak_payload="
      << peak << ", budget=128\n";
}
