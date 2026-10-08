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

#include "02-format-color/result_mapping.hpp"
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
inline const char* sample_problem(const Preparation& state,
                                  std::uint64_t bits) {
  const auto p = numeric_ops::BinaryParts::decode(bits, state.narrow);
  if (p.nan || p.infinite) {
    return "nonfinite participating sample";
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
    return nullptr;
  }
  return x >= lo && x <= hi ? nullptr
                            : "participating sample outside native domain";
}
class Runner final {
  const Preparation& state_;
  const BufferAllocator& allocator_;
  const core_internal::WorkConsumer& consume_;
  bool environment_active_;
  Workspace<StrictMath> strict_;
  Workspace<CompactMath> compact_;
  Workspace<FastMath> fast_;

 public:
  NumericDiagnostics diagnostics;
  std::uint64_t failed_lane = 0;
  Runner(const Preparation& state, const BufferAllocator& allocator,
         const core_internal::WorkConsumer& consume, bool active)
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
        "FMT09/3;real-DAG;compact-words=8;SLEEF-3.9.0/"
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
    if (curve.identity) {
      unsigned bad = count;
      if (state_.semantic) {
        if (stride == width && PHOTOSPIDER_TRANSFER_GAMMA2_SIMD &&
            numeric_ops::accelerated_math_available()) {
          bad = first_nonfinite(source, count, state_.narrow);
        } else {
          for (unsigned i = 0; i < count; ++i) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, source + static_cast<std::int64_t>(i) * stride,
                        width);
            if ((bits & ~sign_mask(state_.narrow)) >=
                infinity_bits(state_.narrow)) {
              bad = i;
              break;
            }
          }
        }
      }
      diagnostics.evaluated_values += bad == count ? count : bad + 1;
      if (bad != count) {
        failed_lane = bad;
        return sample_error("nonfinite participating sample");
      }
      if (target) {
        if (stride == width) {
          std::memcpy(target, source, count * width);
        } else {
          for (unsigned i = 0; i < count; ++i)
            std::memcpy(target + i * width,
                        source + static_cast<std::int64_t>(i) * stride, width);
        }
        diagnostics.copied_elements += count;
      } else {
        diagnostics.view_elements += count;
      }
      return Status::success();
    }
    // No DAG arrays, branch gathering or workspace for exact gamma=2. This
    // path still validates every participating semantic sample before SIMD.
    if (curve.definition.curve == TransferCurve::PowerGamma &&
        *curve.definition.gamma == 2 && environment_active_) {
      if (state_.semantic) {
        unsigned bad = count;
        if (stride == width && PHOTOSPIDER_TRANSFER_GAMMA2_SIMD &&
            numeric_ops::accelerated_math_available()) {
          bad = first_nonfinite(source, count, state_.narrow);
        } else {
          const auto inf = infinity_bits(state_.narrow);
          for (unsigned i = 0; i < count; ++i) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, source + static_cast<std::int64_t>(i) * stride,
                        width);
            if ((bits & ~sign_mask(state_.narrow)) >= inf) {
              bad = i;
              break;
            }
          }
        }
        diagnostics.evaluated_values += bad == count ? count : bad + 1;
        if (bad != count) {
          failed_lane = bad;
          return sample_error("nonfinite participating sample");
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
          const auto bad = first_nonfinite(target, count, state_.narrow);
          if (bad != count) {
            failed_lane = bad;
            return sample_error("nonfinite rounded result",
                                FailureReason::ArithmeticOverflow);
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
        if (const auto* problem = sample_problem(state_, bits[i])) {
          return sample_error(problem);
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
      std::array<unsigned, FastMath::kLanes> indices{};
      std::array<double, FastMath::kLanes> operands{};
      unsigned lanes = 0;
      auto batch = [&]() -> Status {
        if (!lanes) {
          return Status::success();
        }
        std::array<std::uint64_t, FastMath::kLanes> computed{};
        std::array<bool, FastMath::kLanes> accepted{};
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
using tensor_ops::require;
using tensor_ops::take;
using Poll = Result<ResultProgramPoll>;
ResultBuilder result_builder(const ResultProgramPhase& phase, bool empty) {
  auto builder = take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{},
      phase.query.tile_height, phase.query.tile_width, phase.query.resources));
  require(builder.bind_descriptor_relation(take(ResultRelation::cartesian(
      phase.resources, 1,
      {0, 8, 0, empty ? 0U : 1U, ResultSupportTarget::Descriptor, 0}))));
  return builder;
}
Poll empty_result(const ResultProgramPhase& phase) try {
  auto builder = result_builder(phase, true);
  return Poll(ResultPublication{take(builder.seal()), true});
} catch (const Status& status) {
  return Poll(status);
}
Status evaluate(const ResultProgramPhase& phase, const Preparation& state,
                Runner* runner, const ResultTensorReadWindow& source,
                const Region& region, const ResultTensorWriteWindow* writer) {
  const auto& dims = region.dimensions();
  const auto sample_axis = source.sample_axis();
  std::vector<std::uint64_t> at;
  for (auto d : dims)
    at.push_back(d.offset);
  const auto width = state.narrow ? 4U : 8U;
  for (;;) {
    if (phase.query.cancellation.cancelled())
      return {ErrorCode::Cancelled, "FMT-09 cancelled"};
    const auto read = take(source.row_run(at));
    const bool split =
        state.sample_axis && *state.sample_axis == sample_axis && !state.all;
    const bool selected =
        state.selected(state.sample_axis ? at[*state.sample_axis] : 0);
    const bool simple =
        !selected || state.program.identity ||
        (state.program.definition.curve == TransferCurve::PowerGamma &&
         *state.program.definition.gamma == 2);
    const auto batch = split ? 1U : simple ? 1024U : 64U;
    auto n =
        static_cast<unsigned>(std::min<std::uint64_t>(batch, read.samples));
    n = static_cast<unsigned>(std::min<std::uint64_t>(
        n,
        dims[sample_axis].offset + dims[sample_axis].extent - at[sample_axis]));
    std::uint8_t* target = nullptr;
    if (writer) {
      auto write = take(writer->row_run(at));
      target = write.data;
      n = static_cast<unsigned>(std::min<std::uint64_t>(n, write.samples));
      if (write.sample_stride_bytes != static_cast<std::int64_t>(width))
        n = 1;
    }
    auto status =
        runner->span(read.data, read.sample_stride_bytes, target, n, selected);
    if (!status.ok()) {
      at[sample_axis] += runner->failed_lane;
      return located(status, at);
    }
    at[sample_axis] += n;
    if (at[sample_axis] < dims[sample_axis].offset + dims[sample_axis].extent)
      continue;
    at[sample_axis] = dims[sample_axis].offset;
    bool next = false;
    for (std::size_t axis = dims.size(); axis;) {
      --axis;
      if (axis == sample_axis)
        continue;
      if (++at[axis] < dims[axis].offset + dims[axis].extent) {
        next = true;
        break;
      }
      at[axis] = dims[axis].offset;
    }
    if (!next)
      return Status::success();
  }
}
ResultRelation support(const ResultProgramPhase& phase,
                       const Preparation& state) {
  const auto& tensor = phase.query.output.result_schema->tensors[0];
  const auto shape = tensor.sample_shape();
  std::vector<ResultMappedAxis> axes(shape.size());
  for (std::size_t i = 0; i < axes.size(); ++i)
    axes[i].output_axis = static_cast<std::int32_t>(i);
  ResourceVector<ResultRelation> relations;
  for (const auto& span : state.spans) {
    require(phase.consume_work(shape.size() + 1));
    relations.push_back(take(ResultRelation::mapped(
        phase.resources, shape, span.region, shape, axes,
        {0, span.roles, 0, 0, ResultSupportTarget::Tensor, 0})));
  }
  relations.push_back(take(ResultRelation::cartesian(
      phase.resources, take(tensor.sample_count()),
      {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
  while (relations.size() > 1) {
    ResourceVector<ResultRelation> next;
    for (std::size_t i = 0; i < relations.size(); i += 16) {
      require(phase.consume_work(17));
      std::vector<ResultRelation> group;
      for (std::size_t j = i; j < std::min(i + 16, relations.size()); ++j)
        group.push_back(relations[j]);
      next.push_back(take(ResultRelation::unite(phase.resources, group)));
    }
    relations = std::move(next);
  }
  return relations.front();
}
struct Continuation final {
  const Preparation* state;
  bool requested = false;
  Footprint output;
  explicit Continuation(const Preparation* s) : state(s) {}
  Poll poll(const ResultProgramPhase& phase) try {
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    if (!requested) {
      requested = true;
      output = phase.query.tensor_outputs
                   ? *phase.query.tensor_outputs
                   : take(Footprint::all(tensor.sample_shape()));
      ResultProgramNeed need;
      std::uint32_t roles = 9;
      for (const auto& span : state->spans) {
        if (!(span.roles & 4))
          continue;
        for (const auto& box : output.boxes()) {
          require(phase.consume_work(box.rank() + 1));
          bool overlap = true;
          for (std::size_t axis = 0; axis < box.rank(); ++axis) {
            const auto a = box.dimensions()[axis];
            const auto b = span.region.dimensions()[axis];
            if (a.offset >= b.offset + b.extent ||
                b.offset >= a.offset + a.extent) {
              overlap = false;
              break;
            }
          }
          if (overlap) {
            roles |= 4;
            break;
          }
        }
        if (roles & 4)
          break;
      }
      need.tensors.push_back({0, 0, output, roles});
      return Poll(std::move(need));
    }
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Poll(sample_error("floating environment unavailable"));
    const std::function<Status(std::uint64_t)> consume = [&](std::uint64_t n) {
      if (phase.query.cancellation.cancelled())
        return Status{ErrorCode::Cancelled, "FMT-09 cancelled"};
      return phase.consume_work(n);
    };
    ExactWorkScope exact(&consume, &phase.query.cancellation);
    core_internal::WorkConsumer checkpoint(consume);
    Runner runner(*state, phase.allocator, checkpoint, environment.active());
    auto result = [&]() -> Poll {
      try {
        auto builder = result_builder(phase, false);
        const auto relation = support(phase, *state);
        std::vector<ResultMappedAxis> axes(tensor.sample_shape().size());
        for (std::size_t i = 0; i < axes.size(); ++i)
          axes[i].output_axis = static_cast<std::int32_t>(i);
        require(format_result::planes(tensor, output, [&](const Region& box) {
          auto source = take(
              phase.tensors->at({0, 0}).acquire(box, phase.query.cancellation));
          if (state->program.identity && !state->materialize) {
            const auto prior_views = runner.diagnostics.view_elements;
            require(evaluate(phase, *state, &runner, source, box, nullptr));
            runner.diagnostics.view_elements = prior_views;
            format_result::PublicationCounts counts;
            auto status =
                format_result::publish(phase, &builder, box, source, axes,
                                       relation, state->layout, 0, &counts);
            runner.diagnostics.view_elements += counts.viewed;
            runner.diagnostics.copied_elements += counts.copied;
            return status;
          }
          return builder.publish_tensor_kernel(
              0, box,
              [&](const auto& writers) {
                try {
                  for (const auto& writer : writers)
                    require(evaluate(phase, *state, &runner, source,
                                     writer.region(), &writer));
                  return Status::success();
                } catch (const Status& status) {
                  return status;
                } catch (const ExactWorkFailure& failure) {
                  return failure.status;
                }
              },
              relation, {true, true, true, true}, phase.query.cancellation);
        }));
        require(consume(1));
        return Poll(ResultPublication{take(builder.seal()), true});
      } catch (const Status& status) {
        return Poll(status);
      } catch (const ExactWorkFailure& failure) {
        return Poll(failure.status);
      }
    }();
    if (phase.report_numeric) {
      auto status = phase.report_numeric(runner.diagnostics);
      if (!status.ok())
        return Poll(status);
    }
    return result;
  } catch (const Status& status) {
    return Poll(status);
  }
};
OperationDefinition operation(bool encode, SequenceProfile profile,
                              const char* suffix) {
  OperationDefinition d;
  d.key =
      std::string("color.transfer_") + (encode ? "encode" : "decode") + suffix;
  auto& t = d.traits;
  t.input_count = 1;
  OperationPortConstraint port;
  port.kind = OperationPortKind::Result;
  port.element_type_mask = 127;
  t.input_schema = {port};
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
  out.output_schema = port;
  out.result_schema = tensor_ops::scalar_schema();
  out.region_rule = OperationRegionRule::Dependency;
  out.continuation_bytes = sizeof(Continuation);
  out.maximum_dependency_stages = 2;
  d.prepare_static = [encode, profile](const auto& inputs, const auto& params) {
    return prepare(inputs, params, encode, profile);
  };
  d.start_result = [](const ResultProgramQuery& query,
                      const BufferAllocator& allocator) {
    if (query.tensor_outputs && query.tensor_outputs->empty())
      return ResultContinuation::stateless<empty_result>();
    return ResultContinuation::make<Continuation>(
        allocator, static_cast<const Preparation*>(query.prepared->state()));
  };
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
