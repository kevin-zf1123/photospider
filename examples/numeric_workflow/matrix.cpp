#include "photospider/numeric/matrix.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/execution/resource_allocator.hpp"
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
                     auto status = result.values.at("values").read(
                         at, &bits, ps::Value::element_size(dtype));
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
  auto frozen = take(context.freeze(plan.plan, fixture.bindings));
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
        require(result.values.at("values")
                        .read(side ? std::vector<std::uint64_t>{i / side,
                                                                i % side, o}
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
                           result.values.at("values").read(at, &bits, 8);
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
              result.diagnostics.operation_timings[0].invocation_count == 1,
          "partial consumer invokes one complete Whole callback");
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
                       expected.values.at("values").read(at, &want, 4).ok() &&
                           whole.values.at("values")
                               .read(at, &actual, 4)
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
                    expected.values.at("values").read(at, &want, 4).ok() &&
                        result.values.at("values").read(at, &actual, 4).ok() &&
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
  const std::vector<ps::Region> strided_regions{x.region(), m.region(),
                                                b.region()};
  const auto strided_node = authored(profile);
  ps::OperationInvocation invocation(strided_inputs, strided_regions,
                                     strided_node.parameters, ps::Backend::Cpu,
                                     {}, ps::Region::whole({5, 13, 4}));
  const auto sr =
      take(fixture.registry->invoke(authored(profile).operation, invocation));
  const auto se = take(fixture.registry->invoke(
      authored(ps::CpuNumericProfile::Strict).operation, invocation));
  require(sr.bytes().size() == se.bytes().size() &&
              std::memcmp(sr.bytes().data(), se.bytes().data(),
                          sr.bytes().size()) == 0,
          "blocked zero/negative stride bits");
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
    ps::OperationInvocation direct(direct_inputs, strided_regions,
                                   strided_node.parameters, ps::Backend::Cpu,
                                   {}, ps::Region::whole({5, 13, 4}));
    auto actual =
        take(fixture.registry->invoke(strided_node.operation, direct));
    require(
        all.visit(
               [&](const auto& at) {
                 std::uint32_t want = 0, bits = 0;
                 require(expected.values.at("values").read(at, &want, 4).ok(),
                         "expected read");
                 std::memcpy(
                     &bits,
                     actual.bytes().data() + take(actual.byte_address(at)), 4);
                 require(want == bits, "unaligned packed offset/origin bits");
                 return ps::Status::success();
               },
               4096)
            .ok(),
        "direct shifted Whole result");
  }
  std::cout << "Float32 block/tail, sparse rank-3 partitions, source NaN, "
               "zero/negative strides and caller fenv passed\n";
}
void whole_contract(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  const auto node = authored(profile);
  std::vector<ps::OperationMetadata> metadata{{{Type::Float32, {1, 1, 4}}, {}},
                                              {{Type::Float32, {2, 4}}, {}},
                                              {{Type::Float32, {2}}, {}}};
  const auto traits =
      take(registry->resolve_traits(node.operation, metadata, node.parameters));
  require(traits.outputs[0].region_rule == ps::OperationRegionRule::Whole &&
              traits.outputs[0].dependency_version == 0,
          "matrix Whole registration");
  for (unsigned kind = 0; kind < 4; ++kind) {
    auto bad = metadata;
    if (kind == 0)
      bad[1].descriptor.element_type = Type::Float64;
    if (kind == 1)
      bad[0].descriptor.shape = {1, 1, 5};
    if (kind == 2)
      bad[2].descriptor.shape = {3};
    if (kind == 3)
      bad[0].descriptor.shape = {UINT64_C(1) << 40, 4};
    auto answer =
        registry->resolve_traits(node.operation, bad, node.parameters);
    require(!answer.ok() &&
                answer.status().code == ps::ErrorCode::TypeMismatch &&
                answer.status().detail.origin == ps::FailureOrigin::Schema,
            "matrix metadata rejects invalid dtype/shape/count");
  }
  const std::vector<ps::Value> inputs{
      array(Type::Float32, {1, 1, 4}, {0, 0, 0, 0}),
      array(Type::Float32, {2, 4}, std::vector<std::uint64_t>(8, 0)),
      array(Type::Float32, {2}, {0, 0})};
  Fixture fixture(node, inputs);
  auto empty =
      take(fixture.run({{"values", take(ps::Footprint::none({1, 1, 2}))}}));
  require(empty.diagnostics.operation_timings.empty(),
          "Empty skips Whole callback");
  const std::vector<ps::Region> regions{inputs[0].region(), inputs[1].region(),
                                        inputs[2].region()};
  ps::OperationInvocation partial(inputs, regions, node.parameters,
                                  ps::Backend::Cpu, {},
                                  ps::Region({{0, 1}, {0, 1}, {1, 1}}));
  require(!registry->invoke(node.operation, partial).ok(),
          "direct ROI rejected");
  for (bool narrow : {false, true}) {
    const auto type = narrow ? Type::Float32 : Type::Float64;
    const std::uint64_t one =
        narrow ? 0x3f800000 : UINT64_C(0x3ff0000000000000);
    const std::vector<ps::Value> values{
        array(type, {2, 2}, {one, one, one, one}),
        array(type, {2, 2}, {one, one, one, one}), array(type, {2}, {0, 0})};
    const std::vector<ps::Region> demands{
        values[0].region(), values[1].region(), values[2].region()};
    for (bool payload : {false, true}) {
      ps::ResourceLimits limits;
      if (payload)
        limits.capacity[ps::ResourceKind::Payload] = 8;
      else
        limits.maximum_work = narrow ? 8 : 1000;
      ps::ResourceBudget budget(limits);
      {
        ps::ResourceAllocationScope scope(budget);
        ps::OperationInvocation call(
            values, demands, node.parameters, ps::Backend::Cpu, {},
            ps::Region::whole({2, 2}), budget.allocator());
        auto answer = registry->invoke(node.operation, call);
        require(!answer.ok() &&
                    answer.status().code == ps::ErrorCode::ResourceExhausted,
                "Whole resource failure");
      }
      require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
              "Whole failed output and scratch released");
    }
  }
  ps::CancellationSource cancelled;
  cancelled.cancel();
  ps::OperationInvocation call(inputs, regions, node.parameters,
                               ps::Backend::Cpu, cancelled.token(),
                               ps::Region::whole({1, 1, 2}));
  require(registry->invoke(node.operation, call).status().code ==
              ps::ErrorCode::Cancelled,
          "Whole cancellation");
  // Invalid typed source payload must fail validation, even if generic numeric
  // NaNs would have a successful result.
  for (bool invalid : {false, true}) {
    auto typed_inputs = inputs;
    auto raw = array(Type::Float32, {1, 1, 4},
                     {invalid ? UINT64_C(0x7fc00042) : 0, 0, 0, 0x3f800000});
    typed_inputs[0] = take(ps::Value::from_storage(
        raw.descriptor(), raw.region(), raw.layout(), raw.storage(),
        {take(ps::encode_semantic(ps::rgba_semantics()))}));
    ps::OperationInvocation typed(typed_inputs, regions, node.parameters,
                                  ps::Backend::Cpu, {},
                                  ps::Region::whole({1, 1, 2}));
    const auto answer = registry->invoke(node.operation, typed);
    require(answer.ok() != invalid, "Whole typed validation");
  }
  // Synchronize cancellation to admitted callback work, without sleep timing.
  const std::vector<ps::Value> large{
      array(Type::Float32, {16384, 4},
            std::vector<std::uint64_t>(65536, 0x3f800000)),
      array(Type::Float32, {4, 4}, std::vector<std::uint64_t>(16, 0x3f800000)),
      array(Type::Float32, {4}, {0, 0, 0, 0})};
  const std::vector<ps::Region> demands{large[0].region(), large[1].region(),
                                        large[2].region()};
  ps::ResourceBudget budget(ps::ResourceLimits{});
  ps::CancellationSource cancellation;
  std::atomic<bool> ready{false}, finished{false};
  std::thread watcher([&] {
    ready.store(true);
    while (!finished.load() && budget.statistics().issued.work < 22)
      std::this_thread::yield();
    if (!finished.load())
      cancellation.cancel();
  });
  while (!ready.load())
    std::this_thread::yield();
  ps::Status outcome;
  try {
    ps::ResourceAllocationScope scope(budget);
    ps::OperationInvocation running(
        large, demands, node.parameters, ps::Backend::Cpu, cancellation.token(),
        ps::Region::whole({16384, 4}), budget.allocator());
    outcome = registry->invoke(node.operation, running).status();
  } catch (...) {
    finished.store(true);
    watcher.join();
    throw;
  }
  finished.store(true);
  watcher.join();
  require(outcome.code == ps::ErrorCode::Cancelled &&
              budget.statistics().issued.work >= 22 &&
              budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "mid-callback cancellation releases Whole payload");
  std::cout << "Whole/Empty, schema, direct ROI, work/payload failure and "
               "cancellation passed\n";
}

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
  const std::vector<ps::Region> regions{x.region(), m.region(), b.region()};
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save matrix environment");
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "matrix flags");
    ps::OperationInvocation invocation(reversed, regions, node.parameters,
                                       ps::Backend::Cpu, {},
                                       ps::Region::whole({2}));
    auto result = take(registry->invoke(node.operation, invocation));
    std::array<std::uint64_t, 2> bits{};
    std::memcpy(bits.data(), result.bytes().data(), 16);
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
  failure.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
  failure.traits.outputs[0].fixed_output_shape = {2, 2};
  failure.traits.outputs[0].output_element_type = Type::Float64;
  failure.callback = [&](const auto&) {
    ++calls;
    return ps::Result<ps::Value>(
        ps::Status{ps::ErrorCode::OperationFailed, "required matrix producer"});
  };
  require(failed_registry->register_operation(std::move(failure)).ok() &&
              failed_registry->freeze().ok(),
          "failed source registry");
  Fixture fixture(node,
                  {array(Type::Float64, {2}, {0x7ff0000000000042, 0}), m, b});
  fixture.registry = failed_registry;
  fixture.document.inputs.erase(fixture.document.inputs.begin() + 1);
  fixture.bindings.inputs.erase(fixture.bindings.inputs.begin() + 1);
  fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.matrix_failed_source", {}, {}});
  auto answer = fixture.run({{"values", take(ps::Footprint::all({2}))}});
  require(!answer.ok() &&
              answer.status().message == "required matrix producer" &&
              calls == 1,
          "vector NaN cannot suppress matrix source failure");
  std::cout << "negative strides on all ports, fenv flags and required source "
               "failure after NaN "
               "passed\n";
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
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
