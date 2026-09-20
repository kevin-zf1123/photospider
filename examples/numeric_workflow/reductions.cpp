#include "photospider/numeric/reductions.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <cstdint>
#include <cstring>
#include <iostream>
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
  Fixture(ps::WorkflowNode node, const ps::Value& value) {
    document.inputs = {{1, "input", value.descriptor(), value.region(),
                        value.layout(), value.facets()}};
    bindings.inputs = {{"input", value}};
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::Result<ps::DemandResult> run(const ps::Footprint& demand) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 65536;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    return context.execute_fragments(snapshot.value(), {{"values", demand}});
  }
};
ps::WorkflowNode authored(const std::string& operation, ps::ElementType source,
                          ps::ElementType destination,
                          const std::vector<std::uint64_t>& axes,
                          std::int64_t ddof, ps::CpuNumericProfile profile) {
  const ps::WorkflowInput input = ps::WorkflowInputReference{1};
  if (operation == "sum")
    return take(ps::numeric::reduce_sum_node(1, input, axes, source,
                                             destination, profile));
  if (operation == "minimum")
    return take(ps::numeric::reduce_minimum_node(1, input, axes, profile));
  if (operation == "maximum")
    return take(ps::numeric::reduce_maximum_node(1, input, axes, profile));
  if (operation == "mean")
    return take(
        ps::numeric::reduce_mean_node(1, input, axes, destination, profile));
  if (operation == "count")
    return take(ps::numeric::reduce_count_node(1, input, axes, profile));
  if (operation == "variance")
    return take(ps::numeric::reduce_variance_node(1, input, axes, ddof,
                                                  destination, profile));
  require(operation == "std", "unknown reducer");
  return take(
      ps::numeric::reduce_std_node(1, input, axes, ddof, destination, profile));
}
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
void oracle(ps::CpuNumericProfile profile) {
  std::string operation, encoded_shape, encoded_axes;
  unsigned source = 0, destination = 0;
  std::int64_t ddof = 0;
  while (std::cin >> operation >> source >> destination >> ddof >>
         encoded_shape >> encoded_axes) {
    auto shape = list(encoded_shape);
    const auto axes = list(encoded_axes);
    std::uint64_t count = 1;
    for (auto size : shape)
      count *= size;
    std::vector<std::uint64_t> bits(count);
    for (auto& value : bits)
      std::cin >> std::hex >> value >> std::dec;
    const auto type = static_cast<ps::ElementType>(source);
    const auto target = static_cast<ps::ElementType>(destination);
    auto node =
        authored(operation, type, target, axes, ddof < 0 ? 0 : ddof, profile);
    if (operation == "variance" || operation == "std")
      node.parameters["ddof"] = ddof;
    Fixture fixture(std::move(node), array(type, shape, bits));
    for (auto axis : axes)
      shape[axis] = 1;
    auto all = take(ps::Footprint::all(shape));
    auto answer = fixture.run(all);
    if (!answer.ok()) {
      if (answer.status().reason == ps::FailureReason::ArithmeticOverflow)
        std::cout << "overflow\n";
      else if (answer.status().message.find("InvalidDegreesOfFreedom") !=
               std::string::npos)
        std::cout << "ddof\n";
      else
        throw std::runtime_error(answer.status().message);
      continue;
    }
    bool first = true;
    require(all.visit(
                   [&](const auto& coordinate) {
                     std::uint64_t value = 0;
                     auto status = answer.value().values.at("values").read(
                         coordinate, &value, ps::Value::element_size(target));
                     if (status.ok()) {
                       if (!first)
                         std::cout << ' ';
                       first = false;
                       std::cout << std::hex << value << std::dec;
                     }
                     return status;
                   },
                   4096)
                .ok(),
            "oracle result read");
    std::cout << '\n';
  }
}
void examples(ps::CpuNumericProfile profile) {
  const auto input = array(ps::ElementType::Int64, {2, 3}, {1, 2, 3, 4, 5, 6});
  const std::vector<std::string> operations = {
      "sum", "minimum", "maximum", "mean", "count", "variance", "std"};
  const std::vector<std::vector<std::uint64_t>> expected = {
      {6, 15},
      {1, 4},
      {3, 6},
      {0x4000000000000000, 0x4014000000000000},
      {3, 3},
      {0x3fe5555555555555, 0x3fe5555555555555},
      {0x3fea20bd700c2c3e, 0x3fea20bd700c2c3e}};
  for (std::size_t i = 0; i < operations.size(); ++i) {
    const auto type =
        i == 3 || i >= 5 ? ps::ElementType::Float64 : ps::ElementType::Int64;
    for (auto rounding :
         {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(rounding) == 0, "set rounding");
      Fixture fixture(authored(operations[i], ps::ElementType::Int64, type, {1},
                               0, profile),
                      input);
      auto answer = take(fixture.run(take(ps::Footprint::all({2, 1}))));
      for (std::uint64_t row = 0; row < 2; ++row) {
        std::uint64_t actual = 0;
        require(answer.values.at("values").read({row, 0}, &actual, 8).ok() &&
                    actual == expected[i][row],
                "public reduction example mismatch");
      }
      require(fegetround() == rounding, "preserve fenv");
    }
    std::cout << operations[i] << ": passed\n";
  }
  require(fesetround(FE_TONEAREST) == 0, "restore rounding");
}

void streaming(ps::CpuNumericProfile profile) {
  for (const std::string operation : {"sum", "mean", "variance", "std"}) {
    auto declaration =
        array(ps::ElementType::Int64, {4096}, std::vector<std::uint64_t>(4096));
    const auto destination =
        operation == "sum" ? ps::ElementType::Int64 : ps::ElementType::Float64;
    Fixture fixture(authored(operation, ps::ElementType::Int64, destination,
                             {0}, 0, profile),
                    declaration);
    auto source = std::make_shared<ps::RegionalSource>();
    source->descriptor = declaration.descriptor();
    std::uint64_t next = 0, reads = 0;
    source->read = [&](const ps::Region& region, std::uint8_t* data,
                       std::uint64_t bytes, const ps::BufferAllocator&,
                       const ps::CancellationToken&) {
      require(bytes <= 512 && region.dimensions()[0].offset == next,
              "bounded ordered source window");
      ++reads;
      for (std::uint64_t i = 0; i < bytes / 8; ++i) {
        const std::int64_t value = (next + i) % 4;
        std::memcpy(data + i * 8, &value, 8);
      }
      next += bytes / 8;
      return ps::Result<ps::Region>(region);
    };
    fixture.bindings.inputs = {{"input", {}, source}};
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 16384;
    ps::ExecutionContext context(fixture.registry, config);
    ps::ExecutionOptions options;
    options.dependencies.maximum_work = 128 * 1024 * 1024;
    options.maximum_dependency_work = 128 * 1024 * 1024;
    options.maximum_dependency_cache_work = 0;
    auto answer =
        take(context.execute(plan.plan, fixture.bindings, {}, options));
    std::uint64_t actual = 0;
    std::memcpy(&actual, answer.values.at("values").bytes().data(), 8);
    const std::uint64_t expected =
        operation == "sum"        ? 6144
        : operation == "mean"     ? UINT64_C(0x3ff8000000000000)
        : operation == "variance" ? UINT64_C(0x3ff4000000000000)
                                  : UINT64_C(0x3ff1e3779b97f4a8);
    require(actual == expected && reads == 64 && next == 4096,
            "streamed exact reduction");
    require(answer.diagnostics.peak_live_bytes <= 16384,
            "bounded live payload");
  }
  std::cout << "4096-element streamed groups: 64 windows, payload <=16384 "
               "bytes passed\n";
}
void metadata_count(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition failed;
  failed.key = "manual.count_source";
  failed.traits.input_count = 0;
  failed.traits.input_schema.clear();
  auto& output = failed.traits.outputs[0];
  output.shape_rule = ps::OperationShapeRule::Fixed;
  output.fixed_output_shape = {UINT64_C(1) << 20, UINT64_C(1) << 20};
  output.output_element_type = ps::ElementType::Int64;
  failed.callback = [&](const auto&) {
    ++calls;
    return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                            "count must not execute source"});
  };
  require(registry->register_operation(std::move(failed)).ok() &&
              registry->freeze().ok(),
          "count registry");
  ps::WorkflowDocument document;
  document.nodes = {{1, "manual.count_source", {}, {}},
                    take(ps::numeric::reduce_count_node(
                        2, ps::WorkflowNodeOutput{1, "value"}, {1}, profile))};
  document.outputs = {{"values", 2, "values"}};
  ps::GraphContext graph(document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 4096;
  ps::ExecutionContext context(registry, config);
  auto snapshot = take(context.freeze(plan.plan, {}));
  auto answer = take(context.execute_fragments(
      snapshot,
      {{"values", take(ps::Footprint::all({UINT64_C(1) << 20, 1}))}}));
  const auto& value = answer.values.at("values");
  std::int64_t count = 0;
  require(value.read({(UINT64_C(1) << 20) - 1, 0}, &count, 8).ok() &&
              count == 1048576,
          "metadata count large group");
  require(calls == 0 && value.fragments().size() == 1 &&
              value.fragments()[0].storage()->bytes().size() == 8,
          "count skips producer and uses eight-byte owner");
  std::cout << "count: 2^40 logical input, 2^20 results, eight-byte owner, "
               "producer calls=0 passed\n";
}
void atom_isolation(ps::CpuNumericProfile profile) {
  Fixture fixture(
      take(ps::numeric::reduce_sum_node(1, ps::WorkflowInputReference{1}, {1},
                                        ps::ElementType::Int64, {}, profile)),
      array(ps::ElementType::Int64, {2, 2},
            {1, 2, UINT64_C(0x7fffffffffffffff), 1}));
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  auto atoms = take(
      context.execute_atoms(plan.plan, fixture.bindings,
                            {{"values", take(ps::Footprint::all({2, 1}))}}));
  unsigned good = 0, bad = 0;
  for (const auto& atom : atoms.atoms) {
    if (atom.outcome.ok()) {
      ++good;
      require(atom.key.coordinate[0] == 0, "successful group key");
    } else {
      ++bad;
      const auto& error = atom.outcome.status();
      require(atom.key.coordinate[0] == 1 &&
                  error.reason == ps::FailureReason::ArithmeticOverflow &&
                  error.detail.origin == ps::FailureOrigin::Domain &&
                  error.detail.scope == ps::FailureScope::Atom,
              "overflow isolated to requested group");
    }
  }
  require(good == 1 && bad == 1, "independent reduction group outcomes");
  auto first =
      take(ps::Footprint::from_regions({2, 1}, {ps::Region({{0, 1}, {0, 1}})}));
  auto answer = take(fixture.run(first));
  auto expected =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{0, 1}, {0, 2}})}));
  require(take(answer.dependencies.source_support()).at("input") == expected,
          "selected reduction group support");
  const auto untouched =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{1, 1}, {0, 2}})}));
  require(take(answer.dependencies.potential_dirty("input", untouched))
              .at("values")
              .empty(),
          "unrequested group does not invalidate result");
  std::cout
      << "sparse group support/dirty and integer overflow isolation passed\n";
}

void boundaries(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  for (const std::string operation :
       {"sum", "minimum", "maximum", "mean", "count", "variance", "std"}) {
    const auto destination = operation == "count" ? ps::ElementType::Int64
                             : operation == "minimum" || operation == "maximum"
                                 ? ps::ElementType::Float32
                                 : ps::ElementType::Float64;
    auto node = authored(operation, ps::ElementType::Float32, destination, {1},
                         0, profile);
    ps::DependencyRequest request;
    request.inputs = {{{ps::ElementType::Float32, {1, 2, 4}}, {facet}}};
    request.parameters = node.parameters;
    request.snapshot_identity = "reduction-boundaries";
    request.outputs = take(ps::Footprint::none({1, 1, 4}));
    auto empty = take(registry->start_dependency(node.operation, request));
    require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
                empty->poll_count() == 0,
            "empty reduction reads nothing");
    request.outputs = take(ps::Footprint::from_regions(
        {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
    auto session = take(registry->start_dependency(node.operation, request));
    require(session->poll().ok(), "typed reduction first need");
    unsigned data = 0, validation = 0;
    for (const auto& need : take(session->pending_reads())) {
      if (need.roles & 1)
        data += take(need.samples.element_count());
      if (need.roles & 4)
        validation += take(need.samples.element_count());
    }
    require(operation == "count" ? !data && !validation
                                 : data == 2 && validation == 8,
            "typed closure kept separate from numeric source group");
    ps::CancellationSource cancellation;
    request.cancellation = cancellation.token();
    ps::ResourceBudget resources(ps::ResourceLimits{});
    auto cancelled = take(registry->start_dependency(node.operation, request,
                                                     resources.allocator()));
    require(cancelled->poll().ok(), "need before cancellation");
    cancellation.cancel();
    require(cancelled->poll().status().code == ps::ErrorCode::Cancelled,
            "cancellation after need");
    cancelled.reset();
    require(resources.statistics().live[ps::ResourceKind::Payload] == 0,
            "cancelled accumulator releases payload");
  }
  for (auto ddof : {-1, 2}) {
    auto node = authored("variance", ps::ElementType::Int64,
                         ps::ElementType::Float64, {1}, 0, profile);
    node.parameters["ddof"] = static_cast<std::int64_t>(ddof);
    Fixture fixture(node, array(ps::ElementType::Int64, {1, 2}, {1, 2}));
    const auto answer = fixture.run(take(ps::Footprint::all({1, 1})));
    require(!answer.ok() &&
                answer.status().code == ps::ErrorCode::InvalidArgument &&
                answer.status().detail.origin == ps::FailureOrigin::Schema,
            "invalid ddof preflight");
  }
  std::vector<std::uint64_t> bits(130);
  bits[2] = UINT64_C(0x7ff0000000000011);  // logical [0,1]
  bits[1] = UINT64_C(0xfff0000000000022);  // logical [1,0], earlier in storage
  auto packed = array(ps::ElementType::Float64, {130}, bits);
  auto strided = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {2, 65}}, ps::Region::whole({2, 65}),
      {0, {8, 16}}, packed.storage()));
  for (const std::string operation :
       {"sum", "minimum", "maximum", "mean", "variance", "std"}) {
    auto node = authored(operation, ps::ElementType::Float64,
                         ps::ElementType::Float64, {1, 0}, 0, profile);
    const std::vector<ps::Value> inputs{strided};
    const std::vector<ps::Region> demands{strided.region()};
    ps::OperationInvocation invocation(inputs, demands, node.parameters,
                                       ps::Backend::Cpu, {},
                                       ps::Region::whole({1, 1}));
    auto answer = take(registry->invoke(node.operation, invocation));
    std::uint64_t actual = 0;
    std::memcpy(&actual, answer.bytes().data(), 8);
    require(actual == UINT64_C(0x7ff8000000000011),
            "NaN priority follows logical order across strided windows");
  }
  std::cout << "all seven reducers: typed closure, Empty, cancellation; ddof "
               "preflight and strided NaN priority passed\n";
}

void tail_failure(ps::CpuNumericProfile profile) {
  for (const std::string operation :
       {"sum", "minimum", "maximum", "mean", "variance", "std"}) {
    Fixture fixture(authored(operation, ps::ElementType::Float64,
                             ps::ElementType::Float64, {0}, 0, profile),
                    array(ps::ElementType::Float64, {129},
                          std::vector<std::uint64_t>(129)));
    auto source = std::make_shared<ps::RegionalSource>();
    source->descriptor = {ps::ElementType::Float64, {129}};
    unsigned calls = 0;
    source->read = [&](const ps::Region& region, std::uint8_t* data,
                       std::uint64_t bytes, const ps::BufferAllocator&,
                       const ps::CancellationToken&) {
      ++calls;
      if (region.dimensions()[0].offset == 128)
        return ps::Result<ps::Region>(ps::Status{ps::ErrorCode::OperationFailed,
                                                 "required tail after NaN"});
      std::memset(data, 0, bytes);
      const std::uint64_t nan = UINT64_C(0x7ff0000000000011);
      std::memcpy(data, &nan, 8);
      return ps::Result<ps::Region>(region);
    };
    fixture.bindings.inputs = {{"input", {}, source}};
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContext context(fixture.registry, {1});
    auto result = context.execute(plan.plan, fixture.bindings);
    require(!result.ok() &&
                result.status().message == "required tail after NaN" &&
                calls == 3,
            "NaN must not suppress required later source failure");
  }
  std::cout
      << "all numeric reducers preserve required upstream failure after NaN\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile must be strict, apple or x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      examples(profile);
      streaming(profile);
      metadata_count(profile);
      atom_isolation(profile);
      boundaries(profile);
      tail_failure(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
