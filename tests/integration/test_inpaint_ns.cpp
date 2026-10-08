#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef PHOTOSPIDER_TEST_INPAINT_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/photo.hpp>
#endif
#include "inpaint_ns_workflow/workflow.hpp"

namespace {
using namespace inpaint_example;  // NOLINT(build/namespaces)
using Params = std::map<std::string, ps::ParameterValue>;
const char native_key[] =
    "image.local_inpaint_navier_stokes_native_apple_silicon";  // NOLINT(whitespace/indent_namespace)
std::vector<std::string> keys{native_key
#ifdef PHOTOSPIDER_TEST_INPAINT_OPENCV
                              ,
                              "image.local_inpaint_navier_stokes_openCV"
#endif
};
std::vector<Input> inputs(const std::vector<float>& rgb,
                          const std::vector<float>& mask, int h, int w) {
  return {value(rgb, {static_cast<unsigned>(h), static_cast<unsigned>(w), 4},
                ps::rgba_semantics()),
          value(mask, {static_cast<unsigned>(h), static_cast<unsigned>(w)},
                ps::coverage_semantics())};
}
ps::Result<ps::ResultRef> invoke(const std::string& key,
                                 const std::vector<Input>& values,
                                 Params params = {{"radius", std::int64_t{3}}},
                                 ps::ResourceLimits limits = {},
                                 ps::CancellationToken token = {}) {
  auto result = run(key, values, params, {}, token, limits);
  return result.ok()
             ? ps::Result<ps::ResultRef>(result.value().results.at("result"))
             : ps::Result<ps::ResultRef>(result.status());
}
void reject(ps::Result<ps::ResultRef> result, ps::ErrorCode code) {
  check(!result.ok() && result.status().code == code,
        "rejection code: " + result.status().message);
}
std::vector<float> oracle(const std::vector<float>& rgb,
                          const std::vector<float>& mask, int h, int w,
                          int radius) {
#ifdef PHOTOSPIDER_TEST_INPAINT_OPENCV
  std::vector<float> out = rgb;
  cv::Mat holes(h, w, CV_8UC1), source(h, w, CV_32FC1), target;
  for (int i = 0; i < h * w; ++i)
    holes.ptr<unsigned char>()[i] = mask[i] == 0 ? 0 : 255;
  for (int c = 0; c < 3; ++c) {
    for (int i = 0; i < h * w; ++i)
      source.ptr<float>()[i] = mask[i] == 0 ? rgb[i * 4 + c] : 0.F;
    cv::inpaint(source, holes, target, radius, cv::INPAINT_NS);
    for (int i = 0; i < h * w; ++i)
      if (mask[i])
        out[i * 4 + c] = target.ptr<float>()[i];
  }
  return out;
#else
  (void)mask;
  (void)h;
  (void)w;
  (void)radius;
  return rgb;
#endif
}
void numerical() {
  for (const auto& key : keys) {
    std::vector<float> constant(100), mask(25);
    for (unsigned i = 0; i < 100; ++i)
      constant[i] = (i % 4 + 1) * .25F;
    mask[12] = 1;
    const auto actual = pixels(take(invoke(key, inputs(constant, mask, 5, 5),
                                           {{"radius", std::int64_t{1}}})));
    for (unsigned i = 0; i < 100; ++i)
      check(std::abs(actual[i] - constant[i]) <= 1e-6,
            "T02 5x5 r1 analytic constant");
  }
  unsigned cases = 0;
  for (const auto& key : keys)
    for (int size : {3, 5, 11, 17})
      for (int radius : {1, 3, 8, 32}) {
        const int h = size, w = size + 2;
        for (int pattern = 0; pattern < 9; ++pattern) {
#ifndef PHOTOSPIDER_TEST_INPAINT_OPENCV
          if (pattern != 0)
            continue;
#endif
          std::vector<float> rgb(h * w * 4), mask(h * w);
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
              const int i = y * w + x;
              for (int c = 0; c < 4; ++c)
                rgb[i * 4 + c] =
                    c == 3 ? 1.F
                    : pattern == 0
                        ? static_cast<float>(c + 1) * .25F
                        : (static_cast<float>((x * 17 + y * 29 + c * 3) % 37) -
                           18.F) /
                              7.F;
              if (pattern == 0)
                mask[i] = (i == (h * w) / 2);
              if (pattern == 1)
                mask[i] = (x == w / 2);
              if (pattern == 2)
                mask[i] = (x == 0 || y == 0);
              if (pattern == 3)
                mask[i] = (x == w - 1 || y == h - 1);
              if (pattern == 4)
                mask[i] = (i % 3 == 0);
              if (pattern == 5)
                mask[i] =
                    (x > w / 3 && x < 2 * w / 3 && y > h / 3 && y < 2 * h / 3);
              if (pattern == 6)
                mask[i] = (i != 0);
              if (pattern == 7)
                mask[i] = (i != h * w / 2);
              if (pattern == 8)
                mask[i] = (x + y) % 2;
            }
          const auto expected = oracle(rgb, mask, h, w, radius);
          const auto values = inputs(rgb, mask, h, w);
          const auto actual_value =
              take(invoke(key, values, {{"radius", std::int64_t{radius}}}));
          const auto actual = pixels(actual_value);
          for (int i = 0; i < h * w * 4; ++i) {
            if (!std::isfinite(actual[i]) ||
                std::abs(static_cast<double>(actual[i]) - expected[i]) >
                    1e-6 + 1e-5 * std::abs(expected[i]))
              throw std::runtime_error("oracle mismatch " + key +
                                       " size=" + std::to_string(size) +
                                       " pattern=" + std::to_string(pattern) +
                                       " r=" + std::to_string(radius) +
                                       " sample=" + std::to_string(i));
            if (!mask[i / 4] || i % 4 == 3)
              check(std::memcmp(&actual[i], &rgb[i], 4) == 0, "unmasked bits");
          }
          auto changed = rgb;
          for (int i = 0; i < h * w; ++i)
            if (mask[i]) {
              for (int c = 0; c < 3; ++c)
                changed[i * 4 + c] = 100.F + c;
            }
          check(bytes(take(invoke(key, inputs(changed, mask, h, w),
                                  {{"radius", std::int64_t{radius}}}))) ==
                    bytes(actual_value),
                "placeholder independence");
          ++cases;
        }
      }
  std::cout << "numerical fixture/radius/variant cases=" << cases << " PASS\n";
}
void validation() {
  for (const auto& key : keys) {
    std::vector<float> rgb(100, .25F), mask(25);
    for (unsigned i = 3; i < 100; i += 4)
      rgb[i] = 1;
    rgb[0] = -0.F;
    mask[0] = -0.F;
    auto values = inputs(rgb, mask, 5, 5);
    check(bytes(take(invoke(key, values))) == bytes(values[0]),
          "zero mask bits");
    mask[12] = 1;
    values = inputs(rgb, mask, 5, 5);
    for (Params params :
         {Params{}, Params{{"radius", 3.}}, Params{{"radius", std::int64_t{0}}},
          Params{{"radius", std::int64_t{33}}},
          Params{{"radius", std::int64_t{3}}, {"method", std::int64_t{1}}}})
      reject(invoke(key, values, params), ps::ErrorCode::InvalidArgument);
    for (float bad :
         {.5F, -1.F, 2.F, std::numeric_limits<float>::quiet_NaN()}) {
      auto invalid = mask;
      invalid[0] = bad;
      reject(invoke(key, inputs(rgb, invalid, 5, 5)),
             bad == .5F ? ps::ErrorCode::OperationFailed
                        : ps::ErrorCode::InvalidArgument);
    }
    for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity()}) {
      auto invalid = rgb;
      invalid[0] = bad;
      reject(invoke(key, inputs(invalid, std::vector<float>(25), 5, 5)),
             ps::ErrorCode::InvalidArgument);
    }
    auto invalid = rgb;
    invalid[3] = .5F;
    reject(invoke(key, inputs(invalid, mask, 5, 5)),
           ps::ErrorCode::OperationFailed);
    reject(invoke(key, inputs(rgb, std::vector<float>(25, 1), 5, 5)),
           ps::ErrorCode::OperationFailed);
    for (int size : {1, 2}) {
      std::vector<float> tiny(size * 5 * 4, .25F);
      for (unsigned i = 3; i < tiny.size(); i += 4)
        tiny[i] = 1;
      reject(invoke(key, inputs(tiny, std::vector<float>(size * 5), size, 5)),
             ps::ErrorCode::TypeMismatch);
    }
    const auto baseline = bytes(take(invoke(key, values)));
    const auto original = std::fegetround();
    std::fesetround(FE_UPWARD);
    const auto rounded = bytes(take(invoke(key, values)));
    check(std::fegetround() == FE_UPWARD, "floating mode restore");
    std::fesetround(original);
    check(rounded == baseline, "rounding independent bits");
    auto concurrent = std::async(
        std::launch::async, [&] { return bytes(take(invoke(key, values))); });
    check(bytes(take(invoke(key, values))) == baseline &&
              concurrent.get() == baseline,
          "concurrent outputs");
    ps::CancellationSource cancel;
    cancel.cancel();
    reject(invoke(key, values, {{"radius", std::int64_t{3}}},
                  ps::ResourceLimits{}, cancel.token()),
           ps::ErrorCode::Cancelled);
    auto display = ps::rgba_semantics();
    display.reference = "display";
    auto display_inputs = values;
    display_inputs[0] = value(rgb, {5, 5, 4}, display);
    check(take(invoke(key, display_inputs))
                  .schema()
                  .tensors[0]
                  .facets[0]
                  .payload ==
              display_inputs[0].description.tensors[0].facets[0].payload,
          "display reference preserved");
    auto extreme = rgb;
    for (unsigned i = 0; i < 25; ++i)
      for (unsigned c = 0; c < 3; ++c)
        extreme[i * 4 + c] = i % 2 ? 1e30F : -1e30F;
    reject(invoke(key, inputs(extreme, mask, 5, 5)),
           ps::ErrorCode::OperationFailed);
    const auto workflow = take(run(key, values, {{"radius", std::int64_t{3}}}));
    check(bytes(workflow.results.at("result")) == baseline,
          "public workflow output");
    ps::PlanningOptions roi;
    roi.output_regions = {
        {"result", ps::Region({{0, 1}, {0, 1}, {1, 3}, {1, 2}, {0, 4}})}};
    roi.tile_height = 1;
    roi.tile_width = 1;
    auto distant = rgb;
    distant[0] = std::numeric_limits<float>::quiet_NaN();
    auto invalid_roi = run(key, inputs(distant, mask, 5, 5),
                           {{"radius", std::int64_t{3}}}, roi);
    check(!invalid_roi.ok() &&
              invalid_roi.status().code == ps::ErrorCode::InvalidArgument,
          "distant invalid pixel rejected for ROI");
    const auto result =
        take(run(key, values, {{"radius", std::int64_t{3}}}, roi))
            .results.at("result");
    const auto window = take(result.acquire_tensor(
        take(result.descriptor()), 0, roi.output_regions.at("result")));
    std::vector<float> crop(3 * 2 * 4);
    for (std::uint64_t y = 0; y < 3; ++y)
      for (std::uint64_t x = 0; x < 2; ++x)
        for (std::uint64_t c = 0; c < 4; ++c) {
          const auto row = take(window.row_run({0, 0, y + 1, x + 1, c}));
          std::memcpy(&crop[(y * 2 + x) * 4 + c], row.data, 4);
        }
    const auto full = pixels(workflow.results.at("result"));
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 2; ++x)
        for (int c = 0; c < 4; ++c)
          check(
              crop[(y * 2 + x) * 4 + c] == full[((y + 1) * 5 + x + 1) * 4 + c],
              "Whole crop");
  }
  std::cout
      << "validation, rounding, concurrency, public workflow and ROI PASS\n";
}
void cache_policy() {
  std::vector<float> rgb(100, .25F), mask(25);
  for (unsigned i = 3; i < 100; i += 4)
    rgb[i] = 1;
  mask[12] = 1;
  const auto values = inputs(rgb, mask, 5, 5);
  for (const auto& key : keys) {
    auto registry = ps::make_default_operation_registry();
    ps::ExecutionContextConfig config;
    config.result_cache_bytes = 1024 * 1024;
    ps::ExecutionContext context(registry, config);
    auto prepared = prepare(take(context.resource_budget()), key, values,
                            {{"radius", std::int64_t{3}}});
    ps::GraphContext graph(prepared.document);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    const auto& bindings = prepared.bindings;
    const auto first =
        take(context.execute(compiled.plan, bindings, {}, options()));
    const auto second =
        take(context.execute(compiled.plan, bindings, {}, options()));
    check(
        bytes(first.results.at("result")) == bytes(second.results.at("result")),
        "cached-context repeated output");
    check(key == native_key ? second.diagnostics.cache_hits > 0
                            : second.diagnostics.cache_hits == 0,
          "native caches while the external-library adapter recomputes");
    const auto support = take(first.dependencies.source_support());
    for (unsigned port = 0; port < 2; ++port) {
      const auto name = "input" + std::to_string(port);
      const auto shape = values[port].description.tensors[0].sample_shape();
      check(support.at(name.c_str()) == take(ps::Footprint::all(shape)),
            "Whole retains both complete input demands");
      std::vector<ps::RegionDimension> point(shape.size(), {0, 1});
      const auto changed =
          take(ps::Footprint::from_regions(shape, {ps::Region(point)}));
      const auto dirty = take(first.dependencies.potential_dirty(
          name, changed, 1, {}, ps::ResultSupportTarget::Tensor, 0));
      check(dirty.at("result") == take(ps::Footprint::all({1, 1, 5, 5, 4})),
            "Whole dirty propagation covers the complete output");
    }
    for (unsigned change = 0; change < 4; ++change) {
      auto changed_rgb = rgb, changed_mask = mask;
      auto reference = ps::rgba_semantics();
      if (change == 0)
        changed_rgb[0] = .75F;
      if (change == 1) {
        changed_mask[12] = 0;
        changed_mask[0] = 1;
      }
      if (change == 3)
        reference.reference = "display";
      const auto changed_values = std::vector<Input>{
          value(changed_rgb, {5, 5, 4}, reference),
          value(changed_mask, {5, 5}, ps::coverage_semantics())};
      const Params parameters{{"radius", std::int64_t{change == 2 ? 1 : 3}}};
      auto update = prepare(take(context.resource_budget()), key,
                            changed_values, parameters);
      ps::GraphContext updated_graph(update.document);
      auto updated_plan = take(ps::Compiler(registry).compile(updated_graph));
      const auto actual = take(
          context.execute(updated_plan.plan, update.bindings, {}, options()));
      const auto expected = take(invoke(key, changed_values, parameters));
      check(actual.diagnostics.cache_hits == 0 &&
                bytes(actual.results.at("result")) == bytes(expected),
            "changed image, mask, radius or reference cannot reuse stale "
            "completion");
      check(actual.results.at("result").schema().same_schema(expected.schema()),
            "changed metadata preserved through cache lookup");
    }
  }
  std::cout << "result cache policy backends=" << keys.size() << " PASS\n";
}
void resources() {
  std::vector<float> rgb(100, .25F), mask(25);
  mask[12] = 1;
  for (unsigned i = 3; i < 100; i += 4)
    rgb[i] = 1;
  const auto values = inputs(rgb, mask, 5, 5);
  ps::ResourceStatistics measured;
  {
    auto success = run(native_key, values, {{"radius", std::int64_t{3}}}, {},
                       {}, {}, &measured);
    check(success.ok(), success.status().message);
  }
  const auto minimum = measured.peak[ps::ResourceKind::Payload];
  ps::ResourceLimits limits;
  limits.capacity[ps::ResourceKind::Payload] = minimum;
  check(invoke(native_key, values, {{"radius", std::int64_t{3}}}, limits).ok(),
        "measured native Root payload capacity succeeds");
  // Exhaust output, then each of the five private native scratch buffers.
  // These are cumulative payload capacities; source and continuation owners
  // remain present throughout the callback and are included in the peak.
  const std::uint64_t trailing[] = {770, 745, 645, 596, 400, 0};
  for (auto tail : trailing) {
    limits.capacity[ps::ResourceKind::Payload] = minimum - tail - 1;
    auto failed = run(native_key, values, {{"radius", std::int64_t{3}}}, {}, {},
                      limits, &measured);
    check(!failed.ok() &&
              failed.status().code == ps::ErrorCode::ResourceExhausted,
          "native allocation boundary fails with ResourceExhausted");
    check(measured.live[ps::ResourceKind::Payload] == 0,
          "failed Result invocation releases source, state and scratch owners");
  }
  limits = {};
  check(run(native_key, inputs(rgb, std::vector<float>(25), 5, 5),
            {{"radius", std::int64_t{3}}}, {}, {}, {}, &measured)
            .ok(),
        "measure source, validation and identity work");
  limits.maximum_work = measured.issued.work;
  auto failed = run(native_key, values, {{"radius", std::int64_t{3}}}, {}, {},
                    limits, &measured);
  check(!failed.ok() &&
            failed.status().code == ps::ErrorCode::ResourceExhausted &&
            measured.live[ps::ResourceKind::Payload] == 0,
        "native algorithm work admission releases all payload");
  std::cout << "native Root peak=" << minimum
            << "; six capacity boundaries and work exhaustion PASS\n";
}
void layouts() {
  std::vector<float> rgb(100, .25F), mask(25);
  mask[12] = 1;
  for (unsigned i = 3; i < 100; i += 4)
    rgb[i] = 1;
  for (const auto& key : keys)
    for (int mode = 0; mode < 3; ++mode) {
      auto values = inputs(rgb, mask, 5, 5);
      for (unsigned port = 0; port < 2; ++port) {
        const int channels = port == 0 ? 4 : 1;
        const int row = 5 * channels * 4 + 16;
        ps::StridedLayout layout;
        layout.byte_offset = mode == 1 ? 16 + 4 * row : 16;
        layout.byte_strides =
            mode == 1
                ? std::vector<std::int64_t>{-row, channels * 4}
                : std::vector<std::int64_t>{mode == 2 ? 0 : row, channels * 4};
        if (port == 0)
          layout.byte_strides.push_back(4);
        // A nonzero origin with compensating offset exercises logical
        // subtraction.
        if (mode == 0) {
          layout.origin = port == 0 ? std::vector<std::uint64_t>{2, 1, 0}
                                    : std::vector<std::uint64_t>{2, 1};
          layout.byte_offset += 2 * row + channels * 4;
        }
        std::vector<std::uint8_t> bytes(5 * row + 64);
        const auto& numbers = port == 0 ? rgb : mask;
        for (int y = 0; y < 5; ++y)
          for (int x = 0; x < 5; ++x)
            for (int c = 0; c < channels; ++c) {
              const int address = 16 +
                                  (mode == 1   ? 4 - y
                                   : mode == 2 ? 0
                                               : y) *
                                      row +
                                  (x * channels + c) * 4;
              float number = numbers[(y * 5 + x) * channels + c];
              if (mode == 2 && port == 1)
                number = x == 2 ? 1.F : 0.F;
              std::memcpy(bytes.data() + address, &number, 4);
            }
        layout.byte_strides.insert(layout.byte_strides.begin(), {0, 0});
        if (!layout.origin.empty())
          layout.origin.insert(layout.origin.begin(), {0, 0});
        values[port].layout = layout;
        values[port].data = bytes;
      }
      const auto image_before = bytes(values[0]),
                 mask_before = bytes(values[1]);
      const auto result = take(invoke(key, values));
      check(bytes(values[0]) == image_before && bytes(values[1]) == mask_before,
            "immutable views");
      auto dense = inputs(pixels(values[0]), pixels(values[1]), 5, 5);
      check(bytes(result) == bytes(take(invoke(key, dense))),
            "strided equivalence");
    }
  std::cout << "padded offset origin negative and zero stride invocation views "
               "PASS\n";
}
void batch_and_empty() {
  for (const auto& key : keys) {
    std::vector<float> rgb(400), mask(100);
    for (unsigned cell = 0; cell < 4; ++cell)
      for (unsigned i = 0; i < 25; ++i) {
        for (unsigned c = 0; c < 4; ++c)
          rgb[(cell * 25 + i) * 4 + c] = c == 3 ? 1.F : (cell + c + 1) * .125F;
        mask[cell * 25 + i] = cell && (cell == 1   ? i == 12
                                       : cell == 2 ? i % 5 == 0
                                                   : i % 2 == 0);
      }
    const auto batched = std::vector<Input>{
        value(rgb, {5, 5, 4}, ps::rgba_semantics(), {2, 2}),
        value(mask, {5, 5}, ps::coverage_semantics(), {2, 2})};
    const auto full = bytes(take(invoke(key, batched)));
    for (unsigned cell = 0; cell < 4; ++cell) {
      auto independent = inputs(
          {rgb.begin() + cell * 100, rgb.begin() + (cell + 1) * 100},
          {mask.begin() + cell * 25, mask.begin() + (cell + 1) * 25}, 5, 5);
      const auto reference = bytes(take(invoke(key, independent)));
      check(!std::memcmp(full.data() + cell * 400, reference.data(), 400),
            "batch cells retain independent solver order");
    }
    auto mismatch = batched;
    mismatch[1].description.tensors[0].batch_axes = {1, 4};
    reject(invoke(key, mismatch), ps::ErrorCode::TypeMismatch);
    auto full_mask = mask;
    std::fill(full_mask.begin() + 75, full_mask.end(), 1.F);
    reject(invoke(key, {batched[0], value(full_mask, {5, 5},
                                          ps::coverage_semantics(), {2, 2})}),
           ps::ErrorCode::OperationFailed);
    rgb.back() = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = std::vector<Input>{
        value(rgb, {5, 5, 4}, ps::rgba_semantics(), {2, 2}), batched[1]};
    ps::PlanningOptions planning;
    planning.output_regions = {
        {"result", ps::Region({{0, 1}, {0, 1}, {2, 1}, {2, 1}, {0, 4}})}};
    auto failed = run(key, invalid, {{"radius", std::int64_t{3}}}, planning);
    check(
        !failed.ok() && failed.status().code == ps::ErrorCode::InvalidArgument,
        "ROI validates other batch cells");
    auto registry = ps::make_default_operation_registry();
    ps::ExecutionContext context(registry);
    const auto root = take(context.resource_budget());
    auto prepared = prepare(root, key, invalid, {{"radius", std::int64_t{3}}});
    ps::GraphContext graph(prepared.document);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    auto frozen = take(context.freeze(compiled.plan, prepared.bindings));
    const auto before = root.statistics().peak[ps::ResourceKind::Payload];
    auto empty = take(context.execute_fragments(
        frozen, {{"result", take(ps::Footprint::none({2, 2, 5, 5, 4}))}}, {},
        options()));
    check(take(empty.results.at("result").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              root.statistics().peak[ps::ResourceKind::Payload] == before,
          "Empty validates static metadata without reading invalid samples or "
          "allocating state/payload");
  }
  auto registry = ps::make_default_operation_registry();
  for (auto extent : {std::uint64_t{32768}, std::uint64_t{32769}, UINT64_MAX}) {
    ps::OperationMetadata image, mask;
    image.result_schema = std::make_shared<ps::SchemaTemplate>(
        schema({extent, extent, 4}, ps::rgba_semantics()));
    mask.result_schema = std::make_shared<ps::SchemaTemplate>(
        schema({extent, extent}, ps::coverage_semantics()));
    const auto resolved = registry->resolve_traits(
        native_key, {image, mask}, {{"radius", std::int64_t{32}}});
    check(resolved.ok() == (extent == 32768),
          "static shape boundary without sample allocation");
  }
  std::cout << "batch isolation, Empty no-read and static huge metadata PASS\n";
}
void active_cancellation() {
  ps::ResourceBudget observed;
  bool entered = false;
  {
    const unsigned side = 512, count = side * side;
    std::vector<float> rgb(count * 4, .25F), mask(count, 1);
    for (unsigned i = 3; i < rgb.size(); i += 4)
      rgb[i] = 1;
    mask[0] = 0;
    auto registry = ps::make_default_operation_registry();
    ps::ExecutionContext context(registry);
    observed = take(context.resource_budget());
    auto prepared = prepare(observed, native_key, inputs(rgb, mask, side, side),
                            {{"radius", std::int64_t{32}}});
    ps::GraphContext graph(prepared.document);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    ps::CancellationSource stop;
    const auto before = observed.statistics().issued.work;
    auto running = std::async(std::launch::async, [&] {
      return context.execute(compiled.plan, prepared.bindings, stop.token(),
                             options());
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           running.wait_for(std::chrono::milliseconds(0)) !=
               std::future_status::ready) {
      if (observed.statistics().issued.work - before > UINT64_C(1000000000)) {
        entered = true;
        break;
      }
      std::this_thread::yield();
    }
    stop.cancel();
    auto result = running.get();
    check(entered && !result.ok() &&
              result.status().code == ps::ErrorCode::Cancelled,
          "cancel after numerical work admission returns no successful Result");
  }
  check(observed.statistics().live[ps::ResourceKind::Payload] == 0,
        "active cancellation releases all Root payload");
  std::cout << "active native cancellation after algorithm admission PASS\n";
}
}  // namespace
int main() {
  try {
    numerical();
    validation();
    cache_policy();
    resources();
    layouts();
    batch_and_empty();
    active_cancellation();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
