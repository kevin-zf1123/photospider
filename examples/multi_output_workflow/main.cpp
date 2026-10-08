#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, const std::string& reason) {
  if (!condition)
    throw std::runtime_error(reason);
}
template <class T>
T take(Result<T> result) {
  require(result.ok(), result.status().message);
  return result.take_value();
}
const std::array<const char*, 3> names{"full", "left", "right"};
const std::array<std::uint64_t, 3> local_x{3, 0, 1}, source_x{3, 0, 3};
SchemaTemplate image_schema() {
  SchemaTemplate schema;
  schema.id = "photospider.image";
  ResultTensorSpec tensor;
  tensor.key = "pixels";
  tensor.descriptor = {ElementType::Float32, {3, 5, 3}};
  tensor.batch_axes = {1, 1};
  tensor.layout.spatial = true;
  tensor.layout.height_axis = 0;
  tensor.layout.width_axis = 1;
  tensor.layout.channel_axis = 2;
  auto rgb = rgba_semantics();
  rgb.channels.pop_back();
  rgb.association = "none";
  tensor.facets = {take(encode_semantic(rgb))};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
float expected(std::uint64_t row, std::uint64_t column, std::uint64_t channel) {
  return static_cast<float>(((row * 5 + column) * 3 + channel) * 7 % 23) / 22;
}
ResultRef image_source(const ResourceBudget& root,
                       const SchemaTemplate& schema) {
  std::vector<float> pixels;
  for (std::uint64_t y = 0; y < 3; ++y)
    for (std::uint64_t x = 0; x < 5; ++x)
      for (std::uint64_t c = 0; c < 3; ++c)
        pixels.push_back(expected(y, x, c));
  auto builder = take(ResultBuilder::start(root, schema, "split.input"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "source descriptor");
  require(builder
              .publish_tensor(
                  0, Region::whole(schema.tensors[0].sample_shape()),
                  ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                           pixels.size() * 4),
                  take(ResultRelation::cartesian(root, pixels.size(), {})),
                  {true, true, true, true})
              .ok(),
          "source pixels");
  return take(builder.seal());
}
Region pixel(std::uint64_t row, std::uint64_t column) {
  return Region({{0, 1}, {0, 1}, {row, 1}, {column, 1}, {0, 3}});
}
float sample(const ResultRef& result, std::uint64_t y, std::uint64_t x,
             std::uint64_t channel) {
  float number = 0;
  auto status =
      result.read_tensor(take(result.descriptor()), 0, {0, 0, y, x, channel},
                         &number, sizeof(number));
  require(status.ok(), status.message);
  return number;
}
ExecutionResult run(bool joint, bool right_only) {
  auto registry = make_default_operation_registry();
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(registry, config);
  const auto schema = image_schema();
  auto source = image_source(take(execution.resource_budget()), schema);
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "image";
  input.result_schema = std::make_shared<const SchemaTemplate>(schema);
  WorkflowDocument doc;
  doc.inputs = {input};
  doc.nodes = {{1,
                "image.split_horizontal",
                {WorkflowInputReference{1}},
                {{"split_x", std::int64_t{2}}}}};
  PlanningOptions planning;
  planning.tile_height = 1;
  planning.tile_width = 2;
  std::vector<Region> expected_source;
  for (unsigned i = right_only ? 2 : 0; i < 3; ++i) {
    doc.outputs.push_back({names[i], 1, names[i]});
    planning.output_regions.emplace(names[i], pixel(i, local_x[i]));
    expected_source.push_back(pixel(i, source_x[i]));
  }
  GraphContext graph(doc);
  auto compiled = take(Compiler(registry).compile(graph, planning));
  ExecutionBinding binding;
  binding.name = "image";
  binding.result = source;
  ExecutionOptions options;
  options.enable_joint = joint;
  auto result =
      take(execution.execute(compiled.plan, {{binding}}, {}, options));
  require(result.results.size() == (right_only ? 1 : 3),
          "selected output count");
  const auto observations = take(result.dependencies.source_observations());
  unsigned tensor_observations = 0, descriptor_observations = 0;
  const auto support = take(Footprint::from_regions(
      schema.tensors[0].sample_shape(), expected_source));
  for (const auto& observed : observations) {
    require(observed.input == "image", "source identity");
    if (observed.target == ResultSupportTarget::Tensor) {
      require(observed.slot == 0 && observed.roles == 5 &&
                  observed.samples == support,
              "Data and Validation observations equal mapped source pixels");
      ++tensor_observations;
    } else {
      require(observed.target == ResultSupportTarget::Descriptor &&
                  observed.roles == 8,
              "source descriptor observation");
      ++descriptor_observations;
    }
  }
  require(tensor_observations == 1 && descriptor_observations == 1,
          "one tensor and one descriptor observation");
  for (unsigned i = right_only ? 2 : 0; i < 3; ++i) {
    const auto& output = result.results.at(names[i]);
    auto facts = take(output.descriptor());
    const auto shape = output.schema().tensors[0].sample_shape();
    require(shape == std::vector<std::uint64_t>({1, 1, 3,
                                                 i == 0   ? 5U
                                                 : i == 1 ? 2U
                                                          : 3U,
                                                 3}),
            "named output shape");
    require(facts.tensor_coverage(0) ==
                take(Footprint::from_regions(shape, {pixel(i, local_x[i])})),
            "output covers only its requested pixel");
    require(output.association().size() == 1 &&
                output.association()[0] == source.object_id(),
            "output association records the source ObjectId");
  }
  for (const auto& timing : result.diagnostics.operation_timings)
    require(timing.output.node_id == 1 &&
                (!right_only || timing.output.output_index == 2),
            "unselected sibling callbacks are pruned");
  require(!result.diagnostics.operation_timings.empty(),
          "operation callbacks executed");
  std::cout << "split selection=" << (right_only ? "right" : "all")
            << " joint_requested=" << joint
            << " joint_groups=" << result.diagnostics.joint_groups
            << " source_tensor_samples=" << take(support.element_count())
            << '\n';
  return result;
}
void verify(bool joint, bool right_only) {
  // Results and dependency records remain readable after run's context retires.
  auto result = run(joint, right_only);
  for (unsigned i = right_only ? 2 : 0; i < 3; ++i) {
    const auto& output = result.results.at(names[i]);
    for (unsigned c = 0; c < 3; ++c)
      require(sample(output, i, local_x[i], c) == expected(i, source_x[i], c),
              "split source-offset oracle");
    float absent = 0;
    require(output.read_tensor(take(output.descriptor()), 0, {0, 0, 0, 0, 0},
                               &absent, sizeof(absent))
                    .code == ErrorCode::InvalidArgument,
            "unpublished output samples reject unauthorized reads");
  }
  const auto source_shape = image_schema().tensors[0].sample_shape();
  auto edit = take(Footprint::from_regions(
      source_shape, {Region({{0, 1}, {0, 1}, {2, 1}, {3, 1}, {1, 1}})}));
  auto dirty = take(result.dependencies.potential_dirty(
      "image", edit, 1, {}, ResultSupportTarget::Tensor, 0));
  for (const auto& output : result.results) {
    const auto& shape = output.second.schema().tensors[0].sample_shape();
    auto expected_dirty = take(Footprint::from_regions(
        shape, output.first == "right"
                   ? std::vector<Region>{Region(
                         {{0, 1}, {0, 1}, {2, 1}, {1, 1}, {1, 1}})}
                   : std::vector<Region>{}));
    require(dirty.at(output.first) == expected_dirty,
            "source edit maps only to selected right sample");
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    bool joint = true;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "--joint on|off\n";
        return 0;
      }
      require(option == "--joint" && i + 1 < argc, "use --joint on|off");
      const std::string value = argv[++i];
      require(value == "on" || value == "off", "joint value");
      joint = value == "on";
    }
    verify(joint, false);
    verify(joint, true);
    std::cout << "multi-output Result split oracle=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "multi-output failed: " << error.what() << '\n';
    return 1;
  }
}
