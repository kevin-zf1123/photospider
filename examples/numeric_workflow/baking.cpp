#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
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
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/lut1d.hpp"
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
    rf::declare_sources(&document, inputs);
  }
  ps::Result<ps::ExecutionBindings> bindings(
      const ps::ResourceBudget& root) const {
    ps::ExecutionBindings bindings;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto& schema = *document.inputs[i].result_schema;
      auto builder =
          ps::ResultBuilder::start(root, schema, "manual.bake.source", {}, {},
                                   128, 128, inputs[i].resources());
      if (!builder.ok())
        return ps::Result<ps::ExecutionBindings>(builder.status());
      auto writable = builder.take_value();
      auto descriptor = ps::ResultRelation::cartesian(root, 1, {});
      if (!descriptor.ok())
        return ps::Result<ps::ExecutionBindings>(descriptor.status());
      auto status = writable.bind_descriptor_relation(descriptor.take_value());
      if (!status.ok())
        return ps::Result<ps::ExecutionBindings>(status);
      auto count = schema.tensors[0].sample_count();
      if (!count.ok())
        return ps::Result<ps::ExecutionBindings>(count.status());
      auto relation = ps::ResultRelation::cartesian(root, count.value(), {});
      if (!relation.ok())
        return ps::Result<ps::ExecutionBindings>(relation.status());
      status = writable.publish_tensor(
          0, inputs[i].region(), inputs[i].layout(), inputs[i].storage(),
          relation.take_value(), {true, true, true, true});
      if (!status.ok())
        return ps::Result<ps::ExecutionBindings>(status);
      auto published = writable.seal();
      if (!published.ok())
        return ps::Result<ps::ExecutionBindings>(published.status());
      bindings.inputs.push_back(
          {document.inputs[i].name, published.take_value()});
    }
    return ps::Result<ps::ExecutionBindings>(std::move(bindings));
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
    auto published = bindings(take(context.resource_budget()));
    if (!published.ok())
      return ps::Result<ps::DemandResult>(published.status());
    auto frozen = context.freeze(plan.value().plan, published.value());
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
void check_value(const ps::ResultRef& values,
                 const std::vector<std::uint64_t>& at, double expected) {
  std::uint64_t bits = 0;
  const auto width = ps::Value::element_size(
      values.schema().tensors[0].descriptor.element_type);
  std::uint64_t want = raw(expected);
  if (width == 4) {
    float f = static_cast<float>(expected);
    want = 0;
    std::memcpy(&want, &f, 4);
  }
  require(rf::read(values, at, &bits, width).ok() && bits == want,
          "baking expected bits");
}
void compare(const ps::DemandResult& generated,
             const ps::DemandResult& explicit_result) {
  require(generated.results.size() == explicit_result.results.size(),
          "export count equivalence");
  for (const auto& item : generated.results) {
    const auto& other = explicit_result.results.at(item.first);
    const auto& spec = item.second.schema().tensors[0];
    const auto& other_spec = other.schema().tensors[0];
    const auto coverage = take(item.second.descriptor()).tensor_coverage(0);
    require(spec.sample_shape() == other_spec.sample_shape() &&
                spec.descriptor.element_type ==
                    other_spec.descriptor.element_type &&
                coverage == take(other.descriptor()).tensor_coverage(0),
            "bake descriptor/coverage equivalence");
    require(coverage
                .visit(
                    [&](const auto& at) {
                      std::uint64_t a = 0, b = 0;
                      auto width =
                          ps::Value::element_size(spec.descriptor.element_type);
                      require(rf::read(item.second, at, &a, width).ok() &&
                                  rf::read(other, at, &b, width).ok() && a == b,
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
    require(take(generated.dependencies.potential_dirty(
                std::string(source.first.data(), source.first.size()),
                source.second)) ==
                take(explicit_result.dependencies.potential_dirty(
                    std::string(source.first.data(), source.first.size()),
                    source.second)),
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
        f->document.inputs[0].result_schema =
            std::make_shared<ps::SchemaTemplate>(rf::source_schema(input));
      }
      generated.build();
      manual.build(true);
      require(generated.document.nodes.size() == (kind < 2 ? 1U : 2U),
              "ordinary expansion node count");
      for (const auto& node : generated.document.nodes) {
        auto traits = take(generated.registry->find_traits(node.operation));
        for (const auto& output : traits.outputs)
          require(output.region_rule == ps::OperationRegionRule::Whole &&
                      output.dependency_version == 2,
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
        for (const auto& output : actual.results) {
          const auto shape = output.second.schema().tensors[0].sample_shape();
          require(take(output.second.descriptor()).tensor_coverage(0) ==
                      take(ps::Footprint::all(shape)),
                  "sparse baking returns full Whole coverage");
        }
        if (mode == 0) {
          for (unsigned i = 0; i < generated.count; ++i)
            for (unsigned c = 0; c < generated.columns; ++c)
              check_value(actual.results.at("values"),
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
struct FailedSource final {
  unsigned* calls;
  explicit FailedSource(unsigned* counter) : calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::OperationFailed, "required baking producer"});
  }
};
void replace_with_failure(Fixture* fixture, unsigned input, unsigned* calls) {
  auto registry = ps::make_default_operation_registry(false);
  ps::OperationDefinition operation;
  operation.key = "manual.bake_failure";
  operation.traits.input_count = 0;
  operation.traits.input_schema.clear();
  auto& output = operation.traits.outputs[0];
  output.region_rule = ps::OperationRegionRule::Whole;
  output.output_schema.kind = ps::OperationPortKind::Result;
  auto schema = *fixture->document.inputs[input].result_schema;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = std::move(schema);
  output.continuation_bytes = sizeof(FailedSource);
  output.maximum_dependency_stages = 8;
  operation.start_result = [calls](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<FailedSource>(allocator, calls);
  };
  require(registry->register_operation(std::move(operation)).ok() &&
              registry->freeze().ok(),
          "bake failing producer registry");
  fixture->registry = registry;
  const auto input_id = fixture->document.inputs[input].id;
  for (auto& node : fixture->document.nodes)
    for (auto& edge : node.inputs)
      if (auto* ref = std::get_if<ps::WorkflowInputReference>(&edge);
          ref && ref->input_id == input_id)
        edge = ps::WorkflowNodeOutput{100, "value"};
  fixture->document.nodes.push_back({100, "manual.bake_failure", {}, {}});
  fixture->document.inputs.erase(fixture->document.inputs.begin() + input);
  fixture->inputs.erase(fixture->inputs.begin() + input);
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
      check_value(both.results.at("values"),
                  kind >= 4 ? std::vector<std::uint64_t>{0, c}
                            : std::vector<std::uint64_t>{0},
                  singleton.expected[c]);
    check_value(both.results.at("axis"), {0}, kind == 5 ? .5 : 0);
    check_value(both.results.at("axis"), {1}, kind == 5 ? .5 : 0);
    check_value(both.results.at("axis"), {2}, 0);
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
      isolated.document.inputs.push_back({3, "input2",
                                          std::make_shared<ps::SchemaTemplate>(
                                              rf::source_schema(coefficient))});
      bad_source = 2;
    }
    isolated.build();
    calls = 0;
    replace_with_failure(&isolated, bad_source, &calls);
    auto empty = take(
        isolated.run({{"values", region(isolated.shape(), {})}}, budgets()));
    require(calls == 0 && take(empty.results.at("values").descriptor())
                              .tensor_coverage(0)
                              .empty(),
            "empty bake does not poll failing function producer");
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
          check_value(result.results.at("values"),
                      kind >= 4 ? std::vector<std::uint64_t>{i, c}
                                : std::vector<std::uint64_t>{i},
                      equal.expected[c]);
      check_value(result.results.at("axis"), {2}, 0);
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
    if (kind >= 2)
      fixture.document.outputs.push_back(
          {"query_values", fixture.document.nodes.front().id, "values"});
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 4 * 1024 * 1024;
    config.maximum_live_bytes = 8 * 1024 * 1024;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    const auto root = take(context.resource_budget());
    auto bindings = take(fixture.bindings(root));
    auto demand = take(context.open_demand(plan.plan, bindings));
    std::vector<std::shared_ptr<const ps::PreparedOperation>> preparations;
    for (const auto& step : plan.plan.steps()) {
      require(step.prepared != nullptr, "bake static preparation exists");
      preparations.push_back(step.prepared);
    }
    auto options = budgets();
    options.maximum_dependency_cache_work = 128 * 1024 * 1024;
    ps::DemandQuery query{{"values", take(ps::Footprint::all(fixture.shape()))},
                          {"axis", take(ps::Footprint::all({3}))}};
    if (kind >= 2)
      query.emplace("query_values", take(ps::Footprint::all({fixture.count})));
    const auto first = take(demand.request(query, {}, options));
    const auto repeated = take(demand.request(query, {}, options));
    for (const auto* key : {"values", "axis"})
      require(first.results.at(key).object_id() ==
                  repeated.results.at(key).object_id(),
              "repeated bake demand keeps completed Results");
    const auto fresh = take(fixture.bindings(root));
    const auto warm = take(context.execute_fragments(
        take(context.freeze(plan.plan, fresh)), query, {}, options));
    require(warm.diagnostics.cache_hits == (kind >= 2 ? 3 : 2),
            "fresh source Results reuse baked values and axis");
    for (const auto& item : warm.results) {
      const auto* key = item.first.c_str();
      require(
          rf::bytes(first.results.at(key)) == rf::bytes(warm.results.at(key)),
          "bake content cache retains bits");
      const auto association = warm.results.at(key).association();
      for (unsigned port = 0; port < fresh.inputs.size(); ++port) {
        const bool function_values = std::strcmp(key, "values") == 0;
        if ((!function_values && port >= 2) ||
            (function_values && kind >= 2 && port < 2))
          continue;
        require(std::find(association.begin(), association.end(),
                          fresh.inputs[port].result.object_id()) !=
                        association.end() &&
                    std::find(association.begin(), association.end(),
                              bindings.inputs[port].result.object_id()) ==
                        association.end(),
                "bake cached associations use current active sources");
      }
    }
    if (kind >= 2) {
      const auto association = warm.results.at("values").association();
      require(std::find(association.begin(), association.end(),
                        warm.results.at("query_values").object_id()) !=
                      association.end() &&
                  std::find(association.begin(), association.end(),
                            first.results.at("query_values").object_id()) ==
                      association.end(),
              "cached baked table associates the current intermediate query");
    }
    bindings.inputs[0].result = point_math_checks::source(
        root, fixture.inputs[1],
        fixture.document.inputs[0].result_schema.get());
    bindings.inputs[1].result = point_math_checks::source(
        root, fixture.inputs[0],
        fixture.document.inputs[1].result_schema.get());
    require(demand.replace_bindings(bindings).ok(), "replace bake endpoints");
    for (std::size_t i = 0; i < preparations.size(); ++i)
      require(plan.plan.steps()[i].prepared == preparations[i],
              "bake endpoint replacement reuses static preparation");
    auto reversed = take(demand.request(query, {}, options));
    for (unsigned i = 0; i < fixture.count; ++i)
      for (unsigned c = 0; c < fixture.columns; ++c)
        check_value(
            reversed.results.at("values"),
            kind >= 4 ? std::vector<std::uint64_t>{i, c}
                      : std::vector<std::uint64_t>{i},
            fixture.expected[(fixture.count - 1 - i) * fixture.columns + c]);
    double a = 0, b = 0;
    std::memcpy(&a, fixture.inputs[0].bytes().data(), 8);
    std::memcpy(&b, fixture.inputs[1].bytes().data(), 8);
    check_value(reversed.results.at("axis"), {0}, b);
    check_value(reversed.results.at("axis"), {1}, a);
    check_value(reversed.results.at("axis"), {2},
                (a - b) / (fixture.count - 1));
    Fixture limits(kind, profile);
    limits.build();
    auto limited = budgets();
    limited.dependencies.maximum_work = 1;
    auto bounded_query = query;
    bounded_query.erase("query_values");
    auto exhausted = limits.run(bounded_query, limited, 8 * 1024 * 1024, {}, 1);
    require(!exhausted.ok() &&
                exhausted.status().code == ps::ErrorCode::ResourceExhausted &&
                exhausted.status().reason == ps::FailureReason::WorkLimit,
            "bake source work failure");
    exhausted = limits.run(bounded_query, budgets(), 1);
    require(!exhausted.ok() &&
                exhausted.status().code == ps::ErrorCode::ResourceExhausted,
            "bake shared payload cap");
    ps::CancellationSource cancellation;
    cancellation.cancel();
    auto failed = limits.run(bounded_query, budgets(), 8 * 1024 * 1024,
                             cancellation.token());
    require(!failed.ok() && failed.status().code == ps::ErrorCode::Cancelled,
            "bake cancellation propagation");
    require(limits.run(bounded_query, budgets()).ok(),
            "bake recovery after bounded failures");
  }
  Fixture sparse(5, profile);
  sparse.count = 1048576;
  sparse.build();
  auto q = region(sparse.shape(), {ps::Region({{0, 1}, {1, 1}}),
                                   ps::Region({{1048575, 1}, {1, 1}})});
  auto result = sparse.run({{"values", q}}, budgets(), 1024 * 1024);
  require(!result.ok() &&
              result.status().code == ps::ErrorCode::ResourceExhausted &&
              result.status().reason == ps::FailureReason::CapacityLimit &&
              result.status().detail.node_id == 1,
          "million-row sparse bake requires complete query and table payload");
  std::cout << "six cached reversed bindings, work/payload/cancel recovery and "
               "full-output budget passed\n";
}

void layouts_and_active_cancellation(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 6; ++kind) {
    Fixture fixture(kind, profile);
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
      fixture.inputs[port] = std::move(view);
    }
    fixture.build();
    const auto original_document = fixture.document;
    for (const auto rounding :
         {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      fixture.document = original_document;
      fixture.registry = ps::make_default_operation_registry(false);
      auto control = std::make_shared<point_math_checks::Control>();
      control->rounding = rounding;
      for (auto& node : fixture.document.nodes)
        node = point_math_checks::checked_node(fixture.registry, node, control);
      require(fixture.registry->freeze().ok(), "freeze baking fenv adapters");
      fenv_t saved;
      require(fegetenv(&saved) == 0 && fesetround(rounding) == 0 &&
                  feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "set baking caller fenv");
      auto result = take(fixture.run(
          {{"values", take(ps::Footprint::all(fixture.shape()))}}, budgets()));
      require(fegetround() == rounding &&
                  fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO &&
                  control->computation_polls > 0,
              "baking caller and actual worker fenv preserved");
      require(fesetenv(&saved) == 0, "restore baking caller fenv");
      for (unsigned i = 0; i < fixture.count; ++i)
        for (unsigned c = 0; c < fixture.columns; ++c)
          check_value(result.results.at("values"),
                      kind >= 4 ? std::vector<std::uint64_t>{i, c}
                                : std::vector<std::uint64_t>{i},
                      fixture.expected[i * fixture.columns + c]);
    }
    for (const bool cancel : {false, true}) {
      Fixture active(kind, profile);
      active.count = 4097;
      active.build();
      auto control = std::make_shared<point_math_checks::Control>();
      if (cancel)
        control->cancel_after = 32;
      else
        control->maximum_work = 32;
      active.registry = ps::make_default_operation_registry(false);
      for (auto& node : active.document.nodes)
        node = point_math_checks::checked_node(active.registry, node, control);
      require(active.registry->freeze().ok(),
              "freeze baking cancellation adapters");
      ps::GraphContext graph(active.document);
      auto plan = take(ps::Compiler(active.registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.result_cache_bytes = 0;
      config.maximum_live_bytes = 8 * 1024 * 1024;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(active.registry, config);
      auto frozen = take(context.freeze(
          plan.plan, take(active.bindings(take(context.resource_budget())))));
      auto budget = take(context.resource_budget());
      auto cancelled = context.execute_fragments(
          frozen, {{"values", take(ps::Footprint::all(active.shape()))}},
          control->cancellation.token(), budgets());
      require(
          !cancelled.ok() && control->computation_polls > 0 &&
              (cancel ? cancelled.status().code == ps::ErrorCode::Cancelled &&
                            control->work >= 32
                      : cancelled.status().code ==
                                ps::ErrorCode::ResourceExhausted &&
                            cancelled.status().reason ==
                                ps::FailureReason::WorkLimit) &&
              budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "actual computation work/cancellation releases template "
          "intermediates");
    }
  }
  std::cout << "six templates all-port reversed/unaligned/scalar-zero Result "
               "sources, caller/worker fenv and computation work/cancellation "
               "passed\n";
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
  check_value(result.results.at("values"), {1}, .25);
  check_value(result.results.at("offset_values"), {1}, 1.5);
  check_value(result.results.at("offset_axis"), {2}, .5);
  Fixture single_column(4, profile);
  single_column.columns = 1;
  auto y = doubles({2, 1}, {0, 2});
  single_column.inputs[3] = y;
  single_column.document.inputs[3].result_schema =
      std::make_shared<ps::SchemaTemplate>(rf::source_schema(y));
  single_column.build();
  auto column = take(single_column.run(
      {{"values", take(ps::Footprint::all({3, 1}))}}, budgets()));
  require(column.results.at("values").schema().tensors[0].sample_shape() ==
              std::vector<std::uint64_t>{3, 1},
          "multi bake retains C1");
  check_value(column.results.at("values"), {1, 0}, 1);
  std::cout << "multiple reusable named exports and multi C1 shape passed\n";
}
void retained_outputs(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 6; ++kind)
    for (const auto dtype :
         {ps::ElementType::Float32, ps::ElementType::Float64}) {
      ps::ResourceBudget root;
      ps::ResultRef values, axis;
      ps::ResultTensorReadWindow values_window, axis_window;
      std::vector<std::weak_ptr<const ps::CpuStorage>> input_owners;
      std::vector<std::uint64_t> shape;
      std::uint64_t payload = 0;
      double first = 0;
      {
        Fixture fixture(kind, profile, dtype);
        fixture.build();
        shape = fixture.shape();
        first = fixture.expected[0];
        payload =
            fixture.count * fixture.columns * ps::Value::element_size(dtype);
        for (const auto& input : fixture.inputs)
          input_owners.push_back(input.storage());
        ps::GraphContext graph(fixture.document);
        auto plan = take(ps::Compiler(fixture.registry).compile(graph));
        ps::ExecutionContextConfig config;
        config.cpu_workers = 1;
        config.result_cache_bytes = 0;
        config.maximum_live_bytes = 8 * 1024 * 1024;
        config.managed_resources = ps::ResourceLimits{};
        ps::ExecutionContext context(fixture.registry, config);
        root = take(context.resource_budget());
        auto frozen =
            take(context.freeze(plan.plan, take(fixture.bindings(root))));
        auto result = take(context.execute_fragments(
            frozen,
            {{"values", take(ps::Footprint::all(shape))},
             {"axis", take(ps::Footprint::all({3}))}},
            {}, budgets()));
        values = result.results.at("values");
        axis = result.results.at("axis");
        values_window = take(values.acquire_tensor(take(values.descriptor()), 0,
                                                   ps::Region::whole(shape)));
        axis_window = take(axis.acquire_tensor(take(axis.descriptor()), 0,
                                               ps::Region::whole({3})));
      }
      for (const auto& input : input_owners)
        require(input.expired(), "baked outputs retire every source backing");
      require(root.statistics().live[ps::ResourceKind::Payload] == payload + 24,
              "baked Result/window pairs share independent packed owners");
      const std::vector<std::uint64_t> at(shape.size(), 0);
      check_value(values, at, first);
      check_value(axis, {0}, kind == 5 ? .5 : 0);
      values = {};
      axis = {};
      std::uint64_t bits = 0;
      const auto width = ps::Value::element_size(dtype);
      std::memcpy(&bits, take(values_window.row_run(at)).data, width);
      auto want = raw(first);
      if (width == 4) {
        const float f = static_cast<float>(first);
        want = 0;
        std::memcpy(&want, &f, 4);
      }
      require(bits == want, "baked values window outlives Result");
      std::memcpy(&bits, take(axis_window.row_run({0})).data, 8);
      require(bits == raw(kind == 5 ? .5 : 0),
              "baked axis window outlives Result");
      values_window = {};
      require(root.statistics().live[ps::ResourceKind::Payload] == 24,
              "releasing baked values preserves independent axis owner");
      axis_window = {};
      point_math_checks::released(root);
    }
  std::cout << "six templates Float32/64 source retirement, independent Result/"
               "window owners and final all-Root release passed\n";
}
void authoring_boundaries() {
  using ps::numeric::SequenceInput;
  const auto scalar = std::make_shared<ps::SchemaTemplate>(
      rf::source_schema(doubles({1}, {0})));
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
      badstart.result_schema = std::make_shared<ps::SchemaTemplate>(
          rf::source_schema(doubles({2}, {0, 1})));
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
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile must be strict/apple/x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    graph_equivalence(profile);
    source_semantics(profile);
    dynamic_and_limits(profile);
    layouts_and_active_cancellation(profile);
    reusable_exports(profile);
    retained_outputs(profile);
    authoring_boundaries();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
