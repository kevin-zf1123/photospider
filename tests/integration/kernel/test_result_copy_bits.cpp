#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "support/kernel_registry_fixture.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
int copy_bits(ElementType type, unsigned profile) {
  const auto width = TensorSpan::element_size(type);
  const std::uint64_t offset = profile == 1 ? 2 * width : profile == 3 ? 1 : 0;
  const auto stride = profile == 1   ? -static_cast<std::int64_t>(width)
                      : profile == 2 ? 0
                                     : static_cast<std::int64_t>(width);
  const auto bytes = profile == 2 ? width : 3 * width + (profile == 3);
  std::vector<std::uint8_t> storage(bytes), expected(3 * width);
  const std::array<std::uint64_t, 3> words{
      UINT64_C(0),
      width == 8   ? UINT64_C(0x8000000000000000)
      : width == 4 ? UINT64_C(0x80000000)
                   : 128,
      width == 8   ? UINT64_C(0x7ff8000000000042)
      : width == 4 ? UINT64_C(0x7fc00042)
                   : 255};
  for (unsigned i = 0; i < 3; ++i) {
    const auto source = profile == 1   ? (2 - i) * width
                        : profile == 2 ? 0
                                       : i * width + (profile == 3);
    const auto& word = words[profile == 2 ? 2 : i];
    std::memcpy(storage.data() + source, &word, width);
    std::memcpy(expected.data() + i * width, &word, width);
  }
  auto schema = multi_result::schema(type, {3});
  schema.tensors[0].atomic_trailing_axes = 1;
  ResourceBudget source_root;
  auto source_builder = take(ResultBuilder::start(source_root, schema, "bits"));
  auto backing = take(source_root.allocator().allocate(bytes));
  std::memcpy(backing.data(), storage.data(), bytes);
  check(source_builder.publish_tensor(0, Region::whole({3}), {offset, {stride}},
                                      std::move(backing).freeze()));
  auto source = take(source_builder.seal());
  source_builder = {};
  auto registry = ps::test::kernel_registry();
  auto planners = std::make_shared<PlannerRegistry>();
  const auto defaults = make_default_planner_registry();
  const auto lower = defaults->find("static-tiles")->plan;
  check(planners->add({"source-layout", "1", "Explicit affine input layout",
                       [=](const ValidatedIR& ir, PlanBuilder& builder,
                           const PlanningOptions& options) {
                         auto status = lower(ir, builder, options);
                         if (!status.ok())
                           return status;
                         auto& program = builder.program();
                         for (auto input : program.inputs)
                           for (auto ti : program.values[input].tiles) {
                             auto& tile = program.tiles[ti];
                             auto layout = program.layouts[tile.layout];
                             layout.strides[0] = stride;
                             layout.byte_offset = offset;
                             tile.layout = static_cast<physical::Id>(
                                 program.layouts.size());
                             program.layouts.push_back(layout);
                             tile.bytes = bytes;
                             for (auto& task : program.tasks)
                               for (auto& read : task.reads)
                                 if (read.tile == ti)
                                   read.layout = tile.layout;
                           }
                         return physical::finalize_program(&program);
                       }}));
  planners->freeze();
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", schema)};
  document.nodes = {{1, "core.identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"output", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.planner = "source-layout";
  auto plan = take(Compiler(registry, {}, planners).compile(graph, options))
                  .plan.value();
  auto encoded = take(CompilerArtifact::export_plan(plan));
  plan = take(CompilerArtifact::import_plan(registry, encoded));
  ResultRef retained;
  ResourceBudget destination_root;
  {
    ExecutionContext context(registry);
    destination_root = take(context.resource_budget());
    auto prepared = take(context.prepare(plan));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      auto submitted = take(context.submit(prepared, {{{"input", source}}}));
      auto result = take(submitted.join());
      const auto& output = result.results.at("output");
      PS_CHECK(output.schema().same_schema(schema));
      PS_CHECK(output.object_id() != source.object_id());
      auto input = take(source.acquire_tensor(take(source.descriptor()), 0,
                                              Region::whole({3})));
      auto copied = take(output.acquire_tensor(take(output.descriptor()), 0,
                                               Region::whole({3})));
      PS_CHECK(take(input.row_run({0})).data != take(copied.row_run({0})).data);
      if (retained.valid())
        PS_CHECK(retained.object_id() != output.object_id());
      retained = output;
    }
  }
  source = {};
  PS_CHECK(source_root.statistics().live[ResourceKind::Payload] == 0);
  auto facts = take(retained.descriptor());
  std::vector<std::uint8_t> observed(3 * width);
  for (unsigned i = 0; i < 3; ++i)
    check(retained.read_tensor(facts, 0, {i}, observed.data() + i * width,
                               width));
  PS_CHECK(observed == expected);
  PS_CHECK(destination_root.statistics().live[ResourceKind::Payload] ==
           3 * width);
  retained = {};
  PS_CHECK(destination_root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
}  // namespace
int main() {
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64})
    for (unsigned profile = 0; profile < 4; ++profile)
      PS_CHECK(copy_bits(type, profile) == 0);
  return 0;
}
