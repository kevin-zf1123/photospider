#include "photospider/numeric/matrix.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/execution/resource_allocator.hpp"
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
  ps::ExecutionBindings bindings(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
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
    auto snapshot = context.freeze(plan.value().plan,
                                   bindings(take(context.resource_budget())));
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
ps::WorkflowNode authored(ps::CpuNumericProfile profile) {
  return take(ps::numeric::matrix_transform_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, profile));
}
void oracle(ps::CpuNumericProfile profile) {
  unsigned type = 0, cout = 0;
  std::string encoded_shape;
  while (std::cin >> type >> cout >> encoded_shape) {
    auto shape = list(encoded_shape);
    std::uint64_t count = 1;
    for (auto size : shape)
      count *= size;
    const auto cin = shape.back();
    std::vector<std::uint64_t> vectors(count), matrix(cout * cin), bias(cout);
    for (auto* values : {&vectors, &matrix, &bias})
      for (auto& value : *values)
        std::cin >> std::hex >> value >> std::dec;
    const auto dtype = static_cast<ps::ElementType>(type);
    Fixture fixture(authored(profile), {array(dtype, shape, vectors),
                                        array(dtype, {cout, cin}, matrix),
                                        array(dtype, {cout}, bias)});
    shape.back() = cout;
    auto all = take(ps::Footprint::all(shape));
    auto result = take(fixture.run({{"values", all}}));
    bool first = true;
    require(all.visit(
                   [&](const auto& at) {
                     std::uint64_t bits = 0;
                     auto status =
                         rf::read(result.results.at("values"), at, &bits,
                                  ps::Value::element_size(dtype));
                     if (!first)
                       std::cout << ' ';
                     first = false;
                     std::cout << std::hex << bits << std::dec;
                     return status;
                   },
                   4096)
                .ok(),
            "matrix oracle read");
    std::cout << '\n';
  }
}
// Public workflow timing: compilation/freeze and result checking are excluded.
void benchmark(ps::CpuNumericProfile profile, unsigned n, unsigned cin,
               unsigned cout, bool narrow, bool cancellation = false,
               unsigned side = 0) {
  require(n && cin >= 2 && cin <= 4 && cout >= 2 && cout <= 4,
          "benchmark dimensions");
  const auto dtype =
      narrow ? ps::ElementType::Float32 : ps::ElementType::Float64;
  const auto bits = [narrow](double value) {
    std::uint64_t word = 0;
    if (narrow) {
      const float f = static_cast<float>(value);
      std::memcpy(&word, &f, 4);
    } else {
      std::memcpy(&word, &value, 8);
    }
    return word;
  };
  std::vector<std::uint64_t> x, m, b, expected;
  for (unsigned i = 0; i < n; ++i) {
    for (unsigned j = 0; j < cin; ++j)
      x.push_back(bits((i % 31 + j + 1) / 32.));
    for (unsigned o = 0; o < cout; ++o) {
      double y = (o + 1) / 16.;
      for (unsigned j = 0; j < cin; ++j)
        y += (i % 31 + j + 1) / 32. * (o + j + 1) / 8.;
      expected.push_back(bits(y));
    }
  }
  for (unsigned o = 0; o < cout; ++o) {
    b.push_back(bits((o + 1) / 16.));
    for (unsigned j = 0; j < cin; ++j)
      m.push_back(bits((o + j + 1) / 8.));
  }
  if (cancellation) {
    require(cin == 4, "cancellation benchmark requires Cin=4");
    for (unsigned i = 0; i < n; ++i) {
      x[i * cin] = bits(0x1p120);
      x[i * cin + 1] = bits(1);
      x[i * cin + 2] = bits(-0x1p120);
      x[i * cin + 3] = bits(0);
    }
    std::fill(m.begin(), m.end(), bits(1));
    std::fill(b.begin(), b.end(), bits(0));
    std::fill(expected.begin(), expected.end(), bits(1));
  }
  const std::vector<std::uint64_t> input_shape =
      side ? std::vector<std::uint64_t>{side, side, cin}
           : std::vector<std::uint64_t>{n, cin};
  auto output_shape = input_shape;
  output_shape.back() = cout;
  Fixture fixture(authored(profile),
                  {array(dtype, input_shape, x), array(dtype, {cout, cin}, m),
                   array(dtype, {cout}, b)});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = UINT64_C(1) << 30;
  config.result_cache_bytes = 0;
  config.managed_resources = ps::ResourceLimits{};
  // Large Whole outputs require more than the default 256 MiB host capacity.
  // Keep metadata at its default bound; only payload-related capacity grows.
  if (n > 1024 * 1024)
    config.managed_resources->capacity[ps::ResourceKind::Host] = UINT64_C(1)
                                                                 << 30;
  ps::ExecutionContext context(fixture.registry, config);
  auto frozen = take(context.freeze(
      plan.plan, fixture.bindings(take(context.resource_budget()))));
  const auto all = take(ps::Footprint::all(output_shape));
  ps::ExecutionOptions options;
  options.maximum_dependency_cache_work = 0;
  std::vector<double> times;
  options.dependencies.sets.maximum_work =
      std::max(options.dependencies.sets.maximum_work,
               UINT64_C(4) * n * std::max(cin, cout));
  const unsigned repetitions = n > 1024 * 1024 ? 4 : 8;
  std::uint64_t computed = 0, invocations = 0;
  for (unsigned repeat = 0; repeat < repetitions; ++repeat) {
    const auto start = std::chrono::steady_clock::now();
    auto result =
        take(context.execute_fragments(frozen, {{"values", all}}, {}, options));
    const auto elapsed = std::chrono::duration<double, std::micro>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (repeat)
      times.push_back(elapsed);
    for (unsigned i = 0; i < n; ++i)
      for (unsigned o = 0; o < cout; ++o) {
        std::uint64_t actual = 0;
        require(
            rf::read(result.results.at("values"),
                     side ? std::vector<std::uint64_t>{i / side, i % side, o}
                          : std::vector<std::uint64_t>{i, o},
                     &actual, narrow ? 4 : 8)
                    .ok() &&
                actual == expected[i * cout + o],
            "benchmark analytic bits");
      }
    for (const auto& timing : result.diagnostics.operation_timings)
      if (timing.computed_elements) {
        computed = timing.computed_elements;
        invocations = timing.invocation_count;
      }
  }
  std::sort(times.begin(), times.end());
  std::cout << (narrow ? "Float32" : "Float64") << ',' << n << ',' << cin << ','
            << cout << ',' << times[times.size() / 2] << ',' << times.back()
            << ',' << computed << ',' << invocations << ",Whole\n";
}
void examples(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto x = array(Type::Float64, {2, 2},
                       {0x4000000000000000, 0x4008000000000000,
                        0x3ff0000000000000, 0x4010000000000000});
  const auto m =
      array(Type::Float64, {2, 2},
            {0x3ff0000000000000, 0x4000000000000000, 0xbff0000000000000, 0});
  const auto b =
      array(Type::Float64, {2}, {0x4010000000000000, 0x4014000000000000});
  Fixture fixture(authored(profile), {x, m, b});
  auto all = take(ps::Footprint::all({2, 2}));
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0, "set rounding");
    for (bool cache : {false, true}) {
      auto result = take(fixture.run({{"values", all}}, cache));
      const std::vector<std::uint64_t> expected{
          0x4028000000000000, 0x4008000000000000, 0x402a000000000000,
          0x4010000000000000};
      unsigned index = 0;
      require(all.visit(
                     [&](const auto& at) {
                       std::uint64_t bits = 0;
                       auto status =
                           rf::read(result.results.at("values"), at, &bits, 8);
                       require(bits == expected[index++], "affine fixture");
                       return status;
                     },
                     16)
                  .ok(),
              "matrix result lifetime");
      require(fegetround() == mode, "matrix preserves rounding");
    }
  }
  require(fesetround(FE_TONEAREST) == 0, "restore rounding");
  auto selected =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{0, 2}, {1, 1}})}));
  auto result = take(fixture.run({{"values", selected}}, false));
  auto support = take(result.dependencies.source_support());
  require(support.at("input0") == take(ps::Footprint::all({2, 2})),
          "complete selected vectors");
  require(support.at("input1") == take(ps::Footprint::all({2, 2})) &&
              support.at("input2") == take(ps::Footprint::all({2})),
          "Whole matrix and bias support");
  auto changed =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{0, 1}, {0, 1}})}));
  for (const auto* input : {"input0", "input1"})
    require(take(result.dependencies.potential_dirty(input, changed))
                    .at("values") == selected,
            "Whole change dirties all observed outputs");
  require(result.diagnostics.operation_timings.size() == 1 &&
              result.diagnostics.operation_timings[0].computed_elements == 4 &&
              result.diagnostics.operation_timings[0].invocation_count == 2,
          "partial consumer invokes Need and one complete Whole computation");
  std::cout
      << "affine fixture, cache/fenv/lifetime, Whole support/dirty passed\n";
}

void block_consistency(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  std::vector<std::uint64_t> words(65 * 3);
  for (unsigned i = 0; i < words.size(); ++i)
    words[i] = 0x3f000001 + i * 19;
  words[17 * 3] = 0x7f800042;
  const auto x = array(Type::Float32, {5, 13, 3}, words);
  const auto m =
      array(Type::Float32, {4, 3},
            {0x3f800003, 0x3f000002, 0x3e800001, 0x7f800099, 0, 0, 0x3f800000,
             0xbf800000, 0, 0x3e000001, 0x3f400003, 0x3f000001});
  const auto b = array(Type::Float32, {4}, {0x3e000003, 0, 0, 0x3e800001});
  Fixture fixture(authored(profile), {x, m, b});
  Fixture reference(authored(ps::CpuNumericProfile::Strict), {x, m, b});
  const auto all = take(ps::Footprint::all({5, 13, 4}));
  const auto expected = take(reference.run({{"values", all}}, false));
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save blocked environment");
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "set blocked environment");
    for (bool cache : {false, true}) {
      const auto whole = take(fixture.run({{"values", all}}, cache));
      require(
          all.visit(
                 [&](const auto& at) {
                   std::uint32_t want = 0, actual = 0;
                   require(
                       rf::read(expected.results.at("values"), at, &want, 4)
                               .ok() &&
                           rf::read(whole.results.at("values"), at, &actual, 4)
                               .ok() &&
                           want == actual,
                       "blocked exact bits");
                   return ps::Status::success();
                 },
                 4096)
              .ok(),
          "blocked whole");
    }
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "blocked caller fenv restored");
  }
  require(fesetenv(&saved) == 0, "restore blocked environment");
  // Sparse consumers project the complete Whole result.
  const auto selected = take(ps::Footprint::from_regions(
      {5, 13, 4}, {ps::Region({{1, 2}, {2, 7}, {2, 2}}),
                   ps::Region({{4, 1}, {8, 5}, {2, 2}})}));
  const auto result = take(fixture.run({{"values", selected}}, false));
  require(
      selected
          .visit(
              [&](const auto& at) {
                std::uint32_t want = 0, actual = 0;
                require(
                    rf::read(expected.results.at("values"), at, &want, 4)
                            .ok() &&
                        rf::read(result.results.at("values"), at, &actual, 4)
                            .ok() &&
                        want == actual,
                    "blocked sparse partition bits");
                return ps::Status::success();
              },
              4096)
          .ok(),
      "blocked sparse visit");
  const auto support = take(result.dependencies.source_support());
  require(support.at("input1") == take(ps::Footprint::all({4, 3})),
          "sparse consumers retain Whole matrix support");
  // Broadcast vector and reversed matrix/bias layouts use the same read path.
  const auto broadcast = take(ps::Value::from_storage(
      x.descriptor(), x.region(), {0, {0, 0, 0}}, x.storage()));
  const auto reversed_m = take(ps::Value::from_storage(
      m.descriptor(), m.region(), {44, {-12, -4}}, m.storage()));
  const auto reversed_b = take(ps::Value::from_storage(
      b.descriptor(), b.region(), {12, {-4}}, b.storage()));
  const std::vector<ps::Value> strided_inputs{broadcast, reversed_m,
                                              reversed_b};
  const auto strided_node = authored(profile);
  const auto sr =
      take(Fixture(strided_node, strided_inputs).run({{"values", all}}, false));
  const auto se =
      take(Fixture(authored(ps::CpuNumericProfile::Strict), strided_inputs)
               .run({{"values", all}}, false));
  require(
      rf::bytes(sr.results.at("values")) == rf::bytes(se.results.at("values")),
      "blocked zero/negative stride Result bits");
  auto padded = take(ps::BufferAllocator{}.allocate(x.bytes().size() + 1));
  std::memcpy(padded.data() + 1, x.bytes().data(), x.bytes().size());
  auto storage = std::move(padded).freeze();
  for (bool shifted_origin : {false, true}) {
    auto layout = x.layout();
    layout.byte_offset = shifted_origin ? 13 : 1;
    if (shifted_origin)
      layout.origin = {0, 1, 0};
    auto unaligned = take(
        ps::Value::from_storage(x.descriptor(), x.region(), layout, storage));
    const std::vector<ps::Value> direct_inputs{unaligned, m, b};
    auto actual = take(
        Fixture(strided_node, direct_inputs).run({{"values", all}}, false));
    require(
        all.visit(
               [&](const auto& at) {
                 std::uint32_t want = 0, bits = 0;
                 require(
                     rf::read(expected.results.at("values"), at, &want, 4).ok(),
                     "expected read");
                 require(
                     rf::read(actual.results.at("values"), at, &bits, 4).ok(),
                     "unaligned Result read");
                 require(want == bits, "unaligned packed offset/origin bits");
                 return ps::Status::success();
               },
               4096)
            .ok(),
        "shifted Whole Result");
  }
  std::cout << "Float32 block/tail, sparse rank-3 partitions, source NaN, "
               "zero/negative strides and caller fenv passed\n";
}
ps::OperationMetadata tensor_metadata(ps::ElementType type,
                                      std::vector<std::uint64_t> shape) {
  ps::SchemaTemplate schema;
  schema.id = "manual.matrix.metadata";
  ps::ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {type, std::move(shape)};
  schema.tensors.push_back(std::move(tensor));
  ps::OperationMetadata result;
  result.result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(schema));
  return result;
}
void rgba_vectors(Fixture* fixture) {
  const auto& raw = fixture->backing[0];
  fixture->backing[0] =
      take(ps::Value::from_storage({ps::ElementType::Float32, {1, 1, 1, 1, 4}},
                                   ps::Region::whole({1, 1, 1, 1, 4}),
                                   {0, {16, 16, 16, 16, 4}}, raw.storage()));
  auto schema = rf::source_schema(fixture->backing[0]);
  auto& tensor = schema.tensors[0];
  tensor.batch_axes = {1, 1};
  tensor.descriptor.shape = {1, 1, 4};
  tensor.layout.spatial = true;
  tensor.layout.channel_axis = 2;
  tensor.facets = {take(ps::encode_semantic(ps::rgba_semantics()))};
  fixture->document.inputs[0].result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(schema));
}
void whole_contract(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  const auto node = authored(profile);
  std::vector<ps::OperationMetadata> metadata{
      tensor_metadata(Type::Float32, {1, 1, 4}),
      tensor_metadata(Type::Float32, {2, 4}),
      tensor_metadata(Type::Float32, {2})};
  const auto prepared = take(
      registry->prepare_operation(node.operation, metadata, node.parameters));
  const auto& traits = prepared->traits();
  require(traits.outputs[0].region_rule == ps::OperationRegionRule::Whole &&
              traits.outputs[0].dependency_version == 2 &&
              traits.outputs[0].result_schema &&
              traits.outputs[0].result_schema->tensors[0].sample_shape() ==
                  std::vector<std::uint64_t>({1, 1, 2}),
          "matrix Whole Result registration and static preparation");
  for (unsigned kind = 0; kind < 4; ++kind) {
    auto bad = metadata;
    if (kind == 0)
      bad[1] = tensor_metadata(Type::Float64, {2, 4});
    if (kind == 1)
      bad[0] = tensor_metadata(Type::Float32, {1, 1, 5});
    if (kind == 2)
      bad[2] = tensor_metadata(Type::Float32, {3});
    if (kind == 3)
      bad[0] = tensor_metadata(Type::Float32, {UINT64_C(1) << 40, 4});
    auto answer =
        registry->prepare_operation(node.operation, bad, node.parameters);
    require(!answer.ok() &&
                answer.status().code == ps::ErrorCode::TypeMismatch &&
                answer.status().detail.origin == ps::FailureOrigin::Schema,
            "matrix Result metadata rejects invalid dtype/shape/count");
  }
  const std::vector<ps::Value> inputs{
      array(Type::Float32, {1, 1, 4}, {0, 0, 0, 0}),
      array(Type::Float32, {2, 4}, std::vector<std::uint64_t>(8, 0)),
      array(Type::Float32, {2}, {0, 0})};
  Fixture fixture(node, inputs);
  auto empty = take(
      fixture.run({{"values", take(ps::Footprint::none({1, 1, 2}))}}, false));
  const auto facts = take(empty.results.at("values").descriptor());
  require(facts.sealed() && facts.tensor_coverage(0).empty() &&
              std::all_of(empty.diagnostics.operation_timings.begin(),
                          empty.diagnostics.operation_timings.end(),
                          [](const auto& timing) {
                            return timing.computed_elements == 0;
                          }),
          "Empty publishes metadata without matrix arithmetic");
  const auto partial = take(ps::Footprint::from_regions(
      {1, 1, 2}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
  const auto projected = take(fixture.run({{"values", partial}}, false));
  require(
      projected.diagnostics.operation_timings.size() == 1 &&
          projected.diagnostics.operation_timings[0].computed_elements == 2 &&
          projected.diagnostics.operation_timings[0].invocation_count == 2,
      "partial Result demand computes one complete Whole output");
  for (bool narrow : {false, true}) {
    const auto type = narrow ? Type::Float32 : Type::Float64;
    const std::uint64_t one =
        narrow ? 0x3f800000 : UINT64_C(0x3ff0000000000000);
    const std::vector<ps::Value> values{
        array(type, {2, 2}, {one, one, one, one}),
        array(type, {2, 2}, {one, one, one, one}), array(type, {2}, {0, 0})};
    ps::ResourceBudget root;
    auto control = std::make_shared<point_math_checks::Control>();
    {
      point_math_checks::Workflow workflow(node, values, {}, control);
      root = workflow.root;
      {
        const auto first = workflow.run();
        require(
            first.ok() && control->work > 0 && control->computation_polls == 1,
            "measure actual matrix computation work");
      }
      const auto required = control->work;
      for (bool enough : {true, false}) {
        control->work = 0;
        control->computation_polls = 0;
        control->maximum_work = enough ? required : required - 1;
        const auto result = workflow.run();
        require(result.ok() == enough && control->computation_polls == 1,
                "exact matrix callback work threshold");
        if (!enough)
          require(result.status().code == ps::ErrorCode::ResourceExhausted &&
                      result.status().reason == ps::FailureReason::WorkLimit,
                  "matrix callback work failure retains its reason");
      }
    }
    point_math_checks::released(root);
    {
      ps::ResourceLimits limits;
      limits.capacity[ps::ResourceKind::Payload] = 8;
      point_math_checks::Workflow workflow(node, values, limits);
      root = workflow.root;
      const auto result = workflow.run();
      require(!result.ok() &&
                  result.status().code == ps::ErrorCode::ResourceExhausted,
              "Whole Result output and workspace obey Payload budget");
    }
    point_math_checks::released(root);
  }
  ps::CancellationSource cancelled;
  cancelled.cancel();
  {
    auto control = std::make_shared<point_math_checks::Control>();
    point_math_checks::Workflow workflow(node, inputs, {}, control);
    require(workflow.run(cancelled.token()).status().code ==
                    ps::ErrorCode::Cancelled &&
                control->polls == 0,
            "pre-cancelled matrix enters no continuation poll");
  }
  for (bool invalid : {false, true}) {
    auto typed_inputs = inputs;
    typed_inputs[0] =
        array(Type::Float32, {1, 1, 4},
              {invalid ? UINT64_C(0x7fc00042) : 0, 0, 0, 0x3f800000});
    Fixture typed(node, typed_inputs);
    rgba_vectors(&typed);
    const auto answer = typed.run(
        {{"values", take(ps::Footprint::all({1, 1, 1, 1, 2}))}}, false);
    require(answer.ok() != invalid,
            "Whole Result validates complete typed RGBA vectors");
    if (!invalid) {
      const auto alpha = take(ps::Footprint::from_regions(
          {1, 1, 1, 1, 4},
          {ps::Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {3, 1}})}));
      require(
          take(answer.value().dependencies.potential_dirty("input0", alpha, 4))
                  .at("values") == take(ps::Footprint::all({1, 1, 1, 1, 2})),
          "typed validation alpha dirties all observed matrix output");
    }
  }
  const std::vector<ps::Value> large{
      array(Type::Float32, {16384, 4},
            std::vector<std::uint64_t>(65536, 0x3f800000)),
      array(Type::Float32, {4, 4}, std::vector<std::uint64_t>(16, 0x3f800000)),
      array(Type::Float32, {4}, {0, 0, 0, 0})};
  point_math_checks::resources(node, large, 16384 * 4 * 4);
  std::cout << "Result Whole/Empty, metadata, sparse projection, exact work, "
               "typed validation, capacity/cancellation and all-Root cleanup "
               "passed\n";
}
struct FailedSource final {
  unsigned* calls;
  explicit FailedSource(unsigned* count) : calls(count) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::OperationFailed, "required matrix producer"});
  }
};

void strides_and_failures(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  auto node = authored(profile);
  auto x = array(Type::Float64, {2}, {0x4000000000000000, 0x4008000000000000});
  auto m =
      array(Type::Float64, {2, 2},
            {0x3ff0000000000000, 0x4000000000000000, 0xbff0000000000000, 0});
  auto b = array(Type::Float64, {2}, {0x4010000000000000, 0x4014000000000000});
  const std::vector<ps::Value> reversed{
      take(ps::Value::from_storage(x.descriptor(), x.region(), {8, {-8}},
                                   x.storage())),
      take(ps::Value::from_storage(m.descriptor(), m.region(), {24, {-16, -8}},
                                   m.storage())),
      take(ps::Value::from_storage(b.descriptor(), b.region(), {8, {-8}},
                                   b.storage()))};
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save matrix environment");
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "matrix flags");
    auto control = std::make_shared<point_math_checks::Control>();
    control->rounding = mode;
    point_math_checks::Workflow workflow(node, reversed, {}, control);
    auto result = take(workflow.run()).results.at("values");
    const auto bytes = rf::bytes(result);
    std::array<std::uint64_t, 2> bits{};
    std::memcpy(bits.data(), bytes.data(), 16);
    require(bits == std::array<std::uint64_t, 2>{0x4008000000000000,
                                                 0x4028000000000000},
            "all-port negative strides");
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "matrix fenv flags preserved");
  }
  require(fesetenv(&saved) == 0, "restore matrix environment");
  auto failed_registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition failure;
  failure.key = "manual.matrix_failed_source";
  failure.traits.input_count = 0;
  failure.traits.input_schema.clear();
  auto& output = failure.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.result_schema = *tensor_metadata(Type::Float64, {2, 2}).result_schema;
  output.output_schema.result_schema_id = std::string(output.result_schema->id);
  output.output_schema.result_schema_version = output.result_schema->version;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(FailedSource);
  output.maximum_dependency_stages = 1;
  failure.start_result = [&calls](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<FailedSource>(allocator, &calls);
  };
  require(failed_registry->register_operation(std::move(failure)).ok() &&
              failed_registry->freeze().ok(),
          "failed source registry");
  Fixture fixture(node,
                  {array(Type::Float64, {2}, {0x7ff0000000000042, 0}), m, b});
  fixture.registry = failed_registry;
  fixture.document.inputs.erase(fixture.document.inputs.begin() + 1);
  fixture.backing.erase(fixture.backing.begin() + 1);
  fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.matrix_failed_source", {}, {}});
  auto answer = fixture.run({{"values", take(ps::Footprint::all({2}))}});
  require(
      !answer.ok() && answer.status().code == ps::ErrorCode::OperationFailed &&
          answer.status().message == "required matrix producer" && calls == 1,
      "vector NaN cannot suppress matrix source failure");
  std::cout << "negative strides on all ports, fenv flags and required source "
               "failure after NaN "
               "passed\n";
}
void prepared_rebinding(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  Fixture fixture(
      authored(profile),
      {array(Type::Float64, {2}, {0x4000000000000000, 0x4008000000000000}),
       array(Type::Float64, {2, 2},
             {0x3ff0000000000000, 0, 0, 0x3ff0000000000000}),
       array(Type::Float64, {2}, {0, 0})});
  ps::GraphContext graph(fixture.document);
  const auto compiled = take(ps::Compiler(fixture.registry).compile(graph));
  const auto preparation = compiled.plan.steps()[0].prepared;
  require(preparation != nullptr, "matrix plan owns static preparation");
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 1048576;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  const auto root = take(context.resource_budget());
  const ps::DemandQuery query{{"values", take(ps::Footprint::all({2}))}};
  ps::ExecutionOptions options;
  options.maximum_dependency_cache_work = UINT64_C(128) * 1024 * 1024;
  auto bindings = fixture.bindings(root);
  const auto cold = take(context.execute_fragments(
      take(context.freeze(compiled.plan, bindings)), query, {}, options));
  auto fresh = fixture.bindings(root);
  const auto warm = take(context.execute_fragments(
      take(context.freeze(compiled.plan, fresh)), query, {}, options));
  require(warm.diagnostics.cache_hits > 0 &&
              rf::bytes(cold.results.at("values")) ==
                  rf::bytes(warm.results.at("values")),
          "fresh source Results reuse matrix content with identical output");
  const auto association = warm.results.at("values").association();
  for (unsigned port = 0; port < 3; ++port) {
    const auto current = fresh.inputs[port].result.object_id();
    require(std::find(association.begin(), association.end(), current) !=
                    association.end() &&
                std::find(association.begin(), association.end(),
                          bindings.inputs[port].result.object_id()) ==
                    association.end(),
            "cached matrix Result refreshes every source association");
  }
  auto demand = take(context.open_demand(compiled.plan, fresh));
  const auto before = take(demand.request(query, {}, options));
  const auto repeated = take(demand.request(query, {}, options));
  require(before.results.at("values").object_id() ==
              repeated.results.at("values").object_id(),
          "same matrix demand retains its completed Result");
  auto replacement = array(Type::Float64, {2, 2},
                           {0x4000000000000000, 0, 0, 0x4008000000000000});
  fresh.inputs[1].result = point_math_checks::source(
      root, replacement, fixture.document.inputs[1].result_schema.get());
  require(demand.replace_bindings(fresh).ok(),
          "matrix input replacement preserves static schema");
  const auto after = take(demand.request(query, {}, options));
  double x = 0, y = 0;
  require(
      rf::read(after.results.at("values"), {0}, &x, 8).ok() &&
          rf::read(after.results.at("values"), {1}, &y, 8).ok() && x == 4 &&
          y == 9 && compiled.plan.steps()[0].prepared == preparation,
      "dynamic matrix edit reuses preparation and recomputes actual samples");
  const auto changed =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{1, 1}, {1, 1}})}));
  require(take(after.dependencies.potential_dirty("input1", changed, 4))
                  .at("values") == take(ps::Footprint::all({2})),
          "replaced matrix keeps full Validation dirty support");
  std::cout
      << "Result preparation rebind, warm content, refreshed association, "
         "completed demand and validation dirty support passed\n";
}
void retained_output(ps::CpuNumericProfile profile) {
  ps::ResourceBudget root;
  ps::ResultRef output;
  ps::ResultTensorReadWindow window;
  std::weak_ptr<const ps::CpuStorage> input_owner;
  {
    using Type = ps::ElementType;
    Fixture fixture(
        authored(profile),
        {array(Type::Float64, {2}, {0x4000000000000000, 0x4008000000000000}),
         array(Type::Float64, {2, 2},
               {0x3ff0000000000000, 0x4000000000000000, 0xbff0000000000000, 0}),
         array(Type::Float64, {2}, {0x4010000000000000, 0x4014000000000000})});
    input_owner = fixture.backing[0].storage();
    ps::GraphContext graph(fixture.document);
    auto compiled = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    root = take(context.resource_budget());
    auto result = take(context.execute(compiled.plan, fixture.bindings(root)));
    output = result.results.at("values");
    window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                        ps::Region::whole({2})));
  }
  require(input_owner.expired(),
          "owned matrix output does not retain retired source backing");
  require(root.statistics().live[ps::ResourceKind::Payload] == 16,
          "escaped matrix Result and window charge one output owner");
  double value = 0;
  require(rf::read(output, {0}, &value, 8).ok() && value == 12,
          "matrix Result survives context and source retirement");
  output = {};
  auto row = take(window.row_run({1}));
  std::memcpy(&value, row.data, 8);
  require(value == 3 && root.statistics().live[ps::ResourceKind::Payload] == 16,
          "authorized matrix window owns backing after Result release");
  window = {};
  point_math_checks::released(root);
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
    if (argc > 2 && std::string(argv[2]) == "grid") {
      const auto side = argc > 3 ? std::stoul(argv[3]) : 128;
      require(side > 0 && side <= 4096, "grid benchmark side must be 1..4096");
      benchmark(profile, side * side, 4, 4, true, false, side);
    } else if (argc > 2 && std::string(argv[2]) == "benchmark") {
      benchmark(profile, argc > 3 ? std::stoul(argv[3]) : 256,
                argc > 4 ? std::stoul(argv[4]) : 4,
                argc > 5 ? std::stoul(argv[5]) : 4,
                argc <= 6 || std::string(argv[6]) == "float32",
                argc > 7 && std::string(argv[7]) == "cancellation");
    } else if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      whole_contract(profile);
      examples(profile);
      block_consistency(profile);
      strides_and_failures(profile);
      prepared_rebinding(profile);
      retained_output(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
