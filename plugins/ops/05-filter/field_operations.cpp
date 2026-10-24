#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/fields.hpp"

namespace ps::plugin_internal::filter {
namespace {
bool gradient(FieldKind k) {
  return k == FieldKind::Central || k == FieldKind::Sobel ||
         k == FieldKind::Scharr;
}
bool window(FieldKind k) {
  return k == FieldKind::Structure || k == FieldKind::Moments ||
         k == FieldKind::Covariance;
}
SchemaTemplate schema_like(const OperationMetadata& in, ElementType type) {
  auto s = *in.result_schema;
  s.id = "photospider.tensor";
  s.version = 1;
  s.tensors[0].key = "samples";
  s.tensors[0].descriptor.element_type = type;
  check(s.validate());
  return s;
}
std::uint64_t extent(std::int64_t radius) {
  parameter(radius >= 0 && radius <= (INT64_C(1) << 39) - 1,
            "filter radius extent overflow");
  return 2 * static_cast<std::uint64_t>(radius) + 1;
}
}  // namespace

bool Fields::pointwise() const {
  return kind == FieldKind::Magnitude || kind == FieldKind::Orientation ||
         kind == FieldKind::Eigen || kind == FieldKind::Detail;
}
bool Fields::identity() const {
  return (kind == FieldKind::Unsharp || kind == FieldKind::Detail) && gain == 0;
}

Fields::Fields(FieldKind k, Profile p, const std::vector<OperationMetadata>& in,
               const Parameters& args)
    : kind(k), profile(p) {
  check(numeric_ops::sequence_profile_available(p));
  if (kind == FieldKind::LogKernel || kind == FieldKind::LogFilter) {
    sigma = real(args, "sigma");
    const auto r = integer(args, "radius");
    parameter(r >= 1, "LoG radius must be positive");
    radius = r;
    const auto side = extent(r);
    size_product({side, side});
    normalized = mask::boolean(args, "scale_normalized");
    if (kind == FieldKind::LogKernel) {
      add_output(this, tensor_schema(ElementType::Float64, {side, side}));
      add_output(this, tensor_schema(ElementType::Float64, {1}));
      return;
    }
  }
  const auto& input = tensor(in.at(0), true);
  type = input.descriptor.element_type;
  require(floating(type) && input.sample_shape().size() >= 2,
          "filter fields require Float32/64 rank 2..8");
  for (std::size_t i = 1; i < in.size(); ++i) {
    const auto& other = tensor(in[i], true);
    require(other.sample_shape() == input.sample_shape() &&
                other.descriptor.element_type == type,
            "filter fields require matching input shapes and dtypes");
  }
  mode = text(args, gradient(kind)                 ? "normalization"
                    : kind == FieldKind::Magnitude ? "norm"
                                                   : "unit");
  if (gradient(kind)) {
    parameter(mode == "unit_ramp" || mode == "raw", "gradient normalization");
    dx = real(args, "spacing_x");
    dy = real(args, "spacing_y");
  } else if (kind == FieldKind::Laplacian8) {
    dx = dy = real(args, "s");
  } else {
    dx = real(args, "dx", 1);
    dy = real(args, "dy", 1);
  }
  if (kind == FieldKind::Magnitude)
    parameter(mode == "l1" || mode == "l2", "gradient norm");
  if (kind == FieldKind::Orientation)
    parameter(mode == "radian" || mode == "turn", "gradient angle unit");
  scale = real(args, "scale", 1);
  gain = real(args, kind == FieldKind::Unsharp ? "amount" : "gain", 1);
  threshold = real(args, "threshold");
  ddof = static_cast<std::uint64_t>(integer(args, "ddof"));
  if (!pointwise()) {
    auto gp = args;
    std::uint64_t kh = 3, kw = 3;
    if (window(kind)) {
      const auto h = integer(args, "height"), w = integer(args, "width");
      parameter(h > 0 && w > 0, "positive filter box extents required");
      kh = static_cast<std::uint64_t>(h);
      kw = static_cast<std::uint64_t>(w);
      // Bounds the declared temporary storage, not the numerical domain.
      bytes(static_cast<unsigned __int128>(kh) * kw * sizeof(double));
    } else {
      if (kind == FieldKind::LogFilter)
        kh = kw = extent(radius);
      gp["anchor_y"] = static_cast<std::int64_t>(kh / 2);
      gp["anchor_x"] = static_cast<std::int64_t>(kw / 2);
    }
    if (kind != FieldKind::LogFilter && kind != FieldKind::Unsharp &&
        kind != FieldKind::Moments && kind != FieldKind::Covariance) {
      gp["boundary"] = std::string("reflect_half");
    }
    if (kind != FieldKind::LogFilter && kind != FieldKind::Unsharp)
      gp["cval"] = 0.0;
    geometry.emplace(input, gp, kh, kw,
                     kind == FieldKind::Moments || kind == FieldKind::Covariance
                         ? Kind::BoxMean
                         : Kind::BoxSum);
    if (kind == FieldKind::Unsharp || kind == FieldKind::Difference) {
      auto gaussian = args;
      gaussian["boundary"] = kind == FieldKind::Difference
                                 ? std::string("reflect_half")
                                 : text(args, "boundary");
      gaussian["cval"] =
          kind == FieldKind::Difference ? 0.0 : real(args, "cval");
      if (kind == FieldKind::Difference) {
        gaussian["sigma_x"] = real(args, "sigma_small");
        gaussian["sigma_y"] = real(args, "sigma_small");
        gaussian["radius_x"] = integer(args, "radius_small");
        gaussian["radius_y"] = integer(args, "radius_small");
      }
      // Both profiles retain strict stage rounding and exact threshold control.
      small = std::make_shared<Filter>(Kind::Gaussian, Profile::Strict, in,
                                       gaussian);
      small->inputs = in;
      if (kind == FieldKind::Difference) {
        FloatingEnvironment environment;
        require(environment.active(),
                "filter rounding environment unavailable");
        const double large_sigma =
            real(args, "sigma_small") * real(args, "ratio");
        gaussian["sigma_x"] = large_sigma;
        gaussian["sigma_y"] = large_sigma;
        gaussian["radius_x"] = integer(args, "radius_large");
        gaussian["radius_y"] = integer(args, "radius_large");
        large = std::make_shared<Filter>(Kind::Gaussian, Profile::Strict, in,
                                         gaussian);
        large->inputs = in;
      }
    }
  }
  unsigned count = gradient(kind) || kind == FieldKind::Orientation ||
                           kind == FieldKind::Moments
                       ? 2
                   : kind == FieldKind::Hessian || kind == FieldKind::Structure
                       ? 3
                   : kind == FieldKind::Eigen ? 4
                                              : 1;
  for (unsigned i = 0; i < count; ++i) {
    const bool valid = (kind == FieldKind::Orientation && i == 1) ||
                       (kind == FieldKind::Eigen && i == 3);
    add_output(this, schema_like(in[0], valid ? ElementType::UInt8 : type));
  }
}

std::vector<std::pair<int, int>> stencil_offsets(FieldKind kind,
                                                 unsigned port) {
  if (gradient(kind)) {
    std::vector<std::pair<int, int>> taps;
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x)
        if ((port == 0 ? x != 0 : y != 0) &&
            (kind != FieldKind::Central || (port == 0 ? y == 0 : x == 0)))
          taps.emplace_back(y, x);
    return taps;
  }
  if (kind == FieldKind::Hessian) {
    if (port == 0)
      return {{0, 1}, {0, 0}, {0, -1}};
    if (port == 2)
      return {{1, 0}, {0, 0}, {-1, 0}};
    return {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
  }
  if (kind == FieldKind::Laplacian4)
    return {{0, 1}, {0, -1}, {0, 0}, {-1, 0}, {1, 0}};
  return {{-1, 0},  {1, 0}, {0, 1},  {0, -1}, {-1, 1},
          {-1, -1}, {1, 1}, {1, -1}, {0, 0}};
}

std::vector<ReadRegion> Fields::support(unsigned port, const Region& q) const {
  if (kind == FieldKind::LogKernel)
    return {};
  if (identity())
    return {{0, 0, q}};
  if (pointwise()) {
    std::vector<ReadRegion> reads;
    for (unsigned i = 0; i < inputs.size(); ++i)
      reads.push_back({i, 0, q});
    return reads;
  }
  if (small) {
    auto result = small->support(0, q);
    if (large) {
      const auto more = large->support(0, q);
      result.insert(result.end(), more.begin(), more.end());
    }
    if (kind == FieldKind::Unsharp)
      result.push_back({0, 0, q});
    return result;
  }
  std::vector<ReadRegion> reads;
  if (window(kind) || kind == FieldKind::LogFilter) {
    for (unsigned i = 0; i < inputs.size(); ++i) {
      if (kind == FieldKind::Structure &&
          ((port == 0 && i == 1) || (port == 2 && i == 0)))
        continue;
      for (const auto& r : geometry->support(q))
        reads.push_back({i, 0, r});
    }
  } else {
    for (const auto& tap : stencil_offsets(kind, port)) {
      auto g = *geometry;
      g.kh = g.kw = 1;
      g.ay = g.ax = 0;
      g.oy = tap.first;
      g.ox = tap.second;
      for (const auto& r : g.support(q))
        reads.push_back({0, 0, r});
    }
  }
  return reads;
}

std::uint64_t Fields::staging_scratch(const Region& q) const {
  std::uint64_t result = sizeof(generation::Math) + 128;
  if (small)
    result = std::max(result, small->task_scratch(0, q));
  if (large)
    result = std::max(result, large->task_scratch(0, q));
  return bytes((static_cast<unsigned __int128>(result) + 63) / 64 * 64);
}
std::uint64_t Fields::task_scratch(unsigned, const Region& q) const {
  if (identity())
    return 0;
  if (small) {
    const auto count = take(q.element_count());
    return bytes(staging_scratch(q) + static_cast<unsigned __int128>(count) *
                                          generation::width(type) *
                                          (large ? 2 : 1));
  }
  if (kind == FieldKind::LogKernel || kind == FieldKind::LogFilter) {
    const auto side = 2 * radius + 1;
    return bytes(sizeof(generation::Math) +
                 static_cast<unsigned __int128>(side) * side * sizeof(double) +
                 128);
  }
  if (window_rows()) {
    const auto h = q.dimensions()[geometry->ya].extent;
    const auto w = q.dimensions()[geometry->xa].extent;
    const unsigned __int128 rows = h + geometry->kh - 1;
    const unsigned __int128 columns = w + geometry->kw - 1;
    return bytes(sizeof(generation::Math) + 128 +
                 (rows * w + columns) * 3 * sizeof(Fast));
  }
  return sizeof(generation::Math) + 128;
}
void Fields::run(const Kernel& kernel,
                 const ps_kernel_invocation_v8& inv) const {
  const Work work{inv, work_limit};
  if (identity()) {
    const Fuel fuel{work};
    const Reader reader{kernel, inv};
    samples(inv.outputs[0], [&](const Coordinate& at, std::uint64_t) {
      fuel.poll();
      store(inv.outputs[0], at, reader.raw(0, 0, at));
    });
    fuel.flush();
  } else if (small) {
    run_staged(*this, kernel, inv, work);
  } else if (pointwise()) {
    run_pointwise(*this, kernel, inv, work);
  } else if (kind == FieldKind::LogKernel || kind == FieldKind::LogFilter) {
    run_log(*this, kernel, inv, work);
  } else {
    run_stencil(*this, kernel, inv, work);
  }
}

OperationDefinition make_field(const std::string& key, FieldKind kind,
                               Profile profile) {
  std::vector<Parameter> parameters;
  std::vector<std::string> names{"output"};
  unsigned ports = 1;
  const auto add = [&](const char* name, Type type) {
    parameters.push_back({name, type});
  };
  const bool point = kind == FieldKind::Magnitude ||
                     kind == FieldKind::Orientation ||
                     kind == FieldKind::Eigen || kind == FieldKind::Detail;
  if (!point && kind != FieldKind::LogKernel) {
    add("y_axis", Type::Int64);
    add("x_axis", Type::Int64);
  }
  if (gradient(kind)) {
    add("spacing_x", Type::Float64);
    add("spacing_y", Type::Float64);
    add("normalization", Type::String);
    names = {"gx", "gy"};
  }
  if (kind == FieldKind::Laplacian4 || kind == FieldKind::Hessian) {
    add("dx", Type::Float64);
    add("dy", Type::Float64);
  }
  if (kind == FieldKind::Laplacian8)
    add("s", Type::Float64);
  if (kind == FieldKind::Laplacian4 || kind == FieldKind::Laplacian8) {
    add("scale", Type::Float64);
    names = {"laplacian"};
  }
  if (kind == FieldKind::Magnitude) {
    ports = 2;
    names = {"magnitude"};
    add("norm", Type::String);
  }
  if (kind == FieldKind::Orientation) {
    ports = 2;
    names = {"angle", "valid"};
    add("unit", Type::String);
  }
  if (kind == FieldKind::Eigen) {
    ports = 3;
    names = {"lambda_max", "lambda_min", "angle", "orientation_valid"};
  }
  if (kind == FieldKind::Hessian)
    names = {"hxx", "hxy", "hyy"};
  if (window(kind)) {
    add("height", Type::Int64);
    add("width", Type::Int64);
    add("anchor_y", Type::Int64);
    add("anchor_x", Type::Int64);
    if (kind == FieldKind::Structure) {
      ports = 2;
      names = {"jxx", "jxy", "jyy"};
    } else {
      add("boundary", Type::String);
      add("ddof", Type::Int64);
      ports = kind == FieldKind::Covariance ? 2 : 1;
      names = kind == FieldKind::Covariance
                  ? std::vector<std::string>{"covariance"}
                  : std::vector<std::string>{"mean", "variance"};
    }
  }
  if (kind == FieldKind::LogKernel || kind == FieldKind::LogFilter) {
    add("sigma", Type::Float64);
    add("radius", Type::Int64);
    add("scale_normalized", Type::Bool);
    if (kind == FieldKind::LogKernel) {
      ports = 0;
      names = {"kernel", "dc_correction"};
    } else {
      add("boundary", Type::String);
      add("cval", Type::Float64);
    }
  }
  if (kind == FieldKind::Difference) {
    add("sigma_small", Type::Float64);
    add("ratio", Type::Float64);
    add("radius_small", Type::Int64);
    add("radius_large", Type::Int64);
    add("gain", Type::Float64);
  }
  if (kind == FieldKind::Unsharp) {
    add("sigma_x", Type::Float64);
    add("sigma_y", Type::Float64);
    add("radius_x", Type::Int64);
    add("radius_y", Type::Int64);
    add("boundary", Type::String);
    add("cval", Type::Float64);
    add("amount", Type::Float64);
    add("threshold", Type::Float64);
  }
  if (kind == FieldKind::Detail) {
    ports = 2;
    add("gain", Type::Float64);
  }
  std::vector<std::pair<std::string, SchemaTemplate>> outputs;
  for (const auto& name : names) {
    const bool valid = name == "valid" || name == "orientation_valid";
    outputs.emplace_back(
        name, tensor_schema(valid ? ElementType::UInt8 : ElementType::Float64,
                            {1, 1}));
  }
  return generation::operation(key, ports, std::move(parameters), outputs,
                               [kind, profile](const auto& in, const auto& p) {
                                 return std::make_shared<Fields>(kind, profile,
                                                                 in, p);
                               });
}
}  // namespace ps::plugin_internal::filter
