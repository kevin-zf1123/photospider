#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "image_vertical/image_fixture.hpp"
#include "photospider/photospider.hpp"

namespace s3 {
inline void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  require(result.ok(), result.status().message);
  return result.take_value();
}
inline ps::ResultRef scalar(const ps::ResourceBudget& root, float number) {
  return s1_fixture::scalar(root, number);
}
inline std::vector<float> samples(const ps::ResultRef& result) {
  std::vector<float> output;
  auto descriptor = take(result.descriptor());
  s1_fixture::check(descriptor.tensor_coverage(0).visit(
      [&](const auto& at) {
        float value = 0;
        auto status = result.read_tensor(descriptor, 0, at, &value, 4);
        if (status.ok())
          output.push_back(value);
        return status;
      },
      UINT64_MAX));
  return output;
}
/** @brief One hard circle event, in image coordinates and linear RGB. */
struct Stamp {
  float x = 0, y = 0, radius = 1, red = 1, green = 0, blue = 0, alpha = .5F;
};
/** @brief Small editable four-operation scene using installed public APIs only.
 */
class Scene final {
 public:
  static constexpr std::uint64_t height = 13, width = 17;
  explicit Scene(std::shared_ptr<ps::OperationRegistry> registry,
                 ps::ResourceBudget root,
                 ps::ExecutionMode mode = ps::ExecutionMode::CpuExact,
                 std::uint64_t tile_height = 4, std::uint64_t tile_width = 4)
      : mode_(mode),
        tile_height_(tile_height),
        tile_width_(tile_width),
        registry_(std::move(registry)),
        compiler_(registry_),
        root_(std::move(root)) {
    foreground = make_image(false);
    background = make_image(true);
    mask = s1_fixture::tensor(root_, std::vector<float>(height * width, .5F),
                              {height, width}, false);
    full_graph_ = std::make_unique<ps::GraphContext>(document(1));
    proxy_graph_ = std::make_unique<ps::GraphContext>(document(4));
    ps::PlanningOptions options;
    options.execution_mode = mode_;
    options.tile_height = tile_height_;
    options.tile_width = tile_width_;
    full_ = take(compiler_.compile(*full_graph_, options)).plan;
    proxy_ = take(compiler_.compile(*proxy_graph_, options)).plan;
    auto brush_doc = brush_document();
    brush_graph_ = std::make_unique<ps::GraphContext>(brush_doc);
    brush_ = take(compiler_.compile(*brush_graph_, options)).plan;
  }
  ps::ResultRef foreground, background, mask;
  /** @brief Captures current pixels and gain without recompiling either graph.
   */
  ps::FrozenExecution freeze(ps::ExecutionContext& execution, float gain,
                             int factor = 1) const {
    return take(execution.freeze(factor == 1 ? full_ : proxy_, bindings(gain)));
  }
  ps::ExecutionBindings bindings(float gain) const {
    return {{{"foreground", foreground},
             {"background", background},
             {"mask", mask},
             {"gain", scalar(root_, gain)}}};
  }

  /** @brief Applies exactly one admitted stamp as an immutable regional patch.
   */
  void stamp(ps::ExecutionContext& execution, const Stamp& event) {
    const double x0 =
        std::clamp(std::floor(static_cast<double>(event.x) - event.radius), 0.0,
                   static_cast<double>(width));
    const double y0 =
        std::clamp(std::floor(static_cast<double>(event.y) - event.radius), 0.0,
                   static_cast<double>(height));
    const double x1 =
        std::clamp(std::ceil(static_cast<double>(event.x) + event.radius), 0.0,
                   static_cast<double>(width));
    const double y1 =
        std::clamp(std::ceil(static_cast<double>(event.y) + event.radius), 0.0,
                   static_cast<double>(height));
    require(std::isfinite(event.x) && std::isfinite(event.y) &&
                std::isfinite(event.radius) && event.radius > 0,
            "invalid stamp geometry");
    if (x1 <= x0 || y1 <= y0)
      return;
    ps::ExecutionBindings bound{{{"image", foreground}}};
    const float values[] = {event.x,     event.y,    event.radius, event.red,
                            event.green, event.blue, event.alpha};
    const char* names[] = {"x", "y", "radius", "red", "green", "blue", "alpha"};
    for (std::size_t i = 0; i < 7; ++i)
      bound.inputs.push_back({names[i], scalar(root_, values[i])});
    const auto first_y = static_cast<uint64_t>(y0) / 4 * 4;
    const auto first_x = static_cast<uint64_t>(x0) / 4 * 4;
    const auto last_y =
        std::min(height, (static_cast<uint64_t>(y1) + 3) / 4 * 4);
    const auto last_x =
        std::min(width, (static_cast<uint64_t>(x1) + 3) / 4 * 4);
    const ps::Region patch_region({{0, 1},
                                   {0, 1},
                                   {first_y, last_y - first_y},
                                   {first_x, last_x - first_x},
                                   {0, 4}});
    auto frozen = take(execution.freeze(brush_, std::move(bound)));
    auto result =
        take(execution.execute(take(frozen.for_region("image", patch_region))));
    const auto& patch = result.results.at("image");
    auto builder = take(ps::ResultBuilder::start(
        root_, foreground.schema(), "example.edited", {},
        {foreground.object_id(), patch.object_id()}, 4, 4));
    s1_fixture::check(builder.bind_descriptor_relation(
        take(ps::ResultRelation::cartesian(root_, 1, {}))));
    auto witness =
        take(ps::ResultRelation::cartesian(root_, height * width * 4, {}));
    for (uint64_t y = 0; y < height; ++y)
      for (uint64_t x = 0; x < width; x += 4) {
        const ps::Region region({{0, 1},
                                 {0, 1},
                                 {y, 1},
                                 {x, std::min(uint64_t{4}, width - x)},
                                 {0, 4}});
        const auto& source =
            y >= first_y && y < last_y && x >= first_x && x < last_x
                ? patch
                : foreground;
        auto window =
            take(source.acquire_tensor(take(source.descriptor()), 0, region));
        ps::ResultTensorViewTransform identity;
        for (int axis = 0; axis < 5; ++axis)
          identity.source_axes.push_back({axis, 0, 1, 1});
        auto published = builder.publish_tensor_view(
            0, region, window, identity, witness, {true, true, true, true});
        if (!published.ok() &&
            published.message.find("ViewUnavailable") != std::string::npos)
          published = builder.publish_tensor_view(0, region, {&window}, witness,
                                                  {true, true, true, true});
        s1_fixture::check(published);
      }
    foreground = take(builder.seal());
  }
  /** @brief Demonstrates a graph edit whose branch does not feed the image. */
  void edit_unrelated_branch() {
    auto edited = document(1);
    edited.nodes.push_back({99,
                            "image.gaussian_blur",
                            {ps::WorkflowInputReference{2}},
                            {{"radius", INT64_C(3)}, {"sigma", 1.0}}});
    full_graph_->replace(std::move(edited));
    ps::PlanningOptions options;
    options.execution_mode = mode_;
    options.tile_height = tile_height_;
    options.tile_width = tile_width_;
    full_ = take(compiler_.compile(*full_graph_, options)).plan;
  }
  /** @brief Independent full-image 2D convolution/composition oracle. */
  std::vector<float> oracle(float gain) const {
    const auto front = samples(foreground), back = samples(background),
               mask_pixels = samples(mask);
    std::vector<float> output(front.size());
    double denominator = 0;
    double weights[25];
    std::size_t k = 0;
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx) {
        weights[k] = std::exp(-(dx * dx + dy * dy) / 2.0);
        denominator += weights[k++];
      }
    for (std::uint64_t y = 0; y < height; ++y)
      for (std::uint64_t x = 0; x < width; ++x) {
        float blurred[4];
        for (std::uint64_t c = 0; c < 4; ++c) {
          double sum = 0;
          k = 0;
          for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
              const auto yy = std::clamp<std::int64_t>(
                             static_cast<std::int64_t>(y) + dy, 0, height - 1),
                         xx = std::clamp<std::int64_t>(
                             static_cast<std::int64_t>(x) + dx, 0, width - 1);
              sum +=
                  front[(yy * width + xx) * 4 + c] * weights[k++] / denominator;
            }
          const float value = static_cast<float>(sum);
          blurred[c] =
              (c == 3 ? value : value * gain) * mask_pixels[y * width + x];
        }
        for (std::uint64_t c = 0; c < 4; ++c) {
          const float attenuated =
              back[(y * width + x) * 4 + c] * (1 - blurred[3]);
          output[(y * width + x) * 4 + c] = blurred[c] + attenuated;
        }
      }
    return output;
  }

 private:
  ps::ResultRef make_image(bool back) const {
    std::vector<float> pixels(height * width * 4);
    for (size_t i = 0; i < pixels.size(); ++i)
      pixels[i] = i % 4 == 3 ? .5F : back ? .25F : .125F;
    return s1_fixture::tensor(root_, pixels, {height, width, 4});
  }
  static ps::WorkflowInputDeclaration declaration(uint64_t id,
                                                  const std::string& name,
                                                  const ps::ResultRef& input) {
    return s1_fixture::declaration(id, name, input);
  }
  ps::WorkflowDocument document(int factor) const {
    ps::WorkflowDocument d;
    d.inputs = {
        declaration(1, "foreground", foreground),
        declaration(2, "background", background), declaration(3, "mask", mask),
        s1_fixture::declaration(4, "gain", s1_fixture::schema({1}, false))};
    ps::WorkflowInput fg = ps::WorkflowInputReference{1},
                      bg = ps::WorkflowInputReference{2},
                      m = ps::WorkflowInputReference{3};
    if (factor != 1) {
      d.nodes = {{5,
                  "image.downsample_box",
                  {fg},
                  {{"factor", static_cast<std::int64_t>(factor)}}},
                 {6,
                  "image.downsample_box",
                  {bg},
                  {{"factor", static_cast<std::int64_t>(factor)}}},
                 {7,
                  "mask.downsample_box",
                  {m},
                  {{"factor", static_cast<std::int64_t>(factor)}}}};
      fg = ps::WorkflowNodeOutput{5, "value"};
      bg = ps::WorkflowNodeOutput{6, "value"};
      m = ps::WorkflowNodeOutput{7, "value"};
    }
    d.nodes.push_back({10,
                       "image.gaussian_blur",
                       {fg},
                       {{"radius", static_cast<std::int64_t>(
                                       std::max(1, (2 + factor - 1) / factor))},
                        {"sigma", std::max(.1, 1.0 / factor)}}});
    d.nodes.push_back(
        {20,
         "image.exposure_gain",
         {ps::WorkflowNodeOutput{10, "value"}, ps::WorkflowInputReference{4}},
         {}});
    d.nodes.push_back(
        {30, "image.mask", {ps::WorkflowNodeOutput{20, "value"}, m}, {}});
    d.nodes.push_back({40,
                       "image.source_over",
                       {ps::WorkflowNodeOutput{30, "value"}, bg},
                       {}});
    d.outputs = {{"result", 40, "value"}};
    return d;
  }
  ps::WorkflowDocument brush_document() const {
    ps::WorkflowDocument d;
    d.inputs = {declaration(1, "image", foreground)};
    const char* names[] = {"x", "y", "radius", "red", "green", "blue", "alpha"};
    std::vector<ps::WorkflowInput> inputs{ps::WorkflowInputReference{1}};
    for (std::size_t i = 0; i < 7; ++i) {
      d.inputs.push_back(s1_fixture::declaration(
          i + 2, names[i], s1_fixture::schema({1}, false)));
      inputs.push_back(ps::WorkflowInputReference{i + 2});
    }
    d.nodes = {{1, "image.brush_circle", inputs, {}}};
    d.outputs = {{"image", 1, "value"}};
    return d;
  }
  ps::ExecutionMode mode_;
  std::uint64_t tile_height_, tile_width_;
  std::shared_ptr<ps::OperationRegistry> registry_;
  ps::Compiler compiler_;
  ps::ResourceBudget root_;
  std::unique_ptr<ps::GraphContext> full_graph_, proxy_graph_, brush_graph_;
  ps::ExecutionPlan full_, proxy_, brush_;
};
/** @brief Application-owned full frame, outside controlled kernel byte limits.
 */
struct Frame {
  std::uint64_t height = 0, width = 0;
  std::vector<float> pixels;
  Frame() = default;
  Frame(std::uint64_t h, std::uint64_t w)
      : height(h), width(w), pixels(h * w * 4) {}
  void blit(const ps::ResultRef& tile) {
    auto descriptor = take(tile.descriptor());
    s1_fixture::check(descriptor.tensor_coverage(0).visit(
        [&](const auto& at) {
          return tile.read_tensor(descriptor, 0, at,
                                  &pixels[(at[2] * width + at[3]) * 4 + at[4]],
                                  4);
        },
        UINT64_MAX));
  }
  void check(const std::vector<float>& expected) const {
    require(pixels.size() == expected.size(), "frame dimensions mismatch");
    for (std::size_t i = 0; i < pixels.size(); ++i)
      require(std::abs(pixels[i] - expected[i]) <=
                  1e-6 + 1e-5 * std::abs(expected[i]),
              "independent image oracle mismatch");
  }
};
/** @brief One bounded tile per step; cancellation belongs to its caller. */
class TileRun final {
 public:
  explicit TileRun(ps::FrozenExecution frozen) : frozen_(std::move(frozen)) {
    const auto& shape = frozen_.plan()
                            .steps()
                            .at(frozen_.plan().outputs().at("result"))
                            .output_result_schema->tensors[0]
                            .descriptor.shape;
    frame = Frame(shape[0], shape[1]);
  }
  Frame frame;
  std::uint64_t tiles = 0, native_dispatches = 0, transfers = 0, fallbacks = 0;
  std::map<uint64_t, uint64_t> calls;
  bool done = false;
  void step(ps::ExecutionContext& execution) {
    if (done)
      return;
    const auto h = std::min<std::uint64_t>(4, frame.height - y_),
               w = std::min<std::uint64_t>(4, frame.width - x_);
    auto tile = take(frozen_.for_region(
        "result", ps::Region({{0, 1}, {0, 1}, {y_, h}, {x_, w}, {0, 4}})));
    auto result = take(execution.execute(tile));
    frame.blit(result.results.at("result"));
    native_dispatches += result.diagnostics.native_dispatch_count;
    transfers += result.diagnostics.transfer_count;
    fallbacks += result.diagnostics.fallback_reasons.size();
    for (const auto& timing : result.diagnostics.operation_timings)
      calls[timing.output.node_id] += timing.invocation_count;
    ++tiles;
    x_ += w;
    if (x_ == frame.width) {
      x_ = 0;
      y_ += h;
    }
    done = y_ == frame.height;
  }

 private:
  ps::FrozenExecution frozen_;
  std::uint64_t x_ = 0, y_ = 0;
};
inline ps::ExecutionContextConfig config(ps::ExecutionMode mode,
                                         bool cache = true) {
  ps::ExecutionContextConfig result;
  result.cpu_workers = 2;
  result.gpu_enabled = mode == ps::ExecutionMode::NativeGpu;
  result.maximum_queued_tasks = 16;
  result.maximum_live_bytes = 16 * 1024 * 1024;
  result.result_cache_bytes = cache ? 8 * 1024 * 1024 : 0;
  result.maximum_dependency_cache_metadata = 256 * 1024;
  result.managed_resources = ps::ResourceLimits{};
  return result;
}
inline TileRun complete(ps::ExecutionContext& execution,
                        ps::FrozenExecution frozen) {
  TileRun run(std::move(frozen));
  while (!run.done)
    run.step(execution);
  return run;
}
inline uint64_t calls(const TileRun& run, uint64_t node = 0) {
  uint64_t count = 0;
  for (const auto& entry : run.calls)
    if (!node || entry.first == node)
      count += entry.second;
  return count;
}
}  // namespace s3
