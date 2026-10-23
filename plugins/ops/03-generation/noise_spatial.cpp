#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/fast_math.hpp"
#include "03-generation/paths.hpp"
#include "03-generation/scalar_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
using DistanceMath = ScalarMath<128>;
using Distance = DistanceMath::R;
struct Site final {
  std::array<double, 2> position{};
  std::int64_t x = 0, y = 0;
  Fast bound{};
};
Distance squared_distance(DistanceMath& m, const std::array<double, 2>& q,
                          const Site& site) {
  const auto dx =
      m.sub(m.exact.binary(bits(q[0])), m.exact.binary(bits(site.position[0])));
  const auto dy =
      m.sub(m.exact.binary(bits(q[1])), m.exact.binary(bits(site.position[1])));
  return m.add(m.sq(dx), m.sq(dy));
}
Fast distance_bound(const std::array<double, 2>& q, const Site& site) {
  const auto dx = Fast::point(q[0]) - Fast::point(site.position[0]);
  const auto dy = Fast::point(q[1]) - Fast::point(site.position[1]);
  return dx * dx + dy * dy;
}
struct Nearest final : Program {
  bool cellular;
  ElementType type;
  unsigned rank;
  std::uint64_t seed, stream, frame, rings;
  Nearest(bool cells, const std::vector<OperationMetadata>& in,
          const Parameters& p)
      : cellular(cells),
        type(dtype(p)),
        seed(integer(p, "seed")),
        stream(integer(p, "stream")),
        frame(integer(p, "frame")),
        rings(integer(p, "max_search_rings", 64)) {
    const auto& field = tensor(in[0]);
    auto dimensions = field.sample_shape();
    require(floating(field.descriptor.element_type) && dimensions.size() >= 2 &&
                dimensions.back() == 2,
            "nearest noise requires coordinate pairs");
    rank = dimensions.size() - 1;
    dimensions.pop_back();
    add_output(this, tensor_schema(type, dimensions));
    add_output(this, tensor_schema(type, dimensions));
    if (cellular) {
      dimensions.push_back(2);
      add_output(this,
                 tensor_schema(ElementType::Int64, dimensions, false, true));
    } else {
      require(in[1].result_schema != nullptr,
              "nearest points requires a schema");
      const auto& schema = *in[1].result_schema;
      require(schema.id == "photospider.ops.dynamic_points" &&
                  schema.tensors.size() == 3 &&
                  schema.tensors[0].sample_shape().size() == 2,
              "nearest points requires DynamicPoints v1");
      const auto expected = take(ps::generation::dynamic_points_schema(
          schema.tensors[0].sample_shape()[0]));
      require(schema.same_schema(expected), "DynamicPoints schema directory");
      add_output(this, tensor_schema(ElementType::Int64, dimensions));
    }
    // A squared binary64 difference needs at most 4200 bits. Cross products
    // used by ordering fit 8192 bits, including subnormal/large-value mixtures.
    scratch_bytes = sizeof(DistanceMath);
  }
  std::vector<ReadRegion> support(unsigned port,
                                  const Region& region) const override {
    auto dimensions = region.dimensions();
    if (cellular && port == 2)
      dimensions.pop_back();
    dimensions.push_back({0, 2});
    std::vector<ReadRegion> result{{0, 0, Region(std::move(dimensions))}};
    if (!cellular) {
      for (unsigned member = 0; member < 3; ++member)
        result.push_back(
            {1, member,
             Region::whole(
                 inputs[1].result_schema->tensors[member].sample_shape())});
    }
    return result;
  }
  bool before(DistanceMath& m, const std::array<double, 2>& q, const Site& a,
              const Site& b) const {
#ifndef PHOTOSPIDER_NOI_EXACT_ONLY
    if (a.bound.finite() && b.bound.finite()) {
      if (a.bound.high < b.bound.low)
        return true;
      if (a.bound.low > b.bound.high)
        return false;
    }
#endif
    const int order =
        m.exact.order(squared_distance(m, q, a), squared_distance(m, q, b));
    check(m.exact.status());
    return order < 0 ||
           (order == 0 &&
            (cellular ? std::pair{a.y, a.x} < std::pair{b.y, b.x} : a.x < b.x));
  }
  std::array<Site, 2> find(DistanceMath& m, const Reader& reader,
                           const Work& work,
                           const std::array<double, 2>& q) const {
    std::array<Site, 2> best{};
    unsigned count = 0;
    const auto insert = [&](Site site) {
      work.poll(16);
      site.bound = distance_bound(q, site);
      if (!count) {
        best[0] = site;
        count = 1;
      } else if (before(m, q, site, best[0])) {
        best[1] = best[0];
        best[0] = site;
        count = 2;
      } else if (count == 1 || before(m, q, site, best[1])) {
        best[1] = site;
        count = 2;
      }
    };
    if (!cellular) {
      const auto n = static_cast<std::uint64_t>(reader.index(1, 2, {0}));
      for (std::uint64_t i = 0; i < n; ++i)
        insert({{reader.number(1, 0, {i, 0}), reader.number(1, 0, {i, 1})},
                reader.index(1, 1, {i}),
                0,
                {}});
      return best;
    }
    const auto cx = static_cast<std::int64_t>(std::floor(q[0]));
    const auto cy = static_cast<std::int64_t>(std::floor(q[1]));
    for (std::uint64_t radius = 0; radius <= rings; ++radius) {
      const auto r = static_cast<std::int64_t>(radius);
      const auto visit = [&](std::int64_t x, std::int64_t y) {
        work.poll(10);
        const auto block = philox(x, y, 0, 7, seed, stream, frame);
        Site site{{}, x, y, {}};
        for (unsigned axis = 0; axis < 2; ++axis) {
          const auto cell = axis ? y : x;
          const double upper = static_cast<double>(cell + 1);
          const double v = static_cast<double>(cell) +
                           static_cast<double>(block[axis] >> 11) * 0x1p-53;
          site.position[axis] =
              v >= upper ? std::nextafter(upper, -INFINITY) : v;
        }
        insert(site);
      };
      for (auto x = cx - r; x <= cx + r; ++x) {
        visit(x, cy - r);
        if (r)
          visit(x, cy + r);
      }
      for (auto y = cy - r + 1; y < cy + r; ++y) {
        visit(cx - r, y);
        visit(cx + r, y);
      }
      if (count < 2)
        continue;
      const std::array<double, 4> edges{
          static_cast<double>(cx - r), static_cast<double>(cx + r + 1),
          static_cast<double>(cy - r), static_cast<double>(cy + r + 1)};
#ifndef PHOTOSPIDER_NOI_EXACT_ONLY
      const std::array<Fast, 4> gaps{Fast::point(q[0]) - Fast::point(edges[0]),
                                     Fast::point(edges[1]) - Fast::point(q[0]),
                                     Fast::point(q[1]) - Fast::point(edges[2]),
                                     Fast::point(edges[3]) - Fast::point(q[1])};
      double lower = INFINITY;
      for (const auto gap : gaps)
        lower = std::min(lower, gap.low);
      if (lower > 0 && best[1].bound.finite() &&
          numeric_ops::numeric_down(lower * lower) > best[1].bound.high)
        return best;
#endif
      auto gap =
          m.sub(m.exact.binary(bits(q[0])), m.exact.binary(bits(edges[0])));
      for (unsigned i = 1; i < 4; ++i) {
        const auto coordinate = m.exact.binary(bits(q[i / 2]));
        const auto edge = m.exact.binary(bits(edges[i]));
        const auto value =
            i & 1 ? m.sub(edge, coordinate) : m.sub(coordinate, edge);
        if (m.exact.order(value, gap) < 0)
          gap = value;
      }
      const bool closed =
          m.exact.order(m.sq(gap), squared_distance(m, q, best[1])) > 0;
      check(m.exact.status());
      if (closed)
        return best;
    }
    throw Status{ErrorCode::OperationFailed, "cellular F2 search not converged",
                 DomainErrorCode{UINT64_C(0x4e4f490001)}};
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Work work{invocation, work_limit};
    const Reader reader{kernel, invocation};
    auto* math = new (invocation.scratch[0].allocation) DistanceMath(work);
    struct Destroy {
      DistanceMath* p;
      ~Destroy() { p->~DistanceMath(); }
    } destroy{math};
    const auto port = kernel.writes[0].first;
    const auto& out = invocation.outputs[0];
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      if (cellular && port == 2 && at[rank] != 0)
        return;
      math->exact.reset();
      auto coordinate = at;
      coordinate[rank] = 0;
      const double x = reader.number(0, 0, coordinate);
      coordinate[rank] = 1;
      const std::array<double, 2> q{x, reader.number(0, 0, coordinate)};
      const auto best = find(*math, reader, work, q);
      if (port == 2) {
        coordinate = at;
        if (cellular) {
          coordinate[rank] = 0;
          store(out, coordinate, static_cast<std::uint64_t>(best[0].x));
          coordinate[rank] = 1;
          store(out, coordinate, static_cast<std::uint64_t>(best[0].y));
        } else {
          store(out, at, static_cast<std::uint64_t>(best[0].x));
        }
        return;
      }
#ifndef PHOTOSPIDER_NOI_EXACT_ONLY
      if (const auto result =
              fast_result(fast_sqrt(best[port].bound),
                          type == ElementType::Float32, false)) {
        store(out, at, *result);
        return;
      }
#endif
      const auto result = math->exact.rounded_sqrt(
          squared_distance(*math, q, best[port]), type == ElementType::Float32);
      check(math->exact.status());
      store(out, at, result);
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_spatial_noise(OperationRegistry* registry) {
  using generation::append;
  using generation::check;
  using generation::Nearest;
  using generation::operation;
  using generation::rng_parameters;
  using generation::tensor_schema;
  try {
    const auto value = tensor_schema(ElementType::Float64, {1});
    const auto index = tensor_schema(ElementType::Int64, {1});
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (bool cells : {true, false}) {
      std::vector<OperationParameterSpec> parameters{
          {"dtype", OperationParameterType::String, false}};
      if (cells) {
        append(&parameters, rng_parameters());
        parameters.push_back(
            {"max_search_rings", OperationParameterType::Int64, false});
      }
      for (unsigned profile = 0; profile < 3; ++profile) {
        auto definition = operation(
            std::string("noise.") +
                (cells ? "cellular2d_l2" : "nearest_point_distances") + "_v1_" +
                suffix[profile],
            cells ? 1 : 2, parameters,
            {{"f1", value},
             {"f2", value},
             {cells ? "nearest_cell" : "nearest_id", index}},
            [cells, profile](const auto& in, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<Nearest>(cells, in, p);
            });
        if (!cells) {
          definition.traits.input_schema[1].element_type_mask = 0;
          definition.traits.input_schema[1].result_schema_id =
              "photospider.ops.dynamic_points";
          definition.traits.input_schema[1].result_schema_version = 1;
        }
        check(registry->register_operation(std::move(definition)));
      }
    }
    return Status::success();
  } catch (const Status& status) {
    return status;
  }
}
}  // namespace ps::plugin_internal
