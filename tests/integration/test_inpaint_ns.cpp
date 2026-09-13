#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
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
std::vector<ps::Value> inputs(const std::vector<float>& rgb,
                              const std::vector<float>& mask, int h, int w) {
  return {value(rgb, {static_cast<unsigned>(h), static_cast<unsigned>(w), 4},
                ps::rgba_semantics()),
          value(mask, {static_cast<unsigned>(h), static_cast<unsigned>(w)},
                ps::coverage_semantics())};
}
ps::Result<ps::Value> invoke(
    const std::string& key, const std::vector<ps::Value>& values,
    Params params = {{"radius", std::int64_t{3}}},
    ps::BufferAllocator allocator = ps::BufferAllocator{},
    ps::CancellationToken token = {}) {
  const auto registry = ps::make_default_operation_registry();
  std::vector<ps::Region> regions;
  for (const auto& v : values)
    regions.push_back(v.region());
  return registry->invoke(
      key, {values, regions, params, ps::Backend::Cpu, token, {}, allocator});
}
void reject(ps::Result<ps::Value> result, ps::ErrorCode code) {
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
          check(take(invoke(key, inputs(changed, mask, h, w),
                            {{"radius", std::int64_t{radius}}}))
                        .copy_bytes() == actual_value.copy_bytes(),
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
    check(take(invoke(key, values)).copy_bytes() == values[0].copy_bytes(),
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
    const auto baseline = take(invoke(key, values)).copy_bytes();
    const auto original = std::fegetround();
    std::fesetround(FE_UPWARD);
    const auto rounded = take(invoke(key, values)).copy_bytes();
    check(std::fegetround() == FE_UPWARD, "floating mode restore");
    std::fesetround(original);
    check(rounded == baseline, "rounding independent bits");
    auto concurrent = std::async(std::launch::async, [&] {
      return take(invoke(key, values)).copy_bytes();
    });
    check(take(invoke(key, values)).copy_bytes() == baseline &&
              concurrent.get() == baseline,
          "concurrent outputs");
    ps::CancellationSource cancel;
    cancel.cancel();
    reject(invoke(key, values, {{"radius", std::int64_t{3}}},
                  ps::BufferAllocator{}, cancel.token()),
           ps::ErrorCode::Cancelled);
    auto display = ps::rgba_semantics();
    display.reference = "display";
    auto display_inputs = values;
    display_inputs[0] = value(rgb, {5, 5, 4}, display);
    check(take(invoke(key, display_inputs)).facets()[0].payload ==
              display_inputs[0].facets()[0].payload,
          "display reference preserved");
    auto extreme = rgb;
    for (unsigned i = 0; i < 25; ++i)
      for (unsigned c = 0; c < 3; ++c)
        extreme[i * 4 + c] = i % 2 ? 1e30F : -1e30F;
    reject(invoke(key, inputs(extreme, mask, 5, 5)),
           ps::ErrorCode::OperationFailed);
    const auto workflow = take(run(key, values, {{"radius", std::int64_t{3}}}));
    check(workflow.values.at("result").copy_bytes() == baseline,
          "public workflow output");
    ps::PlanningOptions roi;
    roi.output_regions = {{"result", ps::Region({{1, 3}, {1, 2}, {0, 4}})}};
    roi.tile_height = 1;
    roi.tile_width = 1;
    auto distant = rgb;
    distant[0] = std::numeric_limits<float>::quiet_NaN();
    auto invalid_roi = run(key, inputs(distant, mask, 5, 5),
                           {{"radius", std::int64_t{3}}}, roi);
    check(!invalid_roi.ok() &&
              invalid_roi.status().code == ps::ErrorCode::InvalidArgument,
          "distant invalid pixel rejected for ROI");
    const auto crop =
        pixels(take(run(key, values, {{"radius", std::int64_t{3}}}, roi))
                   .values.at("result"));
    const auto full = pixels(workflow.values.at("result"));
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
    ps::WorkflowDocument doc;
    ps::ExecutionBindings bindings;
    ps::InputSnapshotStore snapshots;
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto name = "input" + std::to_string(i);
      doc.inputs.push_back({i + 1, name, values[i].descriptor(),
                            values[i].region(), values[i].layout(),
                            values[i].facets()});
      bindings.inputs.push_back({name,
                                 {},
                                 {},
                                 std::make_shared<ps::InputSnapshot>(
                                     take(snapshots.import_value(values[i])))});
    }
    doc.nodes = {
        {1,
         key,
         {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
         {{"radius", std::int64_t{3}}}}};
    doc.outputs = {{"result", 1, "image"}};
    auto registry = ps::make_default_operation_registry();
    ps::GraphContext graph(doc);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.result_cache_bytes = 1024 * 1024;
    ps::ExecutionContext context(registry, config);
    const auto first = take(context.execute(compiled.plan, bindings));
    const auto second = take(context.execute(compiled.plan, bindings));
    check(first.values.at("result").copy_bytes() ==
              second.values.at("result").copy_bytes(),
          "cached-context repeated output");
    check(key == native_key ? second.diagnostics.cache_hits > 0
                            : second.diagnostics.cache_hits == 0,
          "native caches while the external-library adapter recomputes");
  }
  std::cout << "native and OpenCV result cache policy PASS\n";
}
void resources() {
  std::vector<float> rgb(100, .25F), mask(25);
  mask[12] = 1;
  for (unsigned i = 3; i < 100; i += 4)
    rgb[i] = 1;
  const auto values = inputs(rgb, mask, 5, 5);
  // Native capacities: output 16N + mask N + plane 4N + state P + time 4P +
  // heap 16N.
  constexpr std::uint64_t minimum = 37 * 25 + 5 * 49;
  reject(invoke(native_key, values, {{"radius", std::int64_t{3}}},
                ps::BufferAllocator{}.limited(minimum - 1)),
         ps::ErrorCode::ResourceExhausted);
  check(invoke(native_key, values, {{"radius", std::int64_t{3}}},
               ps::BufferAllocator{}.limited(minimum))
            .ok(),
        "exact native payload capacity");
  for (unsigned fail = 1; fail <= 6; ++fail) {
    unsigned calls = 0;
    std::uint64_t live = 0;
    ps::BufferAllocator allocator(
        [&](std::uint64_t bytes) -> ps::Result<std::shared_ptr<void>> {
          if (++calls == fail)
            return ps::Result<std::shared_ptr<void>>(ps::Status::failure(
                ps::ErrorCode::ResourceExhausted, "injected"));
          live += bytes;
          return ps::Result<std::shared_ptr<void>>(
              std::shared_ptr<void>(new int, [&, bytes](void* p) {
                delete static_cast<int*>(p);
                live -= bytes;
              }));
        });
    reject(invoke(native_key, values, {{"radius", std::int64_t{3}}}, allocator),
           ps::ErrorCode::ResourceExhausted);
    check(live == 0, "failure owner cleanup");
  }
  for (unsigned stop = 1; stop <= 6; ++stop) {
    unsigned calls = 0;
    ps::CancellationSource cancel;
    ps::BufferAllocator allocator(
        [&](std::uint64_t) -> ps::Result<std::shared_ptr<void>> {
          if (++calls == stop)
            cancel.cancel();
          return ps::Result<std::shared_ptr<void>>(std::shared_ptr<void>{});
        });
    reject(invoke(native_key, values, {{"radius", std::int64_t{3}}}, allocator,
                  cancel.token()),
           ps::ErrorCode::Cancelled);
  }
  std::cout << "native exact capacity, six allocation failures and six "
               "allocation cancellation points PASS\n";
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
        values[port] = take(ps::Value::create(values[port].descriptor(),
                                              values[port].region(), layout,
                                              bytes, values[port].facets()));
      }
      const auto image_before = values[0].copy_bytes(),
                 mask_before = values[1].copy_bytes();
      const auto result = take(invoke(key, values));
      check(values[0].copy_bytes() == image_before &&
                values[1].copy_bytes() == mask_before,
            "immutable views");
      auto dense = inputs(pixels(values[0]), pixels(values[1]), 5, 5);
      check(result.copy_bytes() == take(invoke(key, dense)).copy_bytes(),
            "strided equivalence");
    }
  std::cout << "padded offset origin negative and zero stride invocation views "
               "PASS\n";
}
}  // namespace
int main() {
  try {
    numerical();
    validation();
    cache_policy();
    resources();
    layouts();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
