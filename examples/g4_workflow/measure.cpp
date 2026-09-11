#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T checked(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
}  // namespace
void measure_workflow() {
  constexpr std::uint64_t width = 256;
  auto image = checked(
      MutableValue::allocate({ElementType::Float32, {1, width, 4}},
                             Region::whole({1, width, 4}), BufferAllocator{}));
  auto map = checked(
      MutableValue::allocate({ElementType::Float64, {1, width, 2}},
                             Region::whole({1, width, 2}), BufferAllocator{}));
  for (std::uint64_t x = 0; x < width; ++x) {
    const float rgba[] = {static_cast<float>(x) / 256, .25F, .75F, 1};
    const double xy[] = {static_cast<double>(x) + .5, .5};
    std::memcpy(image.data() + x * 16, rgba, sizeof(rgba));
    std::memcpy(map.data() + x * 16, xy, sizeof(xy));
  }
  const auto source = checked(
      std::move(image).publish({checked(encode_semantic(rgba_semantics()))}));
  const auto coordinates = checked(std::move(map).publish());
  auto registry = make_default_operation_registry();
  WorkflowDocument document;
  document.inputs = {{1, "image", source.descriptor(), source.region(),
                      source.layout(), source.facets()},
                     {2,
                      "map",
                      coordinates.descriptor(),
                      coordinates.region(),
                      coordinates.layout(),
                      {}}};
  document.nodes = {{1,
                     "image.stmap",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {{"boundary", std::string("clamp")}}}};
  document.outputs = {{"image", 1, "value"}};
  GraphContext graph(document);
  auto plan = checked(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 8, 32768});
  ExecutionBindings bindings{{{"image", source}, {"map", coordinates}}};
  struct Sample {
    std::uint64_t run_us, poll_us, polls;
  };
  std::vector<Sample> samples;
  // One warmup, then three complete independent Runs with caching disabled.
  for (unsigned iteration = 0; iteration < 4; ++iteration) {
    const auto started = std::chrono::steady_clock::now();
    auto result = checked(context.execute(plan, bindings));
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    const auto& output = result.values.at("image");
    if (output.bytes().size() != source.bytes().size() ||
        std::memcmp(output.bytes().data(), source.bytes().data(),
                    source.bytes().size()) != 0 ||
        checked(result.dependencies.certificate(1)).rows().size() != width)
      throw std::runtime_error("single-pixel measurement oracle failed");
    std::uint64_t polls = 0, callback_us = 0, computed = 0;
    for (const auto& timing : result.diagnostics.operation_timings) {
      polls += timing.invocation_count;
      callback_us += timing.duration_us;
      computed += timing.computed_elements;
    }
    if (polls != 3 * width || computed != 4 * width)
      throw std::runtime_error("unexpected atomic STMap stage count");
    if (iteration)
      samples.push_back({elapsed, callback_us, polls});
  }
  std::sort(samples.begin(), samples.end(),
            [](const auto& a, const auto& b) { return a.run_us < b.run_us; });
  const auto median = samples[1];
  std::cout << "atom-isolated STMap: pixels=" << width
            << ", poll_callbacks=" << median.polls
            << ", median_execute_us=" << median.run_us
            << ", poll_callback_us=" << median.poll_us
            << ", execute_us_per_pixel="
            << static_cast<double>(median.run_us) / width
            << ", workers=1, cache=off, batching=off\n";
}
