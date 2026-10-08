#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/numeric_tensor_program.hpp"
#include "03-generation/perlin_exact.hpp"
#include "03-generation/perlin_gpu.hpp"
#include "data/result_window_access.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using generation_ops::PerlinCoordinate;
using generation_ops::PerlinExact;
using numeric_ops::math_require;
using numeric_ops::math_take;
struct PerlinSlot final {
  PerlinExact<8> small;
  PerlinExact<16> normal;
  PerlinExact<272> full;
  Status failure;
  std::vector<std::uint64_t> coordinate;
};
Result<std::uint64_t> sample(PerlinSlot* slot,
                             const std::array<PerlinCoordinate, 3>& coordinates,
                             bool narrow,
                             const core_internal::WorkConsumer& consume,
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
using Decoded = std::array<PerlinCoordinate, 3>;
struct PerlinWork final {
  const ResultProgramPhase& phase;
  const ResultTensorReadWindow& input;
  const Region& output_region;
  PerlinSlot* slots;
  const Decoded* decoded;
  const Value* affine;
  std::uint8_t* output;
  bool narrow;
};
Result<Decoded> decode(const PerlinWork& work, std::uint64_t index,
                       std::vector<std::uint64_t>& at) {
  const auto& dims = work.output_region.dimensions();
  for (std::size_t axis = dims.size(); axis-- > 0;) {
    at[axis] = dims[axis].offset + index % dims[axis].extent;
    index /= dims[axis].extent;
  }
  const auto width =
      Value::element_size(work.input.spec().descriptor.element_type);
  Decoded coordinates;
  for (unsigned axis = 0; axis < 3; ++axis) {
    at.back() = axis;
    std::uint64_t bits = 0;
    if (work.affine) {
      const auto& layout = work.affine->layout();
      __int128 offset = layout.byte_offset;
      for (std::size_t d = 0; d < at.size(); ++d)
        offset += (static_cast<__int128>(at[d]) -
                   (layout.origin.empty() ? 0 : layout.origin[d])) *
                  layout.byte_strides[d];
      std::memcpy(
          &bits, work.affine->bytes().data() + static_cast<std::size_t>(offset),
          width);
    } else {
      auto run = work.input.row_run(at);
      if (!run.ok())
        return Result<Decoded>(run.status());
      std::memcpy(&bits, run.value().data, width);
    }
    auto part = PerlinCoordinate::decode(bits, width == 4);
    if (!part.ok())
      return Result<Decoded>(part.status());
    coordinates[axis] = part.take_value();
  }
  return Result<Decoded>(coordinates);
}
int perlin_block(void* raw, std::uint64_t begin, std::uint64_t end,
                 std::uint32_t slot_index) noexcept {
  auto& work = *static_cast<PerlinWork*>(raw);
  auto& slot = work.slots[slot_index];
  try {
    const auto consume = [&](std::uint64_t units) {
      return work.decoded ? Status::success() : work.phase.consume_work(units);
    };
    for (auto index = begin; index < end; ++index) {
      if (work.phase.query.cancellation.cancelled()) {
        slot.failure = {ErrorCode::Cancelled, "Perlin cancelled"};
        break;
      }
      Result<Decoded> coordinates = work.decoded
                                        ? Result<Decoded>(work.decoded[index])
                                        : decode(work, index, slot.coordinate);
      if (!coordinates.ok()) {
        slot.failure = coordinates.status();
        break;
      }
      auto result = sample(&slot, coordinates.value(), work.narrow, consume,
                           work.phase.query.cancellation);
      if (!result.ok()) {
        slot.failure = result.status();
        break;
      }
      const auto bits = result.value();
      std::memcpy(work.output + index * (work.narrow ? 4 : 8), &bits,
                  work.narrow ? 4 : 8);
    }
  } catch (const std::bad_alloc&) {
    slot.failure = {ErrorCode::ResourceExhausted, {}};
  } catch (...) {
    slot.failure = {ErrorCode::OperationFailed, {}};
  }
  return slot.failure.ok()                                   ? 0
         : slot.failure.code == ErrorCode::Cancelled         ? 2
         : slot.failure.code == ErrorCode::ResourceExhausted ? 4
         : slot.failure.code == ErrorCode::InvalidArgument   ? 6
                                                             : 1;
}
int perlin_tile(void* raw, const ps_cpu_tile_v1* tile) noexcept {
  return perlin_block(raw, tile->begin[0], tile->end[0], tile->slot);
}
Result<MutableBuffer> compute_cpu(const ResultProgramPhase& phase,
                                  const ResultTensorReadWindow& input,
                                  const Region& region, bool tiled) {
  using Answer = Result<MutableBuffer>;
  const auto count = math_take(region.element_count());
  const bool narrow =
      phase.query.output.result_schema->tensors[0].descriptor.element_type ==
      ElementType::Float32;
  auto affine = execution_internal::ResultWindowAccess::affine(input);
  if (!affine.ok() && affine.status().code != ErrorCode::NotFound)
    return Answer(affine.status());
  const auto lookup =
      affine.ok()
          ? input.region().rank()
          : math_take(execution_internal::ResultWindowAccess::read_work(input));
  if (lookup > (UINT64_MAX - 3) / 3 || count > UINT64_MAX / (3 * lookup + 3))
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  auto charged = phase.consume_work(count * (3 * lookup + 3));
  if (!charged.ok())
    return Answer(charged);
  auto allocated_output =
      phase.resources.allocator().allocate(count * (narrow ? 4 : 8));
  if (!allocated_output.ok())
    return allocated_output;
  auto output = allocated_output.take_value();
  const auto workers = phase.cpu_parallel ? phase.cpu_parallel->maximum_workers
                       : phase.cpu_tiles  ? phase.cpu_tiles->maximum_workers
                                          : 1;
  const auto grant =
      static_cast<unsigned>(std::min<std::uint64_t>(count, workers));
  auto allocated = phase.allocator.allocate(grant * sizeof(PerlinSlot));
  if (!allocated.ok())
    return Answer(allocated.status());
  auto scratch = allocated.take_value();
  auto metadata = phase.resources.reserve(ResourceCapacity::host(
      grant * input.region().rank() * 8, grant * input.region().rank() * 8));
  if (!metadata.ok())
    return Answer(metadata.status());
  auto* slots = reinterpret_cast<PerlinSlot*>(scratch.data());
  unsigned constructed = 0;
  struct Destroy {
    PerlinSlot* slots;
    unsigned& count;
    ~Destroy() {
      while (count)
        slots[--count].~PerlinSlot();
    }
  } destroy{slots, constructed};
  for (; constructed < grant;) {
    new (slots + constructed) PerlinSlot;
    slots[constructed++].coordinate.resize(input.region().rank());
  }
  PerlinWork work{phase,         input,
                  region,        slots,
                  nullptr,       affine.ok() ? &affine.value() : nullptr,
                  output.data(), narrow};
  MutableBuffer decoded_storage;
  if (tiled) {
    auto made = phase.allocator.allocate(count * sizeof(Decoded));
    if (!made.ok())
      return Answer(made.status());
    decoded_storage = made.take_value();
    auto* decoded = reinterpret_cast<Decoded*>(decoded_storage.data());
    std::uint64_t credit = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      if (!(i % 64)) {
        auto status = phase.consume_work(0);
        if (!status.ok())
          return Answer(status);
      }
      auto coordinates = decode(work, i, slots[0].coordinate);
      if (!coordinates.ok())
        return Answer(coordinates.status());
      new (decoded + i) Decoded(coordinates.take_value());
      unsigned q = 0;
      for (const auto& coordinate : decoded[i])
        q = std::max(q, coordinate.denominator_bits);
      const auto units = q <= 31   ? PerlinExact<8>::work_bound(q)
                         : q <= 63 ? PerlinExact<16>::work_bound(q)
                                   : PerlinExact<272>::work_bound(q);
      if (units > UINT64_MAX - credit)
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      credit += units;
    }
    auto status = phase.consume_work(credit);
    if (!status.ok())
      return Answer(status);
    work.decoded = decoded;
  }
  int code = 0;
  if (phase.cpu_tiles) {
    const ps_cpu_tile_stage_v1 stage{sizeof(ps_cpu_tile_stage_v1),
                                     {count, 1, 1},
                                     {8, 1, 1},
                                     grant};
    code = phase.cpu_tiles->run(phase.cpu_tiles->context, &stage, perlin_tile,
                                &work);
  } else if (phase.cpu_parallel) {
    code = phase.cpu_parallel->run(phase.cpu_parallel->context, count, 8, grant,
                                   perlin_block, &work);
  } else {
    code = perlin_block(&work, 0, count, 0);
  }
  for (unsigned i = 0; i < grant; ++i)
    if (!slots[i].failure.ok())
      return Answer(slots[i].failure);
  if (code)
    return Answer(Status{code == 2   ? ErrorCode::Cancelled
                         : code == 4 ? ErrorCode::ResourceExhausted
                                     : ErrorCode::OperationFailed,
                         "Perlin host stage failed"});
  return Answer(std::move(output));
}
struct PerlinProgram final {
  bool tiled, gpu, initialized = false, waiting = false,
                   descriptor_requested = false;
  Footprint outputs;
  std::optional<ResultBuilder> builder;
  ResultRelation relation;
  Region current;
  std::size_t box = 0;
  std::array<std::uint64_t, 8> next{};
  explicit PerlinProgram(bool tiles, bool device) : tiled(tiles), gpu(device) {}
  bool next_region(const ResultProgramPhase& phase) {
    if (box >= outputs.boxes().size())
      return false;
    const auto& bounds = outputs.boxes()[box].dimensions();
    std::vector<RegionDimension> dims;
    for (std::size_t axis = 0; axis < bounds.size(); ++axis) {
      const auto extent = !tiled                      ? bounds[axis].extent
                          : axis + 1 == bounds.size() ? phase.query.tile_width
                          : axis + 2 == bounds.size() ? phase.query.tile_height
                                                      : 1;
      dims.push_back({bounds[axis].offset + next[axis],
                      std::min(extent, bounds[axis].extent - next[axis])});
    }
    current = Region(std::move(dims));
    for (std::size_t axis = bounds.size(); axis-- > 0;) {
      next[axis] += current.dimensions()[axis].extent;
      if (next[axis] < bounds[axis].extent)
        return true;
      next[axis] = 0;
    }
    ++box;
    return true;
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using Answer = Result<ResultProgramPoll>;
    auto metadata =
        math_take(phase.resources.reserve(ResourceCapacity::host(8192, 8192)));
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    if (!initialized) {
      initialized = true;
      outputs = phase.query.tensor_outputs ? *phase.query.tensor_outputs
                                           : math_take(Footprint::all(shape));
      builder.emplace(math_take(ResultBuilder::start(
          phase.resources, schema, phase.query.semantic_key)));
      math_require(builder->bind_descriptor_relation(math_take(
          ResultRelation::cartesian(phase.resources, 1,
                                    {0, 8, 0, outputs.empty() ? 0U : 1U,
                                     ResultSupportTarget::Descriptor, 0}))));
      const auto source_shape =
          phase.query.inputs[0].result_schema->tensors[0].sample_shape();
      if (tiled) {
        std::vector<ResultMappedAxis> axes(shape.size() + 1);
        for (std::size_t axis = 0; axis < shape.size(); ++axis)
          axes[axis].output_axis = axis;
        axes.back().extent = 3;
        relation = math_take(ResultRelation::mapped(
            phase.resources, shape, Region::whole(shape), source_shape, axes,
            {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
      } else {
        relation = math_take(ResultRelation::cartesian(
            phase.resources, math_take(schema.tensors[0].sample_count()),
            {0, 5, 0,
             math_take(phase.query.inputs[0]
                           .result_schema->tensors[0]
                           .sample_count()),
             ResultSupportTarget::Tensor, 0}));
      }
    }
    if (waiting) {
      waiting = false;
      auto source = current.dimensions();
      source.push_back({0, 3});
      auto window = math_take(phase.tensors->at({0, 0}).acquire(
          Region(std::move(source)), phase.query.cancellation));
      auto computed = gpu ? execute_perlin_gpu(phase, window)
                          : compute_cpu(phase, window, current, tiled);
      auto buffer = math_take(std::move(computed));
      StridedLayout layout;
      std::int64_t stride =
          Value::element_size(schema.tensors[0].descriptor.element_type);
      layout.byte_strides.resize(shape.size());
      layout.origin.resize(shape.size());
      for (std::size_t axis = shape.size(); axis-- > 0;) {
        layout.byte_strides[axis] = stride;
        layout.origin[axis] = current.dimensions()[axis].offset;
        stride *= current.dimensions()[axis].extent;
      }
      math_require(builder->publish_tensor(
          0, current, layout, std::move(buffer).freeze(), relation,
          {true, true, true, true}, phase.query.cancellation));
      if (tiled && box < outputs.boxes().size()) {
        return Answer(ResultPublication{builder->reference(), false});
      }
    }
    if (next_region(phase)) {
      waiting = true;
      auto dims = current.dimensions();
      dims.push_back({0, 3});
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0,
           math_take(Footprint::from_regions(
               phase.query.inputs[0].result_schema->tensors[0].sample_shape(),
               {Region(std::move(dims))})),
           descriptor_requested ? 5U : 13U});
      descriptor_requested = true;
      return Answer(std::move(need));
    }
    return Answer(ResultPublication{math_take(builder->seal()), true});
  } catch (const Status& status) {
    return Result<ResultProgramPoll>(status);
  }
};
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
    traits.input_schema[0].kind = OperationPortKind::Result;
    traits.input_schema[0].element_type_mask = 12;
    traits.requires_metadata_specialization = true;
    traits.supports_cpu = !gpu;
    traits.supports_gpu = gpu;
    traits.allows_cpu_fallback = false;
    traits.cpu_staged_tiles = tiled;
    traits.workspace_bytes =
        gpu ? kPerlinGpuWorkspace : 64 * sizeof(PerlinSlot);
    traits.workspace_input_multiplier = tiled ? 6 : 0;
    traits.parameter_schema = {
        {"dtype", OperationParameterType::String, false}};
    numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                         sizeof(PerlinProgram));
    if (tiled) {
      traits.outputs[0].region_rule = OperationRegionRule::Dependency;
      traits.outputs[0].maximum_dependency_stages = 1048576;
    }
    operation.specialize_metadata = [tiled](const auto& inputs,
                                            const auto& parameters)
        -> Result<std::vector<OperationOutputSpecialization>> {
      using Answer = Result<std::vector<OperationOutputSpecialization>>;
      const auto& input = *inputs[0].result_schema;
      if (!input.fields.empty() || input.tensors.size() != 1)
        return Answer(
            Status{ErrorCode::TypeMismatch, "Perlin requires one tensor"});
      auto shape = input.tensors[0].sample_shape();
      if (shape.size() < 2 || shape.size() > 8 || shape.back() != 3)
        return Answer(Status{ErrorCode::TypeMismatch,
                             "Perlin requires coordinates[S...,3]"});
      std::uint64_t count = 1;
      for (auto extent : shape) {
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
      shape.pop_back();
      auto schema = numeric_ops::numeric_tensor_schema(
          dtype != parameters.end() &&
                  std::get<std::string>(dtype->second) == "float32"
              ? ElementType::Float32
              : ElementType::Float64,
          shape);
      if (tiled)
        schema.publication = PublishPolicy::IndependentChunks;
      OperationOutputSpecialization result;
      result.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(schema));
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    };
    operation.start_result = [tiled, gpu](const auto&, const auto& allocator) {
      return ResultContinuation::make<PerlinProgram>(allocator, tiled, gpu);
    };
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
