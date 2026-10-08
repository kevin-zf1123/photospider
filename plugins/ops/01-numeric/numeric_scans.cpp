#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_aggregate.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
struct ScanMetadata {
  unsigned mask = 0, axis = 0;
  ValueDescriptor output;
};
Result<ScanMetadata> metadata(
    bool integral, const ResultTensorSpec& input,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<ScanMetadata>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto shape = input.sample_shape();
  if (shape.size() < (integral ? 2U : 1U) || shape.size() > 8)
    return mismatch("scan requires rank 1..8 (integral rank >=2)");
  ScanMetadata result;
  result.output = {input.descriptor.element_type, shape};
  std::vector<std::uint64_t> axes;
  if (integral) {
    auto parsed = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("axes")), false);
    if (!parsed.ok())
      return Answer(parsed.status());
    axes = parsed.take_value();
    if (axes.size() != 2)
      return Answer(numeric_ops::array_parameter_error("require two axes"));
  } else {
    const auto axis = std::get<std::int64_t>(parameters.at("axis"));
    if (axis < 0)
      return Answer(numeric_ops::array_parameter_error("negative scan axis"));
    axes.push_back(static_cast<std::uint64_t>(axis));
  }
  for (auto axis : axes) {
    if (axis >= shape.size() || (result.mask & (1U << axis)))
      return Answer(numeric_ops::array_parameter_error("invalid scan axes"));
    result.mask |= 1U << axis;
    result.axis = static_cast<unsigned>(axis);
  }
  std::uint64_t source_count = 1, output_count = 1;
  for (std::size_t j = 0; j < shape.size(); ++j) {
    if (!shape[j] || shape[j] > (UINT64_C(1) << 40) / source_count)
      return mismatch("scan input exceeds 2^40 elements");
    source_count *= shape[j];
    result.output.shape[j] += (result.mask >> j) & 1;
    if (result.output.shape[j] > (UINT64_C(1) << 40) / output_count)
      return mismatch("scan output exceeds 2^40 elements");
    output_count *= result.output.shape[j];
  }
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  if (dtype == "uint8")
    result.output.element_type = ElementType::UInt8;
  else if (dtype == "int64")
    result.output.element_type = ElementType::Int64;
  else if (dtype == "float32")
    result.output.element_type = ElementType::Float32;
  else if (dtype == "float64")
    result.output.element_type = ElementType::Float64;
  else
    return Answer(numeric_ops::array_parameter_error("unsupported scan dtype"));
  const auto floating = [](ElementType type) {
    return type == ElementType::Float32 || type == ElementType::Float64;
  };
  if (floating(input.descriptor.element_type) !=
      floating(result.output.element_type))
    return mismatch("scan source/destination numeric domains differ");
  return Answer(std::move(result));
}
// Store only exact carry, never a rounded output or a complete ratio workspace.
// Plane traversal is logical row-major: previous rows precede the current row,
// preserving first-NaN priority while exact finite sums are merged.
struct SumCarry final {
  numeric_ops::RatioWorkspace::Integer magnitude;
  std::uint64_t first_nan = 0;
  bool negative = false, has_nan = false, positive_inf = false,
       negative_inf = false, all_negative_zero = true;
  void load(numeric_ops::ExactAggregate* target) const {
    target->reset();
    target->ratio.numerator = magnitude;
    target->ratio.negative = negative;
    target->first_nan = first_nan;
    target->has_nan = has_nan;
    target->positive_inf = positive_inf;
    target->negative_inf = negative_inf;
    target->all_negative_zero = all_negative_zero;
  }
  void save(const numeric_ops::ExactAggregate& source) {
    magnitude = source.ratio.numerator;
    negative = source.ratio.negative;
    first_nan = source.first_nan;
    has_nan = source.has_nan;
    positive_inf = source.positive_inf;
    negative_inf = source.negative_inf;
    all_negative_zero = source.all_negative_zero;
  }
};
struct ScanState final {
  numeric_ops::ExactAggregate accumulator, conversion;
  ScanState(SequenceProfile profile, ElementType source)
      : accumulator(profile, numeric_ops::AggregateKind::Sum, source),
        conversion(profile, numeric_ops::AggregateKind::Sum, source) {}
};
struct ScanKernel final {
  bool integral;
  SequenceProfile profile;
  ScanKernel(bool rectangle, SequenceProfile selected)
      : integral(rectangle), profile(selected) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed, "scan requires one packed writer"};
    NumericDiagnostics report;
    report.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto identity = std::snprintf(
        report.implementation.data(), report.implementation.size(),
        "photospider.scan-exact/1;accumulator-u64x68;%s",
        numeric_ops::selection_implementation(profile));
    if (identity < 0 ||
        static_cast<std::size_t>(identity) >= report.implementation.size())
      return {ErrorCode::OperationFailed, "scan diagnostic identity overflow"};
    const auto finish = [&](Status result) {
      // Failure attribution precedes optional diagnostics so a rejected report
      // cannot replace an arithmetic or host failure. Cancelled/work-limited
      // services may reject this final report; their original status survives.
      if (!result.ok())
        numeric_ops::math_record_failure(phase, result);
      if (phase.report_numeric) {
        auto reported = phase.report_numeric(report);
        if (result.ok())
          return reported;
      }
      return result;
    };
    const auto& work = phase.consume_work;
    auto status = work(1);
    if (!status.ok())
      return finish(std::move(status));
    const auto& input = phase.tensors->at({0, 0});
    const auto shape = input.spec().sample_shape();
    auto parsed = metadata(integral, input.spec(), phase.query.parameters);
    if (!parsed.ok())
      return finish(parsed.status());
    const auto& description = parsed.value();
    auto allocated = phase.allocator.allocate(sizeof(ScanState));
    if (!allocated.ok())
      return finish(allocated.status());
    auto buffer = allocated.take_value();
    std::unique_ptr<ScanState, void (*)(ScanState*)> state(
        new (buffer.data())
            ScanState(profile, input.spec().descriptor.element_type),
        [](ScanState* item) { item->~ScanState(); });
    numeric_ops::MathTensorReader reader(input, phase.query.cancellation);
    numeric_ops::MathTensorWriter writer(writers[0]);
    std::vector<std::uint64_t> coordinate(shape.size(), 0),
        source(shape.size(), 0);
    const auto read = [&]() -> Status {
      auto charged = work(shape.size() + 1);
      if (!charged.ok())
        return charged;
      const auto bits = reader.bits(source);
      // Boundary outputs and carry snapshots do not read new inputs.
      ++report.evaluated_values;
      return state->accumulator.add(bits, work);
    };
    const auto store = [&](bool empty) -> Status {
      auto charged = work(shape.size() + 1);
      if (!charged.ok())
        return charged;
      auto calculated = empty ? Result<std::uint64_t>(UINT64_C(0))
                              : state->conversion.finish_as(
                                    description.output.element_type, 1, work);
      if (!calculated.ok()) {
        auto failure = calculated.status();
        if (failure.reason == FailureReason::ArithmeticOverflow) {
          failure.detail = {FailureOrigin::Domain, FailureScope::Run};
          failure.message += " output=[";
          for (auto index : coordinate)
            failure.message += std::to_string(index) + ",";
          failure.message += "]";
        }
        return failure;
      }
      std::array<std::uint64_t, 4> replicas{};
      numeric_ops::select_words(replicas.data(), calculated.value(),
                                calculated.value(), 1, profile);
      std::memcpy(writer.address(coordinate), replicas.data(),
                  Value::element_size(description.output.element_type));
      return Status::success();
    };
    std::uint64_t planes = 1;
    for (std::size_t j = 0; j < shape.size(); ++j)
      if (!(description.mask & (1U << j)))
        planes *= shape[j];
    unsigned outer = description.axis, inner = description.axis;
    if (integral) {
      outer = static_cast<unsigned>(__builtin_ctz(description.mask));
      inner = static_cast<unsigned>(
          __builtin_ctz(description.mask & ~(1U << outer)));
    }
    ResourceVector<SumCarry> columns{
        ResourceAllocator<SumCarry>(phase.resources)};
    if (integral)
      columns.resize(shape[inner]);
    for (std::uint64_t plane = 0; plane < planes; ++plane) {
      if (!integral) {
        state->accumulator.reset();
        coordinate[outer] = 0;
        status = store(true);
        if (!status.ok())
          return finish(std::move(status));
        for (std::uint64_t i = 0; i < shape[outer]; ++i) {
          source = coordinate;
          source[outer] = i;
          status = read();
          if (!status.ok())
            return finish(std::move(status));
          coordinate[outer] = i + 1;
          status = work(sizeof(ScanState) / 16 + 1);
          if (!status.ok())
            return finish(std::move(status));
          state->conversion = state->accumulator;
          status = store(false);
          if (!status.ok())
            return finish(std::move(status));
        }
      } else {
        for (auto& column : columns) {
          status = work(sizeof(SumCarry) / 8 + 1);
          if (!status.ok())
            return finish(std::move(status));
          column = SumCarry{};
        }
        coordinate[outer] = 0;
        for (std::uint64_t x = 0; x <= shape[inner]; ++x) {
          coordinate[inner] = x;
          status = store(true);
          if (!status.ok())
            return finish(std::move(status));
        }
        for (std::uint64_t y = 0; y < shape[outer]; ++y) {
          state->accumulator.reset();
          coordinate[outer] = y + 1;
          coordinate[inner] = 0;
          status = store(true);
          if (!status.ok())
            return finish(std::move(status));
          for (std::uint64_t x = 0; x < shape[inner]; ++x) {
            source = coordinate;
            source[outer] = y;
            source[inner] = x;
            status = read();
            if (!status.ok())
              return finish(std::move(status));
            status = work(512 + 2 * sizeof(SumCarry) / 8);
            if (!status.ok())
              return finish(std::move(status));
            auto& row = state->accumulator;
            auto& combined = state->conversion;
            columns[x].load(&combined);
            combined.ratio.term = row.ratio.numerator;
            combined.ratio.add_term(row.ratio.negative);
            if (!combined.has_nan && row.has_nan) {
              combined.first_nan = row.first_nan;
              combined.has_nan = true;
            }
            combined.positive_inf |= row.positive_inf;
            combined.negative_inf |= row.negative_inf;
            combined.all_negative_zero &= row.all_negative_zero;
            columns[x].save(
                combined);  // finish may destroy arithmetic scratch.
            coordinate[inner] = x + 1;
            status = store(false);
            if (!status.ok())
              return finish(std::move(status));
          }
        }
      }
      for (std::size_t j = 0; j < shape.size(); ++j)
        if (description.mask & (1U << j))
          coordinate[j] = 0;
      for (std::size_t j = shape.size(); j; --j)
        if (!(description.mask & (1U << (j - 1)))) {
          if (++coordinate[j - 1] < shape[j - 1])
            break;
          coordinate[j - 1] = 0;
        }
    }
    return finish(work(1));
  }
};
using ScanProgram = numeric_ops::WholeTensorProgram<ScanKernel>;
OperationDefinition scan_operation(const std::string& key, bool integral,
                                   SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].element_type_mask = 15;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {integral ? "axes" : "axis", integral ? OperationParameterType::String
                                            : OperationParameterType::Int64},
      {"dtype", OperationParameterType::String}};
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(ScanProgram));
  traits.workspace_bytes = sizeof(ScanState);
  operation.specialize_metadata = [integral, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved =
        metadata(integral, inputs[0].result_schema->tensors[0], parameters);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    const auto& output = resolved.value().output;
    result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(output.element_type, output.shape));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_result = [integral, profile](const auto&,
                                               const auto& allocator) {
    return ResultContinuation::make<ScanProgram>(allocator, integral, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_scans(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool integral : {false, true}) {
      auto status = registry->register_operation(scan_operation(
          std::string("numeric.") +
              (integral ? "integral_image" : "prefix_sum") + profile.first,
          integral, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
