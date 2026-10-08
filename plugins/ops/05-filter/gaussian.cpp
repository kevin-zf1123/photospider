#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/image_program.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "05-filter/gaussian_coefficients.hpp"
#include "05-filter/gaussian_exact.hpp"
#include "05-filter/gaussian_gpu.hpp"
#include "data/result_window_access.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using filter_ops::GaussianCoefficients;
using filter_ops::GaussianExact;
enum class Boundary { Constant, Clamp, Wrap, Half, Whole };
struct Parameters final {
  std::uint64_t sx, sy, rx, ry, cval;
  unsigned x, y;
  Boundary boundary;
};
std::uint64_t raw(double value) {
  std::uint64_t result;
  std::memcpy(&result, &value, 8);
  return result;
}
Parameters parameters(const std::map<std::string, ParameterValue>& values) {
  const auto& mode = std::get<std::string>(values.at("boundary"));
  return {
      raw(std::get<double>(values.at("sigma_x"))),
      raw(std::get<double>(values.at("sigma_y"))),
      static_cast<std::uint64_t>(std::get<std::int64_t>(values.at("radius_x"))),
      static_cast<std::uint64_t>(std::get<std::int64_t>(values.at("radius_y"))),
      raw(std::get<double>(values.at("cval"))),
      static_cast<unsigned>(std::get<std::int64_t>(values.at("x_axis"))),
      static_cast<unsigned>(std::get<std::int64_t>(values.at("y_axis"))),
      mode == "constant"       ? Boundary::Constant
      : mode == "clamp"        ? Boundary::Clamp
      : mode == "wrap"         ? Boundary::Wrap
      : mode == "reflect_half" ? Boundary::Half
                               : Boundary::Whole};
}
// Wide index arithmetic covers an Int64 radius plus the complete logical
// coordinate. Repeated boundary taps retain their original kernel order.
bool mapped(__int128 index, std::uint64_t extent, Boundary boundary,
            std::uint64_t* output) {
  if (index >= 0 && index < extent) {
    *output = static_cast<std::uint64_t>(index);
    return true;
  }
  if (boundary == Boundary::Constant)
    return false;
  if (boundary == Boundary::Clamp) {
    *output = index < 0 ? 0 : extent - 1;
    return true;
  }
  if (extent == 1) {
    *output = 0;
    return true;
  }
  const auto period = boundary == Boundary::Wrap   ? extent
                      : boundary == Boundary::Half ? 2 * extent
                                                   : 2 * extent - 2;
  auto remainder = index % period;
  if (remainder < 0)
    remainder += period;
  const auto t = static_cast<std::uint64_t>(remainder);
  *output = t < extent ? t : period - t - (boundary == Boundary::Half ? 1 : 0);
  return true;
}
struct PreparedKernel final {
  CancellationToken cancellation;
  Parameters p;
  std::uint64_t* x;
  std::uint64_t* y;
  void* arena;
  std::uint64_t rx = 0, ry = 0;
  Status status{};
};
Status generate(PreparedKernel& work,
                const core_internal::WorkConsumer& consume) {
  auto* math = new (work.arena) GaussianCoefficients(consume);
  struct Destroy {
    GaussianCoefficients* value;
    ~Destroy() { value->~GaussianCoefficients(); }
  } destroy{math};
  for (unsigned axis = 0; axis < 2; ++axis) {
    const auto radius = axis ? work.p.ry : work.p.rx;
    const auto sigma = axis ? work.p.sy : work.p.sx;
    auto* output = axis ? work.y : work.x;
    auto& active = axis ? work.ry : work.rx;
    for (std::uint64_t j = 0; j <= radius; ++j) {
      auto coefficient = math->coefficient(sigma, j, consume);
      if (!coefficient.ok())
        return coefficient.status();
      // Positive Gaussian coefficients decrease with |j|; RN64 is monotone.
      // Once zero is certified, later taps are zero and never participate.
      if (!coefficient.value())
        break;
      auto status = consume(2);
      if (!status.ok())
        return status;
      output[radius - j] = output[radius + j] = coefficient.value();
      active = j;
    }
  }
  return Status::success();
}
struct Slot final {
  GaussianExact math;
  Status status{};
  bool normalized = false;
  std::vector<std::uint64_t> coordinate, source;
};
Result<std::uint64_t> point_work(const Parameters& p, std::uint64_t nx,
                                 std::uint64_t ny, std::size_t rank) {
  auto bound = (!p.rx && !p.ry) ? Result<std::uint64_t>(UINT64_C(0))
                                : GaussianExact::work_bound(nx, ny);
  if (!bound.ok())
    return bound;
  const auto extra =
      static_cast<unsigned __int128>(nx) * ny * (rank * 4 + 16) + rank * 4 + 1;
  const auto total = static_cast<unsigned __int128>(bound.value()) + extra;
  return total > UINT64_MAX
             ? Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted,
                                            "Gaussian sample work overflow"})
             : Result<std::uint64_t>(static_cast<std::uint64_t>(total));
}
template <class Read>
Result<std::uint64_t> evaluate_point(
    Slot* slot, const Parameters& p, const std::uint64_t* kx, std::uint64_t nx,
    const std::uint64_t* ky, std::uint64_t ny,
    const std::vector<std::uint64_t>& shape, const std::uint64_t* coordinate,
    const Read& read, bool narrow, const core_internal::WorkConsumer& consume) {
  using Answer = Result<std::uint64_t>;
  if (!p.rx && !p.ry)
    return read(coordinate[p.y], coordinate[p.x]);
  auto status = slot->normalized
                    ? slot->math.reset(nx, ny, narrow, consume)
                    : slot->math.begin(kx, nx, ky, ny, narrow, consume);
  if (!status.ok())
    return Answer(status);
  slot->normalized = true;
  for (std::uint64_t j = 0; j < ny; ++j)
    for (std::uint64_t i = 0; i < nx; ++i) {
      status = consume(shape.size() * 4 + 16);
      if (!status.ok())
        return Answer(status);
      std::uint64_t y, x;
      const bool inside_y =
          mapped(static_cast<__int128>(coordinate[p.y]) + (ny - 1) / 2 - j,
                 shape[p.y], p.boundary, &y);
      const bool inside_x =
          mapped(static_cast<__int128>(coordinate[p.x]) + (nx - 1) / 2 - i,
                 shape[p.x], p.boundary, &x);
      const bool inside = inside_y && inside_x;
      auto sample = inside ? read(y, x) : Answer(p.cval);
      if (!sample.ok())
        return sample;
      status = slot->math.add(kx[i], ky[j], sample.value(), inside && narrow,
                              consume);
      if (!status.ok())
        return Answer(status);
    }
  return slot->math.finish(consume);
}
using numeric_ops::math_require;
using numeric_ops::math_take;
struct InputRead final {
  ResultTensorReadWindow window;
  std::optional<Value> affine;
};
struct Work final {
  const ResultProgramPhase& phase;
  const PreparedKernel& kernel;
  const ResourceVector<InputRead>& inputs;
  const std::vector<std::uint64_t>& shape;
  const Region& output_region;
  Slot* slots;
  std::uint8_t* output;
  std::uint64_t per_sample, lookup_work;
  bool narrow;
};
Status calculate(Work& work, std::uint64_t begin, std::uint64_t end,
                 Slot* slot) {
  const auto& shape = work.shape;
  const auto& p = work.kernel.p;
  const auto rank = shape.size();
  const auto width = work.narrow ? 4U : 8U;
  const auto* kx = work.kernel.x + p.rx - work.kernel.rx;
  const auto* ky = work.kernel.y + p.ry - work.kernel.ry;
  const auto nx = 2 * work.kernel.rx + 1, ny = 2 * work.kernel.ry + 1;
  std::uint64_t credit = work.per_sample * (end - begin);
  const auto consume = [&](std::uint64_t amount) {
    if (work.phase.query.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "Gaussian cancelled"};
    if (amount > credit)
      return Status{ErrorCode::Internal, "Gaussian work bound exceeded"};
    credit -= amount;
    return Status::success();
  };
  auto& coordinate = slot->coordinate;
  auto& source = slot->source;
  for (auto index = begin; index < end; ++index) {
    auto status = consume(rank * 4 + 1);
    if (!status.ok())
      return status;
    auto remainder = index;
    for (std::size_t axis = rank; axis-- > 0;) {
      const auto d = work.output_region.dimensions()[axis];
      coordinate[axis] = d.offset + remainder % d.extent;
      remainder /= d.extent;
    }
    source = coordinate;
    const auto read = [&](std::uint64_t y,
                          std::uint64_t x) -> Result<std::uint64_t> {
      auto charged = consume(work.lookup_work);
      if (!charged.ok())
        return Result<std::uint64_t>(charged);
      source[p.y] = y;
      source[p.x] = x;
      for (const auto& input : work.inputs) {
        bool contains = true;
        for (std::size_t axis = 0; axis < rank; ++axis) {
          const auto d = input.window.region().dimensions()[axis];
          contains &=
              source[axis] >= d.offset && source[axis] - d.offset < d.extent;
        }
        if (!contains)
          continue;
        std::uint64_t bits = 0;
        if (input.affine) {
          const auto& layout = input.affine->layout();
          std::uint64_t offset = layout.byte_offset;
          for (std::size_t axis = 0; axis < rank; ++axis)
            offset += (source[axis] -
                       (layout.origin.empty() ? 0 : layout.origin[axis])) *
                      static_cast<std::uint64_t>(layout.byte_strides[axis]);
          std::memcpy(&bits, input.affine->bytes().data() + offset, width);
        } else {
          auto row = input.window.row_run(source);
          if (!row.ok())
            return Result<std::uint64_t>(row.status());
          std::memcpy(&bits, row.value().data, width);
        }
        return Result<std::uint64_t>(bits);
      }
      return Result<std::uint64_t>(
          Status{ErrorCode::InvalidArgument,
                 "Gaussian read exceeds authorized halo",
                 FailureReason::UnauthorizedRead,
                 {FailureOrigin::Protocol, FailureScope::Group}});
    };
    auto result = evaluate_point(slot, p, kx, nx, ky, ny, shape,
                                 coordinate.data(), read, work.narrow, consume);
    if (!result.ok())
      return result.status();
    const auto bits = result.value();
    std::memcpy(work.output + index * width, &bits, width);
  }
  return Status::success();
}
int block(void* user, std::uint64_t begin, std::uint64_t end,
          std::uint32_t index) noexcept {
  auto& work = *static_cast<Work*>(user);
  auto& status = work.slots[index].status;
  try {
    status = calculate(work, begin, end, &work.slots[index]);
  } catch (const Status& error) {
    status = error;
  } catch (const std::bad_alloc&) {
    status.code = ErrorCode::ResourceExhausted;
  } catch (...) {
    status.code = ErrorCode::OperationFailed;
  }
  return status.ok()                                   ? 0
         : status.code == ErrorCode::Cancelled         ? 2
         : status.code == ErrorCode::ResourceExhausted ? 4
                                                       : 1;
}
int tile(void* user, const ps_cpu_tile_v1* region) noexcept {
  return block(user, region->begin[0], region->end[0], region->slot);
}
Result<MutableBuffer> compute_cpu(const ResultProgramPhase& phase,
                                  const PreparedKernel& kernel,
                                  const Region& region) {
  using Answer = Result<MutableBuffer>;
  const auto& input = phase.tensors->at({0, 0});
  const auto shape = input.spec().sample_shape();
  ResourceVector<InputRead> windows{
      ResourceAllocator<InputRead>(phase.resources)};
  std::uint64_t lookup_work =
      input.coverage().boxes().size() * (shape.size() + 1);
  for (const auto& box : input.coverage().boxes()) {
    auto window = math_take(input.acquire(box, phase.query.cancellation));
    auto affine = execution_internal::ResultWindowAccess::affine(window);
    if (!affine.ok() && affine.status().code != ErrorCode::NotFound)
      return Answer(affine.status());
    const auto cost =
        affine.ok()
            ? shape.size() * 4 + 1
            : math_take(
                  execution_internal::ResultWindowAccess::read_work(window));
    if (cost > UINT64_MAX - lookup_work)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    lookup_work += cost;
    windows.push_back({std::move(window),
                       affine.ok() ? std::optional<Value>(affine.take_value())
                                   : std::nullopt});
  }
  const auto count = math_take(region.element_count());
  const auto nx = 2 * kernel.rx + 1, ny = 2 * kernel.ry + 1;
  const auto core = math_take(point_work(kernel.p, nx, ny, shape.size()));
  const auto per_sample = static_cast<unsigned __int128>(core) +
                          static_cast<unsigned __int128>(nx) * ny * lookup_work;
  if (per_sample > UINT64_MAX || count > UINT64_MAX / per_sample)
    return Answer(
        Status{ErrorCode::ResourceExhausted, "Gaussian work overflow"});
  math_require(
      phase.consume_work(count * static_cast<std::uint64_t>(per_sample)));
  const bool narrow =
      input.spec().descriptor.element_type == ElementType::Float32;
  auto output =
      math_take(phase.resources.allocator().allocate(count * (narrow ? 4 : 8)));
  const auto workers = phase.cpu_parallel ? phase.cpu_parallel->maximum_workers
                       : phase.cpu_tiles  ? phase.cpu_tiles->maximum_workers
                                          : 1;
  const auto grant =
      static_cast<unsigned>(std::min<std::uint64_t>(count, workers));
  math_require(
      phase.consume_work(grant * ((sizeof(Slot) + 7) / 8 + shape.size() * 2)));
  auto scratch = math_take(phase.allocator.allocate(grant * sizeof(Slot)));
  auto metadata = math_take(phase.resources.reserve(ResourceCapacity::host(
      grant * shape.size() * 16, grant * shape.size() * 16)));
  auto* slots = reinterpret_cast<Slot*>(scratch.data());
  unsigned constructed = 0;
  struct Destroy {
    Slot* slots;
    unsigned& count;
    ~Destroy() {
      while (count)
        slots[--count].~Slot();
    }
  } destroy{slots, constructed};
  while (constructed < grant) {
    auto& slot = *new (slots + constructed++) Slot;
    slot.coordinate.resize(shape.size());
    slot.source.resize(shape.size());
  }
  Work work{phase,         kernel,
            windows,       shape,
            region,        slots,
            output.data(), static_cast<std::uint64_t>(per_sample),
            lookup_work,   narrow};
  int code = 0;
  if (phase.cpu_tiles) {
    const ps_cpu_tile_stage_v1 stage{sizeof(ps_cpu_tile_stage_v1),
                                     {count, 1, 1},
                                     {4, 1, 1},
                                     grant};
    code = phase.cpu_tiles->run(phase.cpu_tiles->context, &stage, tile, &work);
  } else if (phase.cpu_parallel) {
    code = phase.cpu_parallel->run(phase.cpu_parallel->context, count, 4, grant,
                                   block, &work);
  } else {
    code = block(&work, 0, count, 0);
  }
  for (unsigned i = 0; i < grant; ++i)
    if (!slots[i].status.ok())
      return Answer(slots[i].status);
  if (code)
    return Answer(Status{code == 2   ? ErrorCode::Cancelled
                         : code == 4 ? ErrorCode::ResourceExhausted
                                     : ErrorCode::OperationFailed,
                         "Gaussian CPU stage failed"});
  return Answer(std::move(output));
}
struct GaussianProgram final {
  bool tiled, gpu, initialized = false, waiting = false,
                   descriptor_requested = false;
  Parameters p;
  std::shared_ptr<const CpuStorage> coefficients;
  std::uint64_t rx = 0, ry = 0;
  Footprint outputs;
  ResultRelation data, witness;
  std::optional<ResultBuilder> builder;
  Region current;
  std::size_t box = 0;
  std::array<std::uint64_t, 8> next{};
  GaussianProgram(bool tiles, bool device, Parameters values)
      : tiled(tiles), gpu(device), p(values) {}
  bool next_region(const ResultProgramPhase& phase) {
    if (box >= outputs.boxes().size())
      return false;
    const auto& spec = phase.query.output.result_schema->tensors[0];
    const auto tuple =
        input_internal::tuple_channel_axis(spec.descriptor, spec.facets);
    const auto& bounds = outputs.boxes()[box].dimensions();
    std::vector<RegionDimension> dims;
    for (std::size_t axis = 0; axis < bounds.size(); ++axis) {
      const bool atomic = axis >= bounds.size() - spec.atomic_trailing_axes ||
                          (tuple && axis == *tuple + spec.batch_axes.size());
      const auto extent = !tiled || atomic ? bounds[axis].extent
                          : axis == p.x    ? phase.query.tile_width
                          : axis == p.y    ? phase.query.tile_height
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
    auto metadata = math_take(
        phase.resources.reserve(ResourceCapacity::host(16384, 16384)));
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    if (!initialized) {
      initialized = true;
      outputs = phase.query.tensor_outputs ? *phase.query.tensor_outputs
                                           : math_take(Footprint::all(shape));
      builder.emplace(math_take(ResultBuilder::start(
          phase.resources, schema, phase.query.semantic_key, {}, {},
          phase.query.tile_height, phase.query.tile_width,
          phase.query.resources)));
      math_require(builder->bind_descriptor_relation(math_take(
          ResultRelation::cartesian(phase.resources, 1,
                                    {0, 8, 0, outputs.empty() ? 0U : 1U,
                                     ResultSupportTarget::Descriptor, 0}))));
      if (outputs.empty())
        return Answer(ResultPublication{math_take(builder->seal()), true});
      const auto nx = 2 * p.rx + 1, ny = 2 * p.ry + 1;
      math_require(phase.consume_work(nx + ny));
      auto table = math_take(phase.allocator.allocate((nx + ny) * 8));
      auto arena =
          math_take(phase.allocator.allocate(sizeof(GaussianCoefficients)));
      auto* x = reinterpret_cast<std::uint64_t*>(table.data());
      PreparedKernel kernel{phase.query.cancellation, p, x, x + nx,
                            arena.data()};
      math_require(generate(kernel, phase.consume_work));
      rx = kernel.rx;
      ry = kernel.ry;
      coefficients = std::move(table).freeze();
      if (tiled) {
        std::vector<std::uint64_t> radii(shape.size());
        radii[p.x] = rx;
        radii[p.y] = ry;
        data = math_take(ResultRelation::neighborhood(
            phase.resources, shape, radii, p.boundary == Boundary::Wrap,
            {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
        const auto& input = phase.query.inputs[0].result_schema->tensors[0];
        const auto tuple =
            input_internal::tuple_channel_axis(input.descriptor, input.facets);
        if (tuple)
          radii[*tuple + input.batch_axes.size()] =
              shape[*tuple + input.batch_axes.size()] - 1;
        for (std::size_t axis = shape.size() - input.atomic_trailing_axes;
             axis < shape.size(); ++axis)
          radii[axis] = shape[axis] - 1;
        auto validation = math_take(ResultRelation::neighborhood(
            phase.resources, shape, radii, p.boundary == Boundary::Wrap,
            {0, 4, 0, 0, ResultSupportTarget::Tensor, 0}));
        witness = math_take(
            ResultRelation::unite(phase.resources, {data, validation}));
      } else {
        witness = math_take(ResultRelation::cartesian(
            phase.resources, math_take(schema.tensors[0].sample_count()),
            {0, 5, 0, math_take(schema.tensors[0].sample_count()),
             ResultSupportTarget::Tensor, 0}));
      }
    }
    if (waiting) {
      waiting = false;
      const auto nx = 2 * p.rx + 1;
      const auto* x =
          reinterpret_cast<const std::uint64_t*>(coefficients->bytes().data());
      PreparedKernel kernel{phase.query.cancellation,
                            p,
                            const_cast<std::uint64_t*>(x),
                            const_cast<std::uint64_t*>(x + nx),
                            nullptr,
                            rx,
                            ry};
      auto computed =
          gpu ? execute_gaussian_gpu(
                    phase,
                    math_take(phase.tensors->at({0, 0}).acquire(
                        Region::whole(shape), phase.query.cancellation)),
                    {coefficients->bytes().data(), coefficients->bytes().size(),
                     (p.rx - rx) * 8, (nx + p.ry - ry) * 8, 2 * rx + 1,
                     2 * ry + 1, p.cval, p.x, p.y,
                     static_cast<std::uint32_t>(p.boundary), !p.rx && !p.ry})
              : compute_cpu(phase, kernel, current);
      auto buffer = math_take(std::move(computed));
      StridedLayout layout;
      layout.origin.resize(shape.size());
      layout.byte_strides.resize(shape.size());
      std::int64_t stride =
          Value::element_size(schema.tensors[0].descriptor.element_type);
      for (std::size_t axis = shape.size(); axis-- > 0;) {
        layout.origin[axis] = current.dimensions()[axis].offset;
        layout.byte_strides[axis] = stride;
        stride *= current.dimensions()[axis].extent;
      }
      math_require(builder->publish_tensor(
          0, current, layout, std::move(buffer).freeze(), witness,
          {true, true, true, true}, phase.query.cancellation));
      if (tiled && box < outputs.boxes().size())
        return Answer(ResultPublication{builder->reference(), false});
    }
    if (next_region(phase)) {
      waiting = true;
      auto needed = math_take(Footprint::all(shape));
      if (tiled) {
        auto requested = math_take(Footprint::from_regions(shape, {current}));
        FootprintLimits limits;
        limits.cancellation = phase.query.cancellation;
        limits.consume_work = phase.consume_work;
        math_require(data.project(
            requested,
            [&](auto, const Footprint* samples) {
              if (!samples)
                return Status{ErrorCode::Internal,
                              "Gaussian neighborhood lost shape"};
              needed = *samples;
              return Status::success();
            },
            limits));
      }
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0, std::move(needed), descriptor_requested ? 5U : 13U});
      descriptor_requested = true;
      return Answer(std::move(need));
    }
    return Answer(ResultPublication{math_take(builder->seal()), true});
  } catch (const Status& status) {
    return Result<ResultProgramPoll>(status);
  }
};

Result<ResultProgramPoll> empty_result(const ResultProgramPhase& phase) {
  using Answer = Result<ResultProgramPoll>;
  if (!phase.query.tensor_outputs || !phase.query.tensor_outputs->empty())
    return Answer(Status{ErrorCode::InvalidArgument,
                         "empty Gaussian continuation demand changed"});
  auto builder = ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {}, {}, phase.query.tile_height,
      phase.query.tile_width, phase.query.resources);
  if (!builder.ok())
    return Answer(builder.status());
  auto relation = ResultRelation::cartesian(phase.resources, 1, {});
  if (!relation.ok())
    return Answer(relation.status());
  auto output = builder.take_value();
  auto status = output.bind_descriptor_relation(relation.take_value());
  if (!status.ok())
    return Answer(status);
  auto result = output.seal();
  return result.ok() ? Answer(ResultPublication{result.take_value(), true})
                     : Answer(result.status());
}
}  // namespace
Status register_gaussian(OperationRegistry* registry) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    const bool tiled = mode == 1, gpu = mode == 2;
    OperationDefinition operation;
    operation.key = gpu     ? "filter.gaussian_baked64_v1_strict_gpu"
                    : tiled ? "filter.gaussian_baked64_v1_strict_cpu_tiled"
                            : "filter.gaussian_baked64_v1_strict_cpu_whole";
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
    traits.workspace_bytes = sizeof(GaussianCoefficients) +
                             (gpu ? kGaussianGpuWorkspace : 64 * sizeof(Slot));
    for (const auto* name : {"sigma_x", "sigma_y", "cval"})
      traits.parameter_schema.push_back(
          {name, OperationParameterType::Float64, true});
    for (const auto* name : {"radius_x", "radius_y", "x_axis", "y_axis"})
      traits.parameter_schema.push_back(
          {name, OperationParameterType::Int64, true});
    traits.parameter_schema.push_back(
        {"boundary", OperationParameterType::String, true});
    numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                         sizeof(GaussianProgram));
    auto& output = traits.outputs[0];
    output.key = "output";
    output.output_schema.result_schema_id.clear();
    output.output_schema.result_schema_version = 0;
    output.output_schema.element_type_mask = 12;
    output.region_rule =
        tiled ? OperationRegionRule::Dependency : OperationRegionRule::Whole;
    output.maximum_dependency_stages = tiled ? 1048576 : 2;
    operation.prepare_static =
        [tiled](const auto& inputs,
                const auto& values) -> Result<OperationPreparation> {
      using Answer = Result<OperationPreparation>;
      const auto& schema = *inputs[0].result_schema;
      if (!schema.fields.empty() || schema.tensors.size() != 1)
        return Answer(
            Status{ErrorCode::TypeMismatch, "Gaussian requires one tensor"});
      const ValueDescriptor descriptor{
          schema.tensors[0].descriptor.element_type,
          schema.tensors[0].sample_shape()};
      const auto rank = descriptor.shape.size();
      if (rank < 2 || rank > 8)
        return Answer(
            Status{ErrorCode::TypeMismatch, "Gaussian rank must be 2..8"});
      std::uint64_t count = 1;
      for (const auto extent : descriptor.shape) {
        if (!extent || extent > (UINT64_C(1) << 40) / count)
          return Answer(Status{ErrorCode::ResourceExhausted,
                               "Gaussian shape exceeds 2^40 elements"});
        count *= extent;
      }
      for (const auto* name : {"x_axis", "y_axis"}) {
        const auto axis = std::get<std::int64_t>(values.at(name));
        if (axis < 0 || static_cast<std::uint64_t>(axis) >= rank)
          return Answer(Status{ErrorCode::InvalidArgument,
                               "Gaussian spatial axis is out of range"});
      }
      for (const auto* name : {"radius_x", "radius_y"})
        if (std::get<std::int64_t>(values.at(name)) < 0)
          return Answer(
              Status{ErrorCode::InvalidArgument, "negative Gaussian radius"});
      const auto p = parameters(values);
      if (p.x == p.y)
        return Answer(Status{ErrorCode::InvalidArgument,
                             "Gaussian spatial axes must differ"});
      const auto& boundary = std::get<std::string>(values.at("boundary"));
      if (boundary != "constant" && boundary != "clamp" && boundary != "wrap" &&
          boundary != "reflect_half" && boundary != "reflect_whole")
        return Answer(
            Status{ErrorCode::InvalidArgument, "unknown Gaussian boundary"});
      for (auto value : {p.sx, p.sy, p.cval}) {
        const auto parts = numeric_ops::BinaryParts::decode(value, false);
        if (parts.nan || parts.infinite)
          return Answer(Status{ErrorCode::InvalidArgument,
                               "nonfinite Gaussian parameter"});
      }
      for (unsigned axis = 0; axis < 2; ++axis) {
        const auto sigma =
            numeric_ops::BinaryParts::decode(axis ? p.sy : p.sx, false);
        if ((sigma.negative && sigma.magnitude) ||
            (!sigma.magnitude && (axis ? p.ry : p.rx)))
          return Answer(Status{ErrorCode::InvalidArgument,
                               "invalid Gaussian sigma/radius pair"});
      }
      const auto nx = 2 * p.rx + 1, ny = 2 * p.ry + 1;
      if (nx > UINT64_MAX / ny || nx > UINT64_MAX - ny ||
          nx + ny > INT64_MAX / 8)
        return Answer(Status{ErrorCode::ResourceExhausted,
                             "Gaussian kernel size overflow"});
      OperationPreparation result;
      OperationOutputSpecialization specialization;
      auto result_schema = schema;
      result_schema.publication = tiled ? PublishPolicy::IndependentChunks
                                        : PublishPolicy::CompleteBundle;
      specialization.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(result_schema));
      result.state = std::make_shared<const Parameters>(p);
      result.outputs.push_back(std::move(specialization));
      result.additional_workspace_bytes = (nx + ny) * 8;
      return Answer(std::move(result));
    };
    operation.start_result = [tiled, gpu](const ResultProgramQuery& query,
                                          const BufferAllocator& allocator) {
      if (query.tensor_outputs && query.tensor_outputs->empty())
        return ResultContinuation::stateless<empty_result>();
      if (!query.prepared || !query.prepared->state())
        return Result<ResultContinuation>(
            Status{ErrorCode::Internal, "missing Gaussian preparation"});
      return ResultContinuation::make<GaussianProgram>(
          allocator, tiled, gpu,
          *static_cast<const Parameters*>(query.prepared->state()));
    };
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  return image_ops::register_image_algorithm(
      registry, "image.gaussian_blur", image_ops::ImageAlgorithm::Gaussian);
}
}  // namespace ps::plugin_internal
