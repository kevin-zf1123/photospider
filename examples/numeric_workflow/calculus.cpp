#include "photospider/numeric/calculus.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

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
  ps::ExecutionBindings bindings;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto& value = inputs[i];
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
    }
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
    auto snapshot = context.freeze(plan.value().plan, bindings);
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
      require(result.value()
                  .values.at("values")
                  .read({index}, &bits, ps::Value::element_size(dtype))
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
        require(result.values.at("values").read({j}, &bits, 8).ok() &&
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
  std::uint64_t bits = 0;
  require(answer.values.at("values").read({1}, &bits, 8).ok() &&
              bits == 0x4000000000000000,
          "unused center NaN ignored");
  auto stencil = take(ps::Footprint::from_regions(
      {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}));
  require(take(answer.dependencies.source_support()).at("input0") == stencil,
          "two-point exact stencil");
  require(take(answer.dependencies.potential_dirty("input0", center))
              .at("values")
              .empty(),
          "center outside derivative dependencies");
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
  auto raw = take(integral.run({{"values", zero}}));
  require(raw.values.at("values").read({0}, &bits, 8).ok() &&
              bits == 0x7ff0000000000042,
          "initial raw sNaN at zero");
  auto support = take(raw.dependencies.source_support());
  require((support.find("input0") == support.end() ||
           support.at("input0").empty()) &&
              (support.find("input1") == support.end() ||
               support.at("input1").empty()) &&
              support.at("input2") == take(ps::Footprint::all({1})),
          "initial-only support");
  ps::GraphContext graph(integral.document);
  auto plan = take(ps::Compiler(integral.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(integral.registry, config);
  auto atoms =
      take(context.execute_atoms(plan.plan, integral.bindings,
                                 {{"values", take(ps::Footprint::all({3}))}}));
  unsigned good = 0, bad = 0;
  for (const auto& atom : atoms.atoms) {
    if (atom.outcome.ok()) {
      ++good;
      require(atom.key.coordinate[0] == 0, "only zero ignores step");
    } else {
      ++bad;
      const auto& failure = atom.outcome.status();
      require(atom.key.coordinate[0] > 0 &&
                  failure.code == ps::ErrorCode::InvalidArgument &&
                  failure.reason == ps::FailureReason::InvalidDomain &&
                  failure.detail.origin == ps::FailureOrigin::Domain &&
                  failure.detail.scope == ps::FailureScope::Atom &&
                  failure.detail.atom == atom.key &&
                  failure.message.find("InvalidSampleStep: port=1 bits=0") !=
                      std::string::npos,
              "step atom attribution");
    }
  }
  require(good == 1 && bad == 2, "output-zero isolated from invalid step");
  std::cout << "sparse stencil/dirty, center NaN exclusion, raw initial-only "
               "boundary and invalid-step Atom isolation passed\n";
}
void failure_order(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry(false);
  std::array<unsigned, 2> calls{};
  for (unsigned kind = 0; kind < 2; ++kind) {
    ps::OperationDefinition failure;
    failure.key = kind ? "manual.calculus_step" : "manual.calculus_samples";
    failure.traits.input_count = 0;
    failure.traits.input_schema.clear();
    failure.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
    failure.traits.outputs[0].fixed_output_shape = {kind ? 1U : 3U};
    failure.traits.outputs[0].output_element_type = Type::Float64;
    failure.callback = [&, kind](const auto&) {
      ++calls[kind];
      return ps::Result<ps::Value>(ps::Status{
          ps::ErrorCode::OperationFailed,
          kind ? "required step producer" : "required samples producer"});
    };
    require(registry->register_operation(std::move(failure)).ok(),
            "failure source registration");
  }
  require(registry->freeze().ok(), "failure registry freeze");
  Fixture initial_only(
      authored(true, profile),
      {array(Type::Float64, {3}, {0, 0, 0}), array(Type::Float64, {1}, {0}),
       array(Type::Float64, {1}, {0x8000000000000000})});
  initial_only.registry = registry;
  initial_only.document.inputs.erase(initial_only.document.inputs.begin(),
                                     initial_only.document.inputs.begin() + 2);
  initial_only.bindings.inputs.erase(initial_only.bindings.inputs.begin(),
                                     initial_only.bindings.inputs.begin() + 2);
  initial_only.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "value"};
  initial_only.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{3, "value"};
  initial_only.document.nodes.push_back({2, "manual.calculus_samples", {}, {}});
  initial_only.document.nodes.push_back({3, "manual.calculus_step", {}, {}});
  auto zero =
      take(initial_only.run({{"values", take(ps::Footprint::from_regions(
                                            {3}, {ps::Region({{0, 1}})}))}}));
  std::uint64_t bits = 0;
  require(zero.values.at("values").read({0}, &bits, 8).ok() &&
              bits == 0x8000000000000000 &&
              calls == std::array<unsigned, 2>{0, 0},
          "zero skips failing sample/step producers");
  for (bool integral : {false, true}) {
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
      fixture.bindings.inputs.erase(fixture.bindings.inputs.begin());
      fixture.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "value"};
      fixture.document.nodes.push_back({2, "manual.calculus_samples", {}, {}});
      const auto prior = calls[0];
      auto answer = fixture.run(
          {{"values",
            take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}))}});
      require(!answer.ok(), "required failure");
      require(valid ? answer.status().message == "required samples producer" &&
                          calls[0] == prior + 1
                    : answer.status().message.find("InvalidSampleStep") !=
                              std::string::npos &&
                          calls[0] == prior,
              "step before samples, initial NaN still reads samples");
    }
  }
  std::cout << "zero skips failing producers; invalid step precedes samples; "
               "valid step retains upstream failure after initial NaN\n";
}

void strides_and_resources(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
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
    std::vector<ps::Region> regions;
    for (const auto& value : inputs)
      regions.push_back(value.region());
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save calculus fenv");
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "calculus fenv prepare");
      ps::OperationInvocation invocation(inputs, regions, node.parameters,
                                         ps::Backend::Cpu, {},
                                         ps::Region::whole({3}));
      auto result = take(registry->invoke(node.operation, invocation));
      const std::vector<std::uint64_t> expected =
          integral
              ? std::vector<std::uint64_t>{0, 0xbff8000000000000,
                                           0xc000000000000000}
              : std::vector<std::uint64_t>{
                    0x4008000000000000, 0x4000000000000000, 0x3ff0000000000000};
      for (unsigned j = 0; j < 3; ++j) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, result.bytes().data() + j * 8, 8);
        require(bits == expected[j], "negative source/control strides");
      }
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "calculus environment unchanged");
    }
    require(fesetenv(&saved) == 0, "restore calculus fenv");
    ps::DependencyRequest request;
    for (const auto& value : inputs)
      request.inputs.push_back({value.descriptor(), {}});
    request.snapshot_identity = "calculus-resource";
    request.outputs = take(ps::Footprint::none({3}));
    auto empty = take(registry->start_dependency(node.operation, request));
    require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
                empty->poll_count() == 0,
            "empty calculus no reads");
    request.outputs =
        take(ps::Footprint::from_regions({3}, {ps::Region({{2, 1}})}));
    for (bool cancel : {false, true}) {
      ps::ResourceBudget resources(ps::ResourceLimits{});
      ps::CancellationSource cancellation;
      request.cancellation = cancellation.token();
      bool armed = false, interrupted = false;
      std::shared_ptr<ps::DependencySession> session;
      session = take(registry->start_dependency(
          node.operation, request, resources.allocator(),
          [&](std::uint64_t amount) {
            if (armed && amount == (integral ? 512U : 1024U) &&
                session->numeric_diagnostics().evaluated_values == 1) {
              interrupted = true;
              if (cancel)
                cancellation.cancel();
              else
                return ps::Status{ps::ErrorCode::ResourceExhausted,
                                  "calculus arithmetic work",
                                  ps::FailureReason::WorkLimit};
            }
            return ps::Status::success();
          }));
      for (unsigned stage = 0; stage < 2; ++stage) {
        require(session->poll().ok(), "calculus Need before interrupt");
        std::vector<ps::Footprint> wanted;
        for (const auto& input : request.inputs)
          wanted.push_back(take(ps::Footprint::none(input.descriptor.shape)));
        for (const auto& need : take(session->pending_reads()))
          wanted[need.port] = take(wanted[need.port].unite(need.samples));
        std::vector<ps::ValueFragments> supplied;
        for (unsigned port = 0; port < inputs.size(); ++port) {
          auto all = take(ps::ValueFragments::create(
              inputs[port].descriptor(), {},
              take(ps::Footprint::all(inputs[port].descriptor().shape)),
              {inputs[port]}));
          supplied.push_back(take(all.restrict(wanted[port])));
        }
        require(session->supply(supplied, request.snapshot_identity).ok(),
                "calculus supply");
      }
      armed = true;
      auto failed = session->poll();
      require(interrupted && !failed.ok() &&
                  failed.status().code ==
                      (cancel ? ps::ErrorCode::Cancelled
                              : ps::ErrorCode::ResourceExhausted),
              "calculus arithmetic interrupt");
      require(session->numeric_diagnostics().evaluated_values == 1 &&
                  session->numeric_diagnostics().copied_elements == 0,
              "calculus failed attempt counters");
      session.reset();
      require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
              "calculus resource release");
    }
    request.cancellation = {};
    for (unsigned kind = 0; kind < 3; ++kind) {
      auto bad = request;
      if (kind == 0)
        bad.inputs[0].descriptor.shape = {integral ? 0U : 1U};
      if (kind == 1)
        bad.inputs[1].descriptor.element_type = Type::Float32;
      if (kind == 2)
        bad.inputs[1].descriptor.shape = {2};
      auto result = registry->start_dependency(node.operation, bad);
      require(
          !result.ok() && result.status().code == ps::ErrorCode::TypeMismatch,
          "calculus schema");
    }
  }
  std::cout << "negative source/control strides and fenv, Empty/schema, "
               "arithmetic WorkLimit/cancellation and payload release passed\n";
}
void streaming(ps::CpuNumericProfile profile) {
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
  for (std::uint64_t index : {0, 64, 129, 4095}) {
    double value = 0;
    require(result.values.at("values").read({index}, &value, 8).ok() &&
                value == static_cast<double>(index + 2),
            "streamed cumulative integral");
  }
  auto changed =
      take(ps::Footprint::from_regions({4096}, {ps::Region({{64, 1}})}));
  auto positive = take(ps::Footprint::from_regions(
      {4096}, {ps::Region({{64, 1}}), ps::Region({{129, 1}}),
               ps::Region({{4095, 1}})}));
  require(take(result.dependencies.potential_dirty("input0", changed))
                  .at("values") == positive,
          "inclusive integral suffix dirty");
  require(take(result.dependencies.potential_dirty(
                   "input1", take(ps::Footprint::all({1}))))
                  .at("values") == positive,
          "step only positive outputs");
  require(take(result.dependencies.potential_dirty(
                   "input2", take(ps::Footprint::all({1}))))
                  .at("values") == demand,
          "initial all outputs");
  ps::DependencyRequest request;
  for (const auto& binding : fixture.bindings.inputs)
    request.inputs.push_back({binding.value.descriptor(), {}});
  request.snapshot_identity = "integral-streaming";
  request.outputs = demand;
  request.limits.maximum_work = 128 * 1024 * 1024;
  ps::ResourceBudget resources(ps::ResourceLimits{});
  auto session = take(fixture.registry->start_dependency(
      fixture.document.nodes[0].operation, request, resources.allocator()));
  std::uint64_t next = 0, windows = 0;
  while (true) {
    auto progress = take(session->poll());
    if (std::holds_alternative<ps::DependencyResult>(progress))
      break;
    std::vector<ps::Footprint> wanted;
    for (const auto& input : request.inputs)
      wanted.push_back(take(ps::Footprint::none(input.descriptor.shape)));
    for (const auto& need : take(session->pending_reads())) {
      wanted[need.port] = take(wanted[need.port].unite(need.samples));
      if (need.port == 0 && (need.roles & 1)) {
        require(need.samples.boxes().size() == 1 &&
                    need.samples.boxes()[0].dimensions()[0].offset == next,
                "integral ordered windows");
        const auto count = take(need.samples.element_count());
        require(count <= 64, "bounded integral window");
        next += count;
        ++windows;
      }
    }
    std::vector<ps::ValueFragments> supplied;
    for (unsigned port = 0; port < request.inputs.size(); ++port) {
      const auto& input = fixture.bindings.inputs[port].value;
      auto all = take(ps::ValueFragments::create(
          input.descriptor(), {},
          take(ps::Footprint::all(input.descriptor().shape)), {input}));
      supplied.push_back(take(all.restrict(wanted[port])));
    }
    require(session->supply(supplied, request.snapshot_identity).ok(),
            "integral window supply");
  }
  require(next == 4096 && windows == 66 &&
              session->numeric_diagnostics().evaluated_values == 4096,
          "integral scans source once");
  std::cout << "4096 inputs, four sparse integral outputs, 66 bounded windows "
               "and exact sample/step/initial dirty passed\n";
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
      streaming(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
