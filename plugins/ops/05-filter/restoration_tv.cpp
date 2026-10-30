// Copyright 2026 Photospider contributors
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "05-filter/restoration_tv_async.hpp"
#include "common/registry.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"
#include "photospider/ops/restoration.hpp"

namespace ps::plugin_internal::restoration {
using namespace generation;  // NOLINT(build/namespaces)
namespace {
std::uint64_t tv_capacity(unsigned __int128 value, bool bytes = false) {
  if (value > (bytes ? static_cast<unsigned __int128>(PTRDIFF_MAX)
                     : static_cast<unsigned __int128>(UINT64_MAX)))
    throw Status{ErrorCode::ResourceExhausted, "TV capacity overflow",
                 FailureReason::ArithmeticOverflow};
  return static_cast<std::uint64_t>(value);
}
struct Tv final : Program {
  ElementType type;
  unsigned ya, xa;
  std::vector<std::uint64_t> dims;
  std::uint64_t iterations, interval;
  std::size_t height, columns, area, count, planes;
  tv::Parameters<double> parameters;
  double atol, rtol;
  bool isotropic, early, identity;
  std::uint64_t round_work, check_work;

  Tv(bool iso, bool stop, const std::vector<OperationMetadata>& in,
     const Parameters& p)
      : isotropic(iso), early(stop) {
    const auto& input = tensor(in.at(0), true);
    type = input.descriptor.element_type;
    dims = input.sample_shape();
    require(floating(type) && dims.size() >= 2 && dims.size() <= 8,
            "TV requires one rank 2..8 Float32/Float64 tensor");
    const auto y = integer(p, "y_axis"), x = integer(p, "x_axis");
    mask::parameter(
        y >= 0 && x >= 0 && y != x &&
            static_cast<std::uint64_t>(std::max(y, x)) < dims.size(),
        "TV spatial axes");
    ya = static_cast<unsigned>(y);
    xa = static_cast<unsigned>(x);
    const auto steps = integer(p, "max_iterations");
    mask::parameter(steps >= 0, "TV max_iterations must be nonnegative");
    iterations = static_cast<std::uint64_t>(steps);
    const auto check = early ? integer(p, "check_interval") : 1;
    mask::parameter(check > 0, "TV check_interval must be positive");
    interval = static_cast<std::uint64_t>(check);
    parameters = {real(p, "lambda"), real(p, "tau"), real(p, "sigma"),
                  real(p, "theta"),  real(p, "dx"),  real(p, "dy")};
    atol = early ? real(p, "atol") : 0;
    rtol = early ? real(p, "rtol") : 0;
    identity = parameters.lambda == 0 || iterations == 0;
    // The two gradient coordinates are symmetric. Exchanging both axes and
    // spacings exposes a contiguous row for a singleton-width input.
    if (dims[xa] == 1 && dims[ya] > 1) {
      std::swap(ya, xa);
      std::swap(parameters.dx, parameters.dy);
    }
    height = dims[ya];
    columns = dims[xa];
    area = tv_capacity(static_cast<unsigned __int128>(height) * columns, true);
    count = size_product(dims);
    planes = count / area;
    using U = unsigned __int128;
    round_work = tv_capacity(U(count) * (isotropic ? 64 : 40));
    check_work = tv_capacity(U(count) * (isotropic ? 128 : 96));
    if (!identity) {
      scratch_bytes =
          tv_capacity(U(count) * (early ? 8 : 5) * width(type), true);
      tv_capacity(U(count) * 6 + U(iterations) * round_work +
                  (early ? U(iterations / interval) * check_work : U(0)));
    }
    auto schema = *in[0].result_schema;
    schema.id = "photospider.tensor";
    schema.version = 1;
    schema.tensors[0].key = "samples";
    add_output(this, std::move(schema));
    collective = !identity;
    whole_inputs = !identity;
  }

  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    return {{0, 0, identity ? q : Region::whole(dims)}};
  }

  template <class T, class Poll>
  void gather(const mask::Reader& reader, T* input, Poll poll) const {
    Coordinate at{};
    for (std::size_t plane = 0; plane < planes; ++plane) {
      for (std::size_t y = 0; y < height; ++y) {
        at[ya] = y;
        for (std::size_t x = 0; x < columns;) {
          poll();
          at[xa] = x;
          const auto& view = reader.view(0, 0, at);
          const auto end = view.layout->origin[xa] + view.layout->shape[xa];
          const auto n = std::min<std::size_t>({end - x, columns - x, 4096});
          auto* dst = input + plane * area + y * columns + x;
          if (view.layout->strides[xa] == sizeof(T)) {
            std::memcpy(dst,
                        static_cast<const unsigned char*>(view.allocation) +
                            offset(*view.layout, view.byte_offset, at),
                        n * sizeof(T));
          } else {
            for (std::size_t j = 0; j < n; ++j) {
              at[xa] = x + j;
              const auto bits = reader.raw(0, 0, at);
              std::memcpy(dst + j, &bits, sizeof(T));
            }
          }
          x += n;
        }
      }
      for (unsigned a = static_cast<unsigned>(dims.size()); a-- > 0;) {
        if (a == ya || a == xa)
          continue;
        if (++at[a] < dims[a])
          break;
        at[a] = 0;
      }
    }
  }

  std::size_t packed_index(const Coordinate& at) const {
    std::size_t plane = 0;
    for (unsigned a = 0; a < dims.size(); ++a)
      if (a != ya && a != xa)
        plane = plane * dims[a] + at[a];
    return plane * area + at[ya] * columns + at[xa];
  }

  template <bool Isotropic, class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    check(work.stage());
    const mask::Reader reader{kernel, inv};
    const auto poll = [&] {
      if (inv.cancellation.is_cancelled &&
          inv.cancellation.is_cancelled(inv.cancellation.context))
        throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
    };
    std::uint64_t outputs = 0;
    for (unsigned v = 0; v < inv.output_count; ++v) {
      std::uint64_t n = 1;
      for (unsigned a = 0; a < inv.outputs[v].layout->rank; ++a)
        n = tv_capacity(static_cast<unsigned __int128>(n) *
                        inv.outputs[v].layout->shape[a]);
      outputs = tv_capacity(static_cast<unsigned __int128>(outputs) + n);
    }
    if (identity) {
      work.poll(outputs);
      for (unsigned v = 0; v < inv.output_count; ++v)
        samples(inv.outputs[v], [&](const Coordinate& at, std::uint64_t i) {
          if (!(i & 255))
            poll();
          store(inv.outputs[v], at, reader.raw(0, 0, at));
        });
      return;
    }
    auto* input = static_cast<T*>(inv.scratch[0].allocation);
    tv::State<T> state{input, input + count, input + 2 * count,
                       input + 3 * count, input + 4 * count};
    const tv::Parameters<T> native{
        static_cast<T>(parameters.lambda), static_cast<T>(parameters.tau),
        static_cast<T>(parameters.sigma),  static_cast<T>(parameters.theta),
        static_cast<T>(parameters.dx),     static_cast<T>(parameters.dy)};
    work.poll(tv_capacity(static_cast<unsigned __int128>(count) * 5));
    gather(reader, input, poll);
    tv::initialize(state, count, poll);
    const auto advance = [&] {
      work.poll(round_work);
      for (std::size_t plane = 0; plane < planes; ++plane) {
        const auto base = plane * area;
        tv::step<Isotropic>(
            tv::State<T>{state.input + base, state.primal + base,
                         state.extrapolated + base, state.px + base,
                         state.py + base},
            height, columns, native, poll);
      }
    };
    const T* result = state.primal;
    using Checker = tv::Checker<Isotropic, T, decltype(poll)>;
    std::unique_ptr<Checker> checker;
    if (early && iterations > interval) {
      try {
        checker = std::make_unique<Checker>(
            input, input + 5 * count, height, columns, planes, native,
            static_cast<T>(atol), static_cast<T>(rtol), poll);
      } catch (const std::system_error&) {
        throw Status{ErrorCode::ResourceExhausted, "TV predicate worker",
                     FailureReason::CapacityLimit};
      }
    }
    for (std::uint64_t round = 1; round <= iterations; ++round) {
      advance();
      if (checker) {
        if (checker->outcome().reason != tv::Reason::Running)
          break;
        if (round < iterations && round % interval == 0)
          checker->trigger(state, round, [&] { work.poll(check_work); });
      }
    }
    if (checker && checker->finish(iterations).reason == tv::Reason::Converged)
      result = checker->result();
    // All borrowed checker storage is now quiescent. Sticky Run failures and
    // cancellation still override either normal termination record.
    work.poll(outputs);
    for (unsigned v = 0; v < inv.output_count; ++v)
      samples(inv.outputs[v], [&](const Coordinate& at, std::uint64_t i) {
        if (!(i & 255))
          poll();
        const auto pos = packed_index(at);
        std::uint64_t raw = 0;
        std::memcpy(&raw, result + pos, sizeof(T));
        store(inv.outputs[v], at, raw);
      });
  }

  void dispatch(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
                const Work& work) const {
    if (type == ElementType::Float32) {
      if (isotropic)
        compute<true, float>(kernel, inv, work);
      else
        compute<false, float>(kernel, inv, work);
    } else {
      if (isotropic)
        compute<true, double>(kernel, inv, work);
      else
        compute<false, double>(kernel, inv, work);
    }
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    dispatch(kernel, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& kernel,
              const ps_kernel_invocation_v9& inv) const override {
    dispatch(kernel, inv.views, Work{inv.views, work_limit, 0, inv.work});
  }
};

const char* tv_key(bool iso, bool early) {
  return iso ? (early ? "restoration.tv_rof_isotropic_early_stop"
                      : "restoration.tv_rof_isotropic")
             : (early ? "restoration.tv_rof_anisotropic_early_stop"
                      : "restoration.tv_rof_anisotropic");
}
OperationDefinition tv_operation(bool iso, bool early) {
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> parameters{
      {"y_axis", Type::Int64},   {"x_axis", Type::Int64},
      {"lambda", Type::Float64}, {"max_iterations", Type::Int64},
      {"tau", Type::Float64},    {"sigma", Type::Float64},
      {"theta", Type::Float64},  {"dx", Type::Float64},
      {"dy", Type::Float64}};
  if (early) {
    parameters.push_back({"check_interval", Type::Int64});
    parameters.push_back({"atol", Type::Float64});
    parameters.push_back({"rtol", Type::Float64});
  }
  return budgeted_operation(operation(
      tv_key(iso, early), 1, std::move(parameters),
      {{"output", tensor_schema(ElementType::Float64, {1, 1})}},
      [iso, early](const auto& in, const auto& p) {
        return std::make_shared<Tv>(iso, early, in, p);
      },
      false, !early));
}
}  // namespace
}  // namespace ps::plugin_internal::restoration

namespace ps::plugin_internal {
Status register_restoration_tv(OperationRegistry* registry) {
  for (bool iso : {true, false})
    for (bool early : {false, true}) {
      const auto status =
          registry->register_operation(restoration::tv_operation(iso, early));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal

namespace ps::restoration {
namespace {
Result<WorkflowInput> append_tv(WorkflowDocument& document, WorkflowInput input,
                                const TvRofOptions& o, bool iso,
                                const TvStoppingOptions* stop) {
  const auto ids = numeric::available_workflow_node_ids(document, 1, {input});
  if (!ids.ok())
    return Result<WorkflowInput>(ids.status());
  std::map<std::string, ParameterValue> parameters{
      {"y_axis", o.y_axis}, {"x_axis", o.x_axis},
      {"lambda", o.lambda}, {"max_iterations", o.max_iterations},
      {"tau", o.tau},       {"sigma", o.sigma},
      {"theta", o.theta},   {"dx", o.dx},
      {"dy", o.dy}};
  if (stop) {
    parameters.emplace("check_interval", stop->check_interval);
    parameters.emplace("atol", stop->atol);
    parameters.emplace("rtol", stop->rtol);
  }
  const auto id = ids.value()[0];
  document.nodes.push_back(
      {id,
       plugin_internal::restoration::tv_key(iso, stop != nullptr),
       {std::move(input)},
       std::move(parameters)});
  return Result<WorkflowInput>(WorkflowNodeOutput{id, "output"});
}
}  // namespace
Result<WorkflowInput> tv_rof_isotropic(WorkflowDocument& d, WorkflowInput input,
                                       const TvRofOptions& o) {
  return append_tv(d, std::move(input), o, true, nullptr);
}
Result<WorkflowInput> tv_rof_anisotropic(WorkflowDocument& d,
                                         WorkflowInput input,
                                         const TvRofOptions& o) {
  return append_tv(d, std::move(input), o, false, nullptr);
}
Result<WorkflowInput> tv_rof_isotropic_early_stop(
    WorkflowDocument& d, WorkflowInput input, const TvRofOptions& o,
    const TvStoppingOptions& stop) {
  return append_tv(d, std::move(input), o, true, &stop);
}
Result<WorkflowInput> tv_rof_anisotropic_early_stop(
    WorkflowDocument& d, WorkflowInput input, const TvRofOptions& o,
    const TvStoppingOptions& stop) {
  return append_tv(d, std::move(input), o, false, &stop);
}
}  // namespace ps::restoration
