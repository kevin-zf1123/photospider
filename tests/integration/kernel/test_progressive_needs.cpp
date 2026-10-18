#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "support/dependency_workflow_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Follow {
  std::uint64_t position = 0, atom = 0;
  unsigned stage = 0;
  ResourceVector<std::uint64_t> controls;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using dependency_fixture::check;
    using dependency_fixture::take;
    const auto need = [&](std::uint32_t port) {
      ResultProgramNeed batch;
      batch.tiles.push_back(
          {port, 0, false,
           take(TileSet::from_indices(phase.resources, {position}))});
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
      atom = phase.query.tensor_outputs->boxes()[0].dimensions()[1].offset;
      position = atom;
      stage = 1;
      return need(0);
    }
    if (stage == 1) {
      std::int64_t pointer = 0;
      check(phase.read_tensor(0, 0, {0, position}, &pointer, 8));
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
    check(phase.read_tensor(1, 0, {0, position}, &sample, 8));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    auto storage = take(phase.resources.allocator().allocate(sizeof(sample)));
    std::memcpy(storage.data(), &sample, sizeof(sample));
    check(builder.publish_tensor(0, Region({{0, 1}, {atom, 1}}),
                                 {0, {8, 8}, {0, atom}},
                                 std::move(storage).freeze()));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const dependency_fixture::Failure& error) {
    return Result<ResultProgramPoll>(error.status);
  }
};
}  // namespace
void progressive_workflow() {
  using dependency_fixture::check;
  using dependency_fixture::take;
  constexpr std::uint64_t extent = 1000000000;
  auto control_schema = dependency_fixture::schema(ElementType::Int64, extent);
  auto payload_schema =
      dependency_fixture::schema(ElementType::Float64, extent);
  for (auto* schema : {&control_schema, &payload_schema}) {
    auto& tensor = schema->tensors[0];
    tensor.descriptor.shape = {1, extent};
    tensor.layout.spatial = true;
    tensor.layout.channel_axis.reset();
  }
  auto registry = std::make_shared<OperationRegistry>();
  std::vector<std::uint64_t> controls, payloads;
  bool outside_grant = false;
  check(registry->register_operation(dependency_fixture::source(
      "example.controls", control_schema,
      [&](const ResultProgramQuery&, const Region& region, std::uint8_t* bytes,
          std::uint64_t size) {
        const auto i = region.dimensions()[1].offset;
        if (region.dimensions()[1].extent != 1 || size != 8 ||
            (i != 0 && i != 1 && i != 3))
          return Status{ErrorCode::OperationFailed,
                        "unexpected control generation"};
        controls.push_back(i);
        const std::int64_t value = i == 0   ? (outside_grant ? 2 : 1)
                                   : i == 1 ? 3
                                            : -1000000000;
        std::memcpy(bytes, &value, 8);
        return Status::success();
      })));
  check(registry->register_operation(dependency_fixture::source(
      "example.payload", payload_schema,
      [&](const ResultProgramQuery&, const Region& region, std::uint8_t* bytes,
          std::uint64_t size) {
        const auto i = region.dimensions()[1].offset;
        if (region.dimensions()[1].extent != 1 || size != 8 || i != extent - 1)
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

    input.element_type = static_cast<std::uint32_t>(
        i == 0 ? ElementType::Int64 : ElementType::Float64);
    input.rank = 2;
  }
  follow.traits.outputs = {
      dependency_fixture::output(payload_schema, sizeof(Follow), 8)};
  unsigned mappings = 0;
  follow.map_dependencies = [&](const ResultStaticQuery&) {
    ++mappings;
    ResultStaticMapping mapping;
    mapping.inputs = {
        {0, 0, false, take(TileSet::from_indices(ResourceBudget{}, {0, 1, 3}))},
        {1, 0, false,
         take(TileSet::from_indices(ResourceBudget{}, {extent - 1}))}};
    return Result<ResultStaticMapping>(std::move(mapping));
  };
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
  ResourceBudget requests;
  options.tile_extent = 1;
  options.output_requests = {
      {"sample", {{{0, take(TileSet::from_indices(requests, {0}))}}}}};
  auto compiled = take(Compiler(registry).compile(graph, options));
  const auto static_mappings = mappings;
  ExecutionContext execution(registry, {1, false, 4, 128});
  auto result = take(execution.execute(compiled.plan.value()));
  const auto peak = take(execution.resource_budget())
                        .statistics()
                        .peak[ResourceKind::Payload];
  double value = 0;
  const auto& output = result.results.at("sample");
  check(output.read_tensor(take(output.descriptor()), 0, {0, 0}, &value, 8));
  if (value != 17.25 || controls != std::vector<std::uint64_t>({0, 1, 3}) ||
      payloads != std::vector<std::uint64_t>({extent - 1}) || peak > 128)
    throw std::runtime_error("progressive independent oracle failed");
  std::size_t source_instructions = 0, follow_instructions = 0;
  const auto& plan = compiled.plan.value();
  for (const auto& block : plan.blocks())
    for (const auto& instruction : block.instructions) {
      if (plan.steps()[instruction.step_index].node_id != 3) {
        ++source_instructions;
        continue;
      }
      ++follow_instructions;
      if (instruction.shared_stage != UINT32_MAX ||
          instruction.tensor_slot != 0 || instruction.tile_unit != 0 ||
          instruction.inputs.size() != 2 || !instruction.shared_inputs.empty())
        throw std::runtime_error("unexpected follow instruction");
      bool seen[2] = {};
      for (const auto& read : instruction.inputs) {
        const auto& expected = read.input == 0 ? controls : payloads;
        if (read.input > 1 || seen[read.input] || read.tensor_slot != 0 ||
            read.descriptor_only ||
            read.tiles != take(TileSet::from_indices(requests, expected)))
          throw std::runtime_error("follow lost fixed static grants");
        seen[read.input] = true;
      }
    }
  if (source_instructions != 4 || follow_instructions != 1)
    throw std::runtime_error("progressive plan expanded unexpected work");
  // Runtime records project external input dependencies. These generators
  // have no external inputs; internal read capabilities belong to the Plan.
  auto record = take(result.dependencies.tile_record({2, 0, 0}));
  if (!record || !record->inputs.empty())
    throw std::runtime_error("unexpected external dependency record");
  if (mappings != static_mappings)
    throw std::runtime_error("execution invoked the static mapper");
  result = {};
  outside_grant = true;
  execution.clear_result_cache();
  auto refused = execution.execute(compiled.plan.value());
  if (refused.ok() ||
      refused.status().detail.origin != FailureOrigin::Protocol ||
      mappings != static_mappings)
    throw std::runtime_error("progressive Need escaped the compiled grants");
  std::cout
      << "progressive: value=17.25, controls=[0,1,3], payload=[999999999], "
         "generated_bytes=32, peak_payload="
      << peak << ", budget=128\n";
}

int main() try {
  progressive_workflow();
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
