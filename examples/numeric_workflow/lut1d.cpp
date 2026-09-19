#include "photospider/numeric/lut1d.hpp"

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
    bool channels, ps::CpuNumericProfile profile,
    ps::ElementType dtype = ps::ElementType::Float64,
    ps::numeric::CurveDomain domain = ps::numeric::CurveDomain::Reject) {
  const auto helper = channels ? ps::numeric::apply_lut1d_channels_node
                               : ps::numeric::apply_lut1d_node;
  return take(helper(1, ps::WorkflowInputReference{1},
                     ps::WorkflowInputReference{2},
                     ps::WorkflowInputReference{3}, ps::ElementType::Float64,
                     dtype, domain, profile));
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
void value(const ps::DemandResult& result, const std::vector<std::uint64_t>& at,
           std::uint64_t expected, unsigned width = 8) {
  std::uint64_t bits = 0;
  require(result.values.at("values").read(at, &bits, width).ok() &&
              bits == expected,
          "LUT1D expected bits");
}
void examples(ps::CpuNumericProfile profile) {
  for (bool reverse : {false, true}) {
    Fixture single(node(false, profile),
                   {doubles({4}, {0, .25, .5, 1}),
                    doubles({3}, reverse ? std::vector<double>{1, .25, 0}
                                         : std::vector<double>{0, .25, 1}),
                    doubles({3}, reverse ? std::vector<double>{1, 0, -.5}
                                         : std::vector<double>{0, 1, .5})});
    auto result =
        take(single.run({{"values", take(ps::Footprint::all({4}))}}, false));
    const double expected[] = {0, .125, .25, 1};
    for (unsigned i = 0; i < 4; ++i)
      value(result, {i}, raw(expected[i]));
    Fixture multi(node(true, profile),
                  {doubles({2, 2}, {0, 1, .25, .5}),
                   doubles({2, 2}, reverse ? std::vector<double>{2, 8, 0, 10}
                                           : std::vector<double>{0, 10, 2, 8}),
                   doubles({3}, reverse ? std::vector<double>{1, 0, -1}
                                        : std::vector<double>{0, 1, 1})});
    result =
        take(multi.run({{"values", take(ps::Footprint::all({2, 2}))}}, false));
    const double expected_multi[] = {0, 8, .5, 9};
    for (unsigned i = 0; i < 2; ++i)
      for (unsigned c = 0; c < 2; ++c)
        value(result, {i, c}, raw(expected_multi[2 * i + c]));
    for (auto policy : {ps::numeric::CurveDomain::Clamp,
                        ps::numeric::CurveDomain::LinearExtrapolate}) {
      Fixture outside(node(false, profile, ps::ElementType::Float64, policy),
                      {doubles({2}, {-1, 2}),
                       doubles({2}, reverse ? std::vector<double>{2, 0}
                                            : std::vector<double>{0, 2}),
                       doubles({3}, reverse ? std::vector<double>{1, 0, -1}
                                            : std::vector<double>{0, 1, 1})});
      result =
          take(outside.run({{"values", take(ps::Footprint::all({2}))}}, false));
      value(result, {0},
            raw(policy == ps::numeric::CurveDomain::Clamp ? 0 : -2));
      value(result, {1},
            raw(policy == ps::numeric::CurveDomain::Clamp ? 2 : 4));
    }
  }
  for (auto policy :
       {ps::numeric::CurveDomain::Reject, ps::numeric::CurveDomain::Clamp,
        ps::numeric::CurveDomain::LinearExtrapolate}) {
    Fixture one(
        node(false, profile, ps::ElementType::Float64, policy),
        {doubles({1},
                 {policy == ps::numeric::CurveDomain::Reject ? 2.0 : 99.0}),
         doubles({1}, {7}), doubles({3}, {2, 2, 0})});
    auto result =
        take(one.run({{"values", take(ps::Footprint::all({1}))}}, false));
    value(result, {0}, raw(7));
  }
  for (unsigned column = 0; column < 2; ++column) {
    Fixture scalar(node(false, profile),
                   {doubles({1}, {column ? .75 : .25}),
                    doubles({2}, column ? std::vector<double>{10, 8}
                                        : std::vector<double>{0, 2}),
                    doubles({3}, {0, 1, 1})});
    auto result =
        take(scalar.run({{"values", take(ps::Footprint::all({1}))}}, false));
    value(result, {0}, raw(column ? 8.5 : .5));
  }
  auto authored = take(ps::numeric::apply_lut1d_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::ElementType::Float32, {},
      ps::numeric::CurveDomain::Reject, profile));
  Fixture mixed(authored, {array(ps::ElementType::Float32, {1}, {0x3e800000}),
                           doubles({2}, {0, 2}), doubles({3}, {0, 1, 1})});
  auto result =
      take(mixed.run({{"values", take(ps::Footprint::all({1}))}}, false));
  value(result, {0}, 0x3f000000, 4);
  require(result.values.at("values").descriptor().element_type ==
              ps::ElementType::Float32,
          "constructor defaults to input dtype hint");
  std::cout << "scalar/channels analytic, descending, "
               "clamp/extrapolate/singleton and input-dtype default passed\n";
}
void axis_and_support(ps::CpuNumericProfile profile) {
  const auto nan = UINT64_C(0x7ff0000000000042), sign = UINT64_C(1) << 63;
  Fixture local(node(true, profile),
                {doubles({2, 2}, {.25, .75, .5, 1}),
                 array(ps::ElementType::Float64, {3, 2},
                       {0, nan, raw(1), nan, raw(2), raw(8)}),
                 doubles({3}, {0, 1, .5})});
  auto q = region({2, 2},
                  {ps::Region({{0, 1}, {0, 1}}), ps::Region({{1, 1}, {1, 1}})});
  auto result = take(local.run({{"values", q}}, false));
  value(result, {0, 0}, raw(.5));
  value(result, {1, 1}, raw(8));
  auto support = take(result.dependencies.source_support());
  require(support.at("input0") == q &&
              support.at("input1") ==
                  region({3, 2}, {ps::Region({{0, 2}, {0, 1}}),
                                  ps::Region({{2, 1}, {1, 1}})}) &&
              support.at("input2") == take(ps::Footprint::all({3})),
          "LUT1D exact query/local table/shared axis support");
  auto dirty = take(result.dependencies.potential_dirty(
      "input1", region({3, 2}, {ps::Region({{1, 1}, {0, 1}})})));
  require(dirty.at("values") == region({2, 2}, {ps::Region({{0, 1}, {0, 1}})}),
          "column-local pair dirty");
  auto remote = take(result.dependencies.potential_dirty(
      "input1", region({3, 2}, {ps::Region({{1, 1}, {1, 1}})})));
  require(!remote.count("values") || remote.at("values").empty(),
          "unused table column no dirty");
  dirty = take(result.dependencies.potential_dirty(
      "input2", region({3}, {ps::Region({{2, 1}})})));
  require(dirty.at("values") == q,
          "whole axis invalidates every requested component");
  for (auto axis : std::vector<std::vector<std::uint64_t>>{
           {0, raw(1), raw(1)},
           {raw(1), raw(1) + 1, raw(0x1p-53)},
           {0, raw(1), nan},
           {raw(-0.0), 0, 0},
           {raw(-0.0), raw(-0.0), sign}}) {
    const unsigned l = (axis[0] == sign) ? 1 : 3;
    Fixture bad(node(false, profile),
                {doubles({1}, {0}), doubles({l}, std::vector<double>(l, 7)),
                 array(ps::ElementType::Float64, {3}, axis)});
    auto failed = bad.run({{"values", take(ps::Footprint::all({1}))}}, false);
    require(!failed.ok() &&
                failed.status().reason == ps::FailureReason::InvalidDomain,
            "inconsistent/collapsed/nonfinite/signed singleton axis rejected");
  }
  for (unsigned pattern = 0; pattern < 3; ++pattern) {
    Fixture zero(node(false, profile),
                 {doubles({3}, {0, .5, 1}),
                  array(ps::ElementType::Float64, {2},
                        {sign, pattern == 0   ? sign
                               : pattern == 1 ? 0
                                              : UINT64_C(1)}),
                  doubles({3}, {0, 1, 1})});
    result = take(zero.run({{"values", take(ps::Footprint::all({3}))}}, false));
    value(result, {0}, sign);
    value(result, {1}, pattern == 0 ? sign : 0);
  }
  Fixture singleton_nan(node(false, profile, ps::ElementType::Float64,
                             ps::numeric::CurveDomain::Clamp),
                        {array(ps::ElementType::Float64, {1}, {nan}),
                         doubles({1}, {7}), doubles({3}, {2, 2, 0})});
  auto failed =
      singleton_nan.run({{"values", take(ps::Footprint::all({1}))}}, false);
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::InvalidDomain,
          "singleton still validates input");
  std::cout
      << "exact sparse support/dirty, invalid global axes and collapsed "
         "coordinates, signed-zero and singleton query validation passed\n";
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
          "LUT1D exact stage supply");
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
  auto registry = ps::make_default_operation_registry();
  auto authored = node(true, profile);
  std::vector<ps::Value> dense{doubles({2, 2}, {0, 1, .25, .5}),
                               doubles({2, 2}, {0, 10, 2, 8}),
                               doubles({3}, {0, 1, 1})};
  ps::DependencyRequest request;
  for (const auto& input : dense)
    request.inputs.push_back({input.descriptor(), {}});
  request.parameters = authored.parameters;
  request.outputs = region({2, 2}, {ps::Region({{1, 1}, {1, 1}})});
  request.snapshot_identity = "LUT1D-direct";
  request.limits.maximum_work = UINT64_C(16) << 30;
  for (unsigned mask = 0; mask < 8; ++mask) {
    auto inputs = dense;
    for (unsigned p = 0; p < 3; ++p)
      if (mask & (1U << p))
        inputs[p] = reversed_unaligned(inputs[p]);
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save LUT fenv");
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "set LUT fenv");
      auto session =
          take(registry->start_dependency(authored.operation, request));
      for (unsigned stage = 0; stage < 3; ++stage) {
        require(session->poll().ok(), "LUT Need");
        supply(session, request, inputs);
      }
      auto result = take(session->poll());
      std::uint64_t bits = 0;
      require(std::get<ps::DependencyResult>(result)
                      .value.read({1, 1}, &bits, 8)
                      .ok() &&
                  bits == raw(9),
              "LUT strided bits");
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "LUT preserves fenv");
    }
    require(fesetenv(&saved) == 0, "restore LUT fenv");
  }
  for (unsigned mode = 0; mode < 3; ++mode) {
    ps::ResourceBudget budget(ps::ResourceLimits{});
    ps::CancellationSource cancellation;
    request.cancellation = cancellation.token();
    request.outputs = region(
        {2, 2}, {ps::Region({{0, 1}, {0, 1}}), ps::Region({{1, 1}, {1, 1}})});
    bool interrupted = false;
    std::shared_ptr<ps::DependencySession> session;
    session = take(registry->start_dependency(
        authored.operation, request, budget.allocator(),
        [&](std::uint64_t work) {
          if (work == 352 && session->numeric_diagnostics().evaluated_values ==
                                 (mode == 2 ? 2U : 1U)) {
            interrupted = true;
            if (mode)
              cancellation.cancel();
            else
              return ps::Status{ps::ErrorCode::ResourceExhausted,
                                "LUT inner work", ps::FailureReason::WorkLimit};
          }
          return ps::Status::success();
        }));
    for (unsigned stage = 0; stage < 3; ++stage) {
      require(session->poll().ok(), "LUT Need before interrupt");
      supply(session, request, dense);
    }
    auto failed = session->poll();
    require(
        interrupted && !failed.ok() &&
            failed.status().code == (mode ? ps::ErrorCode::Cancelled
                                          : ps::ErrorCode::ResourceExhausted),
        "LUT arithmetic interruption");
    require(
        session->numeric_diagnostics().copied_elements == (mode == 2 ? 1U : 0U),
        "LUT unpublished count");
    session.reset();
    require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "LUT failed outputs/state release");
  }
  request.cancellation = {};
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto limited = request;
    if (mode == 0)
      limited.limits.maximum_state_bytes = 1;
    if (mode == 1)
      limited.limits.maximum_work = 1;
    if (mode == 2)
      limited.limits.maximum_stages = 1;
    auto started = registry->start_dependency(authored.operation, limited);
    bool failed = !started.ok();
    if (started.ok()) {
      for (unsigned stage = 0; stage < 4; ++stage) {
        auto progress = started.value()->poll();
        if (!progress.ok()) {
          require(progress.status().code == ps::ErrorCode::ResourceExhausted,
                  "LUT resource status");
          failed = true;
          break;
        }
        if (std::holds_alternative<ps::DependencyResult>(progress.value()))
          break;
        supply(started.value(), limited, dense);
      }
    }
    require(failed, "LUT state/work/stage bounds");
  }
  auto grid_request = request;
  auto grid_inputs = dense;
  grid_inputs[1] = doubles({16, 2}, std::vector<double>(32, 7));
  grid_inputs[2] = doubles({3}, {0, 15, 1});
  grid_request.inputs[1].descriptor = grid_inputs[1].descriptor();
  ps::CancellationSource grid_cancel;
  grid_request.cancellation = grid_cancel.token();
  unsigned grid_calls = 0;
  ps::ResourceBudget grid_budget(ps::ResourceLimits{});
  auto interrupted = take(registry->start_dependency(
      authored.operation, grid_request, grid_budget.allocator(),
      [&](std::uint64_t amount) {
        if (amount == 8192 && ++grid_calls == 8)
          grid_cancel.cancel();
        return ps::Status::success();
      }));
  require(interrupted->poll().ok(), "axis Need before grid cancellation");
  supply(interrupted, grid_request, grid_inputs);
  auto cancelled = interrupted->poll();
  require(!cancelled.ok() &&
              cancelled.status().code == ps::ErrorCode::Cancelled &&
              grid_calls == 8 &&
              interrupted->numeric_diagnostics().evaluated_values == 0,
          "inside full-axis cancellation precedes query/table");
  interrupted.reset();
  require(grid_budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "axis cancellation owner release");
  auto zero = doubles({1}, {0}), seven = doubles({1}, {7});
  std::vector<ps::Value> aliases{
      take(ps::Value::from_storage({ps::ElementType::Float64, {2, 2}},
                                   ps::Region::whole({2, 2}), {0, {0, 0}},
                                   zero.storage())),
      take(ps::Value::from_storage({ps::ElementType::Float64, {1, 2}},
                                   ps::Region::whole({1, 2}), {0, {0, 0}},
                                   seven.storage())),
      take(ps::Value::from_storage({ps::ElementType::Float64, {3}},
                                   ps::Region::whole({3}), {0, {0}},
                                   zero.storage()))};
  auto alias_request = request;
  alias_request.inputs.clear();
  for (const auto& v : aliases)
    alias_request.inputs.push_back({v.descriptor(), {}});
  auto alias =
      take(registry->start_dependency(authored.operation, alias_request));
  for (unsigned stage = 0; stage < 3; ++stage) {
    require(alias->poll().ok(), "zero-stride LUT Need");
    supply(alias, alias_request, aliases);
  }
  auto alias_result = take(alias->poll());
  std::uint64_t bits = 0;
  require(std::get<ps::DependencyResult>(alias_result)
                  .value.read({1, 1}, &bits, 8)
                  .ok() &&
              bits == raw(7),
          "zero-stride all-port singleton LUT");
  request.cancellation = {};
  request.outputs = take(ps::Footprint::none({2, 2}));
  auto empty = take(registry->start_dependency(authored.operation, request));
  require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
              empty->poll_count() == 0,
          "LUT Empty no payload");
  for (unsigned kind = 0; kind < 8; ++kind) {
    auto bad = request;
    if (kind == 0)
      bad.inputs[1].descriptor.shape = {2, 3};
    if (kind == 1)
      bad.inputs[1].descriptor.shape = {1048577, 2};
    if (kind == 2)
      bad.inputs[2].descriptor.element_type = ps::ElementType::Float32;
    if (kind == 3)
      bad.inputs[2].descriptor.shape = {2};
    if (kind == 4)
      bad.parameters.erase("dtype");
    if (kind == 5)
      bad.parameters["out_of_domain"] = std::string("wrap");
    if (kind == 6)
      bad.inputs[0].descriptor.element_type = ps::ElementType::Int64;
    if (kind == 7)
      bad.inputs[1].descriptor.shape = {2, UINT64_C(1) << 40};
    require(!registry->start_dependency(authored.operation, bad).ok(),
            "LUT static schema bounds");
  }
  std::cout << "all-port negative/unaligned strides/fenv, "
               "axis/inner-work/cancel/second-box release, zero strides and "
               "Empty/schema/state/stage passed\n";
}
void baking_chains(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 6; ++kind) {
    const bool multi = kind >= 4;
    std::vector<ps::Value> inputs{
        doubles(multi ? std::vector<std::uint64_t>{1, 2}
                      : std::vector<std::uint64_t>{1},
                kind == 5 ? std::vector<double>{.75, 1.25}
                : multi   ? std::vector<double>{.25, .75}
                          : std::vector<double>{.25}),
        doubles({1}, {kind == 5 ? .5 : 0}),
        doubles({1}, {kind == 2 || kind == 3 ? 2.0
                      : kind == 5            ? 1.5
                                             : 1.0})};
    if (kind == 1) {
      inputs.push_back(doubles({2, 2}, {0, 0, 1, 1}));
      inputs.push_back(doubles({1, 1, 2}, {.5, 0}));
    }
    if (kind == 2) {
      inputs.push_back(doubles({3}, {0, 1, 2}));
      inputs.push_back(doubles({3}, {0, 2, 4}));
    }
    if (kind == 3) {
      inputs.push_back(doubles({3}, {0, 1, 2}));
      inputs.push_back(doubles({3}, {0, 1, 4}));
    }
    if (kind == 4) {
      inputs.push_back(doubles({2}, {0, 1}));
      inputs.push_back(doubles({2, 2}, {0, 10, 2, 8}));
    }
    if (kind == 5) {
      inputs.push_back(doubles({3}, {0, 1, 2}));
      inputs.push_back(doubles({3, 2}, {0, 4, 1, 3, 4, 0}));
    }
    Fixture fixture(node(multi, profile), inputs);
    fixture.document.nodes.clear();
    auto start = ps::numeric::sequence_input(fixture.document.inputs[1]),
         end = ps::numeric::sequence_input(fixture.document.inputs[2]);
    ps::numeric::BakedLut1d baked;
    if (kind == 0) {
      baked = take(ps::numeric::bake_lut1d_expression(
          fixture.document, "x^2", start, end, 3, {}, ps::ElementType::Float64,
          profile));
    } else if (kind == 1) {
      baked = take(ps::numeric::bake_lut1d_bezier(
          fixture.document, ps::WorkflowInputReference{4},
          ps::WorkflowInputReference{5}, start, end, 2, 3,
          ps::ElementType::Float64, ps::numeric::BezierDomain::Reject,
          profile));
    } else {
      const auto bake = kind == 2   ? ps::numeric::bake_lut1d_linear
                        : kind == 3 ? ps::numeric::bake_lut1d_pchip
                        : kind == 4 ? ps::numeric::bake_lut1d_linear_multi
                                    : ps::numeric::bake_lut1d_pchip_multi;
      baked = take(bake(fixture.document, ps::WorkflowInputReference{4},
                        ps::WorkflowInputReference{5}, start, end,
                        kind == 3   ? 5
                        : kind == 5 ? 2
                                    : 3,
                        ps::ElementType::Float64,
                        ps::numeric::CurveDomain::Reject, profile));
    }
    const auto apply = multi ? ps::numeric::apply_lut1d_channels_node
                             : ps::numeric::apply_lut1d_node;
    fixture.document.nodes.push_back(
        take(apply(100, ps::WorkflowInputReference{1}, baked.values, baked.axis,
                   ps::ElementType::Float64, {},
                   ps::numeric::CurveDomain::Reject, profile)));
    fixture.document.outputs = {{"values", 100, "values"}};
    const auto shape = multi ? std::vector<std::uint64_t>{1, 2}
                             : std::vector<std::uint64_t>{1};
    auto result =
        take(fixture.run({{"values", take(ps::Footprint::all(shape))}}, false));
    if (multi) {
      value(result, {0, 0}, raw(kind == 4 ? .5 : .78125));
      value(result, {0, 1}, raw(kind == 4 ? 8.5 : 2.28125));
    } else {
      value(result, {0}, raw(kind < 2 ? .125 : kind == 2 ? .5 : .15625));
    }
    if (kind >= 2) {
      fixture.bindings.inputs[2].value = inputs[1];
      auto invalid =
          fixture.run({{"values", take(ps::Footprint::all(shape))}}, false);
      require(!invalid.ok() &&
                  invalid.status().reason == ps::FailureReason::InvalidDomain,
              "consumer rejects valid constant interpolation bake axis");
    }
  }
  std::cout << "all six baking-to-LUT chains and separate discrete "
               "approximation/axis acceptance passed\n";
}
void cache_atoms_and_large(ps::CpuNumericProfile profile) {
  Fixture fixture(
      node(false, profile),
      {doubles({1}, {.25}), doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, .5})});
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
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  auto demand = take(context.open_demand(plan.plan, fixture.bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(32) << 30;
  options.dependencies.maximum_work = UINT64_C(16) << 30;
  options.maximum_dependency_cache_work = 128 * 1024 * 1024;
  ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
  value(take(demand.request(query, {}, options)), {0}, raw(.5));
  require(take(demand.request(query, {}, options)).diagnostics.cache_hits > 0,
          "LUT warm cache");
  fixture.bindings.inputs[0].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(doubles({1}, {.75}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "LUT input replacement");
  auto changed = take(demand.request(query, {}, options));
  value(changed, {0}, raw(1.5));
  require(take(changed.dependencies.source_support()).at("input1") ==
              region({3}, {ps::Region({{1, 2}})}),
          "query cache changes pair");
  fixture.bindings.inputs[1].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(doubles({3}, {0, 1, 4}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "LUT table replacement");
  value(take(demand.request(query, {}, options)), {0}, raw(2.5));
  fixture.bindings.inputs[2].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(store.import_value(doubles({3}, {0, 2, 1}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "LUT axis replacement");
  changed = take(demand.request(query, {}, options));
  value(changed, {0}, raw(.75));
  require(take(changed.dependencies.source_support()).at("input1") ==
              region({3}, {ps::Region({{0, 2}})}),
          "axis cache changes pair");
  const auto nan = UINT64_C(0x7ff0000000000042);
  Fixture isolated(node(true, profile), {doubles({1, 2}, {.5, .5}),
                                         array(ps::ElementType::Float64, {2, 2},
                                               {0, nan, raw(2), nan}),
                                         doubles({3}, {0, 1, 1})});
  ps::GraphContext isolated_graph(isolated.document);
  auto isolated_plan =
      take(ps::Compiler(isolated.registry).compile(isolated_graph));
  ps::ExecutionContext isolated_context(isolated.registry, config);
  for (bool joint : {false, true}) {
    options.enable_joint = joint;
    auto atoms = take(isolated_context.execute_atoms(
        isolated_plan.plan, isolated.bindings,
        {{"values", take(ps::Footprint::all({1, 2}))}}, {}, options));
    unsigned good = 0, bad = 0;
    for (const auto& atom : atoms.atoms) {
      if (atom.outcome.ok()) {
        ++good;
      } else {
        ++bad;
        require(atom.key.rank == 2 && atom.key.coordinate[1] == 1 &&
                    atom.outcome.status().detail.atom == atom.key,
                "channel Atom identity");
      }
    }
    require(good == 1 && bad == 1,
            "good channel survives remote column failure");
  }
  const std::vector<std::uint64_t> rank8{2, 1, 1, 1, 1, 1, 1, 2};
  Fixture high_rank(node(true, profile),
                    {doubles(rank8, {0, .5, 1, 1.5}),
                     doubles({2, 2}, {0, 10, 2, 8}), doubles({3}, {0, 1, 1})});
  auto rejected =
      high_rank.run({{"values", region(rank8, {ps::Region({{1, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {1, 1}})})}},
                    false);
  require(!rejected.ok() && rejected.status().detail.atom &&
              rejected.status().detail.atom->rank == 8 &&
              rejected.status().detail.atom->coordinate[0] == 1 &&
              rejected.status().detail.atom->coordinate[7] == 1,
          "rank8 global error Atom");
  const auto columns = UINT64_C(1) << 39;
  Fixture huge(node(true, profile), {doubles({1}, {.5}), doubles({1}, {7}),
                                     doubles({3}, {0, 1, 1})});
  huge.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "values"};
  huge.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{3, "values"};
  huge.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{1}, {1, columns},
                                 ps::numeric::ArrayLayout::View, profile)));
  huge.document.nodes.push_back(take(
      ps::numeric::constant_node(3, ps::WorkflowInputReference{2}, {2, columns},
                                 ps::numeric::ArrayLayout::View, profile)));
  auto result = take(huge.run(
      {{"values",
        region({1, columns}, {ps::Region({{0, 1}, {columns - 1, 1}})})}},
      false));
  value(result, {0, columns - 1}, raw(7));
  const unsigned length = 1048576;
  Fixture full_grid(node(false, profile),
                    {doubles({1}, {0}), doubles({1}, {7}),
                     doubles({3}, {0, 1, 1.0 / (length - 1)})});
  full_grid.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "values"};
  full_grid.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{2}, {length},
                                 ps::numeric::ArrayLayout::View, profile)));
  result =
      take(full_grid.run({{"values", take(ps::Footprint::all({1}))}}, false));
  value(result, {0}, raw(7));
  require(take(result.dependencies.source_support()).at("input1") ==
              take(ps::Footprint::all({1})),
          "full grid validation keeps scalar backing for constant table");
  std::cout << "input/table/axis cache reselection, channel Atom isolation, "
               "sparse 2^39 columns and complete maximum-L grid passed\n";
}
void typed_and_upstream(ps::CpuNumericProfile profile) {
  auto input = array(ps::ElementType::Float32, {1, 1, 4},
                     {0x3e800000, 0, 0, 0x40000000});
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  input =
      take(ps::Value::from_storage(input.descriptor(), input.region(),
                                   input.layout(), input.storage(), {facet}));
  std::vector<ps::Value> inputs{input, doubles({2}, {0, 2}),
                                doubles({3}, {0, 1, 1})};
  auto registry = ps::make_default_operation_registry();
  auto authored = node(false, profile);
  ps::DependencyRequest request;
  for (const auto& v : inputs)
    request.inputs.push_back({v.descriptor(), v.facets()});
  request.parameters = authored.parameters;
  request.outputs = region({1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {0, 1}})});
  request.snapshot_identity = "LUT-image";
  request.limits.maximum_work = UINT64_C(16) << 30;
  auto session = take(registry->start_dependency(authored.operation, request));
  require(session->poll().ok(), "typed axis Need");
  supply(session, request, inputs);
  require(session->poll().ok(), "typed query Need");
  std::vector<ps::Footprint> wanted;
  for (const auto& v : inputs)
    wanted.push_back(take(ps::Footprint::none(v.descriptor().shape)));
  bool control = false, validation = false;
  for (const auto& need : take(session->pending_reads())) {
    wanted[need.port] = take(wanted[need.port].unite(need.samples));
    if (need.port == 0) {
      if (need.roles & 2) {
        require(need.samples == request.outputs,
                "typed Control single channel");
        control = true;
      }
      if (need.roles & 4) {
        require(need.samples == take(ps::Footprint::all({1, 1, 4})),
                "typed Validation whole channels");
        validation = true;
      }
    }
  }
  require(control && validation && wanted[1].empty(),
          "typed closure before table demand");
  std::vector<ps::ValueFragments> ready;
  for (unsigned p = 0; p < 3; ++p) {
    auto all = take(ps::ValueFragments::create(
        inputs[p].descriptor(), inputs[p].facets(),
        take(ps::Footprint::all(inputs[p].descriptor().shape)), {inputs[p]}));
    ready.push_back(take(all.restrict(wanted[p])));
  }
  require(!session->supply(ready, request.snapshot_identity).ok() &&
              session->numeric_diagnostics().evaluated_values == 0,
          "unselected alpha fails before table/evaluation");
  registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition source;
  source.key = "manual.lut_table";
  source.traits.input_count = 0;
  source.traits.input_schema.clear();
  source.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
  source.traits.outputs[0].fixed_output_shape = {2};
  source.traits.outputs[0].output_element_type = ps::ElementType::Float64;
  source.callback = [&](const auto&) {
    ++calls;
    return ps::Result<ps::Value>(ps::Status{ps::ErrorCode::OperationFailed,
                                            "required LUT table producer"});
  };
  require(registry->register_operation(std::move(source)).ok() &&
              registry->freeze().ok(),
          "LUT source registry");
  Fixture fixture(
      node(false, profile),
      {doubles({1}, {2}), doubles({2}, {0, 2}), doubles({3}, {0, 1, 2})});
  fixture.registry = registry;
  fixture.document.inputs.erase(fixture.document.inputs.begin() + 1);
  fixture.bindings.inputs.erase(fixture.bindings.inputs.begin() + 1);
  fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.lut_table", {}, {}});
  auto q = ps::DemandQuery{{"values", take(ps::Footprint::all({1}))}};
  auto failed = fixture.run(q, false);
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::InvalidDomain &&
              !calls,
          "invalid axis before table producer");
  fixture.bindings.inputs[1].value = doubles({3}, {0, 1, 1});
  failed = fixture.run(q, false);
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::InvalidDomain &&
              !calls,
          "reject query before table producer");
  fixture.bindings.inputs[0].value = doubles({1}, {.5});
  failed = fixture.run(q, false);
  require(!failed.ok() &&
              failed.status().message == "required LUT table producer" &&
              calls == 1,
          "selected table upstream failure preserved");
  std::cout << "typed Image Control/Validation closure and axis/query/table "
               "producer order passed\n";
}

void oracle(ps::CpuNumericProfile profile) {
  unsigned channels = 0, dtype = 0, policy = 0, rank = 0, l = 0, c = 0,
           requested = 0;
  while (std::cin >> channels >> dtype >> policy >> rank >> l >> c >>
         requested) {
    std::vector<std::uint64_t> shape(rank);
    std::uint64_t size = 1;
    for (auto& extent : shape) {
      std::cin >> extent;
      size *= extent;
    }
    std::vector<std::vector<std::uint64_t>> coords(
        requested, std::vector<std::uint64_t>(rank));
    for (auto& at : coords)
      for (auto& index : at)
        std::cin >> index;
    std::vector<ps::Value> inputs;
    for (unsigned p = 0; p < 3; ++p) {
      unsigned type = 0;
      std::cin >> type;
      std::vector<std::uint64_t> bits(p == 0 ? size : p == 1 ? l * c : 3);
      for (auto& b : bits)
        std::cin >> std::hex >> b >> std::dec;
      inputs.push_back(array(static_cast<ps::ElementType>(type),
                             p == 0 ? shape
                             : p == 1
                                 ? (channels ? std::vector<std::uint64_t>{l, c}
                                             : std::vector<std::uint64_t>{l})
                                 : std::vector<std::uint64_t>{3},
                             bits));
    }
    Fixture fixture(node(channels, profile, static_cast<ps::ElementType>(dtype),
                         static_cast<ps::numeric::CurveDomain>(policy)),
                    inputs);
    std::vector<ps::Region> boxes;
    for (const auto& at : coords) {
      std::vector<ps::RegionDimension> spans;
      for (auto index : at)
        spans.push_back({index, 1});
      boxes.emplace_back(std::move(spans));
    }
    auto result =
        fixture.run({{"values", region(shape, std::move(boxes))}}, false);
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
    for (const auto& at : coords) {
      std::uint64_t bits = 0;
      require(result.value()
                  .values.at("values")
                  .read(at, &bits, dtype == 4 ? 4 : 8)
                  .ok(),
              "LUT oracle read");
      std::cout << std::hex << bits << ' ';
    }
    std::cout << std::dec << '\n';
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
    } else {
      examples(profile);
      axis_and_support(profile);
      layouts_and_resources(profile);
      baking_chains(profile);
      cache_atoms_and_large(profile);
      typed_and_upstream(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
