#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_curve.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct InversePoint {
  std::uint64_t row = 0, query = 0;
  unsigned first = 0, count = 0, segment = 0;
  int selected = -1;
};
struct InverseState {
  bool pchip, clamp, increasing = true;
  SequenceProfile profile;
  numeric_ops::ExactCurve arithmetic;
  const ResultProgramPhase& phase;
  std::array<std::optional<numeric_ops::MathTensorReader>, 3> readers;
  std::function<Status(std::uint64_t)> consume;
  ResourceVector<std::uint64_t> xs, ys;
  InverseState(bool cubic, bool clipped, SequenceProfile selected,
               const ResultProgramPhase& invocation)
      : pchip(cubic),
        clamp(clipped),
        profile(selected),
        arithmetic(selected),
        phase(invocation),
        consume([this](auto amount) { return work(amount); }),
        xs(ResourceAllocator<std::uint64_t>(phase.resources)),
        ys(ResourceAllocator<std::uint64_t>(phase.resources)) {
    for (unsigned port = 0; port < readers.size(); ++port)
      readers[port].emplace(phase.tensors->at({port, 0}),
                            phase.query.cancellation);
  }
  Status work(std::uint64_t amount) const { return phase.consume_work(amount); }
  Status failure(unsigned port, std::uint64_t index, const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    return {ErrorCode::OperationFailed,
            std::string(message) + "; port=" + std::to_string(port) +
                " index=" + std::to_string(index),
            reason,
            {FailureOrigin::Domain, FailureScope::Run}};
  }
  Result<std::uint64_t> read(unsigned port, std::uint64_t index) {
    auto charged = work(2);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const auto& input = phase.tensors->at({port, 0});
    const bool narrow =
        input.spec().descriptor.element_type == ElementType::Float32;
    auto bits = readers[port]->bits({index});
    const auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Result<std::uint64_t>(
          failure(port, index, "nonfinite inverse input"));
    if (narrow) {
      const auto sign = (bits >> 31) << 63;
      if (!value.magnitude) {
        bits = sign;
      } else {
        const auto top = 63 - __builtin_clzll(value.significand);
        bits =
            sign |
            (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
            ((value.significand << (52 - top)) & UINT64_C(0x000fffffffffffff));
      }
    }
    return Result<std::uint64_t>(bits);
  }
  std::uint64_t key(std::uint64_t bits) const {
    const auto ordered = BinaryParts::decode(bits, false).order_key();
    return increasing ? ordered : UINT64_MAX - ordered;
  }
  Status classify(InversePoint* point) {
    auto query = read(2, point->row);
    if (!query.ok())
      return query.status();
    point->query = query.value();
    const auto wanted = key(point->query);
    unsigned lo = 0, hi = ys.size();
    while (lo < hi) {
      auto work = consume(1);
      if (!work.ok())
        return work;
      const auto middle = lo + (hi - lo) / 2;
      if (key(ys[middle]) < wanted)
        lo = middle + 1;
      else
        hi = middle;
    }
    if (lo < ys.size() && key(ys[lo]) == wanted) {
      point->selected = lo;
    } else if (!lo || lo == ys.size()) {
      if (!clamp)
        return failure(2, point->row, "inverse query outside domain");
      point->selected = lo ? ys.size() - 1 : 0;
    } else {
      point->segment = lo - 1;
    }
    if (point->selected >= 0) {
      point->first = point->selected;
      point->count = 1;
    } else if (!pchip) {
      point->first = point->segment;
      point->count = 2;
    } else {
      point->first = point->segment ? point->segment - 1 : 0;
      point->count =
          std::min<unsigned>(ys.size(), point->segment + 3) - point->first;
    }
    return Status::success();
  }
  Status execute(const ResourceVector<ResultTensorWriteWindow>& writers) {
    xs.resize(phase.tensors->at({0, 0}).spec().sample_shape()[0]);
    ys.resize(xs.size());
    for (unsigned i = 0; i < xs.size(); ++i) {
      auto x = read(0, i), y = read(1, i);
      if (!x.ok() || !y.ok())
        return !x.ok() ? x.status() : y.status();
      xs[i] = x.value();
      ys[i] = y.value();
      if (i && BinaryParts::decode(xs[i - 1], false).order_key() >=
                   BinaryParts::decode(xs[i], false).order_key())
        return failure(0, i, "inverse x requires strict increase");
      if (i == 1)
        increasing = BinaryParts::decode(ys[0], false).order_key() <
                     BinaryParts::decode(ys[1], false).order_key();
      if (i && key(ys[i - 1]) >= key(ys[i]))
        return failure(1, i, "inverse y requires strict monotonicity");
    }
    const auto count = phase.tensors->at({2, 0}).spec().sample_shape()[0];
    // Validate all query controls before inverse arithmetic without retaining
    // per-query classifications or repeating segment searches.
    for (std::uint64_t row = 0; row < count; ++row) {
      auto query = read(2, row);
      if (!query.ok())
        return query.status();
      const auto wanted = key(query.value());
      if (!clamp && (wanted < key(ys.front()) || wanted > key(ys.back())))
        return failure(2, row, "inverse query outside domain");
    }
    const bool narrow =
        phase.query.output.result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    numeric_ops::MathTensorWriter output(writers[0]);
    std::optional<input_internal::Float32Environment> environment;
    if (profile != SequenceProfile::Strict)
      environment.emplace();
    for (std::uint64_t row = 0; row < count; ++row) {
      InversePoint point;
      point.row = row;
      auto status = classify(&point);
      if (!status.ok())
        return status;
      std::array<std::uint64_t, 4> x{}, y{};
      for (unsigned i = 0; i < point.count; ++i) {
        x[i] = xs[point.first + i];
        y[i] = ys[point.first + i];
      }
      auto computed = arithmetic.inverse(
          pchip, xs.size(), point.first, point.count, point.segment,
          point.selected, point.query, x, y, narrow, consume,
          [] { return Status::success(); },
          environment && environment->active());
      if (!computed.ok())
        return computed.status();
      const auto bits = computed.value();
      if (BinaryParts::decode(bits, narrow).infinite)
        return failure(2, point.row, "inverse output overflow",
                       FailureReason::ArithmeticOverflow);
      std::memcpy(output.address({row}), &bits, width);
    }
    return work(1);
  }
};
struct InverseKernel final {
  bool pchip;
  SequenceProfile profile;
  InverseKernel(bool cubic, SequenceProfile selected)
      : pchip(cubic), profile(selected) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed, "inverse requires one packed writer"};
    auto scratch = phase.allocator.allocate(sizeof(InverseState));
    if (!scratch.ok())
      return scratch.status();
    auto buffer = scratch.take_value();
    std::unique_ptr<InverseState, void (*)(InverseState*)> state(
        new (buffer.data()) InverseState(
            pchip,
            std::get<std::string>(phase.query.parameters.at("out_of_domain")) ==
                "clamp",
            profile, phase),
        [](auto* value) { value->~InverseState(); });
    return state->execute(writers);
  }
};
using InverseProgram = numeric_ops::WholeTensorProgram<InverseKernel>;

OperationDefinition operation(const std::string& name, bool pchip,
                              SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = name;
  auto& traits = definition.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& port : traits.input_schema) {
    port.kind = OperationPortKind::Result;
    port.element_type_mask = 12;
  }
  traits.parameter_schema = {{"dtype", OperationParameterType::String},
                             {"out_of_domain", OperationParameterType::String}};
  traits.requires_metadata_specialization = true;
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(InverseProgram));
  traits.workspace_bytes = sizeof(InverseState);
  definition.specialize_metadata = [profile](const auto& inputs,
                                             const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != 3)
      return Answer(mismatch("inverse port count"));
    std::array<ValueDescriptor, 3> descriptors;
    for (unsigned i = 0; i < inputs.size(); ++i) {
      const auto& tensor = inputs[i].result_schema->tensors[0];
      descriptors[i] = {tensor.descriptor.element_type, tensor.sample_shape()};
      if (descriptors[i].shape.size() != 1 ||
          (descriptors[i].element_type != ElementType::Float32 &&
           descriptors[i].element_type != ElementType::Float64))
        return Answer(mismatch("inverse requires Float32/64 rank-one ports"));
    }
    if (descriptors[0].shape[0] != descriptors[1].shape[0])
      return Answer(mismatch("inverse x/y count mismatch"));
    if (descriptors[0].shape[0] < 2 || descriptors[0].shape[0] > 65536 ||
        !descriptors[2].shape[0] ||
        descriptors[2].shape[0] > (UINT64_C(1) << 40))
      return Answer(numeric_ops::array_parameter_error(
          "inverse requires K=2..65536,N=1..2^40"));
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    const auto& policy = std::get<std::string>(parameters.at("out_of_domain"));
    if ((dtype != "float32" && dtype != "float64") ||
        (policy != "reject" && policy != "clamp"))
      return Answer(numeric_ops::array_parameter_error("inverse dtype/policy"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(
            dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
            descriptors[2].shape));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  definition.start_result = [pchip, profile](const auto&,
                                             const auto& allocator) {
    return ResultContinuation::make<InverseProgram>(allocator, pchip, profile);
  };
  return definition;
}
}  // namespace
Status register_curve_inverse(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool pchip : {false, true}) {
      auto status = registry->register_operation(operation(
          std::string(pchip ? "curve.invert_pchip" : "curve.invert_linear") +
              profile.first,
          pchip, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
