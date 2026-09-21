#include "photospider/numeric/ordering.hpp"

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
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs,
          bool sorting) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto& value = inputs[i];
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
    }
    document.outputs = {{"values", node.id, "values"}};
    if (sorting)
      document.outputs.push_back({"indices", node.id, "indices"});
    document.nodes = {std::move(node)};
  }
  ps::Result<ps::DemandResult> run(
      const ps::DemandQuery& query, bool cache = true,
      std::uint64_t proof_work = UINT64_C(64) * 1024 * 1024,
      std::uint64_t cache_bytes = 65536,
      std::uint64_t work = UINT64_C(128) * 1024 * 1024) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 262144;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->maximum_work = work;
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = work;
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
void oracle(ps::CpuNumericProfile profile) {
  std::string operation, encoded_shape;
  unsigned source = 0, qtype = 0, destination = 0;
  std::int64_t axis = 0;
  std::uint64_t qbits = 0;
  while (std::cin >> operation >> source >> qtype >> destination >> axis >>
         encoded_shape >> std::hex >> qbits >> std::dec) {
    auto shape = list(encoded_shape);
    std::uint64_t count = 1;
    for (auto size : shape)
      count *= size;
    std::vector<std::uint64_t> bits(count);
    for (auto& value : bits)
      std::cin >> std::hex >> value >> std::dec;
    const bool sorting = operation == "sort";
    std::vector<ps::Value> inputs{
        array(static_cast<ps::ElementType>(source), shape, bits)};
    auto node = sorting
                    ? take(ps::numeric::sort_node(
                          1, ps::WorkflowInputReference{1}, axis, profile))
                    : take(ps::numeric::quantile_node(
                          1, ps::WorkflowInputReference{1},
                          ps::WorkflowInputReference{2}, axis,
                          static_cast<ps::ElementType>(destination), profile));
    if (!sorting) {
      inputs.push_back(
          array(static_cast<ps::ElementType>(qtype), {1}, {qbits}));
      shape[axis] = 1;
    }
    Fixture fixture(std::move(node), inputs, sorting);
    auto all = take(ps::Footprint::all(shape));
    ps::DemandQuery query{{"values", all}};
    if (sorting)
      query.emplace("indices", all);
    auto answer = fixture.run(query);
    if (!answer.ok()) {
      if (answer.status().message.find("InvalidQuantileProbability") !=
          std::string::npos)
        std::cout << "q\n";
      else
        throw std::runtime_error(answer.status().message);
      continue;
    }
    for (const auto* name : {"values", "indices"}) {
      if (std::string(name) == "indices") {
        if (!sorting)
          break;
        std::cout << " | ";
      }
      bool first = true;
      const auto& values = answer.value().values.at(name);
      const auto width =
          ps::Value::element_size(values.descriptor().element_type);
      require(all.visit(
                     [&](const auto& coordinate) {
                       std::uint64_t value = 0;
                       auto status = values.read(coordinate, &value, width);
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
    }
    std::cout << '\n';
  }
}
void examples(ps::CpuNumericProfile profile) {
  Fixture sorted(take(ps::numeric::sort_node(1, ps::WorkflowInputReference{1},
                                             0, profile)),
                 {array(ps::ElementType::Int64, {4}, {3, 1, 1, 2})}, true);
  auto all = take(ps::Footprint::all({4}));
  for (bool cache : {false, true}) {
    auto answer = take(sorted.run({{"values", all}, {"indices", all}}, cache));
    const std::uint64_t values[] = {1, 1, 2, 3}, indices[] = {1, 2, 3, 0};
    for (std::uint64_t i = 0; i < 4; ++i) {
      std::uint64_t actual = 0;
      require(answer.values.at("values").read({i}, &actual, 8).ok() &&
                  actual == values[i],
              "sort values");
      require(answer.values.at("indices").read({i}, &actual, 8).ok() &&
                  actual == indices[i],
              "sort indices stable ties");
    }
    std::uint64_t invocations = 0;
    for (const auto& timing : answer.diagnostics.operation_timings)
      invocations += timing.invocation_count;
    require(invocations == 2, "one Whole callback per independent sort output");
  }
  Fixture quantile(
      take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                      ps::WorkflowInputReference{2}, 0,
                                      ps::ElementType::Float64, profile)),
      {array(ps::ElementType::Int64, {4}, {0, 10, 20, 30}),
       array(ps::ElementType::Float64, {1}, {UINT64_C(0x3fd0000000000000)})},
      false);
  auto answer = take(quantile.run({{"values", take(ps::Footprint::all({1}))}}));
  std::uint64_t actual = 0;
  require(answer.values.at("values").read({0}, &actual, 8).ok() &&
              actual == UINT64_C(0x401e000000000000),
          "quantile exact 7.5");
  std::cout << "sort [3,1,1,2]: values=[1,1,2,3], indices=[1,2,3,0]; "
               "quantile([0,10,20,30],.25)=7.5 passed\n";
}

void sharing_and_sparse(ps::CpuNumericProfile profile) {
  std::vector<std::uint64_t> bits(256);
  for (std::uint64_t i = 0; i < 128; ++i) {
    bits[i] = UINT64_C(0x3f800000) + (127 - i) / 2;
    bits[128 + i] = UINT64_C(0x7f800001) + i;
  }
  Fixture fixture(take(ps::numeric::sort_node(1, ps::WorkflowInputReference{1},
                                              1, profile)),
                  {array(ps::ElementType::Float32, {2, 128}, bits)}, true);
  auto row = take(
      ps::Footprint::from_regions({2, 128}, {ps::Region({{0, 1}, {0, 128}})}));
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto answer = take(fixture.run(
        {{"values", row}, {"indices", row}}, mode != 0,
        mode == 2 ? 0 : UINT64_C(64) * 1024 * 1024, mode == 3 ? 1 : 65536));
    for (std::uint64_t i = 0; i < 128; ++i) {
      std::uint64_t actual = 0;
      require(answer.values.at("values").read({0, i}, &actual, 4).ok() &&
                  actual == UINT64_C(0x3f800000) + i / 2,
              "long sort values");
      require(answer.values.at("indices").read({0, i}, &actual, 8).ok() &&
                  actual == 126 - 2 * (i / 2) + i % 2,
              "long stable indices");
    }
    std::uint64_t invocations = 0;
    for (const auto& timing : answer.diagnostics.operation_timings)
      invocations += timing.invocation_count;
    require(invocations == 2, "one Whole callback per selected sort output");
    require(take(answer.dependencies.source_support()).at("input0") ==
                take(ps::Footprint::all({2, 128})),
            "selected full-line source support");
    const auto changed = take(
        ps::Footprint::from_regions({2, 128}, {ps::Region({{0, 1}, {10, 1}})}));
    auto dirty = take(answer.dependencies.potential_dirty("input0", changed));
    require(dirty.at("values") == row && dirty.at("indices") == row,
            "one source edit invalidates both sorted lines");
    const auto other = take(ps::Footprint::from_regions(
        {2, 128}, {ps::Region({{1, 1}, {0, 128}})}));
    dirty = take(answer.dependencies.potential_dirty("input0", other));
    require(dirty.at("values") == row && dirty.at("indices") == row,
            "unrequested line invalidates all recorded Whole observations");
  }
  const auto selected = take(
      ps::Footprint::from_regions({2, 128}, {ps::Region({{0, 1}, {70, 1}})}));
  for (const auto* name : {"values", "indices"}) {
    auto answer = take(fixture.run({{name, selected}}));
    require(answer.values.size() == 1 &&
                answer.values.at(name).coverage() == selected,
            "only requested named output position published");
    require(take(answer.dependencies.source_support()).at("input0") ==
                take(ps::Footprint::all({2, 128})),
            "partial sorted position still reads full line");
  }
  std::cout << "128-element line: each output reuses its "
               "permutation; cache capacities, "
               "sparse outputs and line dirty passed\n";
}
void nonlast_axis_lines(ps::CpuNumericProfile profile) {
  std::vector<std::uint64_t> source(256);
  for (std::uint64_t row = 0; row < 128; ++row) {
    source[row * 2] = 127 - row;
    source[row * 2 + 1] = 1127 - row;
  }
  Fixture fixture(take(ps::numeric::sort_node(1, ps::WorkflowInputReference{1},
                                              0, profile)),
                  {array(ps::ElementType::Int64, {128, 2}, source)}, true);
  const auto all = take(ps::Footprint::all({128, 2}));
  const auto sparse = take(ps::Footprint::from_regions(
      {128, 2},
      {ps::Region({{1, 31}, {0, 2}}), ps::Region({{65, 62}, {0, 2}})}));
  for (const auto& q : {all, sparse}) {
    // Two lines per output fit; rebuilding 128 times per line does not.
    auto answer =
        take(fixture.run({{"values", q}, {"indices", q}}, false, 0, 0, 500000));
    require(
        q.visit(
             [&](const auto& at) {
               std::uint64_t value = 0, index = 0;
               require(answer.values.at("values").read(at, &value, 8).ok() &&
                           value == at[0] + at[1] * 1000,
                       "non-last-axis stable values");
               require(answer.values.at("indices").read(at, &index, 8).ok() &&
                           index == 127 - at[0],
                       "non-last-axis original indices");
               return ps::Status::success();
             },
             4096)
            .ok(),
        "non-last-axis result traversal");
  }
  std::cout << "non-last axis and disjoint boxes reuse each line within a "
               "bounded work budget\n";
}
void probability_dependencies(ps::CpuNumericProfile profile) {
  unsigned calls = 0;
  for (bool singleton : {true, false}) {
    auto registry = ps::make_default_operation_registry(false);
    ps::OperationDefinition failed;
    failed.key = "manual.quantile_failed";
    failed.traits.input_count = 0;
    failed.traits.input_schema.clear();
    auto& output = failed.traits.outputs[0];
    output.shape_rule = ps::OperationShapeRule::Fixed;
    output.fixed_output_shape = singleton ? std::vector<std::uint64_t>{1}
                                          : std::vector<std::uint64_t>{2, 2};
    output.output_element_type =
        singleton ? ps::ElementType::Float64 : ps::ElementType::Int64;
    failed.callback = [&](const auto&) {
      ++calls;
      return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                              "required quantile producer"});
    };
    require(registry->register_operation(std::move(failed)).ok() &&
                registry->freeze().ok(),
            "quantile failing registry");
    Fixture fixture(
        take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                        ps::WorkflowInputReference{2}, 1,
                                        ps::ElementType::Float32, profile)),
        {array(ps::ElementType::Int64,
               singleton ? std::vector<std::uint64_t>{2, 1}
                         : std::vector<std::uint64_t>{2, 2},
               singleton
                   ? std::vector<std::uint64_t>{UINT64_C(0x7fffffffffffffff),
                                                UINT64_C(0xfffffffffffffffe)}
                   : std::vector<std::uint64_t>{1, 2, 3, 4}),
         array(ps::ElementType::Float64, {1}, {UINT64_C(0x7ff8000000000011)})},
        false);
    fixture.registry = registry;
    fixture.document.nodes[0].inputs[singleton ? 1 : 0] =
        ps::WorkflowNodeOutput{2, "value"};
    fixture.document.nodes.push_back({2, "manual.quantile_failed", {}, {}});
    calls = 0;
    auto answer = fixture.run({{"values", take(ps::Footprint::all({2, 1}))}});
    require(calls == (singleton ? 0 : 1),
            "singleton skips q; other Whole inputs are eagerly read");
    if (singleton) {
      auto result = take(std::move(answer));
      const std::uint64_t expected[] = {UINT64_C(0x5f000000),
                                        UINT64_C(0xc0000000)};
      for (std::uint64_t i = 0; i < 2; ++i) {
        std::uint64_t actual = 0;
        require(result.values.at("values").read({i, 0}, &actual, 4).ok() &&
                    actual == expected[i],
                "singleton exact integer conversion without q");
      }
    } else {
      require(!answer.ok() &&
                  answer.status().message == "required quantile producer",
              "Whole source failure can precede invalid-q callback validation");
      fixture.bindings.inputs[1].value =
          array(ps::ElementType::Float64, {1}, {UINT64_C(0x3fe0000000000000)});
      answer = fixture.run({{"values", take(ps::Footprint::all({2, 1}))}});
      require(!answer.ok() && calls > 0 &&
                  answer.status().message == "required quantile producer",
              "valid q preserves source failure");
    }
  }
  std::cout << "quantile: N=1 skips failing q, N>=2 eagerly collects "
               "source and preserves required source failure passed\n";
}

void failure_and_schema(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  auto node = take(
      ps::numeric::sort_node(1, ps::WorkflowInputReference{1}, 0, profile));
  auto source = array(ps::ElementType::Int64, {8}, {7, 6, 5, 4, 3, 2, 1, 0});
  std::vector<std::uint64_t> descending(4096);
  for (unsigned i = 0; i < 4096; ++i)
    descending[i] = 4095 - i;
  auto large = array(ps::ElementType::Int64, {4096}, descending);
  point_math_checks::resources(node, {large}, 32768);
  point_math_checks::resources(node, {large}, 32768, 1);
  auto quantile = take(ps::numeric::quantile_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, 0,
      ps::ElementType::Float64, profile));
  auto half = array(ps::ElementType::Float64, {1}, {0x3fe0000000000000});
  point_math_checks::resources(quantile, {large, half}, 8);
  ps::ResourceLimits limits;
  limits.capacity[ps::ResourceKind::Metadata] = 128;
  ps::ResourceBudget budget(limits);
  {
    ps::ResourceAllocationScope scope(budget);
    const std::vector<ps::Value> inputs{large};
    const std::vector<ps::Region> demands{large.region()};
    ps::OperationInvocation call(inputs, demands, node.parameters,
                                 ps::Backend::Cpu, {}, large.region(),
                                 budget.allocator());
    auto result = registry->invoke(node.operation, call);
    require(!result.ok() &&
                result.status().code == ps::ErrorCode::ResourceExhausted,
            "permutation metadata capacity rejected");
  }
  require(budget.statistics().live[ps::ResourceKind::Metadata] == 0 &&
              budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "sort metadata/state/output cleanup");
  Fixture invalid(
      take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                      ps::WorkflowInputReference{2}, 0,
                                      ps::ElementType::Float64, profile)),
      {array(ps::ElementType::Int64, {2}, {1, 2}),
       array(ps::ElementType::Float64, {2}, {0, 0})},
      false);
  auto result = invalid.run({{"values", take(ps::Footprint::all({1}))}});
  require(!result.ok() && result.status().code == ps::ErrorCode::TypeMismatch &&
              result.status().reason == ps::FailureReason::None &&
              result.status().detail.origin == ps::FailureOrigin::Schema,
          "q shape mismatch classification");
  auto oversized = registry->resolve_traits(
      node.operation,
      {{{ps::ElementType::UInt8, {(UINT64_C(1) << 40) + 1}}, {}}},
      node.parameters);
  require(!oversized.ok() &&
              oversized.status().code == ps::ErrorCode::TypeMismatch &&
              oversized.status().reason == ps::FailureReason::None &&
              oversized.status().detail.origin == ps::FailureOrigin::Schema,
          "oversized descriptor classification");
  std::cout << "sorting WorkLimit/cancel attempts and cleanup, descriptor/q "
               "shape classification passed\n";
}

void typed_layout_environment(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  for (unsigned kind = 0; kind < 3; ++kind) {
    const bool quantile = kind == 2;
    auto node = quantile ? take(ps::numeric::quantile_node(
                               1, ps::WorkflowInputReference{1},
                               ps::WorkflowInputReference{2}, 1,
                               ps::ElementType::Float64, profile))
                         : take(ps::numeric::sort_node(
                               1, ps::WorkflowInputReference{1}, 1, profile));
    const auto raw = array(ps::ElementType::Float32, {1, 1, 4},
                           {0x3f800000, 0, 0, 0x40000000});
    const auto bad = take(ps::Value::from_storage(
        raw.descriptor(), raw.region(), raw.layout(), raw.storage(), {facet}));
    std::vector<ps::Value> inputs{bad};
    if (quantile)
      inputs.push_back(
          array(ps::ElementType::Float64, {1}, {0x7ff0000000000001}));
    Fixture empty(node, inputs, !quantile);
    require(empty
                .run({{kind == 1 ? "indices" : "values",
                       take(ps::Footprint::none({1, 1, 4}))}})
                .ok(),
            "Empty skips invalid typed input and q");
    std::vector<ps::Region> demands;
    for (const auto& value : inputs)
      demands.push_back(value.region());
    ps::OperationInvocation call(inputs, demands, node.parameters,
                                 ps::Backend::Cpu, {},
                                 ps::Region::whole({1, 1, 4}));
    call.output_index = kind == 1 ? 1 : 0;
    require(!registry->invoke(node.operation, call).ok(),
            "both sort outputs and singleton quantile validate complete typed "
            "input");
    ps::CancellationSource cancellation;
    cancellation.cancel();
    ps::OperationInvocation stopped(inputs, demands, node.parameters,
                                    ps::Backend::Cpu, cancellation.token(),
                                    ps::Region::whole({1, 1, 4}));
    stopped.output_index = call.output_index;
    require(registry->invoke(node.operation, stopped).status().code ==
                ps::ErrorCode::Cancelled,
            "Whole ordering pre-cancelled");
  }
  const auto packed =
      array(ps::ElementType::Float32, {6},
            {UINT64_C(0x7f800011), UINT64_C(0x80000000), 0,
             UINT64_C(0x40000000), UINT64_C(0xbf800000), UINT64_C(0xff800022)});
  const auto strided = take(ps::Value::from_storage(
      {ps::ElementType::Float32, {2, 3}}, ps::Region::whole({2, 3}),
      {8, {12, -4}}, packed.storage()));
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save fenv");
  for (unsigned kind = 0; kind < 3; ++kind) {
    const bool quantile = kind == 2;
    auto node = quantile ? take(ps::numeric::quantile_node(
                               1, ps::WorkflowInputReference{1},
                               ps::WorkflowInputReference{2}, 1,
                               ps::ElementType::Float32, profile))
                         : take(ps::numeric::sort_node(
                               1, ps::WorkflowInputReference{1}, 1, profile));
    std::vector<ps::Value> inputs{strided};
    std::vector<ps::Region> regions{strided.region()};
    if (quantile) {
      inputs.push_back(
          array(ps::ElementType::Float64, {1}, {UINT64_C(0x3fe0000000000000)}));
      regions.push_back(inputs.back().region());
    }
    const std::vector<std::uint64_t> expected =
        kind == 0   ? std::vector<std::uint64_t>{0,
                                                 UINT64_C(0x80000000),
                                                 UINT64_C(0x7f800011),
                                                 UINT64_C(0xbf800000),
                                                 UINT64_C(0x40000000),
                                                 UINT64_C(0xff800022)}
        : kind == 1 ? std::vector<std::uint64_t>{0, 1, 2, 1, 2, 0}
                    : std::vector<std::uint64_t>{UINT64_C(0x7fc00011),
                                                 UINT64_C(0xffc00022)};
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "prepare fenv");
      ps::OperationInvocation invocation(
          inputs, regions, node.parameters, ps::Backend::Cpu, {},
          ps::Region::whole(quantile ? std::vector<std::uint64_t>{2, 1}
                                     : std::vector<std::uint64_t>{2, 3}));
      invocation.output_index = kind == 1 ? 1 : 0;
      auto value = take(registry->invoke(node.operation, invocation));
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "raw sort/exact quantile preserve caller rounding and flags");
      const auto width =
          ps::Value::element_size(value.descriptor().element_type);
      for (std::size_t j = 0; j < expected.size(); ++j) {
        std::uint64_t actual = 0;
        std::memcpy(&actual, value.bytes().data() + j * width, width);
        require(actual == expected[j],
                "negative-stride values/indices and first-NaN conversion");
      }
    }
  }
  require(fesetenv(&saved) == 0, "restore fenv");
  std::cout << "sort values/indices and quantile: typed closure, Empty, "
               "negative strides and four fenv modes/flags passed\n";
}

struct BlockProbeState {
  bool requested = false;
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    using Answer = ps::Result<ps::DependencyPoll>;
    const auto atom = take(ps::dependency_atom_key(phase.query));
    const std::vector<std::uint64_t> at(atom.coordinate.begin(),
                                        atom.coordinate.begin() + atom.rank);
    if (!requested) {
      requested = true;
      return Answer(
          ps::DependencyNeedBatch{{{at,
                                    {{0,
                                      phase.query.output_index ? 2U : 1U,
                                      phase.query.outputs,
                                      {}}}}},
                                  {}});
    }
    auto incoming = take(
        ps::MutableValue::allocate({ps::ElementType::Int64, {1}},
                                   ps::Region::whole({1}), phase.allocator));
    std::memset(incoming.data(), 0, 8);
    auto state = take(std::move(incoming).publish());
    auto computed =
        phase.block(1, 0, 1, 1, state, [&]() -> ps::Result<ps::Value> {
          std::uint64_t bits = 0;
          auto status = phase.read(0, at, &bits, 1);
          if (!status.ok())
            return ps::Result<ps::Value>(status);
          auto output = take(ps::MutableValue::allocate(
              {ps::ElementType::Int64, {1}}, ps::Region::whole({1}),
              phase.allocator));
          std::memcpy(output.data(), &bits, 8);
          return std::move(output).publish();
        });
    if (!computed.ok())
      return Answer(computed.status());
    auto output = take(ps::MutableValue::allocate(
        phase.query.output.descriptor, phase.query.outputs.boxes()[0],
        phase.allocator));
    std::memcpy(output.data(), computed.value().bytes().data(), output.size());
    auto value = take(std::move(output).publish());
    return Answer(take(ps::ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs, {value})));
  }
};
ps::OperationDefinition block_probe(bool shared) {
  ps::OperationDefinition definition;
  definition.key = "manual.block_probe";
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.workspace_bytes = 16;
  traits.share_blocks_across_outputs = shared;
  traits.outputs.resize(2);
  for (unsigned i = 0; i < 2; ++i) {
    auto& output = traits.outputs[i];
    output.key = i ? "indices" : "values";
    output.output_element_type =
        i ? ps::ElementType::Int64 : ps::ElementType::UInt8;
    output.shape_rule = ps::OperationShapeRule::Fixed;
    output.fixed_output_shape = {2};
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(BlockProbeState);
    output.maximum_dependency_stages = 2;
  }
  definition.start_dependency = [](const auto&, const auto& allocator) {
    return ps::DependencyContinuation::make<BlockProbeState>(allocator);
  };
  return definition;
}
void block_contracts() {
  for (bool shared : {false, true}) {
    auto registry = std::make_shared<ps::OperationRegistry>();
    require(registry->register_operation(block_probe(shared)).ok() &&
                registry->freeze().ok(),
            "register block contract probe");
    std::map<std::string, ps::Value> cache;
    unsigned hits = 0;
    ps::DependencyBlockServices services;
    services.consume_work = [](std::uint64_t) { return true; };
    services.find = [&](const std::string& key) {
      const auto found = cache.find(key);
      if (found != cache.end())
        ++hits;
      return ps::Result<ps::Value>(found == cache.end() ? ps::Value{}
                                                        : found->second);
    };
    services.publish = [&](const std::string& key, const ps::Value& value) {
      require(cache.size() < 8, "bounded manual cache");
      cache[key] = value;
      return ps::Status::success();
    };
    auto source = array(ps::ElementType::UInt8, {2}, {4, 4});
    for (unsigned serial = 0; serial < 4; ++serial) {
      const auto coordinate = serial == 2 ? 1U : 0U;
      const auto output = serial == 0 ? 0U : 1U;
      if (serial == 3)
        source = array(ps::ElementType::UInt8, {2}, {5, 4});
      ps::DependencyRequest request;
      request.inputs = {{source.descriptor(), {}}};
      request.outputs = take(
          ps::Footprint::from_regions({2}, {ps::Region({{coordinate, 1}})}));
      request.snapshot_identity = "block-probe";
      request.output_index = output;
      auto session =
          take(registry->start_dependency("manual.block_probe", request));
      require(session->poll().ok(), "probe need");
      auto supplied = take(ps::ValueFragments::create(
          source.descriptor(), {}, request.outputs,
          {take(source.view(request.outputs.boxes()[0]))}));
      require(session->supply({supplied}, request.snapshot_identity).ok(),
              "probe supply");
      auto progress = take(session->poll(ps::BufferAllocator{}, {}, services));
      const auto& result = std::get<ps::DependencyResult>(progress);
      std::uint64_t actual = 0;
      require(result.value.read({coordinate}, &actual, output ? 8 : 1).ok() &&
                  actual == (serial == 3 ? 5U : 4U),
              "shared block current bits and output dtype");
      unsigned roles = 0;
      for (const auto& need :
           take(result.certificate->backward(request.outputs)))
        roles |= need.roles & 3;
      require(roles == (output ? 2U : 1U),
              "cache reuse never imports other output Data/Control witness");
      if (serial == 1)
        require(hits == (shared ? 1U : 0U), "explicit block scope true/false");
    }
    require(hits == (shared ? 1U : 0U),
            "line coordinate and changed raw byte prevent reuse");
  }
  for (unsigned invalid = 0; invalid < 4; ++invalid) {
    auto definition = block_probe(true);
    if (invalid == 0) {
      definition.traits.deterministic = false;
      definition.traits.cacheable = false;
    } else if (invalid == 1) {
      definition.traits.outputs[0].regional_atomic = true;
    } else if (invalid == 2) {
      definition.traits.outputs[0].observation_kind =
          ps::ObservationKind::RequestRecord;
    } else {
      definition.traits.outputs[0].static_dependency_pieces =
          std::vector<ps::DependencyMapPiece>{
              {take(ps::Footprint::all({2})), {}}};
    }
    ps::OperationRegistry registry;
    require(registry.register_operation(std::move(definition)).code ==
                ps::ErrorCode::InvalidArgument,
            "reject incompatible shared block contract");
  }
  std::cout << "public shared-block contract: opt-in scope, differing output "
               "dtype/roles, changed coordinates/bits and invalid combinations "
               "passed\n";
}

void changed_probability_and_source(ps::CpuNumericProfile profile) {
  Fixture fixture(
      take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                      ps::WorkflowInputReference{2}, 0,
                                      ps::ElementType::Float64, profile)),
      {array(ps::ElementType::Int64, {4}, {0, 10, 20, 30}),
       array(ps::ElementType::Float64, {1}, {UINT64_C(0x3fd0000000000000)})},
      false);
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
  config.result_cache_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  auto demand = take(context.open_demand(plan.plan, fixture.bindings));
  const ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
  auto first = take(demand.request(query));
  require(take(demand.request(query)).diagnostics.cache_hits > 0,
          "warm quantile output cache");
  fixture.bindings.inputs[1].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(array(ps::ElementType::Float64, {1},
                                            {UINT64_C(0x3fe8000000000000)}))));
  require(demand.replace_bindings(fixture.bindings).ok(), "replace q snapshot");
  auto changed = take(demand.request(query));
  std::uint64_t bits = 0;
  require(changed.values.at("values").read({0}, &bits, 8).ok() &&
              bits == UINT64_C(0x4036800000000000) &&
              changed.diagnostics.block_cache_hits == 0,
          "changed q recomputes Whole exact quantile 22.5");
  auto qdirty = take(changed.dependencies.potential_dirty(
      "input1", take(ps::Footprint::all({1}))));
  require(qdirty.at("values") == query.at("values"),
          "q Whole input witness invalidates quantile");
  fixture.bindings.inputs[0].snapshot =
      std::make_shared<const ps::InputSnapshot>(take(snapshots.import_value(
          array(ps::ElementType::Int64, {4}, {30, 10, 20, 0}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "replace source snapshot");
  changed = take(demand.request(query));
  require(changed.values.at("values").read({0}, &bits, 8).ok() &&
              bits == UINT64_C(0x4036800000000000) &&
              changed.diagnostics.block_cache_misses == 0 &&
              changed.diagnostics.block_cache_hits == 0,
          "changed source invalidates old permutation even with identical "
          "sorted values");
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  const auto raw = array(ps::ElementType::Float32, {1, 1, 4},
                         {UINT64_C(0x3f800000), 0, 0, UINT64_C(0x40000000)});
  const auto invalid = take(ps::Value::from_storage(
      raw.descriptor(), raw.region(), raw.layout(), raw.storage(), {facet}));
  for (unsigned kind = 0; kind < 3; ++kind) {
    const bool quantile = kind == 2;
    auto node = quantile ? take(ps::numeric::quantile_node(
                               1, ps::WorkflowInputReference{1},
                               ps::WorkflowInputReference{2}, 1,
                               ps::ElementType::Float64, profile))
                         : take(ps::numeric::sort_node(
                               1, ps::WorkflowInputReference{1}, 1, profile));
    std::vector<ps::Value> inputs{invalid};
    std::vector<ps::Region> regions{invalid.region()};
    if (quantile) {
      inputs.push_back(
          array(ps::ElementType::Float64, {1}, {UINT64_C(0x7ff0000000000001)}));
      regions.push_back(inputs.back().region());
    }
    ps::OperationInvocation invocation(inputs, regions, node.parameters,
                                       ps::Backend::Cpu, {},
                                       ps::Region::whole({1, 1, 4}));
    invocation.output_index = kind == 1 ? 1 : 0;
    const auto answer = fixture.registry->invoke(node.operation, invocation);
    require(!answer.ok() &&
                answer.status().message.find("InvalidQuantileProbability") ==
                    std::string::npos,
            "typed closure failure preserved even for indices or unread "
            "singleton q");
  }
  std::cout << "warm quantile cache, q/source replacement, control dirty and "
               "typed failures passed\n";
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
      sharing_and_sparse(profile);
      nonlast_axis_lines(profile);
      probability_dependencies(profile);
      failure_and_schema(profile);
      typed_layout_environment(profile);
      block_contracts();
      changed_probability_and_source(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
