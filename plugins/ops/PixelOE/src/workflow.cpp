#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/tensor_description.hpp"
#include "photospider/photospider.hpp"

std::map<std::string, ps::ParameterValue> defaults() {
  using I = std::int64_t;
  return {{"pixel_size", I(6)},
          {"thickness", I(3)},
          {"mode", std::string("contrast")},
          {"sharpen_mode", std::string("none")},
          {"sharpen_factor", 0.5},
          {"do_color_match", true},
          {"do_quant", false},
          {"num_colors", I(32)},
          {"quant_mode", std::string("kmeans")},
          {"dither_mode", std::string("ordered")},
          {"no_post_upscale", false},
          {"weight_mapping", std::string("current")},
          {"weight_normalize", std::string("global")},
          {"colorfix_blur", std::string("exact")},
          {"blur_impl", std::string("lowrank")},
          {"blur_rank", I(1)},
          {"local_stats", std::string("lattice")},
          {"stat_padding", std::string("zero")}};
}
void require(const ps::Status& status) {
  if (!status.ok()) {
    throw std::runtime_error(status.message);
  }
}
int main(int argc, char** argv) try {
  if (argc < 4) {
    std::cerr
        << "usage: pixeloe_workflow PLUGIN WIDTH HEIGHT [name=value ...]\n";
    return 2;
  }
  uint64_t w = std::stoull(argv[2]), h = std::stoull(argv[3]);
  auto params = defaults();
  std::string metadata_mode;
  std::string input_file, output_file, operation = "pixeloe.pixelize";
  int repeat = 1, warmup = 0;
  uint64_t budget = 8ULL << 30;
  bool roi = false;
  bool altered_rounding = false;
  for (int i = 4; i < argc; ++i) {
    std::string a = argv[i];
    auto split = a.find('=');
    auto key = a.substr(0, split), val = a.substr(split + 1);
    if (key == "input") {
      input_file = val;
    } else if (key == "output") {
      output_file = val;
    } else if (key == "operation") {
      operation = val;
    } else if (key == "repeat") {
      repeat = std::stoi(val);
    } else if (key == "warmup") {
      warmup = std::stoi(val);
    } else if (key == "budget") {
      budget = std::stoull(val);
    } else if (key == "roi") {
      roi = val == "true";
    } else if (key == "rounding") {
      altered_rounding = val == "down";
    } else if (key == "metadata") {
      metadata_mode = val;
    } else {
      auto& p = params.at(key);
      if (std::holds_alternative<std::int64_t>(p)) {
        p = std::stoll(val);
      } else if (std::holds_alternative<double>(p)) {
        p = std::stod(val);
      } else if (std::holds_alternative<bool>(p)) {
        p = val == "true";
      } else {
        p = val;
      }
    }
  }
  std::vector<float> samples(h * w * 3);
  if (!input_file.empty()) {
    std::ifstream in(input_file, std::ios::binary);
    in.read(reinterpret_cast<char*>(samples.data()), samples.size() * 4);
    if (!in) {
      throw std::runtime_error("raw FP32 HWC input read failed");
    }
  } else {
    for (uint64_t y = 0; y < h; ++y) {
      for (uint64_t x = 0; x < w; ++x) {
        for (uint32_t c = 0; c < 3; ++c) {
          samples[(y * w + x) * 3 + c] =
              static_cast<float>((x * 17 + y * 31 + c * 71 + (x * y) % 113) %
                                 256) /
              255.0f;
        }
      }
    }
  }
  ps::ValueDescriptor descriptor{ps::ElementType::Float32, {h, w, 3}};
  ps::PlanarImageConfig config;
  config.maximum_backed_bytes = budget;
  std::vector<ps::ValueFacet> facets;
  if (!metadata_mode.empty()) {
    ps::TensorDescription d;
    d.channel_axis = 2;
    d.model = "rgb";
    d.transfer = "srgb";
    d.primaries = "srgb";
    d.reference = "display";
    d.association = "straight";
    d.channels = {{"R", "red", "relative"},
                  {"G", "green", "relative"},
                  {"B", "blue", "relative"}};
    d.white = std::array<double, 2>{0.3127, 0.329};
    if (metadata_mode == "bgr") {
      std::swap(d.channels[0], d.channels[2]);
    }
    if (metadata_mode == "d50") {
      d.white = std::array<double, 2>{0.34567, 0.35850};
    }
    if (metadata_mode == "linear") {
      d.transfer = "linear";
    }
    if (metadata_mode == "scene") {
      d.reference = "scene";
    }
    if (metadata_mode == "unit") {
      d.channels[0].unit = "nits";
    }
    if (metadata_mode == "group") {
      ps::TensorColorGroup g;
      g.name = "color";
      g.indices = {0, 1, 2};
      g.components = d.channels;
      g.interpretation.model = "rgb";
      g.interpretation.transfer = "srgb";
      g.interpretation.primaries = "srgb";
      g.interpretation.reference = "display";
      g.interpretation.association = "straight";
      d.groups = {g};
    }
    auto encoded = ps::encode_tensor_description(d);
    if (!encoded.ok()) {
      throw std::runtime_error(encoded.status().message);
    }
    facets.push_back(encoded.take_value());
  }
  auto image = ps::PlanarImage::create(descriptor, config, facets).take_value();
  require(image.publish(ps::Region::whole(descriptor.shape),
                        reinterpret_cast<uint8_t*>(samples.data()),
                        samples.size() * 4));
  samples.clear();
  samples.shrink_to_fit();
  auto registry = std::make_shared<ps::OperationRegistry>();
  require(registry->load_plugin(argv[1]));
  require(registry->freeze());
  ps::WorkflowDocument document;
  document.inputs = {
      {1,
       "image",
       descriptor,
       ps::Region::whole(descriptor.shape),
       {},
       facets,
       ps::PlanarImageLayout{ps::ImagePlaneOrder::Tiled, 0, 1, 2, 0, {}}}};
  document.nodes = {{1, operation, {ps::WorkflowInputReference{1}}, params}};
  document.outputs = {{"result", 1, "values"}};
  ps::GraphContext graph(document);
  ps::PlanningOptions planning;
  if (roi) {
    planning.output_regions = {
        {"result", ps::Region({{1, 3}, {2, 4}, {0, 3}})}};
  }
  auto plan = ps::Compiler(registry).compile(graph, planning);
  if (!plan.ok()) {
    throw std::runtime_error("compile: " + plan.status().message);
  }
  ps::ExecutionContext execution(registry, {1, false, 8, budget});
  ps::ExecutionBindings bindings;
  bindings.inputs.push_back(
      {"image", {}, {}, {}, std::make_shared<const ps::PlanarImage>(image)});
  std::vector<double> ms;
  uint64_t peak = 0;
  std::vector<float> result;
  for (int i = -warmup; i < repeat; ++i) {
    auto start = std::chrono::steady_clock::now();
    fenv_t prior;
    fegetenv(&prior);
    if (altered_rounding) {
      fesetround(FE_DOWNWARD);
      feclearexcept(FE_ALL_EXCEPT);
      feraiseexcept(FE_DIVBYZERO);
    }
    auto run = execution.execute(plan.value().plan, bindings);
    if (altered_rounding && (fegetround() != FE_DOWNWARD ||
                             fetestexcept(FE_ALL_EXCEPT) != FE_DIVBYZERO)) {
      throw std::runtime_error("caller FP environment changed");
    }
    fesetenv(&prior);
    double elapsed = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    if (!run.ok()) {
      throw std::runtime_error("execute: " + run.status().message);
    }
    if (i >= 0) {
      ms.push_back(elapsed);
    }
    peak = std::max(peak, run.value().diagnostics.peak_live_bytes);
    if (i == repeat - 1) {
      const auto& out = run.value().images.at("result");
      auto region = roi ? ps::Region({{1, 3}, {2, 4}, {0, 3}})
                        : ps::Region::whole(out.descriptor().shape);
      result.resize(region.element_count().value());
      require(out.read(region, reinterpret_cast<uint8_t*>(result.data()),
                       result.size() * 4));
      std::cout << "output=" << out.descriptor().shape[1] << "x"
                << out.descriptor().shape[0] << "x" << out.descriptor().shape[2]
                << " ";
    }
  }
  if (!output_file.empty()) {
    std::ofstream out(output_file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(result.data()), result.size() * 4);
  }
  std::sort(ms.begin(), ms.end());
  double sum = 0;
  for (float v : result) {
    if (!std::isfinite(v)) {
      throw std::runtime_error("nonfinite output");
    }
    sum += v;
  }
  std::cout << "median_ms=" << ms[ms.size() / 2] << " p95_ms="
            << ms[std::min(ms.size() - 1,
                           static_cast<size_t>(std::ceil(ms.size() * .95) - 1))]
            << " peak_bytes=" << peak << " checksum=" << sum << "\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << "\n";
  return 1;
}
