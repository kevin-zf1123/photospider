#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "04-mask-morphology/spatial.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
struct Vertex final {
  double y, x;
};
constexpr unsigned kStandard[31][2] = {
    {8, 8},  {12, 12}, {4, 4},  {6, 2},  {14, 6},  {2, 10},  {10, 14}, {9, 5},
    {7, 11}, {13, 9},  {5, 3},  {3, 13}, {1, 7},   {11, 15}, {15, 1},  {9, 9},
    {7, 5},  {5, 10},  {12, 7}, {3, 6},  {10, 13}, {13, 11}, {11, 3},  {6, 14},
    {8, 1},  {4, 2},   {2, 12}, {0, 8},  {15, 4},  {14, 15}, {1, 0},
};
struct PolygonOffset final : Program {
  std::uint64_t vertices, height, width;
  unsigned n = 0, count;
  bool standard;
  double sy, sx, radius;
  ElementType type;
  PolygonOffset(const std::vector<OperationMetadata>& in, const Parameters& p)
      : sy(real(p, "sy")),
        sx(real(p, "sx")),
        radius(real(p, "radius")),
        type(output_dtype(p)) {
    const auto& s = tensor(in[0]);
    require(floating(s.descriptor.element_type) &&
                s.sample_shape().size() == 2 && s.sample_shape()[0] >= 3 &&
                s.sample_shape()[1] == 2,
            "polygon offset requires vertices[V,2]");
    vertices = s.sample_shape()[0];
    const auto h = integer(p, "height"), w = integer(p, "width");
    require(h > 0 && w > 0, "polygon offset canvas");
    height = h;
    width = w;
    size_product({height, width});
    const auto pattern = text(p, "sample_pattern");
    parameter(pattern == "grid_center" || pattern == "vulkan_standard",
              "polygon sample pattern");
    standard = pattern == "vulkan_standard";
    if (standard) {
      parameter(p.count("sample_count") && !p.count("samples_per_axis"),
                "standard samples require sample_count only");
      const auto requested = integer(p, "sample_count");
      parameter(requested == 1 || requested == 2 || requested == 4 ||
                    requested == 8 || requested == 16,
                "standard sample count");
      count = static_cast<unsigned>(requested);
    } else {
      parameter(p.count("samples_per_axis") && !p.count("sample_count"),
                "grid samples require samples_per_axis only");
      const auto requested = integer(p, "samples_per_axis");
      parameter(requested >= 1 && requested <= 64, "grid sample axis");
      n = static_cast<unsigned>(requested);
      count = n * n;
    }
    scratch_bytes =
        bytes(sizeof(Exact) +
              static_cast<unsigned __int128>(vertices) * sizeof(Vertex));
    add_output(this, spatial_schema(type, {height, width}));
  }
  std::array<unsigned, 3> local(unsigned j) const {
    if (!standard) {
      return {2 * (j / n) + 1, 2 * (j % n) + 1, 2 * n};
    }
    const auto& xy = kStandard[count - 1 + j];
    return {xy[1], xy[0], 16};
  }
  std::optional<bool> fast_test(const Vertex* v, Fast y, Fast x,
                                Visits& visits) const {
    int winding = 0;
    bool near = false, closer = false;
    const auto r2 = square(Fast::point(radius));
    if (!r2.finite()) {
      return {};
    }
    for (std::uint64_t i = 0; i < vertices; ++i) {
      visits.step();
      const auto& a = v[i];
      const auto& b = v[i + 1 == vertices ? 0 : i + 1];
      const auto compare_y = [&](double q) -> std::optional<int> {
        if (y.high < q) {
          return -1;
        }
        if (y.low > q) {
          return 1;
        }
        if (y.low == y.high && y.low == q) {
          return 0;
        }
        return {};
      };
      const auto ay = compare_y(a.y), by = compare_y(b.y);
      if (!ay || !by) {
        return {};
      }
      const auto dy = Fast::point(b.y) - Fast::point(a.y);
      const auto dx = Fast::point(b.x) - Fast::point(a.x);
      const auto py = y - Fast::point(a.y), px = x - Fast::point(a.x);
      const auto cross = dx * py - dy * px;
      if (!cross.finite()) {
        return {};
      }
      if (*ay >= 0 && *by < 0) {
        if (cross.low > 0) {
          ++winding;
        } else if (cross.high >= 0) {
          return {};
        }
      } else if (*by >= 0 && *ay < 0) {
        if (cross.high < 0) {
          --winding;
        } else if (cross.low <= 0) {
          return {};
        }
      }
      const auto length = square(dy) + square(dx);
      const auto dot = py * dy + px * dx;
      Fast d2;
      Fast bound = r2;
      if (dot.high <= 0) {
        d2 = square(py) + square(px);
      } else if (dot.low >= length.high) {
        d2 = square(y - Fast::point(b.y)) + square(x - Fast::point(b.x));
      } else if (dot.low > 0 && dot.high < length.low) {
        d2 = square(cross);
        bound = r2 * length;
      } else {
        return {};
      }
      if (!d2.finite() || !bound.finite()) {
        return {};
      }
      if (d2.high < bound.low) {
        near = true;
        closer = true;
      } else if (d2.low <= bound.high) {
        return {};
      }
    }
    return radius >= 0 ? (winding != 0 || near) : (winding != 0 && !closer);
  }
  bool exact_test(Exact& e, const Vertex* v, const Rational& y,
                  const Rational& x, Visits& visits) const {
    int winding = 0;
    bool boundary = false, near = false, closer = false;
    const auto r = e.binary(bits(radius)), r2 = e.multiply(r, r);
    const auto zero = e.integer(0);
    for (std::uint64_t i = 0; i < vertices; ++i) {
      visits.step();
      const auto& a = v[i];
      const auto& b = v[i + 1 == vertices ? 0 : i + 1];
      const auto ay = e.binary(bits(a.y)), ax = e.binary(bits(a.x));
      const auto by = e.binary(bits(b.y)), bx = e.binary(bits(b.x));
      const auto dy = e.subtract(by, ay), dx = e.subtract(bx, ax);
      const auto py = e.subtract(y, ay), px = e.subtract(x, ax);
      const auto cross = e.subtract(e.multiply(dx, py), e.multiply(dy, px));
      const auto length = e.add(e.multiply(dy, dy), e.multiply(dx, dx));
      const auto dot = e.add(e.multiply(py, dy), e.multiply(px, dx));
      const auto side = e.order(cross, zero);
      const auto start = e.order(dot, zero), end = e.order(dot, length);
      boundary |= side == 0 && start >= 0 && end <= 0;
      if (e.order(ay, y) <= 0 && e.order(y, by) < 0 && side > 0) {
        ++winding;
      }
      if (e.order(by, y) <= 0 && e.order(y, ay) < 0 && side < 0) {
        --winding;
      }
      Rational distance, bound = r2;
      if (start <= 0) {
        distance = e.add(e.multiply(py, py), e.multiply(px, px));
      } else if (end >= 0) {
        const auto qy = e.subtract(y, by), qx = e.subtract(x, bx);
        distance = e.add(e.multiply(qy, qy), e.multiply(qx, qx));
      } else {
        distance = e.multiply(cross, cross);
        bound = e.multiply(r2, length);
      }
      const auto order = e.order(distance, bound);
      near |= order <= 0;
      closer |= order < 0;
    }
    check(e.status());
    return radius >= 0 ? (boundary || winding || near)
                       : ((boundary || winding) && !closer);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    ExactArena arena(inv.scratch[0].allocation, work);
    auto& e = *arena.p;
    auto* v = reinterpret_cast<Vertex*>(arena.p + 1);
    const Reader reader{kernel, inv};
    for (std::uint64_t i = 0; i < vertices; ++i) {
      v[i] = {reader.value(0, 0, {i, 0}), reader.value(0, 0, {i, 1})};
      visits.step();
    }
    const auto& output = inv.outputs[0];
    samples(output, [&](const Coordinate& at, std::uint64_t) {
      unsigned hits = 0;
      for (unsigned j = 0; j < count; ++j) {
        visits.step();
        const auto s = local(j);
        const auto fy =
            (Fast::point(static_cast<double>(at[0])) - Fast::point(.5) +
             Fast::point(s[0]) / Fast::point(s[2])) *
            Fast::point(sy);
        const auto fx =
            (Fast::point(static_cast<double>(at[1])) - Fast::point(.5) +
             Fast::point(s[1]) / Fast::point(s[2])) *
            Fast::point(sx);
        const auto quick = fast_test(v, fy, fx, visits);
        if (quick) {
          hits += *quick;
        } else {
          const auto y =
              e.multiply(e.add(e.subtract(e.integer(at[0]), e.fraction(1, 2)),
                               e.fraction(s[0], s[2])),
                         e.binary(bits(sy)));
          const auto x =
              e.multiply(e.add(e.subtract(e.integer(at[1]), e.fraction(1, 2)),
                               e.fraction(s[1], s[2])),
                         e.binary(bits(sx)));
          hits += exact_test(e, v, y, x, visits);
        }
      }
      // Both integers are exactly representable in either dtype (count <=
      // 4096). A single native division is therefore the specified RN_T(hit
      // fraction).
      const bool narrow = type == ElementType::Float32;
      const double coverage =
          narrow ? static_cast<float>(hits) / static_cast<float>(count)
                 : static_cast<double>(hits) / count;
      store(output, at, convert(coverage, narrow));
    });
    visits.flush();
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_polygon_offset(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    check(registry->register_operation(
        definition("offset_polygon_grid", 1,
                   {{"height", Type::Int64},
                    {"width", Type::Int64},
                    {"sy", Type::Float64},
                    {"sx", Type::Float64},
                    {"radius", Type::Float64},
                    {"sample_pattern", Type::String},
                    {"samples_per_axis", Type::Int64, false},
                    {"sample_count", Type::Int64, false},
                    {"output_dtype", Type::String}},
                   [](const auto& in, const auto& p) {
                     return std::make_shared<PolygonOffset>(in, p);
                   })));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
