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
void workflow(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  auto input = raw(ps::ElementType::Float64, {0, UINT64_C(0x3fe0000000000000),
                                              UINT64_C(0x3ff0000000000000),
                                              UINT64_C(0x4000000000000000)});
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", input}};
  const double bounds[] = {0, 1, 0, 255};
  for (std::uint64_t i = 0; i < 4; ++i) {
    const auto scalar = ps::Value::from_float64(bounds[i]);
    const auto name = "bound" + std::to_string(i);
    document.inputs.push_back({i + 2,
                               name,
                               scalar.descriptor(),
                               scalar.region(),
                               scalar.layout(),
                               {}});
    bindings.inputs.push_back({name, scalar});
    document.nodes.push_back(take(ps::numeric::broadcast_node(
        i + 1, ps::WorkflowInputReference{i + 2}, {4}, {0})));
  }
  document.nodes.push_back(
      {5,
       "numeric.remap_range" + profile,
       {ps::WorkflowInputReference{1}, ps::WorkflowNodeOutput{1, "values"},
        ps::WorkflowNodeOutput{2, "values"},
        ps::WorkflowNodeOutput{3, "values"},
        ps::WorkflowNodeOutput{4, "values"}},
       {}});
  document.nodes.push_back({6,
                            "numeric.clamp" + profile,
                            {ps::WorkflowNodeOutput{5, "values"},
                             ps::WorkflowNodeOutput{3, "values"},
                             ps::WorkflowNodeOutput{4, "values"}},
                            {}});
  document.outputs = {{"mapped", 5, "values"}, {"clipped", 6, "values"}};
  ps::GraphContext graph(document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 262144;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  auto result = take(execution.execute(plan.plan, bindings));
  const double expected[] = {0, 127.5, 255, 510};
  for (unsigned i = 0; i < 4; ++i) {
    double mapped = 0, clipped = 0;
    std::memcpy(&mapped, result.values.at("mapped").bytes().data() + i * 8, 8);
    std::memcpy(&clipped, result.values.at("clipped").bytes().data() + i * 8,
                8);
    require(mapped == expected[i] && clipped == (i == 3 ? 255 : expected[i]),
            "broadcast/remap/clamp composition");
  }
  std::cout << "NUM-06: broadcast bounds -> remap=[0,127.5,255,510] -> "
               "clamp=[0,127.5,255,255] passed\n";
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
void bounds_and_demand(const std::string& profile) {
  const auto nan = UINT64_C(0x7ff0000000000001),
             one = UINT64_C(0x3ff0000000000000),
             two = UINT64_C(0x4000000000000000);
  for (const auto& name : {std::string("clamp"), std::string("remap_range")}) {
    std::vector<ps::Value> values{
        raw(ps::ElementType::Float64, {0, nan, one}),
        raw(ps::ElementType::Float64, {0, two, 0}),
        raw(ps::ElementType::Float64, {one, one, one})};
    if (name == "remap_range") {
      values.push_back(raw(ps::ElementType::Float64, {0, 0, 0}));
      values.push_back(raw(ps::ElementType::Float64, {one, one, one}));
    }
    Fixture fixture(name, profile, values);
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(fixture.registry, config);
    const auto sparse = take(ps::Footprint::from_regions(
        {3}, {ps::Region({{0, 1}}), ps::Region({{2, 1}})}));
    auto frozen = take(execution.freeze(plan.plan, fixture.bindings));
    auto failed = execution.execute_fragments(frozen, {{"values", sparse}});
    require(
        !failed.ok() &&
            failed.status().code == ps::ErrorCode::InvalidArgument &&
            failed.status().reason == ps::FailureReason::InvalidDomain &&
            failed.status().detail.scope == ps::FailureScope::Run &&
            !failed.status().detail.atom &&
            failed.status().message.find("InvalidBounds: port=1") !=
                std::string::npos &&
            failed.status().message.find("coordinate=[1]") != std::string::npos,
        "invalid bound outside projection outranks source NaN and fails Whole");
    auto empty = take(execution.execute_fragments(
        frozen, {{"values", take(ps::Footprint::none({3}))}}));
    require(empty.diagnostics.operation_timings.empty(),
            "Empty skips invalid bounds");
    fixture.bindings.inputs[1].value = raw(ps::ElementType::Float64, {0, 0, 0});
    frozen = take(execution.freeze(plan.plan, fixture.bindings));
    auto selected =
        take(execution.execute_fragments(frozen, {{"values", sparse}}));
    const auto support = take(selected.dependencies.source_support());
    for (std::size_t i = 0; i < values.size(); ++i)
      require(support.at("input" + std::to_string(i)) ==
                  take(ps::Footprint::all({3})),
              "all range ports retain complete support");
    auto dirty = take(selected.dependencies.potential_dirty(
        "input1",
        take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}))));
    require(dirty.at("values") == sparse,
            "bound gap dirties every observation");
  }
  Fixture endpoint("remap_range", profile,
                   {ps::Value::from_float64(0), ps::Value::from_float64(0),
                    ps::Value::from_float64(1), ps::Value::from_float64(0),
                    ps::Value::from_float64(255)});
  unsigned reads = 0;
  auto source = std::make_shared<ps::RegionalSource>();
  source->descriptor = endpoint.bindings.inputs[4].value.descriptor();
  source->read = [&](const auto&, auto*, auto, const auto&, const auto&) {
    ++reads;
    return ps::Result<ps::Region>(ps::Status{ps::ErrorCode::OperationFailed,
                                             "required target-upper source"});
  };
  endpoint.bindings.inputs[4].value = {};
  endpoint.bindings.inputs[4].source = source;
  ps::GraphContext graph(endpoint.document);
  auto plan = take(ps::Compiler(endpoint.registry).compile(graph));
  ps::ExecutionContext execution(endpoint.registry);
  auto failed = execution.execute(plan.plan, endpoint.bindings);
  require(
      reads == 1 && failed.status().code == ps::ErrorCode::OperationFailed &&
          failed.status().message.find("required target-upper") !=
              std::string::npos,
      "endpoint still reads all five sources and preserves upstream failure");
  std::cout << "ranges: Whole invalid bounds; all-port full support/dirty "
               "exact; endpoint upstream failure retained\n";
}
void bounded_refinement(const std::string& profile) {
  for (const std::string operation : {"clamp", "remap_range"}) {
    std::vector<ps::Value> inputs;
    for (double value : {.5, 0., 1., 0., 255.}) {
      std::uint64_t bits = 0;
      std::memcpy(&bits, &value, 8);
      inputs.push_back(raw(ps::ElementType::Float64,
                           std::vector<std::uint64_t>(16384, bits)));
    }
    if (operation == "clamp")
      inputs.resize(3);
    point_math_checks::resources({1, "numeric." + operation + profile, {}, {}},
                                 inputs);
  }
  std::cout << "range Whole work/capacity/cancellation releases payload\n";
}
struct SavedEnvironment {
  fenv_t state;
  SavedEnvironment() { require(fegetenv(&state) == 0, "save fenv"); }
  ~SavedEnvironment() { fesetenv(&state); }
};
void typed_cache_and_environment(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  auto bytes = raw(ps::ElementType::Float32, {0x3f800000, 0, 0, 0x40000000});
  const ps::ValueDescriptor descriptor{ps::ElementType::Float32, {1, 1, 4}};
  auto invalid_upper = take(ps::Value::from_storage(
      descriptor, ps::Region::whole({1, 1, 4}), {0, {16, 16, 4}},
      bytes.storage(), {take(ps::encode_semantic(ps::rgba_semantics()))}));
  auto input =
      take(ps::Value::from_storage(descriptor, invalid_upper.region(),
                                   invalid_upper.layout(), bytes.storage()));
  auto zeros = raw(ps::ElementType::Float32, {0, 0, 0, 0});
  auto lower =
      take(ps::Value::from_storage(descriptor, invalid_upper.region(),
                                   invalid_upper.layout(), zeros.storage()));
  const std::vector<ps::Value> values{input, lower, invalid_upper};
  const std::vector<ps::Region> demands{input.region(), lower.region(),
                                        invalid_upper.region()};
  const std::map<std::string, ps::ParameterValue> parameters;
  ps::OperationInvocation typed(values, demands, parameters, ps::Backend::Cpu,
                                {}, ps::Region::whole({1, 1, 4}));
  require(!registry->invoke("numeric.clamp" + profile, typed).ok(),
          "typed upper requires unselected alpha validation");
  Fixture fixture("clamp", profile,
                  {ps::Value::from_float64(.5), ps::Value::from_float64(0),
                   ps::Value::from_float64(1)});
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
          "range warm cache");
  fixture.bindings.inputs[2].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(ps::Value::from_float64(.25))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "replace dynamic upper bound");
  auto changed = take(demand.request(query));
  double actual = 0;
  require(
      changed.values.at("values").read({0}, &actual, 8).ok() && actual == .25,
      "changed bound invalidates cached clamp");
  SavedEnvironment saved;
  const std::vector<ps::Value> tiny{
      raw(ps::ElementType::Float64, {UINT64_C(0x8000000000000001)}),
      ps::Value::from_float64(0), ps::Value::from_float64(2),
      ps::Value::from_float64(0), raw(ps::ElementType::Float64, {1})};
  std::vector<ps::Region> tiny_demands;
  for (const auto& value : tiny)
    tiny_demands.push_back(value.region());
  require(fesetround(FE_UPWARD) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
              feraiseexcept(FE_DIVBYZERO) == 0,
          "set caller fenv");
  const auto flags = fetestexcept(FE_ALL_EXCEPT);
  ps::OperationInvocation invocation(tiny, tiny_demands, parameters);
  const auto negative_zero =
      take(registry->invoke("numeric.remap_range" + profile, invocation));
  require(fegetround() == FE_UPWARD && fetestexcept(FE_ALL_EXCEPT) == flags,
          "exact range preserves caller rounding/flags");
  std::uint64_t bits = 0;
  std::memcpy(&bits, negative_zero.bytes().data(), 8);
  require(bits == (UINT64_C(1) << 63),
          "nonzero exact underflow retains negative sign");
  std::cout << "ranges: typed closure, bound-cache edit, negative underflow "
               "and caller fenv passed\n";
}
void whole_layouts(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  for (const std::string operation : {"clamp", "remap_range"}) {
    for (bool reverse : {false, true}) {
      std::vector<ps::Value> inputs;
      std::vector<ps::Region> demands;
      for (unsigned port = 0; port < (operation == "clamp" ? 3U : 5U); ++port) {
        auto storage = take(ps::BufferAllocator{}.allocate(65 * 4 + 1));
        for (unsigned i = 0; i < 65; ++i) {
          const float x = port == 0                ? i / 32.0f
                          : port == 1 || port == 3 ? 0.0f
                                                   : 1.0f;
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
        float expected = (reverse ? 64 - i : i) / 32.0f;
        if (operation == "clamp" && expected > 1)
          expected = 1;
        std::uint32_t actual = 0, want = 0;
        std::memcpy(&want, &expected, 4);
        std::memcpy(&actual,
                    result.bytes().data() + take(result.byte_address({0, i})),
                    4);
        require(actual == want,
                "range all-port Float32 tail/origin/stride oracle");
      }
    }
  }
}
void schemas_and_layouts(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  auto backing = raw(ps::ElementType::Int64, {1, 2, 3});
  auto reversed = take(ps::Value::from_storage(
      backing.descriptor(), backing.region(), {16, {-8}}, backing.storage()));
  auto zero_backing = raw(ps::ElementType::Int64, {0});
  auto lower =
      take(ps::Value::from_storage(backing.descriptor(), backing.region(),
                                   {0, {0}}, zero_backing.storage()));
  auto padded = take(ps::BufferAllocator{}.allocate(9));
  const std::int64_t two = 2;
  std::memcpy(padded.data() + 1, &two, 8);
  auto upper =
      take(ps::Value::from_storage(backing.descriptor(), backing.region(),
                                   {1, {0}}, std::move(padded).freeze()));
  const std::vector<ps::Value> inputs{reversed, lower, upper};
  const std::vector<ps::Region> demands{reversed.region(), lower.region(),
                                        upper.region()};
  const std::map<std::string, ps::ParameterValue> parameters;
  ps::OperationInvocation call(inputs, demands, parameters);
  auto result = take(registry->invoke("numeric.clamp" + profile, call));
  const std::int64_t expected[] = {2, 2, 1};
  require(
      result.bytes().size() == sizeof(expected) &&
          std::memcmp(result.bytes().data(), expected, sizeof(expected)) == 0,
      "clamp handles negative/zero/unaligned source strides");
  ps::OperationInvocation selected(inputs, demands, parameters,
                                   ps::Backend::Cpu, {}, ps::Region({{1, 1}}));
  require(!registry->invoke("numeric.clamp" + profile, selected).ok(),
          "direct Whole callback rejects partial output");
  Fixture projected("clamp", profile,
                    {raw(ps::ElementType::Int64, {3, 2, 1}),
                     raw(ps::ElementType::Int64, {0, 0, 0}),
                     raw(ps::ElementType::Int64, {2, 2, 2})});
  ps::GraphContext graph(projected.document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext context(registry);
  auto frozen = take(context.freeze(plan.plan, projected.bindings));
  auto projected_result = take(context.execute_fragments(
      frozen, {{"values", take(ps::Footprint::from_regions(
                              {3}, {ps::Region({{1, 1}})}))}}));
  std::int64_t projected_bits = 0;
  require(
      projected_result.values.at("values").read({1}, &projected_bits, 8).ok() &&
          projected_bits == 2,
      "public projection retains global coordinate");
  ps::DependencyRequest request;
  request.inputs = {{backing.descriptor(), {}},
                    {backing.descriptor(), {}},
                    {{ps::ElementType::Float64, {3}}, {}}};
  request.outputs = take(ps::Footprint::none({3}));
  request.snapshot_identity = "range-schema";
  require(registry->resolve_traits("numeric.clamp" + profile, request.inputs,
                                   request.parameters)
                  .status()
                  .code == ps::ErrorCode::TypeMismatch,
          "mixed range dtypes reject even for Empty");
  request.inputs[2] = {backing.descriptor(), {}};
  request.inputs[1].descriptor.shape = {1};
  require(registry->resolve_traits("numeric.clamp" + profile, request.inputs,
                                   request.parameters)
                  .status()
                  .code == ps::ErrorCode::TypeMismatch,
          "range scalar bounds require explicit broadcast");
  request.inputs[1] = {backing.descriptor(), {}};
  request.inputs.resize(5, request.inputs[0]);
  require(registry->resolve_traits("numeric.remap_range" + profile,
                                   request.inputs, request.parameters)
                  .status()
                  .code == ps::ErrorCode::TypeMismatch,
          "integer remap needs explicit cast");
  request.inputs.resize(3);
  request.parameters = {{"min", 0.0}};
  require(registry->resolve_traits("numeric.clamp" + profile, request.inputs,
                                   request.parameters)
                  .status()
                  .code == ps::ErrorCode::InvalidArgument,
          "new dynamic clamp rejects legacy static min parameter");
  std::cout << "ranges: negative/zero/unaligned layouts, packed global ROI and "
               "schema errors passed\n";
}
void oracle(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  std::string operation;
  unsigned dtype = 0;
  std::uint64_t bits[5]{};
  while (std::cin >> operation >> dtype >> std::hex >> bits[0] >> bits[1] >>
         bits[2] >> bits[3] >> bits[4] >> std::dec) {
    ps::WorkflowDocument document;
    ps::ExecutionBindings bindings;
    ps::WorkflowNode node{1, "numeric." + operation + profile, {}, {}};
    const unsigned count = operation == "clamp" ? 3 : 5;
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
              "unexpected range oracle failure");
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
      bounds_and_demand(profile);
      bounded_refinement(profile);
      typed_cache_and_environment(profile);
      schemas_and_layouts(profile);
      whole_layouts(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
