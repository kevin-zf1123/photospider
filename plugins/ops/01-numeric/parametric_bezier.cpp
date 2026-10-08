#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_bezier.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct ParametricRow {
  std::uint64_t index = 0, t = 0, segment = 0;
  int endpoint = -1;
};
struct ParametricState final {
  SequenceProfile profile;
  unsigned degree;
  const ResultProgramPhase& phase;
  std::array<std::optional<numeric_ops::MathTensorReader>, 4> readers;
  std::function<Status(std::uint64_t)> consume;
  numeric_ops::ExactPolynomial arithmetic;
  numeric_ops::ExactSampling sampling;
  std::array<std::uint64_t, 4> controls{}, replicas{};
  ParametricState(SequenceProfile selected,
                  const ResultProgramPhase& invocation)
      : profile(selected),
        degree(
            std::get<std::int64_t>(invocation.query.parameters.at("degree"))),
        phase(invocation),
        consume([this](auto amount) { return work(amount); }),
        arithmetic(selected),
        sampling(selected) {
    for (unsigned port = 0; port < readers.size(); ++port)
      readers[port].emplace(phase.tensors->at({port, 0}),
                            phase.query.cancellation);
  }
  Status work(std::uint64_t amount) const { return phase.consume_work(amount); }
  Status failure(unsigned port, const std::vector<std::uint64_t>& at,
                 const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status result{
        (port == 2 || port == 3) ? ErrorCode::InvalidArgument
                                 : ErrorCode::OperationFailed,
        std::string(message) +
            (port == 4 ? "; output="
                       : "; port=" + std::to_string(port) + " coordinate="),
        reason,
        {FailureOrigin::Domain, FailureScope::Run}};
    for (auto index : at)
      result.message += std::to_string(index) + ",";
    return result;
  }
  Result<std::uint64_t> read(unsigned port,
                             const std::vector<std::uint64_t>& at) {
    auto charged = work(at.size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const auto& input = phase.tensors->at({port, 0});
    const bool narrow =
        input.spec().descriptor.element_type == ElementType::Float32;
    auto bits = readers[port]->bits(at);
    const auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Result<std::uint64_t>(
          failure(port, at, "nonfinite parametric input"));
    return Result<std::uint64_t>(numeric_ops::ExactBezier::widen(bits, narrow));
  }
  Status classify(ParametricRow* row) {
    auto status = work(2);
    if (!status.ok())
      return status;
    std::int64_t segment = 0;
    const auto bits = readers[2]->bits({row->index});
    std::memcpy(&segment, &bits, sizeof(segment));
    if (segment < 0 ||
        static_cast<std::uint64_t>(segment) >=
            phase.query.inputs[0].result_schema->tensors[0].sample_shape()[0] -
                1)
      return failure(2, {row->index}, "parametric segment out of range");
    row->segment = static_cast<std::uint64_t>(segment);
    auto parameter = read(3, {row->index});
    if (!parameter.ok())
      return parameter.status();
    row->t = parameter.value();
    const auto parts = BinaryParts::decode(row->t, false);
    if ((parts.negative && parts.magnitude) ||
        parts.order_key() > UINT64_C(0xbff0000000000000))
      return failure(3, {row->index}, "parametric t outside [0,1]");
    row->endpoint = !parts.magnitude                         ? 0
                    : row->t == UINT64_C(0x3ff0000000000000) ? 1
                                                             : -1;
    return Status::success();
  }
  Result<std::uint64_t> evaluate(const ParametricRow& row, std::uint64_t column,
                                 bool narrow) {
    auto first = read(0, {row.segment + (row.endpoint == 1 ? 1 : 0), column});
    if (!first.ok())
      return first;
    controls[0] = first.value();
    if (row.endpoint >= 0)
      return sampling.weighted(first.value(), 0, 1, 0, 1, narrow, false,
                               consume);
    auto last = read(0, {row.segment + 1, column});
    if (!last.ok())
      return last;
    controls[degree] = last.value();
    for (unsigned h = 0; h + 1 < degree; ++h) {
      auto offset = read(1, {row.segment, h, column});
      if (!offset.ok())
        return offset;
      auto absolute =
          sampling.weighted(h ? controls[degree] : controls[0], offset.value(),
                            1, 1, 1, false, false, consume);
      if (!absolute.ok())
        return absolute;
      if (BinaryParts::decode(absolute.value(), false).infinite)
        return Result<std::uint64_t>(
            failure(1, {row.segment, h, column},
                    "parametric control reconstruction overflow",
                    FailureReason::ArithmeticOverflow));
      controls[h + 1] = absolute.value();
    }
    arithmetic.begin(consume);
    struct End {
      numeric_ops::ExactPolynomial& math;
      ~End() { math.end(); }
    } end{arithmetic};
    auto polynomial = arithmetic.bernstein(controls, degree);
    const auto t = arithmetic.binary(row.t, 1074);
    const auto numerator = arithmetic.evaluate(polynomial, t, 1074);
    const auto denominator =
        arithmetic.shift(arithmetic.integer(1),
                         polynomial.degree > 0 ? polynomial.degree * 1074 : 0);
    auto result = arithmetic.round(numerator, denominator, narrow);
    if (!result.ok())
      return result;
    if (!arithmetic.sign(numerator)) {
      bool negative = true;
      for (unsigned i = 0; i <= degree; ++i)
        negative = negative && controls[i] == (UINT64_C(1) << 63);
      return Result<std::uint64_t>(
          negative ? (UINT64_C(1) << (narrow ? 31 : 63)) : 0);
    }
    return result;
  }
  Status execute(const ResourceVector<ResultTensorWriteWindow>& writers) {
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    const auto shape = tensor.sample_shape();
    for (std::uint64_t i = 0; i < shape[0]; ++i) {
      ParametricRow row;
      row.index = i;
      auto status = classify(&row);
      if (!status.ok())
        return status;
    }
    numeric_ops::MathTensorWriter output(writers[0]);
    const bool narrow = tensor.descriptor.element_type == ElementType::Float32;
    const auto width = narrow ? 4U : 8U;
    for (std::uint64_t i = 0; i < shape[0]; ++i) {
      ParametricRow row;
      row.index = i;
      auto status = classify(&row);
      if (!status.ok())
        return status;
      for (std::uint64_t column = 0; column < shape[1]; ++column) {
        auto value = evaluate(row, column, narrow);
        if (!value.ok())
          return value.status();
        if (BinaryParts::decode(value.value(), narrow).infinite)
          return failure(
              row.endpoint < 0 ? 4 : 0,
              {row.endpoint < 0 ? i : row.segment + row.endpoint, column},
              "parametric output overflow", FailureReason::ArithmeticOverflow);
        numeric_ops::select_words(replicas.data(), value.value(), value.value(),
                                  1, profile);
        std::memcpy(output.address({i, column}), replicas.data(), width);
      }
    }
    return work(1);
  }
};
struct ParametricKernel final {
  SequenceProfile profile;
  explicit ParametricKernel(SequenceProfile selected) : profile(selected) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed,
              "parametric requires one packed writer"};
    auto allocated = phase.allocator.allocate(sizeof(ParametricState));
    if (!allocated.ok())
      return allocated.status();
    auto buffer = allocated.take_value();
    std::unique_ptr<ParametricState, void (*)(ParametricState*)> state(
        new (buffer.data()) ParametricState(profile, phase),
        [](auto* value) { value->~ParametricState(); });
    return state->execute(writers);
  }
};
using ParametricProgram = numeric_ops::WholeTensorProgram<ParametricKernel>;

OperationDefinition parametric_operation(const std::string& key,
                                         SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 4;
  traits.input_schema.resize(4);
  for (auto& port : traits.input_schema)
    port.kind = OperationPortKind::Result;
  for (unsigned p : {0, 1, 3})
    traits.input_schema[p].element_type_mask = 12;
  traits.input_schema[2].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"degree", OperationParameterType::Int64, true, true, 2, 3},
      {"dtype", OperationParameterType::String}};
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(ParametricProgram));
  traits.workspace_bytes = sizeof(ParametricState);
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto degree =
        static_cast<unsigned>(std::get<std::int64_t>(parameters.at("degree")));
    const auto a = inputs[0].result_schema->tensors[0].sample_shape();
    const auto q = inputs[2].result_schema->tensors[0].sample_shape();
    constexpr auto cap = UINT64_C(1) << 40;
    if (a.size() != 2 || a[0] < 2 || a[0] > 65536 || !a[1] ||
        a[1] > cap / a[0] || a[1] > cap / ((a[0] - 1) * (degree - 1)) ||
        q.size() != 1 || !q[0] || q[0] > cap / a[1] ||
        inputs[3].result_schema->tensors[0].sample_shape() != q ||
        inputs[1].result_schema->tensors[0].sample_shape() !=
            std::vector<std::uint64_t>{a[0] - 1, degree - 1, a[1]})
      return Answer(Status{
          ErrorCode::TypeMismatch,
          "parametric shapes require K=2..65536, D/N>=1 and products <=2^40",
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype != "float32" && dtype != "float64")
      return Answer(numeric_ops::array_parameter_error("parametric dtype"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(
            dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
            {q[0], a[1]}));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.start_result = [profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<ParametricProgram>(allocator, profile);
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
