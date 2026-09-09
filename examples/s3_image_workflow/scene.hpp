#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
inline ps::Value scalar(float number) {
  std::vector<std::uint8_t> bytes(4);
  std::memcpy(bytes.data(), &number, 4);
  return take(ps::Value::create({ps::ElementType::Float32, {1}},
                                ps::Region::whole({1}), {0, {4}},
                                std::move(bytes)));
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
                 ps::ExecutionMode mode = ps::ExecutionMode::CpuExact)
      : mode_(mode),
        registry_(std::move(registry)),
        compiler_(registry_),
        store_({1024 * 1024, 4}) {
    foreground = take(store_.import_value(make_image(false)));
    background = take(store_.import_value(make_image(true)));
    std::vector<std::uint8_t> bytes(height * width * 4);
    float half = .5F;
    for (std::size_t i = 0; i < bytes.size(); i += 4)
      std::memcpy(bytes.data() + i, &half, 4);
    mask = take(store_.import_value(take(ps::Value::create(
        {ps::ElementType::Float32, {height, width}},
        ps::Region::whole({height, width}),
        {0, {static_cast<std::int64_t>(width * 4), 4}}, std::move(bytes)))));
    full_graph_ = std::make_unique<ps::GraphContext>(document(1));
    proxy_graph_ = std::make_unique<ps::GraphContext>(document(4));
    ps::PlanningOptions options;
    options.execution_mode = mode_;
    options.tile_height = options.tile_width = 4;
    full_ = take(compiler_.compile(*full_graph_, options)).plan;
    proxy_ = take(compiler_.compile(*proxy_graph_, options)).plan;
    auto brush_doc = brush_document();
    brush_graph_ = std::make_unique<ps::GraphContext>(brush_doc);
    brush_ = take(compiler_.compile(*brush_graph_, options)).plan;
  }
  ps::InputSnapshot foreground, background, mask;
  /** @brief Captures current pixels and gain without recompiling either graph.
   */
  ps::FrozenExecution freeze(ps::ExecutionContext& execution, float gain,
                             int factor = 1) const {
    return take(execution.freeze(factor == 1 ? full_ : proxy_, bindings(gain)));
  }
  ps::ExecutionBindings bindings(float gain) const {
    return {{{"foreground",
              {},
              {},
              std::make_shared<ps::InputSnapshot>(foreground)},
             {"background",
              {},
              {},
              std::make_shared<ps::InputSnapshot>(background)},
             {"mask", {}, {}, std::make_shared<ps::InputSnapshot>(mask)},
             {"gain", scalar(gain)}}};
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
    ps::ExecutionBindings bound{
        {{"image", {}, {}, std::make_shared<ps::InputSnapshot>(foreground)}}};
    const float values[] = {event.x,     event.y,    event.radius, event.red,
                            event.green, event.blue, event.alpha};
    const char* names[] = {"x", "y", "radius", "red", "green", "blue", "alpha"};
    for (std::size_t i = 0; i < 7; ++i)
      bound.inputs.push_back({names[i], scalar(values[i])});
    auto tile = take(brush_.tile_plan(
        "image", ps::Region({{static_cast<std::uint64_t>(y0),
                              static_cast<std::uint64_t>(y1 - y0)},
                             {static_cast<std::uint64_t>(x0),
                              static_cast<std::uint64_t>(x1 - x0)},
                             {0, 4}})));
    auto result = take(execution.execute(tile, std::move(bound)));
    foreground = take(store_.patch(foreground, result.values.at("image")));
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
    options.tile_height = options.tile_width = 4;
    full_ = take(compiler_.compile(*full_graph_, options)).plan;
  }
  /** @brief Independent full-image 2D convolution/composition oracle. */
  std::vector<float> oracle(float gain) const {
    std::vector<float> front(height * width * 4), back(front.size()),
        mask_pixels(height * width);
    require(foreground
                .read(ps::Region::whole({height, width, 4}),
                      reinterpret_cast<std::uint8_t*>(front.data()),
                      front.size() * 4)
                .ok(),
            "oracle front read");
    require(
        background
            .read(ps::Region::whole({height, width, 4}),
                  reinterpret_cast<std::uint8_t*>(back.data()), back.size() * 4)
            .ok(),
        "oracle back read");
    require(mask.read(ps::Region::whole({height, width}),
                      reinterpret_cast<std::uint8_t*>(mask_pixels.data()),
                      mask_pixels.size() * 4)
                .ok(),
            "oracle mask read");
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
  static ps::Value make_image(bool back) {
    const std::string profile = "rgba;linear-srgb;premultiplied;hwc";
    std::vector<std::uint8_t> bytes(height * width * 16);
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
      float number = i % 16 == 12 ? .5F : back ? .25F : .125F;
      std::memcpy(bytes.data() + i, &number, 4);
    }
    return take(ps::Value::create(
        {ps::ElementType::Float32, {height, width, 4}},
        ps::Region::whole({height, width, 4}),
        {0, {static_cast<std::int64_t>(width * 16), 16, 4}}, std::move(bytes),
        {{"photospider.image", 1, {profile.begin(), profile.end()}}}));
  }
  static ps::WorkflowInputDeclaration declaration(
      std::uint64_t id, const std::string& name,
      const ps::InputSnapshot& snapshot) {
    const auto& shape = snapshot.descriptor().shape;
    std::vector<std::int64_t> strides(shape.size());
    std::int64_t stride = 4;
    for (std::size_t a = shape.size(); a > 0; --a) {
      strides[a - 1] = stride;
      stride *= static_cast<std::int64_t>(shape[a - 1]);
    }
    return {id,
            name,
            snapshot.descriptor(),
            ps::Region::whole(shape),
            {0, strides},
            snapshot.facets()};
  }
  ps::WorkflowDocument document(int factor) const {
    ps::WorkflowDocument d;
    d.inputs = {declaration(1, "foreground", foreground),
                declaration(2, "background", background),
                declaration(3, "mask", mask),
                {4,
                 "gain",
                 scalar(1).descriptor(),
                 ps::Region::whole({1}),
                 {0, {4}},
                 {}}};
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
      d.inputs.push_back({i + 2,
                          names[i],
                          scalar(1).descriptor(),
                          ps::Region::whole({1}),
                          {0, {4}},
                          {}});
      inputs.push_back(ps::WorkflowInputReference{i + 2});
    }
    d.nodes = {{1, "image.brush_circle", inputs, {}}};
    d.outputs = {{"image", 1, "value"}};
    return d;
  }
  ps::ExecutionMode mode_;
  std::shared_ptr<ps::OperationRegistry> registry_;
  ps::Compiler compiler_;
  ps::InputSnapshotStore store_;
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
  void blit(const ps::Value& tile) {
    const auto& r = tile.region().dimensions();
    for (std::uint64_t y = r[0].offset; y < r[0].offset + r[0].extent; ++y)
      for (std::uint64_t x = r[1].offset; x < r[1].offset + r[1].extent; ++x)
        for (std::uint64_t c = 0; c < 4; ++c)
          std::memcpy(&pixels[(y * width + x) * 4 + c],
                      tile.bytes().data() + take(tile.byte_address({y, x, c})),
                      4);
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
                            .output_descriptor.shape;
    frame = Frame(shape[0], shape[1]);
  }
  Frame frame;
  std::uint64_t tiles = 0;
  bool done = false;
  void step(ps::ExecutionContext& execution) {
    if (done)
      return;
    const auto h = std::min<std::uint64_t>(4, frame.height - y_),
               w = std::min<std::uint64_t>(4, frame.width - x_);
    auto tile = take(
        frozen_.for_region("result", ps::Region({{y_, h}, {x_, w}, {0, 4}})));
    auto result = take(execution.execute(tile));
    frame.blit(result.values.at("result"));
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
}  // namespace s3
