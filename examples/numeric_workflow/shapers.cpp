#include "photospider/ops/numeric/shapers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/ops/numeric/color_ramps.hpp"
#include "photospider/photospider.hpp"
#include "photospider/plugin/result_program.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
std::uint64_t bits(double value, bool narrow = false) {
  std::uint64_t raw = 0;
  if (narrow) {
    const float small = value;
    std::memcpy(&raw, &small, 4);
  } else {
    std::memcpy(&raw, &value, 8);
  }
  return raw;
}
template <class Input>
ps::ValueDescriptor descriptor(const Input& value) {
  const auto& tensor = value.schema().tensors[0];
  return {tensor.descriptor.element_type, tensor.sample_shape()};
}
struct SampleInput {
  ps::SchemaTemplate format;
  ps::StridedLayout layout;
  std::vector<uint64_t> words;
  const ps::SchemaTemplate& schema() const { return format; }
};
SampleInput array(bool narrow, const std::vector<std::uint64_t>& words,
                  std::vector<std::uint64_t> shape = {},
                  std::vector<ps::ValueFacet> facets = {},
                  std::optional<ps::StridedLayout> layout = {}) {
  const unsigned width = narrow ? 4 : 8;
  if (shape.empty())
    shape = {words.size()};
  ps::SchemaTemplate schema;
  schema.id = "manual.shaper.input";
  ps::ResultTensorSpec tensor;
  tensor.key = "coordinates";
  tensor.descriptor = {
      narrow ? ps::ElementType::Float32 : ps::ElementType::Float64, shape};
  tensor.facets = std::move(facets);
  schema.tensors.push_back(std::move(tensor));
  if (!layout) {
    std::vector<int64_t> strides(shape.size());
    int64_t stride = width;
    for (auto i = shape.size(); i-- > 0;) {
      strides[i] = stride;
      stride *= shape[i];
    }
    layout = ps::StridedLayout{1, strides};
  }
  return {std::move(schema), std::move(*layout), words};
}
ps::Result<ps::ResultRef> publish_input(const SampleInput& input,
                                        const ps::ResourceBudget& root) {
  using Answer = ps::Result<ps::ResultRef>;
  const auto width = ps::Value::element_size(descriptor(input).element_type);
  auto allocation = root.allocator().allocate(width * input.words.size() + 1);
  if (!allocation.ok())
    return Answer(allocation.status());
  auto bytes = allocation.take_value();
  for (std::size_t i = 0; i < input.words.size(); ++i)
    std::memcpy(bytes.data() + 1 + i * width, &input.words[i], width);
  auto started = ps::ResultBuilder::start(root, input.schema(), "shaper.input");
  if (!started.ok())
    return Answer(started.status());
  auto builder = started.take_value();
  auto descriptor_relation =
      ps::ResultRelation::cartesian(root, 1, {0, 8, 0, 0});
  if (!descriptor_relation.ok())
    return Answer(descriptor_relation.status());
  auto bound =
      builder.bind_descriptor_relation(descriptor_relation.take_value());
  if (!bound.ok())
    return Answer(bound);
  auto relation = ps::ResultRelation::cartesian(
      root, take(input.schema().tensors[0].sample_count()), {0, 1, 0, 0});
  if (!relation.ok())
    return Answer(relation.status());
  auto published =
      builder.publish_tensor(0, ps::Region::whole(descriptor(input).shape),
                             input.layout, std::move(bytes).freeze(),
                             relation.take_value(), {true, true, true, true});
  return published.ok() ? builder.seal() : Answer(published);
}
SampleInput numbers(bool narrow, const std::vector<double>& values) {
  std::vector<std::uint64_t> words;
  for (auto value : values)
    words.push_back(bits(value, narrow));
  return array(narrow, words);
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<SampleInput> values;
  Fixture(unsigned method, ps::CpuNumericProfile profile,
          const std::vector<SampleInput>& values)
      : values(values) {
    for (unsigned i = 0; i < values.size(); ++i) {
      const auto& value = values[i];
      const auto name = "input" + std::to_string(i);
      ps::WorkflowInputDeclaration input;
      input.id = i + 1;
      input.name = name;
      input.result_schema =
          std::make_shared<ps::SchemaTemplate>(value.schema());
      document.inputs.push_back(std::move(input));
    }
    const ps::WorkflowInput x = ps::WorkflowInputReference{1},
                            l = ps::WorkflowInputReference{2},
                            u = ps::WorkflowInputReference{3};
    ps::WorkflowNodeOutput output;
    if (method < 2) {
      auto helper = method ? ps::numeric::linear_shaper_inverse
                           : ps::numeric::linear_shaper;
      output = take(helper(document, x, l, u, descriptor(values[0]), profile));
    } else {
      auto helper = method == 3 ? ps::numeric::log2_shaper_inverse_node
                                : ps::numeric::log2_shaper_node;
      document.nodes.push_back(take(helper(1, x, l, u, profile)));
      output = {1, "values"};
    }
    document.outputs = {{"values", output.source_node, output.source_port}};
  }
  ps::Result<ps::ExecutionBindings> bindings_for(
      const ps::ResourceBudget& root) const {
    ps::ExecutionBindings bindings;
    for (unsigned i = 0; i < values.size(); ++i) {
      auto value = publish_input(values[i], root);
      if (!value.ok())
        return ps::Result<ps::ExecutionBindings>(value.status());
      bindings.inputs.push_back({document.inputs[i].name, value.take_value()});
    }
    return ps::Result<ps::ExecutionBindings>(std::move(bindings));
  }
  ps::Result<ps::DemandResult> run(const ps::Footprint& wanted,
                                   bool joint = true) const {
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph);
    if (!compiled.ok())
      return ps::Result<ps::DemandResult>(compiled.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto bindings = bindings_for(take(context.resource_budget()));
    if (!bindings.ok())
      return ps::Result<ps::DemandResult>(bindings.status());
    auto frozen = context.freeze(compiled.value().plan, bindings.value());
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    ps::ExecutionOptions options;
    options.enable_joint = joint;
    options.maximum_dependency_work = UINT64_C(1) << 30;
    options.dependencies.maximum_work = UINT64_C(1) << 30;
    return context.execute_fragments(frozen.value(), {{"values", wanted}}, {},
                                     options);
  }
};
std::uint64_t read(const ps::ResultRef& value,
                   const std::vector<std::uint64_t>& at) {
  std::uint64_t raw = 0;
  require(
      value
          .read_tensor(take(value.descriptor()), 0, at, &raw,
                       ps::Value::element_size(descriptor(value).element_type))
          .ok(),
      "read shaper global coordinate");
  return raw;
}
void examples(ps::CpuNumericProfile profile) {
  for (bool narrow : {false, true}) {
    for (unsigned method = 0; method < 4; ++method) {
      const bool logarithmic = method >= 2, inverse = method & 1;
      const std::vector<double> input =
          logarithmic
              ? (inverse ? std::vector<double>{0, .25, .5, 1, -.25, 1.25}
                         : std::vector<double>{1, 2, 4, 16, .5, 32})
              : (inverse ? std::vector<double>{0, .5, 1, 1.5}
                         : std::vector<double>{-2, 0, 2, 4});
      const std::vector<double> expected =
          logarithmic
              ? (inverse ? std::vector<double>{1, 2, 4, 16, .5, 32}
                         : std::vector<double>{0, .25, .5, 1, -.25, 1.25})
              : (inverse ? std::vector<double>{-2, 0, 2, 4}
                         : std::vector<double>{0, .5, 1, 1.5});
      Fixture fixture(
          method, profile,
          {numbers(narrow, input), numbers(narrow, {logarithmic ? 1. : -2.}),
           numbers(narrow, {logarithmic ? 16. : 2.})});
      for (bool joint : {false, true}) {
        auto result =
            take(fixture.run(take(ps::Footprint::all({input.size()})), joint));
        const auto& output = result.results.at("values");
        require(output.schema().tensors[0].facets.empty(),
                "generic shaper result");
        for (unsigned i = 0; i < expected.size(); ++i)
          require(read(output, {i}) == bits(expected[i], narrow),
                  "public shaper golden");
      }
      const auto selected = take(
          ps::Footprint::from_regions({input.size()}, {ps::Region({{1, 1}})}));
      auto result = take(fixture.run(selected));
      auto support = take(result.dependencies.source_support());
      require(
          support.at("input0") == take(ps::Footprint::all({input.size()})) &&
              support.at("input1") == take(ps::Footprint::all({1})) &&
              support.at("input2") == take(ps::Footprint::all({1})),
          "complete input and both shared bounds");
      require(
          read(result.results.at("values"), {1}) == bits(expected[1], narrow),
          "partial shaper survives context destruction");
      auto empty = take(fixture.run(take(ps::Footprint::none({input.size()}))));
      require(take(empty.dependencies.source_support()).empty(),
              "Empty has no witnesses");
    }
  }
  std::cout
      << "four public shapers, both dtypes, exact landmarks/extrapolation, "
         "joint/cache-off, Whole witnesses, Empty and owner lifetime PASS\n";
}
ps::ResultRef direct(const ps::WorkflowNode& node,
                     const std::vector<SampleInput>& inputs) {
  const auto profile = node.operation.find("apple_silicon") != std::string::npos
                           ? ps::CpuNumericProfile::AppleSiliconNeon
                       : node.operation.find("x86_64") != std::string::npos
                           ? ps::CpuNumericProfile::X86Avx2
                           : ps::CpuNumericProfile::Strict;
  const auto method =
      node.operation.find("inverse") != std::string::npos ? 3U : 2U;
  Fixture fixture(method, profile, inputs);
  return take(
             fixture.run(take(ps::Footprint::all(descriptor(inputs[0]).shape))))
      .results.at("values");
}
SampleInput reversed(const SampleInput& value) {
  const auto meta = descriptor(value);
  const auto width = ps::Value::element_size(meta.element_type);
  const auto count = value.words.size();
  auto words = value.words;
  std::reverse(words.begin(), words.end());
  std::vector<int64_t> strides(meta.shape.size());
  int64_t stride = -static_cast<int64_t>(width);
  for (auto axis = meta.shape.size(); axis-- > 0;) {
    strides[axis] = stride;
    stride *= meta.shape[axis];
  }
  return array(meta.element_type == ps::ElementType::Float32, words, meta.shape,
               value.schema().tensors[0].facets,
               ps::StridedLayout{1 + (count - 1) * width, strides});
}
void admitted_resources(const ps::WorkflowNode& node,
                        const std::vector<SampleInput>& inputs) {
  const auto profile = node.operation.find("apple_silicon") != std::string::npos
                           ? ps::CpuNumericProfile::AppleSiliconNeon
                       : node.operation.find("x86_64") != std::string::npos
                           ? ps::CpuNumericProfile::X86Avx2
                           : ps::CpuNumericProfile::Strict;
  Fixture fixture(node.operation.find("inverse") != std::string::npos ? 3U : 2U,
                  profile, inputs);
  ps::GraphContext graph(fixture.document);
  auto compiled = take(ps::Compiler(fixture.registry).compile(graph));
  std::vector<ps::OperationMetadata> metadata(inputs.size());
  for (unsigned i = 0; i < inputs.size(); ++i)
    metadata[i].result_schema =
        std::make_shared<ps::SchemaTemplate>(inputs[i].schema());
  const auto traits =
      take(fixture.registry->resolve_traits(node.operation, metadata, {}));
  const auto bytes =
      take(inputs[0].schema().tensors[0].sample_count()) *
      ps::Value::element_size(descriptor(inputs[0]).element_type);
  uint64_t input_bytes = 0;
  for (const auto& input : inputs)
    input_bytes += input.words.size() *
                       ps::Value::element_size(descriptor(input).element_type) +
                   1;
  for (unsigned mode = 0; mode < 3; ++mode) {
    ps::ExecutionContextConfig config;
    config.managed_resources = ps::ResourceLimits{};
    if (mode == 0)
      config.managed_resources->maximum_work = 1024;
    else
      config.managed_resources->capacity[ps::ResourceKind::Payload] =
          input_bytes + (mode == 1 ? 8 : bytes + traits.workspace_bytes - 1);
    ps::ExecutionContext context(fixture.registry, config);
    auto root = take(context.resource_budget());
    auto bound = fixture.bindings_for(root);
    if (!bound.ok()) {
      require(bound.status().code == ps::ErrorCode::ResourceExhausted,
              "shaper binding admission");
      continue;
    }
    const auto baseline = root.statistics().live[ps::ResourceKind::Payload];
    auto frozen = context.freeze(compiled.plan, bound.value());
    if (!frozen.ok()) {
      require(frozen.status().code == ps::ErrorCode::ResourceExhausted,
              "shaper work admission");
      continue;
    }
    auto result = context.execute_fragments(
        frozen.value(),
        {{"values", take(ps::Footprint::all(descriptor(inputs[0]).shape))}});
    require(!result.ok() &&
                result.status().code == ps::ErrorCode::ResourceExhausted &&
                root.statistics().live[ps::ResourceKind::Payload] == baseline,
            "shaper work/output/exact workspace budget rollback");
  }
}
void resources(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (unsigned method : {2U, 3U}) {
    Fixture fixture(
        method, profile,
        {numbers(false, {2, 3, 4}), numbers(false, {1}), numbers(false, {16})});
    const auto operation = fixture.document.nodes[0].operation;
    auto dense = fixture.values;
    for (unsigned mask = 0; mask < 8; ++mask) {
      auto values = dense;
      for (unsigned port = 0; port < 3; ++port)
        if (mask & (1U << port))
          values[port] = reversed(values[port]);
      for (int rounding :
           {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        fenv_t saved;
        require(fegetenv(&saved) == 0 && fesetround(rounding) == 0 &&
                    feclearexcept(FE_ALL_EXCEPT) == 0 &&
                    feraiseexcept(FE_DIVBYZERO) == 0,
                "set shaper environment");
        auto output = direct(fixture.document.nodes[0], values);
        std::uint64_t actual = 0;
        actual = read(output, {0});
        require(actual == bits(method == 2 ? .25 : 256.), "strided result");
        require(fegetround() == rounding &&
                    fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
                "preserve shaper environment");
        require(fesetenv(&saved) == 0, "restore shaper environment");
      }
    }
    // Non-dyadic first values enter certified arithmetic before active
    // cancellation.
    dense[0] = numbers(false, std::vector<double>(256, method == 2 ? 3. : .3));
    admitted_resources(fixture.document.nodes[0], dense);
    dense[0] = array(false, {bits(method == 2 ? 4. : .5)}, {2, 2}, {},
                     ps::StridedLayout{1, {0, 0}});
    auto broadcast = direct(fixture.document.nodes[0], dense);
    const auto expected = bits(method == 2 ? .5 : 4.);
    for (unsigned i = 0; i < 4; ++i) {
      std::uint64_t actual = 0;
      actual = read(broadcast, {i / 2, i % 2});
      require(actual == expected, "multidimensional zero-stride shaper");
    }
    Fixture giant(
        method, profile,
        {numbers(false, {.5}), numbers(false, {1}), numbers(false, {16})});
    giant.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "values"};
    giant.document.nodes.push_back(take(ps::numeric::constant_node(
        2, ps::WorkflowInputReference{1}, {UINT64_C(1) << 40},
        ps::numeric::ArrayLayout::View, profile)));
    auto failed = giant.run(take(ps::Footprint::from_regions(
        {UINT64_C(1) << 40}, {ps::Region({{0, 1}})})));
    require(!failed.ok() &&
                failed.status().code == ps::ErrorCode::ResourceExhausted,
            "sparse giant shaper requires full input/output storage");
  }
  std::cout << "log shaper all-port unaligned/negative strides, floating "
               "environment, "
               "work/output/workspace, active cancellation, zero strides "
               "and giant output rejection PASS\n";
}
void invalid_bounds(ps::CpuNumericProfile profile) {
  for (unsigned method = 0; method < 4; ++method) {
    for (const auto& bounds : {std::array<double, 2>{2, 1}, {1, 1}, {0, 0}}) {
      Fixture fixture(
          method, profile,
          {array(false, {UINT64_C(0xfff0000000000042)}),
           numbers(false, {bounds[0]}), numbers(false, {bounds[1]})});
      auto result = fixture.run(take(ps::Footprint::all({1})));
      require(!result.ok() &&
                  result.status().code == ps::ErrorCode::InvalidArgument &&
                  result.status().reason == ps::FailureReason::InvalidDomain,
              "bounds before source signaling NaN");
    }
  }
  ps::WorkflowDocument document;
  document.nodes.push_back(
      {UINT64_MAX, "core.identity", {ps::WorkflowNodeOutput{1, "value"}}, {}});
  document.outputs.push_back({"existing", 2, "value"});
  const auto hint = descriptor(numbers(false, {1, 2}));
  auto result = take(
      ps::numeric::linear_shaper(document, ps::WorkflowNodeOutput{3, "value"},
                                 ps::WorkflowInputReference{1},
                                 ps::WorkflowInputReference{2}, hint, profile));
  require(result.source_node > 3 && document.outputs.size() == 1 &&
              document.nodes.size() == 8,
          "referenced IDs preserved without altering exports");
  const auto count = document.nodes.size();
  auto bad = ps::numeric::linear_shaper(
      document, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, {ps::ElementType::Float64, {0}}, profile);
  require(!bad.ok() && document.nodes.size() == count,
          "failed authoring is atomic");
  std::cout << "four shaper bound guards before NaN, safe authoring "
               "IDs/exports and failed expansion PASS\n";
}
void partitions_and_cache(ps::CpuNumericProfile profile) {
  for (unsigned method = 0; method < 4; ++method) {
    std::vector<double> samples;
    for (unsigned i = 0; i <= 32; ++i)
      samples.push_back(method == 2 ? .25 + i * .5 : i / 16.);
    Fixture fixture(
        method, profile,
        {numbers(false, samples), numbers(false, {1}), numbers(false, {16})});
    const auto all = take(ps::Footprint::all({samples.size()}));
    auto whole = take(fixture.run(all));
    const auto& output = whole.results.at("values");
    double previous = -1e300;
    for (unsigned j = 0; j < samples.size(); ++j) {
      const auto i = samples.size() - j - 1;
      auto partial =
          take(fixture.run(take(ps::Footprint::from_regions(
                               {samples.size()}, {ps::Region({{i, 1}})})),
                           false));
      require(read(partial.results.at("values"), {i}) == read(output, {i}),
              "reverse partition matches joint mapping");
      const auto raw = read(output, {j});
      double value;
      std::memcpy(&value, &raw, 8);
      require(value >= previous, "whole shaper monotonicity");
      previous = value;
    }
    const auto one = take(
        ps::Footprint::from_regions({samples.size()}, {ps::Region({{1, 1}})}));
    require(
        take(whole.dependencies.potential_dirty("input0", one)).at("values") ==
            all,
        "input changes dirty all recorded Whole demand");
    for (const auto* bound : {"input1", "input2"})
      require(take(whole.dependencies.potential_dirty(
                       bound, take(ps::Footprint::all({1}))))
                      .at("values") == all,
              "either bound dirties all dependent observations");
    ps::GraphContext graph(fixture.document);
    auto compiled = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 65536;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    auto root = take(context.resource_budget());
    auto bindings = take(fixture.bindings_for(root));
    auto demand = take(context.open_demand(compiled.plan, bindings));
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(1) << 30;
    options.dependencies.maximum_work = UINT64_C(1) << 30;
    take(demand.request({{"values", one}}, {}, options));
    auto repeated = take(demand.request({{"values", one}}, {}, options));
    require(read(repeated.results.at("values"), {1}) == read(output, {1}),
            "repeated Result request matches the complete mapping");
    for (unsigned port : {1U, 2U}) {
      fixture.values[port] = numbers(false, {port == 1 ? .5 : 32.});
      bindings.inputs[port].result =
          take(publish_input(fixture.values[port], root));
      require(demand.replace_bindings(bindings).ok(), "replace shaper bound");
      auto changed = take(demand.request({{"values", one}}, {}, options));
      auto fresh = take(fixture.run(one, false));
      require(read(changed.results.at("values"), {1}) ==
                      read(fresh.results.at("values"), {1}) &&
                  read(changed.results.at("values"), {1}) != read(output, {1}),
              "cache revalidates changed bound");
    }
  }
  std::cout << "four shaper joint/reversed partitions, monotonicity, "
               "Whole dirty "
               "and both-bound cache replacement PASS\n";
}
void typed_and_schema(ps::CpuNumericProfile profile) {
  const auto facet =
      take(ps::encode_color_array(ps::numeric::color_ramp_rgb_description()));
  auto input = array(false, {bits(2), bits(3), bits(4)}, {1, 3}, {facet});
  for (unsigned method = 0; method < 4; ++method) {
    Fixture fixture(method, profile,
                    {input, numbers(false, {1}), numbers(false, {16})});
    auto wanted = take(
        ps::Footprint::from_regions({1, 3}, {ps::Region({{0, 1}, {1, 1}})}));
    auto result = take(fixture.run(wanted));
    require(
        take(result.results.at("values").descriptor()).tensor_coverage(0) ==
                take(ps::Footprint::all({1, 3})) &&
            result.results.at("values").schema().tensors[0].facets.empty() &&
            take(result.dependencies.source_support()).at("input0") ==
                take(ps::Footprint::all({1, 3})),
        "generic local output with full typed input validation closure");
    fixture.values[0] =
        array(false, {bits(2), bits(3), UINT64_C(0x7ff8000000000042)}, {1, 3},
              {facet});
    auto failed = fixture.run(wanted);
    require(!failed.ok() &&
                failed.status().reason == ps::FailureReason::InvalidDomain,
            "typed invalid unrequested neighbor remains required");
    require(fixture.run(take(ps::Footprint::none({1, 3}))).ok(),
            "Empty skips typed payload validation");
    Fixture mismatch(
        method, profile,
        {numbers(false, {1, 2}), numbers(true, {1}), numbers(false, {16})});
    auto bad = mismatch.run(take(ps::Footprint::all({2})));
    require(!bad.ok() && bad.status().code == ps::ErrorCode::TypeMismatch,
            "bounds dtype schema");
  }
  for (bool narrow : {false, true}) {
    const auto negative_zero = UINT64_C(1) << (narrow ? 31 : 63);
    Fixture fixture(1, profile,
                    {array(narrow, {negative_zero}),
                     array(narrow, {negative_zero}), numbers(narrow, {1})});
    auto result = take(fixture.run(take(ps::Footprint::all({1}))));
    require(read(result.results.at("values"), {0}) == negative_zero,
            "inverse scalar order guard preserves lower negative zero");
  }
  std::cout << "four shaper ColorArray validation closure/Empty, dtype "
               "rejection and inverse -0 endpoint PASS\n";
}
struct FailingSource {
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::OperationFailed, "required shaper source"});
  }
};
void public_resources_and_upstream(ps::CpuNumericProfile profile) {
  for (unsigned method = 0; method < 4; ++method) {
    Fixture fixture(method, profile,
                    {numbers(false, std::vector<double>(4097, .5)),
                     numbers(false, {1}), numbers(false, {16})});
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    for (unsigned mode = 0; mode < 3; ++mode) {
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.result_cache_bytes = 0;
      config.maximum_live_bytes =
          mode == 0 ? 4099 * 8 + 3 + 1024 : 8 * 1024 * 1024;
      config.managed_resources = ps::ResourceLimits{};
      if (mode == 1)
        config.managed_resources->maximum_work = 1024;
      ps::ExecutionContext context(fixture.registry, config);
      auto budget = take(context.resource_budget());
      auto bound = fixture.bindings_for(budget);
      if (!bound.ok()) {
        require(mode != 2 &&
                    bound.status().code == ps::ErrorCode::ResourceExhausted,
                "public shaper input admission budget");
        continue;
      }
      const auto baseline = budget.statistics().live[ps::ResourceKind::Payload];
      auto frozen = context.freeze(plan.plan, bound.value());
      if (!frozen.ok()) {
        require(mode != 2 &&
                    frozen.status().code == ps::ErrorCode::ResourceExhausted,
                "public shaper admission budget category");
        continue;
      }
      const auto work = budget.statistics().issued.work;
      ps::CancellationSource cancellation;
      std::atomic<bool> done{false};
      std::thread watcher;
      if (mode == 2) {
        watcher = std::thread([&] {
          while (!done.load() && budget.statistics().issued.work < work + 10000)
            std::this_thread::yield();
          if (!done.load())
            cancellation.cancel();
        });
      }
      ps::Result<ps::DemandResult> result(
          ps::Status{ps::ErrorCode::Internal, {}});
      try {
        result = context.execute_fragments(
            frozen.value(), {{"values", take(ps::Footprint::all({4097}))}},
            cancellation.token());
      } catch (...) {
        done.store(true);
        if (watcher.joinable())
          watcher.join();
        throw;
      }
      done.store(true);
      if (watcher.joinable())
        watcher.join();
      require(
          !result.ok() &&
              result.status().code ==
                  (mode == 2 ? ps::ErrorCode::Cancelled
                             : ps::ErrorCode::ResourceExhausted) &&
              budget.statistics().live[ps::ResourceKind::Payload] == baseline,
          "public shaper budget/cancellation releases full intermediates");
    }
    auto registry = ps::make_default_operation_registry(false);
    ps::OperationDefinition producer;
    producer.key = "manual.shaper_input";
    producer.traits.input_count = 0;
    producer.traits.input_schema.clear();
    auto& output = producer.traits.outputs[0];
    output.output_schema.kind = ps::OperationPortKind::Result;
    output.output_schema.result_schema_id = fixture.values[0].schema().id;
    output.output_schema.result_schema_version = 1;
    output.result_schema = fixture.values[0].schema();
    output.key = "value";
    output.continuation_bytes = sizeof(FailingSource);
    output.maximum_dependency_stages = 1;
    producer.start_result = [](const auto&, const auto& allocator) {
      return ps::ResultContinuation::make<FailingSource>(allocator);
    };
    require(registry->register_operation(std::move(producer)).ok() &&
                registry->freeze().ok(),
            "shaper failing producer registry");
    fixture.registry = registry;
    for (auto& node : fixture.document.nodes)
      for (auto& input : node.inputs)
        if (const auto* ref = std::get_if<ps::WorkflowInputReference>(&input);
            ref && ref->input_id == 1)
          input = ps::WorkflowNodeOutput{100, "value"};
    fixture.document.nodes.push_back({100, "manual.shaper_input", {}, {}});
    fixture.document.inputs.erase(fixture.document.inputs.begin());
    fixture.values.erase(fixture.values.begin());
    auto failed = fixture.run(
        take(ps::Footprint::from_regions({4097}, {ps::Region({{0, 1}})})));
    require(!failed.ok() && failed.status().message == "required shaper source",
            "complete shaper source failure is preserved");
  }
  std::cout << "four public shapers: work/output budgets, active cancellation "
               "and upstream failure PASS\n";
}
void probe(ps::CpuNumericProfile profile) {
  unsigned method, type;
  std::string x, l, u;
  while (std::cin >> method >> type >> x >> l >> u) {
    require(method < 4 && (type == 3 || type == 4), "shaper probe framing");
    Fixture fixture(method, profile,
                    {array(type == 4, {std::stoull(x, nullptr, 16)}),
                     array(type == 4, {std::stoull(l, nullptr, 16)}),
                     array(type == 4, {std::stoull(u, nullptr, 16)})});
    auto result = fixture.run(take(ps::Footprint::all({1})));
    if (!result.ok()) {
      if (result.status().reason != ps::FailureReason::InvalidDomain)
        throw std::runtime_error(result.status().message);
      std::cout << "domain\n";
    } else {
      std::cout << std::hex << read(result.value().results.at("values"), {0})
                << std::dec << '\n';
    }
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "--probe") {
      probe(profile);
    } else {
      examples(profile);
      invalid_bounds(profile);
      resources(profile);
      partitions_and_cache(profile);
      typed_and_schema(profile);
      public_resources_and_upstream(profile);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
