#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/lut1d.hpp"
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
std::uint64_t raw(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  return bits;
}
ps::Value doubles(const std::vector<std::uint64_t>& shape,
                  const std::vector<double>& values) {
  std::vector<std::uint64_t> bits;
  for (auto value : values)
    bits.push_back(raw(value));
  return array(ps::ElementType::Float64, shape, bits);
}
struct Fixture {
  unsigned kind, count, columns;
  ps::CpuNumericProfile profile;
  ps::ElementType dtype;
  std::vector<double> expected;
  std::vector<ps::Value> inputs;
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  ps::numeric::BakedLut1d baked;
  std::string expression = "x^2";
  std::map<std::string, ps::WorkflowInput> coefficients;
  Fixture(unsigned selected, ps::CpuNumericProfile cpu,
          ps::ElementType output = ps::ElementType::Float64)
      : kind(selected),
        count(selected == 3   ? 5
              : selected == 5 ? 2
                              : 3),
        columns(selected >= 4 ? 2 : 1),
        profile(cpu),
        dtype(output) {
    if (kind == 0) {
      inputs = {doubles({1}, {0}), doubles({1}, {1})};
      expected = {0, .25, 1};
    }
    if (kind == 1) {
      inputs = {doubles({1}, {0}), doubles({1}, {1}),
                doubles({2, 2}, {0, 0, 1, 1}), doubles({1, 1, 2}, {.5, 0})};
      expected = {0, .25, 1};
    }
    if (kind == 2) {
      inputs = {doubles({1}, {0}), doubles({1}, {2}), doubles({3}, {0, 1, 2}),
                doubles({3}, {0, 2, 4})};
      expected = {0, 2, 4};
    }
    if (kind == 3) {
      inputs = {doubles({1}, {0}), doubles({1}, {2}), doubles({3}, {0, 1, 2}),
                doubles({3}, {0, 1, 4})};
      expected = {0, .3125, 1, 2.1875, 4};
    }
    if (kind == 4) {
      inputs = {doubles({1}, {0}), doubles({1}, {1}), doubles({2}, {0, 1}),
                doubles({2, 2}, {0, 10, 2, 8})};
      expected = {0, 10, 1, 9, 2, 8};
    }
    if (kind == 5) {
      inputs = {doubles({1}, {.5}), doubles({1}, {1.5}),
                doubles({3}, {0, 1, 2}), doubles({3, 2}, {0, 4, 1, 3, 4, 0})};
      expected = {.3125, 3.6875, 2.1875, 1.8125};
    }
    for (unsigned i = 0; i < inputs.size(); ++i) {
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1, name, inputs[i].descriptor(),
                                 inputs[i].region(), inputs[i].layout(),
                                 inputs[i].facets()});
      bindings.inputs.push_back({name, inputs[i]});
    }
  }
  std::vector<std::uint64_t> shape() const {
    return kind >= 4 ? std::vector<std::uint64_t>{count, columns}
                     : std::vector<std::uint64_t>{count};
  }
  ps::Result<ps::numeric::BakedLut1d> author(ps::numeric::SequenceInput start,
                                             ps::numeric::SequenceInput end) {
    if (kind == 0)
      return ps::numeric::bake_lut1d_expression(
          document, expression, std::move(start), std::move(end), count,
          coefficients, dtype, profile);
    if (kind == 1)
      return ps::numeric::bake_lut1d_bezier(
          document, ps::WorkflowInputReference{3},
          ps::WorkflowInputReference{4}, std::move(start), std::move(end), 2,
          count, dtype, ps::numeric::BezierDomain::Reject, profile);
    const auto helper = kind == 2   ? ps::numeric::bake_lut1d_linear
                        : kind == 3 ? ps::numeric::bake_lut1d_pchip
                        : kind == 4 ? ps::numeric::bake_lut1d_linear_multi
                                    : ps::numeric::bake_lut1d_pchip_multi;
    return helper(document, ps::WorkflowInputReference{3},
                  ps::WorkflowInputReference{4}, std::move(start),
                  std::move(end), count, dtype,
                  ps::numeric::CurveDomain::Reject, profile);
  }
  void build(bool manual = false) {
    auto start = ps::numeric::sequence_input(document.inputs[0]);
    auto end = ps::numeric::sequence_input(document.inputs[1]);
    if (!manual) {
      baked = take(author(start, end));
    } else {
      const std::string suffix =
          profile == ps::CpuNumericProfile::Strict ? "_strict"
          : profile == ps::CpuNumericProfile::AppleSiliconNeon
              ? "_accelerated_apple_silicon"
              : "_accelerated_x86_64";
      const std::string type =
          dtype == ps::ElementType::Float32 ? "float32" : "float64";
      if (kind == 0) {
        document.nodes.push_back({1,
                                  "numeric.sample_expression" + suffix,
                                  {start.source, end.source},
                                  {{"expression", expression},
                                   {"coefficient_names", std::string{}},
                                   {"count", static_cast<std::int64_t>(count)},
                                   {"dtype", type}}});
      } else if (kind == 1) {
        document.nodes.push_back(
            {1,
             "curve.sample_bezier_function" + suffix,
             {ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4},
              start.source, end.source},
             {{"degree", std::int64_t{2}},
              {"count", static_cast<std::int64_t>(count)},
              {"dtype", type},
              {"out_of_domain", std::string("reject")}}});
      } else {
        document.nodes.push_back({1,
                                  "numeric.linspace" + suffix,
                                  {start.source, end.source},
                                  {{"count", static_cast<std::int64_t>(count)},
                                   {"dtype", std::string("float64")}}});
        const std::string op = kind == 2   ? "interpolate_linear"
                               : kind == 3 ? "interpolate_pchip"
                               : kind == 4 ? "interpolate_linear_multi"
                                           : "interpolate_pchip_multi";
        document.nodes.push_back(
            {2,
             "curve." + op + suffix,
             {ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4},
              ps::WorkflowNodeOutput{1, "values"}},
             {{"dtype", type}, {"out_of_domain", std::string("reject")}}});
      }
      baked = {{kind < 2 ? 1U : 2U, "values"}, {1, "axis"}};
    }
    auto exported = baked.outputs();
    document.outputs.assign(exported.begin(), exported.end());
  }
  ps::Result<ps::DemandResult> run(const ps::DemandQuery& query,
                                   ps::ExecutionOptions options = {},
                                   std::uint64_t payload = 8 * 1024 * 1024,
                                   ps::CancellationToken cancellation = {},
                                   std::uint64_t work = UINT64_MAX) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = payload;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->maximum_work = work;
    ps::ExecutionContext context(registry, config);
    auto frozen = context.freeze(plan.value().plan, bindings);
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    return context.execute_fragments(frozen.value(), query, cancellation,
                                     options);
  }
};
ps::ExecutionOptions budgets() {
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(64) << 30;
  options.dependencies.maximum_work = UINT64_C(32) << 30;
  return options;
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
void check_value(const ps::ValueFragments& values,
                 const std::vector<std::uint64_t>& at, double expected) {
  std::uint64_t bits = 0;
  const auto width = ps::Value::element_size(values.descriptor().element_type);
  std::uint64_t want = raw(expected);
  if (width == 4) {
    float f = static_cast<float>(expected);
    want = 0;
    std::memcpy(&want, &f, 4);
  }
  require(values.read(at, &bits, width).ok() && bits == want,
          "baking expected bits");
}
void compare(const ps::DemandResult& generated,
             const ps::DemandResult& explicit_result) {
  require(generated.values.size() == explicit_result.values.size(),
          "export count equivalence");
  for (const auto& item : generated.values) {
    const auto& other = explicit_result.values.at(item.first);
    require(item.second.descriptor().shape == other.descriptor().shape &&
                item.second.descriptor().element_type ==
                    other.descriptor().element_type &&
                item.second.coverage() == other.coverage(),
            "bake descriptor/coverage equivalence");
    require(item.second.coverage()
                .visit(
                    [&](const auto& at) {
                      std::uint64_t a = 0, b = 0;
                      auto width = ps::Value::element_size(
                          item.second.descriptor().element_type);
                      require(item.second.read(at, &a, width).ok() &&
                                  other.read(at, &b, width).ok() && a == b,
                              "bake exact graph equivalence");
                      return ps::Status::success();
                    },
                    1024)
                .ok(),
            "bake visit");
  }
  auto sources = take(generated.dependencies.source_support());
  require(sources == take(explicit_result.dependencies.source_support()),
          "bake source support equivalence");
  for (const auto& source : sources)
    require(take(generated.dependencies.potential_dirty(source.first,
                                                        source.second)) ==
                take(explicit_result.dependencies.potential_dirty(
                    source.first, source.second)),
            "bake dirty equivalence");
}
void graph_equivalence(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 6; ++kind)
    for (auto dtype : {ps::ElementType::Float64, ps::ElementType::Float32}) {
      Fixture generated(kind, profile, dtype), manual(kind, profile, dtype);
      // Exercise independent endpoint dtypes with exact binary32 fixture
      // values.
      float start = kind == 5 ? .5f : 0.0f;
      std::uint32_t bits = 0;
      std::memcpy(&bits, &start, 4);
      for (auto* f : {&generated, &manual}) {
        auto input = array(ps::ElementType::Float32, {1}, {bits});
        f->inputs[0] = input;
        f->bindings.inputs[0].value = input;
        f->document.inputs[0].descriptor = input.descriptor();
        f->document.inputs[0].layout = input.layout();
      }
      generated.build();
      manual.build(true);
      require(generated.document.nodes.size() == (kind < 2 ? 1U : 2U),
              "ordinary expansion node count");
      for (const auto& node : generated.document.nodes) {
        auto traits = take(generated.registry->find_traits(node.operation));
        for (const auto& output : traits.outputs)
          require(output.region_rule == ps::OperationRegionRule::Whole &&
                      output.dependency_version == 0,
                  "all expanded formal source outputs use Whole");
      }
      for (unsigned mode = 0; mode < 4; ++mode) {
        ps::DemandQuery query;
        if (mode != 1) {
          auto wanted = take(ps::Footprint::all(generated.shape()));
          if (mode == 3)
            wanted =
                kind >= 4
                    ? region(generated.shape(),
                             {ps::Region({{generated.count / 2, 1}, {1, 1}})})
                    : region(generated.shape(),
                             {ps::Region({{generated.count / 2, 1}})});
          query.emplace("values", wanted);
        }
        if (mode == 1 || mode == 2)
          query.emplace("axis", take(ps::Footprint::all({3})));
        auto actual = take(generated.run(query, budgets()));
        auto expected = take(manual.run(query, budgets()));
        compare(actual, expected);
        if (mode == 0) {
          for (unsigned i = 0; i < generated.count; ++i)
            for (unsigned c = 0; c < generated.columns; ++c)
              check_value(actual.values.at("values"),
                          kind >= 4 ? std::vector<std::uint64_t>{i, c}
                                    : std::vector<std::uint64_t>{i},
                          generated.expected[i * generated.columns + c]);
        }
        if (mode == 1) {
          auto support = take(actual.dependencies.source_support());
          require(support.size() == 2 && support.count("input0") &&
                      support.count("input1"),
                  "axis skips function controls");
        }
      }
    }
  std::cout
      << "six generated/explicit graphs, analytic values, mixed endpoints, "
         "both dtypes, independent outputs and sparse witnesses passed\n";
}
void replace_with_failure(Fixture* fixture, unsigned input, unsigned* calls) {
  auto registry = ps::make_default_operation_registry(false);
  ps::OperationDefinition operation;
  operation.key = "manual.bake_failure";
  operation.traits.input_count = 0;
  operation.traits.input_schema.clear();
  operation.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
  operation.traits.outputs[0].fixed_output_shape =
      fixture->inputs[input].descriptor().shape;
  operation.traits.outputs[0].output_element_type =
      fixture->inputs[input].descriptor().element_type;
  operation.callback = [calls](const auto&) {
    ++*calls;
    return ps::Result<ps::Value>(
        ps::Status{ps::ErrorCode::OperationFailed, "required baking producer"});
  };
  require(registry->register_operation(std::move(operation)).ok() &&
              registry->freeze().ok(),
          "bake failing producer registry");
  fixture->registry = registry;
  for (auto& node : fixture->document.nodes)
    for (auto& edge : node.inputs)
      if (auto* ref = std::get_if<ps::WorkflowInputReference>(&edge);
          ref && ref->input_id == input + 1)
        edge = ps::WorkflowNodeOutput{100, "value"};
  fixture->document.nodes.push_back({100, "manual.bake_failure", {}, {}});
  fixture->document.inputs.erase(fixture->document.inputs.begin() + input);
  fixture->bindings.inputs.erase(fixture->bindings.inputs.begin() + input);
}
void source_semantics(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 6; ++kind) {
    Fixture singleton(kind, profile);
    singleton.count = 1;
    singleton.build();
    unsigned calls = 0;
    replace_with_failure(&singleton, 1, &calls);
    auto both = take(
        singleton.run({{"values", take(ps::Footprint::all(singleton.shape()))},
                       {"axis", take(ps::Footprint::all({3}))}},
                      budgets()));
    require(calls == 0, "all six N1 skip failing end");
    for (unsigned c = 0; c < singleton.columns; ++c)
      check_value(both.values.at("values"),
                  kind >= 4 ? std::vector<std::uint64_t>{0, c}
                            : std::vector<std::uint64_t>{0},
                  singleton.expected[c]);
    check_value(both.values.at("axis"), {0}, kind == 5 ? .5 : 0);
    check_value(both.values.at("axis"), {1}, kind == 5 ? .5 : 0);
    check_value(both.values.at("axis"), {2}, 0);
    Fixture first(kind, profile);
    first.build();
    calls = 0;
    replace_with_failure(&first, 1, &calls);
    auto requested = kind >= 4
                         ? region(first.shape(), {ps::Region({{0, 1}, {0, 1}})})
                         : region(first.shape(), {ps::Region({{0, 1}})});
    auto at_start = first.run({{"values", requested}}, budgets());
    require(
        !at_start.ok() &&
            at_start.status().message == "required baking producer" &&
            calls == 1,
        "all Whole baking sources retain N>1 end even for first-value demand");
    Fixture isolated(kind, profile);
    unsigned bad_source = kind == 1 ? 2 : 3;
    if (kind == 0) {
      isolated.expression = "a*x^2";
      isolated.coefficients = {{"a", ps::WorkflowInputReference{3}}};
      auto coefficient = doubles({1}, {1});
      isolated.inputs.push_back(coefficient);
      isolated.document.inputs.push_back({3,
                                          "input2",
                                          coefficient.descriptor(),
                                          coefficient.region(),
                                          coefficient.layout(),
                                          {}});
      isolated.bindings.inputs.push_back({"input2", coefficient});
      bad_source = 2;
    }
    isolated.build();
    calls = 0;
    replace_with_failure(&isolated, bad_source, &calls);
    auto axis = take(
        isolated.run({{"axis", take(ps::Footprint::all({3}))}}, budgets()));
    require(calls == 0, "axis ignores failing function producer");
    auto failed = isolated.run(
        {{"values", take(ps::Footprint::all(isolated.shape()))}}, budgets());
    require(!failed.ok() &&
                failed.status().message == "required baking producer" &&
                calls == 1,
            "values preserve function producer failure");
    Fixture equal(kind, profile);
    equal.inputs[1] = equal.inputs[0];
    equal.bindings.inputs[1].value = equal.inputs[1];
    equal.build();
    auto repeated =
        equal.run({{"values", take(ps::Footprint::all(equal.shape()))},
                   {"axis", take(ps::Footprint::all({3}))}},
                  budgets());
    if (kind < 2) {
      require(!repeated.ok() &&
                  repeated.status().reason == ps::FailureReason::InvalidDomain,
              "sampler equal endpoints reject");
    } else {
      auto result = take(std::move(repeated));
      for (unsigned i = 0; i < equal.count; ++i)
        for (unsigned c = 0; c < equal.columns; ++c)
          check_value(result.values.at("values"),
                      kind >= 4 ? std::vector<std::uint64_t>{i, c}
                                : std::vector<std::uint64_t>{i},
                      equal.expected[c]);
      check_value(result.values.at("axis"), {2}, 0);
    }
  }
  std::cout
      << "six N1/failing-end paths, axis/source isolation, first-coordinate "
         "dependencies and source-specific equal endpoints passed\n";
}
void dynamic_and_limits(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 6; ++kind) {
    Fixture fixture(kind, profile);
    fixture.build();
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::InputSnapshotStore store;
    for (auto& binding : fixture.bindings.inputs) {
      binding.snapshot = std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(binding.value)));
      binding.value = {};
    }
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 4 * 1024 * 1024;
    config.maximum_live_bytes = 8 * 1024 * 1024;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    auto demand = take(context.open_demand(plan.plan, fixture.bindings));
    auto options = budgets();
    options.maximum_dependency_cache_work = 128 * 1024 * 1024;
    ps::DemandQuery query{{"values", take(ps::Footprint::all(fixture.shape()))},
                          {"axis", take(ps::Footprint::all({3}))}};
    take(demand.request(query, {}, options));
    require(take(demand.request(query, {}, options)).diagnostics.cache_hits > 0,
            "bake warm cache");
    fixture.bindings.inputs[0].snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(store.import_value(fixture.inputs[1])));
    fixture.bindings.inputs[1].snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(store.import_value(fixture.inputs[0])));
    require(demand.replace_bindings(fixture.bindings).ok(),
            "replace bake endpoints");
    auto reversed = take(demand.request(query, {}, options));
    for (unsigned i = 0; i < fixture.count; ++i)
      for (unsigned c = 0; c < fixture.columns; ++c)
        check_value(
            reversed.values.at("values"),
            kind >= 4 ? std::vector<std::uint64_t>{i, c}
                      : std::vector<std::uint64_t>{i},
            fixture.expected[(fixture.count - 1 - i) * fixture.columns + c]);
    double a = 0, b = 0;
    std::memcpy(&a, fixture.inputs[0].bytes().data(), 8);
    std::memcpy(&b, fixture.inputs[1].bytes().data(), 8);
    check_value(reversed.values.at("axis"), {0}, b);
    check_value(reversed.values.at("axis"), {1}, a);
    check_value(reversed.values.at("axis"), {2}, (a - b) / (fixture.count - 1));
    Fixture limits(kind, profile);
    limits.build();
    auto limited = budgets();
    limited.dependencies.maximum_work = 1;
    auto exhausted = limits.run(query, limited, 8 * 1024 * 1024, {}, 1);
    require(!exhausted.ok() &&
                exhausted.status().code == ps::ErrorCode::ResourceExhausted,
            "bake source work failure");
    exhausted = limits.run(query, budgets(), 1);
    require(!exhausted.ok() &&
                exhausted.status().code == ps::ErrorCode::ResourceExhausted,
            "bake shared payload cap");
    ps::CancellationSource cancellation;
    cancellation.cancel();
    auto failed =
        limits.run(query, budgets(), 8 * 1024 * 1024, cancellation.token());
    require(!failed.ok() && failed.status().code == ps::ErrorCode::Cancelled,
            "bake cancellation propagation");
    require(limits.run(query, budgets()).ok(),
            "bake recovery after bounded failures");
  }
  Fixture sparse(5, profile);
  sparse.count = 1048576;
  sparse.build();
  auto q = region(sparse.shape(), {ps::Region({{0, 1}, {1, 1}}),
                                   ps::Region({{1048575, 1}, {1, 1}})});
  auto result = sparse.run({{"values", q}}, budgets(), 1024 * 1024);
  require(
      !result.ok() && result.status().code == ps::ErrorCode::ResourceExhausted,
      "million-row sparse bake requires complete query and table payload");
  std::cout << "six cached reversed bindings, work/payload/cancel recovery and "
               "full-output budget passed\n";
}

void layouts_and_active_cancellation(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 6; ++kind) {
    Fixture fixture(kind, profile);
    ps::InputSnapshotStore snapshots;
    for (unsigned port = 0; port < fixture.inputs.size(); ++port) {
      const auto& input = fixture.inputs[port];
      const auto width =
          ps::Value::element_size(input.descriptor().element_type);
      const auto count = input.bytes().size() / width;
      auto storage =
          take(ps::BufferAllocator{}.allocate(input.bytes().size() + 1));
      for (std::uint64_t i = 0; i < count; ++i)
        std::memcpy(storage.data() + 1 + (count - 1 - i) * width,
                    input.bytes().data() + i * width, width);
      std::vector<std::int64_t> strides(input.descriptor().shape.size());
      std::int64_t stride = -static_cast<std::int64_t>(width);
      for (auto j = strides.size(); j; --j) {
        strides[j - 1] = stride;
        stride *= input.descriptor().shape[j - 1];
      }
      if (count == 1)
        strides[0] = 0;
      auto view = take(ps::Value::from_storage(
          input.descriptor(), input.region(),
          {1 + (count - 1) * width, strides}, std::move(storage).freeze()));
      fixture.bindings.inputs[port].snapshot =
          std::make_shared<const ps::InputSnapshot>(
              take(snapshots.import_value(view)));
      fixture.bindings.inputs[port].value = {};
    }
    fixture.build();
    auto result = take(fixture.run(
        {{"values", take(ps::Footprint::all(fixture.shape()))}}, budgets()));
    for (unsigned i = 0; i < fixture.count; ++i)
      for (unsigned c = 0; c < fixture.columns; ++c)
        check_value(result.values.at("values"),
                    kind >= 4 ? std::vector<std::uint64_t>{i, c}
                              : std::vector<std::uint64_t>{i},
                    fixture.expected[i * fixture.columns + c]);
    Fixture active(kind, profile);
    active.count = 4097;
    active.build();
    ps::GraphContext graph(active.document);
    auto plan = take(ps::Compiler(active.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = 8 * 1024 * 1024;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(active.registry, config);
    auto frozen = take(context.freeze(plan.plan, active.bindings));
    auto budget = take(context.resource_budget());
    const auto work = budget.statistics().issued.work;
    ps::CancellationSource cancel;
    std::atomic<bool> done{false};
    std::thread watcher([&] {
      while (!done.load() && budget.statistics().issued.work < work + 10000)
        std::this_thread::yield();
      if (!done.load())
        cancel.cancel();
    });
    ps::Result<ps::DemandResult> cancelled(
        ps::Status{ps::ErrorCode::Internal, "not executed"});
    try {
      cancelled = context.execute_fragments(
          frozen, {{"values", take(ps::Footprint::all(active.shape()))}},
          cancel.token(), budgets());
    } catch (...) {
      done.store(true);
      watcher.join();
      throw;
    }
    done.store(true);
    watcher.join();
    require(!cancelled.ok() &&
                cancelled.status().code == ps::ErrorCode::Cancelled &&
                budget.statistics().issued.work >= work + 10000 &&
                budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "active cancellation releases complete template intermediates");
  }
  std::cout << "six templates all-port reversed/unaligned/scalar-zero snapshot "
               "imports "
               "and active cancellation passed\n";
}

void reusable_exports(ps::CpuNumericProfile profile) {
  Fixture fixture(0, profile);
  fixture.build();
  auto second = take(ps::numeric::bake_lut1d_expression(
      fixture.document, "x+1",
      ps::numeric::sequence_input(fixture.document.inputs[0]),
      ps::numeric::sequence_input(fixture.document.inputs[1]), 3, {},
      ps::ElementType::Float64, profile));
  auto exports = second.outputs("offset_values", "offset_axis");
  fixture.document.outputs.insert(fixture.document.outputs.end(),
                                  exports.begin(), exports.end());
  auto result =
      take(fixture.run({{"values", take(ps::Footprint::all({3}))},
                        {"axis", take(ps::Footprint::all({3}))},
                        {"offset_values", take(ps::Footprint::all({3}))},
                        {"offset_axis", take(ps::Footprint::all({3}))}},
                       budgets()));
  check_value(result.values.at("values"), {1}, .25);
  check_value(result.values.at("offset_values"), {1}, 1.5);
  check_value(result.values.at("offset_axis"), {2}, .5);
  Fixture single_column(4, profile);
  single_column.columns = 1;
  auto y = doubles({2, 1}, {0, 2});
  single_column.inputs[3] = y;
  single_column.bindings.inputs[3].value = y;
  single_column.document.inputs[3] = {4,          "input3",   y.descriptor(),
                                      y.region(), y.layout(), {}};
  single_column.build();
  auto column = take(single_column.run(
      {{"values", take(ps::Footprint::all({3, 1}))}}, budgets()));
  require(column.values.at("values").descriptor().shape ==
              std::vector<std::uint64_t>{3, 1},
          "multi bake retains C1");
  check_value(column.values.at("values"), {1, 0}, 1);
  std::cout << "multiple reusable named exports and multi C1 shape passed\n";
}
void authoring_boundaries() {
  using ps::numeric::SequenceInput;
  const ps::ValueDescriptor scalar{ps::ElementType::Float64, {1}};
  SequenceInput start{ps::WorkflowNodeOutput{4, "values"}, scalar},
      end{ps::WorkflowNodeOutput{5, "values"}, scalar};
  ps::WorkflowDocument graph;
  graph.nodes = {{UINT64_MAX, "unresolved", {}, {}},
                 {1, "unresolved", {ps::WorkflowNodeOutput{2, "values"}}, {}}};
  graph.outputs = {{"reserved", 3, "values"}};
  auto expression = take(ps::numeric::bake_lut1d_expression(
      graph, "a*x", start, end, 3,
      {{"a", ps::WorkflowNodeOutput{6, "values"}}}));
  require(expression.values.source_node == 7 &&
              expression.axis.source_node == 7 && graph.outputs.size() == 1,
          "referenced ids and output labels reserved");
  auto linear = take(ps::numeric::bake_lut1d_linear(
      graph, ps::WorkflowNodeOutput{8, "values"},
      ps::WorkflowNodeOutput{9, "values"}, start, end, 3));
  require(linear.axis.source_node == 10 && linear.values.source_node == 11 &&
              graph.nodes.front().id == UINT64_MAX,
          "two-node smallest free ids without wrap");
  auto exports = linear.outputs("table2", "grid2");
  require(exports[0].name == "table2" && exports[1].name == "grid2",
          "caller named exports");
  const auto size = graph.nodes.size();
  for (unsigned kind = 0; kind < 5; ++kind) {
    auto badstart = start;
    if (kind == 0)
      badstart.descriptor.shape = {2};
    auto failed = ps::numeric::bake_lut1d_expression(
        graph, kind == 1 ? "a+" : "x", badstart, end, kind == 2 ? 0 : 3, {},
        kind == 3 ? ps::ElementType::Int64 : ps::ElementType::Float64,
        kind == 4 ? ps::CpuNumericProfile::Unspecified
                  : ps::CpuNumericProfile::Strict);
    require(
        !failed.ok() && graph.nodes.size() == size && graph.outputs.size() == 1,
        "failed authoring leaves no partial nodes");
  }
  auto failed = ps::numeric::bake_lut1d_linear(
      graph, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      start, end, 3, ps::ElementType::Float64,
      static_cast<ps::numeric::CurveDomain>(99));
  require(!failed.ok() && graph.nodes.size() == size,
          "second-node validation leaves no linspace");
  ps::WorkflowDocument full;
  full.nodes.reserve(65536);
  for (unsigned i = 1; i < 65536; ++i)
    full.nodes.push_back({i, "unresolved", {}, {}});
  SequenceInput bound{ps::WorkflowInputReference{1}, scalar};
  auto last =
      take(ps::numeric::bake_lut1d_expression(full, "x", bound, bound, 1));
  require(last.values.source_node == 65536 && full.nodes.size() == 65536,
          "last node capacity fits");
  require(
      !ps::numeric::bake_lut1d_expression(full, "x", bound, bound, 1).ok() &&
          full.nodes.size() == 65536,
      "node capacity overflow leaves graph");
  std::cout << "collision-free referenced ids, UINT64_MAX, explicit exports, "
               "failed/partial construction and 65536-node boundary passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    graph_equivalence(profile);
    source_semantics(profile);
    dynamic_and_limits(profile);
    layouts_and_active_cancellation(profile);
    reusable_exports(profile);
    authoring_boundaries();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
