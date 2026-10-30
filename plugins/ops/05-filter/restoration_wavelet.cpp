#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "05-filter/restoration_wavelet_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::restoration {
using namespace generation;  // NOLINT(build/namespaces)
using mask::parameter;
using Parameter = OperationParameterSpec;
using Type = OperationParameterType;
namespace {
constexpr unsigned kSpan = 256;
// Preserve arbitrary byte strides, including unaligned native values. Fast
// spans are bounded by input tile edges and by cancellation checkpoints.
template <class T>
const T* load_span(const mask::Reader& r, unsigned port, unsigned member,
                   const Coordinate& at, unsigned axis, std::uint64_t* n,
                   T* buffer) {
  const auto& v = r.view(port, member, at);
  *n = std::min(*n, v.layout->origin[axis] + v.layout->shape[axis] - at[axis]);
  const auto* base = static_cast<const unsigned char*>(v.allocation) +
                     offset(*v.layout, v.byte_offset, at);
  if (v.layout->strides[axis] == sizeof(T) &&
      reinterpret_cast<std::uintptr_t>(base) % alignof(T) == 0)
    return reinterpret_cast<const T*>(base);
  for (std::uint64_t i = 0; i < *n; ++i)
    buffer[i] = mask::load<T>(base, v.layout->strides[axis], i);
  return buffer;
}
template <class T, class Function>
void rows(const ps_kernel_mutable_view_v8& v, Function function) {
  const auto axis = v.layout->rank - 1;
  Coordinate at{};
  std::uint64_t count = 1;
  for (unsigned a = 0; a < v.layout->rank; ++a) {
    at[a] = v.layout->origin[a];
    if (a != axis)
      count *= v.layout->shape[a];
  }
  for (std::uint64_t row = 0; row < count; ++row) {
    const auto end = v.layout->origin[axis] + v.layout->shape[axis];
    for (at[axis] = v.layout->origin[axis]; at[axis] < end;) {
      auto* base = static_cast<unsigned char*>(v.allocation) +
                   offset(*v.layout, v.byte_offset, at);
      const auto n = function(
          at, axis, std::min<std::uint64_t>(kSpan, end - at[axis]), base);
      at[axis] += n;
    }
    for (unsigned a = axis; a-- > 0;) {
      if (++at[a] < v.layout->origin[a] + v.layout->shape[a])
        break;
      at[a] = v.layout->origin[a];
    }
  }
}
template <class T>
void publish_span(const T* data, void* base, std::ptrdiff_t stride,
                  std::uint64_t n) {
  if (stride == sizeof(T)) {
    std::memcpy(base, data, n * sizeof(T));
  } else {
    for (std::uint64_t i = 0; i < n; ++i)
      mask::save(base, stride, i, data[i]);
  }
}
void poll_cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
std::uint64_t count(const ps_kernel_mutable_view_v8& out, unsigned cost) {
  unsigned __int128 n = cost;
  for (unsigned a = 0; a < out.layout->rank; ++a) {
    n *= out.layout->shape[a];
    if (n > UINT64_MAX)
      throw Status{ErrorCode::ResourceExhausted, "RES03 work overflow",
                   FailureReason::ArithmeticOverflow};
  }
  return static_cast<std::uint64_t>(n);
}
struct Threshold final : Program {
  unsigned levels, member = 0;
  std::uint64_t first;
  bool soft;
  ElementType type;
  Threshold(const std::vector<OperationMetadata>& in, const Parameters& p)
      : first(integer(p, "first_level")), soft(text(p, "mode") == "soft") {
    parameter(text(p, "mode") == "hard" || soft, "wavelet threshold mode");
    parameter(integer(p, "first_level") >= 0, "threshold first_level");
    require(in[0].result_schema != nullptr, "wavelet bands schema");
    const auto& s = *in[0].result_schema;
    check(s.validate());
    const std::string id(s.id);
    require(s.version == 1 &&
                (id == "photospider.filter_bands.haar_mean_lifting_v1" ||
                 id == "photospider.filter_bands.cdf53_float_lifting_v1" ||
                 id == "photospider.filter_bands.stationary_haar_mean_v1") &&
                s.tensors.size() >= 4 && s.tensors.size() <= 16 &&
                (s.tensors.size() - 1) % 3 == 0,
            "threshold complete positive-level wavelet directory");
    levels = (s.tensors.size() - 1) / 3;
    type = s.tensors.back().descriptor.element_type;
    require(floating(type), "threshold native dtype");
    auto expected = s.tensors.back().sample_shape();
    const auto rank = expected.size();
    const auto ya = integer(p, "y_axis"), xa = integer(p, "x_axis");
    const auto height = integer(p, "original_height"),
               input_width = integer(p, "original_width");
    parameter(rank >= 2 && ya >= 0 && xa >= 0 && ya != xa &&
                  static_cast<std::uint64_t>(std::max(ya, xa)) < rank &&
                  height > 0 && input_width > 0,
              "threshold directory axes/original dimensions");
    expected[ya] = height;
    expected[xa] = input_width;
    const bool cdf = id == "photospider.filter_bands.cdf53_float_lifting_v1";
    const bool stationary =
        id == "photospider.filter_bands.stationary_haar_mean_v1";
    for (unsigned l = 0; l < levels; ++l) {
      const auto h = expected[ya], w = expected[xa];
      if (!stationary) {
        require(cdf ? h >= 2 && w >= 2 : h > 1 || w > 1,
                "threshold directory excess levels");
        expected[ya] = (h + 1) / 2;
        expected[xa] = (w + 1) / 2;
      }
      for (unsigned orientation = 0; orientation < 3; ++orientation) {
        auto detail = expected;
        if (cdf) {
          if (orientation != 1)
            detail[ya] = h / 2;
          if (orientation != 0)
            detail[xa] = w / 2;
        }
        require(s.tensors[3 * l + orientation].sample_shape() == detail,
                "threshold detail shape/profile mismatch");
      }
    }
    require(s.tensors.back().sample_shape() == expected,
            "threshold residual shape/profile mismatch");
    for (unsigned m = 0; m < s.tensors.size(); ++m) {
      const auto& t = s.tensors[m];
      const auto key = m == 3 * levels ? "LL"
                                       : std::string(m % 3 == 0   ? "LH_y_"
                                                     : m % 3 == 1 ? "HL_x_"
                                                                  : "HH_") +
                                             std::to_string(m / 3 + 1);
      require(std::string(t.key) == key && t.descriptor.element_type == type &&
                  t.sample_shape().size() == rank &&
                  t.batch_axes.size() == s.tensors.back().batch_axes.size(),
              "threshold directory key/dtype/rank/batch");
      size_product(t.sample_shape());
    }
    const auto& table = tensor(in[1]);
    const auto shape = table.sample_shape();
    const auto total_levels = integer(p, "total_levels");
    require(total_levels > 0 &&
                table.descriptor.element_type == ElementType::Float64 &&
                shape.size() == 2 &&
                shape[0] == static_cast<std::uint64_t>(total_levels) &&
                shape[1] == 3 && first <= shape[0] &&
                levels <= shape[0] - first,
            "thresholds requires Float64 [total_levels,3]");
    add_output(this, s);
    scratch_bytes = 2 * kSpan * width(type);
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    if (member == 3 * levels)
      return {{0, member, q}};
    return {{0, member, q},
            {1, 0, Region({{first + member / 3, 1}, {member % 3, 1}})}};
  }
  template <class T, bool Soft>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    check(work.stage());
    const auto& out = inv.outputs[0];
    work.poll(count(out, 4) + 1);
    const generation::Reader control{k, inv};
    Coordinate at{};
    at[0] = first + member / 3;
    at[1] = member % 3;
    const T threshold = static_cast<T>(control.number(1, 0, at));
    // Only port zero uses this cache: <=16 bands. The threshold control uses
    // the uncached reader above, so the 17th input group cannot index it.
    const mask::Reader r{k, inv};
    if (out.layout->rank == 2 && out.layout->shape[1] <= kSpan &&
        out.layout->strides[1] == sizeof(T)) {
      Coordinate origin{};
      origin[0] = out.layout->origin[0];
      origin[1] = out.layout->origin[1];
      const auto& view = r.view(0, member, origin);
      bool covered = view.layout->strides[1] == sizeof(T);
      for (unsigned axis = 0; axis < 2; ++axis)
        covered &= origin[axis] >= view.layout->origin[axis] &&
                   origin[axis] - view.layout->origin[axis] <=
                       view.layout->shape[axis] &&
                   out.layout->shape[axis] <=
                       view.layout->shape[axis] -
                           (origin[axis] - view.layout->origin[axis]);
      const auto* src =
          static_cast<const unsigned char*>(view.allocation) +
          generation::offset(*view.layout, view.byte_offset, origin);
      auto* dst = static_cast<unsigned char*>(out.allocation) +
                  generation::offset(*out.layout, out.byte_offset, origin);
      covered &=
          reinterpret_cast<std::uintptr_t>(src) % alignof(T) == 0 &&
          reinterpret_cast<std::uintptr_t>(dst) % alignof(T) == 0 &&
          view.layout->strides[0] % static_cast<std::ptrdiff_t>(sizeof(T)) ==
              0 &&
          out.layout->strides[0] % static_cast<std::ptrdiff_t>(sizeof(T)) == 0;
      if (covered) {
        const auto width = out.layout->shape[1];
        for (std::uint64_t y = 0; y < out.layout->shape[0];) {
          const auto height =
              std::min<std::uint64_t>(kSpan / width, out.layout->shape[0] - y);
          poll_cancel(inv);
          threshold_rows<T, Soft>(
              reinterpret_cast<const T*>(src + static_cast<std::ptrdiff_t>(y) *
                                                   view.layout->strides[0]),
              reinterpret_cast<T*>(dst + static_cast<std::ptrdiff_t>(y) *
                                             out.layout->strides[0]),
              height, width,
              view.layout->strides[0] / static_cast<std::ptrdiff_t>(sizeof(T)),
              out.layout->strides[0] / static_cast<std::ptrdiff_t>(sizeof(T)),
              threshold);
          y += height;
        }
        return;
      }
    }
    auto* input = static_cast<T*>(inv.scratch[0].allocation);
    auto* buffer = input + kSpan;
    rows<T>(out, [&](const Coordinate& at, unsigned axis, std::uint64_t n,
                     void* base) {
      poll_cancel(inv);
      const auto* src = load_span(r, 0, member, at, axis, &n, input);
      T* dst = out.layout->strides[axis] == sizeof(T) &&
                       reinterpret_cast<std::uintptr_t>(base) % alignof(T) == 0
                   ? static_cast<T*>(base)
                   : buffer;
      threshold_span<T, Soft>(src, dst, n, threshold);
      if (dst == buffer)
        publish_span(dst, base, out.layout->strides[axis], n);
      return n;
    });
  }
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                ps_kernel_work_v9 service) const {
    const Work work{inv, work_limit, 0, service};
    if (type == ElementType::Float32) {
      if (soft)
        compute<float, true>(k, inv, work);
      else
        compute<float, false>(k, inv, work);
    } else {
      if (soft)
        compute<double, true>(k, inv, work);
      else
        compute<double, false>(k, inv, work);
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    dispatch(k, inv, {});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    dispatch(k, inv.views, inv.work);
  }
};
OperationDefinition threshold_operation() {
  auto prototype = tensor_schema(ElementType::Float64, {1});
  prototype.tensors[0].key = "LL";
  auto def = generation::operation("restoration.threshold_wavelet_details", 2,
                                   {{"mode", Type::String},
                                    {"first_level", Type::Int64},
                                    {"total_levels", Type::Int64},
                                    {"y_axis", Type::Int64},
                                    {"x_axis", Type::Int64},
                                    {"original_height", Type::Int64},
                                    {"original_width", Type::Int64}},
                                   {{"bands", prototype}},
                                   [](const auto& in, const auto& p) {
                                     return std::make_shared<Threshold>(in, p);
                                   });
  // A named residual predicate admits every supported profile; preparation
  // validates the complete directory, including all unrequested members.
  def.traits.input_schema[0].tensor_key = "LL";
  def.traits.outputs[0].output_schema = def.traits.input_schema[0];
  def.lower = [](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& s = *static_cast<const Threshold*>(q.static_state);
      OperationProgramBuilder b(q);
      ResourceLimits limits;
      limits.maximum_work = q.limits.maximum_work;
      ResourceBudget budget(limits);
      for (const auto& m : q.requests[0].members) {
        auto bound = s;
        bound.member = m.first;
        const auto grid = take(TileGrid::from_schema(
            *q.outputs[0].result_schema, q.tile_extent, m.first));
        for (auto tile : m.second) {
          check(budget.consume({1}));
          const bool ll = m.first == 3 * s.levels;
          const auto task =
              ll ? take(b.add_copy())
                 : take(b.add_task("generation", "generation-unchecked-v1", {},
                                   {s.scratch_bytes, 0, 0}));
          for (const auto& read : bound.support(0, take(grid.region(tile)))) {
            const auto g =
                take(TileGrid::from_schema(*q.inputs[read.port].result_schema,
                                           q.tile_extent, read.member));
            for (auto t : take(sdk::tiles_for_region(budget, g, read.region)))
              check(b.read_input(task, read.port, read.member, t));
          }
          check(b.write_output(task, 0, m.first, tile));
        }
      }
      return b.finish();
    } catch (const Status& e) {
      return Result<OperationProgram>(e);
    }
  };
  def = budgeted_operation(std::move(def));
  struct Owner {
    std::shared_ptr<Threshold> state;
    ps_prepared_kernel_v9 inner{};
    ~Owner() {
      if (inner.release)
        inner.release(inner.context);
    }
  };
  const auto prepare = def.prepare_kernel_v9;
  def.prepare_kernel_v9 =
      [prepare](const auto& desc, const auto& place, const KernelPreparation& q,
                const auto& resources,
                const auto* native) -> Result<ps_prepared_kernel_v9> {
    try {
      auto owner = std::make_unique<Owner>();
      owner->state = std::make_shared<Threshold>(
          *static_cast<const Threshold*>(q.static_state));
      owner->state->member = q.writes[0].member;
      auto mapped = q;
      mapped.static_state = owner->state.get();
      owner->inner = take(prepare(desc, place, mapped, resources, native));
      auto out = owner->inner;
      out.context = owner.release();
      out.invoke_cpu = [](const ps_kernel_invocation_v9* inv,
                          ps_kernel_status_v8* status) {
        const auto& owner =
            *static_cast<const Owner*>(inv->views.prepared_context);
        auto mapped = *inv;
        mapped.views.prepared_context = owner.inner.context;
        owner.inner.invoke_cpu(&mapped, status);
      };
      out.release = [](const void* p) { delete static_cast<const Owner*>(p); };
      return Result<ps_prepared_kernel_v9>(out);
    } catch (const Status& e) {
      return Result<ps_prepared_kernel_v9>(e);
    }
  };
  return def;
}

struct Point final : Program {
  bool shift;
  unsigned ya = 0, xa = 1;
  std::uint64_t dy = 0, dx = 0;
  double divisor = 1;
  ElementType type;
  std::vector<std::uint64_t> dims;
  Point(bool permutation, const std::vector<OperationMetadata>& in,
        const Parameters& p)
      : shift(permutation) {
    const auto& t = tensor(in[0], true);
    dims = t.sample_shape();
    type = t.descriptor.element_type;
    require(floating(type), "RES03 native pointwise dtype");
    if (shift) {
      const auto y = integer(p, "y_axis"), x = integer(p, "x_axis");
      parameter(y >= 0 && x >= 0 && y != x &&
                    static_cast<std::uint64_t>(std::max(y, x)) < dims.size(),
                "wrap shift axes");
      ya = y;
      xa = x;
      const auto reduce = [](std::int64_t value, std::uint64_t n) {
        const auto r = static_cast<__int128>(value) % n;
        return static_cast<std::uint64_t>(r < 0 ? r + n : r);
      };
      dy = reduce(integer(p, "shift_y"), dims[ya]);
      dx = reduce(integer(p, "shift_x"), dims[xa]);
      // Helper schema assertions are structural, not payload validation.
      std::string shape;
      for (auto n : dims) {
        if (!shape.empty())
          shape += ',';
        shape += std::to_string(n);
      }
      require(text(p, "expected_shape") == shape &&
                  text(p, "expected_dtype") ==
                      (type == ElementType::Float32 ? "float32" : "float64"),
              "cycle spin source schema mismatch");
    } else {
      const auto& b = tensor(in[1], true);
      require(b.sample_shape() == dims && b.descriptor.element_type == type &&
                  b.batch_axes == t.batch_axes,
              "native accumulation shape/dtype/batch");
      divisor = real(p, "divisor");
    }
    auto schema = *in[0].result_schema;
    schema.id = "photospider.tensor";
    schema.version = 1;
    schema.tensors[0].key = "samples";
    add_output(this, std::move(schema));
    scratch_bytes = 3 * kSpan * width(type);
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    if (!shift)
      return {{0, 0, q}, {1, 0, q}};
    // Complete spatial-plane superset; non-spatial coordinates remain local.
    auto d = q.dimensions();
    d[ya] = {0, dims[ya]};
    d[xa] = {0, dims[xa]};
    return {{0, 0, Region(std::move(d))}};
  }
  template <class T>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    check(work.stage());
    const auto& out = inv.outputs[0];
    work.poll(count(out, shift ? 1 : 3));
    const mask::Reader r{k, inv};
    auto* a = static_cast<T*>(inv.scratch[0].allocation);
    auto* b = a + kSpan;
    auto* result = b + kSpan;
    const T div = static_cast<T>(divisor);
    rows<T>(
        out, [&](Coordinate at, unsigned axis, std::uint64_t n, void* base) {
          poll_cancel(inv);
          if (shift) {
            at[ya] = (at[ya] + dy) % dims[ya];
            at[xa] = (at[xa] + dx) % dims[xa];
            if (axis == ya || axis == xa)
              n = std::min(n, dims[axis] - at[axis]);
            const auto* src = load_span(r, 0, 0, at, axis, &n, a);
            publish_span(src, base, out.layout->strides[axis], n);
          } else {
            const auto* av = load_span(r, 0, 0, at, axis, &n, a);
            const auto* bv = load_span(r, 1, 0, at, axis, &n, b);
            T* dst =
                out.layout->strides[axis] == sizeof(T) &&
                        reinterpret_cast<std::uintptr_t>(base) % alignof(T) == 0
                    ? static_cast<T*>(base)
                    : result;
            accumulate_span(av, bv, dst, n, div);
            if (dst == result)
              publish_span(dst, base, out.layout->strides[axis], n);
          }
          return n;
        });
  }
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                ps_kernel_work_v9 service) const {
    const Work work{inv, work_limit, 0, service};
    if (type == ElementType::Float32)
      compute<float>(k, inv, work);
    else
      compute<double>(k, inv, work);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    dispatch(k, inv, {});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    dispatch(k, inv.views, inv.work);
  }
};
OperationDefinition point_operation(bool shift) {
  std::vector<Parameter> p =
      shift ? std::vector<Parameter>{{"y_axis", Type::Int64},
                                     {"x_axis", Type::Int64},
                                     {"shift_y", Type::Int64},
                                     {"shift_x", Type::Int64},
                                     {"expected_shape", Type::String},
                                     {"expected_dtype", Type::String}}
            : std::vector<Parameter>{{"divisor", Type::Float64}};
  return budgeted_operation(generation::operation(
      shift ? "restoration.wrap_shift" : "restoration.accumulate_native",
      shift ? 1 : 2, std::move(p),
      {{"output", tensor_schema(ElementType::Float64, {1, 1})}},
      [shift](const auto& in, const auto& p) {
        return std::make_shared<Point>(shift, in, p);
      }));
}
}  // namespace
}  // namespace ps::plugin_internal::restoration
namespace ps::plugin_internal {
Status register_restoration_wavelet(OperationRegistry* registry) {
  for (auto def :
       {restoration::threshold_operation(), restoration::point_operation(true),
        restoration::point_operation(false)}) {
    const auto status = registry->register_operation(std::move(def));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
