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

#include "../shared/result_source.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Poll = Result<ResultProgramPoll>;
using Failure = example_result::Failure;
void require(Status status) {
  if (!status.ok())
    throw Failure(std::move(status));
}
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw Failure(result.status());
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
SchemaTemplate tensor_schema(ElementType type,
                             std::vector<std::uint64_t> shape) {
  SchemaTemplate result;
  result.id = "photospider.tensor";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {type, std::move(shape)};
  result.tensors.push_back(std::move(tensor));
  return result;
}
OperationOutputTraits tensor_output(const SchemaTemplate& schema,
                                    std::uint64_t state_bytes,
                                    std::uint32_t stages) {
  OperationOutputTraits output;
  output.key = "value";
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = std::string(schema.id);
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = schema;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 2;
  output.continuation_bytes = state_bytes;
  output.maximum_dependency_stages = stages;
  return output;
}
ExecutionBindings bindings(const ResourceBudget& root,
                           const StatisticsSpec& spec, unsigned variant) {
  auto pixels = example_result::input(
      root, "pixels",
      tensor_schema(ElementType::Int64, {spec.height, spec.width}),
      [variant](std::uint64_t i, std::uint8_t* bytes) {
        const auto value = sample(i, variant);
        std::memcpy(bytes, &value, 8);
      },
      variant == 12);
  auto mask = example_result::input(
      root, "mask",
      tensor_schema(ElementType::UInt8, {spec.height, spec.width}),
      [variant](std::uint64_t i, std::uint8_t* bytes) {
        *bytes = selected(i, variant) ? 255 : 0;
      },
      variant == 12, variant == 13);
  return {{std::move(pixels), std::move(mask)}};
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
  Poll poll(const ResultProgramPhase& p) try {
    if (stage == 0) {
      batch = std::min(count - row,
                       std::min<std::uint64_t>(4096, p.query.page_bytes) / 8);
      if (!batch)
        return Poll(Status{ErrorCode::ResourceExhausted, "sink window"});
      stage = 1;
      return Poll(ResultProgramNeed{{{0, 0, false, row + batch}}, {}});
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
      return Poll(ResultProgramNeed{{}, {plan.take_value()}});
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
    auto builder = take(ResultBuilder::start(
        p.resources, *p.query.output.result_schema, p.query.semantic_key, {},
        std::vector<std::uint64_t>(p.association->begin(),
                                   p.association->end())));
    require(builder.bind_descriptor_relation(take(ResultRelation::cartesian(
        p.resources, 1, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))));
    const auto value = static_cast<double>(count);
    auto relation = take(ResultRelation::cartesian(
        p.resources, 1, {0, 7, 0, count, ResultSupportTarget::Field, 0},
        DependencyGuarantee::Conservative));
    require(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&value), 8}, relation,
        {true, true, true, true}));
    return Poll(ResultPublication{take(builder.seal()), true});
  } catch (const Failure& error) {
    return Poll(error.status);
  }
};
void run(std::uint64_t n, std::uint64_t window, unsigned variant = 0,
         bool grade = true, std::uint64_t work = 10000000,
         std::uint64_t host = 1048576, std::uint64_t height = 1,
         std::uint64_t disk = UINT64_MAX, std::uint32_t stages = 100000,
         bool stage_exhausted = false) {
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
  traits.outputs = {tensor_output(tensor_schema(ElementType::Float64, {1}),
                                  sizeof(Sink), 1000000)};
  const long double mean = count ? static_cast<long double>(total) / count : 0;
  sink.start_result = [n, variant, mean, &streamed](const auto&,
                                                    const auto& allocator) {
    return ResultContinuation::make<Sink>(allocator, n, variant, mean,
                                          &streamed);
  };
  check(registry->register_operation(std::move(sink)).ok(), "register sink");
  WorkflowDocument doc;
  doc.inputs = {
      example_result::declaration(
          1, "pixels",
          tensor_schema(ElementType::Int64, {spec.height, spec.width})),
      example_result::declaration(
          2, "mask",
          tensor_schema(ElementType::UInt8, {spec.height, spec.width}))};
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
    config.managed_resources->capacity[ResourceKind::Payload] = 32768;
    config.managed_resources->capacity[ResourceKind::Referenced] = 18 * n;
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    ExecutionOptions options;
    options.maximum_result_window_bytes = window;
    options.maximum_dependency_work = work;
    options.dependencies.maximum_stages = stages;
    CancellationSource cancellation;
    if (variant == 9) {
      options.result_publication = [&](ValueRef output, const ResultRef&) {
        if (output.node_id == 1)
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
      prior = take(context.execute(prior_plan.plan, bindings(root, spec, 0), {},
                                   options));
    }
    try {
      result = context.execute(compiled.plan, bindings(root, spec, variant),
                               cancellation.token(), options);
    } catch (const Failure& failure) {
      result = Result<ExecutionResult>(failure.status);
    }
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
  const bool expected_failure = variant == 4 ||
                                (grade && (variant == 2 || variant == 3)) ||
                                window < 24 || work < 1000 || host < 65536 ||
                                disk < 8192 || variant == 9 || stage_exhausted;
  if (expected_failure) {
    check(!result.ok(), "expected domain/resource failure");
    if (stage_exhausted)
      check(result.status().code == ErrorCode::ResourceExhausted &&
                result.status().message == "structured stage limit",
            "histogram still needs a final consumption/publication poll");
    else if (variant == 9)
      check(result.status().code == ErrorCode::Cancelled,
            "cancel after histogram publication");
    else if (window < 24 || work < 1000 || host < 65536 || disk < 8192)
      check(result.status().code == ErrorCode::ResourceExhausted,
            "bounded failure category");
    else
      check(result.status().reason == FailureReason::InvalidDomain,
            "domain reason");
    check(root.statistics().live[ResourceKind::Disk] == 0 &&
              root.statistics().live[ResourceKind::Payload] == 0 &&
              root.statistics().live[ResourceKind::Referenced] == 0,
          "failed workflow releases backing and payload");
    std::cout << "rejected n=" << n << " variant=" << variant
              << " window=" << window << ": " << result.status().message
              << '\n';
    return;
  }
  if (!result.ok()) {
    const auto stats = root.statistics();
    std::cerr << "statistics run n=" << n << " variant=" << variant
              << " host_peak=" << stats.peak[ResourceKind::Host]
              << " metadata_peak=" << stats.peak[ResourceKind::Metadata]
              << " payload_peak=" << stats.peak[ResourceKind::Payload]
              << " Entries=" << stats.peak[ResourceKind::Entries]
              << " stages=" << stats.issued.stages << '\n';
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
  check(take(witness.intersects(
                 0, {{0, 8, 0, 1, ResultSupportTarget::Descriptor, 0}}, 64))
            .value_or(false),
        "empty descriptor invalidation");
  witness = {};
  if (grade)
    check(n <= window / 8 || streamed, "sink must consume active prefixes");
  auto weak = hist.weak();
  auto weak_parameters = params.weak();
  const auto histogram_id = hist.object_id();
  output = {};
  const auto histogram_disk = root.statistics().live[ResourceKind::Disk];
  hist = {};
  if (!reference.empty())
    check(root.statistics().live[ResourceKind::Disk] < histogram_disk,
          "retiring copied histogram releases its backing");
  const auto association = params.association();
  check(!weak.lock().valid() &&
            std::find(association.begin(), association.end(), histogram_id) !=
                association.end(),
        "copied parameters retain histogram facts without source payload");
  params = {};
  check(!weak_parameters.lock().valid(),
        "captured Result view retires independently of loaded field windows");
  check(root.statistics().live[ResourceKind::Disk] > 0,
        "field windows retain mandatory backing");
  std::array<std::int64_t, 3> retained;
  std::memcpy(retained.data(), record->bytes().data(), 24);
  double retained_mean = 0;
  std::memcpy(&retained_mean, mean_page->bytes().data(), 8);
  check(retained == actual && retained_mean == actual_mean,
        "field windows survive result and context retirement");
  record.reset();
  mean_page.reset();
  check(root.statistics().live[ResourceKind::Disk] == 0,
        "last window releases mandatory disk");
  check(root.statistics().live[ResourceKind::Referenced] == 0,
        "statistics input backing released");
  check(root.statistics().peak[ResourceKind::Host] <= host,
        "managed host capacity");
  check(root.statistics().peak[ResourceKind::Payload] <= 32768,
        "statistics working payload capacity");
  std::cout << "passed n=" << n << " variant=" << variant
            << " window=" << window << " bins=" << reference.size()
            << " count=" << count << " total=" << total
            << " host_peak=" << root.statistics().peak[ResourceKind::Host]
            << " payload_peak=" << root.statistics().peak[ResourceKind::Payload]
            << " referenced_peak="
            << root.statistics().peak[ResourceKind::Referenced]
            << " issued_stages=" << root.statistics().issued.stages << '\n';
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const auto rejected = make_statistics_operation(
        StatisticsOperation::Histogram, {2048, 2048, 65536});
    check(
        !rejected.ok() &&
            rejected.status().code == ErrorCode::ResourceExhausted &&
            rejected.status().message ==
                "histogram required source stages exceed operation stage limit",
        "reject impossible source-stage profile before source binding");
    std::cout << "rejected 2048x2048 bins=65536 before source binding: "
              << rejected.status().message << '\n';
    // Empty 2x513/B513: two strips per row, two passes, then one final poll.
    run(1026, 24, 11, false, 10000000, 1048576, 2, UINT64_MAX, 9);
    run(1026, 24, 11, false, 10000000, 1048576, 2, UINT64_MAX, 8, true);
    if (argc == 2 && std::string(argv[1]) == "--stage-admission")
      return 0;
    if (argc == 2 && std::string(argv[1]) == "--large") {
      run(40000, 24, 8, true, 1000000000, 4194304, 200, 1ULL << 30, 1000000);
      return 0;
    }
    run(1000, 24, 8, true, 10000000, 1048576, 25, 1ULL << 30, 5000);
    if (argc == 2 && std::string(argv[1]) == "--stage-regression")
      return 0;
    check(argc == 1,
          "usage: photospider_statistics_workflow "
          "[--large|--stage-regression|--stage-admission]");
    run(4096, 24);
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
    // Retain the first Run's owned results while executing the next snapshot.
    // Admit both metadata sets; single-Run source DAGs retain a 1 MiB cap.
    run(17, 64, 6, true, 10000000, 2097152);
    run(17, 16);
    run(17, 64, 0, true, 10);
    run(17, 64, 0, true, 10000000, 1024);
    run(65537, 256, 0, true, 100000000);
    run(1003, 64, 7);
    run(1008, 64, 0, true, 10000000, 1048576, 3);
    run(17, 64, 0, true, 10000000, 1048576, 1, 4096);
    run(17, 64, 9);
    run(129, 64, 8);
    run(17, 64, 10);
    run(17, 64, 11, false);
    run(17, 64, 12);
    run(17, 64, 13);
    return 0;
  } catch (const Failure& error) {
    std::cerr << "statistics failure code="
              << static_cast<int>(error.status.code)
              << " reason=" << static_cast<int>(error.status.reason) << " "
              << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
