#include "photospider/numeric/scans.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
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
void check_numeric(const ps::DemandResult& result,
                   ps::CpuNumericProfile profile, std::uint64_t expected,
                   bool executed = true) {
  std::uint64_t evaluated = 0;
  bool reported = false;
  for (const auto& timing : result.diagnostics.operation_timings) {
    const auto& numeric = timing.numeric;
    evaluated += numeric.evaluated_values;
    require(numeric.strict_fallbacks == 0 && numeric.strict_math_calls == 0 &&
                numeric.fallback_reasons == std::array<std::uint64_t, 4>{},
            "exact scan reports no unperformed mathematical fallback");
    if (numeric.profile != ps::CpuNumericProfile::Unspecified) {
      reported = true;
      require(numeric.profile == profile && numeric.implementation[0] &&
                  !numeric.implementation.back(),
              "scan reports selected profile and owning identity");
    }
  }
  require(evaluated == expected && (!executed || reported),
          "scan counts accumulator inputs rather than boundary outputs");
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
  ps::ResourceBudget last_root;
  Fixture(ps::WorkflowNode node, ps::Value value) : backing{std::move(value)} {
    rf::declare_sources(&document, backing);
    document.inputs[0].name = "input";
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::ExecutionBindings bind(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::DemandResult> run(const ps::Footprint& demand,
                                   const ps::CancellationToken& stop = {}) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 1048576;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    last_root = take(context.resource_budget());
    auto snapshot = context.freeze(plan.value().plan, bind(last_root));
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_cache_work = 0;
    return context.execute_fragments(snapshot.value(), {{"values", demand}},
                                     stop, options);
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
      require(rf::read(answer.value().results.at("values"), point, &value,
                       ps::Value::element_size(target))
                  .ok(),
              "oracle result");
      std::cout << std::hex << value << std::dec << '\n';
    }
  }
}
void check(const ps::DemandResult& result, const ps::Footprint& demand,
           const std::vector<std::uint64_t>& expected, ps::ElementType type) {
  std::size_t index = 0;
  require(
      demand.visit(
                [&](const auto& at) {
                  std::uint64_t bits = 0;
                  auto status = rf::read(result.results.at("values"), at, &bits,
                                         ps::Value::element_size(type));
                  require(index < expected.size() && bits == expected[index++],
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
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save caller fenv");
  for (auto rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(rounding) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "set caller rounding and flags");
    Fixture prefix(authored(false, Type::Int64, Type::Int64, {0}, profile),
                   array(Type::Int64, {3}, {1, 2, 3}));
    auto line = take(ps::Footprint::all({4}));
    auto prefix_result = take(prefix.run(line));
    check_numeric(prefix_result, profile, 3);
    check(prefix_result, line, {0, 1, 3, 6}, Type::Int64);
    Fixture integral(authored(true, Type::Int64, Type::Int64, {1, 0}, profile),
                     array(Type::Int64, {2, 2}, {1, 2, 3, 4}));
    auto square = take(ps::Footprint::all({3, 3}));
    auto integral_result = take(integral.run(square));
    check_numeric(integral_result, profile, 4);
    check(integral_result, square, {0, 0, 0, 0, 1, 3, 0, 4, 10}, Type::Int64);
    require(
        fegetround() == rounding && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
        "preserve caller rounding and flags");
  }
  require(fesetenv(&saved) == 0, "restore caller fenv");
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
  std::cout << "exact carry survives rounded overflow, exact cancellation and "
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
  point_math_checks::released(fixture.last_root);
  Fixture overflowing(
      authored(true, Type::Int64, Type::Int64, {0, 1}, profile),
      array(Type::Int64, {2, 2}, {0x7fffffffffffffff, 1, UINT64_MAX, 0}));
  auto failed = overflowing.run(take(
      ps::Footprint::from_regions({3, 3}, {ps::Region({{2, 1}, {2, 1}})})));
  require(!failed.ok() &&
              failed.status().code == ps::ErrorCode::OperationFailed &&
              failed.status().reason == ps::FailureReason::ArithmeticOverflow &&
              failed.status().detail.origin == ps::FailureOrigin::Domain &&
              failed.status().detail.scope == ps::FailureScope::Run &&
              !failed.status().detail.atom &&
              failed.status().message.find("output=[1,2,") != std::string::npos,
          "unrequested rectangle overflow fails Run despite representable "
          "requested sum");
  point_math_checks::released(overflowing.last_root);
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
  ps::ColorArrayDescriptor color;
  color.model = ps::ColorModel::Rgb;
  color.association = ps::ColorAssociation::Straight;
  color.primaries =
      take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
          .primaries;
  color.transfer = ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
  for (bool integral : {false, true}) {
    auto node = authored(integral, Type::Float32, Type::Float32,
                         integral ? std::vector<std::uint64_t>{0, 1}
                                  : std::vector<std::uint64_t>{1},
                         profile);
    Fixture fixture(node, array(Type::Float32, {1, 2, 4},
                                {0x3f800000, 0, 0, 0x40000000, 0x3f800000, 0, 0,
                                 0x3f800000}));
    auto schema = *fixture.document.inputs[0].result_schema;
    schema.tensors[0].facets = {take(ps::encode_color_array(color))};
    schema.tensors[0].atomic_trailing_axes = 1;
    fixture.document.inputs[0].result_schema =
        std::make_shared<ps::SchemaTemplate>(schema);
    const std::vector<std::uint64_t> shape =
        integral ? std::vector<std::uint64_t>{2, 3, 4}
                 : std::vector<std::uint64_t>{1, 3, 4};
    auto empty = take(fixture.run(take(ps::Footprint::none(shape))));
    check_numeric(empty, profile, 0, false);
    require(take(empty.results.at("values").descriptor())
                .tensor_coverage(0)
                .empty(),
            "Empty scan skips invalid typed payload");
    for (const auto& timing : empty.diagnostics.operation_timings)
      require(timing.computed_elements == 0, "Empty scan has no arithmetic");
    for (const auto& support : take(empty.dependencies.source_support()))
      require(support.second.empty(), "Empty scan has no source samples");
    auto green_zero = take(ps::Footprint::from_regions(
        shape, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
    auto failed = fixture.run(green_zero);
    require(!failed.ok() &&
                failed.status().code == ps::ErrorCode::InvalidArgument &&
                failed.status().reason == ps::FailureReason::InvalidDomain &&
                failed.status().detail.input_id == 1,
            "zero-boundary green still requires full typed alpha validation");
    ps::CancellationSource stop;
    stop.cancel();
    require(fixture.run(green_zero, stop.token()).status().code ==
                ps::ErrorCode::Cancelled,
            "pre-cancelled scan preserves host category");
    fixture.backing[0] =
        array(Type::Float32, {1, 2, 4},
              {0x3f800000, 0, 0, 0x3f800000, 0x3f800000, 0, 0, 0x3f800000});
    auto legal = take(fixture.run(green_zero));
    const auto& output = legal.results.at("values");
    require(take(output.descriptor()).tensor_coverage(0) ==
                    take(ps::Footprint::all(shape)) &&
                output.schema().tensors[0].facets.empty() &&
                take(legal.dependencies.source_support()).at("input") ==
                    take(ps::Footprint::all({1, 2, 4})),
            "same-schema legal typed input yields complete generic output");
    for (unsigned channel = 0; channel < 4; ++channel) {
      std::uint64_t zero = 1, last = 0;
      require(rf::read(output, {0, 0, channel}, &zero, 4).ok() && zero == 0 &&
                  rf::read(output, {integral ? 1U : 0U, 2, channel}, &last, 4)
                      .ok() &&
                  last == (channel == 0 || channel == 3 ? 0x40000000 : 0),
              "typed scan zero and final boundaries");
    }
    const auto edit = take(ps::Footprint::from_regions(
        {1, 2, 4}, {ps::Region({{0, 1}, {1, 1}, {3, 1}})}));
    for (unsigned role : {1U, 4U})
      require(
          take(legal.dependencies.potential_dirty("input", edit, role))
                  .at("values") == green_zero,
          "Data and Validation edits invalidate even observed zero boundary");
  }
  auto registry = ps::make_default_operation_registry();
  for (bool integral : {false, true}) {
    auto node = authored(integral, Type::Int64, Type::Int64,
                         integral ? std::vector<std::uint64_t>{0, 1}
                                  : std::vector<std::uint64_t>{0},
                         profile);
    ps::SchemaTemplate schema;
    schema.id = "manual.scan.large";
    ps::ResultTensorSpec tensor;
    tensor.key = "data";
    tensor.descriptor = {
        Type::Int64, integral ? std::vector<std::uint64_t>{1, UINT64_C(1) << 40}
                              : std::vector<std::uint64_t>{UINT64_C(1) << 40}};
    schema.tensors.push_back(tensor);
    auto rejected = registry->resolve_traits(
        node.operation,
        {ps::OperationMetadata{{},
                               {},
                               std::make_shared<ps::SchemaTemplate>(schema)}},
        node.parameters);
    require(!rejected.ok() &&
                rejected.status().code == ps::ErrorCode::TypeMismatch &&
                rejected.status().detail.origin == ps::FailureOrigin::Schema,
            "grown output exceeds logical element cap at metadata preflight");
  }
  auto node = authored(true, Type::Int64, Type::Int64, {0, 1}, profile);
  node.parameters["axes"] = std::string("0,0");
  Fixture duplicate(node, array(Type::Int64, {2, 2}, {1, 2, 3, 4}));
  require(duplicate.run(take(ps::Footprint::all({3, 3}))).status().code ==
              ps::ErrorCode::InvalidArgument,
          "duplicate integral axes rejected before runtime");
  node = authored(false, Type::Float32, Type::Float32, {0}, profile);
  node.parameters["dtype"] = std::string("int64");
  Fixture domain(node, array(Type::Float32, {2}, {0x3f800000, 0}));
  require(domain.run(take(ps::Footprint::all({3}))).status().code ==
              ps::ErrorCode::TypeMismatch,
          "cross-domain scan destination rejected at metadata preflight");
  std::cout << "Result scans: typed closure/positive controls, zero boundary, "
               "Empty, pre-cancellation, shape and parameter checks passed\n";
}
void layouts_and_environment(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto packed =
      array(Type::Float32, {3}, {0x3f800000, 0x40000000, 0x40400000});
  const auto strided =
      take(ps::Value::from_storage({Type::Float32, {3}}, ps::Region::whole({3}),
                                   {8, {-4}}, packed.storage()));
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save fenv");
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "set caller fenv");
    auto control = std::make_shared<point_math_checks::Control>();
    control->rounding = mode;
    {
      point_math_checks::Workflow workflow(
          authored(false, Type::Float32, Type::Float32, {0}, profile),
          {strided}, {}, control);
      auto result = take(workflow.run()).results.at("values");
      const std::uint64_t expected[] = {0, 0x40400000, 0x40a00000, 0x40c00000};
      for (unsigned i = 0; i < 4; ++i) {
        std::uint64_t actual = 0;
        require(rf::read(result, {i}, &actual, 4).ok() && actual == expected[i],
                "negative stride prefix");
      }
      require(control->computation_polls == 1,
              "prefix enters actual worker computation");
    }
    for (bool zero : {false, true}) {
      auto bytes = take(ps::BufferAllocator{}.allocate(33));
      const double raw[] = {1, 2, 3, 4};
      std::memcpy(bytes.data() + 1, raw, 32);
      auto value = take(ps::Value::from_storage(
          {Type::Float64, {2, 2}}, ps::Region::whole({2, 2}),
          {zero ? 1U : 25U, {zero ? 0 : -16, zero ? 0 : -8}},
          std::move(bytes).freeze()));
      control = std::make_shared<point_math_checks::Control>();
      control->rounding = mode;
      point_math_checks::Workflow workflow(
          authored(true, Type::Float64, Type::Float64, {1, 0}, profile),
          {value}, {}, control);
      auto output = take(workflow.run()).results.at("values");
      const double expected[] = {0, 0, 0, 0, 4, 7, 0, 6, 10};
      for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 3; ++x) {
          double actual = -1;
          require(rf::read(output, {y, x}, &actual, 8).ok() &&
                      actual == (zero ? y * x : expected[y * 3 + x]),
                  "unaligned negative/zero integral layout");
        }
      require(control->computation_polls == 1,
              "integral enters actual worker computation");
    }
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "scan preserves caller and actual worker rounding/flags");
  }
  require(fesetenv(&saved) == 0, "restore fenv");
  std::cout << "negative/unaligned/zero strides and four caller/worker fenv "
               "modes passed\n";
}
void batch_axes(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  std::vector<std::uint64_t> bits(48);
  for (unsigned i = 0; i < 48; ++i)
    bits[i] = i + 1;
  const auto packed = array(Type::Int64, {2, 3, 2, 4}, bits);
  const auto reversed =
      take(ps::Value::from_storage(packed.descriptor(), packed.region(),
                                   {24, {192, 64, 32, -8}}, packed.storage()));
  for (bool integral : {false, true}) {
    Fixture fixture(authored(integral, Type::Int64, Type::Int64,
                             integral ? std::vector<std::uint64_t>{3, 1}
                                      : std::vector<std::uint64_t>{0},
                             profile),
                    reversed);
    auto schema = *fixture.document.inputs[0].result_schema;
    schema.tensors[0].descriptor.shape = {3, 2, 4};
    schema.tensors[0].batch_axes = {2};
    fixture.document.inputs[0].result_schema =
        std::make_shared<ps::SchemaTemplate>(schema);
    const std::vector<std::uint64_t> shape =
        integral ? std::vector<std::uint64_t>{2, 4, 2, 5}
                 : std::vector<std::uint64_t>{3, 3, 2, 4};
    auto answer = take(fixture.run(take(ps::Footprint::all(shape))));
    const auto& output = answer.results.at("values");
    require(output.schema().tensors[0].sample_shape() == shape &&
                output.schema().tensors[0].batch_axes.empty() &&
                output.schema().tensors[0].facets.empty(),
            "batch-prefix axes become ordinary scan output axes");
    require(take(ps::Footprint::all(shape))
                .visit(
                    [&](const auto& at) {
                      std::uint64_t expected = 0;
                      const auto source_value =
                          [](std::uint64_t batch, std::uint64_t row,
                             std::uint64_t channel, std::uint64_t column) {
                            return ((batch * 3 + row) * 2 + channel) * 4 +
                                   (3 - column) + 1;
                          };
                      if (integral) {
                        for (std::uint64_t row = 0; row < at[1]; ++row)
                          for (std::uint64_t column = 0; column < at[3];
                               ++column)
                            expected += source_value(at[0], row, at[2], column);
                      } else {
                        for (std::uint64_t batch = 0; batch < at[0]; ++batch)
                          expected += source_value(batch, at[1], at[2], at[3]);
                      }
                      std::uint64_t actual = 0;
                      auto status = rf::read(output, at, &actual, 8);
                      require(actual == expected,
                              "independent nonadjacent integral/scan batch "
                              "enumeration");
                      return status;
                    },
                    1024)
                .ok(),
            "batch scan output traversal");
  }
  std::cout << "Result batch axes, nonadjacent integral axes and per-plane "
               "carry reset passed\n";
}

struct ScanControl {
  bool limit_metadata = false, admitted = false;
  unsigned computations = 0;
  std::uint64_t additions = 0;
  std::uint64_t cancel_after_add = UINT64_MAX;
  ps::CancellationSource cancellation;
  ps::NumericDiagnostics numeric;
};
struct ObservedScan {
  ps::ResultContinuation original;
  std::shared_ptr<ScanControl> control;
  ObservedScan(ps::ResultContinuation program,
               std::shared_ptr<ScanControl> state)
      : original(std::move(program)), control(std::move(state)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    auto observed = phase;
    const bool computation = phase.tensors && !phase.tensors->empty();
    if (computation)
      ++control->computations;
    unsigned ones = 0;
    std::optional<ps::ResourceLease> reservation;
    observed.consume_work = [&](std::uint64_t amount) {
      auto status = phase.consume_work(amount);
      if (!status.ok())
        return status;
      // Int64 sums charge 512 at ExactAggregate::add. Integer conversion
      // charges 128; no ratio.round runs on this source/destination pair.
      if (computation && amount == 512) {
        ++control->additions;
        if (control->additions == control->cancel_after_add)
          control->cancellation.cancel();
      }
      // The first 1 accounts the one supplied tensor; the second enters
      // ScanKernel::write after its output writer and relation are ready.
      if (computation && amount == 1 && ++ones == 2 &&
          control->limit_metadata) {
        const auto remaining =
            UINT64_C(1000000) -
            phase.resources.statistics().live[ps::ResourceKind::Metadata];
        require(remaining > 2048,
                "metadata fixture leaves room for exact state");
        auto admitted = phase.resources.reserve(
            ps::ResourceCapacity::host(remaining - 2048, remaining - 2048));
        if (!admitted.ok())
          return admitted.status();
        reservation = admitted.take_value();
        control->admitted = true;
      }
      return status;
    };
    observed.report_numeric = [&](const ps::NumericDiagnostics& report) {
      auto status = phase.report_numeric(report);
      if (status.ok())
        status = ps::merge_numeric_diagnostics(&control->numeric, report);
      return status;
    };
    return original.poll(observed);
  }
};
ps::WorkflowNode observe_scan(
    const std::shared_ptr<ps::OperationRegistry>& registry,
    ps::WorkflowNode node, const std::shared_ptr<ScanControl>& control) {
  const auto key = node.operation;
  const std::weak_ptr<ps::OperationRegistry> owner = registry;
  ps::OperationDefinition adapter;
  adapter.key = "manual.observe." + key;
  adapter.traits = take(registry->find_traits(key));
  adapter.traits.cacheable = false;
  adapter.traits.outputs[0].continuation_bytes += sizeof(ObservedScan);
  adapter.prepare_static =
      [owner, key](
          const auto& inputs,
          const auto& parameters) -> ps::Result<ps::OperationPreparation> {
    auto registry = owner.lock();
    if (!registry)
      return ps::Result<ps::OperationPreparation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    auto original = registry->prepare_operation(key, inputs, parameters);
    if (!original.ok())
      return ps::Result<ps::OperationPreparation>(original.status());
    ps::OperationPreparation prepared;
    prepared.outputs.resize(1);
    prepared.outputs[0].metadata.result_schema =
        std::make_shared<ps::SchemaTemplate>(
            *original.value()->traits().outputs[0].result_schema);
    prepared.state = original.take_value();
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  adapter.start_result = [owner, key, control](const auto& query,
                                               const auto& allocator) {
    auto registry = owner.lock();
    if (!registry)
      return ps::Result<ps::ResultContinuation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    auto forwarded = query;
    forwarded.prepared = std::shared_ptr<const ps::PreparedOperation>(
        query.prepared,
        static_cast<const ps::PreparedOperation*>(query.prepared->state()));
    auto original = registry->start_result(key, forwarded, allocator);
    if (!original.ok())
      return original;
    return ps::ResultContinuation::make<ObservedScan>(
        allocator, original.take_value(), control);
  };
  node.operation = adapter.key;
  require(registry->register_operation(std::move(adapter)).ok(),
          "register observed Result scan");
  return node;
}
void metadata_limit(const ps::WorkflowNode& node, const ps::Value& input) {
  Fixture fixture(node, input);
  fixture.registry = ps::make_default_operation_registry(false);
  auto control = std::make_shared<ScanControl>();
  control->limit_metadata = true;
  fixture.document.nodes[0] = observe_scan(fixture.registry, node, control);
  require(fixture.registry->freeze().ok(), "freeze metadata-limited scan");
  ps::ResourceBudget root;
  {
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->capacity[ps::ResourceKind::Metadata] = 1000000;
    ps::ExecutionContext context(fixture.registry, config);
    root = take(context.resource_budget());
    auto snapshot = take(context.freeze(plan.plan, fixture.bind(root)));
    auto answer = context.execute(snapshot);
    require(!answer.ok() &&
                answer.status().code == ps::ErrorCode::ResourceExhausted &&
                answer.status().reason == ps::FailureReason::CapacityLimit &&
                control->admitted && control->computations == 1 &&
                control->additions == 0,
            "integral exact column capacity fails inside real kernel before "
            "source arithmetic");
  }
  point_math_checks::released(root);
}
void interruption(ps::CpuNumericProfile profile) {
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
    if (integral)
      metadata_limit(node, input);
    for (bool overflow : {false, true}) {
      const auto data =
          overflow ? array(ps::ElementType::Int64, {1, 3},
                           {UINT64_C(0x7fffffffffffffff), 1, UINT64_MAX})
                   : array(ps::ElementType::Int64, {1, 3}, {1, 2, 3});
      Fixture fixture(
          authored(integral, ps::ElementType::Int64, ps::ElementType::Int64,
                   integral ? std::vector<std::uint64_t>{0, 1}
                            : std::vector<std::uint64_t>{1},
                   profile),
          data);
      fixture.registry = ps::make_default_operation_registry(false);
      auto control = std::make_shared<ScanControl>();
      if (!overflow)
        control->cancel_after_add = 2;
      fixture.document.nodes[0] =
          observe_scan(fixture.registry, fixture.document.nodes[0], control);
      require(fixture.registry->freeze().ok(),
              "freeze failing numeric diagnostic scan");
      ps::GraphContext graph(fixture.document);
      auto plan = take(ps::Compiler(fixture.registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.result_cache_bytes = 0;
      ps::ExecutionContext context(fixture.registry, config);
      auto snapshot = take(context.freeze(
          plan.plan, fixture.bind(take(context.resource_budget()))));
      auto failed = context.execute(snapshot, control->cancellation.token());
      require(
          !failed.ok() &&
              (overflow ? failed.status().reason ==
                              ps::FailureReason::ArithmeticOverflow
                        : failed.status().code == ps::ErrorCode::Cancelled) &&
              control->additions == 2 &&
              (!overflow || (control->numeric.evaluated_values == 2 &&
                             control->numeric.profile == profile &&
                             control->numeric.strict_fallbacks == 0)),
          "arithmetic failure reports two actual input attempts; active "
          "cancellation keeps its original status");
    }
  }
  std::cout << "Whole Result scans: work/output/scratch limits, exact columns, "
               "active cancellation and all Root release passed\n";
}
struct SourceControl {
  unsigned starts = 0, polls = 0;
  std::uint64_t published = 0, object_id = 0;
  ps::ResultRef prefix;
};
struct SourceProgram {
  std::shared_ptr<SourceControl> control;
  bool fail_tail;
  std::optional<ps::ResultBuilder> builder;
  SourceProgram(std::shared_ptr<SourceControl> state, bool fail)
      : control(std::move(state)), fail_tail(fail) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    ++control->polls;
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    if (fail_tail && builder) {
      const auto failure = ps::Status{ps::ErrorCode::OperationFailed,
                                      "required scan tail after NaN"};
      builder->fail(failure);
      return ps::Result<ps::ResultProgramPoll>(failure);
    }
    require(phase.query.tensor_outputs &&
                *phase.query.tensor_outputs == take(ps::Footprint::all(shape)),
            "scan source producer receives complete Whole Need even at zero "
            "boundary");
    builder.emplace(take(ps::ResultBuilder::start(phase.resources, schema,
                                                  phase.query.semantic_key)));
    require(builder
                ->bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "source descriptor relation");
    const auto count = fail_tail ? 128U : shape.back();
    const auto region = shape.size() == 1 ? ps::Region({{0, count}})
                                          : ps::Region({{0, 1}, {0, count}});
    auto status = builder->publish_tensor_kernel(
        0, region,
        [&](const auto& writers) -> ps::Status {
          require(writers.size() == 1, "one producer writer");
          for (std::uint64_t i = 0; i < count; ++i) {
            auto admitted = phase.consume_work(1);
            if (!admitted.ok())
              return admitted;
            const std::uint64_t bits =
                fail_tail ? (i ? 0 : UINT64_C(0x7ff0000000000011)) : 1;
            const std::vector<std::uint64_t> at =
                shape.size() == 1 ? std::vector<std::uint64_t>{i}
                                  : std::vector<std::uint64_t>{0, i};
            auto row = writers[0].row_run(at);
            if (!row.ok())
              return row.status();
            std::memcpy(row.value().data, &bits, 8);
          }
          return ps::Status::success();
        },
        take(ps::ResultRelation::cartesian(
            phase.resources, take(schema.tensors[0].sample_count()), {})),
        {true, true, true, true}, phase.query.cancellation);
    if (!status.ok())
      return ps::Result<ps::ResultProgramPoll>(status);
    control->published += count;
    control->object_id = builder->reference().object_id();
    if (fail_tail)
      control->prefix = builder->reference();
    return ps::Result<ps::ResultProgramPoll>(ps::ResultPublication{
        fail_tail ? builder->reference() : take(builder->seal()), !fail_tail});
  }
};
void source_node(Fixture* fixture,
                 const std::shared_ptr<SourceControl>& control,
                 bool fail_tail) {
  ps::OperationDefinition source;
  source.key = "manual.scan_source";
  source.traits.input_count = 0;
  source.traits.input_schema.clear();
  auto& output = source.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.result_schema = *fixture->document.inputs[0].result_schema;
  if (fail_tail)
    output.result_schema->publication = ps::PublishPolicy::StablePrefix;
  output.output_schema.result_schema_id = output.result_schema->id;
  output.output_schema.result_schema_version = output.result_schema->version;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 2;
  output.regional_atomic = true;
  output.continuation_bytes = sizeof(SourceProgram);
  output.maximum_dependency_stages = fail_tail ? 2 : 1;
  source.start_result = [control, fail_tail](const auto&,
                                             const auto& allocator) {
    ++control->starts;
    return ps::ResultContinuation::make<SourceProgram>(allocator, control,
                                                       fail_tail);
  };
  require(fixture->registry->register_operation(std::move(source)).ok(),
          "register scan Result producer");
  fixture->document.nodes[0].inputs = {ps::WorkflowNodeOutput{2, "value"}};
  fixture->document.nodes.push_back({2, "manual.scan_source", {}, {}});
  fixture->document.inputs.clear();
  fixture->backing.clear();
}
void large_scan_lifetime(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  Fixture fixture(
      authored(false, Type::Int64, Type::Int64, {0}, profile),
      array(Type::Int64, {4096}, std::vector<std::uint64_t>(4096, 1)));
  fixture.registry = ps::make_default_operation_registry(false);
  auto source = std::make_shared<SourceControl>();
  auto scan = std::make_shared<ScanControl>();
  fixture.document.nodes[0] =
      observe_scan(fixture.registry, fixture.document.nodes[0], scan);
  source_node(&fixture, source, false);
  require(fixture.registry->freeze().ok(), "freeze generated scan");
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ResourceBudget root;
  ps::ResultRef retained;
  ps::ResultTensorReadWindow window;
  {
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = 131072;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    root = take(context.resource_budget());
    const auto demand = take(ps::Footprint::from_regions(
        {4097}, {ps::Region({{0, 1}}), ps::Region({{64, 1}}),
                 ps::Region({{129, 1}}), ps::Region({{4096, 1}})}));
    ps::ExecutionOptions options;
    options.maximum_dependency_cache_work = 0;
    options.dependencies.maximum_work = 128 * 1024 * 1024;
    options.maximum_dependency_work = 128 * 1024 * 1024;
    auto snapshot = take(context.freeze(plan.plan, {}));
    auto answer = take(
        context.execute_fragments(snapshot, {{"values", demand}}, {}, options));
    check(answer, demand, {0, 64, 129, 4096}, Type::Int64);
    retained = answer.results.at("values");
    require(take(retained.descriptor()).tensor_coverage(0) ==
                    take(ps::Footprint::all({4097})) &&
                retained.association() ==
                    ps::ResourceVector<std::uint64_t>{source->object_id} &&
                source->starts == 1 && source->polls == 1 &&
                source->published == 4096 && scan->computations == 1 &&
                scan->additions == 4096,
            "4096 generated Result terms are scanned once into complete "
            "boundary output");
    const auto peak = root.statistics().peak[ps::ResourceKind::Payload];
    require(peak >= 65544 && peak <= 131072,
            "actual Root peak admits source and complete output under cap");
    window = take(retained.acquire_tensor(take(retained.descriptor()), 0,
                                          ps::Region::whole({4097})));
  }
  require(root.statistics().live[ps::ResourceKind::Payload] == 32776,
          "escaped dense scan retains only 4097 output words after "
          "source/context retire");
  retained = {};
  const auto row = take(window.row_run({4096}));
  std::uint64_t last = 0;
  std::memcpy(&last, row.data, 8);
  require(last == 4096 &&
              root.statistics().live[ps::ResourceKind::Payload] == 32776,
          "read window owns dense scan after Result handle retires");
  window = {};
  point_math_checks::released(root);
  std::cout << "4096 generated Result terms, actual bounded Root peak, escaped "
               "dense output and window lifetime passed\n";
}
void cache_and_lifetime(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  for (bool integral : {false, true}) {
    Fixture fixture(authored(integral, Type::Int64, Type::Int64,
                             integral ? std::vector<std::uint64_t>{0, 1}
                                      : std::vector<std::uint64_t>{0},
                             profile),
                    integral ? array(Type::Int64, {2, 2}, {1, 2, 3, 4})
                             : array(Type::Int64, {3}, {1, 2, 3}));
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    const auto query =
        integral ? take(ps::Footprint::from_regions(
                       {3, 3}, {ps::Region({{0, 1}, {0, 1}}),
                                ps::Region({{1, 1}, {1, 1}})}))
                 : take(ps::Footprint::from_regions(
                       {4}, {ps::Region({{0, 1}}), ps::Region({{3, 1}})}));
    const std::uint64_t output_count = integral ? 9 : 4;
    ps::ResourceBudget root;
    ps::ResultRef retained;
    ps::ResultTensorReadWindow window;
    {
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.result_cache_bytes = 65536;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      root = take(context.resource_budget());
      auto bindings = fixture.bind(root);
      auto demand = take(context.open_demand(plan.plan, bindings));
      auto cold = take(demand.request({{"values", query}}));
      check_numeric(cold, profile, integral ? 4 : 3);
      retained = cold.results.at("values");
      require(take(demand.request({{"values", query}}))
                      .results.at("values")
                      .object_id() == retained.object_id(),
              "same frozen demand shares scan Result identity");
      auto fresh_bindings = fixture.bind(root);
      auto fresh = take(context.freeze(plan.plan, fresh_bindings));
      auto warm = take(context.execute_fragments(fresh, {{"values", query}}));
      check_numeric(warm, profile, 0, false);
      require(
          warm.diagnostics.cache_hits > 0 &&
              warm.results.at("values").association() ==
                  ps::ResourceVector<std::uint64_t>{
                      fresh_bindings.inputs[0].result.object_id()},
          "completed scan cache rebound records current source association");
      fixture.backing[0] = integral ? array(Type::Int64, {2, 2}, {1, 2, 3, 8})
                                    : array(Type::Int64, {3}, {1, 2, 8});
      bindings = fixture.bind(root);
      require(demand.replace_bindings(bindings).ok(),
              "replace scan Result source");
      auto changed = take(demand.request({{"values", query}}));
      check_numeric(changed, profile, integral ? 4 : 3);
      std::uint64_t computed = 0;
      for (const auto& timing : changed.diagnostics.operation_timings)
        computed += timing.computed_elements;
      const auto zero = integral ? std::vector<std::uint64_t>{0, 0}
                                 : std::vector<std::uint64_t>{0};
      const auto last = integral ? std::vector<std::uint64_t>{2, 2}
                                 : std::vector<std::uint64_t>{3};
      std::uint64_t boundary = 1, end = 0;
      require(
          changed.diagnostics.cache_hits == 0 && computed == output_count &&
              rf::read(changed.results.at("values"), zero, &boundary, 8).ok() &&
              boundary == 0 &&
              rf::read(changed.results.at("values"), last, &end, 8).ok() &&
              end == (integral ? 14U : 11U),
          "source edit recomputes complete output including observed zero "
          "boundary");
      if (integral) {
        std::uint64_t first = 0;
        require(
            rf::read(changed.results.at("values"), {1, 1}, &first, 8).ok() &&
                first == 1,
            "unrequested source edit still invalidates unchanged observed "
            "rectangle");
      }
      const auto source_shape =
          bindings.inputs[0].result.schema().tensors[0].sample_shape();
      require(take(changed.dependencies.source_support()).at("input") ==
                      take(ps::Footprint::all(source_shape)) &&
                  changed.results.at("values").association() ==
                      ps::ResourceVector<std::uint64_t>{
                          bindings.inputs[0].result.object_id()},
              "current complete source support and association");
      const auto edit =
          integral
              ? take(ps::Footprint::from_regions(
                    {2, 2}, {ps::Region({{1, 1}, {1, 1}})}))
              : take(ps::Footprint::from_regions({3}, {ps::Region({{2, 1}})}));
      for (unsigned role : {1U, 4U})
        require(take(changed.dependencies.potential_dirty("input", edit, role))
                        .at("values") == query,
                "Whole dirty witness includes zero and unchanged observed "
                "boundary");
      window = take(retained.acquire_tensor(
          take(retained.descriptor()), 0,
          ps::Region::whole(retained.schema().tensors[0].sample_shape())));
    }
    require(
        root.statistics().live[ps::ResourceKind::Payload] == output_count * 8,
        "escaped scan owns only complete original output payload");
    retained = {};
    const auto last = integral ? std::vector<std::uint64_t>{2, 2}
                               : std::vector<std::uint64_t>{3};
    const auto row = take(window.row_run(last));
    std::uint64_t original = 0;
    std::memcpy(&original, row.data, 8);
    require(
        original == (integral ? 10U : 6U),
        "escaped window retains original scan after rebind/context retirement");
    window = {};
    point_math_checks::released(root);
  }
  std::cout << "prefix/integral completed cache, rebind, Whole dirty including "
               "zero boundaries and output/window retirement passed\n";
}
void required_tail(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  for (bool integral : {false, true}) {
    Fixture fixture(
        authored(integral, Type::Float64, Type::Float64,
                 integral ? std::vector<std::uint64_t>{0, 1}
                          : std::vector<std::uint64_t>{1},
                 profile),
        array(Type::Float64, {1, 129}, std::vector<std::uint64_t>(129)));
    fixture.registry = ps::make_default_operation_registry(false);
    auto source = std::make_shared<SourceControl>();
    auto scan = std::make_shared<ScanControl>();
    fixture.document.nodes[0] =
        observe_scan(fixture.registry, fixture.document.nodes[0], scan);
    source_node(&fixture, source, true);
    require(fixture.registry->freeze().ok(), "freeze required scan tail");
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    const std::vector<std::uint64_t> shape =
        integral ? std::vector<std::uint64_t>{2, 130}
                 : std::vector<std::uint64_t>{1, 130};
    ps::ResourceBudget root;
    {
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.result_cache_bytes = 0;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      root = take(context.resource_budget());
      auto snapshot = take(context.freeze(plan.plan, {}));
      auto empty = take(context.execute_fragments(
          snapshot, {{"values", take(ps::Footprint::none(shape))}}));
      require(
          source->starts == 0 && scan->computations == 0 &&
              take(empty.results.at("values").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              root.statistics().live[ps::ResourceKind::Payload] == 0,
          "Empty computed scan starts no source and owns no output samples");
      auto failed = context.execute_fragments(
          snapshot, {{"values", take(ps::Footprint::from_regions(
                                    shape, {ps::Region({{0, 1}, {0, 1}})}))}});
      require(!failed.ok() &&
                  failed.status().message == "required scan tail after NaN" &&
                  source->starts == 1 && source->polls == 2 &&
                  source->published == 128 && scan->computations == 0,
              "zero boundary requires unpublished source tail after a "
              "published NaN prefix");
    }
    {
      const auto facts = take(source->prefix.descriptor(false));
      std::uint64_t nan = 0, last = 1;
      require(
          source->prefix.production_status().code ==
                  ps::ErrorCode::OperationFailed &&
              facts.tensor_coverage(0) ==
                  take(ps::Footprint::from_regions(
                      {1, 129}, {ps::Region({{0, 1}, {0, 128}})})) &&
              source->prefix.read_tensor(facts, 0, {0, 0}, &nan, 8).ok() &&
              nan == 0x7ff0000000000011 &&
              source->prefix.read_tensor(facts, 0, {0, 127}, &last, 8).ok() &&
              last == 0 &&
              !source->prefix.read_tensor(facts, 0, {0, 128}, &last, 8).ok() &&
              root.statistics().live[ps::ResourceKind::Payload] == 1024,
          "failed certified source prefix remains owning and readable after "
          "context retirement");
    }
    source->prefix = {};
    point_math_checks::released(root);
  }
  std::cout << "Empty/zero-boundary computed scans and required tail failure "
               "after NaN publication passed\n";
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
      layouts_and_environment(profile);
      batch_axes(profile);
      large_scan_lifetime(profile);
      interruption(profile);
      cache_and_lifetime(profile);
      required_tail(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
