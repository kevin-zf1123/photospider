#include "photospider/numeric/calculus.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
namespace rf = numeric_result_fixture;
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
ps::Value array(ps::ElementType type, const std::vector<std::uint64_t>& shape,
                const std::vector<std::uint64_t>& bits) {
  const auto width = ps::Value::element_size(type);
  auto buffer = take(ps::BufferAllocator{}.allocate(bits.size() * width));
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(buffer.data() + i * width, &bits[i], width);
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = width;
  for (std::size_t j = shape.size(); j; --j) {
    strides[j - 1] = stride;
    stride *= shape[j - 1];
  }
  return take(ps::Value::from_storage({type, shape}, ps::Region::whole(shape),
                                      {0, strides},
                                      std::move(buffer).freeze()));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs)
      : backing(inputs) {
    rf::declare_sources(&document, inputs);
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::Result<ps::DemandResult> run(const ps::DemandQuery& query,
                                   bool cache = true,
                                   std::uint64_t proof_work = UINT64_C(64) *
                                                              1024 * 1024,
                                   std::uint64_t cache_bytes = 65536) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 262144;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot =
        context.freeze(plan.value().plan,
                       point_math_checks::bindings(
                           take(context.resource_budget()), backing, document));
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(128) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(16) * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, {}, options);
  }
};
std::vector<std::uint64_t> list(const std::string& text) {
  std::vector<std::uint64_t> result;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find(',', begin);
    result.push_back(std::stoull(text.substr(begin, end - begin)));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return result;
}
ps::WorkflowNode authored(bool integral, ps::CpuNumericProfile profile) {
  return integral ? take(ps::numeric::integrate_1d_node(
                        1, ps::WorkflowInputReference{1},
                        ps::WorkflowInputReference{2},
                        ps::WorkflowInputReference{3}, profile))
                  : take(ps::numeric::derivative_1d_node(
                        1, ps::WorkflowInputReference{1},
                        ps::WorkflowInputReference{2}, profile));
}
void oracle(ps::CpuNumericProfile profile) {
  unsigned integral = 0, type = 0;
  std::uint64_t size = 0, index = 0, step = 0, initial = 0;
  while (std::cin >> integral >> type >> size >> index >> std::hex >> step >>
         initial >> std::dec) {
    std::vector<std::uint64_t> samples(size);
    for (auto& value : samples)
      std::cin >> std::hex >> value >> std::dec;
    const auto dtype = static_cast<ps::ElementType>(type);
    std::vector<ps::Value> inputs{array(dtype, {size}, samples),
                                  array(dtype, {1}, {step})};
    if (integral)
      inputs.push_back(array(dtype, {1}, {initial}));
    Fixture fixture(authored(integral != 0, profile), inputs);
    auto demand =
        take(ps::Footprint::from_regions({size}, {ps::Region({{index, 1}})}));
    auto result = fixture.run({{"values", demand}});
    if (!result.ok()) {
      if (result.status().message.find("InvalidSampleStep") !=
          std::string::npos)
        std::cout << "step\n";
      else
        throw std::runtime_error(result.status().message);
    } else {
      std::uint64_t bits = 0;
      require(rf::read(result.value().results.at("values"), {index}, &bits,
                       ps::Value::element_size(dtype))
                  .ok(),
              "calculus oracle read");
      std::cout << std::hex << bits << std::dec << '\n';
    }
  }
}
void examples(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  for (bool integral : {false, true}) {
    auto samples =
        array(Type::Float64, {3},
              integral ? std::vector<std::uint64_t>{0, 0x3ff0000000000000,
                                                    0x4000000000000000}
                       : std::vector<std::uint64_t>{0, 0x3ff0000000000000,
                                                    0x4010000000000000});
    std::vector<ps::Value> inputs{
        samples, array(Type::Float64, {1}, {0x3ff0000000000000})};
    if (integral)
      inputs.push_back(array(Type::Float64, {1}, {0}));
    Fixture fixture(authored(integral, profile), inputs);
    auto all = take(ps::Footprint::all({3}));
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0, "set rounding");
      auto result = take(fixture.run({{"values", all}}, false));
      const auto expected =
          integral
              ? std::vector<std::uint64_t>{0, 0x3fe0000000000000,
                                           0x4000000000000000}
              : std::vector<std::uint64_t>{
                    0x3ff0000000000000, 0x4000000000000000, 0x4008000000000000};
      for (unsigned j = 0; j < 3; ++j) {
        std::uint64_t bits = 0;
        require(rf::read(result.results.at("values"), {j}, &bits, 8).ok() &&
                    bits == expected[j],
                "calculus fixture/lifetime");
      }
      require(fegetround() == mode, "calculus rounding unchanged");
    }
  }
  require(fesetround(FE_TONEAREST) == 0, "restore rounding");
  std::cout << "derivative [0,1,4], step1 -> [1,2,3]; integral [0,1,2], step1 "
               "initial0 -> [0,0.5,2]\n";
}

void sparse_and_step(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  Fixture derivative(
      authored(false, profile),
      {array(Type::Float64, {3}, {0, 0x7ff0000000000042, 0x4010000000000000}),
       array(Type::Float64, {1}, {0x3ff0000000000000})});
  auto center = take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}));
  auto answer = take(derivative.run({{"values", center}}));
  require(take(answer.results.at("values").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all({3})),
          "sparse derivative Result retains complete Whole coverage");
  std::uint64_t bits = 0;
  require(rf::read(answer.results.at("values"), {1}, &bits, 8).ok() &&
              bits == 0x4000000000000000,
          "unused center NaN ignored");
  require(
      take(answer.dependencies.source_support()).at("input0") ==
          take(ps::Footprint::all({3})),
      "Whole derivative reads full samples while numerically excluding center");
  require(take(answer.dependencies.potential_dirty("input0", center))
                  .at("values") == center,
          "Whole center edit invalidates recorded output");
  require(take(answer.dependencies.potential_dirty(
                   "input1", take(ps::Footprint::all({1}))))
                  .at("values") == center,
          "step invalidates derivative");
  Fixture integral(
      authored(true, profile),
      {array(Type::Float64, {3}, {0, 0x3ff0000000000000, 0x4000000000000000}),
       array(Type::Float64, {1}, {0}),
       array(Type::Float64, {1}, {0x7ff0000000000042})});
  auto zero = take(ps::Footprint::from_regions({3}, {ps::Region({{0, 1}})}));
  auto failed = integral.run({{"values", zero}});
  require(!failed.ok() &&
              failed.status().message.find("InvalidSampleStep") !=
                  std::string::npos &&
              failed.status().detail.scope == ps::FailureScope::Run,
          "invalid step fails even initial-only projection for N>1");
  integral.backing[1] = array(Type::Float64, {1}, {0x3ff0000000000000});
  auto raw = take(integral.run({{"values", zero}}));
  require(rf::read(raw.results.at("values"), {0}, &bits, 8).ok() &&
              bits == 0x7ff0000000000042,
          "output zero preserves raw initial sNaN after full preparation");
  require(take(raw.dependencies.source_support()).at("input0") ==
              take(ps::Footprint::all({3})),
          "initial projection retains full source support");
  std::cout << "Whole support/dirty, center NaN arithmetic exclusion, raw "
               "initial and Run step failure passed\n";
}
struct FailedSource final {
  unsigned* calls;
  bool step;
  FailedSource(unsigned* count, bool is_step) : calls(count), step(is_step) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(ps::Status{
        ps::ErrorCode::OperationFailed,
        step ? "required step producer" : "required samples producer"});
  }
};
void failure_order(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry(false);
  std::array<unsigned, 2> calls{};
  for (unsigned kind = 0; kind < 3; ++kind) {
    ps::OperationDefinition failure;
    failure.key = kind == 0   ? "manual.calculus_samples"
                  : kind == 1 ? "manual.calculus_step"
                              : "manual.calculus_single";
    failure.traits.input_count = 0;
    failure.traits.input_schema.clear();
    auto& output = failure.traits.outputs[0];
    output.region_rule = ps::OperationRegionRule::Whole;
    output.output_schema.kind = ps::OperationPortKind::Result;
    auto schema = rf::source_schema(
        array(Type::Float64, {kind == 0 ? 3U : 1U},
              std::vector<std::uint64_t>(kind == 0 ? 3U : 1U)));
    output.output_schema.result_schema_id = schema.id;
    output.output_schema.result_schema_version = schema.version;
    output.result_schema = std::move(schema);
    output.continuation_bytes = sizeof(FailedSource);
    output.maximum_dependency_stages = 8;
    failure.start_result = [&, kind](const auto&, const auto& allocator) {
      return ps::ResultContinuation::make<FailedSource>(
          allocator, &calls[kind == 1 ? 1 : 0], kind == 1);
    };
    require(registry->register_operation(std::move(failure)).ok(),
            "register calculus failed producers");
  }
  require(registry->freeze().ok(), "freeze calculus failure registry");
  for (bool singleton : {true, false}) {
    auto count = singleton ? 1U : 3U;
    Fixture fixture(
        authored(true, profile),
        {array(Type::Float64, {count}, std::vector<std::uint64_t>(count)),
         array(Type::Float64, {1}, {0}),
         array(Type::Float64, {1}, {0xfff0000000000042})});
    fixture.registry = registry;
    fixture.document.inputs.erase(fixture.document.inputs.begin(),
                                  fixture.document.inputs.begin() + 2);
    fixture.backing.erase(fixture.backing.begin(), fixture.backing.begin() + 2);
    fixture.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "value"};
    fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{3, "value"};
    fixture.document.nodes.push_back(
        {2,
         singleton ? "manual.calculus_single" : "manual.calculus_samples",
         {},
         {}});
    fixture.document.nodes.push_back({3, "manual.calculus_step", {}, {}});
    calls = {};
    auto empty =
        take(fixture.run({{"values", take(ps::Footprint::none({count}))}}));
    require(
        calls == std::array<unsigned, 2>{0, 0} &&
            take(empty.results.at("values").descriptor())
                .tensor_coverage(0)
                .empty(),
        "Empty calculus excludes failed producers and publishes no samples");
    auto result = fixture.run(
        {{"values",
          take(ps::Footprint::from_regions({count}, {ps::Region({{0, 1}})}))}});
    if (singleton) {
      auto answer = take(std::move(result));
      std::uint64_t bits = 0;
      require(rf::read(answer.results.at("values"), {0}, &bits, 8).ok() &&
                  bits == 0xfff0000000000042 &&
                  calls == std::array<unsigned, 2>{0, 0},
              "N1 excludes failed samples/step and copies raw initial");
      const auto support = take(answer.dependencies.source_support());
      require(support.size() == 1 &&
                  support.at("input2") == take(ps::Footprint::all({1})),
              "N1 records only initial support");
    } else {
      require(!result.ok() && calls[0] > 0,
              "N>1 zero projection still reads complete samples");
    }
  }
  for (bool integral : {false, true})
    for (bool valid : {false, true}) {
      std::vector<ps::Value> inputs{
          array(Type::Float64, {3}, {0, 0, 0}),
          array(Type::Float64, {1},
                {valid ? UINT64_C(0x3ff0000000000000) : 0})};
      if (integral)
        inputs.push_back(array(Type::Float64, {1}, {0x7ff0000000000042}));
      Fixture fixture(authored(integral, profile), inputs);
      fixture.registry = registry;
      fixture.document.inputs.erase(fixture.document.inputs.begin());
      fixture.backing.erase(fixture.backing.begin());
      fixture.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "value"};
      fixture.document.nodes.push_back({2, "manual.calculus_samples", {}, {}});
      const auto previous = calls[0];
      auto result = fixture.run({{"values", take(ps::Footprint::all({3}))}});
      require(!result.ok() &&
                  result.status().message == "required samples producer" &&
                  calls[0] == previous + 1,
              "Whole source error precedes step callback validation and "
              "initial NaN");
    }
  std::cout << "singleton projection and Whole eager-source failure priority "
               "passed\n";
}

void strides_and_resources(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  for (bool integral : {false, true}) {
    auto node = authored(integral, profile);
    auto samples =
        array(Type::Float64, {3},
              integral ? std::vector<std::uint64_t>{0, 0x3ff0000000000000,
                                                    0x4000000000000000}
                       : std::vector<std::uint64_t>{0, 0x3ff0000000000000,
                                                    0x4010000000000000});
    auto control = array(Type::Float64, {2}, {0, 0xbff0000000000000});
    std::vector<ps::Value> inputs{
        take(ps::Value::from_storage(samples.descriptor(), samples.region(),
                                     {16, {-8}}, samples.storage())),
        take(ps::Value::from_storage({Type::Float64, {1}},
                                     ps::Region::whole({1}), {8, {-8}},
                                     control.storage()))};
    if (integral)
      inputs.push_back(take(
          ps::Value::from_storage({Type::Float64, {1}}, ps::Region::whole({1}),
                                  {0, {-8}}, control.storage())));
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save calculus fenv");
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "calculus fenv prepare");
      auto checked = std::make_shared<point_math_checks::Control>();
      checked->rounding = mode;
      point_math_checks::Workflow workflow(node, inputs, {}, checked);
      auto result = take(workflow.run()).results.at("values");
      const std::vector<std::uint64_t> expected =
          integral
              ? std::vector<std::uint64_t>{0, 0xbff8000000000000,
                                           0xc000000000000000}
              : std::vector<std::uint64_t>{
                    0x4008000000000000, 0x4000000000000000, 0x3ff0000000000000};
      for (unsigned j = 0; j < 3; ++j) {
        std::uint64_t bits = 0;
        require(rf::read(result, {j}, &bits, 8).ok(), "strided Result read");
        require(bits == expected[j], "negative source/control strides");
      }
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "calculus environment unchanged");
    }
    require(fesetenv(&saved) == 0, "restore calculus fenv");
    Fixture empty(node,
                  {samples, array(Type::Float64, {1}, {0xbff0000000000000})});
    if (integral) {
      empty = Fixture(node,
                      {samples, array(Type::Float64, {1}, {0xbff0000000000000}),
                       array(Type::Float64, {1}, {0})});
    }
    require(empty.run({{"values", take(ps::Footprint::none({3}))}}).ok(),
            "Empty calculus skips inputs");
    ps::CancellationSource stopped;
    stopped.cancel();
    point_math_checks::Workflow cancelled(node, inputs);
    require(cancelled.run(stopped.token()).status().code ==
                ps::ErrorCode::Cancelled,
            "calculus pre-cancelled");
    auto large = array(Type::Float64, {16384},
                       std::vector<std::uint64_t>(16384, 0x3ff0000000000000));
    std::vector<ps::Value> large_inputs{
        large, array(Type::Float64, {1}, {0x3ff0000000000000})};
    if (integral)
      large_inputs.push_back(array(Type::Float64, {1}, {0}));
    point_math_checks::resources(node, large_inputs);
    for (unsigned kind = 0; kind < 3; ++kind) {
      Fixture bad(node, inputs);
      auto schema = *bad.document.inputs[kind == 0 ? 0 : 1].result_schema;
      if (kind == 0)
        schema.tensors[0].descriptor.shape = {integral ? 0U : 1U};
      if (kind == 1)
        schema.tensors[0].descriptor.element_type = Type::Float32;
      if (kind == 2)
        schema.tensors[0].descriptor.shape = {2};
      bad.document.inputs[kind == 0 ? 0 : 1].result_schema =
          std::make_shared<ps::SchemaTemplate>(std::move(schema));
      auto result = bad.run({{"values", take(ps::Footprint::all({3}))}});
      require(
          !result.ok() && result.status().code == ps::ErrorCode::TypeMismatch,
          "calculus Result schema");
    }
  }
  for (bool integral : {false, true})
    for (bool zero : {false, true}) {
      auto node = authored(integral, profile);
      const double raw[] = {0, 1, 2};
      auto samples = take(ps::BufferAllocator{}.allocate(25));
      std::memcpy(samples.data() + 1, raw, 24);
      auto source = take(ps::Value::from_storage(
          {Type::Float64, {3}}, ps::Region::whole({3}), {17, {zero ? 0 : -8}},
          std::move(samples).freeze()));
      auto control = take(ps::BufferAllocator{}.allocate(9));
      const double one = 1;
      std::memcpy(control.data() + 1, &one, 8);
      auto step = take(ps::Value::from_storage({Type::Float64, {1}},
                                               ps::Region::whole({1}), {1, {0}},
                                               std::move(control).freeze()));
      std::vector<ps::Value> inputs{source, step};
      if (integral)
        inputs.push_back(array(Type::Float64, {1}, {0}));
      point_math_checks::Workflow workflow(node, inputs);
      auto result = take(workflow.run()).results.at("values");
      for (unsigned i = 0; i < 3; ++i) {
        double actual = 0;
        require(rf::read(result, {i}, &actual, 8).ok(),
                "unaligned Result read");
        const double expected = integral ? (zero     ? 2 * i
                                            : i == 0 ? 0
                                            : i == 1 ? 1.5
                                                     : 2)
                                         : (zero ? 0 : -1);
        require(actual == expected,
                "unaligned negative/zero source and control layout");
      }
    }
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  for (bool integral : {false, true}) {
    const auto raw = array(Type::Float32, {3}, {0, 0, 0});
    std::vector<ps::Value> inputs{raw, array(Type::Float32, {1}, {0x3f800000})};
    if (integral)
      inputs.push_back(array(Type::Float32, {1}, {0}));
    Fixture bad(authored(integral, profile), inputs);
    auto schema = *bad.document.inputs[0].result_schema;
    schema.tensors[0].facets = {facet};
    bad.document.inputs[0].result_schema =
        std::make_shared<ps::SchemaTemplate>(std::move(schema));
    require(!bad.run({{"values", take(ps::Footprint::all({3}))}}).ok(),
            "incompatible recognized typed metadata rejects rank-one calculus");
  }
  std::cout << "negative source/control strides and fenv, Empty/schema, "
               "arithmetic WorkLimit/cancellation and payload release passed\n";
}
void sparse_integral(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  Fixture fixture(authored(true, profile),
                  {array(Type::Float64, {4096},
                         std::vector<std::uint64_t>(4096, 0x3ff0000000000000)),
                   array(Type::Float64, {1}, {0x3ff0000000000000}),
                   array(Type::Float64, {1}, {0x4000000000000000})});
  auto demand = take(ps::Footprint::from_regions(
      {4096}, {ps::Region({{0, 1}}), ps::Region({{64, 1}}),
               ps::Region({{129, 1}}), ps::Region({{4095, 1}})}));
  auto result = take(fixture.run({{"values", demand}}, false));
  require(take(result.results.at("values").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all({4096})),
          "sparse integral Result retains complete Whole coverage");
  for (std::uint64_t index : {0, 64, 129, 4095}) {
    double value = 0;
    require(rf::read(result.results.at("values"), {index}, &value, 8).ok() &&
                value == static_cast<double>(index + 2),
            "sparse cumulative integral");
  }
  auto changed =
      take(ps::Footprint::from_regions({4096}, {ps::Region({{64, 1}})}));
  require(take(result.dependencies.potential_dirty("input0", changed))
                  .at("values") == demand,
          "inclusive integral suffix dirty");
  require(take(result.dependencies.potential_dirty(
                   "input1", take(ps::Footprint::all({1}))))
                  .at("values") == demand,
          "step only positive outputs");
  require(take(result.dependencies.potential_dirty(
                   "input2", take(ps::Footprint::all({1}))))
                  .at("values") == demand,
          "initial all outputs");
  std::cout << "4096 inputs and complete integral output, sparse projection "
               "and Whole dirty passed\n";
}
void retained_output(ps::CpuNumericProfile profile) {
  for (bool integral : {false, true}) {
    ps::ResourceBudget root;
    ps::ResultRef output;
    ps::ResultTensorReadWindow window;
    std::weak_ptr<const ps::CpuStorage> input_owner;
    {
      std::vector<ps::Value> inputs{
          array(ps::ElementType::Float64, {3},
                {0, 0x3ff0000000000000, 0x4000000000000000}),
          array(ps::ElementType::Float64, {1}, {0x3ff0000000000000})};
      if (integral)
        inputs.push_back(array(ps::ElementType::Float64, {1}, {0}));
      input_owner = inputs[0].storage();
      point_math_checks::Workflow workflow(authored(integral, profile), inputs);
      root = workflow.root;
      output = take(workflow.run()).results.at("values");
      window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                          ps::Region::whole({3})));
    }
    require(input_owner.expired(), "calculus output retires source backing");
    require(root.statistics().live[ps::ResourceKind::Payload] == 24,
            "escaped calculus Result/window share one output owner");
    double number = 0;
    require(
        rf::read(output, {1}, &number, 8).ok() && number == (integral ? .5 : 1),
        "calculus Result survives source and context retirement");
    output = {};
    const auto row = take(window.row_run({2}));
    std::memcpy(&number, row.data, 8);
    require(number == (integral ? 2 : 1) &&
                root.statistics().live[ps::ResourceKind::Payload] == 24,
            "authorized calculus window survives Result release");
    window = {};
    point_math_checks::released(root);
  }
  std::cout << "source retirement, escaped Result/window, one Payload owner "
               "and final all-Root release passed\n";
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
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      examples(profile);
      sparse_and_step(profile);
      failure_order(profile);
      strides_and_resources(profile);
      sparse_integral(profile);
      retained_output(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
