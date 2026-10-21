#include <algorithm>
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

#include "01-numeric/exact_sampling.hpp"
#include "01-numeric/expression_evaluator.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"
#include "photospider/ops/numeric/expression.hpp"

namespace ps::plugin_internal::numeric_ops {
namespace {
Status schema(const char* message) {
  return Status{ErrorCode::InvalidArgument,
                message,
                FailureReason::InvalidDomain,
                {FailureOrigin::Schema, FailureScope::Unspecified}};
}
std::string canonical_names(const ExpressionProgram& program) {
  std::string result;
  for (const auto& name : program.names) {
    if (!result.empty())
      result += ' ';
    result += name;
  }
  return result;
}
struct SampleProgram final {
  ExpressionProgram expression;
  std::uint32_t count;
  ElementType dtype;
};
SchemaTemplate output_schema(const SampleProgram& program, bool axis) {
  auto result = tensor_ops::scalar_schema();
  result.tensors[0].descriptor = {axis ? ElementType::Float64 : program.dtype,
                                  {axis ? UINT64_C(3) : program.count}};
  result.tensors[0].atomic_trailing_axes = axis ? 1 : 0;
  return result;
}
Status input_schemas(const std::vector<OperationMetadata>& inputs) {
  if (inputs.size() < 2 || inputs.size() > 258)
    return {ErrorCode::TypeMismatch, "expression input count"};
  for (const auto& input : inputs) {
    auto valid = tensor_ops::check_tensor(input);
    if (!valid.ok())
      return valid;
    const auto& member = input.result_schema->tensors[0];
    if (member.sample_shape() != std::vector<std::uint64_t>{1} ||
        (member.descriptor.element_type != ElementType::Float32 &&
         member.descriptor.element_type != ElementType::Float64))
      return {ErrorCode::TypeMismatch, "expression requires floating scalars"};
  }
  return Status::success();
}
bool active_input(const SampleProgram& program, bool values, unsigned port) {
  return port == 0 || (port == 1 ? program.count > 1 : values);
}
struct EmptyEvaluator {
  explicit EmptyEvaluator(SequenceProfile) {}
};
template <bool Values>
struct SampleState final {
  std::conditional_t<Values, ExpressionEvaluator, EmptyEvaluator> evaluator;
  ExactSampling sampling;
  std::array<std::uint64_t, Values ? 256 : 0> coefficients{};
  std::array<std::uint64_t, 2> endpoints{};
  explicit SampleState(SequenceProfile profile)
      : evaluator(profile), sampling(profile, true) {}
  static std::uint64_t widened(std::uint64_t bits, bool narrow) {
    if (!narrow)
      return bits;
    const auto value = BinaryParts::decode(bits, true);
    const auto sign = (bits >> 31) << 63;
    if (!value.magnitude)
      return sign;
    const auto top = 63 - __builtin_clzll(value.significand);
    return sign |
           (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
           ((value.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
  }
};
struct ScalarInput final {
  unsigned port;
  bool narrow;
};
struct PreparedSample final {
  const SampleProgram* program;
  SequenceProfile profile;
  std::vector<ScalarInput> inputs;
};
template <bool Values>
void expression_invoke(const ps_kernel_invocation_v8* invocation,
                       ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::BackendUnavailable,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& prepared =
        *static_cast<const PreparedSample*>(invocation->prepared_context);
    const auto& program = *prepared.program;
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    const auto require = [](Status checked) {
      if (!checked.ok())
        throw checked;
    };
    require(consume(1));
    static_assert(alignof(SampleState<Values>) <= alignof(std::max_align_t));
    auto* state = new (invocation->scratch[0].allocation)
        SampleState<Values>(prepared.profile);
    const auto destroy = [](SampleState<Values>* value) {
      value->~SampleState<Values>();
    };
    std::unique_ptr<SampleState<Values>, decltype(destroy)> guard(state,
                                                                  destroy);
    for (unsigned slot = 0; slot < prepared.inputs.size(); ++slot) {
      require(consume(1));
      const auto& input = prepared.inputs[slot];
      const auto& view = invocation->inputs[slot];
      std::uint64_t bits = 0;
      std::memcpy(
          &bits,
          static_cast<const std::uint8_t*>(view.allocation) + view.byte_offset,
          input.narrow ? 4 : 8);
      bits = SampleState<Values>::widened(bits, input.narrow);
      if (input.port < 2)
        state->endpoints[input.port] = bits;
      else if constexpr (Values)
        state->coefficients[input.port - 2] = bits;
    }
    const auto& output = invocation->outputs[0];
    auto* destination =
        static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
    if constexpr (!Values) {
      std::uint64_t step = 0;
      if (program.count > 1) {
        auto calculated = state->sampling.weighted(
            state->endpoints[1], state->endpoints[0], 1, 1, program.count - 1,
            false, true, consume);
        require(calculated.status());
        step = calculated.value();
      }
      const std::array<std::uint64_t, 3> axis{
          state->endpoints[0],
          program.count == 1 ? state->endpoints[0] : state->endpoints[1], step};
      std::memcpy(destination, axis.data(), sizeof(axis));
    } else {
      const auto width = TensorSpan::element_size(program.dtype);
      for (std::uint64_t local = 0; local < output.layout->shape[0];) {
        require(consume(1));
        const auto count = static_cast<std::size_t>(
            std::min(UINT64_C(4), output.layout->shape[0] - local));
        std::array<std::uint64_t, 4> coordinates{}, results{};
        std::array<bool, 4> accepted{};
        for (std::size_t lane = 0; lane < count; ++lane) {
          const auto index = static_cast<std::uint32_t>(
              output.layout->origin[0] + local + lane);
          auto coordinate = state->sampling.coordinate(
              index, program.count, state->endpoints, consume);
          require(coordinate.status());
          coordinates[lane] = coordinate.value();
        }
        if (prepared.profile != SequenceProfile::Strict)
          require(state->evaluator.accelerated.evaluate(
              program.expression, coordinates.data(), state->coefficients,
              count, program.dtype == ElementType::Float32, results.data(),
              accepted.data(), consume, true));
        for (std::size_t lane = 0; lane < count; ++lane) {
          if (!accepted[lane]) {
            auto value =
                state->evaluator.evaluate(program.expression, coordinates[lane],
                                          state->coefficients, consume);
            require(value.status());
            if (program.dtype == ElementType::Float32) {
              value = state->sampling.weighted(value.value(), 0, 1, 0, 1, true,
                                               false, consume);
              require(value.status());
            }
            results[lane] = value.value();
          }
          std::memcpy(destination + (local + lane) * width, &results[lane],
                      width);
        }
        local += count;
      }
    }
    *status = {};
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "expression evaluation failed");
  }
}
std::uint64_t scratch_bytes(bool values) {
  return values ? sizeof(SampleState<true>) : sizeof(SampleState<false>);
}
OperationDefinition expression_operation(const std::string& key,
                                         SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.repeated_minimum = 1;
  traits.repeated_maximum = 257;
  traits.repeated_match = false;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema) {
    port.rank = 1;
    port.element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"expression", OperationParameterType::String},
      {"coefficient_names", OperationParameterType::String},
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String}};
  traits.outputs.resize(2);
  for (unsigned port = 0; port < 2; ++port) {
    traits.outputs[port].key = port ? "axis" : "values";
    traits.outputs[port].output_schema = tensor_ops::port();
    traits.outputs[port].result_schema = tensor_ops::scalar_schema();
  }
  operation.prepare_static =
      [profile](const auto& inputs,
                const auto& parameters) -> Result<OperationPreparation> {
    using Answer = Result<OperationPreparation>;
    auto valid = input_schemas(inputs);
    if (!valid.ok())
      return Answer(valid);
    const auto count = std::get<std::int64_t>(parameters.at("count"));
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if ((dtype != "float32" && dtype != "float64") || count < 1 ||
        count > 1048576)
      return Answer(schema("expression dtype/count"));
    auto parsed =
        ExpressionParser(std::get<std::string>(parameters.at("expression")))
            .run();
    if (!parsed.ok())
      return Answer(parsed.status());
    if (canonical_names(parsed.value()) !=
            std::get<std::string>(parameters.at("coefficient_names")) ||
        inputs.size() != parsed.value().names.size() + 2)
      return Answer(
          schema("coefficient_names must exactly match sorted free identifiers "
                 "and inputs"));
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto program = std::make_shared<const SampleProgram>(SampleProgram{
        parsed.take_value(), static_cast<std::uint32_t>(count),
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64});
    OperationPreparation prepared;
    prepared.state = program;
    prepared.outputs.resize(2);
    for (unsigned port = 0; port < 2; ++port)
      prepared.outputs[port].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(
              output_schema(*program, port != 0));
    return Answer(std::move(prepared));
  };
  operation.lower =
      [](const OperationLowering& query) -> Result<OperationProgram> {
    using Answer = Result<OperationProgram>;
    if (!query.static_state)
      return Answer({ErrorCode::TypeMismatch, "missing expression program"});
    const auto& program =
        *static_cast<const SampleProgram*>(query.static_state);
    OperationProgramBuilder builder(query);
    for (unsigned port = 0; port < query.requests.size(); ++port)
      for (const auto& member : query.requests[port].members)
        for (auto tile : member.second) {
          const bool values = port == 0;
          auto task = builder.add_task(values ? "values" : "axis",
                                       "expression-unchecked-v1", {},
                                       {scratch_bytes(values), 0, 0});
          if (!task.ok())
            return Answer(task.status());
          for (unsigned input = 0; input < query.inputs.size(); ++input) {
            auto status = active_input(program, values, input)
                              ? builder.read_input(task.value(), input, 0, 0)
                              : builder.read_input_schema(task.value(), input);
            if (!status.ok())
              return Answer(status);
          }
          auto status =
              builder.write_output(task.value(), port, member.first, tile);
          if (!status.ok())
            return Answer(status);
        }
    return builder.finish();
  };
  operation.prepare_kernel =
      [profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    const auto invalid = [] {
      return Answer({ErrorCode::TypeMismatch, "invalid expression invocation"});
    };
    if (!query.static_state || query.outputs.size() != 2 ||
        query.writes.size() != 1 ||
        (kernel.symbol != "values" && kernel.symbol != "axis") ||
        kernel.contract != "expression-unchecked-v1")
      return invalid();
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "expression requires CPU Host placement"});
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto valid = input_schemas(query.inputs);
    if (!valid.ok())
      return Answer(valid);
    const auto* program = static_cast<const SampleProgram*>(query.static_state);
    if (query.inputs.size() != program->expression.names.size() + 2)
      return invalid();
    const bool values = kernel.symbol == "values";
    for (unsigned port = 0; port < 2; ++port)
      if (!query.outputs[port].result_schema ||
          !query.outputs[port].result_schema->same_schema(
              output_schema(*program, port != 0)))
        return invalid();
    auto grid = TileGrid::from_schema(
        *query.outputs[values ? 0 : 1].result_schema, query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    const auto& write = query.writes[0];
    auto region = grid.value().region(write.tile_index);
    if (!region.ok())
      return Answer(region.status());
    if (write.port != (values ? 0U : 1U) || write.member ||
        write.domain != physical::Domain::Host ||
        write.layout.element_type !=
            (values ? program->dtype : ElementType::Float64) ||
        !fixed_tensor::matches_region(write.layout, region.value()) ||
        !query.task_parameters.empty() ||
        query.scratch !=
            std::array<std::uint64_t, 3>{scratch_bytes(values), 0, 0})
      return invalid();
    auto state = std::make_unique<PreparedSample>();
    state->program = program;
    state->profile = profile;
    std::size_t slot = 0;
    for (unsigned port = 0; port < query.inputs.size(); ++port) {
      if (!active_input(*program, values, port))
        continue;
      if (slot == query.reads.size())
        return invalid();
      const auto type =
          query.inputs[port].result_schema->tensors[0].descriptor.element_type;
      const auto& read = query.reads[slot++];
      if (read.port != port || read.member || read.tile_index ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != type ||
          !fixed_tensor::matches_region(read.layout, Region::whole({1})))
        return invalid();
      state->inputs.push_back({port, type == ElementType::Float32});
    }
    if (slot != query.reads.size())
      return invalid();
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context = state.release();
    result.invoke_cpu =
        values ? expression_invoke<true> : expression_invoke<false>;
    result.release = [](const void* context) {
      delete static_cast<const PreparedSample*>(context);
    };
    return Answer(result);
  };
  return operation;
}
}  // namespace
}  // namespace ps::plugin_internal::numeric_ops

namespace ps::plugin_internal {
Status register_numeric_expression(OperationRegistry* registry) {
  using namespace numeric_ops;  // NOLINT(build/namespaces)
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(expression_operation(
        std::string("numeric.sample_expression") + entry.first, entry.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
namespace ps::numeric {
Result<WorkflowNode> sample_expression_node(
    std::uint64_t id, std::string expression, WorkflowInput start,
    WorkflowInput end, std::int64_t count,
    const std::map<std::string, WorkflowInput>& coefficients, ElementType dtype,
    CpuNumericProfile profile) {
  using namespace plugin_internal::numeric_ops;  // NOLINT(build/namespaces)
  if (!id || count < 1 || count > 1048576 ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64) ||
      profile < CpuNumericProfile::Strict ||
      profile > CpuNumericProfile::X86Avx2)
    return Result<WorkflowNode>(
        schema("invalid expression id/count/dtype/profile"));
  auto program = ExpressionParser(expression).run();
  if (!program.ok())
    return Result<WorkflowNode>(program.status());
  if (coefficients.size() != program.value().names.size())
    return Result<WorkflowNode>(
        schema("expression coefficient names mismatch"));
  std::vector<WorkflowInput> inputs{std::move(start), std::move(end)};
  for (const auto& name : program.value().names) {
    auto found = coefficients.find(name);
    if (found == coefficients.end())
      return Result<WorkflowNode>(schema("expression coefficient missing"));
    inputs.push_back(found->second);
  }
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                           : "_accelerated_x86_64";
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string("numeric.sample_expression") + suffix,
      std::move(inputs),
      {{"expression", std::move(expression)},
       {"count", count},
       {"dtype",
        std::string(dtype == ElementType::Float32 ? "float32" : "float64")},
       {"coefficient_names", canonical_names(program.value())}}});
}
}  // namespace ps::numeric
