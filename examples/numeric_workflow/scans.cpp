#include "photospider/numeric/scans.hpp"

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
    config.maximum_live_bytes = 1048576;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    return context.execute_fragments(snapshot.value(), {{"values", demand}});
  }
};
ps::WorkflowNode authored(bool integral, ps::ElementType source,
                          ps::ElementType destination,
                          const std::vector<std::uint64_t>& axes,
                          ps::CpuNumericProfile profile) {
  const ps::WorkflowInput input = ps::WorkflowInputReference{1};
  return integral ? take(ps::numeric::integral_image_node(
                        1, input, axes, source, destination, profile))
                  : take(ps::numeric::prefix_sum_node(
                        1, input, axes.at(0), source, destination, profile));
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
  unsigned integral = 0, source = 0, destination = 0;
  std::string encoded_shape, encoded_axes, encoded_point;
  while (std::cin >> integral >> source >> destination >> encoded_shape >>
         encoded_axes >> encoded_point) {
    auto shape = list(encoded_shape);
    auto axes = list(encoded_axes);
    auto point = list(encoded_point);
    std::uint64_t count = 1;
    for (auto size : shape)
      count *= size;
    std::vector<std::uint64_t> bits(count);
    for (auto& value : bits)
      std::cin >> std::hex >> value >> std::dec;
    const auto type = static_cast<ps::ElementType>(source);
    const auto target = static_cast<ps::ElementType>(destination);
    Fixture fixture(authored(integral != 0, type, target, axes, profile),
                    array(type, shape, bits));
    for (auto axis : axes)
      ++shape[axis];
    std::vector<ps::RegionDimension> dimensions;
    for (auto at : point)
      dimensions.push_back({at, 1});
    auto answer = fixture.run(
        take(ps::Footprint::from_regions(shape, {ps::Region(dimensions)})));
    if (!answer.ok()) {
      if (answer.status().reason == ps::FailureReason::ArithmeticOverflow)
        std::cout << "overflow\n";
      else
        throw std::runtime_error(answer.status().message);
    } else {
      std::uint64_t value = 0;
      require(answer.value()
                  .values.at("values")
                  .read(point, &value, ps::Value::element_size(target))
                  .ok(),
              "oracle result");
      std::cout << std::hex << value << std::dec << '\n';
    }
  }
}
void check(const ps::DemandResult& result, const ps::Footprint& demand,
           const std::vector<std::uint64_t>& expected, ps::ElementType type) {
  std::size_t index = 0;
  require(demand.visit(
                    [&](const auto& at) {
                      std::uint64_t bits = 0;
                      auto status = result.values.at("values").read(
                          at, &bits, ps::Value::element_size(type));
                      require(
                          index < expected.size() && bits == expected[index++],
                          "scan result bits");
                      return status;
                    },
                    16384)
                  .ok() &&
              index == expected.size(),
          "scan result coverage");
}
void examples(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  for (auto rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(rounding) == 0, "set rounding");
    Fixture prefix(authored(false, Type::Int64, Type::Int64, {0}, profile),
                   array(Type::Int64, {3}, {1, 2, 3}));
    auto line = take(ps::Footprint::all({4}));
    check(take(prefix.run(line)), line, {0, 1, 3, 6}, Type::Int64);
    Fixture integral(authored(true, Type::Int64, Type::Int64, {1, 0}, profile),
                     array(Type::Int64, {2, 2}, {1, 2, 3, 4}));
    auto square = take(ps::Footprint::all({3, 3}));
    check(take(integral.run(square)), square, {0, 0, 0, 0, 1, 3, 0, 4, 10},
          Type::Int64);
    require(fegetround() == rounding, "preserve rounding");
  }
  require(fesetround(FE_TONEAREST) == 0, "restore rounding");
  for (const auto& fixture_bits : std::vector<
           std::pair<std::vector<std::uint64_t>, std::vector<std::uint64_t>>>{
           {{0x8000000000000000, 0x8000000000000000, 0},
            {0, 0x8000000000000000, 0x8000000000000000, 0}},
           {{0x4340000000000000, 0x3ff0000000000000, 0xc340000000000000},
            {0, 0x4340000000000000, 0x4340000000000000, 0x3ff0000000000000}},
           {{0x7fefffffffffffff, 0x7fefffffffffffff, 0xffefffffffffffff},
            {0, 0x7fefffffffffffff, 0x7ff0000000000000, 0x7fefffffffffffff}},
           {{0x7ff0000000000000, 0xfff0000000000000, 0x7ff0000000000042},
            {0, 0x7ff0000000000000, 0x7ff8000000000000, 0x7ff8000000000042}}}) {
    Fixture fixture(authored(false, Type::Float64, Type::Float64, {0}, profile),
                    array(Type::Float64, {3}, fixture_bits.first));
    auto all = take(ps::Footprint::all({4}));
    check(take(fixture.run(all)), all, fixture_bits.second, Type::Float64);
  }
  std::cout << "prefix [1,2,3] -> [0,1,3,6]; integral [[1,2],[3,4]] -> "
               "[[0,0,0],[0,1,3],[0,4,10]]\n";
  std::cout << "exact carry survives rounded overflow, cancellation and "
               "generated NaNs; signed zero/fenv passed\n";
}
void sparse(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  Fixture fixture(authored(false, Type::Int64, Type::Int64, {0}, profile),
                  array(Type::Int64, {3}, {0x7fffffffffffffff, 1, UINT64_MAX}));
  auto demand = take(ps::Footprint::from_regions(
      {4}, {ps::Region({{0, 1}}), ps::Region({{3, 1}})}));
  auto result = fixture.run(demand);
  require(!result.ok() &&
              result.status().reason == ps::FailureReason::ArithmeticOverflow &&
              result.status().detail.scope == ps::FailureScope::Run,
          "unrequested intermediate prefix overflow fails Whole");
  Fixture rectangle(authored(true, Type::Int64, Type::Int64, {0, 1}, profile),
                    array(Type::Int64, {3, 3}, {1, 2, 3, 4, 5, 6, 7, 8, 9}));
  auto corners = take(ps::Footprint::from_regions(
      {4, 4}, {ps::Region({{1, 1}, {3, 1}}), ps::Region({{3, 1}, {1, 1}})}));
  auto answer = take(rectangle.run(corners));
  check(answer, corners, {6, 12}, Type::Int64);
  require(take(answer.dependencies.source_support()).at("input") ==
              take(ps::Footprint::all({3, 3})),
          "Whole integral reads all source");
  auto missing =
      take(ps::Footprint::from_regions({3, 3}, {ps::Region({{1, 2}, {1, 2}})}));
  require(take(answer.dependencies.potential_dirty("input", missing))
                  .at("values") == corners,
          "unselected source invalidates all recorded integral observations");
  std::cout
      << "Whole prefix Run overflow and integral full input/dirty passed\n";
}

void boundaries(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  for (bool integral : {false, true}) {
    auto node = authored(integral, Type::Float32, Type::Float32,
                         integral ? std::vector<std::uint64_t>{0, 1}
                                  : std::vector<std::uint64_t>{1},
                         profile);
    const auto raw =
        array(Type::Float32, {1, 2, 4},
              {0x3f800000, 0, 0, 0x40000000, 0x3f800000, 0, 0, 0x3f800000});
    const auto bad = take(ps::Value::from_storage(
        raw.descriptor(), raw.region(), raw.layout(), raw.storage(), {facet}));
    const std::vector<std::uint64_t> shape =
        integral ? std::vector<std::uint64_t>{2, 3, 4}
                 : std::vector<std::uint64_t>{1, 3, 4};
    Fixture fixture(node, bad);
    require(fixture.run(take(ps::Footprint::none(shape))).ok(),
            "Empty skips Whole typed input");
    const std::vector<ps::Value> inputs{bad};
    const std::vector<ps::Region> demands{bad.region()};
    ps::OperationInvocation call(inputs, demands, node.parameters,
                                 ps::Backend::Cpu, {},
                                 ps::Region::whole(shape));
    require(!registry->invoke(node.operation, call).ok(),
            "Whole scans validate every typed input");
    auto zero = take(ps::Footprint::from_regions(
        shape, {ps::Region({{0, 1}, {0, 1}, {0, 1}})}));
    require(!fixture.run(zero).ok(),
            "zero boundary still requires valid Whole source");
    ps::CancellationSource cancellation;
    cancellation.cancel();
    ps::OperationInvocation stopped(inputs, demands, node.parameters,
                                    ps::Backend::Cpu, cancellation.token(),
                                    ps::Region::whole(shape));
    require(registry->invoke(node.operation, stopped).status().code ==
                ps::ErrorCode::Cancelled,
            "Whole scan pre-cancelled");
  }
  const auto packed =
      array(Type::Float32, {3}, {0x3f800000, 0x40000000, 0x40400000});
  const auto strided =
      take(ps::Value::from_storage({Type::Float32, {3}}, ps::Region::whole({3}),
                                   {8, {-4}}, packed.storage()));
  const auto node = authored(false, Type::Float32, Type::Float32, {0}, profile);
  const std::vector<ps::Value> inputs{strided};
  const std::vector<ps::Region> regions{strided.region()};
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save fenv");
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "prepare flags");
    ps::OperationInvocation invocation(inputs, regions, node.parameters,
                                       ps::Backend::Cpu, {},
                                       ps::Region::whole({4}));
    auto value = take(registry->invoke(node.operation, invocation));
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "scan preserves flags");
    const std::vector<std::uint64_t> expected{0, 0x40400000, 0x40a00000,
                                              0x40c00000};
    for (unsigned j = 0; j < expected.size(); ++j) {
      std::uint32_t bits = 0;
      std::memcpy(&bits, value.bytes().data() + j * 4, 4);
      require(bits == expected[j], "negative stride prefix");
    }
  }
  require(fesetenv(&saved) == 0, "restore fenv");
  for (bool integral : {false, true}) {
    auto bad = authored(integral, Type::Int64, Type::Int64,
                        integral ? std::vector<std::uint64_t>{0, 1}
                                 : std::vector<std::uint64_t>{0},
                        profile);
    ps::DependencyRequest request;
    request.inputs = {
        {{Type::Int64, integral
                           ? std::vector<std::uint64_t>{1, UINT64_C(1) << 40}
                           : std::vector<std::uint64_t>{UINT64_C(1) << 40}},
         {}}};
    request.parameters = bad.parameters;
    request.snapshot_identity = "scan-over-cap";
    request.outputs = take(ps::Footprint::none({1}));
    auto answer = registry->resolve_traits(bad.operation, request.inputs,
                                           request.parameters);
    require(!answer.ok() &&
                answer.status().code == ps::ErrorCode::TypeMismatch &&
                answer.status().detail.origin == ps::FailureOrigin::Schema,
            "output shape cap preflight");
  }
  for (bool zero : {false, true}) {
    auto bytes = take(ps::BufferAllocator{}.allocate(33));
    const double raw[] = {1, 2, 3, 4};
    std::memcpy(bytes.data() + 1, raw, 32);
    auto value = take(ps::Value::from_storage(
        {Type::Float64, {2, 2}}, ps::Region::whole({2, 2}),
        {zero ? 1U : 25U, {zero ? 0 : -16, zero ? 0 : -8}},
        std::move(bytes).freeze()));
    auto integral =
        authored(true, Type::Float64, Type::Float64, {1, 0}, profile);
    const std::vector<ps::Value> values{value};
    const std::vector<ps::Region> regions{value.region()};
    ps::OperationInvocation call(values, regions, integral.parameters);
    auto result = take(registry->invoke(integral.operation, call));
    const double expected[] = {0, 0, 0, 0, 4, 7, 0, 6, 10};
    for (unsigned y = 0; y < 3; ++y)
      for (unsigned x = 0; x < 3; ++x) {
        double actual = 0;
        std::memcpy(&actual,
                    result.bytes().data() + take(result.byte_address({y, x})),
                    8);
        require(actual == (zero ? y * x : expected[y * 3 + x]),
                "unaligned negative/zero integral layout");
      }
  }
  std::cout << "typed closure, Empty/zero reads, cancellation/release, output "
               "cap and negative strides/fenv flags passed\n";
}
void sort_interruption(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (bool integral : {false, true}) {
    auto node =
        authored(integral, ps::ElementType::Int64, ps::ElementType::Int64,
                 integral ? std::vector<std::uint64_t>{0, 1}
                          : std::vector<std::uint64_t>{1},
                 profile);
    auto input = array(ps::ElementType::Int64, {64, 64},
                       std::vector<std::uint64_t>(4096, 1));
    point_math_checks::resources(node, {input},
                                 (integral ? 65 * 65 : 64 * 65) * 8);
    if (integral) {
      ps::ResourceLimits limits;
      limits.capacity[ps::ResourceKind::Metadata] = 128;
      ps::ResourceBudget budget(limits);
      {
        ps::ResourceAllocationScope scope(budget);
        const std::vector<ps::Value> inputs{input};
        const std::vector<ps::Region> demands{input.region()};
        ps::OperationInvocation call(
            inputs, demands, node.parameters, ps::Backend::Cpu, {},
            ps::Region::whole({65, 65}), budget.allocator());
        auto answer = registry->invoke(node.operation, call);
        require(!answer.ok() &&
                    answer.status().code == ps::ErrorCode::ResourceExhausted,
                "integral exact column metadata budget");
      }
      require(budget.statistics().live[ps::ResourceKind::Metadata] == 0 &&
                  budget.statistics().live[ps::ResourceKind::Payload] == 0,
              "integral metadata failure releases state/output");
    }
  }
  std::cout << "Whole scans: work/output/scratch, exact column metadata and "
               "active cancellation passed\n";
}
void streaming(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  Fixture fixture(
      authored(false, Type::Int64, Type::Int64, {0}, profile),
      array(Type::Int64, {4096}, std::vector<std::uint64_t>(4096, 1)));
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 131072;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  auto demand = take(ps::Footprint::from_regions(
      {4097}, {ps::Region({{0, 1}}), ps::Region({{64, 1}}),
               ps::Region({{129, 1}}), ps::Region({{4096, 1}})}));
  ps::ExecutionOptions options;
  options.maximum_dependency_cache_work = 0;
  options.dependencies.maximum_work = 128 * 1024 * 1024;
  options.maximum_dependency_work = 128 * 1024 * 1024;
  auto snapshot = take(context.freeze(plan.plan, fixture.bindings));
  auto result = take(
      context.execute_fragments(snapshot, {{"values", demand}}, {}, options));
  check(result, demand, {0, 64, 129, 4096}, Type::Int64);
  require(result.diagnostics.peak_live_bytes <= 131072, "bounded scan payload");
  const auto changed =
      take(ps::Footprint::from_regions({4096}, {ps::Region({{64, 1}})}));
  require(take(result.dependencies.potential_dirty("input", changed))
                  .at("values") == demand,
          "Whole edit invalidates all recorded prefixes including boundary");
  std::cout << "4096 source terms scanned once, complete output, sparse "
               "projection and Whole dirty passed\n";
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
      sparse(profile);
      boundaries(profile);
      streaming(profile);
      sort_interruption(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
