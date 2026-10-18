#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/numeric_tensor_program.hpp"
#include "common/basic_common.hpp"
#include "common/floating_environment.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "photospider/plugin/whole_stage.hpp"

namespace ps::plugin_internal::basic_result {
using namespace numeric_ops;  // NOLINT(build/namespaces)
using basic_internal::blend;
using basic_internal::finite;
using basic_internal::fraction;
using basic_internal::interpolate;
using basic_internal::require;
enum class Kind { Linear, Monotone, Lut, Smoothstep };
inline bool elementwise(Kind kind) {
  return kind == Kind::Smoothstep;
}
inline double parameter(const ResultProgramQuery& query, const char* key) {
  return std::get<double>(query.parameters.at(key));
}
inline void check_parameters(
    Kind kind, const std::map<std::string, ParameterValue>& params) {
  auto real = [&](const char* key) { return std::get<double>(params.at(key)); };
  if (kind == Kind::Smoothstep) {
    require(real("edge0") < real("edge1"), ErrorCode::InvalidArgument,
            "smoothstep interval must increase");
  } else {
    require(real("domain_min") < real("domain_max"), ErrorCode::InvalidArgument,
            "domain must increase");
    basic_internal::choice(std::get<std::string>(params.at("out_of_domain")),
                           "reject", "clip");
  }
}
inline void field(const ResultTensorSpec& spec) {
  require(spec.descriptor.shape.size() == 2 &&
              (spec.batch_axes.empty() ||
               (spec.batch_axes.size() == 2 && spec.batch_axes[0] == 1 &&
                spec.batch_axes[1] == 1)),
          ErrorCode::TypeMismatch,
          "field requires rank two with no batches or one frame/layer");
}
class Reader final {
 public:
  Reader(const ResultTensorInput& input, const Region& region,
         const CancellationToken& cancellation)
      : window_(math_take(input.acquire(region, cancellation))),
        type_(input.spec().descriptor.element_type) {}
  double read(const std::vector<std::uint64_t>& at) {
    const auto axis = window_.sample_axis();
    auto& row = rows_[at.back() % 4];
    bool hit = row.valid && at[axis] >= row.at[axis] &&
               at[axis] - row.at[axis] < row.run.samples;
    for (std::size_t i = 0; hit && i < at.size(); ++i) {
      if (i != axis && at[i] != row.at[i]) {
        hit = false;
      }
    }
    if (!hit) {
      row.run = math_take(window_.row_run(at));
      std::copy(at.begin(), at.end(), row.at.begin());
      row.valid = true;
    }
    const auto* address =
        row.run.data + static_cast<std::ptrdiff_t>(
                           static_cast<__int128>(at[axis] - row.at[axis]) *
                           row.run.sample_stride_bytes);
    double result;
    if (type_ == ElementType::Float32) {
      float number;
      std::memcpy(&number, address, 4);
      result = number;
    } else {
      std::memcpy(&result, address, 8);
    }
    return finite(result);
  }

 private:
  struct Row {
    std::array<std::uint64_t, 8> at{};
    ResultTensorRun run;
    bool valid = false;
  };
  ResultTensorReadWindow window_;
  ElementType type_;
  std::array<Row, 4> rows_{};
};
inline void store(MathTensorWriter& writer,
                  const std::vector<std::uint64_t>& at, ElementType type,
                  double value) {
  finite(value);
  if (type == ElementType::Float32) {
    require(std::abs(value) <= std::numeric_limits<float>::max(),
            ErrorCode::OperationFailed, "basic output outside dtype range");
    const float number = static_cast<float>(value);
    std::memcpy(writer.address(at), &number, 4);
  } else {
    std::memcpy(writer.address(at), &value, 8);
  }
}
inline bool same_sign(double a, double b) {
  return (a > 0 && b > 0) || (a < 0 && b < 0);
}
inline double end_slope(double h0, double h1, double d0, double d1) {
  const double ratio = h0 / finite(h0 + h1);
  double slope = finite(d0 + finite(ratio * finite(d0 - d1)));
  if (!same_sign(slope, d0)) {
    return 0;
  }
  if (!same_sign(d0, d1) && std::abs(slope / 3) > std::abs(d0)) {
    slope = finite(3 * d0);
  }
  return slope;
}
struct Program final {
  Kind kind;
  explicit Program(Kind kind) : kind(kind) {}
  Footprint input_footprint(const ResultProgramQuery& query,
                            const Footprint& output,
                            const FootprintLimits& limits) const {
    const auto shape = query.inputs[0].result_schema->tensors[0].sample_shape();
    if (output.shape() == shape) {
      return output;
    }
    std::vector<Region> boxes;
    for (const auto& box : output.boxes()) {
      const auto& dims = box.dimensions();
      boxes.emplace_back(
          std::vector<RegionDimension>(dims.end() - shape.size(), dims.end()));
    }
    return math_take(Footprint::from_regions(shape, boxes, limits));
  }
  Status write(const ResultProgramPhase& phase,
               const ResultTensorWriteWindow& output,
               const FootprintLimits& limits) {
    const auto& query = phase.query;
    const auto& schema = *query.output.result_schema;
    const auto& spec = schema.tensors[0];
    const auto input_shape =
        query.inputs[0].result_schema->tensors[0].sample_shape();
    auto source_region = Region::whole(input_shape);
    if (elementwise(kind)) {
      const auto& dims = output.region().dimensions();
      source_region = Region(std::vector<RegionDimension>(
          dims.end() - input_shape.size(), dims.end()));
    }
    Reader source(phase.tensors->at({0, 0}), source_region, query.cancellation);
    MathTensorWriter destination(output);
    const auto samples = math_take(Footprint::from_regions(
        spec.sample_shape(), {output.region()}, limits));
    auto work = [&](std::uint64_t count = 1) {
      math_require(phase.consume_work(count));
    };
    if (elementwise(kind)) {
      const double low = parameter(query, "edge0");
      const double high = parameter(query, "edge1");
      std::vector<std::uint64_t> input_at(input_shape.size());
      return samples.visit(
          [&](const auto& at) {
            work();
            std::copy(at.end() - input_at.size(), at.end(), input_at.begin());
            const double x = source.read(input_at);
            const double t = x <= low    ? 0
                             : x >= high ? 1
                                         : fraction(x, low, high);
            const double result = t * t * (3 - 2 * t);
            store(destination, at, spec.descriptor.element_type, result);
            return Status::success();
          },
          UINT64_MAX, query.cancellation);
    }
    const double a = parameter(query, "domain_min"),
                 b = parameter(query, "domain_max");
    const bool clip =
        std::get<std::string>(query.parameters.at("out_of_domain")) == "clip";
    if (kind == Kind::Lut) {
      const auto table_shape =
          query.inputs[1].result_schema->tensors[0].sample_shape();
      Reader table(phase.tensors->at({1, 0}), Region::whole(table_shape),
                   query.cancellation);
      const auto n = table_shape[0];
      for (std::uint64_t j = 0; j < n; ++j) {
        work();
        table.read({j});
      }
      return samples.visit(
          [&](const auto& at) {
            work();
            const double q = source.read(at);
            require(clip || (q >= a && q <= b), ErrorCode::OperationFailed,
                    "field query outside LUT domain");
            double result;
            if (q <= a) {
              result = table.read({0});
            } else if (q >= b) {
              result = table.read({n - 1});
            } else {
              const double position = fraction(q, a, b) * (n - 1);
              auto lower =
                  std::min(static_cast<std::uint64_t>(position), n - 2);
              auto knot = [&](std::uint64_t j) {
                return interpolate(static_cast<double>(j), 0,
                                   static_cast<double>(n - 1), a, b);
              };
              if (q < knot(lower) && lower > 0) {
                --lower;
              } else if (q > knot(lower + 1) && lower + 2 < n) {
                ++lower;
              }
              const double left = knot(lower), right = knot(lower + 1);
              require(left < right && q >= left && q <= right,
                      ErrorCode::OperationFailed,
                      "LUT coordinate cannot be resolved");
              result = interpolate(q, left, right, table.read({lower}),
                                   table.read({lower + 1}));
            }
            store(destination, at, spec.descriptor.element_type, result);
            return Status::success();
          },
          UINT64_MAX, query.cancellation);
    }
    const auto k = input_shape[0], n = spec.descriptor.shape[0];
    auto x = [&](std::uint64_t i) { return source.read({i, 0}); };
    auto y = [&](std::uint64_t i) { return source.read({i, 1}); };
    for (std::uint64_t i = 0; i < k; ++i) {
      work();
      x(i);
      y(i);
      require(i == 0 || x(i - 1) < x(i), ErrorCode::OperationFailed,
              "curve abscissas must strictly increase");
    }
    require(clip || (a >= x(0) && b <= x(k - 1)), ErrorCode::OperationFailed,
            "curve sampling outside control domain");
    require(k <= UINT64_MAX / 24, ErrorCode::ResourceExhausted,
            "curve scratch size overflow");
    MutableBuffer scratch;
    if (kind == Kind::Monotone) {
      scratch = math_take(phase.resources.allocator().allocate(k * 24));
    }
    auto get = [&](std::uint64_t index) {
      double value;
      std::memcpy(&value, scratch.data() + index * 8, 8);
      return value;
    };
    auto put = [&](std::uint64_t index, double value) {
      finite(value);
      std::memcpy(scratch.data() + index * 8, &value, 8);
    };
    if (kind == Kind::Monotone) {
      for (std::uint64_t i = 0; i + 1 < k; ++i) {
        work();
        const double h = finite(static_cast<double>(x(i + 1)) - x(i));
        const double d = finite((static_cast<double>(y(i + 1)) - y(i)) / h);
        require(h > 0 && (y(i + 1) == y(i) || d != 0),
                ErrorCode::OperationFailed,
                "PCHIP secant cannot be represented");
        put(i, h);
        put(k + i, d);
      }
      put(2 * k, get(k));
      put(3 * k - 1, get(2 * k - 2));
      if (k > 2) {
        put(2 * k, end_slope(get(0), get(1), get(k), get(k + 1)));
        put(3 * k - 1,
            end_slope(get(k - 2), get(k - 3), get(2 * k - 2), get(2 * k - 3)));
        for (std::uint64_t i = 1; i + 1 < k; ++i) {
          work();
          const double d0 = get(k + i - 1), d1 = get(k + i);
          double slope = 0;
          if (same_sign(d0, d1)) {
            const double h = std::max(get(i - 1), get(i));
            const double h0 = get(i - 1) / h, h1 = get(i) / h;
            const double w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
            const double d = std::min(std::abs(d0), std::abs(d1));
            slope =
                finite(std::copysign(d, d0) /
                       ((w1 * (d / std::abs(d0)) + w2 * (d / std::abs(d1))) /
                        (w1 + w2)));
          }
          put(2 * k + i, slope);
        }
      }
    }
    std::uint64_t segment = 0;
    double previous = a;
    for (std::uint64_t i = 0; i < n; ++i) {
      work();
      const double query_point = interpolate(static_cast<double>(i), 0,
                                             static_cast<double>(n - 1), a, b);
      require(i == 0 || query_point > previous, ErrorCode::OperationFailed,
              "curve sampling coordinates collapse");
      previous = query_point;
      double result;
      if (query_point <= x(0)) {
        result = y(0);
      } else if (query_point >= x(k - 1)) {
        result = y(k - 1);
      } else {
        while (segment + 2 < k && query_point > x(segment + 1)) {
          work();
          ++segment;
        }
        const double left = x(segment), right = x(segment + 1), y0 = y(segment),
                     y1 = y(segment + 1);
        if (kind == Kind::Linear || k == 2 || query_point == left ||
            query_point == right) {
          result = interpolate(query_point, left, right, y0, y1);
        } else {
          const double t = fraction(query_point, left, right), u = 1 - t,
                       h = get(segment);
          const double m0 = finite(h * get(2 * k + segment)),
                       m1 = finite(h * get(2 * k + segment + 1));
          result = finite(finite(y0 * (1 + 2 * t) * u * u) +
                          finite(y1 * t * t * (3 - 2 * t)) +
                          finite(m0 * t * u * u) - finite(m1 * t * t * u));
          result = std::clamp(result, std::min(y0, y1), std::max(y0, y1));
        }
      }
      store(destination, {i}, spec.descriptor.element_type, result);
    }
    return phase.consume_work(0);
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto scratch =
        math_take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    FootprintLimits limits;
    limits.consume_work = phase.consume_work;
    limits.cancellation = phase.query.cancellation;
    const auto output = phase.query.tensor_outputs
                            ? *phase.query.tensor_outputs
                            : math_take(Footprint::all(shape, limits));
    auto builder = math_take(ResultBuilder::start(phase.resources, schema,
                                                  phase.query.semantic_key, {},
                                                  phase.query.tile_extent));
    if (!output.empty()) {
      plugin_internal::FloatingEnvironment environment;
      require(environment.active(), ErrorCode::OperationFailed,
              "numeric environment unavailable");
      for (const auto& box : output.boxes()) {
        const auto write_windows = [&](const auto& writers) {
          return math_callback(phase, [&] {
            try {
              for (const auto& writer : writers) {
                auto status = write(phase, writer, limits);
                if (!status.ok()) {
                  return status;
                }
              }
              return phase.consume_work(0);
            } catch (const basic_internal::Failure& failure) {
              return failure.status;
            }
          });
        };
        auto written =
            (kind == Kind::Linear || kind == Kind::Monotone)
                ? sdk::publish_dense_tensor(phase, builder, 0, box,
                                            write_windows,
                                            phase.query.cancellation)
                : builder.publish_tensor_kernel(0, box, write_windows,
                                                phase.query.cancellation);
        math_require(written);
      }
    }
    return Result<ResultProgramPoll>(
        ResultPublication{math_take(builder.seal()), true});
  } catch (const Status& status) {
    return Result<ResultProgramPoll>(status);
  } catch (const basic_internal::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
inline OperationParameterSpec real(
    const char* key, double low = -std::numeric_limits<double>::max(),
    double high = std::numeric_limits<double>::max()) {
  return {key, OperationParameterType::Float64, true, true, low, high};
}
inline OperationParameterSpec natural(const char* key) {
  return {key, OperationParameterType::Int64, true, true, 2, 1048576};
}
inline OperationDefinition operation(const char* key, Kind kind) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = kind == Kind::Lut ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  for (auto& port : traits.input_schema) {
    port.element_type_mask = 12;
  }
  set_whole_tensor_output(traits, ElementType::Float64, sizeof(Program));
  traits.outputs[0].key = "value";
  if (kind == Kind::Smoothstep) {
    traits.outputs[0].output_schema.result_schema_id = "photospider.image";
    traits.outputs[0].output_schema.tensor_key = "pixels";
    traits.outputs[0].result_schema->id = "photospider.image";
    auto& image = traits.outputs[0].result_schema->tensors[0];
    image.key = "pixels";
    image.descriptor = {ElementType::Float32, {1, 1}};
    image.batch_axes = {1, 1};
    image.layout.spatial = true;
    image.layout.channel_axis.reset();
  }
  traits.outputs[0].maximum_dependency_stages = 3;
  traits.requires_metadata_specialization = true;
  if (kind == Kind::Smoothstep) {
    traits.parameter_schema = {real("edge0"), real("edge1")};
  } else {
    traits.parameter_schema = {
        real("domain_min"),
        real("domain_max"),
        {"out_of_domain", OperationParameterType::String, true}};
    if (kind != Kind::Lut) {
      traits.parameter_schema.push_back(natural("count"));
    }
  }
  std::sort(traits.parameter_schema.begin(), traits.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.specialize_metadata = [kind](const auto& inputs,
                                          const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    try {
      check_parameters(kind, parameters);
      for (const auto& input : inputs) {
        require(input.result_schema && input.result_schema->tensors.size() == 1,
                ErrorCode::TypeMismatch,
                "basic operation requires one tensor and no fields");
      }
      const auto& input = inputs[0].result_schema->tensors[0];
      const auto& shape = input.descriptor.shape;
      std::vector<std::uint64_t> output_shape = shape;
      ElementType type = input.descriptor.element_type;
      if (kind == Kind::Linear || kind == Kind::Monotone) {
        require(input.batch_axes.empty() && shape.size() == 2 &&
                    shape[0] >= 2 && shape[1] == 2,
                ErrorCode::TypeMismatch,
                "curve requires generic Kx2 controls, K>=2");
        output_shape = {static_cast<std::uint64_t>(
            std::get<std::int64_t>(parameters.at("count")))};
      } else {
        field(input);
        if (kind == Kind::Lut) {
          const auto& table = inputs[1].result_schema->tensors[0];
          require(table.batch_axes.empty() &&
                      table.descriptor.shape.size() == 1 &&
                      table.descriptor.shape[0] >= 2 &&
                      table.descriptor.shape[0] <= (std::uint64_t{1} << 53),
                  ErrorCode::TypeMismatch,
                  "LUT requires generic representable N>=2");
          require(table.descriptor.element_type == type,
                  ErrorCode::TypeMismatch, "basic inputs must share dtype");
        } else if (kind == Kind::Smoothstep) {
          type = ElementType::Float32;
        }
      }
      auto schema = numeric_tensor_schema(type, output_shape);
      if (kind == Kind::Lut) {
        schema.tensors[0].batch_axes = input.batch_axes;
      }
      if (kind == Kind::Smoothstep) {
        schema.id = "photospider.image";
        auto& tensor = schema.tensors[0];
        tensor.key = "pixels";
        tensor.batch_axes = {1, 1};
        tensor.layout.spatial = true;
        tensor.layout.channel_axis.reset();
        tensor.layout.height_axis = 0;
        tensor.layout.width_axis = 1;
      }
      OperationOutputSpecialization output;
      output.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(schema));
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(output)});
    } catch (const Status& status) {
      return Answer(status);
    } catch (const basic_internal::Failure& failure) {
      return Answer(failure.status);
    }
  };
  definition.map_dependencies =
      [kind](const ResultStaticQuery& compiled) -> Result<ResultStaticMapping> {
    try {
      const auto query = sdk::static_query_view(compiled);
      const auto shape = query.output.result_schema->tensors[0].sample_shape();
      const auto output = query.tensor_outputs
                              ? *query.tensor_outputs
                              : math_take(Footprint::all(shape));
      ResultStaticMapping result;
      Program geometry(kind);
      for (std::uint32_t port = 0; port < (kind == Kind::Lut ? 2U : 1U);
           ++port) {
        const auto& input = query.inputs[port].result_schema->tensors[0];
        auto samples = elementwise(kind)
                           ? geometry.input_footprint(query, output, {})
                           : math_take(Footprint::all(input.sample_shape()));
        samples = math_take(input.close_samples(samples));
        result.inputs.push_back(
            math_take(sdk::tile_need_for_samples(compiled, port, 0, samples)));
      }
      return Result<ResultStaticMapping>(std::move(result));
    } catch (const Status& status) {
      return Result<ResultStaticMapping>(status);
    }
  };
  definition.start_result = [kind](const ResultProgramQuery&,
                                   const BufferAllocator& allocator) {
    return ResultContinuation::make<Program>(allocator, kind);
  };
  if (kind == Kind::Linear || kind == Kind::Monotone)
    return sdk::with_whole_stage(std::move(definition), UINT64_C(1) << 48);
  return definition;
}
}  // namespace ps::plugin_internal::basic_result
