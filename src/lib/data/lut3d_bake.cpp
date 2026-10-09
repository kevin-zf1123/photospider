#include "photospider/data/lut3d_bake.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/exact_binary_sum.hpp"
#include "core/numeric_bits.hpp"
#include "core/status_helpers.hpp"
#include "data/lut3d_bake_validation.hpp"

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

void append(ResultFacet* facet, std::uint64_t word) {
  for (unsigned i = 0; i < 8; ++i)
    facet->payload.push_back(word >> (8 * i));
}
void text(ResultFacet* facet, const std::string& value) {
  append(facet, value.size());
  facet->payload.insert(facet->payload.end(), value.begin(), value.end());
}
struct Reader {
  const ResourceVector<std::uint8_t>& bytes;
  std::size_t offset = 0;
  bool valid = true;
  std::uint64_t word() {
    if (offset > bytes.size() || bytes.size() - offset < 8) {
      valid = false;
      return 0;
    }
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
      value |= static_cast<std::uint64_t>(bytes[offset++]) << (8 * i);
    return value;
  }
  std::string text() {
    auto count = word();
    if (!valid || count > bytes.size() - offset) {
      valid = false;
      return {};
    }
    std::string value(bytes.begin() + offset, bytes.begin() + offset + count);
    offset += count;
    return value;
  }
};
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
Result<SchemaTemplate> lut3d_bake_schema(const Lut3dBakeDescription& spec) {
  auto checked = validate(spec);
  if (!checked.ok())
    return Result<SchemaTemplate>(checked);
  auto input = color_array_parameter(spec.input_description),
       output = color_array_parameter(spec.output_description);
  if (!input.ok() || !output.ok())
    return Result<SchemaTemplate>(!input.ok() ? input.status()
                                              : output.status());
  SchemaTemplate schema;
  schema.id = "curve.bake_lut3d.report";
  schema.domain = {{ResultExtentKind::Fixed, 1}};
  schema.fields = {
      {"passed", ElementType::UInt8, {}, {}},
      {"axis", ElementType::Float64, {ResultExtentKind::Fixed, 3}, {3}},
      {"validation_count", ElementType::Int64, {}, {}},
      {"failed_count", ElementType::Int64, {}, {}},
      {"max_abs_error", ElementType::Float64, {}, {3}},
      {"max_error_point",
       ElementType::Float64,
       {ResultExtentKind::Fixed, 3},
       {3}},
      {"max_error_index", ElementType::Int64, {}, {3}},
      {"first_failure_index", ElementType::Int64, {}, {}},
      {"first_failure_input", ElementType::Float64, {}, {3}},
      {"first_failure_reference", ElementType::Float64, {}, {3}},
      {"first_failure_lut", ElementType::Float64, {}, {3}}};
  ResultFacet metadata;
  metadata.key = "curve.bake_lut3d.measured";
  for (auto n : spec.shape)
    append(&metadata, n);
  append(&metadata, static_cast<std::uint64_t>(spec.interpolation));
  append(&metadata, core_internal::binary64_bits(spec.atol));
  append(&metadata, core_internal::binary64_bits(spec.rtol));
  append(&metadata, static_cast<std::uint64_t>(spec.table_dtype));
  append(&metadata, static_cast<std::uint64_t>(spec.source_dtype));
  append(&metadata, spec.extra_points);
  text(&metadata, input.value());
  text(&metadata, output.value());
  text(&metadata, spec.recipe_identity);
  schema.metadata.push_back(std::move(metadata));
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
  schema.fields.clear();
  ResultTensorSpec tensor;
  tensor.key = "colors";
  tensor.descriptor = {spec.table_dtype,
                       {spec.shape[0], spec.shape[1], spec.shape[2], 3}};
  auto facet = encode_color_array(spec.output_description);
  if (!facet.ok())
    return Result<SchemaTemplate>(facet.status());
  tensor.facets = {facet.take_value()};
  tensor.atomic_trailing_axes = 1;
  schema.tensors = {std::move(tensor)};
  return Result<SchemaTemplate>(std::move(schema));
}
Result<Lut3dBakeDescription> lut3d_bake_description(
    const SchemaTemplate& schema) {
  using Answer = Result<Lut3dBakeDescription>;
  if ((schema.id != "curve.bake_lut3d.report" &&
       schema.id != "curve.bake_lut3d.table") ||
      schema.version != (schema.id == "curve.bake_lut3d.table" ? 2U : 1U) ||
      schema.metadata.size() != 1 ||
      schema.metadata[0].key != "curve.bake_lut3d.measured" ||
      schema.metadata[0].version != 1)
    return Answer(core_internal::invalid_schema_domain(
        "invalid LUT3D bake report schema"));
  Reader reader{schema.metadata[0].payload};
  Lut3dBakeDescription spec;
  for (auto& n : spec.shape)
    n = reader.word();
  const auto method = reader.word();
  if (method < 1 || method > 2)
    return Answer(core_internal::invalid_schema_domain(
        "invalid LUT3D bake report schema"));
  spec.interpolation = static_cast<Lut3dInterpolation>(method);
  const auto a = reader.word(), r = reader.word();
  std::memcpy(&spec.atol, &a, 8);
  std::memcpy(&spec.rtol, &r, 8);
  const auto table = reader.word(), source = reader.word();
  if ((table != 3 && table != 4) || (source != 3 && source != 4))
    return Answer(core_internal::invalid_schema_domain(
        "invalid LUT3D bake report schema"));
  spec.table_dtype = static_cast<ElementType>(table);
  spec.source_dtype = static_cast<ElementType>(source);
  spec.extra_points = reader.word();
  auto input = color_array_from_parameter(reader.text()),
       output = color_array_from_parameter(reader.text());
  spec.recipe_identity = reader.text();
  if (!reader.valid || reader.offset != reader.bytes.size() || !input.ok() ||
      !output.ok())
    return Answer(core_internal::invalid_schema_domain(
        "invalid LUT3D bake report schema"));
  spec.input_description = input.take_value();
  spec.output_description = output.take_value();
  auto expected = schema.id == "curve.bake_lut3d.report"
                      ? lut3d_bake_schema(spec)
                      : lut3d_bake_table_schema(spec);
  if (!expected.ok() || !schema.same_schema(expected.value()))
    return Answer(core_internal::invalid_schema_domain(
        "invalid LUT3D bake report schema"));
  return Answer(std::move(spec));
}
Result<Lut3dBakeReport> read_lut3d_bake_report(
    const ResultRef& result, std::uint64_t maximum_window,
    const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) {
  using Answer = Result<Lut3dBakeReport>;
  if (!result.valid() || result.schema().id != "curve.bake_lut3d.report")
    return Answer(core_internal::invalid_schema_domain(
        "invalid LUT3D bake report schema"));
  auto description = lut3d_bake_description(result.schema());
  if (!description.ok())
    return Answer(description.status());
  auto descriptor = result.descriptor();
  if (!descriptor.ok())
    return Answer(descriptor.status());
  const auto work = [&](std::uint64_t count) {
    if (cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return consume_work ? consume_work(count) : Status::success();
  };
  auto charged = work(result.schema().canonical_size() + 289);
  if (!charged.ok())
    return Answer(charged);
  Lut3dBakeReport report;
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
    auto plan = result.prepare_read(descriptor.value(), field, 0,
                                    (field == 1 || field == 5) ? 3 : 1);
    if (!plan.ok())
      return Answer(plan.status());
    auto window = plan.value().load(maximum_window, cancellation);
    if (!window.ok())
      return Answer(window.status());
    if (window.value()->bytes().size() != sizes[field])
      return Answer(core_internal::invalid_schema_domain(
          "invalid LUT3D bake report schema"));
    std::memcpy(fields[field], window.value()->bytes().data(), sizes[field]);
  }
  auto validated = input_internal::validate_lut3d_bake_report_values(
      description.value(), &report, passed, work);
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
}  // namespace ps
