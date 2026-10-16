#include "photospider/ops/contracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

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
    }
  }
  return Status::success();
}
}  // namespace
Status register_ops_contracts(OperationRegistry& registry) {
  Status status;
  struct ScalarRange {
    std::uint32_t slot;
    double minimum, maximum;
  };
  ValidationDefinition scalar;
  scalar.identity = scalar_range_contract();
  scalar.requires_complete = false;
  scalar.prepare = [](const ValidationPreparation& p)
      -> Result<std::shared_ptr<const void>> {
    using Answer = Result<std::shared_ptr<const void>>;
    const auto minimum = p.parameters.find("scalar_range.minimum");
    const auto maximum = p.parameters.find("scalar_range.maximum");
    const auto selected = p.parameters.find("scalar_range.tensor_slot");
    std::int64_t slot = 0;
    if (selected != p.parameters.end()) {
      const auto* value = std::get_if<std::int64_t>(&selected->second);
      if (!value)
        return Answer(
            Status{ErrorCode::InvalidArgument, "invalid scalar range slot"});
      slot = *value;
    }
    const auto* lo = minimum == p.parameters.end()
                         ? nullptr
                         : std::get_if<double>(&minimum->second);
    const auto* hi = maximum == p.parameters.end()
                         ? nullptr
                         : std::get_if<double>(&maximum->second);
    if (!lo || !hi || !std::isfinite(*lo) || !std::isfinite(*hi) || *lo > *hi ||
        slot < 0 || static_cast<std::uint64_t>(slot) >= p.schema.tensors.size())
      return Answer(Status{ErrorCode::InvalidArgument,
                           "invalid scalar range parameters"});
    const auto& tensor = p.schema.tensors[slot];
    if (tensor.descriptor.element_type != ElementType::Float32 ||
        tensor.sample_shape() != std::vector<std::uint64_t>{1})
      return Answer(Status{ErrorCode::TypeMismatch,
                           "scalar range requires a Float32 [1] member"});
    return Answer(std::make_shared<const ScalarRange>(
        ScalarRange{static_cast<std::uint32_t>(slot), *lo, *hi}));
  };
  scalar.validate = [](const void* state, const ValidationContext& c) {
    const auto& range = *static_cast<const ScalarRange*>(state);
    if (c.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    auto charged = c.consume(1);
    if (!charged.ok())
      return charged;
    auto window = c.acquire_tensor(range.slot, Region::whole({1}));
    if (!window.ok())
      return window.status();
    auto sample = window.value().row_run({0});
    if (!sample.ok())
      return sample.status();
    float value;
    std::memcpy(&value, sample.value().data, sizeof(value));
    plugin_internal::FloatingEnvironment environment;
    if (!environment.active())
      return Status{ErrorCode::OperationFailed,
                    "cannot set scalar environment"};
    return std::isfinite(value) && value >= range.minimum &&
                   value <= range.maximum
               ? Status::success()
               : Status{ErrorCode::InvalidArgument,
                        "scalar is nonfinite or outside the declared range",
                        FailureReason::InvalidDomain};
  };
  status = registry.register_validator(std::move(scalar));
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
      const auto read = [&](std::uint32_t slot, void* destination,
                            std::uint64_t bytes) {
        auto window = c.acquire_tensor(
            slot, Region::whole(c.schema.tensors[slot].sample_shape()));
        return window.ok() ? window.value().copy_to(
                                 static_cast<std::uint8_t*>(destination), bytes)
                           : window.status();
      };
      std::array<std::uint8_t, 64> stored{};
      auto copied = read(11, stored.data(), stored.size());
      if (!copied.ok())
        return copied;
      if (std::memcmp(stored.data(), identity.value().data(), 64))
        return Status{ErrorCode::TypeMismatch,
                      "bake report configuration mismatch"};
      std::array<std::uint8_t, 8> measured{};
      copied = read(12, measured.data(), measured.size());
      if (!copied.ok())
        return copied;
      if (std::all_of(measured.begin(), measured.end(),
                      [](auto byte) { return byte == 0; }))
        return Status{ErrorCode::OperationFailed,
                      "missing measured table identity",
                      FailureReason::InvalidDomain};
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
        auto copied = read(i, output[i], sizes[i]);
        if (!copied.ok())
          return copied;
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
