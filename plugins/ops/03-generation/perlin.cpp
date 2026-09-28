#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "03-generation/perlin_exact.hpp"
#include "03-generation/perlin_gpu.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using generation_ops::PerlinCoordinate;
using generation_ops::PerlinExact;
struct PerlinSlot final {
  PerlinExact<8> small;
  PerlinExact<16> normal;
  PerlinExact<272> full;
  Status failure;
};
Result<std::uint64_t> sample(PerlinSlot* slot,
                             const std::array<PerlinCoordinate, 3>& coordinates,
                             bool narrow,
                             const execution_internal::WorkConsumer& consume,
                             const CancellationToken& cancellation) {
  unsigned q = 0;
  for (const auto& coordinate : coordinates)
    q = std::max(q, coordinate.denominator_bits);
  std::uint64_t credit = q <= 31   ? PerlinExact<8>::work_bound(q)
                         : q <= 63 ? PerlinExact<16>::work_bound(q)
                                   : PerlinExact<272>::work_bound(q);
  const auto status = consume(credit);
  if (!status.ok())
    return Result<std::uint64_t>(status);
  const auto local = [&](std::uint64_t units) {
    if (cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "Perlin cancelled"};
    if (units > credit)
      return Status{ErrorCode::Internal, "Perlin work bound exceeded"};
    credit -= units;
    return Status::success();
  };
  return q <= 31   ? slot->small.evaluate(coordinates, narrow, local)
         : q <= 63 ? slot->normal.evaluate(coordinates, narrow, local)
                   : slot->full.evaluate(coordinates, narrow, local);
}
struct PerlinWork final {
  const OperationInvocation& call;
  PerlinSlot* slots;
  std::uint8_t* output;
  bool output_narrow;
};
Status calculate(PerlinWork& work, std::uint64_t begin, std::uint64_t end,
                 PerlinSlot* slot) {
  const auto& input = work.call.inputs[0];
  const auto& shape = input.descriptor().shape;
  const auto& layout = input.layout();
  const auto source_width =
      Value::element_size(input.descriptor().element_type);
  const auto output_width = work.output_narrow ? 4U : 8U;
  const auto* budget = resource_internal::metadata_budget();
  const auto consume = [&](std::uint64_t units) {
    if (work.call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "Perlin cancelled"};
    return budget ? budget->consume({units}) : Status::success();
  };
  // Immutable validated layouts may have negative/broadcast strides and a
  // nonzero logical origin. Wide address arithmetic avoids signed overflow;
  // Value validation already proves every accessed coordinate fits storage.
  for (auto index = begin; index < end; ++index) {
    auto status = consume(3 * shape.size());
    if (!status.ok())
      return status;
    auto linear = index;
    __int128 address = layout.byte_offset;
    for (std::size_t axis = shape.size() - 1; axis; --axis) {
      const auto coordinate = linear % shape[axis - 1];
      linear /= shape[axis - 1];
      const auto origin = layout.origin.empty() ? 0 : layout.origin[axis - 1];
      address += (static_cast<__int128>(coordinate) - origin) *
                 layout.byte_strides[axis - 1];
    }
    if (!layout.origin.empty())
      address -= static_cast<__int128>(layout.origin.back()) *
                 layout.byte_strides.back();
    std::array<PerlinCoordinate, 3> coordinates;
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto at =
          address + static_cast<__int128>(axis) * layout.byte_strides.back();
      std::uint64_t raw = 0;
      std::memcpy(&raw, input.bytes().data() + static_cast<std::size_t>(at),
                  source_width);
      auto decoded = PerlinCoordinate::decode(raw, source_width == 4);
      if (!decoded.ok())
        return decoded.status();
      coordinates[axis] = decoded.value();
    }
    auto result = sample(slot, coordinates, work.output_narrow, consume,
                         work.call.cancellation);
    if (!result.ok())
      return result.status();
    const auto raw = result.value();
    std::memcpy(work.output + index * output_width, &raw, output_width);
  }
  return Status::success();
}
int perlin_block(void* user, std::uint64_t begin, std::uint64_t end,
                 std::uint32_t slot) noexcept {
  auto& work = *static_cast<PerlinWork*>(user);
  auto& failure = work.slots[slot].failure;
  try {
    failure = calculate(work, begin, end, &work.slots[slot]);
  } catch (const std::bad_alloc&) {
    failure.code = ErrorCode::ResourceExhausted;
  } catch (...) {
    failure.code = ErrorCode::OperationFailed;
  }
  return failure.ok()                                   ? 0
         : failure.code == ErrorCode::Cancelled         ? 2
         : failure.code == ErrorCode::ResourceExhausted ? 4
         : failure.code == ErrorCode::InvalidArgument   ? 6
                                                        : 1;
}
bool output_narrow(const std::map<std::string, ParameterValue>& parameters) {
  const auto found = parameters.find("dtype");
  return found != parameters.end() &&
         std::get<std::string>(found->second) == "float32";
}
struct PerlinTile final {
  PerlinSlot arithmetic;
  bool requested = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return Result<DependencyPoll>(std::move(batch));
    }
    using Answer = Result<DependencyPoll>;
    using Decoded = std::array<PerlinCoordinate, 3>;
    static_assert(std::is_trivially_destructible_v<Decoded>);
    const auto width =
        Value::element_size(phase.query.inputs[0].descriptor.element_type);
    const auto& descriptor = phase.query.output.descriptor;
    const bool narrow = descriptor.element_type == ElementType::Float32;
    const auto rank = descriptor.shape.size();
    const auto count = phase.query.outputs.element_count().value();
    if (count > SIZE_MAX / sizeof(Decoded))
      return Answer(
          Status{ErrorCode::ResourceExhausted, "Perlin tile scratch overflow"});
    auto allocated = phase.allocator.allocate(count * sizeof(Decoded));
    if (!allocated.ok())
      return Answer(allocated.status());
    auto scratch = allocated.take_value();
    auto* decoded = reinterpret_cast<Decoded*>(scratch.data());
    auto owner =
        dependency_internal::metadata_owner((rank + 1) * sizeof(std::uint64_t));
    std::vector<std::uint64_t> source(rank + 1);
    // Include the actual authorization and fragment search geometry in the
    // prepaid bound. Reads debit this local credit and poll cancellation,
    // avoiding one shared resource-budget lock per fragment candidate.
    const auto input_rank = static_cast<unsigned __int128>(rank) + 1;
    const auto lookup_work = (static_cast<unsigned __int128>(
                                  phase.inputs[0].coverage().boxes().size()) +
                              phase.inputs[0].fragments().size()) *
                                 (input_rank + 1) +
                             4 * input_rank + 2;
    const auto lookup_total =
        3 * static_cast<unsigned __int128>(count) * lookup_work;
    const auto total = lookup_total + count * (3 * input_rank + 3);
    if (total > UINT64_MAX)
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "Perlin tile lookup work overflow"});
    auto status = phase.consume_work(static_cast<std::uint64_t>(total));
    if (!status.ok())
      return Answer(status);
    auto lookup_credit = static_cast<std::uint64_t>(lookup_total);
    auto lookup_limits = phase.sets;
    lookup_limits.cancellation = phase.query.cancellation;
    lookup_limits.consume_work = [&](std::uint64_t units) {
      if (units > lookup_credit)
        return Status{ErrorCode::Internal, "Perlin lookup work bound exceeded"};
      lookup_credit -= units;
      return Status::success();
    };
    std::uint64_t next = 0, credit = 0;
    for (const auto& box : phase.query.outputs.boxes()) {
      const auto size = box.element_count().value();
      if (size > phase.sets.maximum_work)
        return Answer(
            Status{ErrorCode::ResourceExhausted, "Perlin tile visit limit"});
      for (std::size_t axis = 0; axis < rank; ++axis)
        source[axis] = box.dimensions()[axis].offset;
      for (std::uint64_t i = 0; i < size; ++i) {
        if (!(next % 64)) {
          status = phase.consume_work(0);
          if (!status.ok())
            return Answer(status);
        }
        if (phase.query.cancellation.cancelled())
          return Answer(Status{ErrorCode::Cancelled, "Perlin tile cancelled"});
        Decoded coordinates;
        unsigned q = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
          source.back() = axis;
          std::uint64_t raw = 0;
          status = phase.inputs[0].read(source, &raw, width, lookup_limits);
          if (!status.ok()) {
            if (status.code == ErrorCode::InvalidArgument) {
              status.reason = FailureReason::UnauthorizedRead;
              status.detail.origin = FailureOrigin::Protocol;
              status.detail.scope = FailureScope::Atom;
            }
            return Answer(phase.report_failure(status));
          }
          auto part = PerlinCoordinate::decode(raw, width == 4);
          if (!part.ok())
            return Answer(part.status());
          coordinates[axis] = part.value();
          q = std::max(q, coordinates[axis].denominator_bits);
        }
        const auto charge = q <= 31   ? PerlinExact<8>::work_bound(q)
                            : q <= 63 ? PerlinExact<16>::work_bound(q)
                                      : PerlinExact<272>::work_bound(q);
        if (charge > UINT64_MAX - credit)
          return Answer(Status{ErrorCode::ResourceExhausted,
                               "Perlin tile work overflow"});
        credit += charge;
        new (decoded + next++) Decoded(coordinates);
        for (std::size_t axis = rank; axis-- > 0;) {
          const auto& dimension = box.dimensions()[axis];
          if (++source[axis] < dimension.offset + dimension.extent)
            break;
          source[axis] = dimension.offset;
        }
      }
    }
    status = phase.consume_work(credit);
    if (!status.ok())
      return Answer(status);
    const auto prepaid = [&](std::uint64_t units) {
      if (units > credit)
        return Status{ErrorCode::Internal, "Perlin tile work bound exceeded"};
      credit -= units;
      return Status::success();
    };
    numeric_ops::ArrayPublication publication(
        phase.query.outputs.boxes().size(), rank);
    ResourceVector<Value> values;
    values.reserve(phase.query.outputs.boxes().size());
    next = 0;
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto writer = allocated.take_value();
      const auto size = box.element_count().value();
      for (std::uint64_t i = 0; i < size; ++i, ++next) {
        if (!(next % 64)) {
          status = phase.consume_work(0);
          if (!status.ok())
            return Answer(status);
        }
        auto result = sample(&arithmetic, decoded[next], narrow, prepaid,
                             phase.query.cancellation);
        if (!result.ok())
          return Answer(result.status());
        const auto bits = result.value();
        std::memcpy(writer.data() + i * (narrow ? 4 : 8), &bits,
                    narrow ? 4 : 8);
      }
      auto value = std::move(writer).publish();
      if (!value.ok())
        return Answer(value.status());
      auto retained = publication.retain(value.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      values.push_back(retained.take_value());
    }
    auto result = publication.finish(descriptor, phase.query.outputs,
                                     values.data(), values.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
Result<Value> execute_whole(const OperationInvocation& call) {
  using Answer = Result<Value>;
  auto descriptor = call.inputs[0].descriptor();
  descriptor.shape.pop_back();
  const bool narrow = output_narrow(call.parameters);
  descriptor.element_type =
      narrow ? ElementType::Float32 : ElementType::Float64;
  auto allocated =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  const auto count = call.output_region.element_count().value();
  if (!count)
    return std::move(output).publish();
  const auto grant = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      count, call.cpu_parallel ? call.cpu_parallel->maximum_workers : 1));
  auto allocated_scratch = call.allocator.allocate(grant * sizeof(PerlinSlot));
  if (!allocated_scratch.ok())
    return Answer(allocated_scratch.status());
  auto scratch = allocated_scratch.take_value();
  auto* slots = reinterpret_cast<PerlinSlot*>(scratch.data());
  for (unsigned i = 0; i < grant; ++i)
    new (slots + i) PerlinSlot;
  struct Destroy {
    PerlinSlot* slots;
    unsigned count;
    ~Destroy() {
      for (unsigned i = 0; i < count; ++i)
        slots[i].~PerlinSlot();
    }
  } destroy{slots, grant};
  PerlinWork work{call, slots, output.data(), narrow};
  const auto* parallel = call.cpu_parallel;
  const auto code = parallel ? parallel->run(parallel->context, count, 8, grant,
                                             perlin_block, &work)
                             : perlin_block(&work, 0, count, 0);
  for (unsigned i = 0; i < grant; ++i)
    if (!slots[i].failure.ok())
      return Answer(slots[i].failure);
  if (code)
    return Answer(Status{code == 2   ? ErrorCode::Cancelled
                         : code == 4 ? ErrorCode::ResourceExhausted
                                     : ErrorCode::OperationFailed,
                         "Perlin host range failed"});
  return std::move(output).publish();
}
}  // namespace

Status register_perlin(OperationRegistry* registry) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    const bool tiled = mode == 1, gpu = mode == 2;
    OperationDefinition operation;
    operation.key = gpu     ? "noise.perlin2002_3d_v1_strict_gpu"
                    : tiled ? "noise.perlin2002_3d_v1_strict_cpu_tiled"
                            : "noise.perlin2002_3d_v1_strict_cpu_whole";
    auto& traits = operation.traits;
    traits.input_count = 1;
    traits.input_schema.resize(1);
    traits.input_schema[0].element_type_mask = 12;
    traits.requires_metadata_specialization = true;
    traits.supports_cpu = !gpu;
    traits.supports_gpu = gpu;
    traits.allows_cpu_fallback = false;
    traits.workspace_bytes = gpu     ? kPerlinGpuWorkspace
                             : tiled ? 0
                                     : 64 * sizeof(PerlinSlot);
    // Three Float32 coordinates are the smallest input representation.
    // Reserve every decoded coordinate before starting the arithmetic pass.
    traits.workspace_input_multiplier =
        tiled ? (sizeof(std::array<PerlinCoordinate, 3>) + 11) / 12 : 0;
    traits.parameter_schema = {
        {"dtype", OperationParameterType::String, false}};
    auto& output = traits.outputs[0];
    output.key = "values";
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {1};
    output.region_rule =
        tiled ? OperationRegionRule::Dependency : OperationRegionRule::Whole;
    if (tiled) {
      output.dependency_version = 1;
      output.continuation_bytes = sizeof(PerlinTile);
      output.maximum_dependency_stages = 2;
      operation.start_dependency = [](const DependencyQuery&,
                                      const BufferAllocator& allocator) {
        return DependencyContinuation::make<PerlinTile>(allocator);
      };
    }
    output.requires_dense_output = !tiled;
    operation.specialize_metadata = [tiled](const auto& inputs,
                                            const auto& parameters)
        -> Result<std::vector<OperationOutputSpecialization>> {
      using Answer = Result<std::vector<OperationOutputSpecialization>>;
      const auto& descriptor = inputs[0].descriptor;
      if (descriptor.shape.size() < 2 || descriptor.shape.back() != 3)
        return Answer(Status{ErrorCode::TypeMismatch,
                             "Perlin requires coordinates[S...,3]"});
      std::uint64_t count = 1;
      for (const auto extent : descriptor.shape) {
        if (!extent || extent > (UINT64_C(1) << 40) / count)
          return Answer(Status{ErrorCode::ResourceExhausted,
                               "Perlin coordinates exceed 2^40 elements"});
        count *= extent;
      }
      const auto dtype = parameters.find("dtype");
      if (dtype != parameters.end() &&
          std::get<std::string>(dtype->second) != "float32" &&
          std::get<std::string>(dtype->second) != "float64")
        return Answer(Status{ErrorCode::InvalidArgument,
                             "Perlin dtype must be float32 or float64"});
      OperationOutputSpecialization result;
      result.metadata.descriptor = descriptor;
      result.metadata.descriptor.shape.pop_back();
      result.metadata.descriptor.element_type = output_narrow(parameters)
                                                    ? ElementType::Float32
                                                    : ElementType::Float64;
      if (tiled) {
        DependencyMappedNeed coordinates;
        coordinates.port = 0;
        coordinates.roles =
            static_cast<std::uint32_t>(DependencyRole::Data) |
            static_cast<std::uint32_t>(DependencyRole::Validation);
        for (std::size_t axis = 0;
             axis < result.metadata.descriptor.shape.size(); ++axis)
          coordinates.axes.push_back({static_cast<std::int32_t>(axis), {}, 0});
        coordinates.axes.push_back({-1, {0, 3}, 0});
        auto all = Footprint::all(result.metadata.descriptor.shape);
        if (!all.ok())
          return Answer(all.status());
        result.static_dependency_pieces = std::vector<DependencyMapPiece>{
            {all.take_value(), {std::move(coordinates)}}};
        result.regional_atomic = true;
      }
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    };
    if (!tiled)
      operation.callback = gpu ? execute_perlin_gpu : execute_whole;
    auto registered = registry->register_operation(std::move(operation));
    if (!registered.ok())
      return registered;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
