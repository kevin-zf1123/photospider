#include <algorithm>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/advanced.hpp"

namespace ps::plugin_internal::filter {
SchemaTemplate field_schema(const OperationMetadata& in, ElementType type) {
  auto s = *in.result_schema;
  s.id = "photospider.tensor";
  s.version = 1;
  s.tensors[0].key = "samples";
  s.tensors[0].descriptor.element_type = type;
  check(s.validate());
  return s;
}
std::uint64_t radius_extent(std::int64_t r) {
  parameter(r >= 0 && r <= (INT64_C(1) << 39) - 1, "filter radius extent");
  return 2 * static_cast<std::uint64_t>(r) + 1;
}
std::vector<double> real_list(const std::string& value) {
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  std::vector<double> result;
  double v;
  while (input >> v) {
    result.push_back(v);
    input >> std::ws;
    if (input.eof())
      break;
    char comma;
    parameter(static_cast<bool>(input.get(comma)) && comma == ',',
              "expected real list");
  }
  parameter(input.eof() && !result.empty() && value.back() != ',',
            "invalid real list");
  return result;
}
namespace {
bool ranked(LocalKind k) {
  return k == LocalKind::Median || k == LocalKind::Percentile;
}
bool regression(LocalKind k) {
  return k == LocalKind::Guided || k == LocalKind::Vector;
}
std::uint64_t extent(LocalKind k, const std::vector<OperationMetadata>& in,
                     const Parameters& p, bool y) {
  if (!ranked(k))
    return radius_extent(integer(p, y ? "radius_y" : "radius_x"));
  const auto& t = tensor(in.at(1));
  require(t.descriptor.element_type == ElementType::UInt8 &&
              t.sample_shape().size() == 2,
          "rank footprint requires UInt8[H,W]");
  return t.sample_shape()[y ? 0 : 1];
}
Parameters geometry_parameters(LocalKind k, const Parameters& p) {
  auto result = p;
  if (!ranked(k)) {
    result["anchor_y"] = integer(p, "radius_y");
    result["anchor_x"] = integer(p, "radius_x");
    if (regression(k))
      result["boundary"] = std::string("truncate");
    result["cval"] = 0.0;
  }
  return result;
}
}  // namespace
bool LocalFilter::rank_filter() const {
  return ranked(kind);
}
bool LocalFilter::guided() const {
  return regression(kind);
}
bool LocalFilter::identity() const {
  return !rank_filter() && geometry.kh == 1 && geometry.kw == 1;
}
Coordinate LocalFilter::guide_position(Coordinate at, unsigned c) const {
  if (component_axis >= 0)
    at[component_axis] = c;
  return at;
}
LocalFilter::LocalFilter(LocalKind k, Profile prof,
                         const std::vector<OperationMetadata>& in,
                         const Parameters& p)
    : kind(k),
      profile(prof),
      type(tensor(in.at(0), true).descriptor.element_type),
      geometry(tensor(in.at(0), true), geometry_parameters(k, p),
               extent(k, in, p, true), extent(k, in, p, false), Kind::BoxMean) {
  check(numeric_ops::sequence_profile_available(profile));
  size_product({geometry.kh, geometry.kw});
  if (rank_filter()) {
    policy = text(p, k == LocalKind::Median ? "even" : "interpolation");
    parameter(k == LocalKind::Median
                  ? policy == "lower" || policy == "upper" || policy == "mean"
                  : policy == "lower" || policy == "higher" ||
                        policy == "nearest_even" || policy == "midpoint" ||
                        policy == "linear",
              "rank interpolation");
    empty = text(p, "empty");
    parameter(empty == "copy_center" || empty == "error", "rank empty policy");
    quantile = k == LocalKind::Median ? .5 : real(p, "q");
  } else {
    sy = real(p, "sigma_space_y", 1);
    sx = real(p, "sigma_space_x", 1);
    sr = real(p, "sigma_range", 1);
    epsilon = real(p, "epsilon", 1);
    if (!guided())
      parameter(geometry.boundary != Boundary::Constant,
                "bilateral boundary excludes constant");
    if (k != LocalKind::Bilateral) {
      const auto& guide = tensor(in.at(1), true);
      require(guide.descriptor.element_type == type &&
                  guide.sample_shape().size() == geometry.dims.size(),
              "guide dtype/rank mismatch");
      auto dims = guide.sample_shape();
      const auto a = integer(p, "component_axis", -1);
      parameter(
          a >= -1 && (a < 0 || (static_cast<std::uint64_t>(a) < dims.size() &&
                                a != geometry.ya && a != geometry.xa)),
          "guide component axis");
      component_axis = static_cast<int>(a);
      if (component_axis >= 0) {
        components = static_cast<unsigned>(dims[component_axis]);
        require(dims[component_axis] >= 1 && dims[component_axis] <= 4,
                "guide component extent must be 1..4");
        dims[component_axis] = geometry.dims[component_axis];
      }
      require(dims == geometry.dims, "guide spatial/batch shape mismatch");
      require(k != LocalKind::Guided || components == 1,
              "scalar guide needs one component");
      require(k != LocalKind::Vector || component_axis >= 0,
              "vector guide needs an explicit component axis");
    }
    if (k == LocalKind::Guided) {
      scales[0] = real(p, "guide_metric_scale");
    } else {
      const auto list =
          real_list(text(p, guided() ? "guide_metric_scale" : "metric_scale"));
      require(list.size() == components, "guide metric scale length");
      std::copy(list.begin(), list.end(), scales.begin());
    }
  }
  add_output(this, field_schema(in[0], type));
}
std::vector<ReadRegion> LocalFilter::support(unsigned, const Region& q) const {
  if (identity())
    return {{0, 0, q}};
  auto g = geometry;
  if (guided()) {
    g.kh = 2 * g.kh - 1;
    g.kw = 2 * g.kw - 1;
    g.ay *= 2;
    g.ax *= 2;
  }
  std::vector<ReadRegion> reads;
  for (const auto& r : g.support(q)) {
    reads.push_back({0, 0, r});
    if (!rank_filter() && kind != LocalKind::Bilateral) {
      auto d = r.dimensions();
      if (component_axis >= 0)
        d[component_axis] = {0, components};
      reads.push_back({1, 0, Region(std::move(d))});
    }
  }
  if (rank_filter()) {
    reads.push_back({1, 0, Region::whole(tensor(inputs[1]).sample_shape())});
    if (empty == "copy_center")
      reads.push_back({0, 0, q});
  }
  return reads;
}
std::uint64_t LocalFilter::task_scratch(unsigned, const Region& q) const {
  if (identity())
    return 0;
  const unsigned __int128 area =
      static_cast<unsigned __int128>(geometry.kh) * geometry.kw;
  if (rank_filter())
    return bytes(sizeof(generation::Math) + area * 40 + 256);
  if (!guided())
    return bytes(sizeof(generation::Math) + area * 32 +
                 kBilateralRangeCacheEntries * 80 + 256);
  const auto region = geometry.support(q).at(0);
  const unsigned __int128 count =
      static_cast<unsigned __int128>(region.dimensions()[geometry.ya].extent) *
      region.dimensions()[geometry.xa].extent;
  return bytes(
      sizeof(generation::Math) + sizeof(Rational) * 96 + 96 * 32 +
      count * (components + 1) * sizeof(double) +
      static_cast<unsigned __int128>(region.dimensions()[geometry.xa].extent) *
          (components + 1) * 24 +
      256);
}
void LocalFilter::run(const Kernel& kernel,
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
  } else if (rank_filter()) {
    run_rank(*this, kernel, inv, work);
  } else if (guided()) {
    run_guided(*this, kernel, inv, work);
  } else {
    run_bilateral(*this, kernel, inv, work);
  }
}
OperationDefinition make_local(const std::string& key, LocalKind kind,
                               Profile profile) {
  std::vector<Parameter> p{{"y_axis", Type::Int64}, {"x_axis", Type::Int64}};
  unsigned ports = kind == LocalKind::Bilateral ? 1 : 2;
  if (ranked(kind)) {
    generation::append(&p, {{"anchor_y", Type::Int64},
                            {"anchor_x", Type::Int64},
                            {"boundary", Type::String},
                            {"cval", Type::Float64},
                            {"empty", Type::String}});
    if (kind == LocalKind::Median)
      p.push_back({"even", Type::String});
    else
      generation::append(
          &p, {{"q", Type::Float64}, {"interpolation", Type::String}});
  } else {
    generation::append(&p,
                       {{"radius_y", Type::Int64}, {"radius_x", Type::Int64}});
    if (regression(kind)) {
      generation::append(
          &p, {{"epsilon", Type::Float64},
               {"guide_metric_scale",
                kind == LocalKind::Guided ? Type::Float64 : Type::String},
               {"component_axis", Type::Int64}});
    } else {
      generation::append(&p, {{"sigma_space_y", Type::Float64},
                              {"sigma_space_x", Type::Float64},
                              {"sigma_range", Type::Float64},
                              {"metric_scale", Type::String},
                              {"boundary", Type::String}});
      if (kind == LocalKind::Joint)
        p.push_back({"component_axis", Type::Int64});
    }
  }
  return generation::operation(
      key, ports, std::move(p),
      {{"output", tensor_schema(ElementType::Float64, {1, 1})}},
      [kind, profile](const auto& in, const auto& p) {
        return std::make_shared<LocalFilter>(kind, profile, in, p);
      });
}
}  // namespace ps::plugin_internal::filter
