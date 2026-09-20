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

#include "photospider/numeric/arrays.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().message);
  return value.take_value();
}
ps::Value raw(ps::ElementType type, const std::vector<std::uint64_t>& words) {
  const auto width = ps::Value::element_size(type);
  auto bytes = take(ps::BufferAllocator{}.allocate(width * words.size()));
  for (std::size_t i = 0; i < words.size(); ++i)
    std::memcpy(bytes.data() + i * width, &words[i], width);
  return take(ps::Value::from_storage(
      {type, {words.size()}}, ps::Region::whole({words.size()}),
      {0, {static_cast<std::int64_t>(width)}}, std::move(bytes).freeze()));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  Fixture(const std::string& operation, const std::string& profile,
          const std::vector<ps::Value>& values) {
    ps::WorkflowNode node{1, "numeric." + operation + profile, {}, {}};
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto& value = values[i];
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
      node.inputs.push_back(ps::WorkflowInputReference{i + 1});
    }
    document.nodes = {node};
    document.outputs = {{"values", 1, "values"}};
  }
};
ps::Value doubles(const std::vector<double>& values) {
  std::vector<std::uint64_t> bits(values.size());
  std::memcpy(bits.data(), values.data(), values.size() * 8);
  return raw(ps::ElementType::Float64, bits);
}
void workflow(const std::string& profile) {
  const auto x = doubles({-1, 0, .25, .5, .75, 1, 2});
  Fixture fixture("smoothstep", profile, {x});
  fixture.document.nodes.clear();
  for (unsigned i = 0; i < 4; ++i) {
    const auto scalar = ps::Value::from_float64(i == 0   ? 0
                                                : i == 1 ? 1
                                                : i == 2 ? 10
                                                         : 20);
    const auto name = "scalar" + std::to_string(i);
    fixture.document.inputs.push_back({i + 2,
                                       name,
                                       scalar.descriptor(),
                                       scalar.region(),
                                       scalar.layout(),
                                       {}});
    fixture.bindings.inputs.push_back({name, scalar});
    fixture.document.nodes.push_back(take(ps::numeric::broadcast_node(
        i + 1, ps::WorkflowInputReference{i + 2}, {7}, {0})));
  }
  fixture.document.nodes.push_back(
      {5,
       "numeric.smoothstep" + profile,
       {ps::WorkflowInputReference{1}, ps::WorkflowNodeOutput{1, "values"},
        ps::WorkflowNodeOutput{2, "values"}},
       {}});
  fixture.document.nodes.push_back({6,
                                    "numeric.mix" + profile,
                                    {ps::WorkflowNodeOutput{3, "values"},
                                     ps::WorkflowNodeOutput{4, "values"},
                                     ps::WorkflowNodeOutput{5, "values"}},
                                    {}});
  fixture.document.outputs = {{"transition", 5, "values"},
                              {"blended", 6, "values"}};
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 524288;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  auto result = take(execution.execute(plan.plan, fixture.bindings));
  const double expected[] = {0, 0, .15625, .5, .84375, 1, 1};
  for (unsigned i = 0; i < 7; ++i) {
    double smooth = 0, blended = 0;
    std::memcpy(&smooth, result.values.at("transition").bytes().data() + i * 8,
                8);
    std::memcpy(&blended, result.values.at("blended").bytes().data() + i * 8,
                8);
    require(smooth == expected[i] && blended == 10 + 10 * expected[i],
            "public smoothstep -> mix composition");
  }
  std::cout << "NUM-08: broadcast -> smoothstep=[0,0,.15625,.5,.84375,1,1] -> "
               "mix=[10,10,11.5625,15,18.4375,20,20] passed\n";
}
void whole_support(const std::string& profile) {
  Fixture fixture(
      "mix", profile,
      {doubles({10, 10, 10}), doubles({20, 20, 20}), doubles({0, .25, 1})});
  std::array<std::vector<std::uint64_t>, 2> reads;
  for (unsigned branch = 0; branch < 2; ++branch) {
    auto value = fixture.bindings.inputs[branch].value;
    auto source = std::make_shared<ps::RegionalSource>();
    source->descriptor = value.descriptor();
    source->read = [&, branch, value](const auto& region, auto* destination,
                                      auto, const auto&, const auto&) {
      const auto axis = region.dimensions()[0];
      for (auto i = axis.offset; i < axis.offset + axis.extent; ++i) {
        reads[branch].push_back(i);
        if (branch ? i == 0 : i == 2)
          return ps::Result<ps::Region>(ps::Status{
              ps::ErrorCode::OperationFailed, "unselected endpoint source"});
      }
      std::memcpy(destination, value.bytes().data() + axis.offset * 8,
                  axis.extent * 8);
      return ps::Result<ps::Region>(region);
    };
    fixture.bindings.inputs[branch].value = {};
    fixture.bindings.inputs[branch].source = source;
  }
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  auto failed_source = execution.execute(plan.plan, fixture.bindings);
  require(!failed_source.ok() &&
              failed_source.status().message == "unselected endpoint source",
          "Whole mix exposes unselected endpoint source errors");
  fixture.bindings.inputs[2].value = doubles({0, 2, 1});
  auto priority = execution.execute(plan.plan, fixture.bindings);
  require(!priority.ok() &&
              priority.status().message == "unselected endpoint source",
          "collection error precedes invalid factor callback");
  fixture.bindings.inputs[2].value = doubles({0, .25, 1});
  for (unsigned branch = 0; branch < 2; ++branch) {
    fixture.bindings.inputs[branch].source.reset();
    fixture.bindings.inputs[branch].value =
        doubles(branch ? std::vector<double>{20, 20, 20}
                       : std::vector<double>{10, 10, 10});
  }
  auto frozen = take(execution.freeze(plan.plan, fixture.bindings));
  auto result = take(execution.execute_fragments(
      frozen, {{"values", take(ps::Footprint::all({3}))}}));
  const double expected[] = {10, 12.5, 20};
  for (unsigned i = 0; i < 3; ++i) {
    double value = 0;
    require(result.values.at("values").read({i}, &value, 8).ok() &&
                value == expected[i],
            "Whole mix fixture");
  }
  const auto support = take(result.dependencies.source_support());
  for (const auto* name : {"input0", "input1", "input2"}) {
    require(support.at(name) == take(ps::Footprint::all({3})),
            "Whole mix full inputs");
    auto first = take(ps::Footprint::from_regions({3}, {ps::Region({{0, 1}})}));
    require(
        take(result.dependencies.potential_dirty(name, first)).at("values") ==
            take(ps::Footprint::all({3})),
        "any mix input dirties all observed outputs");
  }
  fixture.bindings.inputs[2].value = doubles({0, 2, 1});
  frozen = take(execution.freeze(plan.plan, fixture.bindings));
  auto failed = execution.execute_fragments(
      frozen,
      {{"values", take(ps::Footprint::from_regions(
                      {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}))}});
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::InvalidDomain &&
              failed.status().detail.scope == ps::FailureScope::Run &&
              !failed.status().detail.atom &&
              failed.status().message.find("InvalidMixFactor: port=2 bits=") !=
                  std::string::npos,
          "invalid unprojected factor fails Whole");
  auto empty = take(execution.execute_fragments(
      frozen, {{"values", take(ps::Footprint::none({3}))}}));
  require(empty.diagnostics.operation_timings.empty(),
          "Empty mix skips invalid factor");
  std::cout << "mix Whole: values [10,12.5,20], eager source errors and full "
               "dirty/invalid factor passed\n";
}

void bounded_refinement(const std::string& profile,
                        const std::string& operation) {
  std::vector<ps::Value> inputs;
  for (double value : operation == "mix" ? std::vector<double>{10, 20, .25}
                                         : std::vector<double>{.25, 0, 1}) {
    inputs.push_back(doubles(std::vector<double>(16384, value)));
  }
  point_math_checks::resources({1, "numeric." + operation + profile, {}, {}},
                               inputs);
  std::cout << operation
            << ": Whole work/payload/scratch/cancel release passed\n";
}
void edges_and_sparse(const std::string& profile) {
  const auto nan = UINT64_C(0x7ff0000000000001);
  Fixture fixture(
      "smoothstep", profile,
      {raw(ps::ElementType::Float64, {0, nan, UINT64_C(0x3ff0000000000000)}),
       doubles({0, 2, 0}), doubles({1, 1, 1})});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  const auto sparse = take(ps::Footprint::from_regions(
      {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}));
  auto frozen = take(execution.freeze(plan.plan, fixture.bindings));
  auto invalid = execution.execute_fragments(frozen, {{"values", sparse}});
  require(!invalid.ok() &&
              invalid.status().reason == ps::FailureReason::InvalidDomain &&
              invalid.status().detail.scope == ps::FailureScope::Run &&
              !invalid.status().detail.atom &&
              invalid.status().message.find("InvalidEdges: port=1 bits=") !=
                  std::string::npos,
          "unprojected invalid edges outrank source NaN and fail Whole");
  auto empty = take(execution.execute_fragments(
      frozen, {{"values", take(ps::Footprint::none({3}))}}));
  require(empty.diagnostics.operation_timings.empty(),
          "Empty smoothstep skips invalid edges");
  fixture.bindings.inputs[1].value = doubles({0, 0, 0});
  frozen = take(execution.freeze(plan.plan, fixture.bindings));
  auto result = take(execution.execute_fragments(frozen, {{"values", sparse}}));
  const auto support = take(result.dependencies.source_support());
  double projected = 0;
  require(result.values.at("values").read({2}, &projected, 8).ok() &&
              projected == 1,
          "smoothstep sparse projection preserves global coordinate");

  for (unsigned port = 0; port < 3; ++port)
    require(support.at("input" + std::to_string(port)) ==
                take(ps::Footprint::all({3})),
            "smoothstep full support");
  auto source = std::make_shared<ps::RegionalSource>();
  source->descriptor = fixture.bindings.inputs[2].value.descriptor();
  unsigned reads = 0;
  source->read = [&](const auto&, auto*, auto, const auto&, const auto&) {
    ++reads;
    return ps::Result<ps::Region>(
        ps::Status{ps::ErrorCode::OperationFailed, "required smoothstep edge"});
  };
  fixture.bindings.inputs[2].value = {};
  fixture.bindings.inputs[2].source = source;
  auto failed = execution.execute(plan.plan, fixture.bindings);
  require(reads > 0 && failed.status().code == ps::ErrorCode::OperationFailed &&
              failed.status().message.find("required smoothstep edge") !=
                  std::string::npos,
          "smoothstep endpoint retains upper-edge source failure");
  std::cout << "smoothstep: InvalidEdges precedence, Whole all-port support "
               "and endpoint upstream failure passed\n";
}
void typed_and_cache(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  const auto image = [&](std::vector<std::uint64_t> bits, bool typed) {
    auto value = raw(ps::ElementType::Float32, bits);
    std::vector<ps::ValueFacet> facets;
    if (typed)
      facets.push_back(take(ps::encode_semantic(ps::rgba_semantics())));
    return take(ps::Value::from_storage(
        {ps::ElementType::Float32, {1, 1, 4}}, ps::Region::whole({1, 1, 4}),
        {0, {16, 16, 4}}, value.storage(), facets));
  };
  const auto invalid = image({0x3f800000, 0, 0, 0x40000000}, true);
  const auto valid = image({0x3f800000, 0, 0, 0x3f800000}, false);
  const std::map<std::string, ps::ParameterValue> parameters;
  for (unsigned factor : {0U, 0x3f800000U}) {
    const std::vector<ps::Value> inputs{invalid, valid,
                                        image({factor, 0, 0, 0}, false)};
    const std::vector<ps::Region> demands{
        inputs[0].region(), inputs[1].region(), inputs[2].region()};
    ps::OperationInvocation call(inputs, demands, parameters, ps::Backend::Cpu,
                                 {}, ps::Region::whole({1, 1, 4}));
    require(!registry->invoke("numeric.mix" + profile, call).ok(),
            "Whole mix validates unselected typed branch too");
  }
  for (const auto& operation :
       {std::string("mix"), std::string("smoothstep")}) {
    const std::vector<ps::Value> inputs{valid, image({0, 0, 0, 0}, false),
                                        invalid};
    const std::vector<ps::Region> demands{
        inputs[0].region(), inputs[1].region(), inputs[2].region()};
    ps::OperationInvocation call(inputs, demands, parameters, ps::Backend::Cpu,
                                 {}, ps::Region::whole({1, 1, 4}));
    require(!registry->invoke("numeric." + operation + profile, call).ok(),
            "factor/edge requires typed alpha closure");
  }
  Fixture fixture("mix", profile, {doubles({10}), doubles({20}), doubles({0})});
  ps::InputSnapshotStore snapshots;
  for (auto& binding : fixture.bindings.inputs) {
    binding.snapshot = std::make_shared<const ps::InputSnapshot>(
        take(snapshots.import_value(binding.value)));
    binding.value = {};
  }
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  auto demand = take(execution.open_demand(plan.plan, fixture.bindings));
  const ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
  take(demand.request(query));
  require(take(demand.request(query)).diagnostics.cache_hits > 0,
          "warm mix cache");
  fixture.bindings.inputs[1].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(doubles({99}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "replace unselected endpoint");
  auto unselected = take(demand.request(query));
  double unchanged = 0;
  require(unselected.diagnostics.cache_hits == 0 &&
              unselected.values.at("values").read({0}, &unchanged, 8).ok() &&
              unchanged == 10,
          "unselected endpoint edit invalidates Whole cache while value stays "
          "selected");
  fixture.bindings.inputs[2].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(doubles({1}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "replace mix factor snapshot");
  auto changed = take(demand.request(query));
  double value = 0;
  require(changed.values.at("values").read({0}, &value, 8).ok() && value == 99,
          "changed factor replans branch and output");
  const auto support = take(changed.dependencies.source_support());
  require(support.at("input0") == take(ps::Footprint::all({1})),
          "changed factor retains old branch support too");
  require(support.at("input1") == take(ps::Footprint::all({1})),
          "changed factor retains newly selected support");
  std::cout << "interpolation: Whole branch/factor/edge validation and "
               "factor-cache invalidation passed\n";
}
void layout_schema_environment(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  auto backing = doubles({0, .5, 1});
  const auto reversed = take(ps::Value::from_storage(
      backing.descriptor(), backing.region(), {16, {-8}}, backing.storage()));
  const auto scalar = doubles({0});
  const auto lower = take(ps::Value::from_storage(
      backing.descriptor(), backing.region(), {0, {0}}, scalar.storage()));
  auto bytes = take(ps::BufferAllocator{}.allocate(9));
  const double one = 1;
  std::memcpy(bytes.data() + 1, &one, 8);
  const auto upper =
      take(ps::Value::from_storage(backing.descriptor(), backing.region(),
                                   {1, {0}}, std::move(bytes).freeze()));
  const std::vector<ps::Value> values{reversed, lower, upper};
  const std::vector<ps::Region> demands{backing.region(), backing.region(),
                                        backing.region()};
  const std::map<std::string, ps::ParameterValue> parameters;
  ps::OperationInvocation call(values, demands, parameters);
  auto output = take(registry->invoke("numeric.smoothstep" + profile, call));
  const double expected[] = {1, .5, 0};
  require(std::memcmp(output.bytes().data(), expected, sizeof(expected)) == 0,
          "smoothstep negative/zero/unaligned strides");
  ps::OperationInvocation roi_call(values, demands, parameters,
                                   ps::Backend::Cpu, {}, ps::Region({{1, 1}}));
  require(!registry->invoke("numeric.smoothstep" + profile, roi_call).ok(),
          "direct Whole rejects partial output");
  for (const auto& operation :
       {std::string("mix"), std::string("smoothstep")}) {
    ps::DependencyRequest request;
    request.inputs.assign(3, {{ps::ElementType::Float64, {1}}, {}});
    request.outputs = take(ps::Footprint::none({1}));
    request.snapshot_identity = "interpolation-empty";
    request.inputs[1].descriptor.element_type = ps::ElementType::Float32;
    require(registry->resolve_traits("numeric." + operation + profile,
                                     request.inputs, request.parameters)
                    .status()
                    .code == ps::ErrorCode::TypeMismatch,
            "unselected metadata still checked");
    request.inputs[1] = request.inputs[0];
    request.inputs[1].descriptor.shape = {2};
    require(registry->resolve_traits("numeric." + operation + profile,
                                     request.inputs, request.parameters)
                    .status()
                    .code == ps::ErrorCode::TypeMismatch,
            "no implicit broadcast");
    request.inputs[1] = request.inputs[0];
    request.parameters = {{"edge0", 0.0}};
    require(registry->resolve_traits("numeric." + operation + profile,
                                     request.inputs, request.parameters)
                    .status()
                    .code == ps::ErrorCode::InvalidArgument,
            "no legacy static parameters");
  }
  struct Environment {
    fenv_t saved;
    Environment() { require(fegetenv(&saved) == 0, "save fenv"); }
    ~Environment() { fesetenv(&saved); }
  } environment;
  const std::vector<ps::Value> special{
      raw(ps::ElementType::Float64, {UINT64_C(0x7ff0000000000001)}),
      doubles({1}), doubles({0})};
  const std::vector<ps::Region> single(3, ps::Region::whole({1}));
  require(fesetround(FE_DOWNWARD) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
              feraiseexcept(FE_DIVBYZERO) == 0,
          "set fenv");
  const auto flags = fetestexcept(FE_ALL_EXCEPT);
  ps::OperationInvocation endpoint(special, single, parameters);
  const auto copied = take(registry->invoke("numeric.mix" + profile, endpoint));
  std::uint64_t bits = 0;
  std::memcpy(&bits, copied.bytes().data(), 8);
  require(bits == UINT64_C(0x7ff0000000000001) && fegetround() == FE_DOWNWARD &&
              fetestexcept(FE_ALL_EXCEPT) == flags,
          "endpoint sNaN bitcopy preserves fenv");
  const auto exact =
      take(registry->invoke("numeric.smoothstep" + profile, call));
  require(exact.bytes().size() == 24 && fegetround() == FE_DOWNWARD &&
              fetestexcept(FE_ALL_EXCEPT) == flags,
          "cubic integer arithmetic preserves fenv");
  std::cout << "interpolation: layouts/ROI, Empty/schema and sNaN/cubic caller "
               "fenv passed\n";
}
void whole_layouts(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  for (const std::string operation : {"mix", "smoothstep"}) {
    for (bool reverse : {false, true}) {
      std::vector<ps::Value> inputs;
      std::vector<ps::Region> demands;
      for (unsigned port = 0; port < 3U; ++port) {
        auto storage = take(ps::BufferAllocator{}.allocate(65 * 4 + 1));
        for (unsigned i = 0; i < 65; ++i) {
          const float x = operation == "mix" ? (port == 0   ? 0.f
                                                : port == 1 ? 1.f
                                                            : i / 64.f)
                                             : (port == 0   ? i / 64.f
                                                : port == 1 ? 0.f
                                                            : 1.f);
          std::memcpy(storage.data() + 1 + 4 * i, &x, 4);
        }
        ps::StridedLayout layout{reverse ? UINT64_C(257) : UINT64_C(5),
                                 {777, reverse ? -4 : 4}};
        if (!reverse)
          layout.origin = {0, 1};
        inputs.push_back(take(ps::Value::from_storage(
            {ps::ElementType::Float32, {1, 65}}, ps::Region::whole({1, 65}),
            layout, std::move(storage).freeze())));
        demands.push_back(inputs.back().region());
      }
      const std::map<std::string, ps::ParameterValue> parameters;
      ps::OperationInvocation call(inputs, demands, parameters,
                                   ps::Backend::Cpu, {},
                                   ps::Region::whole({1, 65}));
      auto result =
          take(registry->invoke("numeric." + operation + profile, call));
      for (unsigned i = 0; i < 65; ++i) {
        float expected = (reverse ? 64 - i : i) / 64.0f;
        if (operation == "smoothstep")
          expected = expected * expected * (3 - 2 * expected);
        std::uint32_t actual = 0, want = 0;
        std::memcpy(&want, &expected, 4);
        std::memcpy(&actual,
                    result.bytes().data() + take(result.byte_address({0, i})),
                    4);
        require(actual == want,
                "interpolation all-port Float32 tail/origin/stride oracle");
      }
    }
  }
}
void oracle(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  std::string operation;
  unsigned dtype = 0;
  std::uint64_t bits[3]{};
  while (std::cin >> operation >> dtype >> std::hex >> bits[0] >> bits[1] >>
         bits[2] >> std::dec) {
    ps::WorkflowDocument document;
    ps::ExecutionBindings bindings;
    ps::WorkflowNode node{1, "numeric." + operation + profile, {}, {}};
    const unsigned count = 3;
    for (unsigned i = 0; i < count; ++i) {
      const auto value = raw(static_cast<ps::ElementType>(dtype), {bits[i]});
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1,
                                 name,
                                 value.descriptor(),
                                 value.region(),
                                 value.layout(),
                                 {}});
      bindings.inputs.push_back({name, value});
      node.inputs.push_back(ps::WorkflowInputReference{i + 1});
    }
    document.nodes = {node};
    document.outputs = {{"values", 1, "values"}};
    ps::GraphContext graph(document);
    auto plan = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 262144;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    auto result = execution.execute(plan.plan, bindings);
    if (!result.ok()) {
      require(result.status().code == ps::ErrorCode::InvalidArgument &&
                  result.status().reason == ps::FailureReason::InvalidDomain,
              "unexpected interpolation oracle failure");
      std::cout << "error\n";
      continue;
    }
    std::uint64_t output = 0;
    const auto& value = result.value().values.at("values");
    std::memcpy(&output, value.bytes().data(), value.bytes().size());
    std::cout << std::hex << output << std::dec << '\n';
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string profile = argc > 1 ? argv[1] : "_strict";
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      workflow(profile);
      whole_support(profile);
      for (const auto& operation :
           {std::string("mix"), std::string("smoothstep")}) {
        bounded_refinement(profile, operation);
      }
      edges_and_sparse(profile);
      typed_and_cache(profile);
      layout_schema_environment(profile);
      whole_layouts(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
