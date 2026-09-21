#include "photospider/numeric/shapers.hpp"

#include <array>
#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/numeric/color_ramps.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

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
ps::Value array(bool narrow, const std::vector<std::uint64_t>& words) {
  const unsigned width = narrow ? 4 : 8;
  std::vector<std::uint8_t> bytes(width * words.size());
  for (std::size_t i = 0; i < words.size(); ++i)
    std::memcpy(bytes.data() + i * width, &words[i], width);
  return take(ps::Value::create(
      {narrow ? ps::ElementType::Float32 : ps::ElementType::Float64,
       {words.size()}},
      ps::Region::whole({words.size()}), {0, {width}}, std::move(bytes)));
}
ps::Value numbers(bool narrow, const std::vector<double>& values) {
  std::vector<std::uint64_t> words;
  for (auto value : values)
    words.push_back(bits(value, narrow));
  return array(narrow, words);
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  Fixture(unsigned method, ps::CpuNumericProfile profile,
          const std::vector<ps::Value>& values) {
    for (unsigned i = 0; i < values.size(); ++i) {
      const auto& value = values[i];
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
    }
    const ps::WorkflowInput x = ps::WorkflowInputReference{1},
                            l = ps::WorkflowInputReference{2},
                            u = ps::WorkflowInputReference{3};
    ps::WorkflowNodeOutput output;
    if (method < 2) {
      auto helper = method ? ps::numeric::linear_shaper_inverse
                           : ps::numeric::linear_shaper;
      output = take(helper(document, x, l, u, values[0].descriptor(), profile));
    } else {
      auto helper = method == 3 ? ps::numeric::log2_shaper_inverse_node
                                : ps::numeric::log2_shaper_node;
      document.nodes.push_back(take(helper(1, x, l, u, profile)));
      output = {1, "values"};
    }
    document.outputs = {{"values", output.source_node, output.source_port}};
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
    auto frozen = context.freeze(compiled.value().plan, bindings);
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
std::uint64_t read(const ps::ValueFragments& value,
                   const std::vector<std::uint64_t>& at) {
  std::uint64_t raw = 0;
  require(value
              .read(at, &raw,
                    ps::Value::element_size(value.descriptor().element_type))
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
        const auto& output = result.values.at("values");
        require(output.facets().empty(), "generic shaper result");
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
          read(result.values.at("values"), {1}) == bits(expected[1], narrow),
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
ps::Value direct(const std::shared_ptr<ps::OperationRegistry>& registry,
                 const ps::WorkflowNode& node,
                 const std::vector<ps::Value>& inputs) {
  std::vector<ps::Region> demands;
  for (const auto& input : inputs)
    demands.push_back(input.region());
  ps::ResourceBudget budget(ps::ResourceLimits{});
  ps::ResourceAllocationScope scope(budget);
  ps::OperationInvocation call(inputs, demands, node.parameters,
                               ps::Backend::Cpu, {}, inputs[0].region(),
                               budget.allocator());
  return take(registry->invoke(node.operation, call));
}
ps::Value reversed(const ps::Value& value) {
  const unsigned width =
      ps::Value::element_size(value.descriptor().element_type);
  const auto count = value.bytes().size() / width;
  std::vector<std::uint8_t> bytes(1 + value.bytes().size());
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(bytes.data() + 1 + (count - i - 1) * width,
                value.bytes().data() + i * width, width);
  auto strides = value.layout().byte_strides;
  for (auto& stride : strides)
    stride = -stride;
  return take(ps::Value::create(value.descriptor(), value.region(),
                                {1 + (count - 1) * width, strides},
                                std::move(bytes), value.facets()));
}
void resources(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (unsigned method : {2U, 3U}) {
    Fixture fixture(
        method, profile,
        {numbers(false, {2, 3, 4}), numbers(false, {1}), numbers(false, {16})});
    const auto operation = fixture.document.nodes[0].operation;
    std::vector<ps::Value> dense;
    for (const auto& binding : fixture.bindings.inputs)
      dense.push_back(binding.value);
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
        auto output = direct(registry, fixture.document.nodes[0], values);
        std::uint64_t actual = 0;
        std::memcpy(&actual, output.bytes().data(), 8);
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
    point_math_checks::resources(fixture.document.nodes[0], dense);
    auto scalar = numbers(false, {method == 2 ? 4. : .5});
    dense[0] = take(ps::Value::from_storage({ps::ElementType::Float64, {2, 2}},
                                            ps::Region::whole({2, 2}),
                                            {0, {0, 0}}, scalar.storage()));
    auto broadcast = direct(registry, fixture.document.nodes[0], dense);
    const auto expected = bits(method == 2 ? .5 : 4.);
    for (unsigned i = 0; i < 4; ++i) {
      std::uint64_t actual = 0;
      std::memcpy(&actual, broadcast.bytes().data() + i * 8, 8);
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
  const auto hint = numbers(false, {1, 2}).descriptor();
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
    const auto& output = whole.values.at("values");
    double previous = -1e300;
    for (unsigned j = 0; j < samples.size(); ++j) {
      const auto i = samples.size() - j - 1;
      auto partial =
          take(fixture.run(take(ps::Footprint::from_regions(
                               {samples.size()}, {ps::Region({{i, 1}})})),
                           false));
      require(read(partial.values.at("values"), {i}) == read(output, {i}),
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
    auto demand = take(context.open_demand(compiled.plan, fixture.bindings));
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(1) << 30;
    options.dependencies.maximum_work = UINT64_C(1) << 30;
    take(demand.request({{"values", one}}, {}, options));
    require(take(demand.request({{"values", one}}, {}, options))
                    .diagnostics.cache_hits > 0,
            "shaper warm result cache");
    for (unsigned port : {1U, 2U}) {
      fixture.bindings.inputs[port].value =
          numbers(false, {port == 1 ? .5 : 32.});
      require(demand.replace_bindings(fixture.bindings).ok(),
              "replace shaper bound");
      auto changed = take(demand.request({{"values", one}}, {}, options));
      auto fresh = take(fixture.run(one, false));
      require(read(changed.values.at("values"), {1}) ==
                      read(fresh.values.at("values"), {1}) &&
                  read(changed.values.at("values"), {1}) != read(output, {1}),
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
  auto input = numbers(false, {2, 3, 4});
  input = take(ps::Value::from_storage({ps::ElementType::Float64, {1, 3}},
                                       ps::Region::whole({1, 3}), {0, {24, 8}},
                                       input.storage(), {facet}));
  for (unsigned method = 0; method < 4; ++method) {
    Fixture fixture(method, profile,
                    {input, numbers(false, {1}), numbers(false, {16})});
    auto wanted = take(
        ps::Footprint::from_regions({1, 3}, {ps::Region({{0, 1}, {1, 1}})}));
    auto result = take(fixture.run(wanted));
    require(result.values.at("values").coverage() == wanted &&
                result.values.at("values").facets().empty() &&
                take(result.dependencies.source_support()).at("input0") ==
                    take(ps::Footprint::all({1, 3})),
            "generic local output with full typed input validation closure");
    auto invalid =
        array(false, {bits(2), bits(3), UINT64_C(0x7ff8000000000042)});
    fixture.bindings.inputs[0].value = take(
        ps::Value::from_storage(input.descriptor(), input.region(),
                                input.layout(), invalid.storage(), {facet}));
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
    require(read(result.values.at("values"), {0}) == negative_zero,
            "inverse scalar order guard preserves lower negative zero");
  }
  std::cout << "four shaper ColorArray validation closure/Empty, dtype "
               "rejection and inverse -0 endpoint PASS\n";
}
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
      config.maximum_live_bytes = mode == 0 ? 1024 : 8 * 1024 * 1024;
      config.managed_resources = ps::ResourceLimits{};
      if (mode == 1)
        config.managed_resources->maximum_work = 1024;
      ps::ExecutionContext context(fixture.registry, config);
      auto frozen = context.freeze(plan.plan, fixture.bindings);
      if (!frozen.ok()) {
        require(mode != 2 &&
                    frozen.status().code == ps::ErrorCode::ResourceExhausted,
                "public shaper admission budget category");
        continue;
      }
      auto budget = take(context.resource_budget());
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
      require(!result.ok() &&
                  result.status().code ==
                      (mode == 2 ? ps::ErrorCode::Cancelled
                                 : ps::ErrorCode::ResourceExhausted) &&
                  budget.statistics().live[ps::ResourceKind::Payload] == 0,
              "public shaper budget/cancellation releases full intermediates");
    }
    auto registry = ps::make_default_operation_registry(false);
    ps::OperationDefinition producer;
    producer.key = "manual.shaper_input";
    producer.traits.input_count = 0;
    producer.traits.input_schema.clear();
    producer.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
    producer.traits.outputs[0].fixed_output_shape = {4097};
    producer.traits.outputs[0].output_element_type = ps::ElementType::Float64;
    producer.callback = [](const auto&) {
      return ps::Result<ps::Value>(
          ps::Status{ps::ErrorCode::OperationFailed, "required shaper source"});
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
    fixture.bindings.inputs.erase(fixture.bindings.inputs.begin());
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
      std::cout << std::hex << read(result.value().values.at("values"), {0})
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
