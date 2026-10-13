#include <unistd.h>

#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"
#include "support/stmap_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
using stmap_test::image_schema;
using stmap_test::read;
using stmap_test::registry;
using stmap_test::StmapDriver;
bool equals(const TileSet& tiles, std::initializer_list<TileIndex> expected) {
  return tiles.size() == expected.size() &&
         std::equal(tiles.begin(), tiles.end(), expected.begin());
}
const TileDependency& dependency(const TileRecord& record, std::size_t input) {
  for (const auto& entry : record.inputs)
    if (const auto* binding = std::get_if<PlanWorkflowInput>(&entry.producer))
      if (binding->declaration_index == input)
        return entry;
  throw std::runtime_error("missing actual STMap Need");
}
std::vector<float> pixels(unsigned width) {
  std::vector<float> values(width * 4, 0);
  for (unsigned x = 0; x < width; ++x) {
    values[x * 4] = static_cast<float>(x + 1);
    values[x * 4 + 3] = 1;
  }
  return values;
}
std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
std::int64_t stmap_reference_index(std::int64_t i, std::int64_t n,
                                   const std::string& mode) {
  if (i >= 0 && i < n)
    return i;
  if (mode == "constant")
    return -1;
  if (mode == "clamp")
    return std::max<std::int64_t>(0, std::min(n - 1, i));
  if (n == 1)
    return 0;
  const auto period = mode == "wrap"      ? n
                      : mode == "reflect" ? 2 * n
                                          : 2 * (n - 1);
  const auto r = (i % period + period) % period;
  return mode == "wrap" ? r
                        : std::min(r, period - (mode == "reflect" ? 1 : 0) - r);
}
std::array<float, 4> stmap_reference(const std::vector<float>& source,
                                     std::uint64_t height, std::uint64_t width,
                                     double u, double v,
                                     const std::string& mode) {
  // An independent scalar oracle with explicit rounding after every operation;
  // it cannot inherit the production loop's vectorization or FMA decisions.
  volatile double sx = u - .5, sy = v - .5;
  const auto left = static_cast<std::int64_t>(std::floor(sx));
  const auto top = static_cast<std::int64_t>(std::floor(sy));
  volatile double fx = sx - static_cast<double>(left),
                  fy = sy - static_cast<double>(top);
  std::array<double, 4> weights{};
  std::array<std::array<float, 4>, 4> values{};
  for (unsigned dy = 0; dy < 2; ++dy)
    for (unsigned dx = 0; dx < 2; ++dx) {
      const auto k = dy * 2 + dx;
      volatile double weight = (dy ? fy : 1 - fy) * (dx ? fx : 1 - fx);
      weights[k] = weight;
      const auto y = stmap_reference_index(top + dy, height, mode);
      const auto x = stmap_reference_index(left + dx, width, mode);
      if (x >= 0 && y >= 0) {
        for (std::size_t c = 0; c < 4; ++c)
          values[k][c] = source[(y * width + x) * 4 + c];
      }
    }
  std::array<float, 4> result{};
  for (std::size_t c = 0; c < 4; ++c) {
    double sum = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      volatile double product = static_cast<double>(values[k][c]) * weights[k];
      volatile double next = sum + product;
      sum = next;
    }
    volatile float narrowed = static_cast<float>(sum);
    result[c] = narrowed;
  }
  return result;
}
int stmap_bitwise_oracle(const std::shared_ptr<OperationRegistry>& registry) {
  struct Environment {
    std::fenv_t saved{};
    Environment() {
      std::fegetenv(&saved);
      std::fesetenv(FE_DFL_ENV);
    }
    ~Environment() { std::fesetenv(&saved); }
  } environment;
  const auto tiny = std::numeric_limits<float>::denorm_min();
  const auto normal = std::numeric_limits<float>::min();
  const std::vector<float> original{
      -1,     -0.F,  tiny,      1,        std::nextafter(1.F, 2.F),
      normal, -tiny, 1,         0x1p80F,  -0x1p80F,
      .25F,   1,     -0x1p80F,  0x1p80F,  -.25F,
      1,      1,     -1,        3 * tiny, 1,
      -1,     1,     -3 * tiny, 1};
  const std::vector<double> uv{.5,   .5,      1,      .5,  1.75,    1.25, -3.25,
                               4.75, 0x1p-56, .5,     3.5, 2.5,     -.5,  -.5,
                               1.5,  1.5,     0x1p40, .5,  -0x1p40, .5};
  PlanningOptions tiles;
  tiles.tile_extent = 2;
  for (const auto size : {std::array<std::uint64_t, 2>{2, 3}, {1, 1}, {1, 2}}) {
    const std::vector<float> pixels(original.begin(),
                                    original.begin() + size[0] * size[1] * 4);
    for (const std::string mode :
         {"constant", "clamp", "wrap", "reflect", "mirror"}) {
      StmapDriver driver(registry);
      const auto q = Footprint::all({1, 1, 2, 5, 4}).take_value();
      auto answer =
          driver.run(driver.source(image_schema(size[0], size[1]), pixels),
                     driver.map(uv, {2, 5, 2}), mode, q, {}, {}, tiles);
      PS_REQUIRE_OK(answer);
      for (std::uint64_t i = 0; i < 10; ++i) {
        const auto expected = stmap_reference(pixels, size[0], size[1],
                                              uv[i * 2], uv[i * 2 + 1], mode);
        for (std::uint64_t c = 0; c < 4; ++c) {
          std::uint32_t actual = 0;
          PS_REQUIRE_OK(read(answer.value().results.at("result"),
                             {0, 0, i / 5, i % 5, c}, &actual, 4));
          PS_CHECK(actual == float_bits(expected[c]));
        }
      }
    }
  }
  for (const int rounding :
       {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    PS_CHECK(std::fesetround(rounding) == 0);
    StmapDriver driver(registry);
    auto q = Footprint::all({1, 1, 1, 1, 4}).take_value();
    auto folded = driver.run(
        driver.source(image_schema(2, 2), {0x1p100F, 0, 0, 1, 1, 0, 0, 1,
                                           -0x1p100F, 0, 0, 1, 1, 0, 0, 1}),
        driver.map({1, 1}), "clamp", q);
    PS_REQUIRE_OK(folded);
    std::uint32_t bits = 0;
    PS_REQUIRE_OK(
        read(folded.value().results.at("result"), {0, 0, 0, 0, 0}, &bits, 4));
    PS_CHECK(bits == float_bits(.25F) && std::fegetround() == rounding);
    auto shifted =
        driver.run(driver.source(image_schema(1, 2), {1, 0, 0, 1, -1, 0, 0, 1}),
                   driver.map({0x1p-56, .5}), "wrap", q);
    PS_REQUIRE_OK(shifted);
    PS_REQUIRE_OK(
        read(shifted.value().results.at("result"), {0, 0, 0, 0, 0}, &bits, 4));
    PS_CHECK(bits == 0 && std::fegetround() == rounding);
  }
  return 0;
}

int dependencies_and_replacement() {
  StmapDriver driver;
  auto values = pixels(6);
  auto source = driver.source(image_schema(1, 6), values);
  const std::vector<double> uv{.5, .5, 1.5, .5, 4.5, .5, 5.5, .5};
  auto map = driver.map(uv, {1, 4, 2}, true);
  auto plan = driver.plan(source, map);
  auto handle = take(
      driver.context.open_demand(plan, {{{"source", source}, {"map", map}}}));
  auto answer = take(handle.request(driver.request({0, 1})));
  auto first = take(answer.dependencies.tile_record({0, 0, 0}));
  auto second = take(answer.dependencies.tile_record({0, 0, 1}));
  PS_CHECK(first && second);
  PS_CHECK(!dependency(*first, 0).whole &&
           equals(dependency(*first, 0).tiles, {0, 1}));
  PS_CHECK(equals(dependency(*second, 0).tiles, {2}));
  PS_CHECK(equals(dependency(*first, 1).tiles, {0}));
  PS_CHECK(equals(dependency(*second, 1).tiles, {1}));
  ResourceVector<TileChange> changes{
      ResourceAllocator<TileChange>(driver.root)};
  changes.push_back({PlanWorkflowInput{0}, 0,
                     take(TileSet::from_indices(driver.root, {1})), false});
  auto dirty = take(answer.dependencies.dirty_units(changes));
  PS_CHECK(dirty.size() == 1 && dirty[0].tile == 0);
  const std::vector<double> rerouted{4.5, .5, 4.5, .5, 4.5, .5, 5.5, .5};
  auto updated_map = driver.map(rerouted, {1, 4, 2}, true);
  auto replaced = take(
      handle.replace_bindings({{{"source", source}, {"map", updated_map}}}));
  PS_CHECK(equals(replaced.potential_dirty.at("result"), {0}));
  auto recomputed = take(handle.request(driver.request({0, 1})));
  auto updated_record = take(recomputed.dependencies.tile_record({0, 0, 0}));
  PS_CHECK(equals(dependency(*updated_record, 0).tiles, {2}));
  values[0] = 19;
  auto old_source_edit = driver.source(image_schema(1, 6), values);
  replaced = take(handle.replace_bindings(
      {{{"source", old_source_edit}, {"map", updated_map}}}));
  PS_CHECK(replaced.potential_dirty.at("result").empty());
  values[4 * 4] = 23;
  auto active_source_edit = driver.source(image_schema(1, 6), values);
  replaced = take(handle.replace_bindings(
      {{{"source", active_source_edit}, {"map", updated_map}}}));
  PS_CHECK(equals(replaced.potential_dirty.at("result"), {0, 1}));
  return 0;
}
int disjoint_tiles_and_whole_edits() {
  for (bool spatial : {false, true}) {
    StmapDriver driver;
    auto source = driver.source(image_schema(1, 2), pixels(2));
    std::vector<double> uv(8 * 8 * 2, .5);
    auto map = driver.map(uv, {8, 8, 2}, spatial);
    auto plan = driver.plan(source, map, "clamp", 2);
    auto handle = take(
        driver.context.open_demand(plan, {{{"source", source}, {"map", map}}}));
    auto answer = take(handle.request(driver.request({0, 5, 10, 15})));
    std::vector<Region> boxes;
    for (std::uint64_t d : {0, 2, 4, 6})
      boxes.emplace_back(
          std::vector<RegionDimension>{{0, 1}, {0, 1}, {d, 2}, {d, 2}, {0, 4}});
    const auto& result = answer.results.at("result");
    PS_CHECK(take(result.descriptor()).tensor_coverage(0) ==
             take(Footprint::from_regions({1, 1, 8, 8, 4}, boxes)));
    for (std::uint64_t y = 0; y < 8; ++y)
      for (std::uint64_t x = 0; x < 8; ++x) {
        const bool requested = y / 2 == x / 2;
        for (std::uint64_t c = 0; c < 4; ++c) {
          float value = -1;
          auto status = read(result, {0, 0, y, x, c}, &value, 4);
          PS_CHECK(status.ok() == requested);
          if (requested)
            PS_CHECK(value == (c == 0 || c == 3 ? 1.F : 0.F));
        }
      }
    // An edit outside every requested tile still invalidates a generic Whole
    // UV binding. A spatial UV binding only invalidates its actual consumers.
    uv[2 * 7] = 1.5;
    auto replacement = driver.map(uv, {8, 8, 2}, spatial);
    auto replaced = take(
        handle.replace_bindings({{{"source", source}, {"map", replacement}}}));
    if (spatial)
      PS_CHECK(replaced.potential_dirty.at("result").empty());
    else
      PS_CHECK(equals(replaced.potential_dirty.at("result"), {0, 5, 10, 15}));
  }
  return 0;
}
int validation_scope() {
  StmapDriver driver;
  const auto source = driver.source(image_schema(1, 6), pixels(6));
  const auto query = take(Footprint::from_regions(
      {1, 1, 1, 4, 4}, {Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}})}));
  PlanningOptions planning;
  planning.tile_extent = 2;
  for (bool spatial : {false, true}) {
    auto uv = std::vector<double>{.5, .5, NAN, .5, 4.5, .5, 5.5, .5};
    PS_CHECK(driver
                 .run(source, driver.map(uv, {1, 4, 2}, spatial), "clamp",
                      query, {}, {}, planning)
                 .status()
                 .code == ErrorCode::OperationFailed);
    uv[2] = 1.5;
    uv[6] = NAN;
    auto answer = driver.run(source, driver.map(uv, {1, 4, 2}, spatial),
                             "clamp", query, {}, {}, planning);
    PS_REQUIRE_OK(answer);
    auto record = take(answer.value().dependencies.tile_record({0, 0, 0}));
    PS_CHECK(dependency(*record, 1).whole == !spatial);
  }
  const auto map =
      driver.map({.5, .5, 1.5, .5, 4.5, .5, 5.5, .5}, {1, 4, 2}, true);
  for (unsigned position : {2, 3, 5}) {
    auto values = pixels(6);
    values[position * 4] = NAN;
    auto answer = driver.run(driver.source(image_schema(1, 6), values), map,
                             "clamp", query, {}, {}, planning);
    if (position == 5) {
      PS_REQUIRE_OK(answer);
    } else {
      PS_CHECK(!answer.ok() &&
               answer.status().code == ErrorCode::InvalidArgument);
    }
  }
  auto outside = driver.run(source, driver.map({-100, -100}), "constant",
                            take(Footprint::all({1, 1, 1, 1, 4})));
  PS_REQUIRE_OK(outside);
  auto record = take(outside.value().dependencies.tile_record({0, 0, 0}));
  PS_CHECK(!dependency(*record, 0).whole &&
           dependency(*record, 0).tiles.empty());
  float alpha = 1;
  check(read(outside.value().results.at("result"), {0, 0, 0, 0, 3}, &alpha, 4));
  PS_CHECK(alpha == 0);
  return 0;
}
int stmap_cpu_tiles(const std::shared_ptr<OperationRegistry>& registry) {
  constexpr std::uint64_t side = 256;
  const std::vector<float> pixels{1,  -2,  3,  1, -4,  5,  6,  1,
                                  7,  8,   -9, 1, -10, 11, 12, 1,
                                  13, -14, 15, 1, 16,  17, 18, 1};
  const std::array<std::array<double, 2>, 8> points{
      {{{.875, .75}},
       {{-.25, -.75}},
       {{2.5, 1.5}},
       {{3.875, 2.25}},
       {{.5, .5}},
       {{std::ldexp(1., -56), .5}},
       {{-1099511627776., .5}},
       {{2.125, 1.125}}}};
  std::vector<double> uv(2 * side * side);
  for (std::size_t i = 0; i < side * side; ++i) {
    uv[2 * i] = points[i % points.size()][0];
    uv[2 * i + 1] = points[i % points.size()][1];
  }
  const auto query = take(Footprint::all({1, 1, side, side, 4}));
  for (const std::string mode :
       {"constant", "clamp", "wrap", "reflect", "mirror"}) {
    std::vector<float> reference;
    std::uint64_t reference_work = 0, reference_tiles = 0;
    for (const unsigned workers : {1U, 4U}) {
      ExecutionContextConfig config;
      config.cpu_workers = workers;
      StmapDriver driver(registry, config);
      const auto source = driver.source(image_schema(2, 3), pixels);
      const auto map = driver.map(uv, {side, side, 2});
      ExecutionOptions options;
      options.maximum_parallelism = workers;
      options.maximum_dependency_work = UINT64_C(1) << 32;
      options.dependencies.maximum_work = UINT64_C(1) << 32;
      options.dependencies.sets.maximum_work = UINT64_C(1) << 32;
      const auto before = driver.root.statistics().issued.work;
      const auto rounding = std::fegetround();
      PS_CHECK(std::fesetround(FE_UPWARD) == 0);
      PlanningOptions planning;
      planning.tile_extent = 256;
      const auto answer =
          driver.run(source, map, mode, query, options, {}, planning);
      const auto restored = std::fegetround();
      PS_CHECK(std::fesetround(rounding) == 0);
      PS_REQUIRE_OK(answer);
      PS_CHECK(restored == FE_UPWARD);
      const auto work = driver.root.statistics().issued.work - before;
      const auto& diagnostics = answer.value().diagnostics;
      PS_CHECK(diagnostics.cpu_stage_count == 1);
      PS_CHECK(diagnostics.cpu_tile_callback_count > 0 &&
               diagnostics.cpu_tile_callback_count <= 64);
      const auto& result = answer.value().results.at("result");
      const auto descriptor = take(result.descriptor());
      auto window = take(
          result.acquire_tensor(descriptor, 0, Region::whole(query.shape())));
      PS_CHECK(window.sample_axis() == 3);
      std::vector<float> actual(side * side * 4);
      for (std::uint64_t y = 0; y < side; ++y)
        for (std::uint64_t c = 0; c < 4; ++c)
          for (std::uint64_t x = 0; x < side;) {
            const auto run = take(window.row_run({0, 0, y, x, c}));
            const auto n = std::min(side - x, run.samples);
            for (std::uint64_t i = 0; i < n; ++i)
              std::memcpy(&actual[((y * side + x + i) * 4) + c],
                          run.data + static_cast<std::int64_t>(i) *
                                         run.sample_stride_bytes,
                          4);
            x += n;
          }
      for (std::size_t i = 0; i < points.size(); ++i) {
        const auto expected =
            stmap_reference(pixels, 2, 3, points[i][0], points[i][1], mode);
        PS_CHECK(std::memcmp(actual.data() + i * 4, expected.data(), 16) == 0);
      }
      auto record = take(answer.value().dependencies.tile_record({0, 0, 0}));
      PS_CHECK(record && equals(dependency(*record, 0).tiles, {0}));
      PS_CHECK(dependency(*record, 1).whole);
      if (workers == 1) {
        reference = std::move(actual);
        reference_work = work;
        reference_tiles = diagnostics.cpu_tile_callback_count;
      } else {
        PS_CHECK(std::memcmp(actual.data(), reference.data(),
                             actual.size() * sizeof(float)) == 0);
        PS_CHECK(work == reference_work);
        PS_CHECK(diagnostics.cpu_tile_callback_count == reference_tiles);
      }
    }
  }
  return 0;
}
int stmap_map_rows(const std::shared_ptr<OperationRegistry>& registry) {
  constexpr std::uint64_t side = 256, count = side * side;
  const std::vector<float> pixels{1,  -2,  3,  1, 4,   5,  -6, 1,
                                  7,  8,   9,  1, -10, 11, 12, 1,
                                  13, -14, 15, 1, 16,  17, 18, 1};
  std::vector<double> uv(2 * count);
  for (std::uint64_t y = 0; y < side; ++y)
    for (std::uint64_t x = 0; x < side; ++x) {
      uv[2 * (y * side + x)] = (x % 9) * .5 - .875;
      uv[2 * (y * side + x) + 1] = (y % 7) * .5 - .75;
    }
  const auto query = take(Footprint::all({1, 1, side, side, 4}));
  std::vector<float> reference;
  for (unsigned layout = 0; layout < 4; ++layout) {
    ResourceBudget observed;
    ExecutionDependencies retained;
    {
      ExecutionContextConfig config;
      config.cpu_workers = 4;
      StmapDriver driver(registry, config);
      observed = driver.root;
      const auto source = driver.source(image_schema(2, 3), pixels);
      auto schema = multi_result::schema(ElementType::Float64, {side, side, 2});
      schema.tensors[0].layout.spatial = layout == 1 || layout == 2;
      auto builder = take(
          ResultBuilder::start(driver.root, schema, "map.rows", {}, {}, 32));
      if (layout < 2) {
        std::vector<double> physical(2 * count);
        for (std::uint64_t y = 0; y < side; ++y)
          for (std::uint64_t x = 0; x < side; ++x)
            for (std::uint64_t c = 0; c < 2; ++c) {
              const auto index =
                  layout == 0
                      ? 2 * (y * side + side - 1 - x) + c
                      : c * count + (side - 1 - y) * side + side - 1 - x;
              physical[index] = uv[2 * (y * side + x) + c];
            }
        auto buffer =
            take(driver.root.allocator().allocate(physical.size() * 8));
        std::memcpy(buffer.data(), physical.data(), physical.size() * 8);
        const auto strides =
            layout == 0 ? StridedLayout{(side - 1) * 16, {side * 16, -16, 8}}
                        : StridedLayout{(count - 1) * 8,
                                        {-static_cast<std::int64_t>(side * 8),
                                         -8, count * 8}};
        PS_REQUIRE_OK(builder.publish_tensor(0, Region::whole({side, side, 2}),
                                             strides,
                                             std::move(buffer).freeze()));
      } else if (layout == 2) {
        // Spatial tiles shorter than a logical row require the row-copy path.
        PS_REQUIRE_OK(builder.publish_tensor(
            0, Region::whole({side, side, 2}),
            ByteView(reinterpret_cast<const std::uint8_t*>(uv.data()),
                     uv.size() * 8)));
      } else {
        // One affine top half and two lower pieces mix raw and copied rows.
        for (const auto& region : {Region({{0, 128}, {0, side}, {0, 2}}),
                                   Region({{128, 128}, {0, 127}, {0, 2}}),
                                   Region({{128, 128}, {127, 129}, {0, 2}})}) {
          std::vector<double> piece;
          const auto y = region.dimensions()[0], x = region.dimensions()[1];
          for (auto row = y.offset; row < y.offset + y.extent; ++row)
            for (auto column = x.offset; column < x.offset + x.extent; ++column)
              for (std::size_t c = 0; c < 2; ++c)
                piece.push_back(uv[2 * (row * side + column) + c]);
          PS_REQUIRE_OK(builder.publish_tensor(
              0, region,
              ByteView(reinterpret_cast<const std::uint8_t*>(piece.data()),
                       piece.size() * 8)));
        }
      }
      const auto map = take(builder.seal());
      ExecutionOptions options;
      options.maximum_parallelism = 4;
      options.maximum_dependency_work = UINT64_C(1) << 32;
      options.dependencies.maximum_work = UINT64_C(1) << 32;
      options.dependencies.sets.maximum_work = UINT64_C(1) << 32;
      const auto rounding = std::fegetround();
      PS_CHECK(std::fesetround(FE_DOWNWARD) == 0);
      PlanningOptions planning;
      planning.tile_extent = 256;
      const auto answer =
          driver.run(source, map, "mirror", query, options, {}, planning);
      const auto restored = std::fegetround();
      PS_CHECK(std::fesetround(rounding) == 0);
      PS_REQUIRE_OK(answer);
      PS_CHECK(restored == FE_DOWNWARD);
      PS_CHECK(answer.value().diagnostics.cpu_stage_count == 1 &&
               answer.value().diagnostics.cpu_tile_callback_count == 64);
      const auto& result = answer.value().results.at("result");
      retained = answer.value().dependencies;
      const auto descriptor = take(result.descriptor());
      const auto window = take(
          result.acquire_tensor(descriptor, 0, Region::whole(query.shape())));
      std::vector<float> actual(4 * count);
      for (std::uint64_t y = 0; y < side; ++y)
        for (std::uint64_t c = 0; c < 4; ++c)
          for (std::uint64_t x = 0; x < side;) {
            const auto run = take(window.row_run({0, 0, y, x, c}));
            const auto n = std::min(side - x, run.samples);
            for (std::uint64_t i = 0; i < n; ++i)
              std::memcpy(&actual[4 * (y * side + x + i) + c],
                          run.data + static_cast<std::int64_t>(i) *
                                         run.sample_stride_bytes,
                          4);
            x += n;
          }
      for (const auto y : {0U, 1U, 127U, 128U, 255U})
        for (const auto x : {0U, 1U, 63U, 126U, 127U, 128U, 255U}) {
          const auto expected =
              stmap_reference(pixels, 2, 3, uv[2 * (y * side + x)],
                              uv[2 * (y * side + x) + 1], "mirror");
          PS_CHECK(std::memcmp(actual.data() + 4 * (y * side + x),
                               expected.data(), 16) == 0);
        }
      if (!layout)
        reference = std::move(actual);
      else
        PS_CHECK(std::memcmp(actual.data(), reference.data(), 16 * count) == 0);
      auto record = take(retained.tile_record({0, 0, 0}));
      PS_CHECK(record && equals(dependency(*record, 0).tiles, {0}));
    }
    // Escaped actual-Need records own bounded metadata, with no payload.
    const auto live = observed.statistics().live;
    PS_CHECK(live[ResourceKind::Payload] == 0);
    PS_CHECK(live[ResourceKind::Metadata] > 0);
    PS_CHECK(take(retained.tile_record({0, 0, 0}))->inputs.size() == 2);
    retained = {};
    for (auto live : observed.statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
int batches_and_lifetime() {
  for (bool batched : {false, true}) {
    ResourceBudget observed;
    ResultRef retained;
    ExecutionDependencies records;
    {
      StmapDriver driver;
      observed = driver.root;
      auto schema = image_schema(1, 3, 2);
      schema.tensors[0].batch_axes = {2, 2};
      std::vector<float> source_values(4 * 3 * 4);
      for (unsigned f = 0; f < 2; ++f)
        for (unsigned l = 0; l < 2; ++l)
          for (unsigned x = 0; x < 3; ++x) {
            auto at = ((2 * f + l) * 3 + x) * 4;
            source_values[at] = 100 * f + 10 * l + x;
            source_values[at + 3] = 1;
          }
      std::vector<double> uv;
      for (unsigned batch = 0; batch < (batched ? 4U : 1U); ++batch)
        for (unsigned x = 0; x < 2; ++x) {
          uv.push_back((batch + x) % 3 + .5);
          uv.push_back(.5);
        }
      auto source = driver.source(schema, source_values);
      auto map = driver.map(uv, {1, 2, 2}, true,
                            batched ? std::vector<std::uint64_t>{2, 2}
                                    : std::vector<std::uint64_t>{});
      PlanningOptions planning;
      planning.tile_extent = 1;
      auto answer = take(driver.run(source, map, "clamp",
                                    take(Footprint::all({2, 2, 1, 2, 4})), {},
                                    {}, planning));
      retained = answer.results.at("result");
      records = answer.dependencies;
      PS_CHECK(!retained.association().empty());
      for (unsigned f = 0; f < 2; ++f)
        for (unsigned l = 0; l < 2; ++l)
          for (unsigned x = 0; x < 2; ++x) {
            auto record =
                take(records.tile_record({0, 0, (2 * f + l) * 2 + x}));
            const auto& tiles = dependency(*record, 1).tiles;
            PS_CHECK(tiles.size() == 1 &&
                     *tiles.begin() == (batched ? (2 * f + l) * 2 + x : x));
          }
    }
    for (unsigned f = 0; f < 2; ++f)
      for (unsigned l = 0; l < 2; ++l)
        for (unsigned x = 0; x < 2; ++x) {
          float value = 0;
          check(read(retained, {f, l, 0, x, 0}, &value, 4));
          PS_CHECK(value ==
                   100 * f + 10 * l + ((batched ? 2 * f + l : 0) + x) % 3);
        }
    retained = {};
    PS_CHECK(observed.statistics().live[ResourceKind::Payload] == 0);
    PS_CHECK(take(records.tile_record({0, 0, 7})) != nullptr);
    records = {};
    for (auto live : observed.statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
int empty_and_huge_domains() {
  ExecutionContextConfig config;
  ResourceLimits limits;
  // The overflow-size map needs extent512 to keep its tile count representable;
  // its complete output tile has 262144 pixels of bounded STMap workspace.
  limits.capacity[ResourceKind::Metadata] = 64 * 1024 * 1024;
  config.managed_resources = limits;
  StmapDriver driver(registry(), config);
  auto source = driver.source(image_schema(1, 3), pixels(3));
  const auto output = take(Footprint::all({1, 1, 1, 1, 4}));
  PS_CHECK(driver.run(source, driver.map({0x1p41, .5}), "clamp", output)
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(
      driver.run(source, driver.map({.5, .5, .5}, {1, 1, 3}), "clamp", output)
          .status()
          .code == ErrorCode::TypeMismatch);
  auto empty = take(Footprint::none(output.shape()));
  PS_CHECK(driver.run(source, driver.map({NAN, NAN}), "bogus", empty)
               .status()
               .code == ErrorCode::InvalidArgument);
  auto no_pixels = driver.run(source, driver.map({NAN, NAN}), "clamp", empty);
  PS_REQUIRE_OK(no_pixels);
  PS_CHECK(!take(no_pixels.value().dependencies.tile_record({0, 0, 0})));
  PS_CHECK(take(no_pixels.value().results.at("result").descriptor())
               .tensor_coverage(0)
               .empty());

  auto huge_schema = image_schema(UINT64_C(1) << 40, UINT64_C(1) << 40);
  auto buffer = take(driver.root.allocator().allocate(16));
  const float rgba[] = {1, 2, 3, 1};
  std::memcpy(buffer.data(), rgba, 16);
  auto builder =
      take(ResultBuilder::start(driver.root, huge_schema, "huge.source"));
  check(builder.publish_tensor(
      0, Region::whole(huge_schema.tensors[0].sample_shape()),
      {0, {0, 0, 0, 0, 4}}, std::move(buffer).freeze()));
  auto huge = take(builder.seal());
  auto far = driver.map({0x1p40 - .5, 0x1p40 - .5});
  PS_CHECK(driver.run(huge, far, "clamp", output).status().code ==
           ErrorCode::ResourceExhausted);
  ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(1) << 32;
  options.dependencies.maximum_work = UINT64_C(1) << 32;
  options.dependencies.sets.maximum_work = UINT64_C(1) << 32;
  PlanningOptions planning;
  planning.tile_extent = 512;
  auto sampled = driver.run(huge, far, "clamp", output, options, {}, planning);
  PS_REQUIRE_OK(sampled);
  float blue = 0;
  check(read(sampled.value().results.at("result"), {0, 0, 0, 0, 2}, &blue, 4));
  PS_CHECK(blue == 3);

  for (unsigned exponent : {30, 40}) {
    const std::uint64_t extent = UINT64_C(1) << exponent;
    auto map_schema =
        multi_result::schema(ElementType::Float64, {extent, extent, 2});
    auto coordinates = take(driver.root.allocator().allocate(16));
    const double uv[] = {.5, .5};
    std::memcpy(coordinates.data(), uv, 16);
    auto map_builder =
        take(ResultBuilder::start(driver.root, map_schema, "huge.map"));
    check(map_builder.publish_tensor(0, Region::whole({extent, extent, 2}),
                                     {0, {0, 0, 8}},
                                     std::move(coordinates).freeze()));
    auto map = take(map_builder.seal());
    const auto selected = take(Footprint::from_regions(
        {1, 1, extent, extent, 4},
        {Region({{0, 1}, {0, 1}, {extent - 1, 1}, {extent - 1, 1}, {0, 4}})}));
    planning.tile_extent = exponent == 30 ? 1 : 512;
    auto distant =
        driver.run(source, map, "clamp", selected, options, {}, planning);
    PS_REQUIRE_OK(distant);
    float value = 0;
    check(read(distant.value().results.at("result"),
               {0, 0, extent - 1, extent - 1, 0}, &value, 4));
    PS_CHECK(value == 1);
  }
  return 0;
}
int resources_and_scaling() {
  const auto native_page = sysconf(_SC_PAGESIZE);
  PS_CHECK(native_page > 0);
  const auto page = static_cast<std::uint64_t>(native_page);
  for (std::uint64_t extent : {16, 128}) {
    std::uint64_t previous = 0;
    for (std::uint64_t side : {8, 16, 32, 64}) {
      ResourceBudget observed;
      {
        StmapDriver driver;
        observed = driver.root;
        auto source = driver.source(image_schema(1, 3), pixels(3));
        auto map = driver.map(std::vector<double>(2 * side * side, .5),
                              {side, side, 2}, true);
        auto output = take(Footprint::all({1, 1, side, side, 4}));
        ExecutionOptions options;
        options.maximum_dependency_work = UINT64_C(1) << 32;
        options.dependencies.maximum_work = UINT64_C(1) << 32;
        options.dependencies.sets.maximum_work = UINT64_C(1) << 32;
        PlanningOptions planning;
        planning.tile_extent = extent;
        const auto before = driver.root.statistics();
        {
          auto answer =
              driver.run(source, map, "clamp", output, options, {}, planning);
          PS_REQUIRE_OK(answer);
          const auto work =
              driver.root.statistics().issued.work - before.issued.work;
          std::cout << "STMap extent=" << extent << " side=" << side
                    << " work=" << work << '\n';
          if (previous)
            PS_CHECK(work < 5 * previous);
          previous = work;
          PS_CHECK(work < 1600 * side * side);
          const auto payload =
              driver.root.statistics().live[ResourceKind::Payload] -
              before.live[ResourceKind::Payload];
          const auto tiles =
              ((side + extent - 1) / extent) * ((side + extent - 1) / extent);
          std::uint64_t callbacks = 0;
          for (const auto& operation :
               answer.value().diagnostics.operation_timings)
            callbacks += operation.invocation_count;
          PS_CHECK(callbacks == 3 * tiles);
          // Each independently owned tile needs one host page per RGBA plane.
          // Keep the original absolute bound for the original single-unit case.
          if (extent == 128)
            PS_CHECK(payload < UINT64_C(1048576));
          else
            PS_CHECK(payload == tiles * 4 * page);
          std::cout << "  payload=" << payload << " units=" << tiles << '\n';
          const auto& result = answer.value().results.at("result");
          PS_CHECK(take(result.descriptor()).tensor_coverage(0) == output);
          for (std::uint64_t y = 0; y < side; ++y)
            for (std::uint64_t x = 0; x < side; ++x)
              for (std::uint64_t c = 0; c < 4; ++c) {
                float value = -1;
                check(read(result, {0, 0, y, x, c}, &value, 4));
                PS_CHECK(value == (c == 0 || c == 3 ? 1.F : 0.F));
              }
        }
        PS_CHECK(driver.root.statistics().live[ResourceKind::Payload] ==
                 before.live[ResourceKind::Payload]);
        options.maximum_dependency_work = 1;
        auto exhausted =
            driver.run(source, map, "clamp", output, options, {}, planning);
        PS_CHECK(!exhausted.ok() &&
                 exhausted.status().code == ErrorCode::ResourceExhausted);
        CancellationSource cancellation;
        cancellation.cancel();
        auto cancelled = driver.run(source, map, "clamp", output, {},
                                    cancellation.token(), planning);
        PS_CHECK(!cancelled.ok() &&
                 cancelled.status().code == ErrorCode::Cancelled);
      }
      for (auto live : observed.statistics().live.values)
        PS_CHECK(live == 0);
    }
  }
  return 0;
}
}  // namespace
int main() {
  try {
    return stmap_bitwise_oracle(registry()) || dependencies_and_replacement() ||
           validation_scope() || disjoint_tiles_and_whole_edits() ||
           stmap_cpu_tiles(registry()) || stmap_map_rows(registry()) ||
           batches_and_lifetime() || empty_and_huge_domains() ||
           resources_and_scaling();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  } catch (const Status& status) {
    std::cerr << status.message << '\n';
    return 1;
  }
}
