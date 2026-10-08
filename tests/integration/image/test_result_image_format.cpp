#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_image_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
void image_schema_contracts() {
  Driver d;
  const auto control = scalar(d.root, .5F);
  OperationMetadata scalar_metadata;
  scalar_metadata.result_schema =
      std::make_shared<SchemaTemplate>(control.schema());
  for (const auto& batches :
       std::vector<std::vector<uint64_t>>{{}, {1}, {1, 1, 1}, {1, 1}}) {
    auto input_schema = schema();
    input_schema.tensors[0].batch_axes.assign(batches.begin(), batches.end());
    if (batches.size() == 2) {
      input_schema.tensors[0].layout.spatial = false;
      input_schema.tensors[0].facets.clear();
    }
    require(
        input_schema.validate(true).ok(),
        "generic Result schema permits alternate batch and spatial layouts");
    OperationMetadata input;
    input.result_schema = std::make_shared<SchemaTemplate>(input_schema);
    for (const auto* operation :
         {"image.opacity", "image.source_over", "image.split_horizontal"}) {
      std::vector<OperationMetadata> inputs{input};
      std::map<std::string, ParameterValue> parameters;
      if (std::string(operation) == "image.opacity")
        inputs.push_back(scalar_metadata);
      else if (std::string(operation) == "image.source_over")
        inputs.push_back(input);
      else
        parameters["split_x"] = int64_t{2};
      auto result = d.registry->resolve_traits(operation, inputs, parameters);
      require(!result.ok() && result.status().code == ErrorCode::TypeMismatch,
              "image metadata rejects unsupported batch or spatial layout "
              "before coordinate indexing");
    }
  }
}
void image_control_contracts() {
  Driver d;
  ExecutionBinding image_input, control;
  image_input.name = "image";
  image_input.result = image(d.root);
  control.name = "gain";
  control.result = scalar(d.root, .5F);
  auto prepared = d.prepare("image.opacity", {image_input, control});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  const auto shape = image_input.result.schema().tensors[0].sample_shape();
  auto query = take(Footprint::from_regions(
      shape, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}, {0, 4}})}));
  auto output = take(d.context->execute_fragments(frozen, {{"out", query}}));
  require(read(output.results.at("out"), {1, 0, 1, 2, 0}) ==
              .5F * read(image_input.result, {1, 0, 1, 2, 0}),
          "Result scalar opacity accepts unaligned singleton stride");
  auto changed = take(Footprint::all({1}));
  for (uint32_t role : {1U, 4U})
    require(take(output.dependencies.potential_dirty("gain", changed, role))
                    .at("out") == query,
            "scalar data and validation edits dirty every observed pixel");
  auto empty_output = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none(shape))}}));
  bool scalar_validation = false;
  for (const auto& observation :
       take(empty_output.dependencies.source_observations())) {
    scalar_validation |=
        observation.input == "gain" && (observation.roles & 4U) != 0;
    require(observation.input != "image" ||
                observation.target == ResultSupportTarget::Descriptor,
            "Empty opacity reads no image samples");
  }
  require(scalar_validation && take(empty_output.results.at("out").descriptor())
                                   .tensor_coverage(0)
                                   .empty(),
          "Empty opacity still validates its Result scalar control");
  for (bool empty : {false, true}) {
    for (float value : {-1.F, 1.5F, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
      control.result = scalar(d.root, value);
      auto invalid = d.prepare("image.opacity", {image_input, control});
      const auto before = d.root.statistics().live[ResourceKind::Payload];
      auto captured = d.context->freeze(invalid.plan, invalid.bindings);
      auto failed =
          captured.ok()
              ? d.context->execute_fragments(
                    captured.value(),
                    {{"out", empty ? take(Footprint::none(shape)) : query}})
              : Result<DemandResult>(captured.status());
      require(!failed.ok() &&
                  failed.status().code == ErrorCode::InvalidArgument &&
                  failed.status().detail.input_id == 2 &&
                  d.root.statistics().live[ResourceKind::Payload] == before,
              "direct Result opacity control rejects invalid numbers even "
              "for Empty and releases unpublished output");
    }
  }
  control.result = scalar(d.root, 16);
  require(
      read(d.run("image.exposure_gain", {image_input, control}),
           {0, 0, 0, 1, 0}) == 16 * read(image_input.result, {0, 0, 0, 1, 0}),
      "exposure accepts its inclusive maximum Result control");
  control.result = scalar(d.root, 16.5F);
  auto exposure = d.prepare("image.exposure_gain", {image_input, control});
  require(!d.context->execute(exposure.plan, exposure.bindings).ok(),
          "exposure retains maximum control bound");
  std::vector<ExecutionBinding> brush{image_input};
  for (float value : {0.F, 0.F, 0.F, 1.F, 0.F, 0.F, .5F}) {
    ExecutionBinding input;
    input.name = "p" + std::to_string(brush.size());
    input.result = scalar(d.root, value);
    brush.push_back(std::move(input));
  }
  auto radius = d.prepare("image.brush_circle", brush);
  auto rejected = d.context->execute(radius.plan, radius.bindings);
  require(!rejected.ok() &&
              rejected.status().code == ErrorCode::InvalidArgument &&
              rejected.status().detail.input_id == 4,
          "brush Result radius must be positive normal Float32");
  brush[3].result = scalar(d.root, .5F);
  brush[7].result = scalar(d.root, 1.5F);
  auto alpha = d.prepare("image.brush_circle", brush);
  rejected = d.context->execute(alpha.plan, alpha.bindings);
  require(!rejected.ok() && rejected.status().detail.input_id == 8,
          "brush Result alpha retains unit interval bound");
}
}  // namespace
int main() {
  try {
    image_schema_contracts();
    image_control_contracts();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
