#include <algorithm>
#include <array>
#include <cmath>
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

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Poll = Result<ResultProgramPoll>;
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(
        "code=" + std::to_string(static_cast<int>(result.status().code)) +
        ": " + result.status().message);
  return result.take_value();
}
std::int64_t sample(std::uint64_t i, unsigned variant) {
  if (variant == 10)
    return i == 0 ? 512 : static_cast<std::int64_t>(i % 8);
  if (variant == 8)
    return static_cast<std::int64_t>((i * 509) % 65536);
  if (variant == 7)
    return static_cast<std::int64_t>(i % 257);
  if (variant == 3)
    return 0;
  if ((variant == 4 || variant == 5) && i == 0)
    return -1;
  return static_cast<std::int64_t>((3 * i + 1 + (variant == 6 ? 1 : 0)) % 8);
}
bool selected(std::uint64_t i, unsigned variant) {
  return variant != 2 && variant != 11 && (variant != 1 || i % 3 != 0) &&
         (variant != 5 || i != 0);
}
std::shared_ptr<RegionalSource> source(StatisticsSpec spec, bool mask,
                                       unsigned variant) {
  auto result = std::make_shared<RegionalSource>();
  result->descriptor = {mask ? ElementType::UInt8 : ElementType::Int64,
                        {spec.height, spec.width}};
  result->read = [spec, mask, variant](
                     const Region& region, std::uint8_t* output,
                     std::uint64_t bytes, const BufferAllocator&,
                     const CancellationToken&) -> Result<Region> {
    const auto& r = region.dimensions();
    std::uint64_t offset = 0;
    for (auto y = r[0].offset; y < r[0].offset + r[0].extent; ++y)
      for (auto x = r[1].offset; x < r[1].offset + r[1].extent; ++x) {
        const auto index = y * spec.width + x;
        const auto width = mask ? 1U : 8U;
        if (offset + width > bytes)
          return Result<Region>(
              Status{ErrorCode::InvalidArgument, "source bounds"});
        if (mask) {
          output[offset] = selected(index, variant) ? 255 : 0;
        } else {
          const auto value = sample(index, variant);
          std::memcpy(output + offset, &value, 8);
        }
        offset += width;
      }
    return offset == bytes ? Result<Region>(region)
                           : Result<Region>(Status{ErrorCode::InvalidArgument,
                                                   "source byte count"});
  };
  return result;
}
// A downstream DAG sink requests successive certified prefixes while grade is
// active. Its independent reference uses small exact integer count/total.
struct Sink {
  std::uint64_t count, row = 0, batch = 0;
  unsigned variant, stage = 0;
  long double mean;
  bool* streamed;
  ResultRef input;
  Sink(std::uint64_t n, unsigned v, long double average, bool* observed)
      : count(n), variant(v), mean(average), streamed(observed) {}
  Poll poll(const ResultProgramPhase& p) {
    if (stage == 0) {
      batch = std::min(count - row,
                       std::min<std::uint64_t>(4096, p.query.page_bytes) / 8);
      if (!batch)
        return Poll(Status{ErrorCode::ResourceExhausted, "sink window"});
      stage = 1;
      return Poll(ResultProgramNeed{{}, {{0, 0, false, row + batch}}, {}});
    }
    if (stage == 1) {
      input = p.results.at(0);
      auto descriptor = input.descriptor(false);
      if (!descriptor.ok())
        return Poll(descriptor.status());
      if (descriptor.value().rows(0) < count)
        *streamed = true;
      auto plan = input.prepare_read(descriptor.value(), 0, row, batch);
      if (!plan.ok())
        return Poll(plan.status());
      stage = 2;
      return Poll(ResultProgramNeed{{}, {}, {plan.take_value()}});
    }
    auto fuel = p.consume_work(batch);
    if (!fuel.ok())
      return Poll(fuel);
    const auto& page = std::get<std::shared_ptr<const CpuStorage>>(p.io.at(0));
    for (std::uint64_t i = 0; i < batch; ++i) {
      double value;
      std::memcpy(&value, page->bytes().data() + i * 8, 8);
      const auto expected = 2.0L * sample(row + i, variant) / mean;
      if (std::abs(static_cast<long double>(value) - expected) > 1e-12L)
        return Poll(Status{ErrorCode::OperationFailed,
                           "independent graded pixel oracle"});
    }
    row += batch;
    if (row != count) {
      stage = 0;
      return poll(p);
    }
    auto memory = MutableValue::allocate(p.query.output.descriptor,
                                         Region::whole({1}), p.allocator);
    if (!memory.ok())
      return Poll(memory.status());
    auto output = memory.take_value();
    const auto value = static_cast<double>(count);
    std::memcpy(output.data(), &value, 8);
    auto published = std::move(output).publish({});
    if (!published.ok())
      return Poll(published.status());
    const auto held = published.take_value();
    auto fragments = ValueFragments::create_view(
        p.query.output.descriptor, {}, *p.query.value_outputs, &held, 1);
    if (!fragments.ok())
      return Poll(fragments.status());
    auto relation = ResultRelation::cartesian(
        p.resources, 1, {0, 7, 0, count}, DependencyGuarantee::Conservative);
    if (!relation.ok())
      return Poll(relation.status());
    return Poll(
        ResultValuePublication{fragments.take_value(), relation.take_value()});
  }
};
void run(std::uint64_t n, std::uint64_t window, unsigned variant = 0,
         bool grade = true, std::uint64_t work = 10000000,
         std::uint64_t host = 65536, std::uint64_t height = 1,
         std::uint64_t disk = UINT64_MAX) {
  const StatisticsSpec spec{height, n / height,
                            variant == 10 || variant == 11 ? 513U
                            : variant == 8                 ? 65536U
                            : variant == 7                 ? 257U
                                                           : 8U};
  std::map<std::int64_t, std::int64_t> reference;
  std::int64_t total = 0, count = 0;
  for (std::uint64_t i = 0; i < n; ++i)
    if (selected(i, variant)) {
      ++reference[sample(i, variant)];
      total += sample(i, variant);
      ++count;
    }
  bool streamed = false;
  std::array<unsigned, 3> starts{};
  auto registry = std::make_shared<OperationRegistry>();
  for (auto op :
       {StatisticsOperation::Histogram, StatisticsOperation::Parameters,
        StatisticsOperation::Grade}) {
    auto definition = take(make_statistics_operation(op, spec));
    auto start = definition.start_result;
    const auto index = static_cast<unsigned>(op) - 1;
    definition.start_result = [start, index, &starts](const auto& query,
                                                      const auto& allocator) {
      ++starts[index];
      return start(query, allocator);
    };
    check(registry->register_operation(std::move(definition)).ok(),
          "register statistics");
  }
  OperationDefinition sink;
  sink.key = "example.statistics_sink";
  auto& traits = sink.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.workspace_bytes = 4096;
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].result_schema_id = "photospider.graded_scalar";
  traits.input_schema[0].result_schema_version = 1;
  auto& out = traits.outputs[0];
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(Sink);
  out.maximum_dependency_stages = 1000000;
  out.output_element_type = ElementType::Float64;
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  const long double mean = count ? static_cast<long double>(total) / count : 0;
  sink.start_result = [n, variant, mean, &streamed](const auto&,
                                                    const auto& allocator) {
    return ResultContinuation::make<Sink>(allocator, n, variant, mean,
                                          &streamed);
  };
  check(registry->register_operation(std::move(sink)).ok(), "register sink");
  WorkflowDocument doc;
  doc.inputs = {{1,
                 "pixels",
                 {ElementType::Int64, {spec.height, spec.width}},
                 Region::whole({spec.height, spec.width}),
                 {0, {static_cast<std::int64_t>(spec.width * 8), 8}},
                 {}},
                {2,
                 "mask",
                 {ElementType::UInt8, {spec.height, spec.width}},
                 Region::whole({spec.height, spec.width}),
                 {0, {static_cast<std::int64_t>(spec.width), 1}},
                 {}}};
  doc.nodes = {
      {1,
       "statistics.histogram",
       {WorkflowInputReference{1}, WorkflowInputReference{2}},
       {}},
      {2, "statistics.parameters", {WorkflowNodeOutput{1, "value"}}, {}},
      {5,
       "statistics.histogram",
       {WorkflowInputReference{1}, WorkflowInputReference{2}},
       {}}};
  if (grade) {
    doc.nodes.push_back(
        {3,
         "statistics.grade",
         {WorkflowInputReference{1}, WorkflowNodeOutput{2, "value"}},
         {{"target", 2.0}}});
    doc.nodes.push_back(
        {4, "example.statistics_sink", {WorkflowNodeOutput{3, "value"}}, {}});
    doc.outputs = {{"a_sink", 4, "value"}, {"graded", 3, "value"}};
  }
  doc.outputs.push_back({"histogram", 1, "value"});
  doc.outputs.push_back({"parameters", 2, "value"});
  doc.outputs.push_back({"alias", 5, "value"});
  check(registry->freeze().ok(), "freeze");
  GraphContext graph(doc);
  auto compiled = take(Compiler(registry).compile(graph));
  ResourceBudget root;
  Result<ExecutionResult> result(Status{ErrorCode::Internal, {}});
  {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = host;
    config.managed_resources->capacity[ResourceKind::Metadata] = host;
    config.managed_resources->capacity[ResourceKind::Disk] = disk;
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    ExecutionOptions options;
    options.maximum_result_window_bytes = window;
    options.maximum_dependency_work = work;
    options.dependencies.maximum_stages = 100000;
    CancellationSource cancellation;
    if (variant == 9) {
      options.result_publication = [&](ValueRef, const ResultRef&) {
        cancellation.cancel();
        return Status::success();
      };
    }
    ExecutionResult prior;
    if (variant == 6) {
      auto prior_doc = doc;
      prior_doc.outputs = {{"parameters", 2, "value"}};
      GraphContext prior_graph(prior_doc);
      auto prior_plan = take(Compiler(registry).compile(prior_graph));
      prior = take(context.execute(prior_plan.plan,
                                   {{{"pixels", {}, source(spec, false, 0)},
                                     {"mask", {}, source(spec, true, 0)}}},
                                   {}, options));
    }
    result = context.execute(compiled.plan,
                             {{{"pixels", {}, source(spec, false, variant)},
                               {"mask", {}, source(spec, true, variant)}}},
                             cancellation.token(), options);
    if (variant == 6 && result.ok()) {
      check(prior.results.at("parameters").object_id() !=
                result.value().results.at("parameters").object_id(),
            "different Run input snapshots must not alias global results");
      auto old = prior.results.at("parameters");
      auto page = take(
          take(old.prepare_read(take(old.descriptor()), 0, 0, 1)).load(window));
      std::array<std::int64_t, 3> counts;
      std::memcpy(counts.data(), page->bytes().data(), 24);
      check(counts[1] == 57 && total == 58,
            "retained old snapshot and new snapshot totals");
    }
  }
  const bool expected_failure =
      variant == 4 || (grade && (variant == 2 || variant == 3)) ||
      window < 24 || work < 1000 || host < 65536 || disk < 8192 || variant == 9;
  if (expected_failure) {
    check(!result.ok(), "expected domain/resource failure");
    if (variant == 9)
      check(result.status().code == ErrorCode::Cancelled,
            "cancel after histogram publication");
    else if (window < 24 || work < 1000 || host < 65536 || disk < 8192)
      check(result.status().code == ErrorCode::ResourceExhausted,
            "bounded failure category");
    else
      check(result.status().reason == FailureReason::InvalidDomain,
            "domain reason");
    check(root.statistics().live[ResourceKind::Disk] == 0 &&
              root.statistics().live[ResourceKind::Payload] == 0,
          "failed workflow releases backing and payload");
    std::cout << "rejected n=" << n << " variant=" << variant
              << " window=" << window << ": " << result.status().message
              << '\n';
    return;
  }
  auto output = take(std::move(result));
  check(starts[0] == (variant == 6 ? 2U : 1U) &&
            starts[1] == (variant == 6 ? 2U : 1U) &&
            starts[2] == (grade ? 1U : 0U),
        "cache-off shared producer count");
  auto hist = output.results.at("histogram"),
       params = output.results.at("parameters");
  check(hist.object_id() == output.results.at("alias").object_id(),
        "histogram alias owner");
  auto descriptor = take(hist.descriptor());
  check(descriptor.rows(0) == reference.size() &&
            descriptor.rows(1) == reference.size(),
        "dynamic nonzero bins");
  std::uint64_t index = 0;
  for (auto expected : reference) {
    std::int64_t id, value;
    auto a =
        take(take(hist.prepare_read(descriptor, 0, index, 1)).load(window));
    auto b =
        take(take(hist.prepare_read(descriptor, 1, index++, 1)).load(window));
    std::memcpy(&id, a->bytes().data(), 8);
    std::memcpy(&value, b->bytes().data(), 8);
    check(id == expected.first && value == expected.second,
          "independent sparse histogram map");
  }
  auto fact = take(params.descriptor());
  auto record = take(take(params.prepare_read(fact, 0, 0, 1)).load(window));
  std::array<std::int64_t, 3> actual;
  std::memcpy(actual.data(), record->bytes().data(), 24);
  check(actual == std::array<std::int64_t, 3>{count, total, count ? 1 : 0},
        "integer global parameters");
  auto mean_page = take(take(params.prepare_read(fact, 1, 0, 1)).load(window));
  double actual_mean;
  std::memcpy(&actual_mean, mean_page->bytes().data(), 8);
  check(std::abs(static_cast<long double>(actual_mean) - mean) <
            1e-15L * std::max(1.0L, std::abs(mean)),
        "mean reference");
  auto witness = take(params.descriptor_relation());
  check(witness.guarantee() == DependencyGuarantee::Conservative,
        "support must remain Conservative");
  check(take(witness.intersects(0, {{0, 8, 0, 1}}, 64)).value_or(false),
        "empty descriptor invalidation");
  witness = {};
  if (grade)
    check(n <= window / 8 || streamed, "sink must consume active prefixes");
  auto weak = hist.weak();
  output = {};
  hist = {};
  check(weak.lock().valid(),
        "parameters own histogram after context and outputs");
  params = {};
  check(weak.lock().valid(), "parameter windows retain histogram association");
  record.reset();
  mean_page.reset();
  check(!weak.lock().valid(), "last result/window owner retires histogram");
  check(root.statistics().live[ResourceKind::Disk] == 0,
        "last window releases mandatory disk");
  check(root.statistics().peak[ResourceKind::Host] <= host,
        "managed host capacity");
  std::cout << "passed n=" << n << " variant=" << variant
            << " window=" << window << " bins=" << reference.size()
            << " count=" << count << " total=" << total
            << " host_peak=" << root.statistics().peak[ResourceKind::Host]
            << '\n';
}
}  // namespace
int main() {
  try {
    for (auto n : {1U, 17U, 1003U})
      for (auto window : {64U, 256U, 4096U})
        run(n, window);
    run(1003, 64, 1);
    run(17, 64, 2, false);
    run(17, 64, 3, false);
    run(17, 64, 2);
    run(17, 64, 3);
    run(17, 64, 4);
    run(17, 64, 5);
    run(17, 64, 6);
    run(17, 16);
    run(17, 64, 0, true, 10);
    run(17, 64, 0, true, 10000000, 1024);
    run(65537, 256);
    run(1003, 64, 7);
    run(1008, 64, 0, true, 10000000, 65536, 3);
    run(17, 64, 0, true, 10000000, 65536, 1, 4096);
    run(17, 64, 9);
    run(129, 64, 8);
    run(17, 64, 10);
    run(17, 64, 11, false);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
