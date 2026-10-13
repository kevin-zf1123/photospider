#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "support/stmap_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t source_side = 256, output_side = 128;
int measure(std::uint64_t extent, bool scattered, bool fragmented) {
  ResourceBudget observed;
  ExecutionDependencies records;
  std::uint64_t wall = 0, work = 0, payload = 0, metadata = 0;
  std::uint64_t callbacks = 0, edges = 0;
  {
    stmap_test::StmapDriver driver;
    observed = driver.root;
    auto schema = stmap_test::image_schema(source_side, source_side);
    auto builder =
        take(ResultBuilder::start(driver.root, schema, "probe.source"));
    const auto piece = fragmented ? 16U : source_side;
    for (std::uint64_t y = 0; y < source_side; y += piece)
      for (std::uint64_t x = 0; x < source_side; x += piece) {
        std::vector<float> pixels;
        for (std::uint64_t dy = 0; dy < piece; ++dy)
          for (std::uint64_t dx = 0; dx < piece; ++dx) {
            pixels.push_back(static_cast<float>(x + dx));
            pixels.push_back(static_cast<float>(y + dy));
            pixels.push_back(0);
            pixels.push_back(1);
          }
        auto buffer = take(driver.root.allocator().allocate(pixels.size() * 4));
        std::memcpy(buffer.data(), pixels.data(), pixels.size() * 4);
        check(builder.publish_tensor(
            0, Region({{0, 1}, {0, 1}, {y, piece}, {x, piece}, {0, 4}}),
            StridedLayout{0,
                          {0, 0, static_cast<std::int64_t>(piece * 16), 16, 4},
                          {0, 0, y, x, 0}},
            std::move(buffer).freeze()));
      }
    const auto source = take(builder.seal());
    std::vector<double> uv;
    for (std::uint64_t y = 0; y < output_side; ++y)
      for (std::uint64_t x = 0; x < output_side; ++x) {
        uv.push_back((scattered ? (x * 73 + y * 19) % source_side : x) + .5);
        uv.push_back((scattered ? (x * 11 + y * 97) % source_side : y) + .5);
      }
    const auto map = driver.map(uv, {output_side, output_side, 2}, true);
    auto plan = driver.plan(source, map, "clamp", extent);
    auto snapshot =
        take(driver.context.freeze(plan, {{{"source", source}, {"map", map}}}));
    ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(1) << 36;
    options.dependencies.maximum_work = UINT64_C(1) << 36;
    options.dependencies.sets.maximum_work = UINT64_C(1) << 36;
    const auto before = driver.root.statistics();
    const auto begin = Clock::now();
    auto answer = take(driver.context.execute(snapshot, {}, options));
    wall = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                 begin)
               .count();
    auto after = driver.root.statistics();
    work = after.issued.work - before.issued.work;
    payload =
        after.live[ResourceKind::Payload] - before.live[ResourceKind::Payload];
    metadata = after.live[ResourceKind::Metadata] -
               before.live[ResourceKind::Metadata];
    for (const auto& timing : answer.diagnostics.operation_timings)
      callbacks += timing.invocation_count;
    const auto count = ((output_side + extent - 1) / extent);
    for (TileIndex i = 0; i < count * count; ++i) {
      const auto record = take(answer.dependencies.tile_record({0, 0, i}));
      for (const auto& input : record->inputs)
        edges += input.whole ? 1 : input.tiles.size();
    }
    const auto& result = answer.results.at("result");
    const auto facts = take(result.descriptor());
    const auto window = take(result.acquire_tensor(
        facts, 0, Region::whole({1, 1, output_side, output_side, 4})));
    if (window.sample_axis() != 3)
      throw std::runtime_error("unexpected output row axis");
    for (std::uint64_t y = 0; y < output_side; ++y)
      for (std::uint64_t c = 0; c < 4; ++c)
        for (std::uint64_t x = 0; x < output_side;) {
          const auto run = take(window.row_run({0, 0, y, x, c}));
          const auto count = std::min(output_side - x, run.samples);
          for (std::uint64_t i = 0; i < count; ++i) {
            float value = -1;
            std::memcpy(&value,
                        run.data + static_cast<std::int64_t>(i) *
                                       run.sample_stride_bytes,
                        4);
            const float expected =
                c < 2 ? static_cast<float>(
                            uv[2 * (y * output_side + x + i) + c] - .5)
                      : c == 3;
            if (value != expected)
              throw std::runtime_error("STMap probe numerical mismatch");
          }
          x += count;
        }
    records = std::move(answer.dependencies);
  }
  const auto retained = observed.statistics().live;
  if (retained[ResourceKind::Payload] != 0)
    throw std::runtime_error("dependency records retained payload");
  std::cout << extent << ',' << scattered << ',' << fragmented << ',' << wall
            << ',' << work << ',' << callbacks << ',' << edges << ',' << payload
            << ',' << metadata << ',' << retained[ResourceKind::Metadata]
            << std::endl;
  records = {};
  for (auto live : observed.statistics().live.values)
    if (live)
      throw std::runtime_error("STMap probe owner leak");
  return 0;
}
}  // namespace
int main(int argc, char** argv) try {
  const auto repeats = argc > 1 ? std::stoul(argv[1]) : 3;
  std::cout << "extent,scattered,fragmented,us,work,callbacks,edges,payload,"
               "metadata,retained_metadata\n";
  for (unsigned repeat = 0; repeat < repeats; ++repeat)
    for (std::uint64_t extent : {32, 64, 128, 256})
      for (bool scattered : {false, true})
        for (bool fragmented : {false, true})
          measure(extent, scattered, fragmented);
  return 0;
} catch (const std::exception& failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
