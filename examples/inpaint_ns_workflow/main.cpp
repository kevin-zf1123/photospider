/**
 * @file main.cpp
 * @brief Public PNT-05A workflow for both required navier_stokes variants.
 *
 * Builds immutable opaque linear RGBA and binary coverage inputs, runs the
 * registered `image.local_inpaint_navier_stokes_*` operation with the required
 * Int64 `radius`, and inspects the named `image` result through the public
 * embedded API only. Reported evidence: selected operation keys, output
 * descriptor/Region/layout/facets, unmasked bit preservation, alpha
 * preservation, hole values, execution diagnostics and the declared external
 * allocation estimate of the OpenCV adapter. It never links OpenCV.
 *
 * Implementation note and acceptance mapping: `README.md` next to this file;
 * the operator contract is in `docs/kernel-architecture/Image-Operations.md`.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#define PHOTOSPIDER_EXAMPLE_HAS_RSS 1
#endif

#include "photospider/photospider.hpp"

namespace {
using ps::ErrorCode;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ps::ParameterValue>;

constexpr const char* native_key =
    "image.local_inpaint_navier_stokes_native_apple_silicon";  // NOLINT(whitespace/indent_namespace)
constexpr const char* opencv_key = "image.local_inpaint_navier_stokes_openCV";
constexpr std::uint64_t height = 9;
constexpr std::uint64_t width = 9;
constexpr std::uint64_t channels = 4;
constexpr std::int64_t default_radius = 3;
constexpr std::uint64_t padded_state_bytes = 7;
constexpr std::uint64_t padded_heap_bytes = 32;

/** @brief Throws when one public expectation does not hold. */
void require(bool condition, const std::string& reason) {
  if (!condition)
    throw std::runtime_error(reason);
}
template <class T>
T take(ps::Result<T> result) {
  require(result.ok(), result.status().message);
  return result.take_value();
}

float sample(const ps::Value& value, std::uint64_t y, std::uint64_t x,
             std::uint64_t channel) {
  const auto address = take(value.byte_address({y, x, channel}));
  float number = 0;
  std::memcpy(&number, value.bytes().data() + address, sizeof(number));
  return number;
}

/** @brief Opaque linear RGBA fixture with a visible gradient and a line. */
std::vector<float> fixture_rgba() {
  std::vector<float> rgba(height * width * channels);
  for (std::uint64_t y = 0; y < height; ++y)
    for (std::uint64_t x = 0; x < width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y * width + x);
      const bool line = (y + x) % 5 == 0;
      rgba[index * channels + 0] = line ? 0.9F : static_cast<float>(y) * 0.07F;
      rgba[index * channels + 1] = line ? 0.2F : static_cast<float>(x) * 0.05F;
      rgba[index * channels + 2] = line ? 0.1F : 0.4F;
      rgba[index * channels + 3] = 1.0F;
    }
  return rgba;
}

/** @brief Binary coverage with one interior hole and one corner hole. */
std::vector<float> fixture_mask() {
  std::vector<float> mask(height * width, 0.0F);
  mask[static_cast<std::size_t>(4 * width + 4)] = 1.0F;
  mask[static_cast<std::size_t>(2 * width + 5)] = 1.0F;
  mask[static_cast<std::size_t>((height - 1) * width + (width - 1))] = 1.0F;
  return mask;
}

ps::Value image_value(const std::vector<float>& rgba) {
  const std::vector<std::uint64_t> shape{height, width, channels};
  auto writer = take(ps::MutableValue::allocate(
      {ps::ElementType::Float32, shape}, ps::Region::whole(shape),
      ps::BufferAllocator{}));
  std::memcpy(writer.data(), rgba.data(), rgba.size() * sizeof(float));
  return take(std::move(writer).publish(
      {take(ps::encode_semantic(ps::rgba_semantics()))}));
}

ps::Value mask_value(const std::vector<float>& mask) {
  const std::vector<std::uint64_t> shape{height, width};
  auto writer = take(ps::MutableValue::allocate(
      {ps::ElementType::Float32, shape}, ps::Region::whole(shape),
      ps::BufferAllocator{}));
  std::memcpy(writer.data(), mask.data(), mask.size() * sizeof(float));
  return take(std::move(writer).publish(
      {take(ps::encode_semantic(ps::coverage_semantics()))}));
}

/** @brief Returns the maintained registry keys required by the profile. */
std::vector<std::string> selected_keys(
    const std::shared_ptr<ps::OperationRegistry>& registry) {
  const auto available = registry->keys();
  std::vector<std::string> keys;
  for (const char* name : {native_key, opencv_key})
    if (std::find(available.begin(), available.end(), std::string(name)) !=
        available.end())
      keys.push_back(name);
  require(!keys.empty(), "no PNT-05A navier_stokes variant is registered");
  return keys;
}

std::uint64_t current_max_rss_bytes() {
#ifdef PHOTOSPIDER_EXAMPLE_HAS_RSS
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return 0;
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#else
  return 0;
#endif
}

/** @brief Runs one variant and prints the complete public evidence. */
void run_variant(const std::string& key, const ps::Value& image,
                 const ps::Value& holes, std::int64_t radius) {
  ps::WorkflowDocument document;
  document.inputs = {{1, "image", image.descriptor(), image.region(),
                      image.layout(), image.facets()},
                     {2, "hole_mask", holes.descriptor(), holes.region(),
                      holes.layout(), holes.facets()}};
  document.nodes = {
      {1,
       key,
       {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
       {{"radius", radius}}}};
  document.outputs = {{"image", 1, "image"}};
  auto registry = ps::make_default_operation_registry();
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext execution(registry, {2, false, 8, 1U << 30});
  ps::ExecutionBindings bindings;
  bindings.inputs.push_back({"image", image});
  bindings.inputs.push_back({"hole_mask", holes});
  const std::uint64_t before = current_max_rss_bytes();
  const auto result = take(execution.execute(compiled.plan, bindings));
  const std::uint64_t after = current_max_rss_bytes();
  require(result.values.count("image") == 1U, "named image output is absent");
  const auto& named = result.values.at("image");
  std::cout << '[' << key << "] plan_digest=" << compiled.plan.digest().value;
  std::cout << " steps=" << result.diagnostics.operation_timings.size();
  std::cout << " peak_live_bytes=" << result.diagnostics.peak_live_bytes
            << '\n';
  std::cout << '[' << key << "] descriptor="
            << (named.descriptor().element_type == ps::ElementType::Float32
                    ? "Float32"
                    : "other")
            << " shape=";
  for (const auto extent : named.descriptor().shape)
    std::cout << extent << ',';
  std::cout << " region=";
  for (const auto& axis : named.region().dimensions())
    std::cout << axis.offset << ':' << axis.extent << ',';
  std::cout << " byte_offset=" << named.layout().byte_offset << " strides=";
  for (const auto stride : named.layout().byte_strides)
    std::cout << stride << ',';
  std::cout << " facets=" << named.facets().size();
  for (const auto& facet : named.facets())
    std::cout << ' ' << facet.key << '@' << facet.version
              << " payload_bytes=" << facet.payload.size();
  std::cout << '\n';
  const auto mask = fixture_mask();
  const auto rgba = fixture_rgba();
  std::uint64_t preserved = 0;
  std::uint64_t holes_checked = 0;
  bool finite = true;
  for (std::uint64_t y = 0; y < height; ++y)
    for (std::uint64_t x = 0; x < width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y * width + x);
      const bool hole = mask[index] == 1.0F;
      for (std::uint64_t channel = 0; channel < channels; ++channel) {
        const float got = sample(named, y, x, channel);
        if (hole && channel < 3) {
          finite = finite && std::isfinite(got);
          ++holes_checked;
          continue;
        }
        const float want = rgba[index * channels + channel];
        require(std::memcmp(&got, &want, sizeof(float)) == 0,
                "an unmasked sample or alpha sample changed");
        ++preserved;
      }
    }
  require(finite, "a hole sample is not finite");
  std::cout << '[' << key << "] preserved_samples=" << preserved
            << " hole_samples=" << holes_checked << " hole_finite=yes";
  std::cout << " hole_value=" << sample(named, 4, 4, 0) << ','
            << sample(named, 4, 4, 1) << ',' << sample(named, 4, 4, 2) << '\n';
  const std::uint64_t padded = (height + 2) * (width + 2);
  std::cout << '[' << key << "] host_scratch_model="
            << (key == std::string(native_key)
                    ? 5 * height * width + 23 * padded
                    : 9 * height * width)
            << " bytes";
  if (key == std::string(opencv_key)) {
    // Declared adapter limitation: the pinned library allocates its guard grid
    // and heap vector outside the invocation allocator. Estimated from the
    // pinned source (7 bytes per padded sample plus a 16-byte heap entry per
    // padded sample at two-times geometric capacity); never charged to the
    // host execution budget.
    std::cout << " external_estimate="
              << padded * (padded_state_bytes + padded_heap_bytes) << " bytes";
    if (before != 0 && after > before)
      std::cout << " observed_max_rss_increase=" << (after - before)
                << " bytes(upper bound)";
  }
  std::cout << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::string requested;
    std::int64_t radius = default_radius;
    for (int argument = 1; argument < argc; ++argument) {
      const std::string option = argv[argument];
      if (option == "--key" && argument + 1 < argc) {
        requested = argv[++argument];
      } else if (option == "--radius" && argument + 1 < argc) {
        radius = std::stoll(argv[++argument]);
      } else {
        std::cerr << "usage: " << argv[0] << " [--key <operation key>]"
                  << " [--radius <1..32>]\n";
        return 2;
      }
    }
    require(radius >= 1 && radius <= 32, "radius must be inside [1,32]");
    const auto rgba = fixture_rgba();
    const auto mask = fixture_mask();
    const auto image = image_value(rgba);
    const auto holes = mask_value(mask);
    auto registry = ps::make_default_operation_registry();
    const auto keys = selected_keys(registry);
    std::cout << "PNT-05A local_inpaint_navier_stokes radius=" << radius
              << " registry_keys=" << registry->keys().size() << '\n';
    for (const auto& key : keys) {
      if (!requested.empty() && requested != key)
        continue;
      run_variant(key, image, holes, radius);
    }
    if (!requested.empty() &&
        std::find(keys.begin(), keys.end(), requested) == keys.end()) {
      std::cerr << "requested key is not registered: " << requested << '\n';
      return 2;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "inpaint_ns_workflow failed: " << error.what() << '\n';
    return 1;
  }
}
