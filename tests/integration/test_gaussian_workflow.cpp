#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/icc_fixture.hpp"
#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ParameterValue>;
Parameters parameters(std::int64_t rx = 1, std::int64_t ry = 1,
                      const std::string& boundary = "clamp") {
  return {{"radius_x", rx},
          {"radius_y", ry},
          {"sigma_x", 1.},
          {"sigma_y", 1.},
          {"x_axis", std::int64_t{1}},
          {"y_axis", std::int64_t{0}},
          {"cval", 3.},
          {"boundary", boundary}};
}
Value input_value(bool narrow, const std::vector<std::uint64_t>& shape,
                  const std::vector<std::uint64_t>& bits) {
  ValueDescriptor descriptor{
      narrow ? ElementType::Float32 : ElementType::Float64, shape};
  auto writer = MutableValue::allocate(descriptor, Region::whole(shape),
                                       BufferAllocator{})
                    .take_value();
  const auto width = narrow ? 4 : 8;
  for (unsigned i = 0; i < bits.size(); ++i)
    std::memcpy(writer.data() + i * width, &bits[i], width);
  return std::move(writer).publish().take_value();
}
Result<ResultRef> run(const Value& input, Parameters parameters,
                      unsigned workers, ResourceLimits limits = {},
                      CancellationToken cancellation = {},
                      bool producer = false, bool tiled = false) {
  auto registry = make_default_operation_registry(false);
  WorkflowDocument document;
  numeric_result_fixture::declare_sources(&document, {input});
  document.nodes = {{1,
                     tiled ? "filter.gaussian_baked64_v1_strict_cpu_tiled"
                           : "filter.gaussian_baked64_v1_strict_cpu_whole",
                     {WorkflowInputReference{1}},
                     std::move(parameters)}};
  document.outputs = {{"output", 1, "output"}};
  if (producer) {
    document.nodes[0].inputs = {WorkflowNodeOutput{2, "value"}};
    document.nodes.push_back(
        {2, "core.identity", {WorkflowInputReference{1}}, {}});
  }
  registry->freeze();
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_width = planning.tile_height = 2;
  auto compiled = Compiler(registry).compile(graph, planning);
  if (!compiled.ok())
    return Result<ResultRef>(compiled.status());
  ExecutionContextConfig config;
  config.gpu_enabled = false;
  config.cpu_workers = workers;
  config.managed_resources = limits;
  ExecutionContext context(registry, config);
  ExecutionOptions options;
  options.dependencies.maximum_work = UINT64_C(1000000000000);
  options.maximum_dependency_work = UINT64_C(1000000000000);
  auto bindings = numeric_result_fixture::bind_sources(
      context.resource_budget().take_value(), {input});
  auto result = context.execute(compiled.value().plan, std::move(bindings),
                                cancellation, options);
  return result.ok() ? Result<ResultRef>(result.value().results.at("output"))
                     : Result<ResultRef>(result.status());
}
std::uint64_t word(const ResultRef& value, unsigned index) {
  std::uint64_t result = 0;
  const auto width =
      Value::element_size(value.schema().tensors[0].descriptor.element_type);
  std::memcpy(&result,
              numeric_result_fixture::bytes(value).data() + index * width,
              width);
  return result;
}
double number(std::uint64_t bits) {
  double value;
  std::memcpy(&value, &bits, 8);
  return value;
}
int typed_results() {
  using numeric_result_fixture::take;
  for (bool image : {false, true}) {
    ResultRef retained, empty;
    ColorProfileIdentity identity;
    std::vector<std::uint8_t> expected;
    for (bool tiled : {false, true}) {
      auto registry = make_default_operation_registry();
      ExecutionContextConfig config;
      config.cpu_workers = 4;
      config.gpu_enabled = false;
      config.managed_resources = ResourceLimits{};
      ExecutionContext context(registry, config);
      const auto root = take(context.resource_budget());
      SchemaTemplate schema;
      schema.id = image ? "photospider.image" : "test.gaussian.cmyk";
      ResultTensorSpec tensor;
      tensor.key = "pixels";
      tensor.batch_axes = {2, 1};
      tensor.descriptor = {ElementType::Float32, {3, 5, 4}};
      tensor.layout.spatial = image;
      ResourceBindings resources;
      if (image) {
        tensor.facets = {take(encode_semantic(rgba_semantics()))};
      } else {
        auto bytes = numeric_fixture::fixture();
        auto profile =
            take(IccProfile::import({bytes.data(), bytes.size()}, root));
        identity = profile.identity();
        resources = take(ResourceBindings::create({profile}, root));
        ColorArrayDescriptor color;
        color.model = ColorModel::Cmyk;
        color.reference = ColorReference::ProfileRelative;
        color.white.reset();
        color.profile = identity;
        tensor.facets = {take(encode_color_array(color))};
      }
      schema.tensors.push_back(tensor);
      const auto shape = tensor.sample_shape();
      const auto count = take(tensor.sample_count());
      auto builder = take(ResultBuilder::start(root, schema, "gaussian.typed",
                                               {}, {}, 2, 2, resources));
      PS_CHECK(builder
                   .bind_descriptor_relation(
                       take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                   .ok());
      std::vector<float> values(count);
      for (unsigned i = 0; i < count; ++i)
        values[i] = i % 4 == 3 ? .5F : static_cast<float>(i % 11) / 16;
      PS_CHECK(
          builder
              .publish_tensor(
                  0, Region::whole(shape),
                  ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                           count * 4),
                  take(ResultRelation::cartesian(root, count, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok());
      auto input = take(builder.seal());
      WorkflowDocument document;
      WorkflowInputDeclaration declaration;
      declaration.id = 1;
      declaration.name = "input";
      declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
      document.inputs.push_back(declaration);
      auto p = parameters(1, 1);
      p["x_axis"] = std::int64_t{3};
      p["y_axis"] = std::int64_t{2};
      document.nodes = {{1,
                         tiled ? "filter.gaussian_baked64_v1_strict_cpu_tiled"
                               : "filter.gaussian_baked64_v1_strict_cpu_whole",
                         {WorkflowInputReference{1}},
                         p}};
      document.outputs = {{"output", 1, "output"}};
      PlanningOptions planning;
      planning.tile_height = planning.tile_width = 2;
      ExecutionBindings bindings{{{"input", input}}};
      ExecutionOptions options;
      options.dependencies.maximum_work = UINT64_C(1000000000000);
      options.maximum_dependency_work = UINT64_C(1000000000000);
      GraphContext graph(document);
      auto plan = take(Compiler(registry).compile(graph, planning, resources));
      retained = take(context.execute(plan.plan, bindings, {}, options))
                     .results.at("output");
      auto output_schema = retained.schema();
      output_schema.publication = schema.publication;
      PS_CHECK(output_schema.same_schema(schema));
      const auto actual = numeric_result_fixture::bytes(retained);
      if (!tiled)
        expected = actual;
      PS_CHECK(actual == expected);
      if (!image)
        PS_CHECK(retained.resources().icc_profile(identity).ok());
      if (tiled) {
        auto frozen = take(context.freeze(plan.plan, bindings));
        const auto q = take(Footprint::from_regions(
            shape, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}, {1, 1}})}));
        auto roi = take(context.execute_fragments(frozen, {{"output", q}}, {},
                                                  options))
                       .results.at("output");
        auto facts = take(roi.descriptor());
        PS_CHECK(take(facts.tensor_coverage(0).element_count()) == 4);
        for (unsigned c = 0; c < 4; ++c) {
          std::uint32_t bits = 0;
          PS_CHECK(roi.read_tensor(facts, 0, {1, 0, 1, 2, c}, &bits, 4).ok());
          PS_CHECK(
              !std::memcmp(&bits, expected.data() + ((15 + 7) * 4 + c) * 4, 4));
        }
        unsigned data_samples = 0, validation_samples = 0;
        PS_CHECK(
            take(roi.tensor_relation(0))
                .project(q,
                         [&](ResultSupport support, const Footprint* samples) {
                           if (!samples)
                             return Status{ErrorCode::Internal,
                                           "missing shaped support"};
                           if (support.roles & 1)
                             data_samples += take(samples->element_count());
                           if (support.roles & 4)
                             validation_samples +=
                                 take(samples->element_count());
                           return Status::success();
                         })
                .ok());
        PS_CHECK(data_samples == 9 && validation_samples == 36);
        for (unsigned poison_index : {0U, (15U + 7U) * 4U}) {
          auto poisoned_values = values;
          const std::uint32_t nan = 0x7fc00123;
          std::memcpy(&poisoned_values[poison_index], &nan, 4);
          auto poisoned = take(ResultBuilder::start(
              root, schema, "gaussian.poison", {}, {}, 2, 2, resources));
          PS_CHECK(poisoned
                       .bind_descriptor_relation(take(
                           ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                       .ok());
          PS_CHECK(poisoned
                       .publish_tensor(
                           0, Region::whole(shape),
                           ByteView(reinterpret_cast<const std::uint8_t*>(
                                        poisoned_values.data()),
                                    count * 4),
                           take(ResultRelation::cartesian(root, count,
                                                          {0, 1, 0, 0})),
                           {true, true, true, true})
                       .ok());
          ExecutionBindings bad_bindings{{{"input", take(poisoned.seal())}}};
          auto bad_frozen = take(context.freeze(plan.plan, bad_bindings));
          auto checked = context.execute_fragments(bad_frozen, {{"output", q}},
                                                   {}, options);
          if (!poison_index) {
            PS_CHECK(checked.ok());
          } else {
            PS_CHECK(!checked.ok() &&
                     checked.status().code == ErrorCode::InvalidArgument);
          }
          PS_CHECK(
              context
                  .execute_fragments(bad_frozen,
                                     {{"output", take(Footprint::none(shape))}},
                                     {}, options)
                  .ok());
        }
        empty = take(context.execute_fragments(
                         frozen, {{"output", take(Footprint::none(shape))}}, {},
                         options))
                    .results.at("output");
      }
      if (image) {
        document.nodes.push_back({2,
                                  "image.split_horizontal",
                                  {WorkflowNodeOutput{1, "output"}},
                                  {{"split_x", std::int64_t{2}}}});
        document.outputs = {{"left", 2, "left"}};
        GraphContext chain(document);
        auto compiled = take(Compiler(registry).compile(chain, planning));
        auto left = take(context.execute(compiled.plan, bindings, {}, options))
                        .results.at("left");
        PS_CHECK(left.schema().tensors[0].sample_shape() ==
                 std::vector<std::uint64_t>({2, 1, 3, 2, 4}));
      }
    }
    PS_CHECK(numeric_result_fixture::bytes(retained) == expected);
    PS_CHECK(take(empty.descriptor()).tensor_coverage(0).empty());
    if (!image) {
      PS_CHECK(retained.resources().icc_profile(identity).ok());
      PS_CHECK(empty.resources().icc_profile(identity).ok());
    }
  }
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc == 2 && (std::string(argv[1]) == "--stdin" ||
                    std::string(argv[1]) == "--tiled-stdin")) {
    unsigned narrow, height, width, rx, ry;
    std::uint64_t sx, sy, cval;
    std::string boundary;
    while (std::cin >> std::dec >> narrow >> height >> width >> rx >> ry >>
           std::hex >> sx >> sy >> cval >> boundary) {
      std::vector<std::uint64_t> data(height * width);
      for (auto& value : data)
        std::cin >> std::hex >> value;
      auto p = parameters(rx, ry, boundary);
      p["sigma_x"] = number(sx);
      p["sigma_y"] = number(sy);
      p["cval"] = number(cval);
      auto result = run(input_value(narrow, {height, width}, data), p, 4, {},
                        {}, false, std::string(argv[1]) == "--tiled-stdin");
      if (!result.ok()) {
        std::cerr << result.status().message << '\n';
        return 1;
      }
      for (unsigned i = 0; i < data.size(); ++i)
        std::cout << std::hex << word(result.value(), i) << ' ';
      std::cout << '\n';
    }
    return std::cin.eof() ? 0 : 2;
  }
  PS_CHECK(typed_results() == 0);
  auto input = input_value(true, {1, 3}, {0x3f800000, 0x40000000, 0x40800000});
  const std::array<const char*, 5> boundaries{"constant", "clamp", "wrap",
                                              "reflect_half", "reflect_whole"};
  const std::array<std::array<std::uint32_t, 3>, 5> expected{
      {{0x401df06a, 0x402b01b3, 0x40452444},
       {0x3fa314ae, 0x40118a57, 0x405ceb52},
       {0x4006295c, 0x40118a57, 0x40284c4c},
       {0x3fa314ae, 0x40118a57, 0x405ceb52},
       {0x3fc6295c, 0x40118a57, 0x4039d6a4}}};
  for (unsigned i = 0; i < boundaries.size(); ++i)
    for (unsigned workers : {1, 4}) {
      auto result = run(input, parameters(1, 1, boundaries[i]), workers);
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      for (unsigned j = 0; j < 3; ++j)
        PS_CHECK(word(result.value(), j) == expected[i][j]);
    }
  auto exceptional =
      input_value(true, {1, 3}, {0xff800123, 0x7f800456, 0x80000000});
  auto copied = run(exceptional, parameters(0, 0), 4);
  PS_CHECK(copied.ok() && numeric_result_fixture::bytes(copied.value()) ==
                              exceptional.copy_bytes());
  auto arithmetic = run(exceptional, parameters(1, 0), 4);
  PS_CHECK(arithmetic.ok() && word(arithmetic.value(), 0) == 0x7fc00456);
  auto singleton_parameters = parameters(1, 0);
  singleton_parameters["sigma_x"] = .01;
  for (bool tiled : {false, true}) {
    auto singleton =
        run(exceptional, singleton_parameters, 4, {}, {}, false, tiled);
    PS_CHECK(singleton.ok() && word(singleton.value(), 0) == 0xffc00123 &&
             word(singleton.value(), 1) == 0x7fc00456 &&
             word(singleton.value(), 2) == 0x80000000);
  }
  for (const auto* name : {"sigma_x", "sigma_y", "radius_x", "boundary"}) {
    auto p = parameters();
    p.erase(name);
    PS_CHECK(!run(input, p, 1).ok());
  }
  for (const auto& change : std::vector<std::pair<std::string, ParameterValue>>{
           {"sigma_x", -1.},
           {"sigma_y", 0.},
           {"radius_x", std::int64_t{-1}},
           {"x_axis", std::int64_t{0}},
           {"boundary", std::string("mirror")}}) {
    auto p = parameters();
    p[change.first] = change.second;
    auto result = run(input, p, 1);
    PS_CHECK(!result.ok() &&
             result.status().code == ErrorCode::InvalidArgument);
  }
  std::vector<std::uint64_t> batch_bits(70);
  for (unsigned i = 0; i < batch_bits.size(); ++i) {
    const double value = (static_cast<int>(i * 17 % 53) - 26) / 8.;
    std::memcpy(&batch_bits[i], &value, 8);
  }
  auto batch = input_value(false, {2, 5, 7}, batch_bits);
  auto batch_parameters = parameters(2, 1, "wrap");
  batch_parameters["x_axis"] = std::int64_t{2};
  batch_parameters["y_axis"] = std::int64_t{1};
  auto serial = run(batch, batch_parameters, 1);
  auto parallel = run(batch, batch_parameters, 4);
  PS_CHECK(serial.ok() && parallel.ok());
  PS_CHECK(numeric_result_fixture::bytes(serial.value()) ==
           numeric_result_fixture::bytes(parallel.value()));
  for (unsigned plane = 0; plane < 2; ++plane) {
    std::vector<std::uint64_t> slice(batch_bits.begin() + plane * 35,
                                     batch_bits.begin() + (plane + 1) * 35);
    auto separate =
        run(input_value(false, {5, 7}, slice), parameters(2, 1, "wrap"), 4);
    PS_CHECK(separate.ok());
    PS_CHECK(
        std::memcmp(numeric_result_fixture::bytes(separate.value()).data(),
                    numeric_result_fixture::bytes(parallel.value()).data() +
                        plane * 35 * 8,
                    35 * 8) == 0);
  }
  std::vector<std::uint8_t> reversed_bytes(80);
  const std::uint32_t reversed_words[] = {0x40800000, 0x40000000, 0x3f800000};
  std::memcpy(reversed_bytes.data() + 17, reversed_words, 12);
  auto reversed =
      Value::create({ElementType::Float32, {1, 3}}, Region::whole({1, 3}),
                    {57, {12, -4}, {5, 7}}, reversed_bytes);
  PS_CHECK(reversed.ok());
  auto strided = run(reversed.value(), parameters(), 4, {}, {}, true);
  if (!strided.ok())
    std::cerr << strided.status().message << "\n";
  PS_CHECK(strided.ok());
  for (unsigned i = 0; i < 3; ++i)
    PS_CHECK(word(strided.value(), i) == expected[1][i]);
  ResourceLimits work;
  work.maximum_work = 10000;
  auto exhausted = run(input, parameters(), 4, work);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  work.maximum_work = 5000000;
  auto arithmetic_budget = run(batch, batch_parameters, 4, work);
  PS_CHECK(!arithmetic_budget.ok() &&
           arithmetic_budget.status().code == ErrorCode::ResourceExhausted);
  ResourceLimits capacity;
  capacity.capacity[ResourceKind::Payload] = 4096;
  auto shortage = run(input, parameters(), 4, capacity);
  PS_CHECK(!shortage.ok() &&
           shortage.status().code == ErrorCode::ResourceExhausted);
  CancellationSource stop;
  stop.cancel();
  auto cancelled = run(input, parameters(), 4, {}, stop.token());
  PS_CHECK(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled);
  auto huge = parameters(INT64_MAX, 1);
  auto overflow = run(input, huge, 1);
  PS_CHECK(!overflow.ok() &&
           overflow.status().code == ErrorCode::ResourceExhausted);
  return 0;
}
