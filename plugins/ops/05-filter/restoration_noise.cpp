// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "05-filter/restoration_noise_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::restoration {
using namespace generation;  // NOLINT(build/namespaces)
using Reader = mask::Reader;
using Parameter = OperationParameterSpec;
using Type = OperationParameterType;

struct Noise final : Program {
  bool estimator;
  ElementType type;
  unsigned ya = 0, xa = 1;
  std::vector<std::uint64_t> dims;
  std::vector<unsigned> plane_axes;
  std::uint64_t min_samples = 1;
  double gain = 1, offset = 0, read_sigma = 0;
  Region selected;

  Noise(bool estimate, const std::vector<OperationMetadata>& in,
        const Parameters& args)
      : estimator(estimate) {
    collective = true;
    const auto& input = tensor(in.at(0), true);
    dims = input.sample_shape();
    type = input.descriptor.element_type;
    require(floating(type), "RES01 requires Float32/Float64");
    if (estimator) {
      const auto y = integer(args, "y_axis"), x = integer(args, "x_axis");
      mask::parameter(
          dims.size() >= 2 && y >= 0 && x >= 0 && y != x &&
              static_cast<std::uint64_t>(std::max(y, x)) < dims.size(),
          "MAD spatial axes");
      ya = static_cast<unsigned>(y);
      xa = static_cast<unsigned>(x);
      const auto& m = tensor(in.at(1), true);
      require(m.sample_shape() == dims &&
                  m.descriptor.element_type == ElementType::UInt8,
              "MAD mask requires matching shape and UInt8");
      min_samples = static_cast<std::uint64_t>(integer(args, "min_samples"));
      std::vector<std::uint64_t> output;
      for (unsigned axis = 0; axis < dims.size(); ++axis) {
        if (axis != ya && axis != xa) {
          plane_axes.push_back(axis);
          output.push_back(dims[axis]);
        }
      }
      output.push_back(1);
      add_output(this, tensor_schema(ElementType::Float64, output));
      const auto count =
          static_cast<unsigned __int128>(dims[ya] - 1) * (dims[xa] - 1);
      scratch_bytes = mask::bytes((2 * count + 16) * width(type));
    } else {
      gain = real(args, "gain");
      offset = real(args, "offset");
      read_sigma = real(args, "read_sigma");
      const auto policy = text(args, "below_offset");
      mask::parameter(policy == "clamp" || policy == "error",
                      "calibration policy");
      auto schema = *in[0].result_schema;
      schema.id = "photospider.tensor";
      schema.version = 1;
      schema.tensors[0].key = "samples";
      add_output(this, schema);
      add_output(this, schema);
      scratch_bytes = 3 * 128 * width(type);
    }
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    if (!estimator)
      return {{0, 0, selected}};
    if (dims[ya] < 2 || dims[xa] < 2)
      return {};
    std::vector<RegionDimension> range;
    for (auto d : dims)
      range.push_back({0, d});
    for (unsigned a = 0; a < plane_axes.size(); ++a)
      range[plane_axes[a]] = selected.dimensions()[a];
    return {{0, 0, Region(range)}, {1, 0, Region(std::move(range))}};
  }
  template <class T>
  static const T* address(const ps_kernel_const_view_v8& view,
                          const Coordinate& at) {
    return reinterpret_cast<const T*>(
        static_cast<const unsigned char*>(view.allocation) +
        generation::offset(*view.layout, view.byte_offset, at));
  }
  template <class T>
  static bool aligned(const T* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignof(T) == 0;
  }
  template <class T>
  void estimate(const Kernel& k, const ps_kernel_invocation_v8& inv,
                const Work& work) const {
    const Reader r{k, inv};
    auto* residual = static_cast<T*>(inv.scratch[0].allocation);
    const auto capacity = (dims[ya] - 1) * (dims[xa] - 1);
    auto* selection = residual + capacity + 8;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      samples(inv.outputs[o], [&](const Coordinate& output, std::uint64_t) {
        work.poll();
        std::uint64_t pending = 0;
        // Input blocks charge at most 640 units after computation. Flushing
        // at 384 keeps these intervals below 1024 units. Selector callbacks
        // retain their own checkpoints because they also prepay scan blocks.
        const auto charge = [&](std::size_t amount) {
          pending += amount;
          if (pending >= 384) {
            work.poll(pending);
            pending = 0;
          }
        };
        Coordinate at{};
        for (unsigned a = 0; a < plane_axes.size(); ++a)
          at[plane_axes[a]] = output[a];
        std::uint64_t count = 0;
        for (std::uint64_t y = 0; y + 1 < dims[ya]; ++y) {
          at[ya] = y;
          for (std::uint64_t x = 0; x + 1 < dims[xa];) {
            charge(1);
            at[xa] = x;
            auto below = at;
            ++below[ya];
            const auto& mt = r.view(1, 0, at);
            const auto& mb = r.view(1, 0, below);
            const auto mt_end = mt.layout->origin[xa] + mt.layout->shape[xa];
            const auto mb_end = mb.layout->origin[xa] + mb.layout->shape[xa];
            auto n = std::min<std::uint64_t>(
                {64, dims[xa] - 1 - x, mt_end - x - 1, mb_end - x - 1});
            bool all =
                n && mt.layout->strides[xa] == 1 && mb.layout->strides[xa] == 1;
            if (all) {
              const auto* a = address<std::uint8_t>(mt, at);
              const auto* b = address<std::uint8_t>(mb, below);
              for (std::uint64_t j = 0; j <= n; ++j)
                all &= (a[j] & b[j]) != 0;
            }
            if (all) {
              const auto& it = r.view(0, 0, at);
              const auto& ib = r.view(0, 0, below);
              n = std::min<std::uint64_t>(
                  {n, it.layout->origin[xa] + it.layout->shape[xa] - x - 1,
                   ib.layout->origin[xa] + ib.layout->shape[xa] - x - 1});
              if (n && it.layout->strides[xa] == sizeof(T) &&
                  ib.layout->strides[xa] == sizeof(T) &&
                  aligned(address<T>(it, at)) &&
                  aligned(address<T>(ib, below))) {
                residuals(address<T>(it, at), address<T>(ib, below),
                          residual + count, n);
                count += n;
                x += n;
                charge(6 * n);
                continue;
              }
            }
            // Tile boundaries and partially selected blocks use the exact mask
            // predicate before any image load, including signalling NaNs.
            n = std::max<std::uint64_t>(1, n);
            for (std::uint64_t j = 0; j < n; ++j) {
              at[xa] = x + j;
              below[xa] = x + j;
              auto right = at, diagonal = below;
              ++right[xa];
              ++diagonal[xa];
              if (r.raw(1, 0, at) && r.raw(1, 0, right) && r.raw(1, 0, below) &&
                  r.raw(1, 0, diagonal)) {
                const T a = static_cast<T>(r.value(0, 0, at));
                const T b = static_cast<T>(r.value(0, 0, right));
                const T c = static_cast<T>(r.value(0, 0, below));
                const T d = static_cast<T>(r.value(0, 0, diagonal));
                residual[count++] = ((a - b) - (c - d)) * T(0.5);
              }
            }
            x += n;
            charge(10 * n);
          }
        }
        work.poll(pending);
        pending = 0;
        if (!count || count < min_samples)
          throw Status{ErrorCode::OperationFailed,
                       "insufficient selected MAD residuals",
                       FailureReason::InvalidDomain};
        const double result =
            mad(residual, selection, count,
                [&](std::size_t amount) { work.poll(amount); });
        work.poll(pending);
        store(inv.outputs[o], output, bits(result));
      });
    }
  }
  template <class T>
  void calibrated(const Kernel& k, const ps_kernel_invocation_v8& inv,
                  const Work& work) const {
    work.poll();
    const Reader r{k, inv};
    const ps_kernel_mutable_view_v8 *v = nullptr, *s = nullptr;
    for (unsigned o = 0; o < inv.output_count; ++o)
      (k.writes[o].first == 0 ? v : s) = &inv.outputs[o];
    const auto& output = v ? *v : *s;
    const auto axis = output.layout->rank - 1;
    Coordinate at{};
    unsigned __int128 row_count = 1;
    for (unsigned a = 0; a < output.layout->rank; ++a) {
      at[a] = output.layout->origin[a];
      if (a != axis)
        row_count *= output.layout->shape[a];
    }
    const auto sample_count = row_count * output.layout->shape[axis];
    if (sample_count > UINT64_MAX / 6)
      throw Status{ErrorCode::ResourceExhausted,
                   "RES01 pointwise work capacity overflow",
                   FailureReason::ArithmeticOverflow};
    const auto rows = static_cast<std::uint64_t>(row_count);
    const auto count = static_cast<std::uint64_t>(sample_count);
    // This pointwise bound is known from the selected descriptor. Admit it
    // once, then poll cancellation independently within 128-sample spans.
    work.poll(6 * count);
    const auto cancelled = [&] {
      if (inv.cancellation.is_cancelled &&
          inv.cancellation.is_cancelled(inv.cancellation.context))
        throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
    };
    // Fresh result pages favor ordinary stores below 32 MiB per output on
    // the measured Intel runtime. Reused numerical buffers retain their own
    // smaller thresholds. All spans execute on this invocation thread.
    // Order its streaming stores
    // before publication, and also before releasing buffers on cancellation.
    struct StreamGuard {
      bool active;
      ~StreamGuard() {
        if (active)
          stream_fence();
      }
    } guard{v && count >= (UINT64_C(32) << 20) / sizeof(T) &&
            streaming_extent<T>(count, s != nullptr)};
    auto* buffer = static_cast<T*>(inv.scratch[0].allocation);
    const T a = static_cast<T>(gain), b = static_cast<T>(offset);
    const T noise = static_cast<T>(read_sigma), noise2 = noise * noise;
    const auto ptr = [&](const ps_kernel_mutable_view_v8* p) -> T* {
      if (!p)
        return nullptr;
      return reinterpret_cast<T*>(
          static_cast<unsigned char*>(p->allocation) +
          generation::offset(*p->layout, p->byte_offset, at));
    };
    for (std::uint64_t row = 0; row < rows; ++row) {
      const auto end = output.layout->origin[axis] + output.layout->shape[axis];
      for (at[axis] = output.layout->origin[axis]; at[axis] < end;) {
        const auto& input = r.view(0, 0, at);
        const auto n =
            std::min<std::uint64_t>({128, end - at[axis],
                                     input.layout->origin[axis] +
                                         input.layout->shape[axis] - at[axis]});
        cancelled();
        const auto* src = address<T>(input, at);
        auto* vp = ptr(v);
        auto* sp = ptr(s);
        const bool direct = aligned(src) && aligned(vp) && aligned(sp) &&
                            input.layout->strides[axis] == sizeof(T) &&
                            (!v || v->layout->strides[axis] == sizeof(T)) &&
                            (!s || s->layout->strides[axis] == sizeof(T));
        if (direct) {
          variance(src, vp, sp, n, a, b, noise2,
                   guard.active ? StoreMode::DeferredStreaming
                                : StoreMode::Automatic);
        } else {
          for (std::uint64_t j = 0; j < n; ++j)
            buffer[j] = mask::load<T>(src, input.layout->strides[axis], j);
          variance(buffer, v ? buffer + 128 : nullptr,
                   s ? buffer + 256 : nullptr, n, a, b, noise2);
          for (std::uint64_t j = 0; j < n; ++j) {
            if (v)
              mask::save(vp, v->layout->strides[axis], j, buffer[128 + j]);
            if (s)
              mask::save(sp, s->layout->strides[axis], j, buffer[256 + j]);
          }
        }
        at[axis] += n;
      }
      for (unsigned a = axis; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
        at[a] = output.layout->origin[a];
      }
    }
    cancelled();
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    if (estimator) {
      if (type == ElementType::Float32)
        estimate<float>(k, inv, work);
      else
        estimate<double>(k, inv, work);
    } else {
      if (type == ElementType::Float32)
        calibrated<float>(k, inv, work);
      else
        calibrated<double>(k, inv, work);
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    compute(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    compute(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
  }
};

OperationDefinition selected_tasks(OperationDefinition def) {
  def.lower = [](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& state = *static_cast<const Noise*>(q.static_state);
      OperationProgramBuilder b(q);
      ResourceLimits limits;
      limits.maximum_work = q.limits.maximum_work;
      ResourceBudget budget(limits);
      std::map<TileIndex, std::vector<unsigned>> groups;
      for (unsigned p = 0; p < q.requests.size(); ++p)
        for (const auto& m : q.requests[p].members)
          for (auto tile : m.second) {
            check(budget.consume({1}));
            groups[tile].push_back(p);
          }
      for (const auto& entry : groups) {
        const auto grid = take(TileGrid::from_schema(
            *q.outputs[0].result_schema, q.tile_extent, 0));
        auto bound = state;
        bound.selected = take(grid.region(entry.first));
        const auto task =
            take(b.add_task("generation", "generation-unchecked-v1", {},
                            {bound.scratch_bytes, 0, 0}));
        for (const auto& read : bound.support(0, {})) {
          const auto rg = take(TileGrid::from_schema(
              *q.inputs[read.port].result_schema, q.tile_extent, 0));
          for (auto tile : take(sdk::tiles_for_region(budget, rg, read.region)))
            check(b.read_input(task, read.port, 0, tile));
        }
        for (auto p : entry.second)
          check(b.write_output(task, p, 0, entry.first));
      }
      return b.finish();
    } catch (const Status& e) {
      return Result<OperationProgram>(e);
    }
  };
  def = generation::budgeted_operation(std::move(def));
  struct Owner {
    std::shared_ptr<Noise> state;
    ps_prepared_kernel_v9 inner{};
    ~Owner() {
      if (inner.release)
        inner.release(inner.context);
    }
  };
  const auto prepare = def.prepare_kernel_v9;
  def.prepare_kernel_v9 =
      [prepare](const auto& desc, const auto& placement,
                const KernelPreparation& q, const auto& resources,
                const auto* native) -> Result<ps_prepared_kernel_v9> {
    try {
      auto owner = std::make_unique<Owner>();
      owner->state =
          std::make_shared<Noise>(*static_cast<const Noise*>(q.static_state));
      owner->state->selected = fixed_tensor::view_region(q.writes[0].layout);
      auto mapped = q;
      mapped.static_state = owner->state.get();
      owner->inner = take(prepare(desc, placement, mapped, resources, native));
      auto result = owner->inner;
      result.context = owner.release();
      result.invoke_cpu = [](const ps_kernel_invocation_v9* inv,
                             ps_kernel_status_v8* status) {
        const auto& owner =
            *static_cast<const Owner*>(inv->views.prepared_context);
        auto mapped = *inv;
        mapped.views.prepared_context = owner.inner.context;
        owner.inner.invoke_cpu(&mapped, status);
      };
      result.release = [](const void* p) {
        delete static_cast<const Owner*>(p);
      };
      return Result<ps_prepared_kernel_v9>(result);
    } catch (const Status& e) {
      return Result<ps_prepared_kernel_v9>(e);
    }
  };
  return def;
}
OperationDefinition operation(bool estimate) {
  std::vector<Parameter> p;
  std::vector<std::pair<std::string, SchemaTemplate>> outputs;
  if (estimate) {
    p = {{"y_axis", Type::Int64},
         {"x_axis", Type::Int64},
         {"min_samples", Type::Int64}};
    outputs.emplace_back("sigma", tensor_schema(ElementType::Float64, {1}));
  } else {
    p = {{"gain", Type::Float64},
         {"offset", Type::Float64},
         {"read_sigma", Type::Float64},
         {"below_offset", Type::String}};
    outputs.emplace_back("variance", tensor_schema(ElementType::Float64, {1}));
    outputs.emplace_back("sigma", tensor_schema(ElementType::Float64, {1}));
  }
  return selected_tasks(generation::operation(
      estimate ? "restoration.noise_mad_highpass"
               : "restoration.calibrated_noise_variance",
      estimate ? 2 : 1, std::move(p), outputs,
      [estimate](const auto& in, const auto& args) {
        return std::make_shared<Noise>(estimate, in, args);
      }));
}
}  // namespace ps::plugin_internal::restoration
namespace ps::plugin_internal {
Status register_restoration(OperationRegistry* registry) {
  for (bool estimate : {true, false}) {
    const auto result =
        registry->register_operation(restoration::operation(estimate));
    if (!result.ok())
      return result;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
