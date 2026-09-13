/**
 * @file test_local_inpaint_navier_stokes.cpp
 * @brief PNT-05A acceptance for both required navier_stokes variants.
 *
 * Covers the frozen profile contract of
 * `docs/built-in_ops/09-composite/op_specs/PNT-05A_local_inpaint_navier_stokes.md`
 * revision 0.3.0: exact parameter schema, complete logical-domain validation,
 * hole zeroing, unmasked bit preservation, Whole Region behavior, resources,
 * cancellation, determinism and the public registry -> compile -> bind ->
 * execute -> named `image` path. The pinned OpenCV 4.12.0 oracle is compiled in
 * only when `PS_INPAINT_NAVIER_STOKES_ORACLE` is defined; it calls the library
 * directly and never the production helper.
 *
 * Acceptance-matrix mapping, commands, evidence and gaps are recorded in
 * `examples/inpaint_ns_workflow/README.md`; the operator contract is in
 * `docs/kernel-architecture/Image-Operations.md`.
 */

#include <algorithm>
#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

#ifdef PS_INPAINT_NAVIER_STOKES_ORACLE
#include <opencv2/core.hpp>
#include <opencv2/photo.hpp>
#endif

namespace {
using ps::ErrorCode;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ps::ParameterValue>;

constexpr const char* native_key =
    "image.local_inpaint_navier_stokes_native_apple_silicon";  // NOLINT(whitespace/indent_namespace)
constexpr const char* opencv_key = "image.local_inpaint_navier_stokes_openCV";

/** @brief Oracle-comparison evidence of one variant. */
struct VariantStats {
  std::uint64_t samples = 0;
  std::uint64_t bit_differences = 0;
  std::uint64_t above_half_tolerance = 0;
  double worst_absolute = 0;
  double worst_tolerance_fraction = 0;
};
/** @brief Per-variant oracle evidence keyed by operation key. */
std::map<std::string, VariantStats> oracle_stats;
/** @brief Cross-variant differences and their worst relative magnitude. */
std::uint64_t variant_bit_differences = 0;
double variant_worst_relative = 0;

/** @brief Records one failed condition with its reason. */
void require(bool condition, const std::string& reason) {
  if (!condition)
    throw std::runtime_error(reason);
}
template <class T>
T take(ps::Result<T> result) {
  require(result.ok(), result.status().message);
  return result.take_value();
}

/** @brief Logical grid of one profile fixture. */
struct Grid final {
  std::uint64_t height = 0;
  std::uint64_t width = 0;
  std::uint64_t pixels() const noexcept { return height * width; }
  std::uint64_t padded() const noexcept { return (height + 2) * (width + 2); }
};

bool same_region(const ps::Region& left, const ps::Region& right) {
  if (left.rank() != right.rank())
    return false;
  for (std::size_t axis = 0; axis < left.rank(); ++axis)
    if (left.dimensions()[axis].offset != right.dimensions()[axis].offset ||
        left.dimensions()[axis].extent != right.dimensions()[axis].extent)
      return false;
  return true;
}

bool same_facets(const ps::ValueFacet& left, const ps::ValueFacet& right) {
  return left.key == right.key && left.version == right.version &&
         left.payload == right.payload;
}

/** @brief Builds one Float32 RGBA image with the canonical image profile. */
ps::Value image_value(Grid grid, const std::vector<float>& rgba) {
  const std::vector<std::uint64_t> shape{grid.height, grid.width, 4};
  require(rgba.size() == grid.pixels() * 4, "image fixture size mismatch");
  auto writer = take(ps::MutableValue::allocate(
      {ps::ElementType::Float32, shape}, ps::Region::whole(shape),
      ps::BufferAllocator{}));
  std::memcpy(writer.data(), rgba.data(), rgba.size() * sizeof(float));
  return take(std::move(writer).publish(
      {take(ps::encode_semantic(ps::rgba_semantics()))}));
}

/** @brief Builds one Float32 coverage mask with canonical typed semantics. */
ps::Value mask_value(Grid grid, const std::vector<float>& mask) {
  const std::vector<std::uint64_t> shape{grid.height, grid.width};
  require(mask.size() == grid.pixels(), "mask fixture size mismatch");
  auto writer = take(ps::MutableValue::allocate(
      {ps::ElementType::Float32, shape}, ps::Region::whole(shape),
      ps::BufferAllocator{}));
  std::memcpy(writer.data(), mask.data(), mask.size() * sizeof(float));
  return take(std::move(writer).publish(
      {take(ps::encode_semantic(ps::coverage_semantics()))}));
}

/** @brief Reads one Float32 sample through checked logical addressing. */
float sample(const ps::Value& value, std::uint64_t y, std::uint64_t x,
             std::uint64_t channel) {
  const auto address = take(value.byte_address({y, x, channel}));
  float number = 0;
  std::memcpy(&number, value.bytes().data() + address, sizeof(number));
  return number;
}

/** @brief Allocator accounting shared by the resource and cancellation rows. */
struct Budget final {
  std::uint64_t live = 0;
  std::uint64_t peak = 0;
  std::uint64_t allocations = 0;
  std::uint64_t limit = 0;
  std::function<void(std::uint64_t)> after_allocation;
  std::function<void(std::uint64_t)> before_allocation;

  /** @brief Reservation callback with an optional exact byte ceiling. */
  ps::BufferAllocator allocator() {
    return ps::BufferAllocator(
        [this](std::uint64_t size) -> ps::Result<std::shared_ptr<void>> {
          if (before_allocation)
            before_allocation(allocations + 1);
          if (limit != 0 && size > limit - live)
            return ps::Result<std::shared_ptr<void>>(ps::Status::failure(
                ErrorCode::ResourceExhausted, "fixture budget"));
          live += size;
          peak = std::max(peak, live);
          ++allocations;
          if (after_allocation)
            after_allocation(allocations);
          return ps::Result<std::shared_ptr<void>>(
              std::shared_ptr<void>(new int(0), [this, size](void* pointer) {
                delete static_cast<int*>(pointer);
                live -= size;
              }));
        });
  }
};

/** @brief Exact host-owned scratch bytes declared by each variant. */
std::uint64_t declared_scratch(const std::string& key, Grid grid) {
  if (key == native_key)
    return 5 * grid.pixels() + 23 * grid.padded();
  return 9 * grid.pixels();
}

/** @brief Host-owned allocations of one profile invocation. */
std::uint64_t declared_allocations(const std::string& key) {
  return key == native_key ? 6U : 4U;
}

ps::Result<ps::Value> invoke(
    const std::string& key, const ps::Value& image, const ps::Value& mask,
    const Parameters& parameters, ps::Region output = {},
    ps::BufferAllocator allocator = ps::BufferAllocator(),
    ps::CancellationToken cancellation = {}) {
  auto registry = ps::make_default_operation_registry();
  const std::vector<ps::Value> inputs{image, mask};
  const std::vector<ps::Region> demands{image.region(), mask.region()};
  return registry->invoke(
      key, {inputs, demands, parameters, ps::Backend::Cpu, cancellation,
            std::move(output), std::move(allocator)});
}

Parameters radius_parameters(std::int64_t radius) {
  return {{"radius", radius}};
}

/** @brief Requires that one invocation fails with an exact error category. */
void rejected(const std::string& key, const ps::Value& image,
              const ps::Value& mask, const Parameters& parameters,
              ErrorCode code, const std::string& label) {
  const auto result = invoke(key, image, mask, parameters);
  require(!result.ok() && result.status().code == code,
          label + ": unexpected failure category");
}

bool bit_identical(const ps::Value& left, const ps::Value& right) {
  return left.bytes().size() == right.bytes().size() &&
         std::memcmp(left.bytes().data(), right.bytes().data(),
                     left.bytes().size()) == 0;
}

#ifdef PS_INPAINT_NAVIER_STOKES_ORACLE
/**
 * @brief Independent pinned oracle plane for one fixture and channel.
 * @note Calls OpenCV 4.12.0 `INPAINT_NS` on a Float32 plane with zeroed hole
 * samples and restores every unmasked sample from the fixture. It never calls
 * the production helper.
 */
std::vector<float> oracle_plane(Grid grid, const std::vector<float>& rgba,
                                const std::vector<float>& mask,
                                std::int64_t radius, std::uint64_t channel) {
  const std::size_t pixels = static_cast<std::size_t>(grid.pixels());
  std::vector<float> source(pixels);
  std::vector<std::uint8_t> holes(pixels);
  for (std::size_t index = 0; index < pixels; ++index) {
    const bool hole = mask[index] == 1.0F;
    holes[index] = hole ? 255 : 0;
    source[index] = hole ? 0.0F : rgba[index * 4 + channel];
  }
  const std::size_t step = static_cast<std::size_t>(grid.width) * sizeof(float);
  const cv::Mat input(static_cast<int>(grid.height),
                      static_cast<int>(grid.width), CV_32FC1, source.data(),
                      step);
  const cv::Mat hole_mask(static_cast<int>(grid.height),
                          static_cast<int>(grid.width), CV_8UC1, holes.data(),
                          static_cast<std::size_t>(grid.width));
  cv::Mat solved;
  cv::inpaint(input, hole_mask, solved, static_cast<double>(radius),
              cv::INPAINT_NS);
  std::vector<float> result(pixels);
  std::memcpy(result.data(), solved.data, pixels * sizeof(float));
  for (std::size_t index = 0; index < pixels; ++index)
    if (holes[index] == 0)
      result[index] = rgba[index * 4 + channel];
  return result;
}
#endif

/** @brief Compares every hole sample of one result against the oracle. */
void compare_oracle(Grid grid, const std::vector<float>& rgba,
                    const std::vector<float>& mask, std::int64_t radius,
                    const ps::Value& actual, const std::string& key,
                    const std::string& label) {
#ifdef PS_INPAINT_NAVIER_STOKES_ORACLE
  auto& stats = oracle_stats[key];
  for (std::uint64_t channel = 0; channel < 3; ++channel) {
    const auto reference = oracle_plane(grid, rgba, mask, radius, channel);
    for (std::uint64_t y = 0; y < grid.height; ++y)
      for (std::uint64_t x = 0; x < grid.width; ++x) {
        const std::size_t index = static_cast<std::size_t>(y * grid.width + x);
        if (mask[index] != 1.0F)
          continue;
        const float want = reference[index];
        const float got = sample(actual, y, x, channel);
        require(std::isfinite(want),
                label + ": pinned oracle produced a nonfinite hole sample");
        require(std::isfinite(got), label + ": nonfinite hole sample");
        const double difference =
            std::fabs(static_cast<double>(got) - static_cast<double>(want));
        require(
            difference <= 1e-6 + 1e-5 * std::fabs(static_cast<double>(want)),
            label + ": pinned oracle tolerance exceeded");
        const double tolerance =
            1e-6 + 1e-5 * std::fabs(static_cast<double>(want));
        if (std::memcmp(&got, &want, sizeof(float)) != 0)
          stats.bit_differences += 1;
        if (difference > 0.5 * tolerance)
          stats.above_half_tolerance += 1;
        stats.samples += 1;
        stats.worst_absolute = std::max(stats.worst_absolute, difference);
        stats.worst_tolerance_fraction =
            std::max(stats.worst_tolerance_fraction, difference / tolerance);
      }
  }
#else
  (void)grid;
  (void)rgba;
  (void)mask;
  (void)radius;
  (void)actual;
  (void)key;
  (void)label;
#endif
}

/** @brief Compares the two variants on every hole sample and channel. */
void compare_variants(Grid grid, const ps::Value& left, const ps::Value& right,
                      const std::string& label) {
  for (std::uint64_t y = 0; y < grid.height; ++y)
    for (std::uint64_t x = 0; x < grid.width; ++x)
      for (std::uint64_t channel = 0; channel < 3; ++channel) {
        const float a = sample(left, y, x, channel);
        const float b = sample(right, y, x, channel);
        const double difference =
            std::fabs(static_cast<double>(a) - static_cast<double>(b));
        if (std::memcmp(&a, &b, sizeof(float)) != 0)
          ++variant_bit_differences;
        variant_worst_relative = std::max(
            variant_worst_relative,
            difference / (1e-6 + 1e-5 * std::fabs(static_cast<double>(b))));
        require(difference <= 1e-6 + 1e-5 * std::fabs(static_cast<double>(b)),
                label + ": variant divergence exceeds the profile tolerance");
      }
}

/** @brief Solid RGBA fixture with alpha exactly one. */
std::vector<float> constant_image(Grid grid, float r, float g, float b) {
  std::vector<float> rgba(grid.pixels() * 4);
  for (std::size_t index = 0; index < grid.pixels(); ++index) {
    rgba[index * 4 + 0] = r;
    rgba[index * 4 + 1] = g;
    rgba[index * 4 + 2] = b;
    rgba[index * 4 + 3] = 1.0F;
  }
  return rgba;
}

/** @brief Smooth continuous fixture with distinguishable planes. */
std::vector<float> gradient_image(Grid grid) {
  std::vector<float> rgba(grid.pixels() * 4);
  for (std::uint64_t y = 0; y < grid.height; ++y)
    for (std::uint64_t x = 0; x < grid.width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y * grid.width + x);
      rgba[index * 4 + 0] = static_cast<float>(y) * 0.05F;
      rgba[index * 4 + 1] = static_cast<float>(x) * 0.03F;
      rgba[index * 4 + 2] = static_cast<float>(y + x) * 0.02F - 0.4F;
      rgba[index * 4 + 3] = 1.0F;
    }
  return rgba;
}

/** @brief Asymmetric thin lines, including a one-pixel diagonal. */
std::vector<float> lines_image(Grid grid) {
  auto rgba = constant_image(grid, 0.2F, 0.4F, 0.6F);
  for (std::uint64_t y = 0; y < grid.height; ++y)
    for (std::uint64_t x = 0; x < grid.width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y * grid.width + x);
      const bool line = y == grid.height / 2 || x == grid.width / 3 ||
                        (y == x) || (y + 1 == grid.width - x);
      if (line) {
        rgba[index * 4 + 0] = 0.9F;
        rgba[index * 4 + 1] = 0.1F;
        rgba[index * 4 + 2] = 0.35F;
      }
    }
  return rgba;
}

/** @brief Checkerboard fixture with two block scales. */
std::vector<float> checker_image(Grid grid) {
  std::vector<float> rgba(grid.pixels() * 4);
  for (std::uint64_t y = 0; y < grid.height; ++y)
    for (std::uint64_t x = 0; x < grid.width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y * grid.width + x);
      const bool first = ((y / 3) + (x / 3)) % 2 == 0;
      const bool second = ((y / 7) + (x / 7)) % 2 == 0;
      rgba[index * 4 + 0] = first ? 0.85F : 0.1F;
      rgba[index * 4 + 1] = second ? 0.7F : 0.2F;
      rgba[index * 4 + 2] = first == second ? 0.05F : 0.9F;
      rgba[index * 4 + 3] = 1.0F;
    }
  return rgba;
}

std::vector<float> zero_mask(Grid grid) {
  return std::vector<float>(grid.pixels(), 0.0F);
}

/** @brief Rectangular hole region clipped to the grid. */
std::vector<float> block_mask(Grid grid, std::uint64_t y0, std::uint64_t x0,
                              std::uint64_t height, std::uint64_t width) {
  std::vector<float> mask(grid.pixels(), 0.0F);
  for (std::uint64_t y = y0; y < std::min(grid.height, y0 + height); ++y)
    for (std::uint64_t x = x0; x < std::min(grid.width, x0 + width); ++x)
      mask[static_cast<std::size_t>(y * grid.width + x)] = 1.0F;
  return mask;
}

/** @brief One-pixel diagonal scratch with two isolated single-pixel holes. */
std::vector<float> scratch_mask(Grid grid) {
  auto mask = zero_mask(grid);
  for (std::uint64_t step = 0; step < std::min(grid.height, grid.width); ++step)
    mask[static_cast<std::size_t>(step * grid.width + step)] = 1.0F;
  mask[1] = 1.0F;
  mask[static_cast<std::size_t>(grid.width - 1)] = 1.0F;
  return mask;
}

std::vector<float> centered_mask(Grid grid, std::uint64_t height,
                                 std::uint64_t width) {
  return block_mask(grid, (grid.height - height) / 2, (grid.width - width) / 2,
                    height, width);
}

/** @brief Reports which required variants the frozen registry publishes. */
std::vector<std::string> selected_keys(
    const std::shared_ptr<ps::OperationRegistry>& registry) {
  const auto available = registry->keys();
  const auto present = [&available](const char* key) {
    return std::find(available.begin(), available.end(), std::string(key)) !=
           available.end();
  };
  require(present(native_key),
          "image.local_inpaint_navier_stokes_native_apple_silicon is absent");
#ifdef PS_INPAINT_NAVIER_STOKES_ADAPTER
  require(present(opencv_key),
          "image.local_inpaint_navier_stokes_openCV is absent");
#else
  require(!present(opencv_key),
          "native-only builds must not publish the OpenCV adapter");
#endif
  std::vector<std::string> keys{native_key};
  if (present(opencv_key))
    keys.push_back(opencv_key);
  return keys;
}

/** @brief T08 and the schema half of T17: closed traits and parameter schema.
 */
void registration_and_schema() {
  auto registry = ps::make_default_operation_registry();
  for (const auto& name : selected_keys(registry)) {
    auto traits = take(registry->find_traits(name));
    require(traits.input_count == 2U, "profile requires exactly two inputs");
    require(traits.input_schema.size() == 2U, "profile port count");
    // Typed Image semantics accept the canonical profile with either the scene
    // or the display reference; the callback validates the remaining fields.
    const auto typed_image = [](const ps::OperationPortConstraint& port) {
      return port.kind == ps::OperationPortKind::Typed &&
             port.semantic_kind ==
                 static_cast<std::uint32_t>(ps::SemanticKind::Image) &&
             port.element_type ==
                 static_cast<std::uint32_t>(ps::ElementType::Float32) &&
             port.rank == 3;
    };
    require(
        typed_image(traits.input_schema[0]) &&
            traits.input_schema[1].kind == ps::OperationPortKind::Float32Mask,
        "profile port kinds");
    require(traits.outputs.size() == 1U && traits.outputs[0].key == "image",
            "the only named output port is image");
    require(traits.outputs[0].shape_rule ==
                    ps::OperationShapeRule::PreserveFirstInput &&
                traits.outputs[0].region_rule == ps::OperationRegionRule::Whole,
            "profile shape/Region rules");
    require(typed_image(traits.outputs[0].output_schema),
            "profile output kind");
    require(traits.parameter_schema.size() == 1U, "closed parameter schema");
    const auto& radius = traits.parameter_schema[0];
    require(radius.key == "radius" &&
                radius.type == ps::OperationParameterType::Int64 &&
                radius.required && radius.bounded && radius.minimum == 1 &&
                radius.maximum == 32,
            "radius must be a required bounded Int64 in [1,32]");
    require(traits.supports_cpu && !traits.supports_gpu &&
                !traits.allows_cpu_fallback,
            "profile is CPU only without implicit fallback");
    require(traits.deterministic && traits.side_effect_free && traits.cacheable,
            "profile determinism traits");
  }
  const Grid grid{5, 5};
  const auto image =
      image_value(grid, constant_image(grid, 0.25F, 0.5F, 0.75F));
  const auto mask = mask_value(grid, centered_mask(grid, 1, 1));
  for (const auto& key : selected_keys(registry)) {
    for (const auto radius : {std::int64_t{1}, std::int64_t{32}})
      require(invoke(key, image, mask, radius_parameters(radius)).ok(),
              "inclusive radius endpoint was rejected");
    for (const auto radius : {std::int64_t{0}, std::int64_t{-1},
                              std::int64_t{33}, std::int64_t{100}}) {
      rejected(key, image, mask, radius_parameters(radius),
               ErrorCode::InvalidArgument, "radius range");
    }
    Parameters wrong_type{{"radius", 3.0}};
    rejected(key, image, mask, wrong_type, ErrorCode::InvalidArgument,
             "Float64 radius");
    Parameters absent;
    rejected(key, image, mask, absent, ErrorCode::InvalidArgument,
             "absent radius");
    Parameters unknown = radius_parameters(3);
    unknown["method"] = std::string("ns");
    rejected(key, image, mask, unknown, ErrorCode::InvalidArgument,
             "unknown parameter");
    Parameters flag = radius_parameters(3);
    flag["iterations"] = std::int64_t{1};
    rejected(key, image, mask, flag, ErrorCode::InvalidArgument,
             "solver parameter");
  }
}

/** @brief T01: complete validation precedes the all-zero identity result. */
void zero_mask_identity(const std::vector<std::string>& keys) {
  const Grid grid{6, 7};
  const auto rgba = gradient_image(grid);
  auto mask = zero_mask(grid);
  mask[0] = -0.0F;
  mask[3] = -0.0F;
  const auto image = image_value(grid, rgba);
  const auto holes = mask_value(grid, mask);
  auto straight = ps::rgba_semantics();
  straight.association = "straight";
  auto straight_writer = take(ps::MutableValue::allocate(
      image.descriptor(), image.region(), ps::BufferAllocator{}));
  std::memcpy(straight_writer.data(), rgba.data(), rgba.size() * sizeof(float));
  const auto straight_image =
      take(std::move(straight_writer)
               .publish({take(ps::encode_semantic(straight))}));
  for (const auto& key : keys) {
    const auto result = take(invoke(key, image, holes, radius_parameters(3)));
    require(bit_identical(result, image),
            "zero mask must return the complete unedited input bits");
    require(result.facets().size() == 1 &&
                same_facets(result.facets()[0], image.facets()[0]),
            "zero mask must preserve image facets");
    Parameters bad_radius = radius_parameters(0);
    rejected(key, image, holes, bad_radius, ErrorCode::InvalidArgument,
             "validation before noop");
    auto half = mask;
    half[2] = 0.5F;
    rejected(key, image, mask_value(grid, half), radius_parameters(3),
             ErrorCode::OperationFailed, "0.5 coverage inside a noop mask");
    auto alpha = rgba;
    alpha[3] = 0.5F;
    rejected(key, image_value(grid, alpha), holes, radius_parameters(3),
             ErrorCode::OperationFailed, "nonopaque alpha with noop mask");
    auto infinite = rgba;
    infinite[4 * 5 + 3] = std::numeric_limits<float>::infinity();
    require(
        !invoke(key, image_value(grid, infinite), holes, radius_parameters(3))
             .ok(),
        "nonfinite RGB must fail even for a noop mask");
    require(!invoke(key, straight_image, holes, radius_parameters(3)).ok(),
            "non-premultiplied image profile was accepted");
  }
}

/** @brief T03: hole placeholders never influence the published result. */
void placeholder_independence(const std::vector<std::string>& keys) {
  const Grid grid{9, 11};
  const auto rgba = gradient_image(grid);
  const auto mask = centered_mask(grid, 3, 4);
  const auto holes = mask_value(grid, mask);
  std::vector<float> first = rgba, second = rgba;
  for (std::uint64_t y = 0; y < grid.height; ++y)
    for (std::uint64_t x = 0; x < grid.width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y * grid.width + x);
      if (mask[index] == 0)
        continue;
      first[index * 4 + 0] = 1234.5F;
      first[index * 4 + 1] = -3.25F;
      first[index * 4 + 2] = -0.0F;
      second[index * 4 + 0] = -9e30F;
      second[index * 4 + 1] = 4.5e-30F;
      second[index * 4 + 2] = 77.0F;
    }
  for (const auto& key : keys) {
    const auto a = take(
        invoke(key, image_value(grid, first), holes, radius_parameters(2)));
    const auto b = take(
        invoke(key, image_value(grid, second), holes, radius_parameters(2)));
    require(bit_identical(a, b),
            "finite hole placeholders changed the published result");
  }
}

/** @brief T02: constant image, single center hole, radius one. */
void analytic_constant(const std::vector<std::string>& keys) {
  const Grid grid{5, 5};
  const std::vector<float> channels{0.25F, 0.5F, 0.75F};
  const auto rgba = constant_image(grid, channels[0], channels[1], channels[2]);
  const auto mask = centered_mask(grid, 1, 1);
  const auto image = image_value(grid, rgba);
  const auto holes = mask_value(grid, mask);
  for (const auto& key : keys) {
    const auto result = take(invoke(key, image, holes, radius_parameters(1)));
    for (std::uint64_t y = 0; y < grid.height; ++y)
      for (std::uint64_t x = 0; x < grid.width; ++x) {
        const std::size_t index = static_cast<std::size_t>(y * grid.width + x);
        for (std::uint64_t channel = 0; channel < 4; ++channel) {
          const float got = sample(result, y, x, channel);
          const float want = rgba[index * 4 + channel];
          if (channel < 3)
            require(std::fabs(static_cast<double>(got) -
                              static_cast<double>(want)) <= 1e-6,
                    "constant hole must keep the constant color");
          else
            require(std::memcmp(&got, &want, sizeof(float)) == 0,
                    "alpha must be bitwise preserved at one");
        }
      }
    compare_oracle(grid, rgba, mask, 1, result, key, "T02");
  }
  if (keys.size() > 1)
    compare_variants(grid,
                     take(invoke(keys[0], image, holes, radius_parameters(1))),
                     take(invoke(keys[1], image, holes, radius_parameters(1))),
                     "T02 variant agreement");
}

/** @brief T04 and T05: pinned Float32 oracle, edges, corners and components. */
void pinned_oracle(const std::vector<std::string>& keys) {
  struct Fixture final {
    Grid grid;
    std::vector<float> rgba;
    std::vector<float> mask;
    std::int64_t radius;
    std::string label;
  };
  const Grid small{12, 15};
  const Grid wide{7, 33};
  std::vector<Fixture> fixtures;
  fixtures.push_back({small, gradient_image(small), centered_mask(small, 4, 5),
                      1, "gradient r1"});
  fixtures.push_back({small, gradient_image(small), centered_mask(small, 4, 5),
                      3, "gradient r3"});
  fixtures.push_back(
      {small, lines_image(small), scratch_mask(small), 2, "scratch r2"});
  fixtures.push_back({small, checker_image(small), centered_mask(small, 5, 5),
                      2, "checker r2"});
  fixtures.push_back({wide, checker_image(wide), block_mask(wide, 0, 0, 2, 3),
                      2, "corner hole r2"});
  fixtures.push_back(
      {wide, gradient_image(wide), scratch_mask(wide), 1, "edge line r1"});
  fixtures.push_back({small, lines_image(small), block_mask(small, 0, 0, 1, 1),
                      1, "single corner pixel r1"});
  fixtures.push_back({small, lines_image(small), block_mask(small, 0, 10, 3, 5),
                      3, "right edge block r3"});
  fixtures.push_back({small, lines_image(small), block_mask(small, 9, 0, 3, 4),
                      2, "bottom edge block r2"});
  {
    auto mask = zero_mask(small);
    mask[1] = 1.0F;
    mask[static_cast<std::size_t>(small.width - 1)] = 1.0F;
    mask[static_cast<std::size_t>(small.width * 5 + 7)] = 1.0F;
    fixtures.push_back(
        {small, gradient_image(small), mask, 2, "disjoint holes r2"});
  }
  fixtures.push_back({wide, checker_image(wide), block_mask(wide, 2, 2, 3, 3),
                      32, "radius 32"});
  fixtures.push_back({wide, checker_image(wide), block_mask(wide, 3, 3, 1, 20),
                      8, "scratch r8"});
  for (const auto& fixture : fixtures) {
    const auto image = image_value(fixture.grid, fixture.rgba);
    const auto holes = mask_value(fixture.grid, fixture.mask);
    for (const auto& key : keys) {
      const auto result =
          take(invoke(key, image, holes, radius_parameters(fixture.radius)));
      compare_oracle(fixture.grid, fixture.rgba, fixture.mask, fixture.radius,
                     result, key, fixture.label);
      for (std::uint64_t y = 0; y < fixture.grid.height; ++y)
        for (std::uint64_t x = 0; x < fixture.grid.width; ++x) {
          const std::size_t index =
              static_cast<std::size_t>(y * fixture.grid.width + x);
          if (fixture.mask[index] == 1.0F)
            continue;
          for (std::uint64_t channel = 0; channel < 4; ++channel) {
            const float got = sample(result, y, x, channel);
            require(std::memcmp(&got, &fixture.rgba[index * 4 + channel],
                                sizeof(float)) == 0,
                    fixture.label + ": unmasked sample changed");
          }
        }
    }
    if (keys.size() > 1)
      compare_variants(fixture.grid,
                       take(invoke(keys[0], image, holes,
                                   radius_parameters(fixture.radius))),
                       take(invoke(keys[1], image, holes,
                                   radius_parameters(fixture.radius))),
                       fixture.label + " variant agreement");
  }
  // One known sample: the pinned result stays finite where the frontier runs.
  const Grid single{5, 5};
  auto single_mask = block_mask(single, 0, 0, single.height, single.width);
  single_mask[0] = 0.0F;
  const auto one_known = mask_value(single, single_mask);
  for (const auto& key : keys) {
    const auto result = take(invoke(
        key, image_value(single, constant_image(single, 0.3F, 0.6F, 0.9F)),
        one_known, radius_parameters(2)));
    for (std::uint64_t y = 0; y < single.height; ++y)
      for (std::uint64_t x = 0; x < single.width; ++x) {
        if (y == 0 && x == 0)
          continue;
        for (std::uint64_t channel = 0; channel < 3; ++channel)
          require(std::isfinite(sample(result, y, x, channel)),
                  "single-known-sample holes must stay finite");
      }
  }
}

/** @brief T06: a full mask never publishes a result. */
void full_mask(const std::vector<std::string>& keys) {
  const Grid grid{5, 6};
  const auto image = image_value(grid, gradient_image(grid));
  const auto all_holes =
      mask_value(grid, block_mask(grid, 0, 0, grid.height, grid.width));
  for (const auto& key : keys)
    rejected(key, image, all_holes, radius_parameters(3),
             ErrorCode::OperationFailed, "full mask");
}

/** @brief T07: invalid coverage, alpha, RGB and mask samples by entry point. */
void invalid_inputs(const std::vector<std::string>& keys) {
  const Grid grid{6, 6};
  const auto rgba = gradient_image(grid);
  const auto mask = centered_mask(grid, 2, 2);
  const auto image = image_value(grid, rgba);
  const auto holes = mask_value(grid, mask);
  const auto with_sample = [&rgba, &grid](std::uint64_t index,
                                          std::uint64_t channel, float value) {
    auto copy = rgba;
    copy[index * 4 + channel] = value;
    return image_value(grid, copy);
  };
  for (const auto& key : keys) {
    for (const float value :
         {0.5F, 0.25F, -1.0F, 2.0F, std::numeric_limits<float>::quiet_NaN()}) {
      auto bad = mask;
      bad[7] = value;
      require(
          !invoke(key, image, mask_value(grid, bad), radius_parameters(3)).ok(),
          "invalid coverage reached a successful result");
    }
    auto half_alpha = rgba;
    half_alpha[4 * 9 + 3] = 0.5F;
    rejected(key, image_value(grid, half_alpha), holes, radius_parameters(3),
             ErrorCode::OperationFailed, "nonopaque alpha");
    require(
        !invoke(key, with_sample(9, 0, std::numeric_limits<float>::quiet_NaN()),
                holes, radius_parameters(3))
             .ok(),
        "nonfinite RGB reached a successful result");
    require(
        !invoke(key, with_sample(9, 2, std::numeric_limits<float>::infinity()),
                holes, radius_parameters(3))
             .ok(),
        "infinite RGB reached a successful result");
    const std::uint64_t hole_index = 2 * grid.width + 2;
    require(!invoke(key,
                    with_sample(hole_index, 1,
                                std::numeric_limits<float>::quiet_NaN()),
                    holes, radius_parameters(3))
                 .ok(),
            "nonfinite hole placeholder reached a successful result");
  }
}

/** @brief T09: extent bounds and rejection without real materialization. */
void extent_bounds(const std::vector<std::string>& keys) {
  const auto build = [](Grid grid) {
    return std::make_pair(
        image_value(grid, constant_image(grid, 0.5F, 0.5F, 0.5F)),
        mask_value(grid, centered_mask(grid, 1, 1)));
  };
  for (const auto& key : keys) {
    const Grid minimum{3, 3};
    auto minimum_fixture = build(minimum);
    require(invoke(key, minimum_fixture.first, minimum_fixture.second,
                   radius_parameters(1))
                .ok(),
            "the smallest admitted extent must run");
    for (const Grid too_small :
         {Grid{2, 5}, Grid{5, 2}, Grid{1, 1}, Grid{2, 2}}) {
      const auto fixture = build(too_small);
      rejected(key, fixture.first, fixture.second, radius_parameters(1),
               ErrorCode::TypeMismatch, "extent below three");
    }
    const Grid grid{5, 5};
    auto fixture = build(grid);
    const auto other = mask_value(Grid{5, 6}, zero_mask(Grid{5, 6}));
    require(!invoke(key, fixture.first, other, radius_parameters(1)).ok(),
            "mismatched mask grid was accepted");
  }
}

/** @brief T10: nonzero ROI, tile geometry and Whole global validation. */
void region_and_tiles(const std::vector<std::string>& keys) {
  const Grid grid{11, 14};
  const auto rgba = checker_image(grid);
  const auto mask = centered_mask(grid, 5, 6);
  const auto image = image_value(grid, rgba);
  const auto holes = mask_value(grid, mask);
  const ps::Region roi({{3, 5}, {4, 7}, {0, 4}});
  const auto bind = [&image, &holes] {
    ps::ExecutionBindings result;
    result.inputs.push_back({"image", image});
    result.inputs.push_back({"hole_mask", holes});
    return result;
  }();
  for (const auto& key : keys) {
    ps::WorkflowDocument document;
    document.inputs = {{1, "image", image.descriptor(), image.region(),
                        image.layout(), image.facets()},
                       {2, "hole_mask", holes.descriptor(), holes.region(),
                        holes.layout(), holes.facets()}};
    document.nodes = {
        {1,
         key,
         {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
         radius_parameters(3)}};
    document.outputs = {{"image", 1, "image"}};
    auto registry = ps::make_default_operation_registry();
    ps::GraphContext graph(document);
    ps::Compiler compiler(registry);
    ps::PlanningOptions options;
    options.tile_height = 3;
    options.tile_width = 4;
    auto whole = take(compiler.compile(graph, options));
    ps::ExecutionContext execution(registry,
                                   {2, false, 8, 1024U * 1024U * 1024U});
    const auto full = take(execution.execute(whole.plan, bind));
    require(full.values.count("image") == 1U,
            "the named image output is absent");
    const auto& whole_image = full.values.at("image");
    require(whole_image.descriptor().shape ==
                    std::vector<std::uint64_t>{grid.height, grid.width, 4} &&
                same_region(whole_image.region(),
                            ps::Region::whole({grid.height, grid.width, 4})),
            "Whole inference changed the output descriptor or coverage");
    require(whole_image.facets().size() == 1 &&
                same_facets(whole_image.facets()[0], image.facets()[0]),
            "Whole inference did not preserve image facets");
    ps::PlanningOptions cropped = options;
    cropped.output_regions = {{"image", roi}};
    auto compiled = take(compiler.compile(graph, cropped));
    const auto cropped_result = take(execution.execute(compiled.plan, bind));
    const auto& crop = cropped_result.values.at("image");
    require(same_region(crop.region(), roi),
            "cropped output coverage mismatch");
    for (std::uint64_t y = roi.dimensions()[0].offset;
         y < roi.dimensions()[0].offset + roi.dimensions()[0].extent; ++y)
      for (std::uint64_t x = roi.dimensions()[1].offset;
           x < roi.dimensions()[1].offset + roi.dimensions()[1].extent; ++x)
        for (std::uint64_t channel = 0; channel < 4; ++channel) {
          const float a = sample(crop, y, x, channel);
          const float b = sample(whole_image, y, x, channel);
          require(std::memcmp(&a, &b, sizeof(float)) == 0,
                  "Whole crop differs from the complete result");
        }
    // A nonfinite known sample outside the ROI still fails the whole Run.
    auto nan = rgba;
    nan[static_cast<std::size_t>(grid.width * 0 + 0) * 4 + 0] =
        std::numeric_limits<float>::quiet_NaN();
    ps::ExecutionBindings invalid;
    invalid.inputs.push_back({"image", image_value(grid, nan)});
    invalid.inputs.push_back({"hole_mask", holes});
    require(!execution.execute(compiled.plan, invalid).ok(),
            "a distant nonfinite sample must fail global validation");
  }
}

/** @brief T11: padded, offset, negative and zero-stride computed inputs. */
void computed_views(const std::vector<std::string>& keys) {
  const Grid grid{6, 9};
  const auto rgba = gradient_image(grid);
  const auto mask = centered_mask(grid, 3, 3);
  const auto packed_image = image_value(grid, rgba);
  const auto packed_mask = mask_value(grid, mask);
  const std::size_t row_bytes = static_cast<std::size_t>(grid.width) * 16;
  const std::size_t stride = row_bytes + 32;
  // Padded/offset view: spare bytes above and below plus a nonzero offset.
  std::vector<std::uint8_t> padded(
      8 + static_cast<std::size_t>(grid.height) * stride, 0x7F);
  for (std::uint64_t y = 0; y < grid.height; ++y)
    std::memcpy(
        padded.data() + 8 + y * stride,
        reinterpret_cast<const std::uint8_t*>(rgba.data()) + y * row_bytes,
        row_bytes);
  const auto padded_image = take(ps::Value::create(
      packed_image.descriptor(), packed_image.region(),
      {8, {static_cast<std::int64_t>(stride), 16, 4}, {0, 0, 0}}, padded,
      packed_image.facets()));
  // Negative row stride: the same logical image stored bottom row first.
  std::vector<std::uint8_t> reversed(
      64 + static_cast<std::size_t>(grid.height) * row_bytes, 0x5A);
  const std::size_t base = 64 + (grid.height - 1) * row_bytes;
  for (std::uint64_t y = 0; y < grid.height; ++y)
    std::memcpy(
        reversed.data() + base - y * row_bytes,
        reinterpret_cast<const std::uint8_t*>(rgba.data()) + y * row_bytes,
        row_bytes);
  const auto reversed_image = take(ps::Value::create(
      packed_image.descriptor(), packed_image.region(),
      {base, {-static_cast<std::int64_t>(row_bytes), 16, 4}, {0, 0, 0}},
      reversed, packed_image.facets()));
  // Zero-stride view: one logical image row broadcast over three rows, with a
  // matching broadcast mask row that really contains holes.
  const std::uint64_t broadcast_row = 3;
  const Grid broadcast{3, grid.width};
  std::vector<float> row_rgba(static_cast<std::size_t>(grid.width) * 4);
  std::memcpy(row_rgba.data(), rgba.data() + broadcast_row * grid.width * 4,
              row_rgba.size() * sizeof(float));
  std::vector<std::uint8_t> row_pixels(row_rgba.size() * sizeof(float));
  std::memcpy(row_pixels.data(), row_rgba.data(), row_pixels.size());
  const std::vector<std::uint64_t> broadcast_shape{broadcast.height,
                                                   broadcast.width, 4};
  const auto broadcast_image = take(ps::Value::create(
      {ps::ElementType::Float32, broadcast_shape},
      ps::Region::whole(broadcast_shape), {0, {0, 16, 4}, {0, 0, 0}},
      row_pixels, packed_image.facets()));
  std::vector<float> row_mask(mask.begin() + broadcast_row * grid.width,
                              mask.begin() + (broadcast_row + 1) * grid.width);
  std::vector<std::uint8_t> row_mask_bytes(row_mask.size() * sizeof(float));
  std::memcpy(row_mask_bytes.data(), row_mask.data(), row_mask_bytes.size());
  const auto broadcast_mask = take(ps::Value::create(
      {ps::ElementType::Float32, {broadcast.height, broadcast.width}},
      ps::Region::whole({broadcast.height, broadcast.width}),
      {0, {0, 4}, {0, 0}}, row_mask_bytes, packed_mask.facets()));
  // Packed equivalent of the broadcast view for the equivalence check.
  std::vector<float> packed_rows(broadcast.pixels() * 4);
  std::vector<float> packed_row_mask(broadcast.pixels());
  for (std::uint64_t y = 0; y < broadcast.height; ++y) {
    std::memcpy(packed_rows.data() + y * broadcast.width * 4, row_rgba.data(),
                row_rgba.size() * sizeof(float));
    std::memcpy(packed_row_mask.data() + y * broadcast.width, row_mask.data(),
                row_mask.size() * sizeof(float));
  }
  const auto packed_broadcast_image = image_value(broadcast, packed_rows);
  const auto packed_broadcast_mask = mask_value(broadcast, packed_row_mask);
  const auto before_image = packed_image.copy_bytes();
  const auto before_mask = packed_mask.copy_bytes();
  for (const auto& key : keys) {
    const auto plain =
        take(invoke(key, packed_image, packed_mask, radius_parameters(2)));
    const auto padded_result =
        take(invoke(key, padded_image, packed_mask, radius_parameters(2)));
    const auto reversed_result =
        take(invoke(key, reversed_image, packed_mask, radius_parameters(2)));
    require(bit_identical(plain, padded_result) &&
                bit_identical(plain, reversed_result),
            "packed equivalence failed for a padded or negative-stride view");
    const auto broadcast_result = take(
        invoke(key, broadcast_image, broadcast_mask, radius_parameters(2)));
    const auto packed_broadcast_result =
        take(invoke(key, packed_broadcast_image, packed_broadcast_mask,
                    radius_parameters(2)));
    require(broadcast_result.descriptor().shape == broadcast_shape,
            "zero-stride output shape mismatch");
    require(bit_identical(broadcast_result, packed_broadcast_result),
            "zero-stride view is not read as an immutable broadcast");
    require(packed_image.copy_bytes() == before_image &&
                packed_mask.copy_bytes() == before_mask,
            "the profile wrote into an immutable input");
  }
}

/** @brief T12: repetition and caller rounding-mode restoration. */
void determinism(const std::vector<std::string>& keys) {
  const Grid grid{10, 12};
  const auto rgba = checker_image(grid);
  const auto mask = block_mask(grid, 2, 3, 4, 5);
  const auto image = image_value(grid, rgba);
  const auto holes = mask_value(grid, mask);
  for (const auto& key : keys) {
    const auto first = take(invoke(key, image, holes, radius_parameters(3)));
    const auto second = take(invoke(key, image, holes, radius_parameters(3)));
    require(bit_identical(first, second),
            "identical invocations must repeat bitwise");
    const int previous = std::fegetround();
    require(std::fesetround(FE_UPWARD) == 0, "FE_UPWARD is unavailable");
    const auto under_upward =
        take(invoke(key, image, holes, radius_parameters(3)));
    require(std::fegetround() == FE_UPWARD,
            "caller rounding mode was not restored");
    require(std::fesetround(previous) == 0, "rounding mode restore failed");
    require(bit_identical(first, under_upward),
            "computation must use round-to-nearest regardless of the caller");
  }
}

/** @brief T13: signed/HDR values are not clamped and overflow fails. */
void extreme_values(const std::vector<std::string>& keys) {
  const Grid grid{7, 7};
  const auto rgba = constant_image(grid, -1.5F, 2.25F, 8.0F);
  const auto mask = centered_mask(grid, 3, 3);
  for (const auto& key : keys) {
    const auto result =
        take(invoke(key, image_value(grid, rgba), mask_value(grid, mask),
                    radius_parameters(2)));
    for (std::uint64_t y = 0; y < grid.height; ++y)
      for (std::uint64_t x = 0; x < grid.width; ++x) {
        const std::size_t index = static_cast<std::size_t>(y * grid.width + x);
        if (mask[index] == 1.0F)
          continue;
        for (std::uint64_t channel = 0; channel < 3; ++channel) {
          const float got = sample(result, y, x, channel);
          require(
              std::memcmp(&got, &rgba[index * 4 + channel], sizeof(float)) == 0,
              "signed/HDR known samples were modified");
        }
      }
    const float beyond = sample(result, 3, 3, 2);
    require(beyond > 1.0F && std::isfinite(beyond),
            "HDR hole values must stay unclamped and finite");
  }
  // Overflowing arithmetic must fail instead of publishing infinity or NaN.
  // Alternating signed extremes make the pinned gradient difference overflow.
  const Grid narrow{5, 5};
  std::vector<float> extreme(narrow.pixels() * 4);
  for (std::size_t index = 0; index < narrow.pixels(); ++index) {
    const float value = index % 2 == 0 ? std::numeric_limits<float>::max()
                                       : -std::numeric_limits<float>::max();
    extreme[index * 4 + 0] = value;
    extreme[index * 4 + 1] = -value;
    extreme[index * 4 + 2] = value;
    extreme[index * 4 + 3] = 1.0F;
  }
  const auto narrow_mask = centered_mask(narrow, 1, 1);
  for (const auto& key : keys)
    require(!invoke(key, image_value(narrow, extreme),
                    mask_value(narrow, narrow_mask), radius_parameters(1))
                 .ok(),
            "overflowing computation must fail");
}

/** @brief T14: edits to samples, mask, radius and metadata invalidate. */
void invalidation(const std::vector<std::string>& keys) {
  const Grid grid{9, 9};
  const auto rgba = gradient_image(grid);
  const auto mask = centered_mask(grid, 3, 3);
  const auto document_for = [&grid, &mask](const std::vector<float>& fixture,
                                           std::int64_t radius,
                                           const std::string& key) {
    ps::WorkflowDocument document;
    const auto image = image_value(grid, fixture);
    const auto holes = mask_value(grid, mask);
    document.inputs = {{1, "image", image.descriptor(), image.region(),
                        image.layout(), image.facets()},
                       {2, "hole_mask", holes.descriptor(), holes.region(),
                        holes.layout(), holes.facets()}};
    document.nodes = {
        {1,
         key,
         {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
         radius_parameters(radius)}};
    document.outputs = {{"image", 1, "image"}};
    return std::make_pair(document, std::make_pair(image, holes));
  };
  for (const auto& key : keys) {
    auto registry = ps::make_default_operation_registry();
    ps::PlanningOptions options;
    auto base = document_for(rgba, 3, key);
    ps::GraphContext graph(base.first);
    ps::Compiler compiler(registry);
    auto compiled = take(compiler.compile(graph, options));
    ps::ExecutionContext execution(
        registry, {1, false, 8, 1024U * 1024U * 1024U, 1U << 20});
    const auto bind = [](const ps::Value& image, const ps::Value& holes) {
      ps::ExecutionBindings result;
      result.inputs.push_back({"image", image});
      result.inputs.push_back({"hole_mask", holes});
      return result;
    };
    const auto first = take(execution.execute(
        compiled.plan, bind(base.second.first, base.second.second)));
    auto edited = rgba;
    edited[0] = -0.75F;
    auto edited_fixture = document_for(edited, 3, key);
    const auto second = take(execution.execute(
        compiled.plan,
        bind(edited_fixture.second.first, edited_fixture.second.second)));
    require(!bit_identical(first.values.at("image"), second.values.at("image")),
            "changed known samples reused a stale successful result");
    auto nan = rgba;
    nan[static_cast<std::size_t>(8 * grid.width + 8) * 4 + 1] =
        std::numeric_limits<float>::quiet_NaN();
    auto nan_fixture = document_for(nan, 3, key);
    require(!execution
                 .execute(compiled.plan, bind(nan_fixture.second.first,
                                              nan_fixture.second.second))
                 .ok(),
            "a distant NaN must invalidate the whole output");
    const auto other_mask = block_mask(grid, 1, 1, 5, 5);
    const auto third =
        take(invoke(key, image_value(grid, rgba), mask_value(grid, other_mask),
                    radius_parameters(3)));
    require(!bit_identical(first.values.at("image"), third),
            "changed hole mask reused a stale successful result");
    auto larger = document_for(rgba, 4, key);
    ps::GraphContext other_graph(larger.first);
    auto other_plan = take(compiler.compile(other_graph, options));
    require(other_plan.plan.digest().value != compiled.plan.digest().value,
            "radius must participate in the plan identity");
    const auto fourth = take(execution.execute(
        other_plan.plan, bind(larger.second.first, larger.second.second)));
    require(!bit_identical(first.values.at("image"), fourth.values.at("image")),
            "changed radius reused a stale successful result");
    // The typed image port admits either legal reference, so inference accepts
    // a changed association and the callback rejects the exact remaining
    // profile with TypeMismatch instead of reusing a stale successful result.
    auto straight = ps::rgba_semantics();
    straight.association = "straight";
    auto metadata = document_for(rgba, 3, key);
    metadata.first.inputs[0].facets = {take(ps::encode_semantic(straight))};
    ps::GraphContext metadata_graph(metadata.first);
    auto metadata_plan = compiler.compile(metadata_graph, options);
    if (metadata_plan.ok()) {
      const auto invalid = execution.execute(
          metadata_plan.value().plan,
          bind(metadata.second.first, metadata.second.second));
      require(!invalid.ok() && invalid.status().code == ErrorCode::TypeMismatch,
              "changed image metadata was accepted by the callback");
    }
  }
}

/** @brief T15: exact admitted scratch, one byte less, then later success. */
void allocation_limits(const std::vector<std::string>& keys) {
  const Grid grid{13, 17};
  const auto image = image_value(grid, checker_image(grid));
  const auto holes = mask_value(grid, centered_mask(grid, 5, 5));
  for (const auto& key : keys) {
    const std::uint64_t needed =
        16 * grid.pixels() + declared_scratch(key, grid);
    for (const std::uint64_t limit : {needed - 1, needed / 2, needed / 8}) {
      Budget budget;
      budget.limit = limit;
      const auto result = invoke(key, image, holes, radius_parameters(3), {},
                                 budget.allocator());
      require(
          !result.ok() && result.status().code == ErrorCode::ResourceExhausted,
          "an insufficient budget must fail with ResourceExhausted");
      require(budget.live == 0,
              "a failed invocation must release every owned allocation");
    }
    Budget budget;
    budget.limit = needed;
    {
      const auto result = take(invoke(key, image, holes, radius_parameters(3),
                                      {}, budget.allocator()));
      require(budget.peak == needed,
              "admitted scratch differs from the declared profile model");
      require(budget.allocations == declared_allocations(key),
              "unexpected host-owned allocation count");
      require(budget.live == 16 * grid.pixels(),
              "only the published output may stay charged on success");
      const auto reference =
          take(invoke(key, image, holes, radius_parameters(3)));
      require(bit_identical(result, reference),
              "the exact-minimum budget changed the result");
    }
    require(budget.live == 0,
            "retiring the published result must release every owned byte");
  }
}

/** @brief T16: cancellation at each stage, released owners, host priority. */
void cancellation(const std::vector<std::string>& keys) {
  const Grid grid{10, 10};
  const auto image = image_value(grid, gradient_image(grid));
  const auto holes = mask_value(grid, centered_mask(grid, 4, 4));
  for (const auto& key : keys) {
    ps::CancellationSource source;
    source.cancel();
    std::uint64_t live = 0;
    const auto pre = invoke(
        key, image, holes, radius_parameters(3), {},
        ps::BufferAllocator(
            [&live](std::uint64_t size) -> ps::Result<std::shared_ptr<void>> {
              live += size;
              return ps::Result<std::shared_ptr<void>>(std::shared_ptr<void>(
                  new int(0), [&live, size](void* pointer) {
                    delete static_cast<int*>(pointer);
                    live -= size;
                  }));
            }),
        source.token());
    require(!pre.ok() && pre.status().code == ErrorCode::Cancelled,
            "a cancelled token must fail before validation");
    require(live == 0, "pre-entry cancellation allocated host bytes");
    for (std::uint64_t stage = 1; stage <= declared_allocations(key); ++stage) {
      ps::CancellationSource cancellation;
      Budget budget;
      budget.before_allocation = [stage, &cancellation](std::uint64_t index) {
        if (index >= stage)
          cancellation.cancel();
      };
      const auto result = invoke(key, image, holes, radius_parameters(3), {},
                                 budget.allocator(), cancellation.token());
      require(!result.ok(), "cancellation must not publish a partial result");
      require(result.status().code == ErrorCode::Cancelled,
              "cancellation must keep the host error priority");
      require(budget.live == 0,
              "cancelled invocations must release every owned allocation");
      require(budget.allocations <= stage,
              "cancellation must stop before the next allocation");
    }
  }
  // Frontier cancellation: request a stop while the solver runs.
  const Grid large{192, 192};
  const auto large_image = image_value(large, checker_image(large));
  const auto large_holes =
      mask_value(large, block_mask(large, 24, 24, 144, 144));
  for (const auto& key : keys) {
    ps::CancellationSource cancellation;
    Budget budget;
    std::thread trigger([&cancellation] {
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      cancellation.cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        invoke(key, large_image, large_holes, radius_parameters(32), {},
               budget.allocator(), cancellation.token());
    trigger.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(!result.ok() && result.status().code == ErrorCode::Cancelled,
            "mid-frontier cancellation must not publish a result");
    require(budget.live == 0,
            "mid-frontier cancellation must release every owner");
    require(elapsed < std::chrono::seconds(5),
            "mid-frontier cancellation did not stop the solver promptly");
  }
  // Host priority: an unusable plan fails Stale before cancellation is read.
  auto registry = ps::make_default_operation_registry();
  ps::ExecutionContext execution(registry, {1, false, 4, 1U << 20});
  ps::CancellationSource cancelled;
  cancelled.cancel();
  const auto stale =
      execution.execute(ps::ExecutionPlan{}, {}, cancelled.token());
  require(!stale.ok() && stale.status().code == ErrorCode::Stale,
          "a default plan must fail Stale before cancellation is observed");
}

/** @brief T17: public registry -> compile -> bind -> execute -> named image. */
void public_workflow(const std::vector<std::string>& keys) {
  const Grid grid{8, 8};
  const auto rgba = constant_image(grid, 0.25F, 0.5F, 0.75F);
  const auto mask = centered_mask(grid, 2, 2);
  const auto image = image_value(grid, rgba);
  const auto holes = mask_value(grid, mask);
  for (const auto& key : keys) {
    ps::WorkflowDocument document;
    document.inputs = {{1, "image", image.descriptor(), image.region(),
                        image.layout(), image.facets()},
                       {2, "hole_mask", holes.descriptor(), holes.region(),
                        holes.layout(), holes.facets()}};
    document.nodes = {
        {7,
         key,
         {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
         radius_parameters(3)}};
    document.outputs = {{"image", 7, "image"}};
    auto registry = ps::make_default_operation_registry();
    ps::GraphContext graph(document);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContext execution(registry, {2, false, 8, 1U << 30});
    ps::ExecutionBindings bindings;
    bindings.inputs.push_back({"image", image});
    bindings.inputs.push_back({"hole_mask", holes});
    const auto result = take(execution.execute(compiled.plan, bindings));
    require(result.values.size() == 1U, "named output inventory");
    const auto& named = result.values.at("image");
    require(named.descriptor().shape ==
                std::vector<std::uint64_t>{grid.height, grid.width, 4},
            "named output shape");
    require(named.facets().size() == 1 &&
                same_facets(named.facets()[0], image.facets()[0]),
            "named output facets");
    for (std::uint64_t y = 0; y < grid.height; ++y)
      for (std::uint64_t x = 0; x < grid.width; ++x) {
        const std::size_t index = static_cast<std::size_t>(y * grid.width + x);
        const bool hole = mask[index] == 1.0F;
        for (std::uint64_t channel = 0; channel < 4; ++channel) {
          const float got = sample(named, y, x, channel);
          const float want = rgba[index * 4 + channel];
          if (hole && channel < 3)
            require(std::fabs(static_cast<double>(got) -
                              static_cast<double>(want)) <= 1e-6,
                    "public workflow hole value outside tolerance");
          else
            require(std::memcmp(&got, &want, sizeof(float)) == 0,
                    "public workflow changed a preserved sample");
        }
      }
    require(
        result.diagnostics.operation_timings.size() == 1U &&
            result.diagnostics.operation_timings[0].backend == ps::Backend::Cpu,
        "public workflow backend evidence");
  }
}

/**
 * @brief Deterministic fixture generator of the common comparison harness.
 * @note Reproduces the independent harness content and hole patterns exactly so
 * a reported mismatch stays reproducible in-tree.
 */
struct HarnessFixture final {
  Grid grid;
  std::vector<float> rgba;
  std::vector<float> mask;
};

HarnessFixture harness_fixture(std::uint64_t height, std::uint64_t width,
                               const std::string& pattern) {
  HarnessFixture fixture;
  fixture.grid = Grid{height, width};
  fixture.rgba.assign(fixture.grid.pixels() * 4, 0.0F);
  fixture.mask.assign(fixture.grid.pixels(), 0.0F);
  std::uint32_t rng = 0x12345678U;
  for (std::uint64_t y = 0; y < height; ++y)
    for (std::uint64_t x = 0; x < width; ++x) {
      const std::size_t p = static_cast<std::size_t>(y * width + x);
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      fixture.rgba[p * 4 + 0] =
          static_cast<float>(
              static_cast<std::int64_t>((x * 13 + y * 7) % 1024) - 256) /
          128.0F;
      fixture.rgba[p * 4 + 1] =
          static_cast<float>((x * 3 + y * 19) % 512) / 256.0F;
      fixture.rgba[p * 4 + 2] = ((x / 3 + y / 5) % 2) != 0 ? 1.5F : -0.5F;
      fixture.rgba[p * 4 + 3] = 1.0F;
      bool hole = false;
      if (pattern == "block")
        hole = y >= height / 4 && y < 3 * height / 4 && x >= width / 4 &&
               x < 3 * width / 4;
      if (pattern == "center")
        hole = y == height / 2 && x == width / 2;
      if (pattern == "sparse")
        hole = rng % 100 == 0;
      if (pattern == "edge")
        hole = (y < 2 && x < width / 2) || (x > width - 3 && y > height / 2) ||
               (x < 2 && y > height - 3);
      if (pattern == "scratch")
        hole = (x == width / 3 || x == 2 * width / 3 || y == height / 2) &&
               (x + y) % 7 != 0;
      if (pattern == "oneknown")
        hole = !(y == 0 && x == 0);
      if (pattern == "random")
        hole = rng % 4 == 0;
      fixture.mask[p] = hole ? 1.0F : 0.0F;
    }
  return fixture;
}

/**
 * @brief Independent-harness fixtures: block holes, extents, profile, overflow.
 * @note The 17x23 block fixture is the regression the independent review
 * reported at radius 3, 8 and 32; it is compared against the pinned oracle with
 * the frozen tolerance, never a relaxed one.
 */
void common_harness_fixtures(const std::vector<std::string>& keys) {
  const auto block = harness_fixture(17, 23, "block");
  const auto block_image = image_value(block.grid, block.rgba);
  const auto block_holes = mask_value(block.grid, block.mask);
  for (const auto radius :
       {std::int64_t{1}, std::int64_t{3}, std::int64_t{8}, std::int64_t{32}})
    for (const auto& key : keys)
      compare_oracle(block.grid, block.rgba, block.mask, radius,
                     take(invoke(key, block_image, block_holes,
                                 radius_parameters(radius))),
                     key, "block r" + std::to_string(radius));
  for (const char* pattern :
       {"center", "sparse", "edge", "scratch", "oneknown", "random"}) {
    const auto fixture = harness_fixture(17, 23, pattern);
    const auto image = image_value(fixture.grid, fixture.rgba);
    const auto holes = mask_value(fixture.grid, fixture.mask);
    for (const auto radius : {std::int64_t{1}, std::int64_t{8}}) {
      for (const auto& key : keys)
        compare_oracle(
            fixture.grid, fixture.rgba, fixture.mask, radius,
            take(invoke(key, image, holes, radius_parameters(radius))), key,
            std::string(pattern) + " r" + std::to_string(radius));
    }
  }
  // Extents above the profile bound are rejected on either axis.
  for (const bool wide : {false, true}) {
    const auto thin =
        harness_fixture(wide ? 3 : 32769, wide ? 32769 : 3, "zero");
    const auto image = image_value(thin.grid, thin.rgba);
    const auto holes = mask_value(thin.grid, thin.mask);
    for (const auto& key : keys)
      require(!invoke(key, image, holes, radius_parameters(3)).ok(),
              "extent above 32768 accepted");
  }
  // A display-reference image keeps its exact facet and still matches.
  {
    const auto fixture = harness_fixture(5, 5, "center");
    auto display = ps::rgba_semantics();
    display.reference = "display";
    const auto facets =
        std::vector<ps::ValueFacet>{take(ps::encode_semantic(display))};
    const auto descriptor =
        image_value(fixture.grid, fixture.rgba).descriptor();
    const auto bytes = image_value(fixture.grid, fixture.rgba).copy_bytes();
    const auto display_image = take(ps::Value::create(
        descriptor, ps::Region::whole(descriptor.shape),
        {0,
         {static_cast<std::int64_t>(fixture.grid.width * 16), 16, 4},
         {0, 0, 0}},
        bytes, facets));
    const auto holes = mask_value(fixture.grid, fixture.mask);
    for (const auto& key : keys) {
      const auto output =
          take(invoke(key, display_image, holes, radius_parameters(3)));
      require(output.facets().size() == facets.size() &&
                  same_facets(output.facets()[0], facets[0]),
              "display reference facets changed");
      compare_oracle(fixture.grid, fixture.rgba, fixture.mask, 3, output, key,
                     "display reference");
    }
    // Scene and display differ only in that field; both stay accepted.
    for (const auto& key : keys)
      require(invoke(key, display_image, holes, radius_parameters(3)).ok(),
              "display reference image was rejected");
  }
  // Nonfinite intermediate arithmetic fails even when the published hole value
  // would be finite: alternating signed extremes overflow the gradient square.
  {
    auto fixture = harness_fixture(5, 5, "center");
    for (std::size_t p = 0; p < fixture.mask.size(); ++p)
      for (std::uint64_t c = 0; c < 3; ++c)
        fixture.rgba[p * 4 + c] = (p % 2) != 0 ? 1e30F : -1e30F;
    const auto image = image_value(fixture.grid, fixture.rgba);
    const auto holes = mask_value(fixture.grid, fixture.mask);
    for (const auto& key : keys)
      rejected(key, image, holes, radius_parameters(3),
               ErrorCode::OperationFailed, "nonfinite intermediate");
  }
  // A rejected arithmetic exception never leaks into the caller state, and a
  // clean invocation leaves no arithmetic flag behind.
  {
    const auto fixture = harness_fixture(9, 9, "random");
    const auto image = image_value(fixture.grid, fixture.rgba);
    const auto holes = mask_value(fixture.grid, fixture.mask);
    for (const auto& key : keys) {
      std::feclearexcept(FE_ALL_EXCEPT);
      require(invoke(key, image, holes, radius_parameters(3)).ok(),
              "clean fixture failed");
      require(std::fetestexcept(FE_INVALID | FE_OVERFLOW | FE_DIVBYZERO) == 0,
              "arithmetic exceptions leaked to the caller");
      volatile float large = 1e30F;
      volatile float product = large * large;
      require(product > 0 && std::fetestexcept(FE_OVERFLOW) != 0,
              "fixture did not raise a caller exception flag");
      const auto result = invoke(key, image, holes, radius_parameters(3));
      require(result.ok() && std::fetestexcept(FE_OVERFLOW) != 0,
              "caller exception state was not restored");
      std::feclearexcept(FE_ALL_EXCEPT);
    }
  }
}

/** @brief T18: independent concurrent invocations share no call state. */
void concurrent_invocations(const std::vector<std::string>& keys) {
  const Grid grid{16, 16};
  const auto rgba = checker_image(grid);
  const auto mask = block_mask(grid, 3, 3, 6, 7);
  const auto image = image_value(grid, rgba);
  const auto holes = mask_value(grid, mask);
  auto registry = ps::make_default_operation_registry();
  for (const auto& key : keys) {
    const std::vector<ps::Value> inputs{image, holes};
    const std::vector<ps::Region> demands{image.region(), holes.region()};
    const auto expected =
        take(registry->invoke(key, {inputs, demands, radius_parameters(3)}));
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
      workers.emplace_back([&] {
        try {
          for (int repetition = 0; repetition < 6; ++repetition) {
            auto concurrent = take(
                registry->invoke(key, {inputs, demands, radius_parameters(3)}));
            if (!bit_identical(concurrent, expected))
              failed = true;
          }
        } catch (const std::exception&) {
          failed = true;
        }
      });
    for (auto& worker : workers)
      worker.join();
    require(!failed, "concurrent invocations interfered with each other");
  }
}
}  // namespace

int main() {
  try {
    auto registry = ps::make_default_operation_registry();
    const auto keys = selected_keys(registry);
    registration_and_schema();
    zero_mask_identity(keys);
    placeholder_independence(keys);
    analytic_constant(keys);
    pinned_oracle(keys);
    full_mask(keys);
    invalid_inputs(keys);
    extent_bounds(keys);
    region_and_tiles(keys);
    computed_views(keys);
    determinism(keys);
    extreme_values(keys);
    invalidation(keys);
    allocation_limits(keys);
    cancellation(keys);
    public_workflow(keys);
    common_harness_fixtures(keys);
    concurrent_invocations(keys);
    std::cout << "PNT-05A variants:";
    for (const auto& key : keys)
      std::cout << ' ' << key;
#ifdef PS_INPAINT_NAVIER_STOKES_ORACLE
    for (const auto& entry : oracle_stats)
      std::cout << "\n"
                << entry.first
                << " vs pinned oracle 4.12.0: samples=" << entry.second.samples
                << " bit_differences=" << entry.second.bit_differences
                << " above_half_tolerance=" << entry.second.above_half_tolerance
                << " worst_absolute=" << entry.second.worst_absolute
                << " worst_tolerance_fraction="
                << entry.second.worst_tolerance_fraction;
#else
    std::cout << "\npinned oracle: unavailable in this build";
#endif
    std::cout << "\ncross-variant bit differences: " << variant_bit_differences
              << " (worst fraction of the profile tolerance "
              << variant_worst_relative << ")\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "local inpaint navier stokes test failed: " << error.what()
              << '\n';
    return 1;
  }
}
