// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "05-filter/restoration_nlm_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::restoration {
using namespace generation;  // NOLINT(build/namespaces)
struct Nlm final : Program {
  ElementType type;
  unsigned ya, xa, components;
  int component_axis;
  std::uint64_t patch, search;
  bool corrected;
  double h;
  std::array<double, 4> scales{};
  std::vector<std::uint64_t> dims;
  Nlm(bool correction, const std::vector<OperationMetadata>& in,
      const Parameters& p)
      : corrected(correction) {
    const auto& input = tensor(in.at(0), true);
    const auto& guide = tensor(in.at(1), true);
    dims = input.sample_shape();
    type = input.descriptor.element_type;
    require(floating(type) && guide.descriptor.element_type == type,
            "NLM requires matching Float32/Float64 input and guide");
    const auto y = integer(p, "y_axis"), x = integer(p, "x_axis");
    mask::parameter(
        dims.size() >= 2 && y >= 0 && x >= 0 && y != x &&
            static_cast<std::uint64_t>(std::max(y, x)) < dims.size(),
        "NLM spatial axes");
    ya = static_cast<unsigned>(y);
    xa = static_cast<unsigned>(x);
    const auto ca = integer(p, "component_axis"),
               gc = integer(p, "guide_components");
    mask::parameter(
        ca >= -1 && (ca < 0 || (static_cast<std::uint64_t>(ca) < dims.size() &&
                                ca != y && ca != x)),
        "NLM guide component axis");
    mask::parameter(gc >= 1 && gc <= 4, "NLM guide components must be 1..4");
    component_axis = static_cast<int>(ca);
    components = static_cast<unsigned>(gc);
    auto gd = guide.sample_shape();
    require(gd.size() == dims.size(), "NLM guide rank mismatch");
    if (component_axis >= 0) {
      require(gd[component_axis] == components, "NLM guide component extent");
      gd[component_axis] = dims[component_axis];
    } else {
      require(components == 1, "scalar NLM guide has one component");
    }
    require(gd == dims, "NLM guide spatial/batch shape mismatch");
    // A single column is the same contiguous line after exchanging the
    // isotropic spatial axes. Keep long one-dimensional inputs vectorized.
    if (dims[xa] == 1 && dims[ya] > 1)
      std::swap(ya, xa);
    const auto pr = integer(p, "patch_radius"),
               sr = integer(p, "search_radius");
    mask::parameter(pr >= 0 && sr >= 0, "NLM radii must be nonnegative");
    patch = static_cast<std::uint64_t>(pr);
    search = static_cast<std::uint64_t>(sr);
    if (search && (dims[ya] > static_cast<std::uint64_t>(INT64_MAX / 4) ||
                   dims[xa] > static_cast<std::uint64_t>(INT64_MAX / 4)))
      throw Status{ErrorCode::ResourceExhausted,
                   "NLM coordinate capacity overflow",
                   FailureReason::ArithmeticOverflow};
    if (search && patch > static_cast<std::uint64_t>(INT64_MAX / 4))
      throw Status{ErrorCode::ResourceExhausted,
                   "NLM patch coordinate overflow",
                   FailureReason::ArithmeticOverflow};
    h = real(p, "h");
    std::string values = text(p, "metric_scale");
    std::replace(values.begin(), values.end(), ',', ' ');
    std::istringstream stream(values);
    for (unsigned c = 0; c < components; ++c)
      require(static_cast<bool>(stream >> scales[c]),
              "NLM metric_scale length/type");
    std::string extra;
    require(!(stream >> extra), "NLM metric_scale length");
    if (corrected) {
      const auto& sigma = tensor(in.at(2), true);
      require(sigma.descriptor.element_type == ElementType::Float64 &&
                  sigma.sample_shape() == std::vector<std::uint64_t>{1},
              "NLM noise_sigma requires one Float64 scalar");
    }
    auto schema = *in[0].result_schema;
    schema.id = "photospider.tensor";
    schema.version = 1;
    schema.tensors[0].key = "samples";
    add_output(this, std::move(schema));
  }
  nlm::Geometry geometry(const Region& q) const {
    const auto& d = q.dimensions();
    return {static_cast<std::int64_t>(dims[ya]),
            static_cast<std::int64_t>(dims[xa]),
            {static_cast<std::int64_t>(d[ya].offset),
             static_cast<std::int64_t>(d[xa].offset),
             static_cast<std::int64_t>(d[ya].offset + d[ya].extent),
             static_cast<std::int64_t>(d[xa].offset + d[xa].extent)},
            patch,
            search,
            components};
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    if (!search)
      return {{0, 0, q}};
    const auto g = geometry(q);
    const auto i = g.input();
    auto a = q.dimensions();
    a[ya] = {static_cast<std::uint64_t>(i.y0), i.rows()};
    a[xa] = {static_cast<std::uint64_t>(i.x0), i.cols()};
    std::vector<ReadRegion> out{{0, 0, Region(a)}};
    const auto halo = g.guide();
    const auto bounded = nlm::intersect(halo, {0, 0, g.height, g.width});
    a[ya] = {static_cast<std::uint64_t>(bounded.y0), bounded.rows()};
    a[xa] = {static_cast<std::uint64_t>(bounded.x0), bounded.cols()};
    if (component_axis >= 0)
      a[component_axis] = {0, components};
    out.push_back({1, 0, Region(std::move(a))});
    if (corrected)
      out.push_back({2, 0, Region::whole({1})});
    return out;
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    if (!search)
      return 0;
    const auto g = geometry(q);
    const auto i = g.input(), v = g.guide();
    using U = unsigned __int128;
    const U guide_area = U(v.rows()) * v.cols(),
            input_area = U(i.rows()) * i.cols();
    const U output_area = U(g.output.rows()) * g.output.cols();
    const auto checked = [](U value) {
      if (value > static_cast<U>(PTRDIFF_MAX))
        throw Status{ErrorCode::ResourceExhausted,
                     "NLM scratch capacity overflow",
                     FailureReason::ArithmeticOverflow};
      return static_cast<std::uint64_t>(value);
    };
    checked(guide_area);
    return checked(
        (input_area + guide_area * (components + 2) + 2 * output_area) *
        width(type));
  }
  template <class T, class Poll>
  static void gather(const mask::Reader& r, unsigned port, Coordinate at,
                     unsigned ya, unsigned xa, nlm::Rect region, T* dst,
                     std::int64_t height, std::int64_t width, bool reflect,
                     Poll poll) {
    for (auto y = region.y0; y < region.y1; ++y) {
      at[ya] =
          static_cast<std::uint64_t>(reflect ? nlm::reflect(y, height) : y);
      for (auto x = region.x0; x < region.x1;) {
        poll();
        const auto xx = reflect ? nlm::reflect(x, width) : x;
        at[xa] = static_cast<std::uint64_t>(xx);
        const auto& view = r.view(port, 0, at);
        const auto end = view.layout->origin[xa] + view.layout->shape[xa];
        const auto n = static_cast<std::size_t>(std::max<std::int64_t>(
            1, std::min<std::int64_t>(
                   {region.x1 - x, static_cast<std::int64_t>(end) - xx, 256,
                    reflect ? (x >= 0 && x < width ? width - x : 1)
                            : region.x1 - x})));
        auto* target = dst + (y - region.y0) * region.cols() + x - region.x0;
        if (view.layout->strides[xa] == sizeof(T) && n > 1) {
          const auto* source =
              static_cast<const unsigned char*>(view.allocation) +
              offset(*view.layout, view.byte_offset, at);
          std::memcpy(target, source, n * sizeof(T));
        } else {
          for (std::size_t j = 0; j < n; ++j) {
            at[xa] = static_cast<std::uint64_t>(xx) + j;
            const auto bits = r.raw(port, 0, at);
            std::memcpy(target + j, &bits, sizeof(T));
          }
        }
        x += static_cast<std::int64_t>(n);
      }
    }
  }
  template <class T>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    check(work.stage());
    const mask::Reader reader{k, inv};
    const auto& output = inv.outputs[0];
    const auto poll = [&] {
      if (inv.cancellation.is_cancelled &&
          inv.cancellation.is_cancelled(inv.cancellation.context))
        throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
    };
    if (!search) {
      unsigned __int128 count = 1;
      for (unsigned a = 0; a < output.layout->rank; ++a) {
        count *= output.layout->shape[a];
        if (count > UINT64_MAX)
          throw Status{ErrorCode::ResourceExhausted, "NLM copy count overflow",
                       FailureReason::ArithmeticOverflow};
      }
      work.poll(static_cast<std::uint64_t>(count));
      samples(output, [&](const Coordinate& at, std::uint64_t i) {
        if (!(i & 127))
          poll();
        store(output, at, reader.raw(0, 0, at));
      });
      return;
    }
    std::vector<RegionDimension> selected;
    for (unsigned a = 0; a < dims.size(); ++a)
      selected.push_back({output.layout->origin[a], output.layout->shape[a]});
    const auto g = geometry(Region(std::move(selected)));
    const auto i = g.input(), v = g.guide(), o = g.output;
    using U = unsigned __int128;
    const auto bounded = [](U value) {
      if (value > UINT64_MAX)
        throw Status{ErrorCode::ResourceExhausted, "NLM work capacity overflow",
                     FailureReason::ArithmeticOverflow};
      return static_cast<std::uint64_t>(value);
    };
    const auto offsets = bounded(U(g.sy()) * (2 * U(g.sx()) + 1) + g.sx());
    auto patch_work = bounded(U(9) * offsets);
    patch_work = bounded(U(patch_work) * (U(o.rows()) + 2 * patch));
    patch_work = bounded(U(patch_work) * (U(o.cols()) + 2 * patch));
    patch_work =
        bounded(U(patch_work) * (3 * components + 2 * (2 * U(patch) + 1) + 40));
    const auto per_plane =
        bounded(U(i.rows()) * i.cols() + U(v.rows()) * v.cols() * components +
                2 * U(o.rows()) * o.cols() + patch_work);
    std::uint64_t planes = 1;
    Coordinate at{};
    for (unsigned a = 0; a < dims.size(); ++a) {
      at[a] = output.layout->origin[a];
      if (a != ya && a != xa)
        planes = bounded(U(planes) * output.layout->shape[a]);
    }
    const auto admitted = bounded(U(per_plane) * planes);
    work.poll(admitted);
    auto* input = static_cast<T*>(inv.scratch[0].allocation);
    auto* guide = input + i.rows() * i.cols();
    auto* result = guide + components * v.rows() * v.cols();
    auto* scratch = result + o.rows() * o.cols();
    std::array<T, 4> metric{};
    for (unsigned c = 0; c < components; ++c)
      metric[c] = static_cast<T>(scales[c]);
    const T native_h = static_cast<T>(h);
    const T sigma =
        corrected ? static_cast<T>(reader.value(2, 0, Coordinate{})) : T(0);
    for (std::uint64_t plane = 0; plane < static_cast<std::uint64_t>(planes);
         ++plane) {
      gather(reader, 0, at, ya, xa, i, input, g.height, g.width, false, poll);
      for (unsigned c = 0; c < components; ++c) {
        auto pos = at;
        if (component_axis >= 0)
          pos[component_axis] = c;
        gather(reader, 1, pos, ya, xa, v, guide + c * v.rows() * v.cols(),
               g.height, g.width, true, poll);
      }
      nlm::compute(g, nlm::Buffers<T>{input, guide, result, scratch, metric},
                   native_h, sigma, corrected, poll);
      for (auto y = o.y0; y < o.y1; ++y) {
        at[ya] = static_cast<std::uint64_t>(y);
        for (auto x = o.x0; x < o.x1; ++x) {
          if (!((x - o.x0) & 255))
            poll();
          at[xa] = static_cast<std::uint64_t>(x);
          std::uint64_t raw = 0;
          std::memcpy(&raw, result + (y - o.y0) * o.cols() + x - o.x0,
                      sizeof(T));
          store(output, at, raw);
        }
      }
      for (unsigned a = static_cast<unsigned>(dims.size()); a-- > 0;) {
        if (a == ya || a == xa)
          continue;
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
        at[a] = output.layout->origin[a];
      }
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(k, inv, Work{inv, work_limit});
    else
      compute<double>(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    const Work work{inv.views, work_limit, 0, inv.work};
    if (type == ElementType::Float32)
      compute<float>(k, inv.views, work);
    else
      compute<double>(k, inv.views, work);
  }
};
OperationDefinition nlm_operation(bool corrected) {
  using Type = OperationParameterType;
  return budgeted_operation(operation(
      corrected ? "restoration.nlm_noise_corrected" : "restoration.nlm_plain",
      corrected ? 3 : 2,
      {{"y_axis", Type::Int64},
       {"x_axis", Type::Int64},
       {"component_axis", Type::Int64},
       {"guide_components", Type::Int64},
       {"patch_radius", Type::Int64},
       {"search_radius", Type::Int64},
       {"h", Type::Float64},
       {"metric_scale", Type::String}},
      {{"output", tensor_schema(ElementType::Float64, {1})}},
      [corrected](const auto& in, const auto& p) {
        return std::make_shared<Nlm>(corrected, in, p);
      }));
}
}  // namespace ps::plugin_internal::restoration
namespace ps::plugin_internal {
Status register_restoration_nlm(OperationRegistry* registry) {
  for (bool corrected : {false, true}) {
    const auto status =
        registry->register_operation(restoration::nlm_operation(corrected));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
