// Copyright 2026 Photospider contributors
#include "07-grade/gradient_channels.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "07-grade/gradient_channels_math.hpp"
#include "common/domain/icc_validation.hpp"
#include "common/registry.hpp"
#include "photospider/ops/numeric/color_description.hpp"

namespace ps::plugin_internal::grade::gradient_channels {
using namespace generation;  // NOLINT(build/namespaces)
namespace {
std::vector<std::string> array(const std::string& source, unsigned count) {
  std::vector<std::string> result;
  std::size_t i = 0;
  const auto space = [&] {
    while (i < source.size() && (source[i] == ' ' || source[i] == '\n' ||
                                 source[i] == '\r' || source[i] == '\t'))
      ++i;
  };
  const auto token = [&](char c) {
    space();
    require(i < source.size() && source[i++] == c,
            "GRD06C requires a JSON string array");
  };
  token('[');
  for (unsigned c = 0; c < count; ++c) {
    if (c)
      token(',');
    token('"');
    std::string value;
    bool closed = false;
    while (i < source.size()) {
      unsigned char ch = source[i++];
      if (ch == '"') {
        closed = true;
        break;
      }
      if (ch == '\\') {
        require(i < source.size(), "GRD06C truncated JSON escape");
        ch = source[i++];
        if (ch == 'u') {
          unsigned code = 0;
          for (unsigned j = 0; j < 4; ++j) {
            require(i < source.size(), "GRD06C truncated Unicode escape");
            const char h = source[i++];
            const int d = h >= '0' && h <= '9'   ? h - '0'
                          : h >= 'a' && h <= 'f' ? h - 'a' + 10
                          : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                 : -1;
            require(d >= 0, "GRD06C invalid Unicode escape");
            code = 16 * code + d;
          }
          require(code >= 32 && code < 127, "GRD06C enum must be ASCII");
          ch = code;
        } else {
          require(ch == '"' || ch == '\\' || ch == '/',
                  "GRD06C invalid enum escape");
        }
      }
      require(ch >= 32 && value.size() < 32, "GRD06C invalid enum string");
      value.push_back(ch);
    }
    require(closed, "GRD06C unterminated JSON string");
    result.push_back(std::move(value));
  }
  token(']');
  space();
  require(i == source.size(), "GRD06C JSON array trailing data");
  return result;
}
fast_curve::Edge edge(const std::string& name) {
  require(name == "reject" || name == "clamp" || name == "linear_extrapolate",
          "GRD06C invalid boundary policy");
  return name == "clamp"                ? fast_curve::Edge::Clamp
         : name == "linear_extrapolate" ? fast_curve::Edge::Linear
                                        : fast_curve::Edge::Reject;
}
bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](const auto& d) { return !d.extent; });
}
void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context))
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
}
}  // namespace

Configuration::Configuration(const std::vector<OperationMetadata>& input,
                             const Parameters& p) {
  const auto& source = tensor(input.at(0), true);
  input_shape = source.sample_shape();
  input_type = source.descriptor.element_type;
  require(floating(input_type), "GRD06C input requires Float32/64");
  const auto a = integer(p, "axis", -1),
             channel = integer(p, "query_channel", -1);
  require(
      a >= 0 && static_cast<std::uint64_t>(a) < source.descriptor.shape.size(),
      "GRD06C source axis");
  source_axis = a + source.batch_axes.size();
  require(channel >= 0 &&
              static_cast<std::uint64_t>(channel) < input_shape[source_axis],
          "GRD06C query_channel");
  query_channel = channel;
  if (source.layout.spatial)
    require(source.layout.channel_axis == std::optional<std::uint32_t>(a),
            "GRD06C spatial source component axis");
  sample_shape = input_shape;
  sample_shape.erase(sample_shape.begin() + source_axis);
  require(!sample_shape.empty() && sample_shape.size() <= 7,
          "GRD06C sample rank");
  const auto color = take(numeric::resolve_color_tuple(
      take(tensor_description_from_parameter(text(p, "color_description")))));
  require(!color.alpha, "GRD06C does not construct alpha");
  columns = color.components.size();
  require(columns == 3 || columns == 4, "GRD06C destination tuple size");
  for (const auto& component : color.components)
    require(component.unit != "rational_pi",
            "GRD06C requires floating hue units");
  profile = color.interpretation.profile;
  require(input.size() == 3 + 2 * columns, "GRD06C input arity");
  const auto n = integer(p, "count");
  require(n >= 1 && n <= 1048576, "GRD06C count");
  length = n;
  type = dtype(p);
  shape(input[1], {1});
  shape(input[2], {1});
  methods = array(text(p, "methods"), columns);
  curve_edges = p.count("curve_edges")
                    ? array(text(p, "curve_edges"), columns)
                    : std::vector<std::string>(columns, "reject");
  lookup_policy = text(p, "lookup_edge", "clamp");
  lookup_edge = edge(lookup_policy);
  for (unsigned c = 0; c < columns; ++c) {
    require(methods[c] == "linear" || methods[c] == "pchip", "GRD06C method");
    cubic.push_back(methods[c] == "pchip");
    edges.push_back(edge(curve_edges[c]));
    const auto& x = tensor(input[3 + 2 * c], true);
    const auto& y = tensor(input[4 + 2 * c], true);
    require(floating(x.descriptor.element_type) &&
                floating(y.descriptor.element_type) &&
                x.sample_shape().size() == 1 &&
                y.sample_shape() == x.sample_shape(),
            "GRD06C control ranks/dtypes");
    const auto k = x.sample_shape()[0];
    require(k >= 2 && k <= 65536, "GRD06C control count");
    knots.push_back(k);
    max_knots = std::max(max_knots, k);
  }
  auto values_shape = sample_shape;
  values_shape.push_back(columns);
  size_product(values_shape);
  auto values = tensor_schema(type, values_shape);
  values.tensors[0].layout.channel_axis = values_shape.size() - 1;
  auto table = tensor_schema(type, {length, columns});
  auto axis = tensor_schema(ElementType::Float64, {3});
  axis.tensors[0].atomic_trailing_axes = 1;
  for (auto* s : {&values, &table, &axis})
    outputs.push_back({std::make_shared<const SchemaTemplate>(std::move(*s))});
}
std::vector<OperationParameterSpec> parameters() {
  using Type = OperationParameterType;
  return {{"axis", Type::Int64, true},
          {"query_channel", Type::Int64, true},
          {"color_description", Type::String, true},
          {"methods", Type::String, true},
          {"count", Type::Int64, true},
          {"dtype", Type::String, false},
          {"curve_edges", Type::String, false},
          {"lookup_edge", Type::String, false}};
}
Status validate_profile(const Configuration& config,
                        const ResourceSet& resources) {
  if (!config.profile)
    return Status::success();
  auto selected = resources.find(*config.profile);
  if (!selected.ok())
    return selected.status();
  const auto bytes = selected.value().bytes();
  if (bytes.size() < 132 || std::memcmp(bytes.data() + 12, "prtrCMYK", 8))
    return {ErrorCode::InvalidArgument,
            "GRD06C requires an ICC CMYK output profile"};
  ResourceLimits limits;
  limits.maximum_work = 64 * 1024 * 1024;
  ResourceBudget budget(limits);
  return color_internal::validate_icc_resource(
      bytes, budget,
      [&budget](std::uint64_t amount) { return budget.consume({amount}); });
}

struct Compiled final : Program {
  Configuration config;
  Compiled(const std::vector<OperationMetadata>& input, const Parameters& p)
      : config(input, p) {
    outputs = config.outputs;
  }
  std::uint64_t task_scratch(unsigned port, const Region& r) const override {
    if (port == 2 || empty(r))
      return 0;
    return sizeof(fast_curve::Curve) +
           (6 * config.max_knots +
            config.length * r.dimensions().back().extent) *
               8;
  }
  std::vector<ReadRegion> support(unsigned port,
                                  const Region& r) const override {
    if (empty(r))
      return {};
    std::vector<ReadRegion> reads{{1, 0, Region({{0, 1}})}};
    if (config.length > 1)
      reads.push_back({2, 0, Region({{0, 1}})});
    if (port == 2)
      return reads;
    const auto components = r.dimensions().back();
    for (std::uint64_t c = components.offset;
         c < components.offset + components.extent; ++c) {
      reads.push_back({static_cast<unsigned>(3 + 2 * c), 0,
                       Region({{0, config.knots[c]}})});
      reads.push_back({static_cast<unsigned>(4 + 2 * c), 0,
                       Region({{0, config.knots[c]}})});
    }
    if (!port && config.length > 1) {
      auto query = r.dimensions();
      query.pop_back();
      query.insert(query.begin() + config.source_axis,
                   {config.query_channel, 1});
      reads.push_back({0, 0, Region(std::move(query))});
    }
    return reads;
  }
  double* bake(const mask::Reader& reader, void* scratch, unsigned first,
               unsigned nc, double start, double end,
               const ps_kernel_invocation_v8& inv, const Work& work) const {
    auto* curve = new (scratch) fast_curve::Curve;
    auto* x = reinterpret_cast<double*>(curve + 1);
    auto* y = x + config.max_knots;
    auto* workspace = y + config.max_knots;
    auto* table = workspace + 4 * config.max_knots;
    for (unsigned c = 0; c < nc; ++c) {
      const auto channel = first + c;
      const auto k = config.knots[channel];
      work.poll(k * 64 + config.length * 56);
      Coordinate at{};
      for (std::uint64_t j = 0; j < k; ++j) {
        if (!(j % 4096))
          cancel(inv);
        at[0] = j;
        x[j] = reader.value(3 + 2 * channel, 0, at);
        y[j] = reader.value(4 + 2 * channel, 0, at);
      }
      curve->prepare(x, y, k, config.cubic[channel], config.edges[channel],
                     workspace);
      const auto poll = [&] { cancel(inv); };
      if (config.type == ElementType::Float32)
        bake_column<float>(*curve, start, end, config.length, table + c, nc,
                           poll);
      else
        bake_column<double>(*curve, start, end, config.length, table + c, nc,
                            poll);
    }
    return table;
  }
  Coordinate source_at(const Coordinate& at) const {
    Coordinate source{};
    for (unsigned a = 0, b = 0; a < config.input_shape.size(); ++a)
      source[a] = a == config.source_axis ? config.query_channel : at[b++];
    return source;
  }
  template <class Input, class Output>
  bool contiguous(const mask::Reader& reader,
                  const ps_kernel_invocation_v8& inv,
                  const ps_kernel_mutable_view_v8& output,
                  const fast_curve::VectorLut& lut) const {
    const unsigned component = output.layout->rank - 1, line = component - 1;
    const unsigned source_line = line >= config.source_axis ? line + 1 : line;
    const auto nc = output.layout->shape[component];
    if (output.layout->strides[component] != sizeof(Output) ||
        output.layout->strides[line] != nc * sizeof(Output))
      return false;
    Coordinate at{};
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < component; ++a) {
      at[a] = output.layout->origin[a];
      if (a != line)
        rows *= output.layout->shape[a];
    }
    at[component] = output.layout->origin[component];
    const auto end = at[line] + output.layout->shape[line];
    for (std::uint64_t r = 0; r < rows; ++r) {
      for (at[line] = output.layout->origin[line]; at[line] < end;) {
        cancel(inv);
        auto n = std::min<std::uint64_t>(4096, end - at[line]);
        const Input* source = nullptr;
        std::ptrdiff_t step = 1;
        if (config.length > 1) {
          const auto position = source_at(at);
          const auto& input = reader.view(0, 0, position);
          if (input.layout->strides[source_line] % sizeof(Input))
            return false;
          step = input.layout->strides[source_line] /
                 static_cast<std::ptrdiff_t>(sizeof(Input));
          n = std::min(n, input.layout->origin[source_line] +
                              input.layout->shape[source_line] -
                              position[source_line]);
          source = reinterpret_cast<const Input*>(
              static_cast<const unsigned char*>(input.allocation) +
              offset(*input.layout, input.byte_offset, position));
          if (reinterpret_cast<std::uintptr_t>(source) % alignof(Input))
            return false;
        }
        auto* target = reinterpret_cast<Output*>(
            static_cast<unsigned char*>(output.allocation) +
            offset(*output.layout, output.byte_offset, at));
        if (reinterpret_cast<std::uintptr_t>(target) % alignof(Output))
          return false;
        lut.span(source, target, n, step);
        at[line] += n;
      }
      for (unsigned a = line; a-- > 0;) {
        if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
          break;
        at[a] = output.layout->origin[a];
      }
    }
    return true;
  }
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& output = inv.outputs[o];
      const auto port = kernel.writes[o].first;
      std::uint64_t points = 1;
      for (unsigned a = 0; a < output.layout->rank; ++a)
        points *= output.layout->shape[a];
      if (!points)
        continue;
      const double start = reader.value(1, 0, Coordinate{});
      const double end =
          config.length == 1 ? start : reader.value(2, 0, Coordinate{});
      if (port == 2) {
        work.poll(4);
        const std::array<double, 3> axis{
            start, end,
            config.length == 1 ? 0 : (end - start) / (config.length - 1)};
        samples(output, [&](const Coordinate& at, auto) {
          store(output, at, bits(axis[at[0]]));
        });
        continue;
      }
      const unsigned component = output.layout->rank - 1;
      const unsigned first = output.layout->origin[component],
                     nc = output.layout->shape[component];
      auto* table = bake(reader, inv.scratch[0].allocation, first, nc, start,
                         end, inv, work);
      if (port == 1) {
        work.poll(points);
        samples(output, [&](const Coordinate& at, auto i) {
          if (!(i % 4096))
            cancel(inv);
          store(output, at,
                convert(table[at[0] * nc + at[1] - first],
                        config.type == ElementType::Float32));
        });
        continue;
      }
      work.poll(points * 24);
      const fast_curve::VectorLut lut(table, config.length, nc, start, end,
                                      config.lookup_edge);
      const bool q32 = config.input_type == ElementType::Float32;
      const bool packed =
          config.type == ElementType::Float32
              ? (q32 ? contiguous<float, float>(reader, inv, output, lut)
                     : contiguous<double, float>(reader, inv, output, lut))
              : (q32 ? contiguous<float, double>(reader, inv, output, lut)
                     : contiguous<double, double>(reader, inv, output, lut));
      if (packed)
        continue;
      Coordinate at{};
      for (unsigned a = 0; a < output.layout->rank; ++a)
        at[a] = output.layout->origin[a];
      for (std::uint64_t i = 0; i < points / nc; ++i) {
        if (!(i % 4096))
          cancel(inv);
        const auto position = lut.position(
            config.length == 1 ? 0 : reader.value(0, 0, source_at(at)));
        for (unsigned c = 0; c < nc; ++c) {
          at[component] = first + c;
          store(output, at,
                convert(lut.value(position, c),
                        config.type == ElementType::Float32));
        }
        for (unsigned a = component; a-- > 0;) {
          if (++at[a] < output.layout->origin[a] + output.layout->shape[a])
            break;
          at[a] = output.layout->origin[a];
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
OperationDefinition compiled_operation() {
  auto result = budgeted_operation(generation::operation(
      "grade.gradient_map_channels_compiled", 0, parameters(),
      {{"values", tensor_schema(ElementType::Float64, {1, 3})},
       {"table", tensor_schema(ElementType::Float64, {1, 3})},
       {"axis", tensor_schema(ElementType::Float64, {3})}},
      [](const auto& inputs, const auto& p) {
        return std::make_shared<Compiled>(inputs, p);
      },
      true));
  result.traits.repeated_minimum = 9;
  result.traits.repeated_maximum = 11;
  const auto prepare = result.prepare_static;
  result.prepare_static = [prepare](
                              const auto& inputs,
                              const auto& p) -> Result<OperationPreparation> {
    auto r = prepare(inputs, p);
    if (!r.ok())
      return r;
    auto ready = r.take_value();
    const auto& state = *static_cast<const Compiled*>(ready.state.get());
    if (state.config.profile)
      ready.resources.push_back(*state.config.profile);
    return Result<OperationPreparation>(std::move(ready));
  };
  result.validate_resources = [](const void* state,
                                 const ResourceSet& resources) {
    return validate_profile(static_cast<const Compiled*>(state)->config,
                            resources);
  };
  return result;
}
}  // namespace ps::plugin_internal::grade::gradient_channels
namespace ps::plugin_internal {
Status register_grade_gradient_channels(OperationRegistry* registry) {
  auto status = registry->register_operation(
      grade::gradient_channels::compiled_operation());
  if (!status.ok())
    return status;
  return registry->register_operation(
      grade::gradient_channels::combined_operation());
}
}  // namespace ps::plugin_internal
