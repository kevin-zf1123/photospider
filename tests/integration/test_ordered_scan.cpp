#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Value values(const std::vector<double>& numbers) {
  std::vector<std::uint8_t> bytes(numbers.size() * 8);
  std::memcpy(bytes.data(), numbers.data(), bytes.size());
  return Value::create({ElementType::Float64, {numbers.size()}},
                       Region::whole({numbers.size()}), {0, {8}}, bytes)
      .take_value();
}
WorkflowDocument document(std::uint64_t n, std::int64_t block) {
  WorkflowDocument doc;
  doc.inputs = {
      {1, "x", {ElementType::Float64, {n}}, Region::whole({n}), {0, {8}}, {}}};
  doc.nodes = {{1,
                "numeric.ordered_scan",
                {WorkflowInputReference{1}},
                {{"block_size", block}}}};
  doc.outputs = {{"y", 1, "value"}};
  return doc;
}
Footprint point(std::uint64_t at, std::uint64_t n) {
  return Footprint::from_regions({n}, {Region({{at, 1}})}).take_value();
}
int direct_and_checkpoint_guards() {
  auto registry = make_default_operation_registry();
  const auto input = values({1, 2});
  const std::vector<Value> inputs{input};
  const std::vector<Region> regions{input.region()};
  const std::map<std::string, ParameterValue> parameters{
      {"block_size", INT64_C(1)}};
  OperationInvocation invocation{inputs,           regions, parameters,
                                 Backend::Cpu,     {},      input.region(),
                                 BufferAllocator{}};
  auto direct = registry->invoke("numeric.ordered_scan", invocation);
  PS_CHECK(direct.ok());
  double second = 0;
  std::memcpy(&second, direct.value().bytes().data() + 8, 8);
  PS_CHECK(second == 3);
  DependencyRequest request{{{input.descriptor(), {}}},
                            parameters,
                            point(1, 2),
                            "snapshot"};
  DependencyCheckpoint saved;
  DependencyCheckpointServices services;
  services.identity = "node-a";
  services.publish = [&](const DependencyCheckpoint& checkpoint) {
    saved = checkpoint;
    return Status::success();
  };
  for (bool host : {false, true}) {
    auto session = registry->start_dependency("numeric.ordered_scan", request)
                       .take_value();
    for (;;) {
      auto progress = session->poll(
          BufferAllocator{}, host ? services : DependencyCheckpointServices{});
      PS_CHECK(progress.ok());
      if (const auto* done = std::get_if<DependencyResult>(&progress.value())) {
        PS_CHECK(done->value.read({1}, &second, 8).ok() && second == 3);
        break;
      }
      auto pending = session->pending_reads().take_value();
      auto coverage = Footprint::none({2}).take_value();
      for (const auto& need : pending)
        coverage = coverage.unite(need.samples).take_value();
      PS_CHECK(session
                   ->supply({ValueFragments::create(input.descriptor(), {},
                                                    coverage, {input})
                                 .take_value()},
                            "snapshot")
                   .ok());
    }
  }
  PS_CHECK(saved.valid() && saved.sequence() == 1 &&
           saved.state().as_float64().value() == 3);
  services.find = [&](std::uint32_t, std::uint64_t) {
    return Result<std::optional<DependencyCheckpoint>>(saved);
  };
  auto hit =
      registry->start_dependency("numeric.ordered_scan", request).take_value();
  auto completed = hit->poll(BufferAllocator{}, services);
  PS_CHECK(completed.ok() &&
           std::holds_alternative<DependencyResult>(completed.value()));
  const auto& evidence =
      std::get<DependencyResult>(completed.value()).certificate;
  PS_CHECK(evidence && evidence->backward(point(1, 2)).ok());
  for (unsigned bad = 0; bad < 3; ++bad) {
    auto invalid = request;
    auto wrong = services;
    if (bad == 0)
      wrong.identity = "node-b";
    else if (bad == 1)
      invalid.snapshot_identity = "another-snapshot";
    else
      invalid.outputs = point(0, 2);  // Host illegally returns a future state.
    auto session = registry->start_dependency("numeric.ordered_scan", invalid)
                       .take_value();
    PS_CHECK(session->poll(BufferAllocator{}, wrong).status().code ==
             ErrorCode::InvalidArgument);
  }
  auto bounded = request;
  bounded.limits.sets.maximum_boxes = 8;
  auto small = registry->start_dependency("numeric.ordered_scan", bounded);
  PS_CHECK(small.ok());
  PS_CHECK(small.value()->poll(BufferAllocator{}, services).status().code ==
           ErrorCode::ResourceExhausted);
  auto first = BufferAllocator{}.limited(16);
  auto sibling = BufferAllocator{}.limited(16);
  auto writer = MutableValue::allocate({ElementType::Float64, {1}},
                                       Region::whole({1}), first)
                    .take_value();
  auto published = std::move(writer).publish().take_value();
  PS_CHECK(first.owns_allocation(*published.storage()));
  PS_CHECK(!sibling.owns_allocation(*published.storage()) &&
           !first.owns(*published.storage()));
  return 0;
}
int exact_reads(bool ancestor) {
  auto registry = make_default_operation_registry();
  constexpr std::uint64_t n = 256;
  auto doc = document(n, 16);
  if (ancestor) {
    doc.nodes[0].inputs = {WorkflowNodeOutput{2, "value"}};
    doc.nodes.push_back({2, "core.identity", {WorkflowInputReference{1}}, {}});
  }
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = {ElementType::Float64, {n}};
  std::uint64_t samples = 0, next = 0;
  source->read = [&](const Region& region, std::uint8_t* bytes,
                     std::uint64_t size, const BufferAllocator&,
                     const CancellationToken&) {
    if (region.dimensions()[0].offset != next)
      return Result<Region>(
          Status{ErrorCode::OperationFailed, "scan reread prefix"});
    for (std::uint64_t i = 0; i < size / 8; ++i) {
      const double value = static_cast<double>(next + 1);
      std::memcpy(bytes + i * 8, &value, 8);
      ++next;
      ++samples;
    }
    return Result<Region>(region);
  };
  ExecutionContext context(registry, {1, false, 8, 8192});
  auto result = context.execute(plan, {{{"x", {}, source}}});
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok() && samples == n);
  const auto& output = result.value().values.at("y");
  for (std::uint64_t i = 0; i < n; ++i) {
    double value = 0;
    std::memcpy(&value, output.bytes().data() + i * 8, 8);
    PS_CHECK(value == static_cast<double>((i + 1) * (i + 2) / 2));
  }
  auto certificate = result.value().dependencies.certificate(1);
  PS_CHECK(certificate.ok());
  for (auto i : {0U, 13U, 255U}) {
    const auto prefix =
        Footprint::from_regions({n}, {Region({{0, i + 1}})}).take_value();
    auto support = certificate.value().backward(point(i, n));
    PS_CHECK(support.ok());
    Footprint fetched = Footprint::none({n}).take_value();
    for (const auto& need : support.value())
      if (need.roles & 1)
        fetched = fetched.unite(need.samples).take_value();
    PS_CHECK(fetched == prefix);
  }
  auto dirty = result.value().dependencies.potential_dirty("x", point(13, n));
  PS_CHECK(
      dirty.ok() &&
      dirty.value().at("y") ==
          Footprint::from_regions({n}, {Region({{13, n - 13}})}).take_value());
  return 0;
}
int edited_prefixes() {
  auto registry = make_default_operation_registry();
  GraphContext graph(document(4, 2));
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096, 256});
  ExecutionBindings bindings{{{"x", values({0, 1, 0x1p54, 4})}}};
  auto demand = context.open_demand(plan, bindings).take_value();
  DemandQuery query{
      {"y", Footprint::from_regions({4}, {Region({{1, 3}})}).take_value()}};
  auto initial = demand.request(query);
  PS_CHECK(initial.ok());
  auto frozen = demand.freeze().take_value();
  bindings.inputs[0].value = values({1, 1, 0x1p54, 4});
  auto edit = demand.replace_bindings(bindings);
  PS_CHECK(edit.ok() && edit.value().potential_dirty.at("y") == query.at("y"));
  auto changed = demand.request(query);
  auto old = context.execute_fragments(frozen, query);
  PS_CHECK(changed.ok() && old.ok());
  PS_CHECK(changed.value().diagnostics.block_cache_hits == 1 &&
           changed.value().diagnostics.block_cache_misses == 2);
  for (unsigned i = 1; i < 4; ++i) {
    double before = 0, after = 0, pinned = 0;
    PS_CHECK(initial.value().values.at("y").read({i}, &before, 8).ok());
    PS_CHECK(changed.value().values.at("y").read({i}, &after, 8).ok());
    PS_CHECK(old.value().values.at("y").read({i}, &pinned, 8).ok());
    const double expected = i == 1 ? 1 : i == 2 ? 0x1p54 : 0x1p54 + 4;
    PS_CHECK(before == expected && pinned == expected);
    // Incoming 0/1 changes the current block's first output, although its
    // final outgoing accumulator reconverges at 2^54 by nearest-even rounding.
    PS_CHECK(after == (i == 1 ? 2 : expected));
  }
  return 0;
}
int block_reconvergence() {
  auto registry = make_default_operation_registry();
  GraphContext graph(document(6, 1));
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096, 512});
  std::vector<double> numbers{0, 1, 0x1p54, 4, 5, 6};
  auto demand =
      context.open_demand(plan, {{{"x", values(numbers)}}}).take_value();
  DemandQuery query{{"y", point(5, 6)}};
  auto initial = demand.request(query);
  PS_CHECK(initial.ok() && initial.value().diagnostics.block_cache_hits == 0 &&
           initial.value().diagnostics.block_cache_misses == 6);
  numbers[0] = 1;
  PS_CHECK(demand.replace_bindings({{{"x", values(numbers)}}}).ok());
  auto changed = demand.request(query);
  PS_CHECK(changed.ok() && changed.value().diagnostics.block_cache_hits == 3 &&
           changed.value().diagnostics.block_cache_misses == 3);
  volatile double expected = 0;
  for (const auto number : numbers)
    expected = expected + number;
  double actual = 0;
  PS_CHECK(changed.value().values.at("y").read({5}, &actual, 8).ok() &&
           actual == expected);
  auto evidence = changed.value().dependencies.source_support();
  PS_CHECK(evidence.ok() &&
           evidence.value().at("x") == Footprint::all({6}).value());
  auto dirty = changed.value().dependencies.potential_dirty("x", point(0, 6));
  PS_CHECK(dirty.ok() && dirty.value().at("y") == point(5, 6));
  context.clear_result_cache();
  auto cleared = demand.request(query);
  PS_CHECK(cleared.ok() && cleared.value().diagnostics.block_cache_hits == 0 &&
           cleared.value().diagnostics.block_cache_misses == 6);
  ExecutionOptions disabled;
  disabled.maximum_dependency_cache_work = 0;
  auto plain = demand.request(query, {}, disabled);
  PS_CHECK(plain.ok() && plain.value().diagnostics.cache_hits == 0 &&
           plain.value().diagnostics.block_cache_hits == 0 &&
           plain.value().diagnostics.block_cache_misses == 0);
  PS_CHECK(plain.value().values.at("y").read({5}, &actual, 8).ok() &&
           actual == expected);
  return 0;
}
int errors_and_order() {
  auto registry = make_default_operation_registry();
  const std::vector<double> input{1e16, 1, -1e16, 4, 1, 0x1p54, 2, 3};
  for (auto block : {1, 2, 3, 64}) {
    GraphContext graph(document(input.size(), block));
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, {1, false, 8, 4096, 512});
    auto demand =
        context.open_demand(plan, {{{"x", values(input)}}}).take_value();
    for (unsigned warm = 0; warm < 2; ++warm) {
      auto result =
          demand.request({{"y", Footprint::all({input.size()}).take_value()}});
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      volatile double carry = 0;
      for (std::uint64_t i = 0; i < input.size(); ++i) {
        carry = carry + input[i];
        double value = 0, expected = carry;
        PS_CHECK(result.value().values.at("y").read({i}, &value, 8).ok());
        PS_CHECK(std::memcmp(&value, &expected, 8) == 0);
      }
    }
  }
  GraphContext graph(document(2, 64));
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096, 64});
  auto demand =
      context
          .open_demand(
              plan,
              {{{"x", values({1, std::numeric_limits<double>::infinity()})}}})
          .take_value();
  for (unsigned warm = 0; warm < 2; ++warm) {
    auto first = demand.request({{"y", point(0, 2)}});
    double output = 0;
    PS_CHECK(first.ok() &&
             first.value().values.at("y").read({0}, &output, 8).ok() &&
             output == 1);
    auto both = demand.request({{"y", Footprint::all({2}).take_value()}});
    PS_CHECK(both.status().code == ErrorCode::OperationFailed &&
             both.status().message == "nonfinite scan input 1");
    auto second = demand.request({{"y", point(1, 2)}});
    PS_CHECK(second.status().code == ErrorCode::OperationFailed &&
             second.status().message == "nonfinite scan input 1");
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(direct_and_checkpoint_guards() == 0);
  PS_CHECK(exact_reads(false) == 0);
  PS_CHECK(exact_reads(true) == 0);
  PS_CHECK(errors_and_order() == 0);
  PS_CHECK(edited_prefixes() == 0);
  PS_CHECK(block_reconvergence() == 0);
  return 0;
}
