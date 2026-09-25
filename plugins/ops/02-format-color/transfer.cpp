#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "02-format-color/transfer_fast.hpp"
#include "02-format-color/transfer_math.hpp"
#include "02-format-color/transfer_prepare.hpp"
#include "02-format-color/transfer_runtime.hpp"
#include "02-format-color/transfer_simd.hpp"
#include "photospider/data/region_runs.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal::transfer_ops {
using data_internal::format_numeric::ExactWorkFailure;
using data_internal::format_numeric::ExactWorkScope;

// Allocated lazily, through the observation's bounded host allocator. An
// alpha-only/copy/identity request creates neither numerical workspace.
template <class T>
class Workspace final {
  MutableBuffer storage_;
  T* value_ = nullptr;

 public:
  Workspace() = default;
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;
  ~Workspace() {
    if (value_) {
      value_->~T();
    }
  }
  template <class... Args>
  Result<T*> get(const BufferAllocator& allocator, Args&&... args) {
    if (!value_) {
      auto allocated = allocator.allocate(sizeof(T));
      if (!allocated.ok()) {
        return Result<T*>(allocated.status());
      }
      storage_ = allocated.take_value();
      static_assert(alignof(T) <= alignof(std::max_align_t));
      value_ = new (storage_.data()) T(std::forward<Args>(args)...);
    }
    return Result<T*>(value_);
  }
};
inline Status validate_sample(const Preparation& state, std::uint64_t bits) {
  const auto p = numeric_ops::BinaryParts::decode(bits, state.narrow);
  if (p.nan || p.infinite) {
    return sample_error("nonfinite participating sample");
  }
  const auto& c = state.program;
  const double x = numeric_double(bits, state.narrow);
  double lo = 0, hi = 1;
  if (c.definition.curve == TransferCurve::Bt1886) {
    if (c.encode) {
      lo = *c.definition.black_luminance;
      hi = *c.definition.white_luminance;
    }
  } else if (c.definition.curve == TransferCurve::Pq) {
    if (c.encode) {
      hi = 10000;
    }
  } else if (c.definition.curve != TransferCurve::HlgOetf) {
    return Status::success();
  }
  return x >= lo && x <= hi
             ? Status::success()
             : sample_error("participating sample outside native domain");
}
class Runner final {
  const Preparation& state_;
  const BufferAllocator& allocator_;
  const std::function<Status(std::uint64_t)>& consume_;
  bool environment_active_;
  Workspace<StrictMath> strict_;
  Workspace<CompactMath> compact_;
  Workspace<FastMath> fast_;

 public:
  NumericDiagnostics diagnostics;
  std::uint64_t failed_lane = 0;
  Runner(const Preparation& state, const BufferAllocator& allocator,
         const std::function<Status(std::uint64_t)>& consume, bool active)
      : state_(state),
        allocator_(allocator),
        consume_(consume),
        environment_active_(active) {
    diagnostics.profile = state.profile == SequenceProfile::Strict
                              ? CpuNumericProfile::Strict
                          : state.profile == SequenceProfile::AppleSilicon
                              ? CpuNumericProfile::AppleSiliconNeon
                              : CpuNumericProfile::X86Avx2;
    std::snprintf(
        diagnostics.implementation.data(), diagnostics.implementation.size(),
        "FMT09/2;real-DAG;SLEEF-3.9.0/"
        "u10;filter=%d;compact=%d;gamma2-simd=%d;%s",
        PHOTOSPIDER_TRANSFER_FAST_MATH, PHOTOSPIDER_TRANSFER_COMPACT_MATH,
        PHOTOSPIDER_TRANSFER_GAMMA2_SIMD,
        numeric_ops::sequence_implementation());
  }
  Status span(const std::uint8_t* source, std::int64_t stride,
              std::uint8_t* target, unsigned count, bool selected) {
    const unsigned width = state_.narrow ? 4 : 8;
    auto status = consume_(count);
    if (!status.ok()) {
      return status;
    }
    if (!selected) {
      if (target) {
        if (stride == width) {
          std::memcpy(target, source, count * width);
        } else {
          for (unsigned i = 0; i < count; ++i) {
            std::memcpy(target + i * width,
                        source + static_cast<std::int64_t>(i) * stride, width);
          }
        }
        diagnostics.copied_elements += count;
      } else {
        diagnostics.view_elements += count;
      }
      return Status::success();
    }
    const auto& curve = state_.program;
    // No DAG arrays, branch gathering or workspace for exact gamma=2. This
    // path still validates every participating semantic sample before SIMD.
    if (curve.definition.curve == TransferCurve::PowerGamma &&
        *curve.definition.gamma == 2 && environment_active_) {
      if (state_.semantic) {
        for (unsigned i = 0; i < count; ++i) {
          failed_lane = i;
          std::uint64_t bits = 0;
          std::memcpy(&bits, source + static_cast<std::int64_t>(i) * stride,
                      width);
          ++diagnostics.evaluated_values;
          // Power-gamma has no bounded native input interval. Avoid creating
          // and assigning the owning Status object for every valid sample.
          const auto infinity = state_.narrow ? UINT64_C(0x7f800000)
                                              : UINT64_C(0x7ff0000000000000);
          if ((bits & ~sign_mask(state_.narrow)) >= infinity) {
            return sample_error("nonfinite participating sample");
          }
        }
      } else {
        diagnostics.evaluated_values += count;
      }
#if PHOTOSPIDER_TRANSFER_GAMMA2_SIMD
      if (target && stride == width &&
          numeric_ops::accelerated_math_available()) {
        gamma2_simd(source, target, count, state_.narrow, curve.encode);
        if (state_.semantic && !curve.encode) {
          // Signed power-gamma accepts finite values outside [0,1]. Squaring
          // such a value may overflow; semantic output must remain finite.
          const auto infinity = state_.narrow ? UINT64_C(0x7f800000)
                                              : UINT64_C(0x7ff0000000000000);
          for (unsigned i = 0; i < count; ++i) {
            failed_lane = i;
            std::uint64_t bits = 0;
            std::memcpy(&bits, target + i * width, width);
            if ((bits & ~sign_mask(state_.narrow)) >= infinity) {
              return sample_error("nonfinite rounded result",
                                  FailureReason::ArithmeticOverflow);
            }
          }
        }
        return Status::success();
      }
#endif
      for (unsigned i = 0; i < count; ++i) {
        failed_lane = i;
        std::uint64_t bits = 0;
        std::memcpy(&bits, source + static_cast<std::int64_t>(i) * stride,
                    width);
        auto direct = nonfinite(curve, bits, state_.narrow);
        if (!direct) {
          const double x = std::abs(numeric_double(bits, state_.narrow));
          if (state_.narrow) {
            const float f = static_cast<float>(x);
            const float y = curve.encode ? std::sqrt(f) : f * f;
            direct = numeric_bits(y, true);
          } else {
            direct = numeric_bits(curve.encode ? std::sqrt(x) : x * x);
          }
          *direct |= bits & sign_mask(state_.narrow);
        }
        if (state_.semantic) {
          const auto result =
              numeric_ops::BinaryParts::decode(*direct, state_.narrow);
          if (result.nan || result.infinite) {
            return sample_error("nonfinite rounded result",
                                FailureReason::ArithmeticOverflow);
          }
        }
        if (target) {
          std::memcpy(target + i * width, &*direct, width);
        }
      }
      return Status::success();
    }
    std::array<std::uint64_t, 64> bits{}, result{};
    std::array<double, 64> input{};
    std::array<unsigned, 64> branches{};
    std::array<bool, 64> done{};
    const auto& c = state_.program;
    for (unsigned i = 0; i < count; ++i) {
      failed_lane = i;
      std::memcpy(&bits[i], source + static_cast<std::int64_t>(i) * stride,
                  width);
      ++diagnostics.evaluated_values;
      if (state_.semantic) {
        status = validate_sample(state_, bits[i]);
        if (!status.ok()) {
          return status;
        }
      }
      if (c.identity) {
        result[i] = bits[i];
        done[i] = true;
        continue;
      }
      auto direct = nonfinite(c, bits[i], state_.narrow);
      if (!direct) {
        direct = anchor(c, bits[i], state_.narrow);
      }
      input[i] = numeric_double(bits[i], state_.narrow);
      if (c.signed_curve) {
        input[i] = std::abs(input[i]);
      }
      if (!direct && !state_.semantic &&
          c.definition.curve == TransferCurve::Pq && input[i] < 0) {
        direct = nan_bits(state_.narrow);
      }
      if (direct) {
        result[i] = *direct;
        done[i] = true;
        continue;
      }
      branches[i] = c.branch(input[i]);
    }
    for (unsigned branch = 0; branch < c.branch_count; ++branch) {
      std::array<unsigned, 4> indices{};
      std::array<double, 4> operands{};
      unsigned lanes = 0;
      auto batch = [&]() -> Status {
        if (!lanes) {
          return Status::success();
        }
        std::array<std::uint64_t, 4> computed{};
        std::array<bool, 4> accepted{};
        if (PHOTOSPIDER_TRANSFER_FAST_MATH && environment_active_ &&
            numeric_ops::accelerated_math_available()) {
          auto w = fast_.get(allocator_);
          if (!w.ok()) {
            return w.status();
          }
          auto s = w.value()->evaluate(
              c.branches[branch], branch, operands.data(), lanes, state_.narrow,
              state_.profile == SequenceProfile::Strict, computed.data(),
              accepted.data(), consume_, environment_active_);
          if (!s.ok()) {
            return s;
          }
        }
        for (unsigned lane = 0; lane < lanes; ++lane) {
          const auto index = indices[lane];
          failed_lane = index;
          if (!accepted[lane]) {
            if (state_.profile != SequenceProfile::Strict) {
              ++diagnostics.strict_fallbacks;
              ++diagnostics.fallback_reasons[static_cast<unsigned>(
                  NumericFallbackReason::RoundingUnresolved)];
            }
            ++diagnostics.strict_math_calls;
            auto evaluate = [&]() -> Result<std::uint64_t> {
#if PHOTOSPIDER_TRANSFER_COMPACT_MATH
              const auto& program = c.branches[branch];
              // The compact tier has no algebraic fallback. Tiny rational
              // toes lose their input at its initial 128-bit precision and
              // otherwise pay an exception before repeating the wide path.
              // Final sqrt already requires that same wide algebraic path.
              const bool wide_algebraic =
                  program.final_sqrt ||
                  (program.rational && std::abs(operands[lane]) < 0x1p-128);
              if (!wide_algebraic && std::isfinite(operands[lane])) {
                try {
                  auto w = compact_.get(allocator_, SequenceProfile::Strict);
                  if (!w.ok())
                    return Result<std::uint64_t>(w.status());
                  return w.value()->evaluate(program, operands[lane],
                                             state_.narrow, consume_);
                } catch (const numeric_ops::DirectedCapacityRetry&) {
                  // Never catch resource, cancellation or Stale statuses here.
                }
              }
#endif
              auto w = strict_.get(allocator_, SequenceProfile::Strict);
              if (!w.ok())
                return Result<std::uint64_t>(w.status());
              return w.value()->evaluate(c.branches[branch], operands[lane],
                                         state_.narrow, consume_);
            };
            auto evaluated = evaluate();
            if (!evaluated.ok())
              return evaluated.status();
            computed[lane] = evaluated.value();
          }
          result[index] = computed[lane];
          if (c.signed_curve)
            result[index] |= bits[index] & sign_mask(state_.narrow);
        }
        lanes = 0;
        return Status::success();
      };
      for (unsigned i = 0; i < count; ++i) {
        if (!done[i] && branches[i] == branch) {
          indices[lanes] = i;
          operands[lanes++] = input[i];
          if (lanes == indices.size()) {
            status = batch();
            if (!status.ok()) {
              return status;
            }
          }
        }
      }
      status = batch();
      if (!status.ok()) {
        return status;
      }
    }
    for (unsigned i = 0; i < count; ++i) {
      failed_lane = i;
      if (state_.semantic) {
        const auto p =
            numeric_ops::BinaryParts::decode(result[i], state_.narrow);
        if (p.infinite || p.nan) {
          return sample_error("nonfinite rounded result",
                              FailureReason::ArithmeticOverflow);
        }
      }
      if (target) {
        std::memcpy(target + i * width, &result[i], width);
      }
    }
    if (c.identity) {
      if (target) {
        diagnostics.copied_elements += count;
      } else {
        diagnostics.view_elements += count;
      }
    }
    return Status::success();
  }
};
inline std::optional<Region> intersection(const Region& a, const Region& b) {
  std::vector<RegionDimension> dimensions;
  for (std::size_t i = 0; i < a.rank(); ++i) {
    const auto x = a.dimensions()[i], y = b.dimensions()[i];
    const auto begin = std::max(x.offset, y.offset),
               end = std::min(x.offset + x.extent, y.offset + y.extent);
    if (begin >= end) {
      return {};
    }
    dimensions.push_back({begin, end - begin});
  }
  return Region(std::move(dimensions));
}
inline Status located(Status status, const std::vector<std::uint64_t>& at) {
  if (status.code == ErrorCode::OperationFailed) {
    status.message += " coordinate=[";
    for (std::size_t i = 0; i < at.size(); ++i) {
      if (i) {
        status.message += ',';
      }
      status.message += std::to_string(at[i]);
    }
    status.message += ']';
  }
  return status;
}
Result<ValueFragments> publish(const DependencyPhase& phase,
                               const Preparation& state, Runner& runner) {
  using R = Result<ValueFragments>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  std::uint64_t facet_bytes = 0;
  for (const auto& f : facets) {
    facet_bytes += f.key.size() + f.payload.size() + sizeof(ValueFacet);
  }
  numeric_ops::ArrayPublication publication(
      phase.query.outputs.boxes().size(), descriptor.shape.size(), facet_bytes);
  std::vector<Value> values;
  const auto width = state.narrow ? 4U : 8U;
  for (const auto& box : phase.query.outputs.boxes()) {
    const bool view = state.program.identity && !state.materialize;
    std::optional<MutableValue> writer;
    if (!view) {
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok()) {
        return R(allocated.status());
      }
      writer.emplace(allocated.take_value());
    }
    for (const auto& fragment : phase.inputs[0].fragments()) {
      auto overlap = intersection(fragment.region(), box);
      if (!overlap) {
        continue;
      }
      const auto& region = *overlap;
      std::vector<std::uint64_t> at(region.rank());
      auto status = visit_value_runs(
          fragment, region, box, 64, state.all ? std::nullopt : state.axis,
          [&](const ValueReadRun& run) {
            if (state.axis && !state.all) {
              region_run_coordinate(region, run.logical_element, &at);
            }
            const bool selected =
                state.selected(state.axis ? at[*state.axis] : 0);
            auto s = runner.span(
                run.data, run.stride_bytes,
                writer ? writer->data() + run.destination_element * width
                       : nullptr,
                static_cast<unsigned>(run.samples), selected);
            if (!s.ok()) {
              region_run_coordinate(
                  region, run.logical_element + runner.failed_lane, &at);
              return located(s, at);
            }
            return s;
          });
      if (!status.ok()) {
        return R(status);
      }
      if (view) {
        auto v = fragment.view(region);
        if (!v.ok()) {
          return R(v.status());
        }
        auto mapped = Value::from_storage(
            descriptor, region, v.value().layout(), v.value().storage(), facets,
            phase.query.resources);
        if (!mapped.ok()) {
          return R(mapped.status());
        }
        auto retained = publication.retain(mapped.take_value());
        if (!retained.ok()) {
          return R(retained.status());
        }
        values.push_back(retained.take_value());
      }
    }
    if (writer) {
      auto v = std::move(*writer).publish(facets, phase.query.resources);
      if (!v.ok()) {
        return R(v.status());
      }
      auto retained = publication.retain(v.take_value());
      if (!retained.ok()) {
        return R(retained.status());
      }
      values.push_back(retained.take_value());
    }
  }
  return publication.finish(descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, facets,
                            phase.query.resources);
}
struct Continuation final {
  const Preparation* state;
  bool requested = false;
  explicit Continuation(const Preparation* s) : state(s) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using R = Result<DependencyPoll>;
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return R(std::move(batch));
    }
    input_internal::Float32Environment environment;
    if (!environment.active()) {
      return R(sample_error("floating environment unavailable"));
    }
    const std::function<Status(std::uint64_t)> consume = [&](std::uint64_t n) {
      if (phase.query.cancellation.cancelled()) {
        return Status{ErrorCode::Cancelled, "FMT-09 cancelled"};
      }
      return phase.consume_work(n);
    };
    ExactWorkScope exact(&consume, &phase.query.cancellation);
    Runner runner(*state, phase.allocator, consume, environment.active());
    auto result = [&]() -> Result<ValueFragments> {
      try {
        return publish(phase, *state, runner);
      } catch (const ExactWorkFailure& e) {
        return Result<ValueFragments>(e.status);
      }
    }();
    if (phase.report_numeric) {
      auto s = phase.report_numeric(runner.diagnostics);
      if (!s.ok()) {
        return R(s);
      }
    }
    return result.ok() ? R(result.take_value()) : R(result.status());
  }
};
Status planar_run(const PlanarOperationInvocation& call,
                  const Preparation& state, Runner& runner) {
  const auto& layout = *call.output_metadata.planar_layout;
  const auto& dimensions = call.output_region.dimensions();
  const auto height = layout.height_axis, width_axis = layout.width_axis;
  const auto channels = layout.channel_axis;
  std::vector<std::uint64_t> at(dimensions.size());
  for (std::size_t i = 0; i < at.size(); ++i) {
    at[i] = dimensions[i].offset;
  }
  const auto begin = channels ? dimensions[*channels].offset : 0;
  const auto end = channels ? begin + dimensions[*channels].extent : 1;
  const unsigned width = state.narrow ? 4 : 8;
  for (auto channel = begin; channel < end; ++channel) {
    if (channels) {
      at[*channels] = channel;
    }
    for (auto row = dimensions[height].offset;
         row < dimensions[height].offset + dimensions[height].extent;) {
      at[height] = row;
      auto advanced =
          dimensions[height].offset + dimensions[height].extent - row;
      for (auto column = dimensions[width_axis].offset;
           column <
           dimensions[width_axis].offset + dimensions[width_axis].extent;) {
        at[height] = row;
        at[width_axis] = column;
        auto read = call.inputs[0].rectangle_run(at);
        if (!read.ok()) {
          return read.status();
        }
        auto write = call.output.rectangle_run(at);
        if (!write.ok()) {
          return write.status();
        }
        const auto& src = read.value();
        const auto& dst = write.value();
        const auto rows = std::min({src.rows, dst.rows, advanced});
        advanced = rows;
        const auto samples = std::min(src.row.samples, dst.row.samples);
        if (!rows || !samples) {
          return Status{ErrorCode::Internal, "FMT-09 empty rectangle run"};
        }
        for (std::uint64_t dy = 0; dy < rows; ++dy) {
          at[height] = row + dy;
          const auto* source = src.row.data + dy * src.row_stride_bytes;
          auto* target = dst.row.data + dy * dst.row_stride_bytes;
          const bool split_selection =
              state.axis && *state.axis == width_axis && !state.all;
          const bool selected =
              state.selected(state.axis ? at[*state.axis] : 0);
          // Copies need no DAG lane arrays. Amortize host checkpoints over at
          // most 1024 authorized entries, retaining the contract's stop bound.
          const unsigned batch = !split_selection && !selected ? 1024 : 64;
          for (std::uint64_t dx = 0; dx < samples; dx += batch) {
            at[width_axis] = column + dx;
            const auto n = static_cast<unsigned>(
                std::min<std::uint64_t>(batch, samples - dx));
            // A raw selection may use a spatial axis rather than the physical
            // channel axis. Split only that unusual case, without peer reads.
            if (split_selection) {
              for (unsigned i = 0; i < n; ++i) {
                auto s = runner.span(source + (dx + i) * width, width,
                                     target + (dx + i) * width, 1,
                                     state.selected(column + dx + i));
                if (!s.ok()) {
                  at[width_axis] += i;
                  return located(s, at);
                }
              }
            } else {
              auto s = runner.span(source + dx * width, width,
                                   target + dx * width, n, selected);
              if (!s.ok()) {
                at[width_axis] += runner.failed_lane;
                return located(s, at);
              }
            }
          }
        }
        column += samples;
      }
      row += advanced;
    }
  }
  return Status::success();
}
Status planar(const PlanarOperationInvocation& call) {
  input_internal::Float32Environment environment;
  if (!environment.active()) {
    return sample_error("floating environment unavailable");
  }
  if (!call.prepared || !call.prepared->state()) {
    return Status{ErrorCode::Internal, "FMT-09 preparation missing"};
  }
  const auto& state = *static_cast<const Preparation*>(call.prepared->state());
  if (!call.consume_work) {
    return Status{ErrorCode::Internal, "FMT-09 planar work service missing"};
  }
  const auto& consume = call.consume_work;
  ExactWorkScope exact(&consume, &call.cancellation);
  Runner runner(state, call.allocator, consume, environment.active());
  Status result;
  try {
    result = planar_run(call, state, runner);
  } catch (const ExactWorkFailure& e) {
    result = e.status;
  }
  if (call.report_numeric) {
    auto s = call.report_numeric(runner.diagnostics);
    if (!s.ok()) {
      return s;
    }
  }
  return result;
}
OperationDefinition operation(bool encode, SequenceProfile profile,
                              const char* suffix) {
  OperationDefinition d;
  d.key =
      std::string("color.transfer_") + (encode ? "encode" : "decode") + suffix;
  auto& t = d.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  t.planar_storage_capable = true;
  t.cacheable = false;
  t.requires_metadata_specialization = true;
  t.workspace_bytes =
      sizeof(StrictMath) + sizeof(FastMath) +
      (PHOTOSPIDER_TRANSFER_COMPACT_MATH ? sizeof(CompactMath) : 0);
  t.parameter_schema = {
      {"metadata_mode", OperationParameterType::String, false},
      {"group", OperationParameterType::String, false},
      {"components", OperationParameterType::String, false},
      {"axis", OperationParameterType::Int64, false},
      {"curve", OperationParameterType::String, false},
      {"gamma", OperationParameterType::Float64, false},
      {"coefficient_variant", OperationParameterType::String, false},
      {"black_luminance", OperationParameterType::Float64, false},
      {"white_luminance", OperationParameterType::Float64, false},
      {"metadata_override", OperationParameterType::String, false},
      {"layout", OperationParameterType::String, false}};
  auto& out = t.outputs[0];
  out.key = "values";
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 1;
  out.continuation_bytes = sizeof(Continuation);
  out.maximum_dependency_stages = 2;
  d.prepare_static = [encode, profile](const auto& inputs, const auto& params) {
    return prepare(inputs, params, encode, profile);
  };
  d.start_dependency = [](const DependencyQuery& query,
                          const BufferAllocator& allocator) {
    return DependencyContinuation::make<Continuation>(
        allocator, static_cast<const Preparation*>(query.prepared->state()));
  };
  d.planar_callback = planar;
  return d;
}
}  // namespace ps::plugin_internal::transfer_ops

namespace ps::plugin_internal {
Status register_transfer_operations(OperationRegistry* registry) {
  using namespace transfer_ops;  // NOLINT(build/namespaces)
  for (const auto& p :
       {std::pair{"_strict", SequenceProfile::Strict},
        {"_accelerated_apple_silicon", SequenceProfile::AppleSilicon},
        {"_accelerated_x86_64", SequenceProfile::X86Avx2}}) {
    for (bool encode : {false, true}) {
      auto status =
          registry->register_operation(operation(encode, p.second, p.first));
      if (!status.ok()) {
        return status;
      }
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
