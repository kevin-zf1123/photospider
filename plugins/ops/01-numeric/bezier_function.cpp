#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_bezier.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct Description final {
  unsigned degree, count, knots;
  ElementType dtype;
};
Result<Description> metadata(
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<Description>;
  const auto mismatch = [] {
    return Status{ErrorCode::TypeMismatch,
                  "Bezier anchors/handles/scalar shapes and types",
                  FailureReason::None,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  if (inputs.size() != 4)
    return Answer(mismatch());
  for (const auto& input : inputs) {
    auto valid = tensor_ops::check_tensor(input);
    if (!valid.ok())
      return Answer(valid);
    const auto dtype = input.result_schema->tensors[0].descriptor.element_type;
    if (dtype != ElementType::Float32 && dtype != ElementType::Float64)
      return Answer(mismatch());
  }
  const auto degree = std::get<std::int64_t>(parameters.at("degree"));
  const auto count = std::get<std::int64_t>(parameters.at("count"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& domain = std::get<std::string>(parameters.at("out_of_domain"));
  if ((degree != 2 && degree != 3) || count < 1 || count > 1048576 ||
      (dtype != "float32" && dtype != "float64") ||
      (domain != "reject" && domain != "clamp"))
    return Answer(
        numeric_ops::array_parameter_error("Bezier degree/count/dtype/domain"));
  const auto anchors = inputs[0].result_schema->tensors[0].sample_shape();
  if (anchors.size() != 2 || anchors[0] < 2 || anchors[0] > 65536 ||
      anchors[1] != 2 ||
      inputs[1].result_schema->tensors[0].sample_shape() !=
          std::vector<std::uint64_t>{
              anchors[0] - 1, static_cast<std::uint64_t>(degree - 1), 2} ||
      inputs[2].result_schema->tensors[0].sample_shape() !=
          std::vector<std::uint64_t>{1} ||
      inputs[3].result_schema->tensors[0].sample_shape() !=
          std::vector<std::uint64_t>{1})
    return Answer(mismatch());
  return Answer(Description{
      static_cast<unsigned>(degree), static_cast<unsigned>(count),
      static_cast<unsigned>(anchors[0]),
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64});
}
SchemaTemplate output_schema(const Description& description, bool values) {
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = {
      values ? description.dtype : ElementType::Float64,
      {values ? description.count : UINT64_C(3)}};
  schema.tensors[0].atomic_trailing_axes = values ? 0 : 1;
  return schema;
}
std::array<Region, 4> input_regions(const Description& description,
                                    bool values) {
  return {Region({{0, values ? description.knots : 0}, {0, values ? 2U : 0U}}),
          Region({{0, values ? description.knots - 1 : 0},
                  {0, values ? description.degree - 1 : 0},
                  {0, values ? 2U : 0U}}),
          Region({{0, 1}}), Region({{0, description.count > 1 ? 1U : 0U}})};
}
struct EmptyBezier {
  explicit EmptyBezier(SequenceProfile) {}
};
template <bool Values>
struct Arithmetic final {
  numeric_ops::ExactSampling sampling;
  std::conditional_t<Values, numeric_ops::ExactBezier, EmptyBezier> bezier;
  explicit Arithmetic(SequenceProfile profile)
      : sampling(profile, true), bezier(profile) {}
};
std::uint64_t scratch_bytes(const Description& description, bool values) {
  return values                   ? sizeof(Arithmetic<true>)
         : description.count == 1 ? 0
                                  : sizeof(Arithmetic<false>);
}
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto parsed = metadata(query.inputs, query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  std::array<TileGrid, 4> inputs;
  for (unsigned port = 0; port < 4; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    inputs[port] = grid.take_value();
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (unsigned port = 0; port < query.requests.size(); ++port) {
    const bool values = port == 0;
    const auto regions = input_regions(parsed.value(), values);
    for (const auto& member : query.requests[port].members)
      for (auto tile : member.second) {
        auto task = builder.add_task(
            values ? "values" : "axis", "bezier-function-unchecked-v1", {},
            {scratch_bytes(parsed.value(), values), 0, 0});
        if (!task.ok())
          return Answer(task.status());
        for (unsigned input = 0; input < 4; ++input) {
          if (regions[input].empty()) {
            auto valid = builder.read_input_schema(task.value(), input);
            if (!valid.ok())
              return Answer(valid);
            continue;
          }
          auto reads =
              sdk::tiles_for_region(budget, inputs[input], regions[input]);
          if (!reads.ok())
            return Answer(reads.status());
          for (auto index : reads.value()) {
            auto valid = budget.consume({1});
            if (valid.ok())
              valid = builder.read_input(task.value(), input, 0, index);
            if (!valid.ok())
              return Answer(valid);
          }
        }
        auto valid =
            builder.write_output(task.value(), port, member.first, tile);
        if (!valid.ok())
          return Answer(valid);
      }
  }
  return builder.finish();
}
struct PreparedBezier final {
  Description description;
  SequenceProfile profile;
  std::array<bool, 4> narrow{};
  fixed_tensor::PointInputs<4> inputs;
};
template <bool Values>
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& prepared =
        *static_cast<const PreparedBezier*>(invocation->prepared_context);
    const auto& description = prepared.description;
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    const auto accept = [&](const Status& checked) {
      if (!checked.ok())
        *status = kernel_failure(checked.code, checked.message.c_str(),
                                 checked.reason);
      return checked.ok();
    };
    if (!accept(consume(1)))
      return;
    static_assert(alignof(Arithmetic<Values>) <= alignof(std::max_align_t));
    Arithmetic<Values>* math = nullptr;
    if (Values || description.count > 1)
      math = new (invocation->scratch[0].allocation)
          Arithmetic<Values>(prepared.profile);
    const auto destroy = [](Arithmetic<Values>* value) {
      value->~Arithmetic<Values>();
    };
    std::unique_ptr<Arithmetic<Values>, decltype(destroy)> guard(math, destroy);
    fixed_tensor::PointReader<4> reader{prepared.inputs, *invocation};
    const auto read = [&](unsigned port,
                          const std::array<std::uint64_t, 8>& at) {
      return numeric_ops::ExactBezier::widen(
          reader.bits(port, at, prepared.narrow[port] ? 4 : 8),
          prepared.narrow[port]);
    };
    std::array<std::uint64_t, 2> endpoints{read(2, {0}), 0};
    endpoints[1] = description.count == 1 ? endpoints[0] : read(3, {0});
    const auto& output = invocation->outputs[0];
    if constexpr (!Values) {
      std::uint64_t step = 0;
      if (description.count > 1) {
        auto calculated = math->sampling.weighted(endpoints[1], endpoints[0], 1,
                                                  1, description.count - 1,
                                                  false, true, consume);
        if (!accept(calculated.status()))
          return;
        step = calculated.value();
      }
      const std::array<std::uint64_t, 3> axis{endpoints[0], endpoints[1], step};
      for (unsigned i = 0; i < 3; ++i)
        std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                        output.byte_offset + i * output.layout->strides[0],
                    &axis[i], 8);
    } else {
      const bool narrow = description.dtype == ElementType::Float32;
      for (std::uint64_t local = 0; local < output.layout->shape[0]; ++local) {
        if (!accept(consume(1)))
          return;
        const auto index = output.layout->origin[0] + local;
        auto coordinate = math->sampling.coordinate(index, description.count,
                                                    endpoints, consume);
        if (!accept(coordinate.status()))
          return;
        const auto query = coordinate.value();
        const auto wanted = BinaryParts::decode(query, false).order_key();
        unsigned lo = 0, hi = description.knots;
        while (lo < hi) {
          if (!accept(consume(1)))
            return;
          const auto mid = lo + (hi - lo) / 2;
          if (BinaryParts::decode(read(0, {mid, 0}), false).order_key() <
              wanted)
            lo = mid + 1;
          else
            hi = mid;
        }
        const bool selected =
            !lo || lo == description.knots ||
            BinaryParts::decode(read(0, {lo, 0}), false).order_key() == wanted;
        std::uint64_t bits;
        if (selected) {
          // Reject's closed domain is a caller precondition. Both endpoint
          // selections remain in bounds even for an out-of-contract query.
          const auto anchor = lo == description.knots ? lo - 1 : lo;
          const auto value = read(0, {anchor, 1});
          bits = narrow ? numeric_ops::numeric_bits(
                              numeric_ops::numeric_double(value), true)
                        : value;
        } else {
          const auto segment = lo - 1;
          std::array<std::uint64_t, 4> x{}, y{};
          for (unsigned component = 0; component < 2; ++component) {
            auto& controls = component ? y : x;
            controls[0] = read(0, {segment, component});
            controls[description.degree] = read(0, {segment + 1, component});
            for (unsigned h = 0; h + 1 < description.degree; ++h) {
              auto value = math->sampling.weighted(
                  h ? controls[description.degree] : controls[0],
                  read(1, {segment, h, component}), 1, 1, 1, false, false,
                  consume);
              if (!accept(value.status()))
                return;
              controls[h + 1] = value.value();
            }
          }
          auto value = math->bezier.inverse(x, y, description.degree, query,
                                            narrow, consume);
          if (!accept(value.status()))
            return;
          bits = value.value();
        }
        std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                        output.byte_offset + local * output.layout->strides[0],
                    &bits, narrow ? 4 : 8);
      }
    }
    *status = {};
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "Bezier function invocation failed");
  }
}
Result<ps_prepared_kernel_v8> prepare(SequenceProfile profile, bool values,
                                      const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(
        {ErrorCode::TypeMismatch, "invalid fixed Bezier function invocation"});
  };
  if (query.outputs.size() != 2 || query.writes.size() != 1 ||
      !query.task_parameters.empty())
    return invalid();
  auto parsed = metadata(query.inputs, query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  const auto& description = parsed.value();
  if (query.scratch !=
      std::array<std::uint64_t, 3>{scratch_bytes(description, values), 0, 0})
    return invalid();
  for (unsigned port = 0; port < 2; ++port)
    if (!query.outputs[port].result_schema ||
        !query.outputs[port].result_schema->same_schema(
            output_schema(description, port == 0)))
      return invalid();
  const auto& write = query.writes[0];
  const auto schema = output_schema(description, values);
  auto grid = TileGrid::from_schema(schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto region = grid.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (write.port != (values ? 0U : 1U) || write.member ||
      write.domain != physical::Domain::Host ||
      write.layout.element_type != schema.tensors[0].descriptor.element_type ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  auto state = std::make_unique<PreparedBezier>();
  state->description = description;
  state->profile = profile;
  for (unsigned port = 0; port < 4; ++port)
    state->narrow[port] =
        query.inputs[port].result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
  auto valid = fixed_tensor::prepare_input_regions(
      query, &state->inputs, input_regions(description, values));
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = state.release();
  kernel.invoke_cpu = values ? invoke<true> : invoke<false>;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedBezier*>(context);
  };
  return Answer(kernel);
}
OperationDefinition bezier_operation(const std::string& key,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 4;
  traits.input_schema.resize(4);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"degree", OperationParameterType::Int64, true, true, 2, 3},
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String},
      {"out_of_domain", OperationParameterType::String}};
  traits.outputs.resize(2);
  for (unsigned port = 0; port < 2; ++port) {
    traits.outputs[port].key = port ? "axis" : "values";
    traits.outputs[port].output_schema = tensor_ops::port();
    traits.outputs[port].result_schema = tensor_ops::scalar_schema();
  }
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto parsed = metadata(inputs, parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    std::vector<OperationOutputSpecialization> outputs(2);
    for (unsigned port = 0; port < 2; ++port)
      outputs[port].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(
              output_schema(parsed.value(), port == 0));
    return Answer(std::move(outputs));
  };
  operation.lower = lower;
  operation.prepare_kernel =
      [profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if ((kernel.symbol != "values" && kernel.symbol != "axis") ||
        kernel.contract != "bezier-function-unchecked-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid Bezier function kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "Bezier function requires CPU Host"});
    return prepare(profile, kernel.symbol == "values", query);
  };
  return operation;
}
}  // namespace
Status register_bezier_function(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(bezier_operation(
        std::string("curve.sample_bezier_function") + entry.first,
        entry.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
