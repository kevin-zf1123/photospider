#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_profiles.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace numeric_ops;  // NOLINT(build/namespaces)
struct ArrayKernel final {
  bool constant;
  SequenceProfile profile;
  ArrayKernel(bool constant, SequenceProfile profile)
      : constant(constant), profile(profile) {}
  Status publish(const ResultProgramPhase& phase, ResultBuilder& builder,
                 ResultRelation relation) {
    const auto& target = phase.query.output.result_schema->tensors[0];
    const auto shape = target.sample_shape();
    const auto& input = phase.tensors->at({0, 0});
    const auto source_shape = input.spec().sample_shape();
    const auto width = Value::element_size(target.descriptor.element_type);
    const bool view =
        std::get<std::string>(phase.query.parameters.at("layout")) == "view";
    std::vector<uint64_t> axes;
    if (!constant)
      axes = math_take(parse_array_list(
          std::get<std::string>(phase.query.parameters.at("axis_map")), false));
    Status status;
    if (view && constant) {
      MathTensorReader reader(input, phase.query.cancellation);
      const auto bits = reader.bits({0});
      math_require(phase.consume_work(width + shape.size()));
      auto buffer = math_take(phase.resources.allocator().allocate(width));
      std::memcpy(buffer.data(), &bits, width);
      status = builder.publish_tensor(
          0, Region::whole(shape), {0, std::vector<int64_t>(shape.size(), 0)},
          std::move(buffer).freeze(), std::move(relation),
          {true, true, true, true}, phase.query.cancellation);
    } else if (view) {
      ResultTensorViewTransform transform;
      transform.source_axes.resize(source_shape.size());
      for (size_t axis = 0; axis < source_shape.size(); ++axis)
        transform.source_axes[axis] = {static_cast<int32_t>(axes[axis]), 0,
                                       source_shape[axis] == 1 ? 0 : 1, 1};
      auto window = math_take(
          input.acquire(Region::whole(source_shape), phase.query.cancellation));
      status = builder.publish_tensor_view(
          0, Region::whole(shape), window, transform, std::move(relation),
          {true, true, true, true}, phase.query.cancellation);
      if (status.code == ErrorCode::InvalidArgument &&
          status.message.find("ViewUnavailable") != std::string::npos)
        return {ErrorCode::InvalidArgument,
                "ViewUnavailable: complete broadcast source is not affine",
                FailureReason::InvalidDomain,
                {FailureOrigin::Domain, FailureScope::Run}};
    } else {
      MathTensorReader reader(input, phase.query.cancellation);
      status = builder.publish_tensor_kernel(
          0, Region::whole(shape),
          [&](const auto& writers) -> Status {
            return math_callback(phase, [&]() -> Status {
              if (writers.size() != 1)
                return {ErrorCode::OperationFailed,
                        "array requires one packed writer"};
              std::vector<uint64_t> coordinate(shape.size(), 0),
                  source(source_shape.size(), 0);
              if (constant) {
                const auto bits = reader.bits({0});
                const auto plane_axes = std::min<size_t>(shape.size(), 2);
                const auto plane_samples =
                    shape.back() *
                    (plane_axes == 2 ? shape[shape.size() - 2] : 1);
                const auto planes =
                    math_take(target.sample_count()) / plane_samples;
                for (uint64_t plane = 0; plane < planes; ++plane) {
                  auto rectangle =
                      math_take(writers[0].rectangle_run(coordinate));
                  if (rectangle.row.samples != shape.back() ||
                      rectangle.rows !=
                          (plane_axes == 2 ? shape[shape.size() - 2] : 1) ||
                      rectangle.row.sample_stride_bytes !=
                          static_cast<int64_t>(width) ||
                      (rectangle.rows > 1 &&
                       rectangle.row_stride_bytes !=
                           static_cast<int64_t>(shape.back() * width)))
                    return {ErrorCode::OperationFailed,
                            "constant requires packed rectangles"};
                  math_require(phase.consume_work(width));
                  std::memcpy(rectangle.row.data, &bits, width);
                  const auto bytes = plane_samples * width;
                  for (uint64_t offset = width; offset < bytes;) {
                    const auto size =
                        std::min<uint64_t>({offset, 65536, bytes - offset});
                    math_require(phase.consume_work(size));
                    array_copy_block(rectangle.row.data + offset,
                                     rectangle.row.data, size, profile);
                    offset += size;
                  }
                  for (size_t axis = shape.size() - plane_axes; axis-- > 0;) {
                    if (++coordinate[axis] < shape[axis])
                      break;
                    coordinate[axis] = 0;
                  }
                }
              } else {
                MathTensorWriter writer(writers[0]);
                std::array<uint8_t, 32> block{};
                std::array<uint8_t*, 32> destinations{};
                size_t buffered = 0;
                const auto flush = [&] {
                  const auto lanes = buffered / width;
                  for (size_t lane = 1; lane < lanes; ++lane)
                    if (destinations[lane] != destinations[0] + lane * width)
                      throw Status{ErrorCode::OperationFailed,
                                   "broadcast requires packed output"};
                  array_copy_block(destinations[0], block.data(), buffered,
                                   profile);
                  buffered = 0;
                };
                for (uint64_t i = 0, count = math_take(target.sample_count());
                     i < count; ++i) {
                  math_require(phase.consume_work(axes.size() + width));
                  for (size_t axis = 0; axis < source_shape.size(); ++axis)
                    source[axis] =
                        source_shape[axis] == 1 ? 0 : coordinate[axes[axis]];
                  const auto bits = reader.bits(source);
                  std::memcpy(block.data() + buffered, &bits, width);
                  destinations[buffered / width] = writer.address(coordinate);
                  buffered += width;
                  if (buffered == block.size())
                    flush();
                  math_next(coordinate, shape);
                }
                if (buffered)
                  flush();
              }
              return phase.consume_work(1);
            });
          },
          std::move(relation), {true, true, true, true},
          phase.query.cancellation);
    }
    if (!status.ok())
      return status;
    math_require(phase.consume_work(1));
    if (phase.report_numeric) {
      NumericDiagnostics report;
      report.profile =
          static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
      const auto* implementation = array_implementation(profile, view);
      std::memcpy(report.implementation.data(), implementation,
                  std::strlen(implementation) + 1);
      if (view)
        report.view_elements = math_take(target.sample_count());
      else
        report.copied_elements = math_take(target.sample_count());
      return phase.report_numeric(report);
    }
    return Status::success();
  }
};
using ArrayProgram = WholeTensorProgram<ArrayKernel>;
OperationDefinition array_operation(const std::string& key, bool constant,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].element_type_mask = 127;
  traits.parameter_schema = {{"layout", OperationParameterType::String},
                             {"shape", OperationParameterType::String}};
  if (!constant)
    traits.parameter_schema.push_back(
        {"axis_map", OperationParameterType::String});
  set_whole_tensor_output(traits, ElementType::Float64, sizeof(ArrayProgram));
  operation.specialize_metadata = [constant, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    const auto& source = inputs[0].result_schema->tensors[0];
    const auto source_shape = source.sample_shape();
    if (constant && source_shape != std::vector<uint64_t>{1})
      return mismatch("constant requires one scalar");
    auto shape =
        parse_array_list(std::get<std::string>(parameters.at("shape")), true);
    if (!shape.ok())
      return Answer(shape.status());
    if (!constant) {
      auto axes = parse_array_list(
          std::get<std::string>(parameters.at("axis_map")), false);
      if (!axes.ok())
        return Answer(axes.status());
      if (axes.value().size() != source_shape.size() ||
          shape.value().size() < source_shape.size())
        return mismatch(
            "broadcast must map every source axis without rank reduction");
      uint32_t used = 0;
      for (size_t axis = 0; axis < source_shape.size(); ++axis) {
        const auto mapped = axes.value()[axis];
        if (mapped >= shape.value().size() || (used & (1U << mapped)))
          return Answer(array_parameter_error(
              "broadcast axis map must be injective and in range"));
        used |= 1U << mapped;
        if (source_shape[axis] != 1 &&
            source_shape[axis] != shape.value()[mapped])
          return mismatch("broadcast mapped extent must match or be one");
      }
    }
    const auto& layout = std::get<std::string>(parameters.at("layout"));
    if (layout != "view" && layout != "dense")
      return Answer(
          array_parameter_error("array layout must be view or dense"));
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto schema = numeric_tensor_schema(source.descriptor.element_type,
                                        shape.take_value());
    if (constant && layout == "view")
      schema.tensors[0].atomic_trailing_axes =
          schema.tensors[0].descriptor.shape.size();
    OperationOutputSpecialization result;
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_result = [constant, profile](const auto&,
                                               const auto& allocator) {
    return ResultContinuation::make<ArrayProgram>(allocator, constant, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_arrays(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (bool constant : {true, false}) {
      auto status = registry->register_operation(array_operation(
          std::string(constant ? "numeric.constant" : "numeric.broadcast") +
              variant.first,
          constant, variant.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
