#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/certified.hpp"
#include "05-filter/local_windows.hpp"

namespace ps::plugin_internal::filter {
namespace {
constexpr char kGaussian[] = "photospider.filter_bands.pyramid_binomial5_v1";
constexpr char kLaplacian[] = "photospider.filter_bands.pyramid_binomial5_v1";
std::int64_t reflect(std::int64_t i, std::uint64_t n) {
  const auto period = static_cast<std::int64_t>(2 * n);
  auto t = i % period;
  if (t < 0)
    t += period;
  return t < static_cast<std::int64_t>(n) ? t : period - 1 - t;
}
template <class Raw>
std::uint64_t pyramid_pixel(const Raw* input, std::uint64_t h, std::uint64_t w,
                            std::uint64_t y, std::uint64_t x, bool expand,
                            bool narrow, generation::Math& m,
                            const Fuel& fuel) {
  constexpr int weights[5] = {1, 4, 6, 4, 1};
  Ball sum;
  double scalar = 0, absolute = 0, roundoff = 0;
  bool finite = true, negative_zero = true;
  std::array<Atom, 25> values;
  std::array<int, 25> coefficients{};
  unsigned count = 0;
  std::array<std::uint64_t, 5> ys{}, xs{};
  std::array<int, 5> yw{}, xw{};
  unsigned yn = 0, xn = 0;
  for (int delta = -2; delta <= 2; ++delta) {
    if (!expand || !((static_cast<std::int64_t>(y) - delta) & 1)) {
      const auto at = expand ? (static_cast<std::int64_t>(y) - delta) / 2
                             : 2 * static_cast<std::int64_t>(y) + delta;
      ys[yn] =
          at >= 0 && static_cast<std::uint64_t>(at) < h ? at : reflect(at, h);
      yw[yn++] = weights[delta + 2];
    }
    if (!expand || !((static_cast<std::int64_t>(x) - delta) & 1)) {
      const auto at = expand ? (static_cast<std::int64_t>(x) - delta) / 2
                             : 2 * static_cast<std::int64_t>(x) + delta;
      xs[xn] =
          at >= 0 && static_cast<std::uint64_t>(at) < w ? at : reflect(at, w);
      xw[xn++] = weights[delta + 2];
    }
  }
  for (unsigned iy = 0; iy < yn; ++iy)
    for (unsigned ix = 0; ix < xn; ++ix) {
      fuel.poll();
      values[count] = {input[ys[iy] * w + xs[ix]], narrow};
      coefficients[count] = yw[iy] * xw[ix];
      const double v = values[count].value();
      if (narrow) {
        const double term = v * coefficients[count];
        const auto addition = Ball::two_sum(scalar, term);
        scalar = addition.first;
        if (addition.second != 0)
          roundoff =
              numeric_ops::numeric_up(roundoff + std::abs(addition.second));
        absolute += std::abs(term);
        finite &= std::isfinite(v);
        negative_zero &= v == 0 && std::signbit(v);
      } else {
        sum = sum + Ball::point(v).times(coefficients[count]);
      }
      ++count;
    }
  const auto divisor = expand ? 64 : 256;
  if (narrow && finite) {
    if (absolute == 0)
      return negative_zero ? UINT64_C(0x80000000) : 0;
    // Float32 * integer products and division by this power of two are
    // exact binary64 operations. TwoSum tracks the actual discarded sums;
    // zero roundoff proves the direct RN32, including exact midpoints.
    if (roundoff == 0)
      return generation::convert(scalar / divisor, true);
    const double error = roundoff;
    const auto low = generation::convert(
        numeric_ops::numeric_down(scalar - error) / divisor, true);
    const auto high = generation::convert(
        numeric_ops::numeric_up(scalar + error) / divisor, true);
    if (low == high)
      return low;
  }
  auto result = narrow ? std::optional<std::uint64_t>{}
                       : BoundedArithmetic{false}.finish(
                             BallValue{sum, Ball::point(divisor), true});
  if (!result) {
    ExactArithmetic e{m.exact,
                      narrow ? ElementType::Float32 : ElementType::Float64,
                      {}};
    auto sum = m.exact.integer(0);
    for (unsigned i = 0; i < count; ++i) {
      fuel.poll();
      const auto term = m.scale(e.from(values[i]), coefficients[i]);
      sum = i ? m.add(sum, term) : term;
    }
    result = e.finish(m.div(sum, m.exact.integer(divisor)));
  }
  return *result;
}
std::uint64_t combine(std::uint64_t a, std::uint64_t b, bool subtract,
                      bool narrow) {
  const Atom x{a, narrow}, y{b, narrow};
  const auto type = narrow ? ElementType::Float32 : ElementType::Float64;
  if (x.nan())
    return numeric_ops::converted_nan(a, type, type);
  if (y.nan())
    return numeric_ops::converted_nan(b, type, type);
  if (narrow) {
    const float aa = static_cast<float>(x.value()),
                bb = static_cast<float>(y.value());
    const float value = subtract ? aa - bb : aa + bb;
    return std::isnan(value) ? UINT64_C(0x7fc00000)
                             : generation::convert(value, true);
  }
  const double value = subtract ? x.value() - y.value() : x.value() + y.value();
  return std::isnan(value) ? UINT64_C(0x7ff8000000000000) : bits(value);
}
struct Pyramid final : Program {
  int kind;
  Profile profile;
  ElementType type;
  Geometry geometry;
  unsigned levels = 0;
  std::vector<std::vector<std::uint64_t>> shapes;
  Region selected;
  std::set<unsigned> bands;
  Pyramid(int k, Profile p, const std::vector<OperationMetadata>& in,
          const Parameters& args)
      : kind(k),
        profile(p),
        type(ElementType::Float64),
        geometry(make_geometry(k, in, args)) {
    check(numeric_ops::sequence_profile_available(p));
    type = in[0].result_schema->tensors[0].descriptor.element_type;
    if (args.count("expected_shape")) {
      std::string declared;
      for (auto n : geometry.dims) {
        if (!declared.empty())
          declared += ',';
        declared += std::to_string(n);
      }
      require(text(args, "expected_shape") == declared,
              "pyramid directory/source shape mismatch");
    }
    if (args.count("expected_dtype"))
      require(text(args, "expected_dtype") ==
                  (type == ElementType::Float32 ? "float32" : "float64"),
              "pyramid directory/source dtype mismatch");
    if (kind == 2) {
      const auto& schema = *in[0].result_schema;
      require(schema.id == kLaplacian && schema.version == 1 &&
                  !schema.tensors.empty(),
              "Laplacian band schema/profile");
      levels = static_cast<unsigned>(schema.tensors.size() - 1);
      shapes.push_back(schema.tensors[0].sample_shape());
      for (unsigned l = 0; l < levels; ++l) {
        auto next = shapes.back();
        require(next[geometry.ya] != 1 || next[geometry.xa] != 1,
                "extra pyramid level");
        next[geometry.ya] = (next[geometry.ya] + 1) / 2;
        next[geometry.xa] = (next[geometry.xa] + 1) / 2;
        shapes.push_back(std::move(next));
      }
      for (unsigned l = 0; l <= levels; ++l) {
        const auto& t = schema.tensors[l];
        require(
            std::string_view(t.key) ==
                    (l == levels ? "base" : "detail_" + std::to_string(l)) &&
                t.sample_shape() == shapes[l] &&
                t.descriptor.element_type == type,
            "Laplacian band directory shape/role/dtype");
        auto expected = schema.tensors[0];
        expected.key = t.key;
        for (unsigned a = 0; a < shapes[l].size(); ++a)
          if (a < expected.batch_axes.size())
            expected.batch_axes[a] = shapes[l][a];
          else
            expected.descriptor.shape[a - expected.batch_axes.size()] =
                shapes[l][a];
        require(expected.atomic_trailing_axes == t.atomic_trailing_axes &&
                    expected.batch_axes == t.batch_axes &&
                    expected.layout.spatial == t.layout.spatial &&
                    expected.layout.height_axis == t.layout.height_axis &&
                    expected.layout.width_axis == t.layout.width_axis &&
                    expected.layout.channel_axis == t.layout.channel_axis,
                "Laplacian band batch/tile layout");
      }
      SchemaTemplate out;
      out.id = "photospider.tensor";
      out.tensors.push_back(schema.tensors[0]);
      out.tensors[0].key = "samples";
      check(out.validate());
      add_output(this, std::move(out));
    } else {
      const auto l = integer(args, "levels");
      parameter(l >= 0 && l <= 15,
                "pyramid fixed Result holds 0..15 levels; use SDK composition "
                "for deeper pyramids");
      levels = static_cast<unsigned>(l);
      shapes.push_back(geometry.dims);
      for (unsigned i = 0; i < levels; ++i) {
        auto next = shapes.back();
        parameter(next[geometry.ya] != 1 || next[geometry.xa] != 1,
                  "extra pyramid level");
        next[geometry.ya] = (next[geometry.ya] + 1) / 2;
        next[geometry.xa] = (next[geometry.xa] + 1) / 2;
        shapes.push_back(std::move(next));
      }
      SchemaTemplate schema;
      schema.id = kind == 0 ? kGaussian : kLaplacian;
      for (unsigned i = 0; i <= levels; ++i) {
        auto t = tensor(in[0], true);
        t.key = kind == 0     ? "gaussian_" + std::to_string(i)
                : i == levels ? "base"
                              : "detail_" + std::to_string(i);
        for (unsigned a = 0; a < shapes[i].size(); ++a)
          if (a < t.batch_axes.size())
            t.batch_axes[a] = shapes[i][a];
          else
            t.descriptor.shape[a - t.batch_axes.size()] = shapes[i][a];
        schema.tensors.push_back(std::move(t));
      }
      check(schema.validate());
      add_output(this, std::move(schema));
    }
    collective = true;
    const auto pixels = static_cast<unsigned __int128>(geometry.h) * geometry.w;
    const auto coarse = static_cast<unsigned __int128>((geometry.h + 1) / 2) *
                        ((geometry.w + 1) / 2);
    scratch_bytes =
        bytes(sizeof(generation::Math) +
              (pixels + coarse) * (type == ElementType::Float32 ? 4 : 8) + 128);
  }
  static Geometry make_geometry(int k, const std::vector<OperationMetadata>& in,
                                const Parameters& p) {
    require(in[0].result_schema && !in[0].result_schema->tensors.empty(),
            "pyramid input schema");
    if (k != 2)
      tensor(in[0], true);
    auto args = p;
    args["anchor_y"] = static_cast<std::int64_t>(2);
    args["anchor_x"] = static_cast<std::int64_t>(2);
    args["boundary"] = std::string("reflect_half");
    args["cval"] = 0.0;
    return Geometry(in[0].result_schema->tensors[0], args, 5, 5, Kind::BoxSum);
  }
  Region plane_region(const Region& q) const {
    auto d = q.dimensions();
    d[geometry.ya] = {0, geometry.h};
    d[geometry.xa] = {0, geometry.w};
    return Region(std::move(d));
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    if (kind != 2)
      return {{0, 0, selected}};
    std::vector<ReadRegion> result;
    for (unsigned l = 0; l <= levels; ++l) {
      auto d = selected.dimensions();
      d[geometry.ya] = {0, shapes[l][geometry.ya]};
      d[geometry.xa] = {0, shapes[l][geometry.xa]};
      result.push_back({0, l, Region(std::move(d))});
    }
    return result;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    const Fuel fuel{work};
    const Reader reader{kernel, inv};
    generation::MathArena arena(inv.scratch[0].allocation, work);
    auto& m = *arena;
    m.exact.bind(fuel);
    const auto& g = geometry;
    const bool narrow = type == ElementType::Float32;
    const auto execute = [&](auto raw_type) {
      using Raw = decltype(raw_type);
      auto* first = reinterpret_cast<Raw*>(arena.value + 1);
      auto* second = first + g.h * g.w;
      each_plane(inv.outputs[0], g, [&](Coordinate plane) {
        auto* current = (kind == 2 && (levels & 1)) ? second : first;
        auto* next = current == first ? second : first;
        const unsigned start = kind == 2 ? levels : 0;
        auto h = shapes[start][g.ya], w = shapes[start][g.xa];
        for (std::uint64_t y = 0; y < h; ++y) {
          plane[g.ya] = y;
          for (std::uint64_t x = 0; x < w;) {
            plane[g.xa] = x;
            const auto& view = reader.view(0, start, plane);
            const auto count = std::min<std::uint64_t>(
                512, view.layout->origin[g.xa] + view.layout->shape[g.xa] - x);
            fuel.poll(count);
            const auto* input =
                static_cast<const std::uint8_t*>(view.allocation) +
                generation::offset(*view.layout, view.byte_offset, plane);
            if (view.layout->strides[g.xa] == sizeof(Raw)) {
              std::memcpy(current + y * w + x, input, count * sizeof(Raw));
            } else {
              for (std::uint64_t i = 0; i < count; ++i)
                std::memcpy(current + y * w + x + i,
                            input + static_cast<std::ptrdiff_t>(i) *
                                        view.layout->strides[g.xa],
                            sizeof(Raw));
            }
            x += count;
          }
        }
        const auto publish = [&](unsigned member, const Raw* data,
                                 std::uint64_t width, const Raw* coarse,
                                 std::uint64_t ch, std::uint64_t cw) {
          for (unsigned o = 0; o < inv.output_count; ++o) {
            if (kernel.writes[o].second != member)
              continue;
            const auto& view = inv.outputs[o];
            for (std::uint64_t y = view.layout->origin[g.ya];
                 y < view.layout->origin[g.ya] + view.layout->shape[g.ya];
                 ++y) {
              plane[g.ya] = y;
              if (!coarse && view.layout->strides[g.xa] == sizeof(Raw)) {
                plane[g.xa] = view.layout->origin[g.xa];
                auto* output =
                    static_cast<std::uint8_t*>(view.allocation) +
                    generation::offset(*view.layout, view.byte_offset, plane);
                for (std::uint64_t x = 0; x < view.layout->shape[g.xa];) {
                  const auto count = std::min<std::uint64_t>(
                      512, view.layout->shape[g.xa] - x);
                  fuel.poll(count);
                  std::memcpy(output + x * sizeof(Raw),
                              data + y * width + plane[g.xa] + x,
                              count * sizeof(Raw));
                  x += count;
                }
                continue;
              }
              for (std::uint64_t x = view.layout->origin[g.xa];
                   x < view.layout->origin[g.xa] + view.layout->shape[g.xa];
                   ++x) {
                fuel.poll();
                plane[g.xa] = x;
                auto raw = data[y * width + x];
                if (coarse)
                  raw = combine(raw,
                                pyramid_pixel(coarse, ch, cw, y, x, true,
                                              narrow, m, fuel),
                                true, narrow);
                store(view, plane, raw);
              }
            }
          }
        };
        if (kind == 2) {
          for (unsigned l = levels; l-- > 0;) {
            const auto nh = shapes[l][g.ya], nw = shapes[l][g.xa];
            for (std::uint64_t y = 0; y < nh; ++y) {
              plane[g.ya] = y;
              for (std::uint64_t x = 0; x < nw; ++x) {
                fuel.poll();
                plane[g.xa] = x;
                const auto expanded =
                    pyramid_pixel(current, h, w, y, x, true, narrow, m, fuel);
                next[y * nw + x] =
                    combine(reader.raw(0, l, plane), expanded, false, narrow);
              }
            }
            std::swap(current, next);
            h = nh;
            w = nw;
          }
          publish(0, current, w, nullptr, 0, 0);
        } else {
          const unsigned highest = *bands.rbegin();
          const unsigned needed =
              kind == 0 ? highest : std::min(levels, highest + 1);
          if (kind == 0 && bands.count(0))
            publish(0, current, w, nullptr, 0, 0);
          for (unsigned l = 0; l < needed; ++l) {
            const auto nh = shapes[l + 1][g.ya], nw = shapes[l + 1][g.xa];
            for (std::uint64_t y = 0; y < nh; ++y)
              for (std::uint64_t x = 0; x < nw; ++x)
                next[y * nw + x] =
                    pyramid_pixel(current, h, w, y, x, false, narrow, m, fuel);
            if (kind == 1 && bands.count(l))
              publish(l, current, w, next, nh, nw);
            std::swap(current, next);
            h = nh;
            w = nw;
            if (kind == 0 && bands.count(l + 1))
              publish(l + 1, current, w, nullptr, 0, 0);
          }
          if (kind == 1 && bands.count(levels))
            publish(levels, current, w, nullptr, 0, 0);
        }
      });
    };
    if (narrow)
      execute(std::uint32_t{});
    else
      execute(std::uint64_t{});
    fuel.flush();
  }
};
using Address = std::pair<unsigned, TileIndex>;
std::vector<std::uint64_t> key_for(const Region& r) {
  std::vector<std::uint64_t> key;
  for (const auto& d : r.dimensions()) {
    key.push_back(d.offset);
    key.push_back(d.extent);
  }
  return key;
}
Result<OperationProgram> lower_pyramid(const OperationLowering& q) {
  try {
    const auto& s = *static_cast<const Pyramid*>(q.static_state);
    OperationProgramBuilder b(q);
    ResourceLimits limits;
    limits.maximum_work = q.limits.maximum_work;
    ResourceBudget budget(limits);
    struct Group {
      Region region;
      std::vector<Address> writes;
    };
    std::map<std::vector<std::uint64_t>, Group> groups;
    for (const auto& member : q.requests[0].members) {
      const auto grid = take(TileGrid::from_schema(
          *q.outputs[0].result_schema, q.tile_extent, member.first));
      for (auto tile : member.second) {
        check(budget.consume({1}));
        if ((s.kind == 0 && member.first == 0) || s.levels == 0) {
          const auto task = take(b.add_copy());
          check(b.read_input(task, 0, 0, tile));
          check(b.write_output(task, 0, member.first, tile));
          continue;
        }
        const auto region = s.plane_region(take(grid.region(tile)));
        auto& group = groups[key_for(region)];
        group.region = region;
        group.writes.emplace_back(member.first, tile);
      }
    }
    for (const auto& item : groups) {
      const auto task = take(b.add_task("generation", "generation-unchecked-v1",
                                        {}, {s.scratch_bytes, 0, 0}));
      auto bound = s;
      bound.selected = item.second.region;
      for (const auto& read : bound.support(0, {})) {
        const auto grid = take(TileGrid::from_schema(
            *q.inputs[read.port].result_schema, q.tile_extent, read.member));
        for (auto tile : take(sdk::tiles_for_region(budget, grid, read.region)))
          check(b.read_input(task, read.port, read.member, tile));
      }
      for (const auto& write : item.second.writes)
        check(b.write_output(task, 0, write.first, write.second));
    }
    return b.finish();
  } catch (const Status& e) {
    return Result<OperationProgram>(e);
  }
}
struct PyramidKernel {
  std::shared_ptr<Pyramid> state;
  ps_prepared_kernel_v8 inner{};
  ~PyramidKernel() {
    if (inner.release)
      inner.release(inner.context);
  }
};
}  // namespace
OperationDefinition make_pyramid(const std::string& key, int kind,
                                 Profile profile) {
  std::vector<Parameter> p{{"y_axis", Type::Int64}, {"x_axis", Type::Int64}};
  if (kind != 2)
    p.push_back({"levels", Type::Int64});
  p.push_back({"expected_shape", Type::String, false});
  p.push_back({"expected_dtype", Type::String, false});
  auto schema = tensor_schema(ElementType::Float64, {1, 1});
  if (kind != 2) {
    schema.id = kind == 0 ? kGaussian : kLaplacian;
    schema.tensors[0].key = kind == 0 ? "gaussian_0" : "base";
  }
  auto result = generation::operation(
      key, 1, std::move(p), {{kind == 2 ? "output" : "bands", schema}},
      [kind, profile](const auto& in, const auto& p) {
        return std::make_shared<Pyramid>(kind, profile, in, p);
      });
  if (kind == 2) {
    result.traits.input_schema[0].element_type_mask = 0;
    result.traits.input_schema[0].result_schema_id = kLaplacian;
    result.traits.input_schema[0].result_schema_version = 1;
  }
  result.lower = lower_pyramid;
  const auto prepare = result.prepare_kernel;
  result.prepare_kernel =
      [prepare](const auto& desc, const auto& placement,
                const KernelPreparation& query, const auto& resources,
                const auto* native) -> Result<ps_prepared_kernel_v8> {
    try {
      require(!query.writes.empty(), "empty pyramid invocation");
      auto state = std::make_shared<Pyramid>(
          *static_cast<const Pyramid*>(query.static_state));
      state->selected = state->plane_region(
          fixed_tensor::view_region(query.writes[0].layout));
      for (const auto& write : query.writes) {
        require(key_for(state->selected) ==
                    key_for(state->plane_region(
                        fixed_tensor::view_region(write.layout))),
                "pyramid plane group");
        state->bands.insert(write.member);
      }
      auto mapped = query;
      mapped.static_state = state.get();
      auto owner = std::make_unique<PyramidKernel>();
      owner->state = std::move(state);
      owner->inner = take(prepare(desc, placement, mapped, resources, native));
      auto prepared = owner->inner;
      prepared.context = owner.release();
      prepared.invoke_cpu = [](const ps_kernel_invocation_v8* inv,
                               ps_kernel_status_v8* out) {
        const auto& owner =
            *static_cast<const PyramidKernel*>(inv->prepared_context);
        auto mapped = *inv;
        mapped.prepared_context = owner.inner.context;
        owner.inner.invoke_cpu(&mapped, out);
      };
      prepared.release = [](const void* p) {
        delete static_cast<const PyramidKernel*>(p);
      };
      return Result<ps_prepared_kernel_v8>(prepared);
    } catch (const Status& e) {
      return Result<ps_prepared_kernel_v8>(e);
    }
  };
  return result;
}

OperationDefinition make_pyramid_assembly() {
  struct Assembly final : Program {
    Assembly(const std::vector<OperationMetadata>& in, const Parameters& p) {
      require(!in.empty() && in.size() <= 16,
              "Laplacian assembly member count");
      SchemaTemplate schema;
      schema.id = kLaplacian;
      for (unsigned i = 0; i < in.size(); ++i) {
        auto member = tensor(in[i], true);
        member.key =
            i + 1 == in.size() ? "base" : "detail_" + std::to_string(i);
        schema.tensors.push_back(std::move(member));
      }
      check(schema.validate());
      OperationMetadata metadata;
      metadata.result_schema = std::make_shared<const SchemaTemplate>(schema);
      const Pyramid verified(2, Profile::Strict, {metadata}, p);
      add_output(this, std::move(schema));
    }
    void run(const Kernel&, const ps_kernel_invocation_v8&) const override {}
  };
  auto schema = tensor_schema(ElementType::Float64, {1, 1});
  schema.id = kLaplacian;
  schema.tensors[0].key = "base";
  auto def = generation::operation(
      "filter.assemble_laplacian_bands", 0,
      {{"y_axis", Type::Int64},
       {"x_axis", Type::Int64},
       {"expected_shape", Type::String},
       {"expected_dtype", Type::String}},
      {{"bands", schema}},
      [](const auto& in, const auto& p) {
        return std::make_shared<Assembly>(in, p);
      },
      true);
  def.traits.repeated_maximum = 16;
  def.lower = [](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      OperationProgramBuilder b(q);
      for (const auto& member : q.requests[0].members)
        for (auto tile : member.second) {
          const auto task = take(b.add_copy());
          check(b.read_input(task, member.first, 0, tile));
          check(b.write_output(task, 0, member.first, tile));
        }
      return b.finish();
    } catch (const Status& e) {
      return Result<OperationProgram>(e);
    }
  };
  return def;
}
}  // namespace ps::plugin_internal::filter
