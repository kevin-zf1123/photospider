#include "photospider/ops/contracts.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

#include "01-numeric/lut3d_bake_validation.hpp"
#include "common/domain/typed_sample_validation.hpp"
#include "common/floating_environment.hpp"
namespace ps {
namespace {
Status tuples(const ValidationContext& c, const ColorArrayDescriptor* color) {
  plugin_internal::FloatingEnvironment environment;
  if (!environment.active())
    return {ErrorCode::OperationFailed, "cannot set tuple numeric environment"};
  const auto& spec = c.schema.tensors[0];
  const auto* coverage = c.tensor_coverage(0);
  if (!coverage)
    return {ErrorCode::InvalidArgument, "missing tuple authorization"};
  for (const auto& region : coverage->boxes()) {
    auto acquired = c.acquire_tensor(0, region);
    if (!acquired.ok())
      return acquired.status();
    auto& window = acquired.value();
    const auto read = [&](const auto& at) -> Result<double> {
      if (c.cancellation.cancelled())
        return Result<double>(Status{ErrorCode::Cancelled, {}});
      auto charged = c.consume(1);
      if (!charged.ok())
        return Result<double>(charged);
      auto row = window.row_run(at);
      if (!row.ok())
        return Result<double>(row.status());
      double value;
      if (spec.descriptor.element_type == ElementType::Float32) {
        float f;
        std::memcpy(&f, row.value().data, 4);
        value = f;
      } else {
        std::memcpy(&value, row.value().data, 8);
      }
      return Result<double>(value);
    };
    if (color) {
      auto status = input_internal::validate_color_samples(
          *color, {spec.descriptor.element_type, spec.sample_shape()}, region,
          read, ErrorCode::OperationFailed, [&] {
            return c.cancellation.cancelled() ? ErrorCode::Cancelled
                                              : ErrorCode::Ok;
          });
      if (!status.ok())
        return status;
    } else {
      std::array<double, 4> tuple{};
      auto status =
          input_internal::visit_typed_samples(region, {}, [&](const auto& at) {
            auto value = read(at);
            if (!value.ok())
              return value.status();
            tuple[at.back()] = value.value();
            if (!std::isfinite(value.value()))
              return Status{ErrorCode::OperationFailed,
                            "nonfinite RGBA sample"};
            if (at.back() == 3 &&
                (value.value() < 0 || value.value() > 1 ||
                 (value.value() == 0 &&
                  (tuple[0] != 0 || tuple[1] != 0 || tuple[2] != 0))))
              return Status{ErrorCode::OperationFailed,
                            "invalid premultiplied RGBA tuple"};
            return Status::success();
          });
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace
Status register_ops_contracts(OperationRegistry& registry) {
  ValidationDefinition rgba;
  rgba.identity = rgba_contract();
  rgba.scratch_bytes = 4096;
  rgba.prepare = [](const ValidationPreparation& p)
      -> Result<std::shared_ptr<const void>> {
    if (p.schema.tensors.size() != 1 || !p.schema.fields.empty())
      return Result<std::shared_ptr<const void>>(
          Status{ErrorCode::TypeMismatch, "RGBA tensor required"});
    const auto& spec = p.schema.tensors[0];
    if (spec.descriptor.element_type != ElementType::Float32 ||
        spec.descriptor.shape.back() != 4 || spec.atomic_trailing_axes != 1)
      return Result<std::shared_ptr<const void>>(
          Status{ErrorCode::TypeMismatch,
                 "RGBA contract requires Float32 atomic tuples of four"});
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  rgba.validate = [](const void*, const ValidationContext& c) {
    return tuples(c, nullptr);
  };
  auto status = registry.register_validator(std::move(rgba));
  if (!status.ok())
    return status;
  for (bool report : {false, true}) {
    ValidationDefinition definition;
    definition.identity =
        report ? bake_report_contract() : bake_table_contract();
    definition.requires_complete = report;
    definition.scratch_bytes = 65536;
    definition.prepare = [report](const ValidationPreparation& p)
        -> Result<std::shared_ptr<const void>> {
      auto description = lut3d_bake_from_parameters(p.parameters);
      if (!description.ok())
        return Result<std::shared_ptr<const void>>(description.status());
      auto expected = report ? lut3d_bake_schema(description.value())
                             : lut3d_bake_table_schema(description.value());
      if (!expected.ok() || !p.schema.same_schema(expected.value()))
        return Result<std::shared_ptr<const void>>(Status{
            ErrorCode::TypeMismatch, "bake contract structure mismatch"});
      return Result<std::shared_ptr<const void>>(
          std::make_shared<const Lut3dBakeDescription>(
              description.take_value()));
    };
    definition.validate = [report](const void* raw,
                                   const ValidationContext& c) {
      const auto& description = *static_cast<const Lut3dBakeDescription*>(raw);
      if (!report)
        return tuples(c, &description.output_description);
      auto identity = lut3d_bake_configuration_id(description);
      if (!identity.ok())
        return identity.status();
      auto stored = c.read_field(11, 0, 1);
      if (!stored.ok())
        return stored.status();
      if (stored.value()->bytes().size() != 64 ||
          std::memcmp(stored.value()->bytes().data(), identity.value().data(),
                      64))
        return Status{ErrorCode::TypeMismatch,
                      "bake report configuration mismatch"};
      Lut3dBakeReport value;
      std::uint8_t passed;
      const std::array<void*, 11> output{&passed,
                                         value.axis.data(),
                                         &value.validation_count,
                                         &value.failed_count,
                                         value.max_abs_error.data(),
                                         value.max_error_point.data(),
                                         value.max_error_index.data(),
                                         &value.first_failure_index,
                                         value.first_failure_input.data(),
                                         value.first_failure_reference.data(),
                                         value.first_failure_lut.data()};
      const std::array<std::size_t, 11> sizes{1,  72, 8,  8,  24, 72,
                                              24, 8,  24, 24, 24};
      for (unsigned i = 0; i < 11; ++i) {
        auto field = c.read_field(i, 0, (i == 1 || i == 5) ? 3 : 1);
        if (!field.ok())
          return field.status();
        if (field.value()->bytes().size() != sizes[i])
          return Status{ErrorCode::TypeMismatch, "bake report field width"};
        std::memcpy(output[i], field.value()->bytes().data(), sizes[i]);
      }
      return input_internal::validate_lut3d_bake_report_values(
          description, &value, passed, c.consume);
    };
    status = registry.register_validator(std::move(definition));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps
