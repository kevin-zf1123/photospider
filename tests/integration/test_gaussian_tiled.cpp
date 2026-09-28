#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
constexpr unsigned height = 7, width = 13;
using Point = std::pair<unsigned, unsigned>;
using Parameters = std::map<std::string, ParameterValue>;
Parameters parameters(const std::string& boundary) {
  return {{"radius_x", std::int64_t{2}},
          {"radius_y", std::int64_t{1}},
          {"sigma_x", 1.},
          {"sigma_y", 1.},
          {"x_axis", std::int64_t{1}},
          {"y_axis", std::int64_t{0}},
          {"cval", -3.},
          {"boundary", boundary}};
}
// Independent tap-by-tap reflection; no closed support interval calculation.
int extension(int index, int size, const std::string& mode) {
  if (mode == "constant" && (index < 0 || index >= size))
    return -1;
  if (mode == "clamp")
    return std::max(0, std::min(index, size - 1));
  if (mode == "wrap") {
    while (index < 0)
      index += size;
    while (index >= size)
      index -= size;
    return index;
  }
  if (size == 1)
    return 0;
  while (index < 0 || index >= size) {
    if (index < 0)
      index = -index - (mode == "reflect_half" ? 1 : 0);
    if (index >= size)
      index = 2 * size - index - (mode == "reflect_half" ? 1 : 2);
  }
  return index;
}
std::set<Point> needed(unsigned y, unsigned x, unsigned ry, unsigned rx,
                       const std::string& mode) {
  std::set<Point> result;
  for (int j = -static_cast<int>(ry); j <= static_cast<int>(ry); ++j)
    for (int i = -static_cast<int>(rx); i <= static_cast<int>(rx); ++i) {
      const auto yy = extension(static_cast<int>(y) + j, height, mode);
      const auto xx = extension(static_cast<int>(x) + i, width, mode);
      if (yy >= 0 && xx >= 0)
        result.emplace(yy, xx);
    }
  return result;
}
std::set<Point> points(const Footprint& value) {
  std::set<Point> result;
  auto status = value.visit(
      [&](const auto& at) {
        result.emplace(at[0], at[1]);
        return Status::success();
      },
      100000);
  if (!status.ok())
    throw std::runtime_error(status.message);
  return result;
}
WorkflowDocument document(const Value& input, const Parameters& p, bool tiled) {
  WorkflowDocument result;
  result.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  result.nodes = {{1,
                   tiled ? "filter.gaussian_baked64_v1_strict_cpu_tiled"
                         : "filter.gaussian_baked64_v1_strict_cpu_whole",
                   {WorkflowInputReference{1}},
                   p}};
  result.outputs = {{"output", 1, "output"}};
  return result;
}
ExecutionOptions options() {
  ExecutionOptions result;
  result.dependencies.maximum_work = UINT64_C(1000000000000);
  result.maximum_dependency_work = UINT64_C(1000000000000);
  return result;
}
Result<DependencyResult> direct(
    const std::shared_ptr<OperationRegistry>& registry, const Value& input,
    DependencyRequest request,
    const BufferAllocator& allocator = BufferAllocator{}) {
  auto started = registry->start_dependency(
      "filter.gaussian_baked64_v1_strict_cpu_tiled", request, allocator);
  if (!started.ok())
    return Result<DependencyResult>(started.status());
  auto session = started.take_value();
  auto first = session->poll(allocator);
  if (!first.ok())
    return Result<DependencyResult>(first.status());
  if (auto* result = std::get_if<DependencyResult>(&first.value()))
    return Result<DependencyResult>(std::move(*result));
  auto reads = session->pending_reads();
  if (!reads.ok())
    return Result<DependencyResult>(reads.status());
  auto required = Footprint::none(input.descriptor().shape).take_value();
  for (const auto& read : reads.value())
    required = required.unite(read.samples).take_value();
  auto fragments = ValueFragments::create(input.descriptor(), input.facets(),
                                          required, {input});
  if (!fragments.ok())
    return Result<DependencyResult>(fragments.status());
  auto supplied =
      session->supply({fragments.take_value()}, request.snapshot_identity);
  if (!supplied.ok())
    return Result<DependencyResult>(supplied);
  auto second = session->poll(allocator);
  if (!second.ok())
    return Result<DependencyResult>(second.status());
  if (!std::holds_alternative<DependencyResult>(second.value()))
    return Result<DependencyResult>(
        Status{ErrorCode::Internal, "unexpected extra Gaussian stage"});
  return Result<DependencyResult>(
      std::get<DependencyResult>(second.take_value()));
}
int check(const std::string& boundary, const Region& roi) {
  auto registry = make_default_operation_registry();
  auto writer =
      MutableValue::allocate({ElementType::Float64, {height, width}},
                             Region::whole({height, width}), BufferAllocator{})
          .take_value();
  for (unsigned i = 0; i < height * width; ++i) {
    const double value = (static_cast<int>(i * 19 % 83) - 41) / 16.;
    std::memcpy(writer.data() + i * 8, &value, 8);
  }
  const auto input = std::move(writer).publish().take_value();
  const auto p = parameters(boundary);
  GraphContext whole_graph(document(input, p, false));
  auto whole_plan = Compiler(registry).compile(whole_graph).take_value();
  ExecutionContextConfig config;
  config.cpu_workers = 4;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto whole =
      context.execute(whole_plan.plan, {{{"input", input}}}, {}, options());
  PS_CHECK(whole.ok());
  const auto& reference = whole.value().values.at("output");
  std::set<Point> required;
  auto query = Footprint::from_regions({height, width}, {roi}).take_value();
  for (const auto& at : points(query)) {
    auto support = needed(at.first, at.second, 1, 2, boundary);
    required.insert(support.begin(), support.end());
  }
  std::set<Point> observed;
  std::mutex mutex;
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = input.descriptor();
  source->read = [&](const Region& region, std::uint8_t* output,
                     std::uint64_t size, const BufferAllocator&,
                     const CancellationToken&) {
    std::uint64_t next = 0;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto y = region.dimensions()[0].offset;
         y < region.dimensions()[0].offset + region.dimensions()[0].extent; ++y)
      for (auto x = region.dimensions()[1].offset;
           x < region.dimensions()[1].offset + region.dimensions()[1].extent;
           ++x) {
        if (!required.count({y, x}))
          return Result<Region>(Status{ErrorCode::Internal,
                                       "read outside exact Gaussian support"});
        observed.emplace(y, x);
        std::memcpy(output + next, input.bytes().data() + (y * width + x) * 8,
                    8);
        next += 8;
      }
    return next == size
               ? Result<Region>(region)
               : Result<Region>(Status{ErrorCode::Internal, "source size"});
  };
  GraphContext graph(document(input, p, true));
  PlanningOptions planning;
  planning.tile_width = planning.tile_height = 2;
  planning.output_regions.emplace("output", roi);
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  ExecutionBindings bindings{{{"input", {}, source, {}}}};
  unsigned delivered = 0;
  auto streamed = context.execute_stream(
      compiled.value().plan, bindings,
      [&](const std::string&, ValueView tile) {
        const auto& dims = tile.region().dimensions();
        if (dims[0].extent > 2 || dims[1].extent > 2)
          return Status{ErrorCode::Internal, "unbounded Gaussian computation"};
        std::uint64_t next = 0;
        for (auto y = dims[0].offset; y < dims[0].offset + dims[0].extent; ++y)
          for (auto x = dims[1].offset; x < dims[1].offset + dims[1].extent;
               ++x) {
            if (std::memcmp(tile.bytes().data() + next,
                            reference.bytes().data() + (y * width + x) * 8, 8))
              return Status{ErrorCode::Internal,
                            "Gaussian tile differs from Whole"};
            next += 8;
          }
        ++delivered;
        return Status::success();
      },
      {}, options());
  if (!streamed.ok())
    std::cerr << streamed.status().message << '\n';
  PS_CHECK(streamed.ok() && delivered > 1);
  PS_CHECK(observed == required);
  auto collected =
      context.execute(compiled.value().plan, bindings, {}, options());
  PS_CHECK(collected.ok());
  auto certificate = collected.value().dependencies.certificate({1, 0});
  PS_CHECK(certificate.ok());
  for (const auto& at : points(query)) {
    auto row = certificate.value().row({at.first, at.second});
    PS_CHECK(row.ok());
    unsigned data_needs = 0;
    for (const auto& need : row.value().inputs) {
      if (need.roles & static_cast<std::uint32_t>(DependencyRole::Data)) {
        ++data_needs;
        PS_CHECK(need.port == 0 &&
                 points(need.samples) ==
                     needed(at.first, at.second, 1, 2, boundary));
      } else {
        PS_CHECK(need.roles ==
                     static_cast<std::uint32_t>(DependencyRole::Descriptor) &&
                 need.samples.empty());
      }
    }
    PS_CHECK(data_needs == 1);
  }
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      auto dirty =
          Footprint::from_regions({height, width}, {Region({{y, 1}, {x, 1}})})
              .take_value();
      auto affected = certificate.value().transpose({0, 1, dirty, {}});
      PS_CHECK(affected.ok());
      std::set<Point> expected;
      for (const auto& at : points(query))
        if (needed(at.first, at.second, 1, 2, boundary).count({y, x}))
          expected.insert(at);
      PS_CHECK(points(affected.value()) == expected);
    }
  return 0;
}
}  // namespace
int main() {
  for (const auto* boundary :
       {"constant", "clamp", "wrap", "reflect_half", "reflect_whole"}) {
    PS_CHECK(check(boundary, Region({{2, 3}, {3, 4}})) == 0);
    PS_CHECK(check(boundary, Region({{0, 3}, {11, 2}})) == 0);
  }
  auto registry = make_default_operation_registry();
  std::vector<std::uint8_t> bytes(height * width * 8, 0);
  const std::uint64_t poison = UINT64_C(0x7ff0000000000001);
  std::memcpy(bytes.data() + 5 * 8, &poison, 8);
  auto input =
      Value::create({ElementType::Float64, {height, width}},
                    Region::whole({height, width}), {0, {width * 8, 8}}, bytes)
          .take_value();
  DependencyRequest request;
  request.inputs = {{input.descriptor(), {}}};
  request.parameters = parameters("constant");
  request.parameters["radius_x"] = std::int64_t{5};
  request.parameters["radius_y"] = std::int64_t{0};
  request.parameters["sigma_x"] = .125;
  request.parameters["cval"] = 0.;
  request.outputs =
      Footprint::from_regions({height, width}, {Region({{0, 1}, {0, 1}})})
          .take_value();
  request.snapshot_identity = "zero-tap";
  request.limits.maximum_work = UINT64_C(10000000000);
  auto result = direct(registry, input, request);
  PS_CHECK(result.ok());
  std::uint64_t value = 1;
  PS_CHECK(result.value().value.read({0, 0}, &value, 8).ok() && value == 0);
  auto row = result.value().certificate->row({0, 0});
  PS_CHECK(row.ok());
  unsigned data_needs = 0;
  for (const auto& need : row.value().inputs)
    if (need.roles & static_cast<std::uint32_t>(DependencyRole::Data)) {
      ++data_needs;
      PS_CHECK(points(need.samples) == needed(0, 0, 0, 4, "constant"));
    }
  PS_CHECK(data_needs == 1);
  request.limits.maximum_work = 1000;
  auto exhausted = direct(registry, input, request);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  request.limits.maximum_work = UINT64_C(10000000000);
  ResourceLimits limits;
  limits.capacity[ResourceKind::Payload] = 4096;
  ResourceBudget root(limits);
  auto capacity = direct(registry, input, request, root.allocator());
  PS_CHECK(!capacity.ok() &&
           capacity.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  request.outputs = Footprint::none({height, width}).take_value();
  request.limits.maximum_work = 10000;
  auto empty = direct(registry, input, request, BufferAllocator{}.limited(0));
  PS_CHECK(empty.ok() && empty.value().value.coverage().empty());
  return 0;
}
