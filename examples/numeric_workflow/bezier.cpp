#include "photospider/numeric/bezier.hpp"

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
    document.outputs = {{"values", node.id, "values"},
                        {"axis", node.id, "axis"}};
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
    config.maximum_live_bytes = 8 * 1024 * 1024;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(64) * 1024 * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(32) * 1024 * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, {}, options);
  }
};
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
ps::WorkflowNode node(
    unsigned degree, unsigned count, ps::CpuNumericProfile profile,
    ps::ElementType dtype = ps::ElementType::Float64,
    ps::numeric::BezierDomain domain = ps::numeric::BezierDomain::Reject) {
  return take(ps::numeric::sample_bezier_function_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4}, degree,
      count, dtype, domain, profile));
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
void value(const ps::DemandResult& result, const char* key, std::uint64_t at,
           std::uint64_t expected, unsigned width = 8) {
  std::uint64_t bits = 0;
  require(
      result.values.at(key).read({at}, &bits, width).ok() && bits == expected,
      "Bezier expected bits");
}
void examples(ps::CpuNumericProfile profile) {
  Fixture cubic(node(3, 9, profile), {doubles({2, 2}, {0, 0, 1, 1}),
                                      doubles({1, 2, 2}, {0, .25, -1, -.25}),
                                      doubles({1}, {0}), doubles({1}, {1})});
  auto demand = region({9}, {ps::Region({{0, 2}}), ps::Region({{8, 1}})});
  auto result = take(cubic.run(
      {{"values", demand}, {"axis", take(ps::Footprint::all({3}))}}, false));
  value(result, "values", 0, 0);
  value(result, "values", 1, raw(.5));
  value(result, "values", 8, raw(1));
  value(result, "axis", 0, 0);
  value(result, "axis", 1, raw(1));
  value(result, "axis", 2, raw(.125));
  std::uint64_t root_calls = 0;
  for (const auto& timing : result.diagnostics.operation_timings)
    root_calls += timing.numeric.strict_math_calls;
  require(root_calls == 1, "one exact dyadic root sign evaluation");
  Fixture quadratic(node(2, 5, profile),
                    {doubles({2, 2}, {0, 0, 1, 0}), doubles({1, 1, 2}, {0, 1}),
                     doubles({1}, {0}), doubles({1}, {1})});
  auto q = take(quadratic.run(
      {{"values", region({5}, {ps::Region({{0, 2}}), ps::Region({{4, 1}})})}},
      false));
  value(q, "values", 0, 0);
  value(q, "values", 1, raw(.5));
  value(q, "values", 4, 0);
  for (unsigned degree : {2, 3}) {
    Fixture identity(
        node(degree, 3, profile),
        {doubles({2, 2}, {0, 0, 1, 1}),
         doubles({1, degree - 1, 2},
                 degree == 2 ? std::vector<double>{.5, .5}
                             : std::vector<double>{.25, .25, -.25, -.25}),
         doubles({1}, {1}), doubles({1}, {0})});
    auto run = take(identity.run({{"values", take(ps::Footprint::all({3}))},
                                  {"axis", take(ps::Footprint::all({3}))}},
                                 false));
    value(run, "values", 0, raw(1));
    value(run, "values", 1, raw(.5));
    value(run, "values", 2, 0);
    value(run, "axis", 2, raw(-.5));
  }
  for (int direction : {-1, 1}) {
    Fixture overshoot(node(2, 3, profile),
                      {doubles({2, 2}, {0, 0, 1, 0}),
                       doubles({1, 1, 2}, {.5, 4.0 * direction}),
                       doubles({1}, {0}), doubles({1}, {1})});
    auto run =
        take(overshoot.run({{"values", take(ps::Footprint::all({3}))}}, false));
    value(run, "values", 1, raw(2.0 * direction));
  }
  for (auto type : {ps::ElementType::Float32, ps::ElementType::Float64}) {
    const auto next =
        raw(1) + (type == ps::ElementType::Float32 ? (UINT64_C(1) << 29) : 1);
    Fixture midpoint(
        node(2, 1, profile, type),
        {array(ps::ElementType::Float64, {2, 2}, {0, raw(1), raw(1), next}),
         doubles({1, 1, 2}, {0, 0}), doubles({1}, {.5}),
         array(ps::ElementType::Float64, {1}, {0x7ff0000000000042})});
    auto run = take(midpoint.run({{"values", take(ps::Footprint::all({1}))},
                                  {"axis", take(ps::Footprint::all({3}))}},
                                 false));
    value(run, "values", 0,
          type == ps::ElementType::Float32 ? 0x3f800000 : raw(1),
          type == ps::ElementType::Float32 ? 4 : 8);
    value(run, "axis", 0, raw(.5));
    value(run, "axis", 1, raw(.5));
    value(run, "axis", 2, 0);
  }
  for (int sign : {-1, 1}) {
    const auto sub = [&](int units) {
      auto v = units * sign;
      return static_cast<std::uint64_t>(v < 0 ? -v : v) |
             (v < 0 ? (UINT64_C(1) << 63) : 0);
    };
    Fixture tie(node(3, 1, profile),
                {array(ps::ElementType::Float64, {2, 2},
                       {0, sub(-17), raw(45), sub(742)}),
                 array(ps::ElementType::Float64, {1, 2, 2},
                       {raw(4), sub(66), raw(-38), sub(-644)}),
                 doubles({1}, {1}), doubles({1}, {0})});
    auto run =
        take(tie.run({{"values", take(ps::Footprint::all({1}))}}, false));
    value(run, "values", 0, sign > 0 ? (UINT64_C(1) << 63) : 0);
  }
  std::cout << "public quadratic/cubic inverse, reverse identity, overshoot, "
               "non-dyadic midpoint and signed half-subnormal passed\n";
}
void topology_and_support(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  const auto nan = UINT64_C(0x7ff0000000000042);
  Fixture local(node(2, 5, profile), {doubles({3, 2}, {0, 0, .5, 1, 1, 0}),
                                      array(Type::Float64, {2, 1, 2},
                                            {raw(.25), raw(.5), raw(.25), nan}),
                                      doubles({1}, {0}), doubles({1}, {1})});
  auto demand = region({5}, {ps::Region({{1, 1}})});
  auto result = take(local.run(
      {{"values", demand}, {"axis", take(ps::Footprint::all({3}))}}, false));
  value(result, "values", 1, raw(.5));
  auto support = take(result.dependencies.source_support());
  require(
      support.at("input0") == region({3, 2}, {ps::Region({{0, 3}, {0, 1}}),
                                              ps::Region({{0, 2}, {1, 1}})}) &&
          support.at("input1") ==
              region({2, 1, 2}, {ps::Region({{0, 2}, {0, 1}, {0, 1}}),
                                 ps::Region({{0, 1}, {0, 1}, {1, 1}})}),
      "Bezier full X and selected segment Y support");
  auto remote = take(result.dependencies.potential_dirty(
      "input1", region({2, 1, 2}, {ps::Region({{1, 1}, {0, 1}, {1, 1}})})));
  require((!remote.count("values") || remote.at("values").empty()) &&
              (!remote.count("axis") || remote.at("axis").empty()),
          "unused y no dirty");
  auto changed = take(result.dependencies.potential_dirty(
      "input1", region({2, 1, 2}, {ps::Region({{1, 1}, {0, 1}, {0, 1}})})));
  require(changed.at("values") == demand &&
              (!changed.count("axis") || changed.at("axis").empty()),
          "remote X dirties values only");
  auto knot =
      take(local.run({{"values", region({5}, {ps::Region({{2, 1}})})}}, false));
  value(knot, "values", 2, raw(1));
  require(take(knot.dependencies.source_support()).at("input1") ==
              region({2, 1, 2}, {ps::Region({{0, 2}, {0, 1}, {0, 1}})}),
          "C0 anchor ignores handles Y");
  auto selected_bad =
      local.run({{"values", region({5}, {ps::Region({{3, 1}})})}}, false);
  require(!selected_bad.ok() &&
              selected_bad.status().detail.atom->coordinate[0] == 3 &&
              selected_bad.status().message.find("port=1") != std::string::npos,
          "selected invalid Y Atom");
  local.bindings.inputs[1].value =
      array(Type::Float64, {2, 1, 2}, {raw(.25), raw(.5), raw(-.25), nan});
  auto badx =
      local.run({{"values", region({5}, {ps::Region({{0, 1}})})}}, false);
  require(!badx.ok() && badx.status().message.find(
                            "backward Bezier segment=1") != std::string::npos,
          "remote backward X rejects anchor request");
  require(local.run({{"axis", take(ps::Footprint::all({3}))}}, false).ok(),
          "axis ignores invalid curve");
  for (const auto& middle :
       {std::array<double, 2>{.75, .25}, std::array<double, 2>{1, 0},
        std::array<double, 2>{2, -1}}) {
    Fixture shape(node(3, 1, profile),
                  {doubles({2, 2}, {0, 0, 1, 1}),
                   doubles({1, 2, 2}, {middle[0], 0, middle[1] - 1, -1}),
                   doubles({1}, {.5}), doubles({1}, {0})});
    auto answer = shape.run({{"values", take(ps::Footprint::all({1}))}}, false);
    if (middle[0] == 2) {
      require(!answer.ok() &&
                  answer.status().reason == ps::FailureReason::InvalidDomain,
              "cubic fold");
    } else {
      require(answer.ok(), "crossing/stationary cubic accepted");
      value(answer.value(), "values", 0, raw(.125));
    }
  }
  for (auto policy :
       {ps::numeric::BezierDomain::Reject, ps::numeric::BezierDomain::Clamp}) {
    Fixture outside(node(2, 2, profile, Type::Float64, policy),
                    {doubles({2, 2}, {0, -0.0, 1, 2}),
                     array(Type::Float64, {1, 1, 2}, {raw(.5), nan}),
                     doubles({1}, {-1}), doubles({1}, {2})});
    auto answer = outside.run({{"values", take(ps::Footprint::all({2}))},
                               {"axis", take(ps::Footprint::all({3}))}},
                              false);
    if (policy == ps::numeric::BezierDomain::Reject) {
      require(!answer.ok() && answer.status().message.find(
                                  "outside anchor domain") != std::string::npos,
              "reject before NaN Y handle");
    } else {
      require(answer.ok(), "clamp skips Y handle");
      value(answer.value(), "values", 0, UINT64_C(1) << 63);
      value(answer.value(), "values", 1, raw(2));
      value(answer.value(), "axis", 0, raw(-1));
      value(answer.value(), "axis", 1, raw(2));
    }
  }
  Fixture collapsed(
      node(2, 3, profile),
      {array(Type::Float64, {2, 2}, {raw(1), 0, raw(1) + 1, raw(1)}),
       doubles({1, 1, 2}, {0, 0}), doubles({1}, {1}),
       array(Type::Float64, {1}, {raw(1) + 1})});
  require(collapsed.run({{"axis", take(ps::Footprint::all({3}))}}, false).ok(),
          "axis no coordinate scan");
  auto bad =
      collapsed.run({{"values", region({3}, {ps::Region({{0, 1}})})}}, false);
  require(
      !bad.ok() && bad.status().reason == ps::FailureReason::ArithmeticOverflow,
      "local coordinate collapse");
  auto good = take(
      collapsed.run({{"values", region({3}, {ps::Region({{2, 1}})})}}, false));
  value(good, "values", 2, raw(1));
  std::cout
      << "global X/local Y support and dirty, C0/clamp/domain, "
         "crossing/stationary/fold and local coordinate collapse passed\n";
}
void producers_and_schema(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry(false);
  std::array<unsigned, 2> calls{};
  for (unsigned kind = 0; kind < 2; ++kind) {
    ps::OperationDefinition source;
    source.key = kind ? "manual.bezier_end" : "manual.bezier_anchors";
    source.traits.input_count = 0;
    source.traits.input_schema.clear();
    source.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
    source.traits.outputs[0].fixed_output_shape =
        kind ? std::vector<std::uint64_t>{1} : std::vector<std::uint64_t>{2, 2};
    source.traits.outputs[0].output_element_type = Type::Float64;
    source.callback = [&, kind](const auto&) {
      ++calls[kind];
      return ps::Result<ps::Value>(ps::Status{
          ps::ErrorCode::OperationFailed,
          kind ? "required end producer" : "required anchor producer"});
    };
    require(registry->register_operation(std::move(source)).ok(),
            "Bezier failure producer");
  }
  require(registry->freeze().ok(), "Bezier producer freeze");
  Fixture both(node(2, 1, profile),
               {doubles({2, 2}, {0, 0, 1, 1}), doubles({1, 1, 2}, {.5, .5}),
                doubles({1}, {.5}), doubles({1}, {0})});
  both.registry = registry;
  both.document.inputs.erase(both.document.inputs.begin() + 3);
  both.bindings.inputs.erase(both.bindings.inputs.begin() + 3);
  both.document.inputs.erase(both.document.inputs.begin());
  both.bindings.inputs.erase(both.bindings.inputs.begin());
  both.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "value"};
  both.document.nodes[0].inputs[3] = ps::WorkflowNodeOutput{3, "value"};
  both.document.nodes.push_back({2, "manual.bezier_anchors", {}, {}});
  both.document.nodes.push_back({3, "manual.bezier_end", {}, {}});
  auto axis = take(both.run({{"axis", take(ps::Footprint::all({3}))}}, false));
  value(axis, "axis", 0, raw(.5));
  require(calls == std::array<unsigned, 2>{0, 0},
          "axis/N1 skips failing controls/end");
  auto values = both.run({{"values", take(ps::Footprint::all({1}))}}, false);
  require(!values.ok() &&
              values.status().message == "required anchor producer" &&
              calls == std::array<unsigned, 2>{1, 0},
          "values N1 still validates controls");
  Fixture end_only(node(2, 1, profile),
                   {doubles({2, 2}, {0, 0, 1, 1}), doubles({1, 1, 2}, {.5, .5}),
                    doubles({1}, {.5}), doubles({1}, {0})});
  end_only.registry = registry;
  end_only.document.inputs.pop_back();
  end_only.bindings.inputs.pop_back();
  end_only.document.nodes[0].inputs[3] = ps::WorkflowNodeOutput{3, "value"};
  end_only.document.nodes.push_back({3, "manual.bezier_end", {}, {}});
  auto one =
      take(end_only.run({{"values", take(ps::Footprint::all({1}))}}, false));
  value(one, "values", 0, raw(.5));
  require(calls[1] == 0, "N1 values skip failing end");
  end_only.document.nodes[0].parameters["count"] = std::int64_t{2};
  auto required =
      end_only.run({{"axis", take(ps::Footprint::all({3}))}}, false);
  require(!required.ok() &&
              required.status().message == "required end producer" &&
              calls[1] == 1,
          "N2 end required");
  auto builtins = ps::make_default_operation_registry();
  auto authored = node(3, 9, profile);
  ps::DependencyRequest request;
  request.inputs = {{{Type::Float64, {2, 2}}, {}},
                    {{Type::Float32, {1, 2, 2}}, {}},
                    {{Type::Float32, {1}}, {}},
                    {{Type::Float64, {1}}, {}}};
  request.parameters = authored.parameters;
  request.output_index = 1;
  request.outputs = take(ps::Footprint::none({3}));
  request.snapshot_identity = "Bezier-schema";
  auto empty = take(builtins->start_dependency(authored.operation, request));
  require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
              empty->poll_count() == 0,
          "Empty Bezier no payload");
  for (unsigned kind = 0; kind < 8; ++kind) {
    auto bad = request;
    if (kind == 0)
      bad.parameters.erase("count");
    if (kind == 1)
      bad.parameters["degree"] = std::int64_t{4};
    if (kind == 2)
      bad.parameters["count"] = std::int64_t{1048577};
    if (kind == 3)
      bad.parameters["out_of_domain"] = std::string("linear_extrapolate");
    if (kind == 4)
      bad.inputs[1].descriptor.shape = {1, 1, 2};
    if (kind == 5)
      bad.inputs[0].descriptor.shape = {65537, 2};
    if (kind == 6)
      bad.inputs[2].descriptor.element_type = Type::Int64;
    if (kind == 7)
      bad.inputs[3].descriptor.shape = {2};
    auto invalid = builtins->start_dependency(authored.operation, bad);
    require(!invalid.ok() &&
                (invalid.status().code == ps::ErrorCode::TypeMismatch ||
                 invalid.status().code == ps::ErrorCode::InvalidArgument),
            "Bezier static validation even axis Empty");
  }
  std::cout << "axis/Empty/failing controls and N1/N2 end isolation, static "
               "degree/arity/count/type bounds passed\n";
}

void supply(const std::shared_ptr<ps::DependencySession>& session,
            const ps::DependencyRequest& request,
            const std::vector<ps::Value>& inputs) {
  std::vector<ps::Footprint> wanted;
  for (const auto& input : inputs)
    wanted.push_back(take(ps::Footprint::none(input.descriptor().shape)));
  for (const auto& need : take(session->pending_reads()))
    wanted[need.port] = take(wanted[need.port].unite(need.samples));
  std::vector<ps::ValueFragments> ready;
  for (unsigned p = 0; p < inputs.size(); ++p) {
    auto all = take(ps::ValueFragments::create(
        inputs[p].descriptor(), inputs[p].facets(),
        take(ps::Footprint::all(inputs[p].descriptor().shape)), {inputs[p]}));
    ready.push_back(take(all.restrict(wanted[p])));
  }
  require(session->supply(ready, request.snapshot_identity).ok(),
          "Bezier exact stage supply");
}
ps::Value reversed_unaligned(const ps::Value& value) {
  const auto bytes = value.bytes();
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = bytes.size() / width;
  auto buffer = take(ps::BufferAllocator{}.allocate(bytes.size() + 1));
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(buffer.data() + 1 + (count - i - 1) * width,
                bytes.data() + i * width, width);
  std::vector<std::int64_t> strides(value.descriptor().shape.size());
  std::int64_t stride = -static_cast<std::int64_t>(width);
  for (auto i = strides.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= value.descriptor().shape[i - 1];
  }
  return take(ps::Value::from_storage(value.descriptor(), value.region(),
                                      {1 + (count - 1) * width, strides},
                                      std::move(buffer).freeze()));
}
void layouts_and_resources(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  auto registry = ps::make_default_operation_registry();
  auto authored = node(3, 9, profile);
  std::vector<ps::Value> dense{doubles({2, 2}, {0, 0, 1, 1}),
                               doubles({1, 2, 2}, {0, .25, -1, -.25}),
                               doubles({1}, {0}), doubles({1}, {1})};
  ps::DependencyRequest request;
  for (const auto& input : dense)
    request.inputs.push_back({input.descriptor(), {}});
  request.parameters = authored.parameters;
  request.outputs = region({9}, {ps::Region({{1, 1}})});
  request.snapshot_identity = "Bezier-direct";
  request.limits.maximum_work = UINT64_C(8) * 1024 * 1024 * 1024;
  for (unsigned mask = 0; mask < 16; ++mask) {
    auto inputs = dense;
    for (unsigned p = 0; p < 4; ++p)
      if (mask & (1U << p))
        inputs[p] = reversed_unaligned(inputs[p]);
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save Bezier fenv");
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "set Bezier fenv");
      ps::ResourceBudget budget(ps::ResourceLimits{});
      auto session = take(registry->start_dependency(
          authored.operation, request, budget.allocator()));
      for (unsigned stage = 0; stage < 3; ++stage) {
        require(session->poll().ok(), "strided Bezier Need");
        supply(session, request, inputs);
      }
      auto result = take(session->poll());
      require(std::holds_alternative<ps::DependencyResult>(result),
              "strided result");
      std::uint64_t bits = 0;
      require(std::get<ps::DependencyResult>(result)
                      .value.read({1}, &bits, 8)
                      .ok() &&
                  bits == raw(.5),
              "strided Bezier bits");
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "Bezier preserves fenv");
    }
    require(fesetenv(&saved) == 0, "restore Bezier fenv");
  }
  for (unsigned mode = 0; mode < 3; ++mode) {
    ps::ResourceBudget budget(ps::ResourceLimits{});
    ps::CancellationSource cancel;
    request.cancellation = cancel.token();
    request.outputs =
        mode == 2 ? region({9}, {ps::Region({{1, 1}}), ps::Region({{3, 1}})})
                  : region({9}, {ps::Region({{1, 1}})});
    bool interrupted = false;
    std::shared_ptr<ps::DependencySession> session;
    session = take(registry->start_dependency(
        authored.operation, request, budget.allocator(),
        [&](std::uint64_t amount) {
          if (amount == 640 &&
              session->numeric_diagnostics().evaluated_values ==
                  (mode == 2 ? 2U : 1U)) {
            interrupted = true;
            if (mode)
              cancel.cancel();
            else
              return ps::Status{ps::ErrorCode::ResourceExhausted,
                                "Bezier inverse work",
                                ps::FailureReason::WorkLimit};
          }
          return ps::Status::success();
        }));
    for (unsigned stage = 0; stage < 3; ++stage) {
      require(session->poll().ok(), "Bezier Need before failure");
      supply(session, request, dense);
    }
    auto failed = session->poll();
    require(
        interrupted && !failed.ok() &&
            failed.status().code == (mode ? ps::ErrorCode::Cancelled
                                          : ps::ErrorCode::ResourceExhausted),
        "Bezier inner inverse interrupt");
    require(session->numeric_diagnostics().evaluated_values ==
                    (mode == 2 ? 2U : 1U) &&
                session->numeric_diagnostics().copied_elements ==
                    (mode == 2 ? 1U : 0U),
            "Bezier failed counters");
    session.reset();
    require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "Bezier unpublished owner release");
  }
  request.cancellation = {};
  request.outputs = region({9}, {ps::Region({{1, 1}})});
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto limited = request;
    if (mode == 0)
      limited.limits.maximum_state_bytes = 1024;
    if (mode == 1)
      limited.limits.maximum_work = 1;
    if (mode == 2)
      limited.limits.maximum_stages = 1;
    auto started = registry->start_dependency(authored.operation, limited);
    bool failed = !started.ok();
    if (started.ok()) {
      for (unsigned phase = 0; phase < 4; ++phase) {
        auto progress = started.value()->poll();
        if (!progress.ok()) {
          require(progress.status().code == ps::ErrorCode::ResourceExhausted,
                  "Bezier resource category");
          failed = true;
          break;
        }
        if (std::holds_alternative<ps::DependencyResult>(progress.value()))
          break;
        supply(started.value(), limited, dense);
      }
    }
    require(failed, "Bezier state/work/stage admission");
  }
  Fixture large(node(3, 65536, profile), dense);
  ps::GraphContext graph(large.document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 8 * 1024 * 1024;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(registry, config);
  auto frozen = take(context.freeze(plan.plan, large.bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(16) * 1024 * 1024 * 1024;
  options.dependencies.maximum_work = UINT64_C(8) * 1024 * 1024 * 1024;
  options.dependencies.sets.maximum_boxes = 1024;
  ps::DemandQuery roi{{"values", region({65536}, {ps::Region({{1, 1}})})}};
  require(context.execute_fragments(frozen, roi, {}, options).ok(),
          "Bezier small ROI fits");
  auto full = context.execute_fragments(
      frozen, {{"values", take(ps::Footprint::all({65536}))}}, {}, options);
  require(!full.ok() && full.status().code == ps::ErrorCode::ResourceExhausted,
          "Bezier full association budget");
  require(context.execute_fragments(frozen, roi, {}, options).ok(),
          "Bezier same-context ROI recovery");
  std::cout << "all-port signed/unaligned strides/fenv, inverse work/cancel "
               "and second-box release, state/stage and association ROI "
               "recovery passed\n";
}

void cache_atoms_and_typed(ps::CpuNumericProfile profile) {
  using Type = ps::ElementType;
  Fixture fixture(node(2, 5, profile), {doubles({3, 2}, {0, 0, .5, 1, 1, 0}),
                                        doubles({2, 1, 2}, {.25, .5, .25, -.5}),
                                        doubles({1}, {0}), doubles({1}, {1})});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::InputSnapshotStore snapshots;
  for (auto& binding : fixture.bindings.inputs) {
    binding.snapshot = std::make_shared<const ps::InputSnapshot>(
        take(snapshots.import_value(binding.value)));
    binding.value = {};
  }
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 4 * 1024 * 1024;
  config.maximum_live_bytes = 8 * 1024 * 1024;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  auto demand = take(context.open_demand(plan.plan, fixture.bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(16) * 1024 * 1024 * 1024;
  options.dependencies.maximum_work = UINT64_C(8) * 1024 * 1024 * 1024;
  options.maximum_dependency_cache_work = 128 * 1024 * 1024;
  ps::DemandQuery query{{"values", take(ps::Footprint::all({5}))},
                        {"axis", take(ps::Footprint::all({3}))}};
  auto first = take(demand.request(query, {}, options));
  value(first, "values", 1, raw(.5));
  require(take(demand.request(query, {}, options)).diagnostics.cache_hits > 0,
          "Bezier warm cache");
  fixture.bindings.inputs[1].snapshot =
      std::make_shared<const ps::InputSnapshot>(take(
          snapshots.import_value(doubles({2, 1, 2}, {.25, 1.5, .25, -.5}))));
  require(demand.replace_bindings(fixture.bindings).ok(), "Bezier handle edit");
  auto changed = take(demand.request(query, {}, options));
  value(changed, "values", 1, raw(1));
  value(changed, "values", 2, raw(1));
  value(changed, "values", 3, raw(.5));
  value(changed, "axis", 2, raw(.25));
  fixture.bindings.inputs[0].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(doubles({3, 2}, {0, 0, .2, 1, 1, 0}))));
  fixture.bindings.inputs[1].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(doubles({2, 1, 2}, {.1, .5, .4, -.5}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "Bezier topology edit");
  auto moved = take(demand.request(
      {{"values", region({5}, {ps::Region({{1, 1}})})}}, {}, options));
  require(take(moved.dependencies.source_support()).at("input1") ==
              region({2, 1, 2}, {ps::Region({{0, 2}, {0, 1}, {0, 1}}),
                                 ps::Region({{1, 1}, {0, 1}, {1, 1}})}),
          "topology edit changes selected segment Y");
  Fixture fresh(node(2, 5, profile), {doubles({3, 2}, {0, 0, .2, 1, 1, 0}),
                                      doubles({2, 1, 2}, {.1, .5, .4, -.5}),
                                      doubles({1}, {0}), doubles({1}, {1})});
  auto expected =
      take(fresh.run({{"values", region({5}, {ps::Region({{1, 1}})})}}, false));
  std::uint64_t bits = 0;
  require(expected.values.at("values").read({1}, &bits, 8).ok(),
          "fresh result");
  value(moved, "values", 1, bits);
  const auto nan = UINT64_C(0x7ff0000000000042);
  Fixture isolated(
      node(2, 5, profile),
      {doubles({3, 2}, {0, 0, .5, 1, 1, 0}),
       array(Type::Float64, {2, 1, 2}, {raw(.25), raw(.5), raw(.25), nan}),
       doubles({1}, {0}), doubles({1}, {1})});
  ps::GraphContext isolated_graph(isolated.document);
  auto isolated_plan =
      take(ps::Compiler(isolated.registry).compile(isolated_graph));
  ps::ExecutionContext isolated_context(isolated.registry, config);
  for (bool joint : {false, true}) {
    options.enable_joint = joint;
    auto outcomes = take(isolated_context.execute_atoms(
        isolated_plan.plan, isolated.bindings,
        {{"values", region({5}, {ps::Region({{1, 1}}), ps::Region({{3, 1}})})},
         {"axis", take(ps::Footprint::all({3}))}},
        {}, options));
    unsigned good = 0, bad = 0;
    for (const auto& atom : outcomes.atoms) {
      if (atom.outcome.ok()) {
        ++good;
      } else {
        ++bad;
        require(atom.key.output_index == 0 && atom.key.coordinate[0] == 3 &&
                    atom.outcome.status().detail.atom == atom.key,
                "Bezier failed Atom identity");
      }
    }
    require(good == 2 && bad == 1,
            "axis and first segment survive second segment Y failure");
  }
  auto registry = ps::make_default_operation_registry();
  auto authored = node(2, 2, profile);
  auto anchors = array(Type::Float32, {2, 2}, {0, 0, 0x3f800000, 0x3fc00000});
  const auto facet = take(ps::encode_semantic(ps::coverage_semantics()));
  anchors = take(ps::Value::from_storage(anchors.descriptor(), anchors.region(),
                                         anchors.layout(), anchors.storage(),
                                         {facet}));
  std::vector<ps::Value> inputs{anchors, doubles({1, 1, 2}, {.5, 0}),
                                doubles({1}, {0}), doubles({1}, {1})};
  ps::DependencyRequest request;
  for (const auto& input : inputs)
    request.inputs.push_back({input.descriptor(), input.facets()});
  request.parameters = authored.parameters;
  request.outputs = region({2}, {ps::Region({{1, 1}})});
  request.snapshot_identity = "Bezier-typed";
  auto session = take(registry->start_dependency(authored.operation, request));
  for (unsigned stage = 0; stage < 2; ++stage) {
    require(session->poll().ok(), "Bezier typed control Need");
    supply(session, request, inputs);
  }
  require(session->poll().ok(), "Bezier typed y Need");
  std::vector<ps::Footprint> wanted;
  for (const auto& input : inputs)
    wanted.push_back(take(ps::Footprint::none(input.descriptor().shape)));
  for (const auto& need : take(session->pending_reads()))
    wanted[need.port] = take(wanted[need.port].unite(need.samples));
  require(wanted[0] == region({2, 2}, {ps::Region({{1, 1}, {1, 1}})}) &&
              wanted[1].empty(),
          "typed knot only selected Y");
  std::vector<ps::ValueFragments> ready;
  for (unsigned p = 0; p < inputs.size(); ++p) {
    auto all = take(ps::ValueFragments::create(
        inputs[p].descriptor(), inputs[p].facets(),
        take(ps::Footprint::all(inputs[p].descriptor().shape)), {inputs[p]}));
    ready.push_back(take(all.restrict(wanted[p])));
  }
  require(!session->supply(ready, request.snapshot_identity).ok() &&
              session->numeric_diagnostics().evaluated_values == 0,
          "Bezier typed coverage failure precedes arithmetic");
  auto anchor_storage = doubles({2}, {0, 1});
  auto handle_storage = doubles({2}, {0, -1});
  inputs = {take(ps::Value::from_storage({Type::Float64, {2, 2}},
                                         ps::Region::whole({2, 2}), {0, {8, 0}},
                                         anchor_storage.storage())),
            take(ps::Value::from_storage(
                {Type::Float64, {1, 2, 2}}, ps::Region::whole({1, 2, 2}),
                {0, {16, 8, 0}}, handle_storage.storage())),
            doubles({1}, {0}), doubles({1}, {1})};
  authored = node(3, 9, profile);
  request.inputs.clear();
  for (const auto& input : inputs)
    request.inputs.push_back({input.descriptor(), {}});
  request.parameters = authored.parameters;
  request.outputs = region({9}, {ps::Region({{1, 1}})});
  request.limits.maximum_work = UINT64_C(8) * 1024 * 1024 * 1024;
  session = take(registry->start_dependency(authored.operation, request));
  for (unsigned stage = 0; stage < 3; ++stage) {
    require(session->poll().ok(), "zero-stride Need");
    supply(session, request, inputs);
  }
  auto zero_stride = take(session->poll());
  bits = 0;
  require(std::get<ps::DependencyResult>(zero_stride)
                  .value.read({1}, &bits, 8)
                  .ok() &&
              bits == raw(.125),
          "zero-stride controls preserve y=x");
  std::cout << "dynamic handles/topology cache, joint on/off Atom isolation, "
               "typed Mask and zero-stride controls passed\n";
}

void oracle(ps::CpuNumericProfile profile) {
  unsigned degree = 0, count = 0, k = 0, dtype = 0, policy = 0, requested = 0;
  while (std::cin >> degree >> count >> k >> dtype >> policy >> requested) {
    std::vector<std::uint64_t> indices(requested);
    for (auto& i : indices)
      std::cin >> i;
    std::vector<ps::Value> inputs;
    for (unsigned port = 0; port < 4; ++port) {
      unsigned type = 0;
      std::cin >> type;
      const auto size = port == 0   ? k * 2
                        : port == 1 ? (k - 1) * (degree - 1) * 2
                                    : 1;
      std::vector<std::uint64_t> bits(size);
      for (auto& b : bits)
        std::cin >> std::hex >> b >> std::dec;
      const auto shape = port == 0 ? std::vector<std::uint64_t>{k, 2}
                         : port == 1
                             ? std::vector<std::uint64_t>{k - 1, degree - 1, 2}
                             : std::vector<std::uint64_t>{1};
      inputs.push_back(array(static_cast<ps::ElementType>(type), shape, bits));
    }
    Fixture fixture(
        node(degree, count, profile, static_cast<ps::ElementType>(dtype),
             static_cast<ps::numeric::BezierDomain>(policy)),
        inputs);
    ps::DemandQuery query{{"axis", take(ps::Footprint::all({3}))}};
    if (requested) {
      std::vector<ps::Region> boxes;
      for (auto i : indices)
        boxes.emplace_back(std::vector<ps::RegionDimension>{{i, 1}});
      query.emplace("values", region({count}, std::move(boxes)));
    }
    auto result = fixture.run(query, false);
    if (!result.ok()) {
      std::cout << (result.status().reason ==
                            ps::FailureReason::ArithmeticOverflow
                        ? "overflow"
                    : result.status().reason == ps::FailureReason::InvalidDomain
                        ? "domain"
                        : "other")
                << '\n';
      if (result.status().reason != ps::FailureReason::ArithmeticOverflow &&
          result.status().reason != ps::FailureReason::InvalidDomain)
        std::cerr << result.status().message << '\n';
      continue;
    }
    for (auto i : indices) {
      std::uint64_t bits = 0;
      require(result.value()
                  .values.at("values")
                  .read({i}, &bits, dtype == 4 ? 4 : 8)
                  .ok(),
              "Bezier oracle output");
      std::cout << std::hex << bits << ' ';
    }
    std::cout << "| ";
    for (unsigned i = 0; i < 3; ++i) {
      std::uint64_t bits = 0;
      require(result.value().values.at("axis").read({i}, &bits, 8).ok(),
              "Bezier oracle axis");
      std::cout << std::hex << bits << ' ';
    }
    std::cout << std::dec << '\n';
  }
}

void benchmark_stress(ps::CpuNumericProfile profile,
                      const std::string& selected) {
  std::cout
      << "profile,fixture,repetitions,median_us,max_us,root_calls,evaluated,"
         "fallbacks,peak_payload_bytes\n";
  for (bool cancellation : {false, true}) {
    const auto sub = [](int units) {
      return static_cast<std::uint64_t>(units < 0 ? -units : units) |
             (units < 0 ? (UINT64_C(1) << 63) : 0);
    };
    const double t = 0x1p-10;
    Fixture fixture(
        node(3, 1, profile),
        cancellation
            ? std::vector<ps::Value>{array(ps::ElementType::Float64, {2, 2},
                                           {0, sub(-17), raw(45), sub(742)}),
                                     array(ps::ElementType::Float64, {1, 2, 2},
                                           {raw(4), sub(66), raw(-38),
                                            sub(-644)}),
                                     doubles({1}, {1}), doubles({1}, {0})}
            : std::vector<ps::Value>{doubles({2, 2}, {0, 0, 1, 1}),
                                     doubles({1, 2, 2}, {0, .25, -1, -.25}),
                                     doubles({1}, {0x1p-30}),
                                     doubles({1}, {0})});
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 8 * 1024 * 1024;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    auto snapshot = take(context.freeze(plan.plan, fixture.bindings));
    ps::ExecutionOptions options;
    options.dependencies.maximum_work = UINT64_C(64) * 1024 * 1024 * 1024;
    options.maximum_dependency_work = UINT64_C(128) * 1024 * 1024 * 1024;
    ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
    std::vector<std::int64_t> times;
    std::uint64_t calls = 0, peak = 0;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
      const auto start = std::chrono::steady_clock::now();
      auto result =
          take(context.execute_fragments(snapshot, query, {}, options));
      times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count());
      value(result, "values", 0,
            cancellation ? (UINT64_C(1) << 63)
                         : raw(.75 * t + .75 * t * t - .5 * t * t * t));
      calls = 0;
      std::uint64_t evaluated = 0, fallbacks = 0;
      for (const auto& timing : result.diagnostics.operation_timings) {
        calls += timing.numeric.strict_math_calls;
        evaluated += timing.numeric.evaluated_values;
        fallbacks += timing.numeric.strict_fallbacks;
      }
      require(evaluated == 1 && !fallbacks, "Bezier stress counts");
      peak = std::max(peak, result.diagnostics.peak_live_bytes);
    }
    std::sort(times.begin(), times.end());
    std::cout << selected << ',' << (cancellation ? "half_subnormal" : "flat_x")
              << ",3," << times[1] << ',' << times[2] << ',' << calls << ",1,0,"
              << peak << '\n';
  }
}

// Deliberately bounded: large dense demands report their actual resource
// failure under the same limits; they are not silently changed into ROIs.
void benchmark(ps::CpuNumericProfile profile, const std::string& selected) {
  std::cout << "profile,degree,K,N,region,repetitions,status,median_us,max_us,"
               "peak_payload_bytes,peak_metadata_bytes,issued_work,root_calls,"
               "evaluated,fallbacks,source_coordinates\n";
  for (unsigned degree : {2, 3})
    for (unsigned k : {2, 64, 4096, 65536})
      for (unsigned n : {256, 65536, 1048576})
        for (bool sparse : {false, true}) {
          std::vector<double> anchors, handles;
          for (unsigned j = 0; j < k; ++j) {
            anchors.insert(anchors.end(),
                           {static_cast<double>(j), static_cast<double>(j)});
            if (j + 1 < k) {
              if (degree == 2)
                handles.insert(handles.end(), {.5, .5});
              else
                handles.insert(handles.end(), {.25, .25, -.25, -.25});
            }
          }
          Fixture fixture(
              node(degree, n, profile),
              {doubles({k, 2}, anchors),
               doubles({k - 1, degree - 1, 2}, handles), doubles({1}, {0}),
               doubles({1}, {static_cast<double>(k - 1)})});
          ps::GraphContext graph(fixture.document);
          auto plan = take(ps::Compiler(fixture.registry).compile(graph));
          ps::ExecutionContextConfig config;
          config.cpu_workers = 1;
          config.maximum_live_bytes = 32 * 1024 * 1024;
          config.managed_resources = ps::ResourceLimits{};
          ps::ExecutionContext context(fixture.registry, config);
          auto budget = take(context.resource_budget());
          auto snapshot = take(context.freeze(plan.plan, fixture.bindings));
          ps::ExecutionOptions options;
          options.maximum_dependency_work = UINT64_C(128) * 1024 * 1024 * 1024;
          options.dependencies.maximum_work = UINT64_C(64) * 1024 * 1024 * 1024;
          options.dependencies.sets.maximum_work = 64 * 1024 * 1024;
          auto wanted =
              sparse
                  ? region({n}, {ps::Region({{0, 1}}), ps::Region({{n / 2, 1}}),
                                 ps::Region({{n - 1, 1}})})
                  : take(ps::Footprint::all({n}));
          ps::DemandQuery query{{"values", wanted}};
          std::vector<std::int64_t> times;
          std::string outcome;
          std::uint64_t work = 0, calls = 0, evaluated = 0, fallbacks = 0,
                        sources = 0;
          for (unsigned repeat = 0; repeat < 3; ++repeat) {
            auto before = budget.statistics().issued.work;
            const auto start = std::chrono::steady_clock::now();
            auto result =
                context.execute_fragments(snapshot, query, {}, options);
            times.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
            work = budget.statistics().issued.work - before;
            const std::string status =
                result.ok() ? "ok" : "resource_exhausted";
            require(result.ok() || result.status().code ==
                                       ps::ErrorCode::ResourceExhausted,
                    "Bezier benchmark unexpected failure");
            require(outcome.empty() || outcome == status,
                    "stable benchmark status");
            outcome = status;
            if (!result.ok())
              continue;
            calls = evaluated = fallbacks = sources = 0;
            for (const auto& timing :
                 result.value().diagnostics.operation_timings) {
              calls += timing.numeric.strict_math_calls;
              evaluated += timing.numeric.evaluated_values;
              fallbacks += timing.numeric.strict_fallbacks;
            }
            require(evaluated == (sparse ? 3 : n) && !fallbacks,
                    "Bezier benchmark counts");
            for (const auto& source :
                 take(result.value().dependencies.source_support()))
              sources += take(source.second.element_count());
            for (const auto& box : wanted.boxes()) {
              const auto span = box.dimensions()[0];
              for (auto i = span.offset; i < span.offset + span.extent; ++i) {
                // All integers are exactly representable. One hardware RN64
                // division is an independent y=x checkpoint for this workload.
                const double q = static_cast<double>(i * (k - 1)) / (n - 1);
                value(result.value(), "values", i, raw(q));
              }
            }
          }
          std::sort(times.begin(), times.end());
          const auto stats = budget.statistics();
          std::cout << selected << ',' << degree << ',' << k << ',' << n << ','
                    << (sparse ? "ROI3" : "Whole") << ",3," << outcome << ','
                    << times[1] << ',' << times[2] << ','
                    << stats.peak[ps::ResourceKind::Payload] << ','
                    << stats.peak[ps::ResourceKind::Metadata] << ',' << work
                    << ',';
          if (outcome == "ok")
            std::cout << calls << ',' << evaluated << ',' << fallbacks << ','
                      << sources;
          else
            std::cout << "NA,NA,NA,NA";
          std::cout << '\n' << std::flush;
        }
}

}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else if (argc > 2 && std::string(argv[2]) == "benchmark") {
      benchmark(profile, selected);
    } else if (argc > 2 && std::string(argv[2]) == "benchmark_stress") {
      benchmark_stress(profile, selected);
    } else {
      examples(profile);
      topology_and_support(profile);
      producers_and_schema(profile);
      layouts_and_resources(profile);
      cache_atoms_and_typed(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
