#include "photospider/ops/numeric/lut3d_bake.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/lut3d_bake_validation.hpp"
#include "common/exact_binary_sum.hpp"
#include "common/numeric_bits.hpp"
#include "photospider/core/content_digest.hpp"
#include "photospider/core/status_helpers.hpp"

namespace ps {
namespace {

// Validate the registered report's rounded axis without retaining a knot table
// or depending on an operator's sampling profile. Schema validation bounds the
// shape to 2..256; every weighted sum uses the same fixed work charge as
// before.
Status validate_report_axis(
    const std::array<double, 3>& axis, unsigned count,
    const std::function<Status(std::uint64_t)>& consume) {
  const auto invalid = [](std::string message) {
    return Status{ErrorCode::OperationFailed,
                  std::move(message),
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Domain, FailureScope::Group}};
  };
  std::array<std::uint64_t, 3> bits{};
  for (unsigned j = 0; j < 3; ++j) {
    bits[j] = core_internal::binary64_bits(axis[j]);
    if (!core_internal::finite_binary64(axis[j]))
      return invalid("nonfinite axis component=" + std::to_string(j));
  }
  if (count == 1) {
    if (bits[0] != bits[1] || bits[2] != 0)
      return invalid(
          "singleton axis requires bit-identical endpoints and +0 step");
    return consume(1);
  }
  const auto first_key = core_internal::binary64_order_key(bits[0]);
  const auto last_key = core_internal::binary64_order_key(bits[1]);
  if (first_key == last_key)
    return invalid("axis endpoints must differ");
  const bool descending = last_key < first_key;
  core_internal::ExactBinarySum first, second;
  const auto rounded = [&](double a, double b, std::uint32_t wa,
                           std::uint32_t wb, bool subtract) {
    first.set(a);
    second.set(b);
    first.multiply(wa);
    second.multiply(wb);
    if (subtract)
      second.negative = !second.negative;
    first.add(second);
    const bool negative_zero = !subtract &&
                               (core_internal::binary64_bits(a) >> 63) &&
                               (!wb || (core_internal::binary64_bits(b) >> 63));
    return first.rounded_bits(count - 1, false, negative_zero);
  };
  auto work = consume(8192);
  if (!work.ok())
    return work;
  const auto step = rounded(axis[1], axis[0], 1, 1, true);
  constexpr auto sign = UINT64_C(1) << 63;
  constexpr auto infinity = UINT64_C(0x7ff0000000000000);
  if (!(step & (sign - 1)) || (step & infinity) == infinity ||
      ((step >> 63) != descending) || step != bits[2])
    return invalid("axis component=2 inconsistent or unrepresentable step");
  std::uint64_t previous = 0;
  for (unsigned j = 0; j < count; ++j) {
    work = consume(1);
    if (!work.ok())
      return work;
    auto knot = j == 0 ? bits[0] : bits[1];
    if (j && j + 1 != count) {
      work = consume(8192);
      if (!work.ok())
        return work;
      knot = rounded(axis[0], axis[1], count - 1 - j, j, false);
    }
    const auto ordered = core_internal::binary64_order_key(knot);
    const auto key = descending ? UINT64_MAX - ordered : ordered;
    if (j && previous >= key)
      return invalid("non-strict reconstructed axis knot=" + std::to_string(j));
    previous = key;
  }
  return Status::success();
}

Status validate(const Lut3dBakeDescription& spec) {
  for (auto n : spec.shape)
    if (n < 2 || n > 256)
      return core_internal::invalid_schema_domain(
          "LUT3D bake shape must be 2..256");
  if ((spec.interpolation != Lut3dInterpolation::Trilinear &&
       spec.interpolation != Lut3dInterpolation::Tetrahedral) ||
      !core_internal::finite_binary64(spec.atol) ||
      !core_internal::finite_binary64(spec.rtol) ||
      ((core_internal::binary64_bits(spec.atol) >> 63) &&
       (core_internal::binary64_bits(spec.atol) << 1)) ||
      ((core_internal::binary64_bits(spec.rtol) >> 63) &&
       (core_internal::binary64_bits(spec.rtol) << 1)) ||
      (spec.table_dtype != ElementType::Float32 &&
       spec.table_dtype != ElementType::Float64) ||
      (spec.source_dtype != ElementType::Float32 &&
       spec.source_dtype != ElementType::Float64) ||
      spec.extra_points > 1048576 || spec.recipe_identity.size() != 64)
    return core_internal::invalid_schema_domain(
        "invalid LUT3D bake report schema");
  for (char c : spec.recipe_identity)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return core_internal::invalid_schema_domain(
          "invalid LUT3D bake report schema");
  for (const auto* description :
       {&spec.input_description, &spec.output_description}) {
    if (description->model != spec.input_description.model ||
        description->model == ColorModel::Cmyk ||
        description->association != ColorAssociation::None ||
        description->source_layout != ColorSourceLayout::Interleaved)
      return core_internal::invalid_schema_domain(
          "LUT3D bake requires same-model three-component colors");
    auto checked = validate_color_array_descriptor(
        *description, {ElementType::Float64, {1, 3}});
    if (!checked.ok())
      return checked;
  }
  return Status::success();
}
}  // namespace
Result<std::string> lut3d_bake_configuration_id(
    const Lut3dBakeDescription& spec) {
  auto checked = validate(spec);
  if (!checked.ok())
    return Result<std::string>(checked);
  content_internal::Sha256 hash;
  hash.text("photospider.bake-configuration.v1");
  hash.text(spec.recipe_identity);
  for (auto extent : spec.shape)
    hash.integer(extent);
  hash.integer(static_cast<unsigned>(spec.interpolation));
  hash.integer(static_cast<unsigned>(spec.source_dtype));
  hash.integer(static_cast<unsigned>(spec.table_dtype));
  hash.integer(spec.extra_points);
  hash.integer(core_internal::binary64_bits(spec.atol == 0 ? 0 : spec.atol));
  hash.integer(core_internal::binary64_bits(spec.rtol == 0 ? 0 : spec.rtol));
  for (const auto* description :
       {&spec.input_description, &spec.output_description}) {
    auto bytes = encode_color_array(*description);
    if (!bytes.ok())
      return Result<std::string>(bytes.status());
    hash.integer(bytes.value().size());
    hash.bytes(bytes.value().data(), bytes.value().size());
  }
  return Result<std::string>(hash.finish());
}
Result<SchemaTemplate> lut3d_bake_schema(const Lut3dBakeDescription& spec) {
  auto checked = validate(spec);
  if (!checked.ok())
    return Result<SchemaTemplate>(checked);
  SchemaTemplate schema;
  schema.id = "curve.bake_lut3d.report";
  schema.version = 2;
  schema.domain = {{ResultExtentKind::Fixed, 1}};
  const auto add = [&](const char* key, ElementType dtype,
                       std::vector<std::uint64_t> shape, unsigned atomic) {
    ResultTensorSpec member;
    member.key = key;
    member.descriptor = {dtype, std::move(shape)};
    member.atomic_trailing_axes = atomic;
    schema.tensors.push_back(std::move(member));
  };
  add("passed", ElementType::UInt8, {1}, 0);
  add("axis", ElementType::Float64, {3, 3}, 1);
  add("validation_count", ElementType::Int64, {1}, 0);
  add("failed_count", ElementType::Int64, {1}, 0);
  add("max_abs_error", ElementType::Float64, {1, 3}, 1);
  add("max_error_point", ElementType::Float64, {3, 3}, 1);
  add("max_error_index", ElementType::Int64, {1, 3}, 1);
  add("first_failure_index", ElementType::Int64, {1}, 0);
  add("first_failure_input", ElementType::Float64, {1, 3}, 1);
  add("first_failure_reference", ElementType::Float64, {1, 3}, 1);
  add("first_failure_lut", ElementType::Float64, {1, 3}, 1);
  add("configuration_id", ElementType::UInt8, {1, 64}, 1);
  add("measured_table_id", ElementType::UInt8, {1, 8}, 1);
  return Result<SchemaTemplate>(std::move(schema));
}
Result<SchemaTemplate> lut3d_bake_table_schema(
    const Lut3dBakeDescription& spec) {
  auto made = lut3d_bake_schema(spec);
  if (!made.ok())
    return made;
  auto schema = made.take_value();
  schema.id = "curve.bake_lut3d.table";
  schema.domain.clear();
  schema.version = 2;
  for (auto n : spec.shape)
    schema.domain.push_back({ResultExtentKind::Fixed, n});
  schema.tensors.clear();
  ResultTensorSpec tensor;
  tensor.key = "colors";
  tensor.descriptor = {spec.table_dtype,
                       {spec.shape[0], spec.shape[1], spec.shape[2], 3}};
  tensor.atomic_trailing_axes = 1;
  schema.tensors = {std::move(tensor)};
  return Result<SchemaTemplate>(std::move(schema));
}
Status validate_lut3d_bake_schema(const SchemaTemplate& schema,
                                  const Lut3dBakeDescription& description) {
  auto expected = schema.id == "curve.bake_lut3d.report"
                      ? lut3d_bake_schema(description)
                      : lut3d_bake_table_schema(description);
  if (!expected.ok())
    return expected.status();
  return schema.same_schema(expected.value())
             ? Status::success()
             : core_internal::invalid_schema_domain("LUT3D structure mismatch");
}
Result<Lut3dBakeReport> read_lut3d_bake_report(
    const ResultRef& result, const Lut3dBakeDescription& description,
    std::uint64_t maximum_window, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) {
  using Answer = Result<Lut3dBakeReport>;
  if (!result.valid() || result.schema().id != "curve.bake_lut3d.report")
    return Answer(core_internal::invalid_schema_domain(
        "invalid LUT3D bake report schema"));
  auto checked = validate_lut3d_bake_schema(result.schema(), description);
  if (!checked.ok())
    return Answer(checked);
  auto descriptor = result.descriptor();
  if (!descriptor.ok())
    return Answer(descriptor.status());
  const auto work = [&](std::uint64_t count) {
    if (cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return consume_work ? consume_work(count) : Status::success();
  };
  auto charged = work(result.schema().canonical_size() + 361);
  if (!charged.ok())
    return Answer(charged);
  auto identity = lut3d_bake_configuration_id(description);
  if (!identity.ok())
    return Answer(identity.status());
  const auto read = [&](std::uint32_t slot, void* destination,
                        std::uint64_t bytes) {
    if (bytes > maximum_window)
      return Status{ErrorCode::ResourceExhausted, {}};
    auto window = result.acquire_tensor(
        descriptor.value(), slot,
        Region::whole(result.schema().tensors[slot].sample_shape()),
        cancellation);
    return window.ok() ? window.value().copy_to(
                             static_cast<std::uint8_t*>(destination), bytes)
                       : window.status();
  };
  std::array<std::uint8_t, 64> stored_identity{};
  auto copied_identity =
      read(11, stored_identity.data(), stored_identity.size());
  if (!copied_identity.ok())
    return Answer(copied_identity);
  if (std::memcmp(stored_identity.data(), identity.value().data(), 64))
    return Answer(
        core_internal::invalid_domain("bake report configuration mismatch"));
  Lut3dBakeReport report;
  std::array<std::uint8_t, 8> measured{};
  auto copied_measured = read(12, measured.data(), measured.size());
  if (!copied_measured.ok())
    return Answer(copied_measured);
  for (unsigned i = 0; i < measured.size(); ++i)
    report.measured_table_id |= std::uint64_t{measured[i]} << (8 * i);
  if (!report.measured_table_id)
    return Answer(
        core_internal::invalid_domain("missing measured table identity"));
  std::uint8_t passed = 0;
  const std::array<void*, 11> fields{&passed,
                                     report.axis.data(),
                                     &report.validation_count,
                                     &report.failed_count,
                                     report.max_abs_error.data(),
                                     report.max_error_point.data(),
                                     report.max_error_index.data(),
                                     &report.first_failure_index,
                                     report.first_failure_input.data(),
                                     report.first_failure_reference.data(),
                                     report.first_failure_lut.data()};
  const std::array<std::uint64_t, 11> sizes{1,  72, 8,  8,  24, 72,
                                            24, 8,  24, 24, 24};
  for (unsigned field = 0; field < fields.size(); ++field) {
    auto copied = read(field, fields[field], sizes[field]);
    if (!copied.ok())
      return Answer(copied);
  }
  auto validated = input_internal::validate_lut3d_bake_report_values(
      description, &report, passed, work);
  return validated.ok() ? Answer(report) : Answer(validated);
}
namespace input_internal {
Status validate_lut3d_bake_report_values(
    const Lut3dBakeDescription& description, Lut3dBakeReport* output,
    std::uint8_t passed, const std::function<Status(std::uint64_t)>& work) {
  auto& report = *output;
  auto expected = description.extra_points;
  std::uint64_t centers = 1;
  for (auto n : description.shape)
    centers *= n - 1;
  expected += centers;
  const auto malformed = [&] {
    return Status{ErrorCode::OperationFailed,
                  "invalid measured LUT3D report values",
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Domain, FailureScope::Group}};
  };
  if (passed > 1 ||
      report.validation_count != static_cast<std::int64_t>(expected) ||
      report.failed_count < 0 ||
      report.failed_count > report.validation_count ||
      (passed != (report.failed_count == 0)))
    return malformed();
  report.passed = passed != 0;
  for (unsigned i = 0; i < 3; ++i) {
    auto valid = validate_report_axis(
        {report.axis[3 * i], report.axis[3 * i + 1], report.axis[3 * i + 2]},
        description.shape[i], work);
    if (!valid.ok()) {
      if (valid.detail.origin == FailureOrigin::Domain)
        valid.detail.scope = FailureScope::Group;
      return valid;
    }
  }
  const auto valid_point = [&](const double* point) {
    for (unsigned c = 0; c < 3; ++c) {
      if (!core_internal::finite_binary64(point[c]))
        return false;
      const auto first = core_internal::binary64_order_key(
          core_internal::binary64_bits(report.axis[3 * c]));
      const auto last = core_internal::binary64_order_key(
          core_internal::binary64_bits(report.axis[3 * c + 1]));
      const auto key = core_internal::binary64_order_key(
          core_internal::binary64_bits(point[c]));
      if (key < std::min(first, last) || key > std::max(first, last))
        return false;
    }
    return true;
  };
  const auto valid_chroma = [](const ColorArrayDescriptor& color,
                               double chroma) {
    return (color.model != ColorModel::Cielch &&
            color.model != ColorModel::Oklch) ||
           !(core_internal::binary64_bits(chroma) >> 63) ||
           !(core_internal::binary64_bits(chroma) << 1);
  };
  if (!valid_chroma(description.input_description, report.axis[3]) ||
      !valid_chroma(description.input_description, report.axis[4]))
    return malformed();
  for (unsigned c = 0; c < 3; ++c)
    if (!valid_point(report.max_error_point.data() + 3 * c))
      return malformed();
  if (!report.passed && (!valid_point(report.first_failure_input.data()) ||
                         !valid_chroma(description.output_description,
                                       report.first_failure_reference[1]) ||
                         !valid_chroma(description.output_description,
                                       report.first_failure_lut[1])))
    return malformed();

  for (const auto* values : {&report.axis, &report.max_error_point})
    for (auto value : *values)
      if (!core_internal::finite_binary64(value))
        return malformed();
  for (unsigned i = 0; i < 3; ++i) {
    const auto error = core_internal::binary64_bits(report.max_abs_error[i]);
    if ((error >> 63) || error > UINT64_C(0x7ff0000000000000) ||
        report.max_error_index[i] < 0 ||
        report.max_error_index[i] >= report.validation_count)
      return malformed();
    for (auto value :
         {report.first_failure_input[i], report.first_failure_reference[i],
          report.first_failure_lut[i]})
      if (!core_internal::finite_binary64(value) ||
          (report.passed && core_internal::binary64_bits(value)))
        return malformed();
  }
  if (report.passed ? report.first_failure_index != -1
                    : (report.first_failure_index < 0 ||
                       report.first_failure_index >= report.validation_count))
    return malformed();
  return Status::success();
}
}  // namespace input_internal
Result<Lut3dBakeDescription> lut3d_bake_from_parameters(
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<Lut3dBakeDescription>;
  Lut3dBakeDescription spec;
  spec.shape = {
      static_cast<std::uint64_t>(std::get<std::int64_t>(parameters.at("n0"))),
      static_cast<std::uint64_t>(std::get<std::int64_t>(parameters.at("n1"))),
      static_cast<std::uint64_t>(std::get<std::int64_t>(parameters.at("n2")))};
  const auto& method = std::get<std::string>(parameters.at("interpolation"));
  if (method != "trilinear" && method != "tetrahedral")
    return Answer(core_internal::invalid_domain("bake interpolation"));
  spec.interpolation = method == "trilinear" ? Lut3dInterpolation::Trilinear
                                             : Lut3dInterpolation::Tetrahedral;
  spec.atol = std::get<double>(parameters.at("atol"));
  spec.rtol = std::get<double>(parameters.at("rtol"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& source = std::get<std::string>(parameters.at("source_dtype"));
  if ((dtype != "float32" && dtype != "float64") ||
      (source != "float32" && source != "float64"))
    return Answer(core_internal::invalid_domain("bake dtype"));
  spec.table_dtype =
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
  spec.source_dtype =
      source == "float32" ? ElementType::Float32 : ElementType::Float64;
  spec.extra_points = std::get<std::int64_t>(parameters.at("extra_count"));
  auto a = color_array_from_parameter(
           std::get<std::string>(parameters.at("input_color_description"))),
       b = color_array_from_parameter(
           std::get<std::string>(parameters.at("output_color_description")));
  if (!a.ok() || !b.ok())
    return Answer(!a.ok() ? a.status() : b.status());
  spec.input_description = a.take_value();
  spec.output_description = b.take_value();
  spec.recipe_identity = std::get<std::string>(parameters.at("source_recipe"));
  auto schema = lut3d_bake_schema(spec);
  return schema.ok() ? Answer(std::move(spec)) : Answer(schema.status());
}
}  // namespace ps
