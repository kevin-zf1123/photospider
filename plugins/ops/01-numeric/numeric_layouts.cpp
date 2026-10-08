#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_profiles.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
enum class LayoutKind { Reshape, Transpose, Slice };
struct LayoutProgram final {
  LayoutKind kind;
  SequenceProfile profile;
  bool started = false;
  Footprint output;
  std::array<uint64_t, 8> permutation{};
  std::array<int64_t, 8> starts{}, steps{};
  LayoutProgram(LayoutKind kind, SequenceProfile profile)
      : kind(kind), profile(profile) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    auto scratch =
        math_take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const auto& source = phase.query.inputs[0].result_schema->tensors[0];
    const auto source_shape = source.sample_shape();
    const auto& schema = *phase.query.output.result_schema;
    const auto& target_shape = schema.tensors[0].descriptor.shape;
    const bool singleton = std::all_of(target_shape.begin(), target_shape.end(),
                                       [](auto n) { return n == 1; });
    const auto active_ports =
        kind == LayoutKind::Slice ? (singleton ? 2U : 3U) : 1U;
    if (!started) {
      output = phase.query.tensor_outputs
                   ? *phase.query.tensor_outputs
                   : math_take(Footprint::all(target_shape));
      started = true;
      if (!output.empty()) {
        ResultProgramNeed need;
        for (uint32_t port = 0; port < active_ports; ++port)
          need.tensors.push_back(
              {port, 0,
               math_take(Footprint::all(phase.query.inputs[port]
                                            .result_schema->tensors[0]
                                            .sample_shape())),
               13});
        return Result<ResultProgramPoll>(std::move(need));
      }
    }
    auto builder = math_take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {},
        phase.association ? std::vector<uint64_t>(phase.association->begin(),
                                                  phase.association->end())
                          : std::vector<uint64_t>{}));
    ResultRelation descriptor, relation;
    const auto count = math_take(schema.tensors[0].sample_count());
    for (uint32_t port = 0; port < active_ports; ++port) {
      auto basis = math_take(
          ResultRelation::cartesian(phase.resources, 1,
                                    {port, 8, 0, output.empty() ? 0U : 1U,
                                     ResultSupportTarget::Descriptor, 0}));
      descriptor = descriptor.valid()
                       ? math_take(ResultRelation::unite(phase.resources,
                                                         {descriptor, basis}))
                       : std::move(basis);
      if (!output.empty()) {
        auto support = math_take(
            ResultRelation::cartesian(phase.resources, count,
                                      {port, 1, 0,
                                       math_take(phase.query.inputs[port]
                                                     .result_schema->tensors[0]
                                                     .sample_count()),
                                       ResultSupportTarget::Tensor, 0}));
        relation = relation.valid() ? math_take(ResultRelation::unite(
                                          phase.resources, {relation, support}))
                                    : std::move(support);
      }
    }
    math_require(builder.bind_descriptor_relation(std::move(descriptor)));
    if (output.empty())
      return Result<ResultProgramPoll>(
          ResultPublication{math_take(builder.seal()), true});
    math_require(phase.consume_work(source_shape.size() + target_shape.size()));
    ResultTensorViewTransform transform;
    transform.reshape = kind == LayoutKind::Reshape;
    if (kind == LayoutKind::Transpose) {
      auto parsed = math_take(parse_array_list(
          std::get<std::string>(phase.query.parameters.at("permutation")),
          false));
      std::copy(parsed.begin(), parsed.end(), permutation.begin());
      transform.source_axes.resize(source_shape.size());
      for (size_t axis = 0; axis < target_shape.size(); ++axis)
        transform.source_axes[permutation[axis]].output_axis = axis;
    } else if (kind == LayoutKind::Slice) {
      MathTensorReader start(phase.tensors->at({1, 0}),
                             phase.query.cancellation);
      std::optional<MathTensorReader> step;
      if (!singleton)
        step.emplace(phase.tensors->at({2, 0}), phase.query.cancellation);
      transform.source_axes.resize(source_shape.size());
      for (size_t axis = 0; axis < source_shape.size(); ++axis) {
        math_require(phase.consume_work(8));
        const auto start_bits = start.bits({axis});
        std::memcpy(&starts[axis], &start_bits, 8);
        if (target_shape[axis] > 1) {
          const auto step_bits = step->bits({axis});
          std::memcpy(&steps[axis], &step_bits, 8);
        }
        const auto last =
            static_cast<__int128>(starts[axis]) +
            static_cast<__int128>(target_shape[axis] - 1) * steps[axis];
        if (starts[axis] < 0 ||
            static_cast<uint64_t>(starts[axis]) >= source_shape[axis] ||
            (target_shape[axis] > 1 && !steps[axis]) || last < 0 ||
            last >= source_shape[axis])
          return Result<ResultProgramPoll>(
              Status{ErrorCode::InvalidArgument,
                     "InvalidSlice: axis=" + std::to_string(axis) +
                         " start=" + std::to_string(starts[axis]) +
                         " step=" + std::to_string(steps[axis]),
                     FailureReason::InvalidDomain,
                     {FailureOrigin::Domain, FailureScope::Run}});
        // Singleton axes have no meaningful physical stride, including a
        // source INT64_MIN stride with a numerically ignored negative step.
        transform.source_axes[axis] = {
            static_cast<int32_t>(axis), static_cast<uint64_t>(starts[axis]),
            target_shape[axis] > 1 ? steps[axis] : 0, 1};
      }
    }
    const auto& layout =
        std::get<std::string>(phase.query.parameters.at("layout"));
    bool viewed = false;
    if (layout != "dense") {
      auto window = math_take(phase.tensors->at({0, 0}).acquire(
          Region::whole(source_shape), phase.query.cancellation));
      auto status = builder.publish_tensor_view(
          0, Region::whole(target_shape), window, transform, relation,
          {true, true, true, true}, phase.query.cancellation);
      math_require(phase.consume_work(1));
      if (status.ok()) {
        viewed = true;
      } else if (status.code != ErrorCode::InvalidArgument ||
                 status.message.find("ViewUnavailable") == std::string::npos) {
        return Result<ResultProgramPoll>(status);
      } else if (layout == "view") {
        return Result<ResultProgramPoll>(
            Status{ErrorCode::InvalidArgument,
                   "ViewUnavailable: complete output is not one affine owner",
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Domain, FailureScope::Run}});
      }
    }
    if (!viewed) {
      MathTensorReader reader(phase.tensors->at({0, 0}),
                              phase.query.cancellation);
      math_require(builder.publish_tensor_kernel(
          0, Region::whole(target_shape),
          [&](const auto& writers) {
            return math_callback(phase, [&]() -> Status {
              if (writers.size() != 1)
                return Status{ErrorCode::OperationFailed,
                              "layout requires one packed output window"};
              MathTensorWriter writer(writers[0]);
              std::vector<uint64_t> coordinate(target_shape.size(), 0),
                  source_coordinate(source_shape.size(), 0);
              const auto width =
                  Value::element_size(source.descriptor.element_type);
              std::array<uint8_t, 32> block{};
              std::array<uint8_t*, 32> destinations{};
              size_t buffered = 0;
              const auto flush = [&] {
                const auto lanes = buffered / width;
                bool packed = true;
                for (size_t lane = 1; lane < lanes; ++lane)
                  packed = packed &&
                           destinations[lane] == destinations[0] + lane * width;
                if (packed) {
                  array_copy_block(destinations[0], block.data(), buffered,
                                   profile);
                } else {
                  for (size_t lane = 0; lane < lanes; ++lane)
                    std::memcpy(destinations[lane], block.data() + lane * width,
                                width);
                }
                buffered = 0;
              };
              for (uint64_t i = 0; i < count; ++i) {
                auto status = phase.consume_work(source_shape.size() +
                                                 target_shape.size() + width);
                if (!status.ok())
                  return status;
                if (kind == LayoutKind::Reshape) {
                  auto ordinal = i;
                  for (size_t axis = source_shape.size(); axis-- > 0;) {
                    source_coordinate[axis] = ordinal % source_shape[axis];
                    ordinal /= source_shape[axis];
                  }
                } else if (kind == LayoutKind::Transpose) {
                  for (size_t axis = 0; axis < target_shape.size(); ++axis)
                    source_coordinate[permutation[axis]] = coordinate[axis];
                } else {
                  for (size_t axis = 0; axis < source_shape.size(); ++axis)
                    source_coordinate[axis] = static_cast<uint64_t>(
                        static_cast<__int128>(starts[axis]) +
                        static_cast<__int128>(coordinate[axis]) * steps[axis]);
                }
                const auto bits = reader.bits(source_coordinate);
                std::memcpy(block.data() + buffered, &bits, width);
                destinations[buffered / width] = writer.address(coordinate);
                buffered += width;
                if (buffered == block.size())
                  flush();
                math_next(coordinate, target_shape);
              }
              if (buffered)
                flush();
              return phase.consume_work(1);
            });
          },
          relation, {true, true, true, true}, phase.query.cancellation));
    }
    if (phase.report_numeric) {
      NumericDiagnostics report;
      report.profile =
          static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
      const auto* implementation = array_implementation(profile, viewed);
      const auto size = std::strlen(implementation);
      if (size >= report.implementation.size())
        return Result<ResultProgramPoll>(Status{
            ErrorCode::OperationFailed, "layout diagnostic identity overflow"});
      std::memcpy(report.implementation.data(), implementation, size + 1);
      if (viewed)
        report.view_elements = count;
      else
        report.copied_elements = count;
      math_require(phase.report_numeric(report));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{math_take(builder.seal()), true});
  } catch (const Status& status) {
    numeric_ops::math_record_failure(phase, status);
    return Result<ResultProgramPoll>(status);
  } catch (const std::bad_alloc&) {
    auto failure = Status{ErrorCode::ResourceExhausted,
                          {},
                          FailureReason::CapacityLimit,
                          {FailureOrigin::Resource, FailureScope::Run}};
    numeric_ops::math_record_failure(phase, failure);
    return Result<ResultProgramPoll>(failure);
  }
};
OperationDefinition layout_operation(const std::string& key, LayoutKind kind,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  // Physical viewability depends on source owner/stride partitioning. Current
  // disposable content caches cannot witness that partition, so do not reuse
  // these results across executions. Pure active-Run sharing remains valid.
  traits.cacheable = false;
  traits.requires_metadata_specialization = true;
  traits.input_count = kind == LayoutKind::Slice ? 3 : 1;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 127;
  }
  for (unsigned port = 1; port < traits.input_count; ++port) {
    traits.input_schema[port].element_type_mask = 0;
    traits.input_schema[port].rank = 1;
    traits.input_schema[port].element_type =
        static_cast<std::uint32_t>(ElementType::Int64);
  }
  const std::string parameter = kind == LayoutKind::Reshape     ? "shape"
                                : kind == LayoutKind::Transpose ? "permutation"
                                                                : "counts";
  traits.parameter_schema = {{parameter, OperationParameterType::String},
                             {"layout", OperationParameterType::String}};
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(LayoutProgram));
  operation.specialize_metadata =
      [kind, profile, parameter](const auto& inputs, const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    const auto& member = inputs[0].result_schema->tensors[0];
    const ValueDescriptor input{member.descriptor.element_type,
                                member.sample_shape()};
    std::uint64_t source_count = 1;
    for (auto size : input.shape) {
      if (!size || size > (UINT64_C(1) << 40) / source_count)
        return mismatch("layout source exceeds 2^40 elements");
      source_count *= size;
    }
    auto parameters_list = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at(parameter)),
        kind != LayoutKind::Transpose);
    if (!parameters_list.ok())
      return Answer(parameters_list.status());
    auto shape = parameters_list.value();
    if (kind == LayoutKind::Transpose) {
      if (shape.size() != input.shape.size())
        return mismatch("transpose permutation rank differs from input");
      unsigned used = 0;
      for (std::size_t j = 0; j < shape.size(); ++j) {
        const auto axis = parameters_list.value()[j];
        if (axis >= shape.size() || (used & (1U << axis)))
          return Answer(numeric_ops::array_parameter_error(
              "transpose axes must form a permutation"));
        used |= 1U << axis;
        shape[j] = input.shape[axis];
      }
    } else if (kind == LayoutKind::Reshape) {
      std::uint64_t count = 1;
      for (auto size : shape)
        count *= size;
      if (count != source_count)
        return Answer(numeric_ops::array_parameter_error(
            "reshape element products must match"));
    } else {
      if (shape.size() != input.shape.size())
        return mismatch("slice counts rank differs from input");
      for (unsigned port = 1; port < 3; ++port)
        if (inputs[port].result_schema->tensors[0].sample_shape() !=
            std::vector<std::uint64_t>{shape.size()})
          return mismatch("slice controls must be Int64[input rank]");
    }
    const auto& layout = std::get<std::string>(parameters.at("layout"));
    if (layout != "view" && layout != "auto" && layout != "dense")
      return Answer(numeric_ops::array_parameter_error(
          "layout must be auto, view or dense"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(input.element_type, shape));
    if (kind == LayoutKind::Slice &&
        std::all_of(shape.begin(), shape.end(),
                    [](auto size) { return size == 1; }))
      result.input_indices = std::vector<std::uint32_t>{0, 1};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_result = [kind, profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<LayoutProgram>(allocator, kind, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_layouts(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& item : {std::make_pair("reshape", LayoutKind::Reshape),
                             std::make_pair("transpose", LayoutKind::Transpose),
                             std::make_pair("slice", LayoutKind::Slice)}) {
      auto status = registry->register_operation(
          layout_operation(std::string("array.") + item.first + variant.first,
                           item.second, variant.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
