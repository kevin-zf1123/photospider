#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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
        " reason=" + std::to_string(static_cast<int>(result.status().reason)) +
        ": " + result.status().message);
  return result.take_value();
}
struct Reference {
  std::vector<std::int64_t> labels;
  std::vector<std::array<std::int64_t, 3>> rows;
  std::vector<std::uint8_t> mask;
};
Reference bfs(const std::vector<std::uint8_t>& source, std::uint64_t width,
              std::int64_t threshold) {
  Reference r;
  r.labels.resize(source.size());
  r.mask.resize(source.size());
  // Independent queue traversal, not the product's union/find recipe. Oracle
  // buffers are ordinary caller-owned reference storage, outside the Run
  // ledger.
  std::vector<std::uint64_t> queue;
  for (std::uint64_t seed = 0; seed < source.size(); ++seed) {
    if (!source[seed] || r.labels[seed])
      continue;
    const auto id = static_cast<std::int64_t>(seed + 1);
    queue.clear();
    queue.push_back(seed);
    r.labels[seed] = id;
    for (std::size_t head = 0; head < queue.size(); ++head) {
      const auto p = queue[head], x = p % width;
      const auto add = [&](std::uint64_t neighbour) {
        if (source[neighbour] && !r.labels[neighbour]) {
          r.labels[neighbour] = id;
          queue.push_back(neighbour);
        }
      };
      if (x)
        add(p - 1);
      if (x + 1 < width)
        add(p + 1);
      if (p >= width)
        add(p - width);
      if (p + width < source.size())
        add(p + width);
    }
    r.rows.push_back({id, static_cast<std::int64_t>(queue.size()),
                      static_cast<std::int64_t>(seed)});
    for (auto p : queue)
      r.mask[p] = static_cast<std::int64_t>(queue.size()) >= threshold ? 1 : 0;
  }
  return r;
}
std::shared_ptr<RegionalSource> source(const std::vector<std::uint8_t>& pixels,
                                       std::uint64_t width, unsigned* reads,
                                       CancellationSource* cancellation,
                                       bool cancel) {
  auto result = std::make_shared<RegionalSource>();
  result->descriptor = {ElementType::UInt8, {pixels.size() / width, width}};
  result->read = [&pixels, width, reads, cancellation, cancel](
                     const Region& region, std::uint8_t* output,
                     std::uint64_t bytes, const BufferAllocator&,
                     const CancellationToken&) -> Result<Region> {
    ++*reads;
    if (cancel && *reads == 2)
      cancellation->cancel();
    const auto& r = region.dimensions();
    std::uint64_t copied = 0;
    for (auto y = r[0].offset; y < r[0].offset + r[0].extent; ++y)
      for (auto x = r[1].offset; x < r[1].offset + r[1].extent; ++x) {
        if (copied == bytes)
          return Result<Region>(
              Status{ErrorCode::InvalidArgument, "source byte range"});
        output[copied++] = pixels[y * width + x];
      }
    return copied == bytes ? Result<Region>(region)
                           : Result<Region>(Status{ErrorCode::InvalidArgument,
                                                   "source byte count"});
  };
  return result;
}
struct Sink {
  const std::vector<std::uint8_t>* expected;
  std::uint64_t row = 0, batch = 0, kept = 0;
  unsigned stage = 0;
  ResultRef input;
  ResultDescriptor descriptor;
  explicit Sink(const std::vector<std::uint8_t>* reference)
      : expected(reference) {}
  Poll poll(const ResultProgramPhase& p) {
    if (!stage) {
      stage = 1;
      return Poll(ResultProgramNeed{{}, {{0, 0, true, 0}}, {}});
    }
    if (stage == 1) {
      input = p.results.at(0);
      auto d = input.descriptor();
      if (!d.ok())
        return Poll(d.status());
      descriptor = d.take_value();
      stage = 2;
    }
    if (stage == 3) {
      const auto& page =
          std::get<std::shared_ptr<const CpuStorage>>(p.io.at(0));
      auto fuel = p.consume_work(batch);
      if (!fuel.ok())
        return Poll(fuel);
      for (std::uint64_t i = 0; i < batch; ++i) {
        const auto value = page->bytes().data()[i];
        if (value != (*expected)[row + i])
          return Poll(Status{ErrorCode::OperationFailed,
                             "independent BFS filter pixel oracle"});
        kept += value;
      }
      row += batch;
      stage = 2;
    }
    if (row < expected->size()) {
      batch =
          std::min<std::uint64_t>(expected->size() - row, p.query.page_bytes);
      auto read = input.prepare_read(descriptor, 0, row, batch);
      if (!read.ok())
        return Poll(read.status());
      stage = 3;
      return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
    }
    auto memory = MutableValue::allocate(p.query.output.descriptor,
                                         Region::whole({1}), p.allocator);
    if (!memory.ok())
      return Poll(memory.status());
    auto buffer = memory.take_value();
    const double value = static_cast<double>(kept);
    std::memcpy(buffer.data(), &value, 8);
    auto published = std::move(buffer).publish({});
    if (!published.ok())
      return Poll(published.status());
    const auto held = published.take_value();
    auto fragments = ValueFragments::create_view(
        p.query.output.descriptor, {}, *p.query.value_outputs, &held, 1);
    if (!fragments.ok())
      return Poll(fragments.status());
    auto relation =
        ResultRelation::cartesian(p.resources, 1, {0, 7, 0, expected->size()},
                                  DependencyGuarantee::Conservative);
    if (!relation.ok())
      return Poll(relation.status());
    return Poll(
        ResultValuePublication{fragments.take_value(), relation.take_value()});
  }
};
void run(const char* name, std::uint64_t h, std::uint64_t w,
         std::vector<std::uint8_t> mask, std::int64_t threshold = 2,
         std::uint64_t window = 64, std::uint64_t maximum = 1048576,
         unsigned failure = 0, std::uint64_t work = 10000000) {
  check(mask.size() == h * w, "fixture dimensions");
  const auto reference = bfs(mask, w, threshold);
  ComponentsSpec spec{h, w, maximum, ComponentIdScheme::MinPixel};
  auto registry = std::make_shared<OperationRegistry>();
  std::array<unsigned, 3> starts{};
  for (auto op : {ComponentOperation::Labels, ComponentOperation::Area,
                  ComponentOperation::Filter}) {
    auto definition = take(make_component_operation(op, spec));
    const auto index = static_cast<unsigned>(op) - 1;
    auto start = definition.start_result;
    definition.start_result = [&starts, index, start](const auto& query,
                                                      const auto& allocator) {
      ++starts[index];
      return start(query, allocator);
    };
    check(registry->register_operation(std::move(definition)).ok(),
          "register components4");
  }
  OperationDefinition sink;
  sink.key = "example.component_sink";
  auto& traits = sink.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.workspace_bytes = 4096;
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].result_schema_id = "photospider.component_filter";
  traits.input_schema[0].result_schema_version = 1;
  auto& out = traits.outputs[0];
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(Sink);
  out.maximum_dependency_stages = 1000000;
  out.output_element_type = ElementType::Float64;
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  sink.start_result = [&reference](const auto&, const auto& allocator) {
    return ResultContinuation::make<Sink>(allocator, &reference.mask);
  };
  check(registry->register_operation(std::move(sink)).ok(), "register sink");
  WorkflowDocument doc;
  doc.inputs = {{1,
                 "mask",
                 {ElementType::UInt8, {h, w}},
                 Region::whole({h, w}),
                 {0, {static_cast<std::int64_t>(w), 1}},
                 {}}};
  doc.nodes = {
      {1, "components4.labels", {WorkflowInputReference{1}}, {}},
      {2, "components4.area", {WorkflowNodeOutput{1, "value"}}, {}},
      {3,
       "components4.filter",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {{"minimum_area", threshold}}},
      {4, "example.component_sink", {WorkflowNodeOutput{3, "value"}}, {}},
      {5, "components4.labels", {WorkflowInputReference{1}}, {}}};
  std::vector<std::uint8_t> other = mask;
  if (other.size() > 1)
    other[1] = 0;
  if (failure == 3) {
    doc.inputs.push_back({2,
                          "other",
                          {ElementType::UInt8, {h, w}},
                          Region::whole({h, w}),
                          {0, {static_cast<std::int64_t>(w), 1}},
                          {}});
    doc.nodes.push_back(
        {6, "components4.labels", {WorkflowInputReference{2}}, {}});
    doc.nodes[2].inputs[0] = WorkflowNodeOutput{6, "value"};
  }
  doc.outputs = {{"a_sink", 4, "value"},
                 {"labels", 1, "value"},
                 {"area", 2, "value"},
                 {"filter", 3, "value"},
                 {"alias", 5, "value"}};
  check(registry->freeze().ok(), "freeze registry");
  GraphContext graph(doc);
  auto compiled = take(Compiler(registry).compile(graph));
  ResourceBudget root;
  unsigned reads = 0;
  Result<ExecutionResult> result(Status{ErrorCode::Internal, {}});
  {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = 65536;
    config.managed_resources->capacity[ResourceKind::Metadata] = 65536;
    if (failure == 1)
      config.managed_resources->capacity[ResourceKind::Disk] = 4096;
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    CancellationSource cancellation;
    ExecutionBindings bindings{
        {{"mask", {}, source(mask, w, &reads, &cancellation, failure == 2)}}};
    if (failure == 3)
      bindings.inputs.push_back(
          {"other", {}, source(other, w, &reads, &cancellation, false)});
    ExecutionOptions options;
    options.maximum_result_window_bytes = window;
    options.maximum_dependency_work = work;
    options.dependencies.maximum_stages = 1000000;
    if (failure == 4)
      graph.replace(doc);
    result =
        context.execute(compiled.plan, bindings, cancellation.token(), options);
  }
  if (failure || window < 32 || work < 1000 ||
      reference.rows.size() > maximum) {
    check(!result.ok(), "expected component failure");
    if (failure == 2)
      check(result.status().code == ErrorCode::Cancelled,
            "component cancellation");
    else if (failure == 3)
      check(result.status().reason == FailureReason::InvalidAssociation,
            "foreign area association");
    else if (failure == 4)
      check(result.status().code == ErrorCode::Stale && reads == 0,
            "stale before source reads");
    else if (reference.rows.size() > maximum)
      check(result.status().code == ErrorCode::OperationFailed &&
                result.status().reason == FailureReason::InvalidDomain &&
                result.status().detail.origin == FailureOrigin::Domain &&
                result.status().detail.scope == FailureScope::Group,
            "component semantic count limit");
    else
      check(result.status().code == ErrorCode::ResourceExhausted,
            "component bounded failure category");
    check(root.statistics().live[ResourceKind::Disk] == 0 &&
              root.statistics().live[ResourceKind::Payload] == 0,
          "component failed cleanup");
    std::cout << name << " rejected: " << result.status().message << '\n';
    return;
  }
  auto output = take(std::move(result));
  check(starts == std::array<unsigned, 3>{1, 1, 1},
        "cache-off component producers shared once");
  auto labels = output.results.at("labels"), area = output.results.at("area"),
       filtered = output.results.at("filter");
  check(labels.object_id() == output.results.at("alias").object_id(),
        "shared Labels identity");
  auto descriptor = take(labels.descriptor());
  check(descriptor.rows(0) == h * w &&
            descriptor.rows(1) == reference.rows.size(),
        "dynamic components count");
  for (std::uint64_t row = 0; row < h * w;) {
    const auto batch = std::min(h * w - row, window / 8);
    auto page =
        take(take(labels.prepare_read(descriptor, 0, row, batch)).load(window));
    for (std::uint64_t i = 0; i < batch; ++i) {
      std::int64_t label;
      std::memcpy(&label, page->bytes().data() + i * 8, 8);
      check(label == reference.labels[row + i], "independent BFS label basis");
    }
    row += batch;
  }
  auto index_descriptor = take(area.descriptor());
  check(index_descriptor.rows(0) == reference.rows.size(),
        "area index dynamic count");
  for (std::uint64_t row = 0; row < reference.rows.size(); ++row) {
    auto original =
        take(take(labels.prepare_read(descriptor, 1, row, 1)).load(window));
    std::array<std::int64_t, 3> actual;
    std::memcpy(actual.data(), original->bytes().data(), 24);
    check(actual == reference.rows[row],
          "independent BFS component area/minimum");
    auto index =
        take(take(area.prepare_read(index_descriptor, 0, row, 1)).load(window));
    std::array<std::int64_t, 2> pair;
    std::memcpy(pair.data(), index->bytes().data(), 16);
    check(pair[0] == actual[0] && pair[1] == actual[1], "paged area index");
  }
  check(area.association().size() == 1 &&
            area.association()[0] == labels.object_id(),
        "area source association");
  auto support = take(area.descriptor_relation());
  check(support.guarantee() == DependencyGuarantee::Conservative,
        "Conservative global support");
  check(take(support.intersects(0, {{0, 8, 0, 1}}, 64)).value_or(false),
        "empty descriptor invalidation");
  support = {};
  auto held =
      take(take(filtered.prepare_read(take(filtered.descriptor()), 0, 0, 1))
               .load(window));
  auto weak = labels.weak();
  output = {};
  labels = {};
  area = {};
  filtered = {};
  check(weak.lock().valid(),
        "filter window retains Labels and area after context");
  held.reset();
  check(!weak.lock().valid() && root.statistics().live[ResourceKind::Disk] == 0,
        "last component window releases backing");
  std::cout << name << " passed HW=" << h << 'x' << w
            << " K=" << reference.rows.size() << " window=" << window
            << " host_peak=" << root.statistics().peak[ResourceKind::Host]
            << " source_reads=" << reads << '\n';
}
}  // namespace
int main() {
  try {
    for (auto window : {32U, 64U, 256U}) {
      run("empty", 2, 3, {0, 0, 0, 0, 0, 0}, 2, window, 0);
      run("same-root", 2, 2, {1, 1, 1, 1}, 4, window, 1);
      run("no-row-wrap", 2, 3, {0, 0, 1, 1, 0, 0}, 2, window);
      run("two-roots", 2, 3, {1, 0, 1, 1, 1, 1}, 5, window);
      run("nonzero-mask", 1, 5, {0, 2, 255, 0, 7}, 2, window);
      run("root-not-minimum", 3, 7,
          {1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0}, 14,
          window, 1);
      std::vector<std::uint8_t> comb(254, 1);
      for (unsigned x = 0; x < 127; ++x)
        comb[x] = x % 2 == 0 ? 1 : 0;
      run("cross-page-comb", 2, 127, comb, 191, window, 1);
    }
    run("maximum-count", 1, 5, {1, 0, 1, 0, 1}, 1, 64, 2);
    run("foreign-index", 1, 5, {1, 1, 0, 1, 1}, 2, 64, 2, 3);
    run("small-window", 2, 3, {1, 1, 1, 1, 1, 1}, 1, 31);
    run("work", 2, 3, {1, 1, 1, 1, 1, 1}, 1, 64, 1, 0, 10);
    run("disk-after-UF", 2, 3, {1, 1, 1, 1, 1, 1}, 1, 64, 1, 1);
    run("cancel-after-write", 2, 3, {1, 1, 1, 1, 1, 1}, 1, 32, 1, 2);
    run("stale", 2, 3, {1, 1, 1, 1, 1, 1}, 1, 64, 1, 4);
    run("int64-threshold", 1, 3, {1, 1, 1}, INT64_MAX, 64, 1);
    std::vector<std::uint8_t> isolated(32 * 33);
    for (unsigned y = 0; y < 32; ++y)
      for (unsigned x = 0; x < 33; ++x)
        isolated[y * 33 + x] = (x + y) % 2 == 0;
    run("paged-area-index", 32, 33, isolated, 1, 256);
    run("large-connected", 100, 100, std::vector<std::uint8_t>(10000, 1), 10000,
        256, 1);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
