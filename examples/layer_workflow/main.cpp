#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Poll = Result<ResultProgramPoll>;
void check(bool success, const char* message) {
  if (!success)
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
struct Import {
  ResultBuilder builder;
  std::uint64_t count = 0, row = 0, batch = 0;
  unsigned stage = 0;
  Poll poll(const ResultProgramPhase& p) {
    if (!stage) {
      stage = 1;
      auto requested = Footprint::from_regions(
          p.query.inputs[0].descriptor.shape, {Region({{0, 8}})});
      return requested.ok()
                 ? Poll(
                       ResultProgramNeed{{{0, requested.take_value()}}, {}, {}})
                 : Poll(requested.status());
    }
    if (stage == 1) {
      for (unsigned i = 0; i < 8; ++i) {
        std::uint8_t byte = 0;
        auto s = p.read(0, {i}, &byte, 1);
        if (!s.ok())
          return Poll(s);
        count |= static_cast<std::uint64_t>(byte) << (8 * i);
      }
      if (count > 1048576 ||
          p.query.inputs[0].descriptor.shape[0] != 8 + count * 64)
        return Poll(Status{ErrorCode::TypeMismatch,
                           "contribution wire count mismatch"});
      auto started = ResultBuilder::start(
          p.resources, *p.query.output.result_schema, p.query.semantic_key);
      if (!started.ok())
        return Poll(started.status());
      builder = started.take_value();
      auto support =
          ResultRelation::cartesian(p.resources, 1, {0, 15, 0, 8 + count * 64},
                                    DependencyGuarantee::Conservative);
      if (!support.ok())
        return Poll(support.status());
      auto bound = builder.bind_descriptor_relation(support.take_value());
      if (!bound.ok())
        return Poll(bound);
      stage = 2;
    }
    if (stage == 3) {
      auto memory = p.allocator.allocate(batch * 64);
      if (!memory.ok())
        return Poll(memory.status());
      auto buffer = memory.take_value();
      for (std::uint64_t i = 0; i < batch * 64; ++i) {
        auto s = p.consume_work(1);
        if (!s.ok())
          return Poll(s);
        s = p.read(0, {8 + row * 64 + i}, buffer.data() + i, 1);
        if (!s.ok())
          return Poll(s);
      }
      auto write = builder.prepare_append(0, batch, std::move(buffer).freeze());
      if (!write.ok())
        return Poll(write.status());
      row += batch;
      stage = 2;
      return Poll(ResultProgramNeed{{}, {}, {write.take_value()}});
    }
    if (row < count) {
      batch = std::min(count - row, p.query.page_bytes / 64);
      if (!batch)
        return Poll(Status{ErrorCode::ResourceExhausted,
                           "one contribution does not fit a page"});
      auto requested =
          Footprint::from_regions(p.query.inputs[0].descriptor.shape,
                                  {Region({{8 + row * 64, batch * 64}})});
      if (!requested.ok())
        return Poll(requested.status());
      stage = 3;
      return Poll(ResultProgramNeed{{{0, requested.take_value()}}, {}, {}});
    }
    auto support = ResultRelation::cartesian(p.resources, count,
                                             {0, 15, 0, 8 + count * 64},
                                             DependencyGuarantee::Conservative);
    if (!support.ok())
      return Poll(support.status());
    auto published = builder.publish(0, count, support.take_value(),
                                     {true, true, true, true});
    if (!published.ok())
      return Poll(published);
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
void add(OperationRegistry* registry, LayerOperation operation,
         LayerSpec spec = {}) {
  auto definition = take(make_layer_operation(operation, spec));
  const auto name = definition.key;
  auto status = registry->register_operation(std::move(definition));
  if (!status.ok())
    throw std::runtime_error(name + ": " + status.message);
}
std::shared_ptr<RegionalSource> bytes_source(
    const std::vector<std::uint8_t>& wire) {
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = {ElementType::UInt8, {wire.size()}};
  source->read = [&wire](const Region& region, std::uint8_t* out,
                         std::uint64_t count, const BufferAllocator&,
                         const CancellationToken&) -> Result<Region> {
    const auto span = region.dimensions()[0];
    if (span.extent != count || span.offset > wire.size() ||
        count > wire.size() - span.offset)
      return Result<Region>(
          Status{ErrorCode::InvalidArgument, "wire source bounds"});
    std::memcpy(out, wire.data() + span.offset, count);
    return Result<Region>(region);
  };
  return source;
}
std::shared_ptr<RegionalSource> float_source(
    const WorkflowInputDeclaration& declaration,
    const std::vector<float>& values) {
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = declaration.descriptor;
  source->facets = declaration.facets;
  const auto shape = declaration.descriptor.shape;
  source->read = [&values, shape](const Region& region, std::uint8_t* out,
                                  std::uint64_t bytes, const BufferAllocator&,
                                  const CancellationToken&) -> Result<Region> {
    std::uint64_t offset = 0;
    const auto& r = region.dimensions();
    for (auto y = r[0].offset; y < r[0].offset + r[0].extent; ++y)
      for (auto x = r[1].offset; x < r[1].offset + r[1].extent; ++x)
        for (auto c = r[2].offset; c < r[2].offset + r[2].extent; ++c) {
          if (offset + 4 > bytes)
            return Result<Region>(
                Status{ErrorCode::InvalidArgument, "float source bounds"});
          std::memcpy(out + offset, &values[(y * shape[1] + x) * shape[2] + c],
                      4);
          offset += 4;
        }
    return offset == bytes ? Result<Region>(region)
                           : Result<Region>(Status{ErrorCode::InvalidArgument,
                                                   "float source count"});
  };
  return source;
}
Result<ExecutionResult> execute(
    const std::shared_ptr<OperationRegistry>& registry,
    const WorkflowDocument& document, ExecutionBindings bindings,
    ResourceBudget* resources, std::uint64_t window,
    std::uint64_t work = 10000000, std::uint64_t host = 262144,
    std::uint64_t* publications = nullptr) {
  check(registry->freeze().ok(), "freeze registry");
  GraphContext graph(document);
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] = host;
  config.managed_resources->capacity[ResourceKind::Metadata] = host;
  ExecutionContext context(registry, config);
  *resources = take(context.resource_budget());
  ExecutionOptions options;
  options.maximum_result_window_bytes = window;
  options.maximum_dependency_work = work;
  options.dependencies.maximum_stages = 100000;
  if (publications) {
    options.result_publication = [publications](ValueRef, const ResultRef&) {
      ++*publications;
      return Status::success();
    };
  }
  return context.execute(compiled.plan, bindings, {}, options);
}
std::array<float, 4> coverage(const ResultRef& result, unsigned field = 0) {
  auto page =
      take(take(result.prepare_read(take(result.descriptor()), field, 0, 1))
               .load(64));
  std::array<float, 4> data{};
  std::memcpy(data.data(), page->bytes().data(), 16);
  return data;
}
void raster() {
  auto registry = std::make_shared<OperationRegistry>();
  for (auto op :
       {LayerOperation::Assemble, LayerOperation::Opacity, LayerOperation::Over,
        LayerOperation::EmitFront, LayerOperation::EmitBehind,
        LayerOperation::Flatten, LayerOperation::Response,
        LayerOperation::ResponseOver, LayerOperation::CoverageRawPlus,
        LayerOperation::RawChecked, LayerOperation::RawCapped,
        LayerOperation::Weight, LayerOperation::Reduce,
        LayerOperation::Finalize, LayerOperation::RequireValid})
    add(registry.get(), op);
  const auto facet = take(encode_semantic(rgba_semantics()));
  WorkflowDocument doc;
  doc.inputs = {{1,
                 "coverage",
                 {ElementType::Float32, {1, 1, 4}},
                 Region::whole({1, 1, 4}),
                 {0, {16, 16, 4}},
                 {facet}},
                {2,
                 "emission",
                 {ElementType::Float32, {1, 1, 3}},
                 Region::whole({1, 1, 3}),
                 {0, {12, 12, 4}},
                 {}},
                {3,
                 "background",
                 {ElementType::Float32, {1, 1, 3}},
                 Region::whole({1, 1, 3}),
                 {0, {12, 12, 4}},
                 {}}};
  doc.nodes = {
      {1,
       "layer.assemble",
       {WorkflowInputReference{1}, WorkflowInputReference{2}},
       {}},
      {2, "layer.opacity", {WorkflowNodeOutput{1, "value"}}, {{"factor", .5}}},
      {3,
       "layer.over",
       {WorkflowNodeOutput{2, "value"}, WorkflowNodeOutput{1, "value"}},
       {}},
      {4,
       "layer.emit_front",
       {WorkflowNodeOutput{3, "value"}, WorkflowInputReference{2}},
       {{"factor", 2.0}}},
      {5,
       "layer.emit_behind",
       {WorkflowNodeOutput{3, "value"}, WorkflowInputReference{2}},
       {{"factor", 2.0}}},
      {6,
       "layer.flatten",
       {WorkflowNodeOutput{3, "value"}, WorkflowInputReference{3}},
       {}},
      {7, "layer.response", {WorkflowNodeOutput{3, "value"}}, {}},
      {8,
       "layer.response_over",
       {WorkflowNodeOutput{7, "value"}, WorkflowNodeOutput{7, "value"}},
       {}},
      {9,
       "layer.coverage_raw_plus",
       {WorkflowNodeOutput{3, "value"}, WorkflowNodeOutput{3, "value"}},
       {}},
      {10, "layer.raw_capped", {WorkflowNodeOutput{9, "value"}}, {}},
      {11, "layer.weight", {WorkflowNodeOutput{3, "value"}}, {{"weight", 2.0}}},
      {12, "layer.weighted_reduce", {WorkflowNodeOutput{11, "value"}}, {}},
      {13, "layer.weighted_finalize", {WorkflowNodeOutput{12, "value"}}, {}},
      {14, "layer.require_valid", {WorkflowNodeOutput{13, "value"}}, {}}};
  doc.outputs = {{"layer", 3, "value"},    {"front", 4, "value"},
                 {"behind", 5, "value"},   {"image", 6, "value"},
                 {"response", 8, "value"}, {"capped", 10, "value"},
                 {"mean", 14, "value"}};
  std::vector<float> rgba{2, 0, -1, .5F}, emission{4, -2, 1},
      background{8, 4, 2};
  auto ca = float_source(doc.inputs[0], rgba),
       em = float_source(doc.inputs[1], emission),
       bg = float_source(doc.inputs[2], background);
  for (auto window : {64U, 256U}) {
    ResourceBudget root;
    auto output = take(execute(
        registry, doc,
        {{{"coverage", {}, ca}, {"emission", {}, em}, {"background", {}, bg}}},
        &root, window));
    // Integer dyadic reference: opacity gives (1,0,-.5,.25), E unchanged;
    // over adds .75*(2,0,-1,.5) and .75*(4,-2,1).
    check(coverage(output.results.at("layer")) ==
              std::array<float, 4>{2.5F, 0, -1.25F, .625F},
          "Layer rational coverage oracle");
    check(coverage(output.results.at("mean")) ==
              coverage(output.results.at("layer")),
          "single-location weighted roundtrip");
    const auto& image = output.values.at("image");
    std::array<float, 4> pixels{};
    std::memcpy(pixels.data(), image.bytes().data(), 16);
    check(pixels == std::array<float, 4>{12.5F, -2, 1.25F, 1},
          "source to DAG to sink rational oracle");
    check(coverage(output.results.at("capped")) ==
              std::array<float, 4>{5, 0, -2.5F, 1},
          "raw cap keeps color");
    auto held = output.results.at("mean");
    auto old = output.results.at("layer").weak();
    output = {};
    check(old.lock().valid(),
          "derived Layer retains predecessor after context retirement");
    held = {};
    check(
        !old.lock().valid() && root.statistics().live[ResourceKind::Disk] == 0,
        "final Layer release retires backing");
    std::cout << "layer-raster window=" << window
              << " image=[12.5,-2,1.25,1] passed\n";
  }
  auto run = [&](const WorkflowDocument& workflow, std::uint64_t* observed) {
    ResourceBudget root;
    return execute(
        registry, workflow,
        {{{"coverage", {}, ca}, {"emission", {}, em}, {"background", {}, bg}}},
        &root, 64, 10000000, 262144, observed);
  };
  std::uint64_t observed = 0;
  emission[0] = std::numeric_limits<float>::infinity();
  auto malformed = run(doc, &observed);
  check(!malformed.ok() && observed == 0,
        "all emission components validated before publication");
  emission[0] = 4;
  rgba[3] = std::numeric_limits<float>::denorm_min();
  auto underflow = run(doc, &observed);
  check(!underflow.ok() &&
            underflow.status().reason == FailureReason::AssociationUnderflow,
        "public DAG opacity association underflow");
  rgba[3] = .5F;
  auto checked = doc;
  checked.nodes.push_back(
      {15, "layer.raw_checked", {WorkflowNodeOutput{9, "value"}}, {}});
  checked.outputs = {{"checked", 15, "value"}};
  auto mass_failure = run(checked, &observed);
  check(!mass_failure.ok() &&
            mass_failure.status().reason == FailureReason::InvalidAssociation,
        "public DAG raw mass checked conversion");
  std::cout << "layer-runtime full-seven-component, opacity-underflow and "
               "raw-mass failures passed\n";
}
void dynamic(const char* name,
             const std::vector<LayerContribution>& contributions,
             double expected_sum, double expected_weight, bool valid,
             bool required = false, FailureReason reason = FailureReason::None,
             std::uint64_t window = 64, std::uint64_t work = 10000000) {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition importer;
  importer.key = "example.contributions";
  auto& traits = importer.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.workspace_bytes = 4096;
  auto& out = traits.outputs[0];
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(Import);
  out.maximum_dependency_stages = 1000000;
  out.result_schema = take(layer_schema(LayerRepresentation::Contributions));
  out.output_schema.kind = OperationPortKind::Result;
  out.output_schema.result_schema_id = std::string(out.result_schema->id);
  out.output_schema.result_schema_version = 1;
  importer.start_result = [](const ResultProgramQuery&,
                             const BufferAllocator& allocator) {
    return ResultContinuation::make<Import>(allocator);
  };
  check(registry->register_operation(std::move(importer)).ok(),
        "register dynamic contribution source");
  for (auto op : {LayerOperation::Reduce, LayerOperation::Finalize,
                  LayerOperation::RequireValid})
    add(registry.get(), op);
  std::vector<std::uint8_t> wire(8 + contributions.size() * 64);
  for (unsigned i = 0; i < 8; ++i)
    wire[i] = static_cast<std::uint8_t>(
        static_cast<std::uint64_t>(contributions.size()) >> (i * 8));
  for (std::size_t i = 0; i < contributions.size(); ++i)
    std::memcpy(wire.data() + 8 + i * 64, contributions[i].components.data(),
                64);
  WorkflowDocument doc;
  doc.inputs = {{1,
                 "source",
                 {ElementType::UInt8, {wire.size()}},
                 Region::whole({wire.size()}),
                 {0, {1}},
                 {}}};
  doc.nodes = {
      {1, "example.contributions", {WorkflowInputReference{1}}, {}},
      {2, "layer.weighted_reduce", {WorkflowNodeOutput{1, "value"}}, {}},
      {3, "layer.weighted_finalize", {WorkflowNodeOutput{2, "value"}}, {}},
      {4, "layer.weighted_reduce", {WorkflowNodeOutput{1, "value"}}, {}}};
  doc.outputs = {{"sum", 2, "value"},
                 {"mean", 3, "value"},
                 {"alias", 4, "value"}};
  if (required) {
    doc.nodes.push_back(
        {5, "layer.require_valid", {WorkflowNodeOutput{3, "value"}}, {}});
    doc.outputs.push_back({"image", 5, "value"});
  }
  ResourceBudget root;
  auto result = execute(registry, doc, {{{"source", {}, bytes_source(wire)}}},
                        &root, window, work);
  if (reason != FailureReason::None || window < 64 || work < 1000) {
    check(!result.ok(), "expected bounded or numerical failure");
    if (reason != FailureReason::None)
      check(result.status().reason == reason,
            "stable numerical reason lost by runtime");
    else
      check(result.status().code == ErrorCode::ResourceExhausted,
            "wrong bounded failure category");
    std::cout << name << " rejected: " << result.status().message << '\n';
    return;
  }
  auto output = take(std::move(result));
  auto sum = output.results.at("sum");
  auto mean = output.results.at("mean");
  check(sum.object_id() == output.results.at("alias").object_id(),
        "cache-off duplicate consumers must share result");
  auto page =
      take(take(sum.prepare_read(take(sum.descriptor()), 0, 0, 1)).load(64));
  std::array<double, 8> data{};
  std::memcpy(data.data(), page->bytes().data(), 64);
  check(data[0] == expected_sum && data[7] == expected_weight,
        "independent midpoint tree oracle");
  if (contributions.empty()) {
    auto witness = take(sum.descriptor_relation());
    bool descriptor_seen = false;
    check(witness.visit(0, 32,
                        [&](ResultSupport support) {
                          if (support.input == 0 && support.roles == 8 &&
                              support.first == 0 && support.count == 1)
                            descriptor_seen = true;
                          return Status::success();
                        })
                  .ok() &&
              descriptor_seen,
          "empty input descriptor witness missing");
    check(take(witness.intersects(0, {{0, 8, 0, 1}}, 64)).value_or(false),
          "empty count edit must invalidate sum");
  }
  auto facts = take(mean.descriptor());
  auto bit = take(take(mean.prepare_read(facts, 0, 0, 1)).load(64));
  check((bit->bytes().data()[0] == 1) == valid &&
            facts.rows(1) == (valid ? 1U : 0U),
        "empty weighted validity/count oracle");
  if (valid)
    check(coverage(mean, 1)[0] ==
              static_cast<float>(expected_sum / expected_weight),
          "weighted mean oracle");
  check(take(mean.descriptor_relation()).guarantee() ==
            DependencyGuarantee::Conservative,
        "empty descriptor support guarantee");
  auto weak = sum.weak();
  output = {};
  sum = {};
  page.reset();
  check(weak.lock().valid(), "optional Layer owns weighted predecessor");
  mean = {};
  bit.reset();
  check(!weak.lock().valid() && root.statistics().live[ResourceKind::Disk] == 0,
        "weighted final pin release");
  check(root.statistics().peak[ResourceKind::Host] <= 262144,
        "managed capacity limit");
  std::cout << name << " count=" << contributions.size() << " window=" << window
            << " Np=" << expected_sum << " W=" << expected_weight
            << " passed\n";
}
LayerContribution contribution(double p, double weight = 1, double alpha = 1) {
  return {{p, 0, 0, alpha, 0, 0, 0, weight}};
}
}  // namespace
int main() {
  try {
    raster();
    for (auto window : {64U, 192U, 256U}) {
      dynamic("midpoint-3",
              {contribution(0x1p54), contribution(-0x1p54), contribution(1)}, 0,
              3, true, false, FailureReason::None, window);
      dynamic("midpoint-5",
              {contribution(0), contribution(0), contribution(0x1p54),
               contribution(-0x1p54), contribution(1)},
              0, 5, true, false, FailureReason::None, window);
      dynamic(
          "midpoint-7",
          {contribution(0x1p54), contribution(-0x1p54), contribution(1),
           contribution(0), contribution(0), contribution(0), contribution(0)},
          0, 7, true, false, FailureReason::None, window);
    }
    dynamic("same-weight-tree",
            {contribution(1, 0x1p53), contribution(1), contribution(1)},
            9007199254740994.0, 9007199254740994.0, true);
    dynamic("empty", {}, 0, 0, false);
    dynamic("negative-weight", {contribution(1, -1)}, 0, 0, false, false,
            FailureReason::InvalidAssociation);
    dynamic("zero-weight", {contribution(2, 0), contribution(-3, 0)}, 0, 0,
            false);
    dynamic("empty-image-rejected", {}, 0, 0, false, true,
            FailureReason::EmptyWeightedResult);
    const auto tiny = std::numeric_limits<double>::denorm_min();
    dynamic("scratch-cancellation",
            {contribution(1, tiny, .5), contribution(-1, tiny, .5)}, 0,
            2 * tiny, true);
    dynamic("association-underflow", {contribution(1, tiny, .5)}, 0, tiny,
            false, false, FailureReason::AssociationUnderflow);
    dynamic("page-too-small", {contribution(1)}, 1, 1, true, false,
            FailureReason::None, 63);
    dynamic("work-exhausted", {contribution(1)}, 1, 1, true, false,
            FailureReason::None, 64, 10);
    std::vector<LayerContribution> many(8192, contribution(1));
    dynamic("paged-low-host", many, 8192, 8192, true, false,
            FailureReason::None, 256);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
