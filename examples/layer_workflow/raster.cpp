#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
float sample(std::uint64_t pixel, unsigned channel, bool emission) {
  const auto value =
      static_cast<int>((pixel * (emission ? 3 : 5) + channel) % 17);
  return static_cast<float>(value - 8) / 8;
}
double expected(unsigned node, unsigned field, unsigned c,
                std::uint64_t pixel) {
  const auto p = sample(pixel, c % 3, false);
  const auto e = sample(pixel, c % 3, true);
  if (node == 3) {
    if (c < 3)
      return 1.5 * p;
    if (c == 3)
      return .75;
    if (c < 7)
      return 1.5 * sample(pixel, c - 4, true);
    return .25;
  }
  if (node == 4)
    return c == 3 ? 1 : 1.5 * p + 1.75 * e;
  if (node == 8 || node == 9)
    return c == 3 ? (node == 8 ? .25 : .0625)
                  : (node == 8 ? 1.5 : 1.875) * (p + e);
  if (node == 10)
    return c == 3 ? 1 : 2 * p;
  if (field == 1) {
    if (node == 11 || node == 12)
      return 0;
    return e * (node == 6                ? 3
                : node == 7              ? 2
                : node == 2 || node == 5 ? 1.5
                                         : 1);
  }
  if (c == 3)
    return node == 2                  ? .75
           : node == 5                ? .375
           : node == 11 || node == 12 ? 1
                                      : .5;
  return p * (node == 2                  ? 1.5
              : node == 5                ? .75
              : node == 11 || node == 12 ? 2
                                         : 1);
}
void run(std::uint64_t h, std::uint64_t w, std::uint64_t window, bool large,
         unsigned failure = 0) {
  const auto count = h * w;
  auto registry = std::make_shared<OperationRegistry>();
  for (auto op : {LayerOperation::Assemble, LayerOperation::Over,
                  LayerOperation::Weight, LayerOperation::Flatten,
                  LayerOperation::Opacity, LayerOperation::EmitFront,
                  LayerOperation::EmitBehind, LayerOperation::Response,
                  LayerOperation::ResponseOver, LayerOperation::CoverageRawPlus,
                  LayerOperation::RawChecked, LayerOperation::RawCapped})
    check(registry->register_operation(take(make_layer_operation(op, {h, w})))
              .ok(),
          "register raster operation");
  check(registry->freeze().ok(), "freeze raster registry");
  WorkflowDocument doc;
  doc.inputs = {{1,
                 "rgba",
                 {ElementType::Float32, {h, w, 4}},
                 Region::whole({h, w, 4}),
                 {0, {static_cast<std::int64_t>(w * 16), 16, 4}},
                 {take(encode_semantic(rgba_semantics()))}},
                {2,
                 "emission",
                 {ElementType::Float32, {h, w, 3}},
                 Region::whole({h, w, 3}),
                 {0, {static_cast<std::int64_t>(w * 12), 12, 4}},
                 {}}};
  doc.nodes = {
      {1,
       "layer.assemble",
       {WorkflowInputReference{1}, WorkflowInputReference{2}},
       {}},
      {2,
       "layer.over",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{1, "value"}},
       {}},
      {3, "layer.weight", {WorkflowNodeOutput{2, "value"}}, {{"weight", .25}}}};
  if (!large) {
    doc.nodes.push_back(
        {4,
         "layer.flatten",
         {WorkflowNodeOutput{2, "value"}, WorkflowInputReference{2}},
         {}});
    doc.nodes.push_back({5,
                         "layer.opacity",
                         {WorkflowNodeOutput{2, "value"}},
                         {{"factor", .5}}});
    doc.nodes.push_back(
        {6,
         "layer.emit_front",
         {WorkflowNodeOutput{1, "value"}, WorkflowInputReference{2}},
         {{"factor", 2.0}}});
    doc.nodes.push_back(
        {7,
         "layer.emit_behind",
         {WorkflowNodeOutput{1, "value"}, WorkflowInputReference{2}},
         {{"factor", 2.0}}});
    doc.nodes.push_back(
        {8, "layer.response", {WorkflowNodeOutput{2, "value"}}, {}});
    doc.nodes.push_back(
        {9,
         "layer.response_over",
         {WorkflowNodeOutput{8, "value"}, WorkflowNodeOutput{8, "value"}},
         {}});
    doc.nodes.push_back(
        {10,
         "layer.coverage_raw_plus",
         {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{1, "value"}},
         {}});
    doc.nodes.push_back(
        {11, "layer.raw_checked", {WorkflowNodeOutput{10, "value"}}, {}});
    doc.nodes.push_back(
        {12, "layer.raw_capped", {WorkflowNodeOutput{10, "value"}}, {}});
  }
  for (unsigned i = 1; i <= (large ? 3U : 12U); ++i)
    doc.outputs.push_back({"node" + std::to_string(i), i, "value"});
  GraphContext graph(doc);
  const auto compiled = take(Compiler(registry).compile(graph));
  CancellationSource cancellation;
  ExecutionBindings bindings;
  for (unsigned i = 0; i < 2; ++i) {
    auto source = std::make_shared<RegionalSource>();
    source->descriptor = doc.inputs[i].descriptor;
    source->facets = doc.inputs[i].facets;
    source->read = [&, i](const Region& region, std::uint8_t* data,
                          std::uint64_t bytes, const BufferAllocator&,
                          const CancellationToken&) -> Result<Region> {
      std::uint64_t cursor = 0;
      const auto& dims = region.dimensions();
      for (auto y = dims[0].offset; y < dims[0].offset + dims[0].extent; ++y)
        for (auto x = dims[1].offset; x < dims[1].offset + dims[1].extent; ++x)
          for (auto c = dims[2].offset; c < dims[2].offset + dims[2].extent;
               ++c) {
            const auto pixel = y * w + x;
            float value = i == 0 && c == 3 ? .5F : sample(pixel, c, i != 0);
            if (failure == 1 && i == 1 && pixel == 40)
              value = std::numeric_limits<float>::quiet_NaN();
            if (failure == 4 && i == 0 && c == 0 && pixel == 40)
              value = std::numeric_limits<float>::max();
            if (cursor + 4 > bytes)
              return Result<Region>(
                  Status{ErrorCode::Internal, "source bounds"});
            std::memcpy(data + cursor, &value, 4);
            cursor += 4;
          }
      if (failure == 3 && dims[0].offset > 0)
        cancellation.cancel();
      return Result<Region>(region);
    };
    bindings.inputs.push_back({doc.inputs[i].name, {}, source});
  }
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] = 1048576;
  config.managed_resources->capacity[ResourceKind::Metadata] = 1048576;
  if (failure == 2)
    config.managed_resources->capacity[ResourceKind::Disk] = 4096;
  ExecutionContext context(registry, config);
  const auto root = take(context.resource_budget());
  ExecutionOptions options;
  options.maximum_result_window_bytes = window;
  options.maximum_dependency_work = 1000000000;
  options.dependencies.maximum_stages = large          ? 1000000
                                        : window == 64 ? 4096
                                                       : 512;
  std::set<std::uint64_t> observed;
  options.result_publication = [&](ValueRef ref, const ResultRef& object) {
    const auto descriptor = take(object.descriptor());
    check(observed.insert(ref.node_id).second,
          "duplicate complete notification");
    for (unsigned f = 0; f < descriptor.field_count(); ++f) {
      check(descriptor.rows(f) == count, "raster output count");
      const auto width = take(object.schema().row_bytes(f));
      for (std::uint64_t row = 0; row < count;) {
        const auto rows = std::min(count - row, window / width);
        auto page = take(
            take(object.prepare_read(descriptor, f, row, rows)).load(window));
        for (std::uint64_t j = 0; j < rows; ++j)
          for (unsigned c = 0; c < width / (ref.node_id == 3 ? 8 : 4); ++c) {
            double value = 0;
            if (ref.node_id == 3) {
              std::memcpy(&value, page->bytes().data() + j * width + c * 8, 8);
            } else {
              float v = 0;
              std::memcpy(&v, page->bytes().data() + j * width + c * 4, 4);
              value = v;
            }
            const auto reference = failure == 4 && ref.node_id == 1 && f == 0 &&
                                           c == 0 && row + j == 40
                                       ? std::numeric_limits<float>::max()
                                       : expected(ref.node_id, f, c, row + j);
            check(value == reference, "raster reference mismatch");
          }
        row += rows;
      }
    }
    return Status::success();
  };
  auto result = context.execute_stream(
      compiled.plan, bindings,
      [&](const std::string& name, ValueView value) {
        check(name == "node4", "flatten output name");
        for (std::uint64_t row = 0; row < count; ++row)
          for (unsigned c = 0; c < 4; ++c) {
            float actual = 0;
            std::memcpy(&actual, value.bytes().data() + row * 16 + c * 4, 4);
            check(actual == expected(4, 0, c, row),
                  "flatten reference mismatch");
          }
        return Status::success();
      },
      cancellation.token(), options);
  if (failure) {
    check(!result.ok(), "expected raster failure");
    check(failure == 4 ? observed == std::set<std::uint64_t>{1}
                       : observed.empty(),
          "failed raster must not publish");
    if (failure == 3)
      check(result.status().code == ErrorCode::Cancelled, "cancel category");
    if (failure == 2)
      check(result.status().code == ErrorCode::ResourceExhausted,
            "disk category");
    if (failure == 4)
      check(result.status().reason == FailureReason::ArithmeticOverflow,
            "nonfirst raster arithmetic failure");
  } else {
    if (!result.ok())
      throw std::runtime_error(result.status().message);
    check(observed.size() == (large ? 3U : 11U), "all raster outputs observed");
  }
  check(root.statistics().live[ResourceKind::Disk] == 0,
        "raster backing release");
  std::cout << h << 'x' << w << " window=" << window << " failure=" << failure
            << " stages=" << root.statistics().issued.stages
            << " host_peak=" << root.statistics().peak[ResourceKind::Host]
            << " passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--large") {
      run(1080, 1920, 4096, true);
    } else {
      for (auto window : {64U, 256U, 4096U})
        run(19, 37, window, false);
      for (unsigned failure = 1; failure <= 4; ++failure)
        run(19, 37, 256, false, failure);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
