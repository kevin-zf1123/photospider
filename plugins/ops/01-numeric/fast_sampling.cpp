// Copyright 2026 Photospider contributors
#include "01-numeric/fast_sampling.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/runtime.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::native_sampling {
using namespace generation;  // NOLINT(build/namespaces)
struct Sampling final : Program {
  unsigned kind, degree = 0;
  std::uint64_t count, knots = 0, start_port;
  ElementType type;
  numeric_ops::ExpressionProgram expression;
  Sampling(unsigned mode, const std::vector<OperationMetadata>& input,
           const Parameters& p)
      : kind(mode) {
    const auto n = integer(p, "count");
    require(n >= 1 && n <= 1048576, "native sampling count");
    count = static_cast<std::uint64_t>(n);
    type = dtype(p);
    start_port = kind == 2 ? 2 : 0;
    require(input.size() >= start_port + 2, "native sampling inputs");
    for (unsigned port = start_port; port < input.size(); ++port) {
      shape(input[port], {1});
    }
    if (kind == 1) {
      expression =
          take(numeric_ops::ExpressionParser(text(p, "expression")).run());
      std::string names;
      for (const auto& name : expression.names) {
        if (!names.empty()) {
          names += ' ';
        }
        names += name;
      }
      require(names == text(p, "coefficient_names") &&
                  input.size() == expression.names.size() + 2,
              "native expression coefficient names/inputs");
      scratch_bytes = (expression.size * fast_sampling::kBatch +
                       2 * fast_sampling::kBatch + expression.names.size()) *
                      sizeof(double);
    } else if (kind == 2) {
      const auto d = integer(p, "degree");
      require(d == 2 || d == 3, "native Bezier degree");
      degree = static_cast<unsigned>(d);
      const auto& anchors = tensor(input[0], true);
      require(floating(anchors.descriptor.element_type) &&
                  anchors.sample_shape().size() == 2 &&
                  anchors.sample_shape()[1] == 2,
              "native Bezier anchors require Float32/64[K,2]");
      knots = anchors.sample_shape()[0];
      require(knots >= 2 && knots <= 65536, "native Bezier knot count");
      const auto& handles = tensor(input[1], true);
      require(floating(handles.descriptor.element_type) &&
                  handles.sample_shape() ==
                      std::vector<std::uint64_t>{knots - 1, degree - 1, 2},
              "native Bezier handles require Float32/64[K-1,degree-1,2]");
      require(text(p, "out_of_domain") == "reject" ||
                  text(p, "out_of_domain") == "clamp",
              "native Bezier domain");
      scratch_bytes = 10 * knots * sizeof(double);
    }
    add_output(this, tensor_schema(type, {count}));
    auto axis = tensor_schema(ElementType::Float64, {3});
    axis.tensors[0].atomic_trailing_axes = 1;
    add_output(this, std::move(axis));
  }
  std::uint64_t task_scratch(unsigned port,
                             const Region& region) const override {
    return port || region.dimensions().empty() || !region.dimensions()[0].extent
               ? 0
               : scratch_bytes;
  }
  std::vector<ReadRegion> support(unsigned port,
                                  const Region& region) const override {
    if (region.dimensions().empty() || !region.dimensions()[0].extent) {
      return {};
    }
    std::vector<ReadRegion> reads{
        {static_cast<unsigned>(start_port), 0, Region({{0, 1}})}};
    if (count > 1) {
      reads.push_back(
          {static_cast<unsigned>(start_port + 1), 0, Region({{0, 1}})});
    }
    if (!port && kind == 1) {
      for (unsigned c = 0; c < expression.names.size(); ++c) {
        reads.push_back({c + 2, 0, Region({{0, 1}})});
      }
    }
    if (!port && kind == 2) {
      reads.push_back({0, 0, Region({{0, knots}, {0, 2}})});
      reads.push_back(
          {1, 0, Region({{0, knots - 1}, {0, degree - 1}, {0, 2}})});
    }
    return reads;
  }
  static void cancel(const ps_kernel_invocation_v8& inv) {
    if (inv.cancellation.is_cancelled &&
        inv.cancellation.is_cancelled(inv.cancellation.context)) {
      throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
    }
  }
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const Reader reader{kernel, inv};
    const double start = reader.number(start_port, 0, Coordinate{});
    const double end =
        count == 1 ? start : reader.number(start_port + 1, 0, Coordinate{});
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      const bool axis = kernel.writes[o].first != 0;
      if (axis) {
        work.poll(4);
        const std::array<double, 3> values{
            start, end, count == 1 ? 0 : (end - start) / (count - 1)};
        samples(output, [&](const Coordinate& at, auto) {
          store(output, at, bits(values[at[0]]));
        });
        continue;
      }
      const auto first = output.layout->origin[0];
      const auto last = first + output.layout->shape[0];
      if (first == last) {
        continue;
      }
      work.poll((last - first) * (kind == 2   ? 2048
                                  : kind == 1 ? expression.size * 8 + 8
                                              : 8));
      auto* scratch =
          kind ? static_cast<double*>(inv.scratch[0].allocation) : nullptr;
      if (kind == 0) {
        auto* destination =
            static_cast<unsigned char*>(output.allocation) + output.byte_offset;
        const unsigned bytes = type == ElementType::Float32 ? 4 : 8;
        if (output.layout->strides[0] == bytes &&
            reinterpret_cast<std::uintptr_t>(destination) % bytes == 0) {
          for (auto i = first; i < last; i += 4096) {
            cancel(inv);
            const auto n = std::min<std::uint64_t>(4096, last - i);
            if (bytes == 4) {
              fast_sampling::coordinates(
                  start, end, count, i,
                  reinterpret_cast<float*>(destination) + i - first, n);
            } else {
              fast_sampling::coordinates(
                  start, end, count, i,
                  reinterpret_cast<double*>(destination) + i - first, n);
            }
          }
          continue;
        }
        for (auto i = first; i < last; ++i) {
          if (!(i % 4096)) {
            cancel(inv);
          }
          Coordinate at{};
          at[0] = i;
          store(output, at,
                convert(fast_sampling::coordinate(start, end, i, count),
                        type == ElementType::Float32));
        }
      } else if (kind == 1) {
        auto* coefficients = scratch;
        auto* queries = coefficients + expression.names.size();
        auto* values = queries + fast_sampling::kBatch;
        auto* arena = values + fast_sampling::kBatch;
        for (unsigned c = 0; c < expression.names.size(); ++c) {
          coefficients[c] = reader.number(c + 2, 0, Coordinate{});
        }
        fast_sampling::Polynomial polynomial;
        if (start >= 0 && start <= 1 && end >= 0 && end <= 1 &&
            fast_sampling::polynomial(expression, coefficients, &polynomial,
                                      arena)) {
          auto* destination = static_cast<unsigned char*>(output.allocation) +
                              output.byte_offset;
          const unsigned width = type == ElementType::Float32 ? 4 : 8;
          if (output.layout->strides[0] == width &&
              reinterpret_cast<std::uintptr_t>(destination) % width == 0) {
            for (auto i = first; i < last; i += 4096) {
              cancel(inv);
              const auto n = std::min<std::uint64_t>(4096, last - i);
              if (width == 4) {
                fast_sampling::sample_polynomial(
                    polynomial, start, end, count, i,
                    reinterpret_cast<float*>(destination) + i - first, n);
              } else {
                fast_sampling::sample_polynomial(
                    polynomial, start, end, count, i,
                    reinterpret_cast<double*>(destination) + i - first, n);
              }
            }
            continue;
          }
        }
        for (auto i = first; i < last; i += fast_sampling::kBatch) {
          cancel(inv);
          const auto n =
              std::min<std::uint64_t>(fast_sampling::kBatch, last - i);
          for (unsigned j = 0; j < n; ++j) {
            queries[j] = fast_sampling::coordinate(start, end, i + j, count);
          }
          fast_sampling::expression(expression, coefficients, queries, values,
                                    n, arena);
          for (unsigned j = 0; j < n; ++j) {
            Coordinate at{};
            at[0] = i + j;
            store(output, at, convert(values[j], type == ElementType::Float32));
          }
        }
      } else {
        work.poll(knots * 16);
        auto* xs = scratch;
        auto* ys = xs + knots;
        auto* cx = ys + knots;
        auto* cy = cx + 4 * knots;
        for (std::uint64_t k = 0; k < knots; ++k) {
          Coordinate at{};
          at[0] = k;
          xs[k] = reader.number(0, 0, at);
          at[1] = 1;
          ys[k] = reader.number(0, 0, at);
        }
        for (std::uint64_t k = 0; k + 1 < knots; ++k) {
          cx[4 * k] = xs[k];
          cy[4 * k] = ys[k];
          cx[4 * k + degree] = xs[k + 1];
          cy[4 * k + degree] = ys[k + 1];
          for (unsigned h = 0; h + 1 < degree; ++h) {
            Coordinate at{};
            at[0] = k;
            at[1] = h;
            const unsigned anchor = h == 0 ? k : k + 1;
            cx[4 * k + h + 1] = xs[anchor] + reader.number(1, 0, at);
            at[2] = 1;
            cy[4 * k + h + 1] = ys[anchor] + reader.number(1, 0, at);
          }
        }
        for (auto i = first; i < last; ++i) {
          if (!(i % 128)) {
            cancel(inv);
          }
          const double query = fast_sampling::coordinate(start, end, i, count);
          double value;
          if (query <= xs[0]) {
            value = ys[0];
          } else if (query >= xs[knots - 1]) {
            value = ys[knots - 1];
          } else {
            const auto k = static_cast<std::uint64_t>(
                std::upper_bound(xs, xs + knots, query) - xs - 1);
            value = fast_sampling::bezier_function(cx + 4 * k, cy + 4 * k,
                                                   degree, query);
          }
          Coordinate at{};
          at[0] = i;
          store(output, at, convert(value, type == ElementType::Float32));
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
OperationDefinition sampling(unsigned mode) {
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> parameters{{"count", Type::Int64, true},
                                                 {"dtype", Type::String, true}};
  if (mode == 1) {
    parameters.push_back({"expression", Type::String, true});
    parameters.push_back({"coefficient_names", Type::String, true});
  } else if (mode == 2) {
    parameters.push_back({"degree", Type::Int64, true});
    parameters.push_back({"out_of_domain", Type::String, true});
  }
  static constexpr const char* names[]{"numeric.linspace_fast",
                                       "numeric.sample_expression_fast",
                                       "curve.sample_bezier_function_fast"};
  return budgeted_operation(operation(
      names[mode], mode == 2 ? 4 : 2, std::move(parameters),
      {{"values", tensor_schema(ElementType::Float64, {1})},
       {"axis", tensor_schema(ElementType::Float64, {3})}},
      [mode](const auto& input, const auto& p) {
        return std::make_shared<Sampling>(mode, input, p);
      },
      mode == 1));
}
}  // namespace ps::plugin_internal::native_sampling
namespace ps::plugin_internal {
Status register_fast_sampling(OperationRegistry* registry) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    const auto status =
        registry->register_operation(native_sampling::sampling(mode));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
