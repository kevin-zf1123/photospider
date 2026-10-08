#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_image_execution_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
bool native_image_algorithms() {
  Driver d(true);
  if (!d.context->gpu_enabled())
    return false;
  ExecutionBinding a{"a", image(d.root)};
  ExecutionBinding b{"b", image(d.root, false, 2)};
  ExecutionBinding mask{"mask", image(d.root, true)};
  ExecutionBinding gain{"gain", scalar(d.root, 2)};
  ExecutionBinding opacity{"opacity", scalar(d.root, .25F)};
  std::vector<ExecutionBinding> brush{a};
  for (float value : {4.5F, 2.5F, .5F, 1.F, 0.F, 0.F, .25F})
    brush.push_back(
        {"p" + std::to_string(brush.size()), scalar(d.root, value)});
  struct Case {
    std::string key;
    std::vector<ExecutionBinding> inputs;
    std::map<std::string, ParameterValue> parameters;
  };
  const std::vector<Case> cases{
      {"image.exposure_gain", {a, gain}, {}},
      {"image.opacity", {a, opacity}, {}},
      {"image.gaussian_blur", {a}, {{"radius", int64_t{2}}, {"sigma", .9}}},
      {"image.mask", {a, mask}, {}},
      {"image.source_over", {a, b}, {}},
      {"image.downsample_box", {a}, {{"factor", int64_t{2}}}},
      {"mask.downsample_box", {mask}, {{"factor", int64_t{2}}}},
      {"image.brush_circle", brush, {}}};
  uint64_t dispatches = 0;
  for (const auto& item : cases) {
    d.native = false;
    const auto reference = d.run(item.key, item.inputs, item.parameters);
    d.native = true;
    auto prepared = d.prepare(item.key, item.inputs, item.parameters);
    auto run = take(d.context->execute(prepared.plan, prepared.bindings));
    require(run.diagnostics.native_dispatch_count > 0 &&
                run.diagnostics.fallback_reasons.empty(),
            "image GPU case must execute native without fallback");
    dispatches += run.diagnostics.native_dispatch_count;
    const auto result = run.results.at("out");
    auto query =
        take(Footprint::all(result.schema().tensors[0].sample_shape()));
    require(query
                .visit(
                    [&](const auto& at) {
                      const auto expected = read(reference, at),
                                 actual = read(result, at);
                      require(
                          std::abs(actual - expected) <=
                              1e-6F + 1e-5F * std::abs(expected),
                          "native image result matches CPU numerical contract");
                      return Status::success();
                    },
                    UINT64_MAX)
                .ok(),
            "native image traversal");
  }
  auto make_image = [&](uint64_t width, float rgb) {
    auto s = schema();
    s.tensors[0].batch_axes = {1, 1};
    s.tensors[0].descriptor.shape = {1, width, 4};
    auto builder = take(ResultBuilder::start(d.root, s, "native.boundary"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
                .ok(),
            "native boundary descriptor");
    std::vector<float> pixels(width * 4, rgb);
    for (uint64_t i = 3; i < pixels.size(); i += 4)
      pixels[i] = .5F;
    require(builder
                .publish_tensor(
                    0, Region::whole(s.tensors[0].sample_shape()),
                    ByteView(reinterpret_cast<const uint8_t*>(pixels.data()),
                             pixels.size() * 4),
                    take(ResultRelation::cartesian(d.root, pixels.size(),
                                                   {0, 1, 0, 0})),
                    {true, true, true, true})
                .ok(),
            "native boundary pixels");
    return take(builder.seal());
  };
  {
    auto source = make_image(64 * 205, .25F);
    auto prepared = d.prepare("image.opacity", {{"a", source}, opacity});
    ExecutionOptions options;
    options.maximum_dependency_work = UINT64_MAX;
    options.dependencies.maximum_work = UINT64_MAX;
    auto output =
        take(d.context->execute(prepared.plan, prepared.bindings, {}, options));
    require(output.diagnostics.native_dispatch_count > 0 &&
                output.diagnostics.fallback_reasons.empty(),
            "205 native tiles release buffer tokens after each dispatch");
    require(
        read(output.results.at("out"), {0, 0, 0, 64 * 205 - 1, 0}) == .0625F,
        "last native tile published correctly");
    dispatches += output.diagnostics.native_dispatch_count;
  }
  {
    auto source = make_image(1, 1e-30F);
    std::vector<ExecutionBinding> inputs{{"a", source}, opacity};
    d.native = false;
    auto reference = d.run("image.opacity", inputs);
    d.native = true;
    auto prepared = d.prepare("image.opacity", inputs);
    const auto before = d.root.statistics().live[ResourceKind::Payload];
    {
      auto output = take(d.context->execute(prepared.plan, prepared.bindings));
      require(output.diagnostics.fallback_reasons.size() == 1 &&
                  output.diagnostics.selected_backends.at(
                      prepared.plan.steps()[0].result_ref()) == Backend::Cpu,
              "native domain rejection restarts exact CPU continuation");
      for (uint64_t c = 0; c < 4; ++c)
        require(read(reference, {0, 0, 0, 0, c}) ==
                    read(output.results.at("out"), {0, 0, 0, 0, c}),
                "fallback preserves exact CPU samples");
    }
    require(d.root.statistics().live[ResourceKind::Payload] == before,
            "failed GPU attempt and fallback output release payloads");
    ExecutionOptions limited;
    limited.dependencies.maximum_stages = 3;
    auto failed =
        d.context->execute(prepared.plan, prepared.bindings, {}, limited);
    require(
        !failed.ok() && failed.status().code == ErrorCode::ResourceExhausted,
        "fallback cannot reset caller stage budget");
  }
  ResultRef retained_chain;
  {
    Driver chain(true);
    std::vector<ExecutionBinding> inputs{{"image", image(chain.root)},
                                         {"gain", scalar(chain.root, 2)},
                                         {"opacity", scalar(chain.root, .25F)}};
    WorkflowDocument document;
    for (unsigned i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration declaration;
      declaration.id = i + 1;
      declaration.name = inputs[i].name;
      declaration.result_schema =
          std::make_shared<SchemaTemplate>(inputs[i].result.schema());
      document.inputs.push_back(std::move(declaration));
    }
    document.nodes = {
        {1,
         "image.exposure_gain",
         {WorkflowInputReference{1}, WorkflowInputReference{2}},
         {}},
        {2,
         "image.opacity",
         {WorkflowNodeOutput{1, "value"}, WorkflowInputReference{3}},
         {}}};
    document.outputs = {{"out", 2, "value"}};
    GraphContext graph(document);
    PlanningOptions options;
    options.execution_mode = ExecutionMode::NativeGpu;
    auto compiled = take(Compiler(chain.registry).compile(graph, options));
    auto result =
        take(chain.context->execute(compiled.plan, {std::move(inputs)}));
    require(
        result.diagnostics.native_dispatch_count == 8 &&
            result.diagnostics.transfer_count == 4 &&
            result.diagnostics.fallback_reasons.empty(),
        "native Result chain reuses GPU backing without intermediate uploads");
    retained_chain = result.results.at("out");
  }
  require(read(retained_chain, {0, 0, 0, 1, 0}) == .0625F &&
              read(retained_chain, {1, 1, 0, 1, 3}) == .125F,
          "native Result output retains backing after context retirement");
  std::cout << "eight Result image GPU algorithms passed; native_dispatches="
            << dispatches << '\n';
  return true;
}
}  // namespace
int main() {
  try {
    return native_image_algorithms() ? 0 : 77;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
