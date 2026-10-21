#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/transfer_fast.hpp"
#include "02-format-color/transfer_math.hpp"
#include "02-format-color/transfer_prepare.hpp"
#include "02-format-color/transfer_runtime.hpp"
#include "02-format-color/transfer_simd.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "photospider/plugin/tile_geometry.hpp"

namespace ps::plugin_internal::transfer_ops {
struct SampleBlock final {
  std::array<std::uint64_t, 64> source{}, target{}, offsets{};
};
struct GeneralScratch final {
  SampleBlock block;
  StrictMath strict{SequenceProfile::Strict};
#if PHOTOSPIDER_TRANSFER_COMPACT_MATH
  CompactMath compact{SequenceProfile::Strict};
#endif
  FastMath fast;
  std::array<std::uint64_t, 64> bits{}, result{};
  std::array<double, 64> input{};
  std::array<unsigned, 64> branches{};
  std::array<bool, 64> done{};
  std::array<unsigned, FastMath::kLanes> indices{};
  std::array<double, FastMath::kLanes> operands{};
  std::array<std::uint64_t, FastMath::kLanes> computed{};
  std::array<bool, FastMath::kLanes> accepted{};
};
bool gamma2(const Preparation& state) {
  return state.program.definition.curve == TransferCurve::PowerGamma &&
         *state.program.definition.gamma == 2;
}
bool calculates(const Preparation& state, const Region& output) {
  if (state.program.identity || state.layout == "view")
    return false;
  if (state.all)
    return true;
  const auto channels = output.dimensions()[*state.sample_axis];
  const auto first = std::lower_bound(state.components.begin(),
                                      state.components.end(), channels.offset);
  return first != state.components.end() &&
         *first - channels.offset < channels.extent;
}
std::uint64_t scratch_bytes(const Preparation& state, const Region& output) {
  return !calculates(state, output) ? 0
         : gamma2(state)            ? sizeof(SampleBlock)
                                    : sizeof(GeneralScratch);
}
Status convert_general(const Preparation& state, GeneralScratch* w,
                       unsigned count,
                       const core_internal::WorkConsumer& consume) {
  const auto& c = state.program;
  const unsigned width = state.narrow ? 4 : 8;
  w->done.fill(false);
  for (unsigned i = 0; i < count; ++i) {
    w->bits[i] = 0;
    std::memcpy(&w->bits[i],
                reinterpret_cast<const std::uint8_t*>(w->block.source.data()) +
                    i * width,
                width);
    auto direct = nonfinite(c, w->bits[i], state.narrow);
    if (!direct)
      direct = anchor(c, w->bits[i], state.narrow);
    if (direct) {
      w->result[i] = *direct;
      w->done[i] = true;
      continue;
    }
    w->input[i] = numeric_double(w->bits[i], state.narrow);
    if (c.signed_curve)
      w->input[i] = std::abs(w->input[i]);
    // This branch is part of PQ's raw IEEE formula, independent of semantic
    // caller preconditions. Keep negative operands out of fractional powers.
    if (c.definition.curve == TransferCurve::Pq && w->input[i] < 0) {
      w->result[i] = nan_bits(state.narrow);
      w->done[i] = true;
      continue;
    }
    w->branches[i] = c.branch(w->input[i]);
  }
  for (unsigned branch = 0; branch < c.branch_count; ++branch) {
    unsigned lanes = 0;
    const auto batch = [&]() -> Status {
      if (!lanes)
        return Status::success();
      w->accepted.fill(false);
      const auto& program = c.branches[branch];
      if (PHOTOSPIDER_TRANSFER_FAST_MATH &&
          numeric_ops::accelerated_math_available()) {
        auto status = w->fast.evaluate(
            program, branch, w->operands.data(), lanes, state.narrow,
            state.profile == SequenceProfile::Strict, w->computed.data(),
            w->accepted.data(), consume, true);
        if (!status.ok())
          return status;
      }
      for (unsigned lane = 0; lane < lanes; ++lane) {
        if (!w->accepted[lane]) {
          bool needs_wide = true;
          Result<std::uint64_t> result(UINT64_C(0));
#if PHOTOSPIDER_TRANSFER_COMPACT_MATH
          result = w->compact.evaluate(program, w->operands[lane], state.narrow,
                                       consume, &needs_wide);
          if (!result.ok())
            return result.status();
#endif
          if (needs_wide)
            result = w->strict.evaluate(program, w->operands[lane],
                                        state.narrow, consume);
          if (!result.ok())
            return result.status();
          w->computed[lane] = result.value();
        }
        const auto index = w->indices[lane];
        w->result[index] = w->computed[lane];
        if (c.signed_curve)
          w->result[index] |= w->bits[index] & sign_mask(state.narrow);
      }
      lanes = 0;
      return Status::success();
    };
    for (unsigned i = 0; i < count; ++i) {
      if (!w->done[i] && w->branches[i] == branch) {
        w->indices[lanes] = i;
        w->operands[lanes++] = w->input[i];
        if (lanes == FastMath::kLanes) {
          auto status = batch();
          if (!status.ok())
            return status;
        }
      }
    }
    auto status = batch();
    if (!status.ok())
      return status;
  }
  for (unsigned i = 0; i < count; ++i)
    std::memcpy(
        reinterpret_cast<std::uint8_t*>(w->block.target.data()) + i * width,
        &w->result[i], width);
  return Status::success();
}
void convert_gamma2(const Preparation& state, SampleBlock* block,
                    unsigned count) {
  auto* source = reinterpret_cast<const std::uint8_t*>(block->source.data());
  auto* target = reinterpret_cast<std::uint8_t*>(block->target.data());
#if PHOTOSPIDER_TRANSFER_GAMMA2_SIMD
  if (numeric_ops::accelerated_math_available()) {
    gamma2_simd(source, target, count, state.narrow, state.program.encode);
    return;
  }
#endif
  const unsigned width = state.narrow ? 4 : 8;
  for (unsigned i = 0; i < count; ++i) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, source + i * width, width);
    auto result = nonfinite(state.program, bits, state.narrow);
    if (!result) {
      const auto x = std::abs(numeric_double(bits, state.narrow));
      if (state.narrow) {
        const float f = static_cast<float>(x);
        result =
            numeric_bits(state.program.encode ? std::sqrt(f) : f * f, true);
      } else {
        result = numeric_bits(state.program.encode ? std::sqrt(x) : x * x);
      }
      *result |= bits & sign_mask(state.narrow);
    }
    std::memcpy(target + i * width, &*result, width);
  }
}
Result<TileSet> required_tiles(const Preparation& state, const Region& output,
                               const TileGrid& grid,
                               const ResourceBudget& budget,
                               std::uint64_t maximum_tiles) {
  using Answer = Result<TileSet>;
  if (state.layout == "view")
    return Answer(TileSet{});
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < grid.rank(); ++axis) {
    const auto d = output.dimensions()[axis];
    std::uint64_t extent = 1;
    if (grid.batch_axis_mask() & (1U << axis))
      extent = d.extent;
    else if (axis == grid.height_axis() || axis == grid.width_axis())
      extent = (d.offset + d.extent - 1) / grid.tile_extent() -
               d.offset / grid.tile_extent() + 1;
    if (extent > maximum_tiles / count)
      return Answer({ErrorCode::ResourceExhausted, "transfer read tile limit"});
    count *= extent;
  }
  return sdk::tiles_for_region(budget, grid, output);
}
constexpr const char* transfer_contract = "transfer-unchecked-v2";
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const Preparation*>(query.static_state);
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!input.ok())
    return Answer(input.status());
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      auto task =
          builder.add_task("transfer", transfer_contract, {},
                           {scratch_bytes(state, region.value()), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      auto reads = required_tiles(state, region.value(), input.value(), budget,
                                  query.limits.maximum_records);
      if (!reads.ok())
        return Answer(reads.status());
      if (reads.value().empty()) {
        auto status = builder.read_input_schema(task.value(), 0);
        if (!status.ok())
          return Answer(status);
      }
      for (auto index : reads.value()) {
        auto charged = budget.consume({1});
        if (!charged.ok())
          return Answer(charged);
        auto status = builder.read_input(task.value(), 0, 0, index);
        if (!status.ok())
          return Answer(status);
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
struct PreparedTransfer final {
  // PreparedKernel owns the OperationPreparation for the entire context life.
  const Preparation* state;
  bool calculates;
  fixed_tensor::PointInputs<1> inputs;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& prepared =
      *static_cast<const PreparedTransfer*>(invocation->prepared_context);
  const auto& state = *prepared.state;
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& result) {
    if (!result.ok())
      *status =
          kernel_failure(result.code, result.message.c_str(), result.reason);
    return result.ok();
  };
  if (!accept(consume(0)))
    return;
  if (state.layout == "view") {
    *status = kernel_failure(ErrorCode::InvalidArgument, "ViewUnavailable",
                             FailureReason::ViewUnavailable);
    return;
  }
  FloatingEnvironment environment;
  if (prepared.calculates && !environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "transfer numeric environment");
    return;
  }
  SampleBlock* block = nullptr;
  GeneralScratch* work = nullptr;
  if (prepared.calculates) {
    if (gamma2(state)) {
      block = new (invocation->scratch[0].allocation) SampleBlock;
    } else {
      work = new (invocation->scratch[0].allocation) GeneralScratch;
      block = &work->block;
    }
  }
  struct ScratchLifetime final {
    GeneralScratch* workspace;
    ~ScratchLifetime() {
      if (workspace)
        workspace->~GeneralScratch();
    }
  } lifetime{work};
  fixed_tensor::PointReader<1> reader{prepared.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  auto* destination = static_cast<std::uint8_t*>(output.allocation);
  const auto axis = state.sample_axis;
  const unsigned width = state.narrow ? 4 : 8;
  const auto first = axis ? layout.origin[*axis] : 0;
  const auto channels = axis ? layout.shape[*axis] : 1;
  std::uint64_t points = 1;
  for (unsigned a = 0; a < layout.rank; ++a)
    if (!axis || a != *axis)
      points *= layout.shape[a];
  for (auto channel = first; channel < first + channels; ++channel) {
    const bool calculate = !state.program.identity && state.selected(channel);
    for (std::uint64_t begin = 0; begin < points; begin += 64) {
      if (!accept(consume(0)))
        return;
      const auto count =
          static_cast<unsigned>(std::min<std::uint64_t>(64, points - begin));
      for (unsigned lane = 0; lane < count; ++lane) {
        auto index = begin + lane;
        std::array<std::uint64_t, 8> at{};
        std::uint64_t offset = output.byte_offset;
        for (unsigned a = layout.rank; a-- > 0;) {
          if (axis && a == *axis) {
            at[a] = channel;
          } else {
            at[a] = layout.origin[a] + index % layout.shape[a];
            index /= layout.shape[a];
          }
          offset += (at[a] - layout.origin[a]) * layout.strides[a];
        }
        const auto bits = reader.bits(0, at, width);
        if (!calculate) {
          std::memcpy(destination + offset, &bits, width);
        } else {
          std::memcpy(reinterpret_cast<std::uint8_t*>(block->source.data()) +
                          lane * width,
                      &bits, width);
          block->offsets[lane] = offset;
        }
      }
      if (calculate) {
        if (gamma2(state))
          convert_gamma2(state, block, count);
        else if (!accept(convert_general(state, work, count, consume)))
          return;
        for (unsigned lane = 0; lane < count; ++lane)
          std::memcpy(
              destination + block->offsets[lane],
              reinterpret_cast<const std::uint8_t*>(block->target.data()) +
                  lane * width,
              width);
      }
    }
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid transfer signature"});
  };
  const auto* state = static_cast<const Preparation*>(query.static_state);
  if (!state || query.inputs.size() != 1 || query.outputs.size() != 1 ||
      query.writes.size() != 1 || !query.task_parameters.empty() ||
      !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output_schema))
    return invalid();
  const auto& write = query.writes[0];
  auto grid =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto region = grid.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (query.scratch !=
          std::array<std::uint64_t, 3>{scratch_bytes(*state, region.value()), 0,
                                       0} ||
      write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type !=
          (state->narrow ? ElementType::Float32 : ElementType::Float64) ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  auto prepared = std::make_unique<PreparedTransfer>();
  prepared->state = state;
  prepared->calculates = calculates(*state, region.value());
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!input.ok())
    return Answer(input.status());
  prepared->inputs.grids[0] = input.take_value();
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  auto reads = required_tiles(*state, region.value(), prepared->inputs.grids[0],
                              ResourceBudget(limits), query.reads.size());
  if (!reads.ok())
    return Answer(reads.status());
  if (reads.value().size() != query.reads.size())
    return invalid();
  std::size_t slot = 0;
  for (auto index : reads.value()) {
    const auto& read = query.reads[slot];
    auto expected = prepared->inputs.grids[0].region(index);
    if (!expected.ok())
      return Answer(expected.status());
    if (read.port || read.member || read.tile_index != index ||
        read.domain != physical::Domain::Host ||
        read.layout.element_type != write.layout.element_type ||
        !fixed_tensor::matches_region(read.layout, expected.value()))
      return invalid();
    prepared->inputs.slots[0].push_back(
        {index, static_cast<std::uint32_t>(slot++)});
  }
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = prepared.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedTransfer*>(context);
  };
  return Answer(kernel);
}
OperationDefinition operation(bool encode, SequenceProfile profile,
                              const char* suffix) {
  OperationDefinition d;
  d.key =
      std::string("color.transfer_") + (encode ? "encode" : "decode") + suffix;
  auto& t = d.traits;
  t.input_count = 1;
  OperationPortConstraint port;

  port.element_type_mask = 127;
  t.input_schema = {port};
  t.requires_metadata_specialization = true;
  t.parameter_schema = {
      {"group", OperationParameterType::String, false},
      {"components", OperationParameterType::String, false},
      {"axis", OperationParameterType::Int64, false},
      {"curve", OperationParameterType::String, false},
      {"gamma", OperationParameterType::Float64, false},
      {"coefficient_variant", OperationParameterType::String, false},
      {"black_luminance", OperationParameterType::Float64, false},
      {"white_luminance", OperationParameterType::Float64, false},
      {"input_description", OperationParameterType::String, false},
      {"layout", OperationParameterType::String, false}};
  auto& out = t.outputs[0];
  out.key = "values";
  out.output_schema = port;
  out.result_schema = tensor_ops::scalar_schema();
  d.prepare_static = [encode, profile](const auto& inputs, const auto& params) {
    return prepare(inputs, params, encode, profile);
  };
  d.lower = lower;
  d.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "transfer" || kernel.contract != transfer_contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid transfer kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "transfer requires CPU Host"});
    return prepare_kernel(query);
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
