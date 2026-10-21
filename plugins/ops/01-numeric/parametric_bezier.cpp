#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
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
Result<TensorDescriptor> metadata(
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<TensorDescriptor>;
  const auto mismatch = [] {
    return Status{ErrorCode::TypeMismatch,
                  "parametric Bezier input shapes, types or logical size",
                  FailureReason::None,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  if (inputs.size() != 4)
    return Answer(mismatch());
  for (unsigned port = 0; port < 4; ++port) {
    auto valid = tensor_ops::check_tensor(inputs[port]);
    if (!valid.ok())
      return Answer(valid);
    const auto dtype =
        inputs[port].result_schema->tensors[0].descriptor.element_type;
    if (port == 2
            ? dtype != ElementType::Int64
            : dtype != ElementType::Float32 && dtype != ElementType::Float64)
      return Answer(mismatch());
  }
  const auto degree = std::get<std::int64_t>(parameters.at("degree"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  if ((degree != 2 && degree != 3) ||
      (dtype != "float32" && dtype != "float64"))
    return Answer(
        numeric_ops::array_parameter_error("parametric Bezier degree/dtype"));
  const auto a = inputs[0].result_schema->tensors[0].sample_shape();
  const auto q = inputs[2].result_schema->tensors[0].sample_shape();
  constexpr auto cap = UINT64_C(1) << 40;
  if (a.size() != 2 || a[0] < 2 || a[0] > 65536 || !a[1] || a[1] > cap / a[0] ||
      a[1] > cap / ((a[0] - 1) * (degree - 1)) || q.size() != 1 || !q[0] ||
      q[0] > cap / a[1] ||
      inputs[3].result_schema->tensors[0].sample_shape() != q ||
      inputs[1].result_schema->tensors[0].sample_shape() !=
          std::vector<std::uint64_t>{
              a[0] - 1, static_cast<std::uint64_t>(degree - 1), a[1]})
    return Answer(mismatch());
  return Answer(TensorDescriptor{
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
      {q[0], a[1]}});
}
SchemaTemplate output_schema(const TensorDescriptor& descriptor) {
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = descriptor;
  return schema;
}
std::array<Region, 4> input_regions(unsigned knots, unsigned degree,
                                    const Region& output) {
  const auto rows = output.dimensions()[0];
  const auto columns = output.dimensions()[1];
  return {Region({{0, knots}, columns}),
          Region({{0, knots - 1}, {0, degree - 1}, columns}), Region({rows}),
          Region({rows})};
}
struct ParametricMath final {
  numeric_ops::ExactPolynomial polynomial;
  numeric_ops::ExactSampling sampling;
  explicit ParametricMath(SequenceProfile profile)
      : polynomial(profile), sampling(profile, true) {}
  Result<std::uint64_t> evaluate(
      const std::array<std::uint64_t, 4>& controls, unsigned degree,
      std::uint64_t parameter, bool narrow,
      const std::function<Status(std::uint64_t)>& consume) {
    polynomial.begin(consume);
    struct End {
      numeric_ops::ExactPolynomial& math;
      ~End() { math.end(); }
    } end{polynomial};
    const auto coefficients = polynomial.bernstein(controls, degree);
    const auto t = polynomial.binary(parameter, 1074);
    const auto numerator = polynomial.evaluate(coefficients, t, 1074);
    const auto denominator = polynomial.shift(
        polynomial.integer(1),
        coefficients.degree > 0 ? coefficients.degree * 1074 : 0);
    auto result = polynomial.round(numerator, denominator, narrow);
    if (!result.ok())
      return result;
    if (!polynomial.sign(numerator)) {
      bool negative = true;
      for (unsigned i = 0; i <= degree; ++i)
        negative = negative && controls[i] == (UINT64_C(1) << 63);
      return Result<std::uint64_t>(negative ? UINT64_C(1) << (narrow ? 31 : 63)
                                            : 0);
    }
    return result;
  }
};
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto descriptor = metadata(query.inputs, query.parameters);
  if (!descriptor.ok())
    return Answer(descriptor.status());
  auto output = TileGrid::from_schema(output_schema(descriptor.value()),
                                      query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, 4> inputs;
  for (unsigned port = 0; port < 4; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    inputs[port] = grid.take_value();
  }
  const auto degree = static_cast<unsigned>(
      std::get<std::int64_t>(query.parameters.at("degree")));
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task("evaluate", "parametric-bezier-unchecked-v1",
                                   {}, {sizeof(ParametricMath), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      const auto regions =
          input_regions(inputs[0].shape()[0], degree, region.value());
      for (unsigned port = 0; port < 4; ++port) {
        auto reads = sdk::tiles_for_region(budget, inputs[port], regions[port]);
        if (!reads.ok())
          return Answer(reads.status());
        for (auto index : reads.value()) {
          auto valid = budget.consume({1});
          if (valid.ok())
            valid = builder.read_input(task.value(), port, 0, index);
          if (!valid.ok())
            return Answer(valid);
        }
      }
      auto valid = builder.write_output(task.value(), 0, member.first, tile);
      if (!valid.ok())
        return Answer(valid);
    }
  return builder.finish();
}
struct PreparedParametric final {
  SequenceProfile profile;
  unsigned degree, knots;
  bool narrow;
  std::array<bool, 4> input_narrow{};
  fixed_tensor::PointInputs<4> inputs;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& state =
        *static_cast<const PreparedParametric*>(invocation->prepared_context);
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
    static_assert(alignof(ParametricMath) <= alignof(std::max_align_t));
    auto* math =
        new (invocation->scratch[0].allocation) ParametricMath(state.profile);
    const auto destroy = [](ParametricMath* value) {
      value->~ParametricMath();
    };
    std::unique_ptr<ParametricMath, decltype(destroy)> guard(math, destroy);
    fixed_tensor::PointReader<4> reader{state.inputs, *invocation};
    const auto read = [&](unsigned port,
                          const std::array<std::uint64_t, 8>& at) {
      return numeric_ops::ExactBezier::widen(
          reader.bits(port, at, state.input_narrow[port] ? 4 : 8),
          state.input_narrow[port]);
    };
    const auto& output = invocation->outputs[0];
    for (std::uint64_t row = 0; row < output.layout->shape[0]; ++row) {
      if (!accept(consume(1)))
        return;
      const auto index = output.layout->origin[0] + row;
      const auto segment = reader.bits(2, {index}, 8);
      // Signed negatives also compare above the bound as raw unsigned bits.
      // This guard protects the data-dependent control addresses.
      if (segment >= state.knots - 1) {
        *status = kernel_failure(ErrorCode::InvalidArgument,
                                 "parametric segment out of range",
                                 FailureReason::InvalidDomain);
        return;
      }
      const auto parameter = read(3, {index});
      const auto parts = BinaryParts::decode(parameter, false);
      const int endpoint = !parts.magnitude                            ? 0
                           : parameter == UINT64_C(0x3ff0000000000000) ? 1
                                                                       : -1;
      for (std::uint64_t column = 0; column < output.layout->shape[1];
           ++column) {
        if (!accept(consume(1)))
          return;
        const auto component = output.layout->origin[1] + column;
        std::array<std::uint64_t, 4> controls{};
        controls[0] = read(0, {segment + (endpoint == 1 ? 1 : 0), component});
        std::uint64_t bits;
        if (endpoint >= 0) {
          bits = state.narrow
                     ? numeric_ops::numeric_bits(
                           numeric_ops::numeric_double(controls[0]), true)
                     : controls[0];
        } else {
          controls[state.degree] = read(0, {segment + 1, component});
          for (unsigned h = 0; h + 1 < state.degree; ++h) {
            const auto offset = read(1, {segment, h, component});
            auto absolute = math->sampling.weighted(
                h ? controls[state.degree] : controls[0], offset, 1, 1, 1,
                false, false, consume);
            if (!accept(absolute.status()))
              return;
            controls[h + 1] = absolute.value();
          }
          auto value = math->evaluate(controls, state.degree, parameter,
                                      state.narrow, consume);
          if (!accept(value.status()))
            return;
          bits = value.value();
        }
        const auto offset = output.byte_offset +
                            row * output.layout->strides[0] +
                            column * output.layout->strides[1];
        std::memcpy(static_cast<std::uint8_t*>(output.allocation) + offset,
                    &bits, state.narrow ? 4 : 8);
      }
    }
    *status = {};
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "parametric Bezier invocation failed");
  }
}
Result<ps_prepared_kernel_v8> prepare(SequenceProfile profile,
                                      const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch,
                   "invalid fixed parametric Bezier invocation"});
  };
  if (query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty() ||
      query.scratch !=
          std::array<std::uint64_t, 3>{sizeof(ParametricMath), 0, 0})
    return invalid();
  auto descriptor = metadata(query.inputs, query.parameters);
  if (!descriptor.ok())
    return Answer(descriptor.status());
  if (!query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(
          output_schema(descriptor.value())))
    return invalid();
  auto grid =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto output = grid.value().region(query.writes[0].tile_index);
  if (!output.ok())
    return Answer(output.status());
  auto state = std::make_unique<PreparedParametric>();
  state->profile = profile;
  state->degree = std::get<std::int64_t>(query.parameters.at("degree"));
  state->knots = query.inputs[0].result_schema->tensors[0].sample_shape()[0];
  state->narrow = descriptor.value().element_type == ElementType::Float32;
  for (unsigned port = 0; port < 4; ++port)
    state->input_narrow[port] =
        query.inputs[port].result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
  const auto regions =
      input_regions(state->knots, state->degree, output.value());
  auto valid =
      fixed_tensor::prepare_point_inputs(query, &state->inputs, &regions);
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = state.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedParametric*>(context);
  };
  return Answer(kernel);
}
OperationDefinition parametric_operation(const std::string& key,
                                         SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 4;
  traits.input_schema.resize(4);
  for (unsigned port : {0, 1, 3})
    traits.input_schema[port].element_type_mask = 12;
  traits.input_schema[2].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"degree", OperationParameterType::Int64, true, true, 2, 3},
      {"dtype", OperationParameterType::String}};
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto descriptor = metadata(inputs, parameters);
    if (!descriptor.ok())
      return Answer(descriptor.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        output_schema(descriptor.value()));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.lower = lower;
  operation.prepare_kernel =
      [profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "evaluate" ||
        kernel.contract != "parametric-bezier-unchecked-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid parametric Bezier kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable,
           "parametric Bezier requires CPU Host"});
    return prepare(profile, query);
  };
  return operation;
}
}  // namespace
Status register_parametric_bezier(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(parametric_operation(
        std::string("curve.evaluate_bezier") + entry.first, entry.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
