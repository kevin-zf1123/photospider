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

#include "01-numeric/array_publication.hpp"
#include "05-filter/gaussian_coefficients.hpp"
#include "05-filter/gaussian_exact.hpp"
#include "05-filter/gaussian_gpu.hpp"
#include "photospider/execution/resource_allocator.hpp"
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
Status charge(const CancellationToken& cancellation, std::uint64_t amount) {
  if (cancellation.cancelled())
    return {ErrorCode::Cancelled, "Gaussian cancelled"};
  const auto* budget = resource_internal::metadata_budget();
  return budget ? budget->consume({amount}) : Status::success();
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
                const execution_internal::WorkConsumer& consume) {
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
int prepare_block(void* user, std::uint64_t, std::uint64_t,
                  std::uint32_t) noexcept {
  auto& work = *static_cast<PreparedKernel*>(user);
  try {
    const auto consume = [&](std::uint64_t amount) {
      return charge(work.cancellation, amount);
    };
    work.status = generate(work, consume);
  } catch (const Status& status) {
    work.status = status;
  } catch (const std::bad_alloc&) {
    work.status.code = ErrorCode::ResourceExhausted;
  } catch (...) {
    work.status.code = ErrorCode::OperationFailed;
  }
  return work.status.ok()                                   ? 0
         : work.status.code == ErrorCode::Cancelled         ? 2
         : work.status.code == ErrorCode::ResourceExhausted ? 4
                                                            : 1;
}
struct Slot final {
  GaussianExact math;
  Status status{};
};
struct Work final {
  const OperationInvocation& call;
  const PreparedKernel& kernel;
  Slot* slots;
  std::uint8_t* output;
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
    const Read& read, bool narrow,
    const execution_internal::WorkConsumer& consume) {
  using Answer = Result<std::uint64_t>;
  if (!p.rx && !p.ry)
    return read(coordinate[p.y], coordinate[p.x]);
  auto status = slot->math.begin(kx, nx, ky, ny, narrow, consume);
  if (!status.ok())
    return Answer(status);
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
Status calculate(Work& work, std::uint64_t begin, std::uint64_t end,
                 Slot* slot) {
  const auto& input = work.call.inputs[0];
  const auto& shape = input.descriptor().shape;
  const auto& layout = input.layout();
  const auto& p = work.kernel.p;
  const auto rank = shape.size();
  const bool narrow = input.descriptor().element_type == ElementType::Float32;
  const auto width = narrow ? 4U : 8U;
  const auto* kx = work.kernel.x + p.rx - work.kernel.rx;
  const auto* ky = work.kernel.y + p.ry - work.kernel.ry;
  const auto nx = 2 * work.kernel.rx + 1, ny = 2 * work.kernel.ry + 1;
  auto bound = point_work(p, nx, ny, rank);
  if (!bound.ok())
    return bound.status();
  const auto per_sample = bound.value();
  if (end - begin > UINT64_MAX / per_sample)
    return {ErrorCode::ResourceExhausted, "Gaussian range work overflow"};
  std::uint64_t credit = static_cast<std::uint64_t>(per_sample) * (end - begin);
  auto admitted = charge(work.call.cancellation, credit);
  if (!admitted.ok())
    return admitted;
  const auto consume = [&](std::uint64_t amount) {
    if (work.call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "Gaussian cancelled"};
    if (amount > credit)
      return Status{ErrorCode::Internal, "Gaussian work bound exceeded"};
    credit -= amount;
    return Status::success();
  };
  for (auto index = begin; index < end; ++index) {
    auto status = consume(rank * 4 + 1);
    if (!status.ok())
      return status;
    std::array<std::uint64_t, 8> coordinate{};
    auto remainder = index;
    for (std::size_t axis = rank; axis-- > 0;) {
      coordinate[axis] = remainder % shape[axis];
      remainder /= shape[axis];
    }
    const auto read = [&](std::uint64_t y, std::uint64_t x) {
      __int128 address = layout.byte_offset;
      for (std::size_t axis = 0; axis < rank; ++axis) {
        const auto at = axis == p.y ? y : axis == p.x ? x : coordinate[axis];
        const auto origin = layout.origin.empty() ? 0 : layout.origin[axis];
        address +=
            (static_cast<__int128>(at) - origin) * layout.byte_strides[axis];
      }
      std::uint64_t result = 0;
      std::memcpy(&result,
                  input.bytes().data() + static_cast<std::size_t>(address),
                  width);
      return Result<std::uint64_t>(result);
    };
    auto result = evaluate_point(slot, p, kx, nx, ky, ny, shape,
                                 coordinate.data(), read, narrow, consume);
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
Result<Value> execute(const OperationInvocation& call) {
  using Answer = Result<Value>;
  const auto p = parameters(call.parameters);
  const auto nx = 2 * p.rx + 1, ny = 2 * p.ry + 1;
  auto charged = charge(call.cancellation, nx + ny);
  if (!charged.ok())
    return Answer(charged);
  auto storage = call.allocator.allocate((nx + ny) * 8);
  if (!storage.ok())
    return Answer(storage.status());
  auto coefficients = storage.take_value();
  storage = call.allocator.allocate(sizeof(GaussianCoefficients));
  if (!storage.ok())
    return Answer(storage.status());
  auto arena = storage.take_value();
  auto* x = reinterpret_cast<std::uint64_t*>(coefficients.data());
  PreparedKernel kernel{call.cancellation, p, x, x + nx, arena.data()};
  const auto* parallel = call.cpu_parallel;
  const auto prepared = parallel ? parallel->run(parallel->context, 1, 1, 1,
                                                 prepare_block, &kernel)
                                 : prepare_block(&kernel, 0, 1, 0);
  if (!kernel.status.ok())
    return Answer(kernel.status);
  if (prepared)
    return Answer(Status{
        prepared == 2 ? ErrorCode::Cancelled : ErrorCode::OperationFailed,
        "Gaussian kernel preparation failed"});
  arena = {};
  if (call.backend == Backend::Gpu)
    return execute_gaussian_gpu(
        call, {coefficients.data(), coefficients.size(), (p.rx - kernel.rx) * 8,
               (nx + p.ry - kernel.ry) * 8, 2 * kernel.rx + 1,
               2 * kernel.ry + 1, p.cval, p.x, p.y,
               static_cast<std::uint32_t>(p.boundary), !p.rx && !p.ry});
  auto output = MutableValue::allocate(call.inputs[0].descriptor(),
                                       call.output_region, call.allocator);
  if (!output.ok())
    return Answer(output.status());
  auto writer = output.take_value();
  const auto count = call.output_region.element_count().value();
  const auto grant = static_cast<unsigned>(
      std::min<std::uint64_t>(count, parallel ? parallel->maximum_workers : 1));
  storage = call.allocator.allocate(grant * sizeof(Slot));
  if (!storage.ok())
    return Answer(storage.status());
  auto scratch = storage.take_value();
  auto* slots = reinterpret_cast<Slot*>(scratch.data());
  unsigned constructed = 0;
  struct Destroy {
    Slot* slots;
    unsigned& count;
    ~Destroy() {
      for (unsigned i = 0; i < count; ++i)
        slots[i].~Slot();
    }
  } destroy{slots, constructed};
  static_assert((sizeof(Slot) + 7) / 8 <= 1024);
  for (unsigned i = 0; i < grant; ++i) {
    charged = charge(call.cancellation, (sizeof(Slot) + 7) / 8);
    if (!charged.ok())
      return Answer(charged);
    new (slots + i) Slot;
    ++constructed;
  }
  Work work{call, kernel, slots, writer.data()};
  const auto code =
      parallel ? parallel->run(parallel->context, count, 4, grant, block, &work)
               : block(&work, 0, count, 0);
  for (unsigned i = 0; i < grant; ++i)
    if (!slots[i].status.ok())
      return Answer(slots[i].status);
  if (code)
    return Answer(
        Status{code == 2 ? ErrorCode::Cancelled : ErrorCode::OperationFailed,
               "Gaussian host range failed"});
  return std::move(writer).publish(call.inputs[0].facets(),
                                   call.inputs[0].resources());
}
struct AxisSupport final {
  std::array<RegionDimension, 2> spans{};
  unsigned count = 1;
};
AxisSupport axis_support(RegionDimension centers, std::uint64_t radius,
                         std::uint64_t extent, Boundary boundary) {
  AxisSupport result;
  if (boundary != Boundary::Wrap) {
    // Reflection adds no point outside the clipped symmetric support. Every
    // interior point is already an unreflected tap of one requested center.
    const auto lower = centers.offset > radius ? centers.offset - radius : 0;
    const auto end = centers.offset + centers.extent;
    const auto upper = radius >= extent - end ? extent : end + radius;
    result.spans[0] = {lower, upper - lower};
    return result;
  }
  const auto length = static_cast<unsigned __int128>(centers.extent) +
                      2 * static_cast<unsigned __int128>(radius);
  if (length >= extent) {
    result.spans[0] = {0, extent};
    return result;
  }
  auto first = (static_cast<__int128>(centers.offset) - radius) % extent;
  if (first < 0)
    first += extent;
  const auto start = static_cast<std::uint64_t>(first);
  const auto count = static_cast<std::uint64_t>(length);
  const auto part = std::min(count, extent - start);
  result.spans[0] = {start, part};
  if (part != count) {
    result.count = 2;
    result.spans[1] = {0, count - part};
  }
  return result;
}
Result<Footprint> support(const Region& samples,
                          const std::vector<std::uint64_t>& shape,
                          const Parameters& p, std::uint64_t rx,
                          std::uint64_t ry, const FootprintLimits& limits) {
  const auto xs =
      axis_support(samples.dimensions()[p.x], rx, shape[p.x], p.boundary);
  const auto ys =
      axis_support(samples.dimensions()[p.y], ry, shape[p.y], p.boundary);
  std::vector<Region> boxes;
  boxes.reserve(xs.count * ys.count);
  for (unsigned y = 0; y < ys.count; ++y)
    for (unsigned x = 0; x < xs.count; ++x) {
      auto dimensions = samples.dimensions();
      dimensions[p.x] = xs.spans[x];
      dimensions[p.y] = ys.spans[y];
      boxes.emplace_back(std::move(dimensions));
    }
  return Footprint::from_regions(shape, boxes, limits);
}
struct GaussianTile final {
  std::shared_ptr<const CpuStorage> coefficients;
  Parameters p{};
  std::uint64_t rx = 0, ry = 0;
  Result<DependencyPoll> prepare(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    p = parameters(phase.query.parameters);
    const auto nx = 2 * p.rx + 1, ny = 2 * p.ry + 1;
    auto status = phase.consume_work(nx + ny);
    if (!status.ok())
      return Answer(status);
    auto made = phase.allocator.allocate((nx + ny) * 8);
    if (!made.ok())
      return Answer(made.status());
    auto table = made.take_value();
    made = phase.allocator.allocate(sizeof(GaussianCoefficients));
    if (!made.ok())
      return Answer(made.status());
    auto arena = made.take_value();
    auto* x = reinterpret_cast<std::uint64_t*>(table.data());
    PreparedKernel kernel{phase.query.cancellation, p, x, x + nx, arena.data()};
    status = generate(kernel, phase.consume_work);
    if (!status.ok())
      return Answer(status);
    rx = kernel.rx;
    ry = kernel.ry;
    coefficients = std::move(table).freeze();
    arena = {};

    const auto& shape = phase.query.output.descriptor.shape;
    const auto count = phase.query.observations.element_count().value();
    // Admit temporary vector element capacity before constructing associations.
    // Footprint and final batch owners independently retain their own metadata.
    dependency_internal::MetadataBytes bytes;
    bytes.add(count, sizeof(AtomCertificate) + sizeof(DependencyNeed) +
                         8 * sizeof(Region) + shape.size() * 256);
    auto owner = dependency_internal::metadata_owner(bytes.bytes);
    status = phase.consume_work(count * (shape.size() * 16 + 64));
    if (!status.ok())
      return Answer(status);
    std::vector<AtomCertificate> rows;
    rows.reserve(count);
    status = phase.query.observations.visit(
        [&](const std::vector<std::uint64_t>& at) {
          auto checked = phase.consume_work(0);
          if (!checked.ok())
            return checked;
          std::vector<RegionDimension> dimensions;
          dimensions.reserve(at.size());
          for (auto coordinate : at)
            dimensions.push_back({coordinate, 1});
          auto atom = Footprint::from_regions(phase.query.observations.shape(),
                                              {Region(std::move(dimensions))},
                                              phase.sets);
          if (!atom.ok())
            return atom.status();
          auto samples =
              observation_samples(phase.query.output, atom.value(), phase.sets);
          if (!samples.ok())
            return samples.status();
          auto needed =
              support(samples.value().boxes()[0], shape, p, rx, ry, phase.sets);
          if (!needed.ok())
            return needed.status();
          rows.push_back({at,
                          {{0,
                            static_cast<std::uint32_t>(DependencyRole::Data),
                            needed.take_value(),
                            {}}}});
          return Status::success();
        },
        phase.sets.maximum_work, phase.query.cancellation);
    if (!status.ok())
      return Answer(status);
    return Answer(DependencyNeedBatch(std::move(rows)));
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) try {
    using Answer = Result<DependencyPoll>;
    if (!coefficients)
      return prepare(phase);
    const auto& descriptor = phase.query.output.descriptor;
    const auto& shape = descriptor.shape;
    const auto rank = shape.size();
    const bool narrow = descriptor.element_type == ElementType::Float32;
    const auto width = narrow ? 4U : 8U;
    const auto* table =
        reinterpret_cast<const std::uint64_t*>(coefficients->bytes().data());
    const auto* kx = table + p.rx - rx;
    const auto* ky = table + 2 * p.rx + 1 + p.ry - ry;
    const auto nx = 2 * rx + 1, ny = 2 * ry + 1;
    const auto count = phase.query.outputs.element_count().value();
    auto bound = point_work(p, nx, ny, rank);
    if (!bound.ok())
      return Answer(bound.status());
    const auto lookup_work = (static_cast<unsigned __int128>(
                                  phase.inputs[0].coverage().boxes().size()) +
                              phase.inputs[0].fragments().size()) *
                                 (rank + 1) +
                             4 * rank + 2;
    const auto sample_work =
        static_cast<unsigned __int128>(bound.value()) +
        static_cast<unsigned __int128>(nx) * ny * lookup_work;
    if (sample_work > UINT64_MAX || count > UINT64_MAX / sample_work)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "Gaussian tile work overflow"});
    std::uint64_t credit = count * static_cast<std::uint64_t>(sample_work);
    auto status = phase.consume_work(credit);
    if (!status.ok())
      return Answer(status);
    const auto consume = [&](std::uint64_t amount) {
      if (phase.query.cancellation.cancelled())
        return Status{ErrorCode::Cancelled, "Gaussian tile cancelled"};
      if (amount > credit)
        return Status{ErrorCode::Internal, "Gaussian tile work bound exceeded"};
      credit -= amount;
      return Status::success();
    };
    auto lookup_limits = phase.sets;
    lookup_limits.cancellation = phase.query.cancellation;
    lookup_limits.consume_work = consume;
    status = phase.consume_work((sizeof(Slot) + 7) / 8);
    if (!status.ok())
      return Answer(status);
    auto made = phase.allocator.allocate(sizeof(Slot));
    if (!made.ok())
      return Answer(made.status());
    auto scratch = made.take_value();
    auto* slot = new (scratch.data()) Slot;
    struct Destroy {
      Slot* value;
      ~Destroy() { value->~Slot(); }
    } destroy{slot};
    auto coordinate_owner =
        dependency_internal::metadata_owner(2 * rank * sizeof(std::uint64_t));
    std::vector<std::uint64_t> coordinate(rank), source(rank);
    dependency_internal::MetadataBytes facet_bytes;
    for (const auto& facet : phase.query.output.facets)
      facet_bytes.add(sizeof(ValueFacet) + facet.key.capacity() + 1 +
                      facet.payload.capacity());
    numeric_ops::ArrayPublication publication(
        phase.query.outputs.boxes().size(), rank, facet_bytes.bytes);
    ResourceVector<Value> values;
    values.reserve(phase.query.outputs.boxes().size());
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto output = allocated.take_value();
      for (std::size_t axis = 0; axis < rank; ++axis)
        coordinate[axis] = box.dimensions()[axis].offset;
      const auto elements = box.element_count().value();
      for (std::uint64_t i = 0; i < elements; ++i) {
        status = phase.consume_work(0);
        if (!status.ok())
          return Answer(status);
        status = consume(rank * 4 + 1);
        if (!status.ok())
          return Answer(status);
        source = coordinate;
        const auto read = [&](std::uint64_t y,
                              std::uint64_t x) -> Result<std::uint64_t> {
          source[p.y] = y;
          source[p.x] = x;
          std::uint64_t bits = 0;
          auto loaded =
              phase.inputs[0].read(source, &bits, width, lookup_limits);
          if (!loaded.ok()) {
            if (loaded.code == ErrorCode::InvalidArgument) {
              loaded.reason = FailureReason::UnauthorizedRead;
              loaded.detail.origin = FailureOrigin::Protocol;
              loaded.detail.scope = FailureScope::Atom;
            }
            return Result<std::uint64_t>(phase.report_failure(loaded));
          }
          return Result<std::uint64_t>(bits);
        };
        auto result = evaluate_point(slot, p, kx, nx, ky, ny, shape,
                                     coordinate.data(), read, narrow, consume);
        if (!result.ok())
          return Answer(result.status());
        const auto bits = result.value();
        std::memcpy(output.data() + i * width, &bits, width);
        for (std::size_t axis = rank; axis-- > 0;) {
          const auto& dimension = box.dimensions()[axis];
          if (++coordinate[axis] < dimension.offset + dimension.extent)
            break;
          coordinate[axis] = dimension.offset;
        }
      }
      auto value = std::move(output).publish(phase.query.output.facets,
                                             phase.query.resources);
      if (!value.ok())
        return Answer(value.status());
      auto retained = publication.retain(value.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      values.push_back(retained.take_value());
    }
    auto result = publication.finish(
        descriptor, phase.query.outputs, values.data(), values.size(),
        phase.sets, phase.query.output.facets, phase.query.resources);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  } catch (const Status& status) {
    return Result<DependencyPoll>(status);
  }
};
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
    traits.input_schema[0].element_type_mask = 12;
    traits.requires_metadata_specialization = true;
    traits.supports_cpu = !gpu;
    traits.supports_gpu = gpu;
    traits.workspace_bytes =
        sizeof(GaussianCoefficients) +
        (gpu ? kGaussianGpuWorkspace : (tiled ? 1 : 64) * sizeof(Slot));
    for (const auto* name : {"sigma_x", "sigma_y", "cval"})
      traits.parameter_schema.push_back(
          {name, OperationParameterType::Float64, true});
    for (const auto* name : {"radius_x", "radius_y", "x_axis", "y_axis"})
      traits.parameter_schema.push_back(
          {name, OperationParameterType::Int64, true});
    traits.parameter_schema.push_back(
        {"boundary", OperationParameterType::String, true});
    auto& output = traits.outputs[0];
    output.key = "output";
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {1};
    output.region_rule =
        tiled ? OperationRegionRule::Dependency : OperationRegionRule::Whole;
    if (tiled) {
      output.dependency_version = 1;
      output.continuation_bytes = sizeof(GaussianTile);
      output.maximum_dependency_stages = 2;
      output.requires_dense_output = false;
      operation.start_dependency = [](const DependencyQuery&,
                                      const BufferAllocator& allocator) {
        return DependencyContinuation::make<GaussianTile>(allocator);
      };
    }
    operation.prepare_static =
        [tiled](const auto& inputs,
                const auto& values) -> Result<OperationPreparation> {
      using Answer = Result<OperationPreparation>;
      const auto& descriptor = inputs[0].descriptor;
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
      specialization.metadata = inputs[0];
      specialization.regional_atomic = tiled;
      result.outputs.push_back(std::move(specialization));
      result.additional_workspace_bytes = (nx + ny) * 8;
      return Answer(std::move(result));
    };
    if (!tiled)
      operation.callback = execute;
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
