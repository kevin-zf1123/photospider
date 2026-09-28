#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
constexpr unsigned height = 17, width = 35;
const ValueDescriptor descriptor{ElementType::Float64, {height, width, 3}};
WorkflowDocument document(bool tiled) {
  WorkflowDocument result;
  result.inputs = {{1,
                    "coordinates",
                    descriptor,
                    Region::whole(descriptor.shape),
                    {0, {width * 24, 24, 8}},
                    {}}};
  result.nodes = {{1,
                   tiled ? "noise.perlin2002_3d_v1_strict_cpu_tiled"
                         : "noise.perlin2002_3d_v1_strict_cpu_whole",
                   {WorkflowInputReference{1}},
                   {}}};
  result.outputs = {{"values", 1, "values"}};
  return result;
}
}  // namespace
int main() {
  auto registry = make_default_operation_registry();
  std::vector<double> samples(height * width * 3);
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      samples[(y * width + x) * 3] =
          static_cast<double>((y * 31 + x * 17) % 257) / 64. - 2.;
      samples[(y * width + x) * 3 + 1] = .25 + y / 32.;
      samples[(y * width + x) * 3 + 2] = -.125;
    }
  std::vector<std::uint8_t> bytes(samples.size() * 8);
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  auto input = Value::create(descriptor, Region::whole(descriptor.shape),
                             {0, {width * 24, 24, 8}}, bytes)
                   .take_value();
  ExecutionContextConfig config;
  config.cpu_workers = 4;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  GraphContext whole_graph(document(false));
  auto whole_plan = Compiler(registry).compile(whole_graph);
  PS_CHECK(whole_plan.ok());
  auto whole =
      context.execute(whole_plan.value().plan, {{{"coordinates", input}}});
  PS_CHECK(whole.ok());
  const auto& reference = whole.value().values.at("values");
  GraphContext tile_graph(document(true));
  PlanningOptions planning;
  planning.tile_width = 8;
  planning.tile_height = 4;
  const Region roi({{3, 11}, {5, 27}});
  planning.output_regions.emplace("values", roi);
  auto compiled = Compiler(registry).compile(tile_graph, planning);
  if (!compiled.ok())
    std::cerr << compiled.status().message << '\n';
  PS_CHECK(compiled.ok());
  std::mutex mutex;
  std::vector<Region> reads;
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = descriptor;
  source->read = [&](const Region& region, std::uint8_t* destination,
                     std::uint64_t size, const BufferAllocator&,
                     const CancellationToken&) {
    const auto& d = region.dimensions();
    if (d[0].offset < 3 || d[0].offset + d[0].extent > 14 || d[1].offset < 5 ||
        d[1].offset + d[1].extent > 32 || d[2].offset != 0 ||
        d[2].extent != 3 || size != d[0].extent * d[1].extent * 24)
      return Result<Region>(
          Status{ErrorCode::Internal, "read beyond exact Perlin ROI"});
    std::uint64_t offset = 0;
    for (auto y = d[0].offset; y < d[0].offset + d[0].extent; ++y)
      for (auto x = d[1].offset; x < d[1].offset + d[1].extent; ++x) {
        std::memcpy(destination + offset, &samples[(y * width + x) * 3], 24);
        offset += 24;
      }
    {
      std::lock_guard<std::mutex> lock(mutex);
      reads.push_back(region);
    }
    return Result<Region>(region);
  };
  // Outside-ROI NaN must not be read or included in finite validation.
  const std::uint64_t nan = UINT64_C(0x7ff0000000000001);
  std::memcpy(&samples[0], &nan, 8);
  ExecutionBindings binding{{{"coordinates", {}, source, {}}}};
  unsigned deliveries = 0;
  std::uint64_t computed = 0;
  auto stream = context.execute_stream(
      compiled.value().plan, binding,
      [&](const std::string& name, ValueView view) {
        if (name != "values")
          return Status{ErrorCode::Internal, "wrong output"};
        const auto& d = view.region().dimensions();
        if (d[0].extent > 4 || d[1].extent > 8)
          return Status{ErrorCode::Internal, "unbounded Perlin tile"};
        std::uint64_t offset = 0;
        for (auto y = d[0].offset; y < d[0].offset + d[0].extent; ++y)
          for (auto x = d[1].offset; x < d[1].offset + d[1].extent; ++x) {
            if (std::memcmp(view.bytes().data() + offset,
                            reference.bytes().data() + (y * width + x) * 8, 8))
              return Status{ErrorCode::Internal, "tile differs from Whole"};
            offset += 8;
          }
        ++deliveries;
        computed += d[0].extent * d[1].extent;
        return Status::success();
      });
  if (!stream.ok())
    std::cerr << stream.status().message << '\n';
  PS_CHECK(stream.ok());
  PS_CHECK(deliveries == 12 && computed == 11 * 27);
  PS_CHECK(stream.value().tile_count == 12);
  PS_CHECK(stream.value().source_read_bytes == 11 * 27 * 24);
  PS_CHECK(reads.size() == 12);
  for (const auto& read : reads)
    PS_CHECK(read.dimensions()[0].extent <= 4 &&
             read.dimensions()[1].extent <= 8);
  reads.clear();
  auto collected = context.execute(compiled.value().plan, binding);
  if (!collected.ok())
    std::cerr << collected.status().message << '\n';
  PS_CHECK(collected.ok());
  PS_CHECK(reads.size() == 12);
  for (const auto& read : reads)
    PS_CHECK(read.dimensions()[0].extent <= 4 &&
             read.dimensions()[1].extent <= 8);
  const auto& value = collected.value().values.at("values");
  for (unsigned y = 0; y < 11; ++y)
    for (unsigned x = 0; x < 27; ++x)
      PS_CHECK(
          std::memcmp(value.bytes().data() + (y * 27 + x) * 8,
                      reference.bytes().data() + ((y + 3) * width + x + 5) * 8,
                      8) == 0);

  DependencyRequest empty;
  empty.inputs = {{descriptor, {}}};
  empty.outputs = Footprint::from_regions({height, width}, {}).take_value();
  empty.snapshot_identity = "perlin-empty";
  auto session = registry->start_dependency(
      "noise.perlin2002_3d_v1_strict_cpu_tiled", empty);
  PS_CHECK(session.ok());
  auto finished = session.value()->poll();
  PS_CHECK(finished.ok());
  PS_CHECK(std::holds_alternative<DependencyResult>(finished.value()));
  PS_CHECK(
      std::get<DependencyResult>(finished.value()).value.coverage().empty());

  // Direct sessions preserve multiple boxes; their logical row order overlaps
  // across boxes, so this also exercises the decoded scratch traversal order.
  const ValueDescriptor f32{ElementType::Float32, descriptor.shape};
  auto requested =
      Footprint::from_regions(
          {height, width}, {Region({{2, 3}, {2, 2}}), Region({{2, 3}, {8, 3}})})
          .take_value();
  auto narrow_writer =
      MutableValue::allocate(f32, Region::whole(f32.shape), BufferAllocator{})
          .take_value();
  for (unsigned i = 0; i < samples.size(); ++i) {
    double original;
    std::memcpy(&original, input.bytes().data() + i * 8, 8);
    const float converted = static_cast<float>(original);
    std::memcpy(narrow_writer.data() + i * 4, &converted, 4);
  }
  const auto narrow_input = std::move(narrow_writer).publish().take_value();
  const auto run =
      [&](const Value& source_value, std::uint64_t maximum_work,
          const BufferAllocator& allocator) -> Result<DependencyProgress> {
    DependencyRequest request;
    request.inputs = {{f32, {}}};
    request.outputs = requested;
    request.snapshot_identity = "perlin-boxes";
    request.limits.maximum_work = maximum_work;
    auto started = registry->start_dependency(
        "noise.perlin2002_3d_v1_strict_cpu_tiled", request);
    if (!started.ok())
      return Result<DependencyProgress>(started.status());
    auto active = started.take_value();
    auto need = active->poll(allocator);
    if (!need.ok())
      return need;
    auto pending = active->pending_reads();
    if (!pending.ok())
      return Result<DependencyProgress>(pending.status());
    auto authorized = Footprint::none(f32.shape).take_value();
    for (const auto& read : pending.value())
      authorized = authorized.unite(read.samples).take_value();
    auto fragments =
        ValueFragments::create(f32, {}, authorized, {source_value});
    if (!fragments.ok())
      return Result<DependencyProgress>(fragments.status());
    auto supplied = active->supply({fragments.take_value()}, "perlin-boxes");
    if (!supplied.ok())
      return Result<DependencyProgress>(supplied);
    return active->poll(allocator);
  };
  auto boxes = run(narrow_input, UINT64_C(1000000000), BufferAllocator{});
  PS_CHECK(boxes.ok());
  const auto& output = std::get<DependencyResult>(boxes.value()).value;
  for (const auto& box : requested.boxes())
    for (auto y = box.dimensions()[0].offset;
         y < box.dimensions()[0].offset + box.dimensions()[0].extent; ++y)
      for (auto x = box.dimensions()[1].offset;
           x < box.dimensions()[1].offset + box.dimensions()[1].extent; ++x) {
        std::uint64_t actual;
        PS_CHECK(output.read({y, x}, &actual, 8).ok());
        PS_CHECK(std::memcmp(&actual,
                             reference.bytes().data() + (y * width + x) * 8,
                             8) == 0);
      }
  auto limited = run(narrow_input, 1000, BufferAllocator{});
  PS_CHECK(!limited.ok() &&
           limited.status().code == ErrorCode::ResourceExhausted);
  ResourceLimits limits;
  // The 15 samples require 1080 bytes of decoded coordinates alone.
  limits.capacity[ResourceKind::Payload] = 1079;
  ResourceBudget bounded(limits);
  auto exhausted = run(narrow_input, UINT64_C(1000000000), bounded.allocator());
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(bounded.statistics().live[ResourceKind::Payload] == 0);
  auto poisoned = narrow_input.copy_bytes();
  const std::uint32_t narrow_nan = UINT32_C(0x7f800001);
  std::memcpy(poisoned.data() + (2 * width + 8) * 12, &narrow_nan, 4);
  auto nonfinite = Value::create(f32, Region::whole(f32.shape),
                                 {0, {width * 12, 12, 4}}, poisoned)
                       .take_value();
  auto rejected = run(nonfinite, UINT64_C(1000000000), BufferAllocator{});
  PS_CHECK(!rejected.ok() &&
           rejected.status().code == ErrorCode::InvalidArgument);
  return 0;
}
