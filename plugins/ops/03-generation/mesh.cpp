#include <array>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/fast_math.hpp"
#include "03-generation/scalar_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
enum class MeshKind { Bilinear, Triangle, Bicubic };
using MeshMath = ScalarMath<384>;
using MeshRational = MeshMath::R;
struct MeshStorage final {
  MeshMath math;
  std::array<MeshRational, 4> bounds;
  std::array<MeshRational, 2> span;
  std::array<MeshRational, 16> weights;
  explicit MeshStorage(const Work& work) : math(work) {}
};
struct Mesh final : Program, Canvas {
  MeshKind kind;
  bool pad = true;
  std::uint64_t channels = 1, faces = 0;
  Mesh(MeshKind selected, const std::vector<OperationMetadata>& in,
       const Parameters& p)
      : Canvas(p), kind(selected) {
    scratch_bytes = sizeof(MeshStorage);
    const auto outside = text(p, "outside", "pad");
    if (kind != MeshKind::Triangle) {
      require(outside == "pad" || outside == "reject", "mesh outside mode");
      pad = outside == "pad";
      shape(in[0], {4});
      const auto& control = tensor(in[1]);
      const auto s = control.sample_shape();
      const unsigned n = kind == MeshKind::Bilinear ? 2 : 4;
      require(s.size() == 3 && s[0] == n && s[1] == n &&
                  floating(control.descriptor.element_type),
              "mesh control tensor must be [N,N,C] Float32/64");
      channels = s[2];
    } else {
      const auto& vertices = tensor(in[0]);
      const auto& topology = tensor(in[1]);
      const auto& control = tensor(in[2]);
      const auto v = vertices.sample_shape(), f = topology.sample_shape(),
                 c = control.sample_shape();
      require(v.size() == 2 && v[1] == 2 &&
                  floating(vertices.descriptor.element_type),
              "mesh vertices must be [V,2] Float32/64");
      require(f.size() == 2 && f[1] == 3 &&
                  topology.descriptor.element_type == ElementType::Int64,
              "mesh triangles must be [F,3] Int64");
      require(c.size() == 2 && c[0] == v[0] &&
                  floating(control.descriptor.element_type),
              "mesh controls must be [V,C] Float32/64");
      channels = c[1];
      faces = f[0];
    }
    add_output(this,
               tensor_schema(type, {height, width, channels}, true, true));
    if (kind == MeshKind::Triangle)
      add_output(this,
                 tensor_schema(ElementType::UInt8, {height, width}, true));
  }
  std::vector<ReadRegion> support(unsigned output,
                                  const Region&) const override {
    std::vector<ReadRegion> result;
    const unsigned count = kind == MeshKind::Triangle ? (output ? 2 : 3) : 2;
    for (unsigned p = 0; p < count; ++p)
      result.push_back(
          {p, 0,
           Region::whole(inputs[p].result_schema->tensors[0].sample_shape())});
    return result;
  }
  static MeshRational cross(MeshMath& m, const std::array<MeshRational, 2>& a,
                            const std::array<MeshRational, 2>& b) {
    return m.sub(m.mul(a[0], b[1]), m.mul(a[1], b[0]));
  }
  bool locate(MeshStorage& storage, const Reader& reader, const Work& work,
              const MeshRational& x, const MeshRational& y,
              std::array<std::uint64_t, 3>* rows, bool validity) const {
    auto& m = storage.math;
    auto& e = m.exact;
    auto& weights = storage.weights;
    for (std::uint64_t f = 0; f < faces; ++f) {
      work.poll();
      std::array<std::array<MeshRational, 2>, 3> v;
      for (unsigned j = 0; j < 3; ++j) {
        (*rows)[j] = reader.index(1, 0, {f, j});
        v[j] = {m.input(reader, 0, {(*rows)[j], 0}),
                m.input(reader, 0, {(*rows)[j], 1})};
      }
      const std::array<MeshRational, 2> b{m.sub(v[1][0], v[0][0]),
                                          m.sub(v[1][1], v[0][1])};
      const std::array<MeshRational, 2> c{m.sub(v[2][0], v[0][0]),
                                          m.sub(v[2][1], v[0][1])};
      const std::array<MeshRational, 2> q{m.sub(x, v[0][0]), m.sub(y, v[0][1])};
      const auto denominator = cross(m, b, c);
      weights[1] = cross(m, q, c);
      weights[2] = cross(m, b, q);
      weights[0] = m.sub(denominator, m.add(weights[1], weights[2]));
      bool inside = true;
      for (unsigned j = 0; j < 3; ++j)
        inside &= weights[j].negative == denominator.negative ||
                  MeshMath::E::zero(weights[j]);
      check(e.status());
      if (!inside)
        continue;
      if (!validity) {
        for (unsigned j = 0; j < 3; ++j)
          weights[j] = m.div(weights[j], denominator);
      }
      check(e.status());
      return true;
    }
    return false;
  }
  // A proven outside face may be skipped. Any uncertain edge or determinant
  // returns to exact face-row traversal, preserving boundary ownership.
  std::optional<bool> fast_locate(const Reader& reader, const Work& work,
                                  Fast x, Fast y,
                                  std::array<std::uint64_t, 3>* rows,
                                  std::array<Fast, 16>* weights,
                                  bool validity) const {
    const auto cross_fast = [](const std::array<Fast, 2>& a,
                               const std::array<Fast, 2>& b) {
      return a[0] * b[1] - a[1] * b[0];
    };
    for (std::uint64_t f = 0; f < faces; ++f) {
      work.poll(40);
      std::array<std::array<Fast, 2>, 3> v;
      for (unsigned j = 0; j < 3; ++j) {
        (*rows)[j] = reader.index(1, 0, {f, j});
        v[j] = {Fast::point(reader.number(0, 0, {(*rows)[j], 0})),
                Fast::point(reader.number(0, 0, {(*rows)[j], 1}))};
      }
      const std::array<Fast, 2> b{v[1][0] - v[0][0], v[1][1] - v[0][1]};
      const std::array<Fast, 2> c{v[2][0] - v[0][0], v[2][1] - v[0][1]};
      const std::array<Fast, 2> q{x - v[0][0], y - v[0][1]};
      auto denominator = cross_fast(b, c);
      if (!denominator.finite() || !denominator.nonzero())
        return {};
      (*weights)[1] = cross_fast(q, c);
      (*weights)[2] = cross_fast(b, q);
      (*weights)[0] = denominator - ((*weights)[1] + (*weights)[2]);
      if (denominator.high < 0) {
        denominator = -denominator;
        for (unsigned j = 0; j < 3; ++j)
          (*weights)[j] = -(*weights)[j];
      }
      bool outside = false, inside = true;
      for (unsigned j = 0; j < 3; ++j) {
        if (!(*weights)[j].finite())
          return {};
        outside |= (*weights)[j].high < 0;
        inside &= (*weights)[j].low >= 0;
      }
      if (outside)
        continue;
      if (!inside)
        return {};
      if (!validity) {
        for (unsigned j = 0; j < 3; ++j)
          (*weights)[j] = (*weights)[j] / denominator;
      }
      return true;
    }
    return false;
  }
  void patch_weights(MeshStorage& storage, const MeshRational& x,
                     const MeshRational& y, int* corner) const {
    auto& m = storage.math;
    auto& e = m.exact;
    auto u = m.div(m.sub(x, storage.bounds[0]), storage.span[0]);
    auto v = m.div(m.sub(y, storage.bounds[1]), storage.span[1]);
    const auto one = e.integer(1), zero = e.integer(0);
    if (pad) {
      u = e.order(u, zero) < 0 ? zero : e.order(u, one) > 0 ? one : u;
      v = e.order(v, zero) < 0 ? zero : e.order(v, one) > 0 ? one : v;
    }
    const unsigned n = kind == MeshKind::Bilinear ? 2 : 4;
    const bool u0 = MeshMath::E::zero(u), v0 = MeshMath::E::zero(v);
    const bool u1 = e.order(u, one) == 0, v1 = e.order(v, one) == 0;
    *corner = (u0 || u1) && (v0 || v1)
                  ? static_cast<int>((v1 ? n - 1 : 0) * n + (u1 ? n - 1 : 0))
                  : -1;
    std::array<MeshRational, 4> bu, bv;
    if (n == 2) {
      bu[0] = m.sub(one, u);
      bu[1] = u;
      bv[0] = m.sub(one, v);
      bv[1] = v;
    } else {
      const auto a = m.sub(one, u), b = m.sub(one, v);
      const auto three = e.integer(3);
      bu = {m.mul(m.sq(a), a), m.mul(m.mul(m.sq(a), u), three),
            m.mul(m.mul(a, m.sq(u)), three), m.mul(m.sq(u), u)};
      bv = {m.mul(m.sq(b), b), m.mul(m.mul(m.sq(b), v), three),
            m.mul(m.mul(b, m.sq(v)), three), m.mul(m.sq(v), v)};
    }
    for (unsigned j = 0; j < n; ++j)
      for (unsigned i = 0; i < n; ++i)
        storage.weights[j * n + i] = m.mul(bu[i], bv[j]);
    check(e.status());
  }
  bool fast_patch_weights(const std::array<Fast, 4>& bounds, Fast x, Fast y,
                          std::array<Fast, 16>* weights) const {
    auto u = (x - bounds[0]) / (bounds[2] - bounds[0]);
    auto v = (y - bounds[1]) / (bounds[3] - bounds[1]);
    if (!u.finite() || !v.finite())
      return false;
    if (pad) {
      u = {std::clamp(u.low, 0.0, 1.0), std::clamp(u.high, 0.0, 1.0)};
      v = {std::clamp(v.low, 0.0, 1.0), std::clamp(v.high, 0.0, 1.0)};
    }
    const auto one = Fast::point(1), three = Fast::point(3);
    const unsigned n = kind == MeshKind::Bilinear ? 2 : 4;
    std::array<Fast, 4> bu{one - u, u}, bv{one - v, v};
    if (n == 4) {
      const auto a = one - u, b = one - v;
      bu = {a * a * a, three * a * a * u, three * a * u * u, u * u * u};
      bv = {b * b * b, three * b * b * v, three * b * v * v, v * v * v};
    }
    for (unsigned j = 0; j < n; ++j)
      for (unsigned i = 0; i < n; ++i)
        (*weights)[j * n + i] = bu[i] * bv[j];
    return true;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* memory = new (invocation.scratch[0].allocation) MeshStorage(work);
    struct Destroy {
      MeshStorage* p;
      ~Destroy() { p->~MeshStorage(); }
    } destroy{memory};
    auto& storage = *memory;
    auto& m = storage.math;
    auto& e = m.exact;
    const bool narrow = type == ElementType::Float32;
    const auto& out = invocation.outputs[0];
    const bool validity = kernel.writes[0].first == 1;
    std::array<Fast, 4> bounds;
    if (kind != MeshKind::Triangle) {
      for (unsigned j = 0; j < 4; ++j) {
        storage.bounds[j] = m.input(reader, 0, {j});
        bounds[j] = Fast::point(reader.number(0, 0, {j}));
      }
      for (unsigned j = 0; j < 2; ++j)
        storage.span[j] = m.sub(storage.bounds[j + 2], storage.bounds[j]);
    }
    std::array<std::uint64_t, 3> rows{};
    std::array<Fast, 16> weights;
    std::uint64_t previous_x = UINT64_MAX, previous_y = UINT64_MAX;
    bool fast = false, exact = false, inside = true;
    int corner = -1;
    const unsigned n = kind == MeshKind::Bilinear ? 2 : 4;
    const unsigned terms = kind == MeshKind::Triangle ? 3 : n * n;
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      if (at[1] != previous_x || at[0] != previous_y) {
        previous_x = at[1];
        previous_y = at[0];
        fast = exact = false;
        inside = true;
        corner = -1;
        if (narrow || validity) {
          work.poll(80);
          const auto x = fast_center(origin_x, at[1]);
          const auto y = fast_center(origin_y, at[0]);
          if (kind == MeshKind::Triangle) {
            const auto located =
                fast_locate(reader, work, x, y, &rows, &weights, validity);
            fast = located.has_value();
            if (fast)
              inside = *located;
          } else {
            fast = fast_patch_weights(bounds, x, y, &weights);
          }
        }
      }
      if (fast && (validity || !inside)) {
        store(out, at, inside ? 1 : 0);
        return;
      }
      const auto control = [&](unsigned term) -> Coordinate {
        return kind == MeshKind::Triangle
                   ? Coordinate{rows[term], at[2]}
                   : Coordinate{term / n, term % n, at[2]};
      };
      const unsigned port = kind == MeshKind::Triangle ? 2 : 1;
      if (fast) {
        work.poll(terms * 2);
        auto sum = Fast::point(0);
        for (unsigned j = 0; j < terms; ++j)
          sum = sum +
                weights[j] * Fast::point(reader.number(port, 0, control(j)));
        if (const auto rounded = fast_result(sum, narrow, false)) {
          store(out, at, *rounded);
          return;
        }
      }
      if (!exact) {
        const auto x = m.center(origin_x, at[1]), y = m.center(origin_y, at[0]);
        if (kind == MeshKind::Triangle) {
          inside = locate(storage, reader, work, x, y, &rows, validity);
          if (inside && !validity) {
            for (unsigned j = 0; j < 3; ++j)
              if (e.order(storage.weights[j], e.integer(1)) == 0)
                corner = static_cast<int>(j);
          }
        } else {
          patch_weights(storage, x, y, &corner);
        }
        check(e.status());
        exact = true;
      }
      if (validity || !inside) {
        store(out, at, inside ? 1 : 0);
      } else if (corner >= 0) {
        store(out, at,
              convert(reader.number(port, 0, control(corner)), narrow));
      } else {
        auto sum = e.integer(0);
        for (unsigned j = 0; j < terms; ++j)
          sum = m.add(sum, m.mul(storage.weights[j],
                                 m.input(reader, port, control(j))));
        store(out, at, m.rounded(sum, narrow));
      }
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_mesh_generation(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    const char* names[] = {"bilinear_rectangle_gradient",
                           "triangle_mesh_gradient", "bicubic_parameter_patch"};
    for (unsigned i = 0; i < 3; ++i) {
      const auto kind = static_cast<MeshKind>(i);
      auto parameters = canvas_parameters();
      if (kind != MeshKind::Triangle)
        parameters.push_back(
            {"outside", OperationParameterType::String, false});
      std::vector<std::pair<std::string, SchemaTemplate>> outputs{
          {"values", tensor_schema(ElementType::Float64, {1})}};
      if (kind == MeshKind::Triangle)
        outputs.emplace_back("valid", tensor_schema(ElementType::UInt8, {1}));
      auto definition =
          operation(std::string("generation.") + names[i] + "_v1_strict",
                    kind == MeshKind::Triangle ? 3 : 2, parameters, outputs,
                    [kind](const auto& inputs, const auto& p) {
                      return std::make_shared<Mesh>(kind, inputs, p);
                    });
      if (kind == MeshKind::Triangle)
        definition.traits.outputs[1].input_indices =
            std::vector<std::uint32_t>{0, 1};
      check(registry->register_operation(std::move(definition)));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
