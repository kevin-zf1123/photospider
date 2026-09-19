#include "photospider/data/lut3d_bake.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/uniform_axis.hpp"

namespace ps {
namespace {
Status invalid(const char* message = "invalid LUT3D bake report schema") {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
std::uint64_t raw(double value) {
  std::uint64_t bits;
  std::memcpy(&bits, &value, 8);
  return bits;
}
bool finite(double value) {
  return (raw(value) & UINT64_C(0x7ff0000000000000)) !=
         UINT64_C(0x7ff0000000000000);
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
      return invalid("LUT3D bake shape must be 2..256");
  if ((spec.interpolation != Lut3dInterpolation::Trilinear &&
       spec.interpolation != Lut3dInterpolation::Tetrahedral) ||
      !finite(spec.atol) || !finite(spec.rtol) ||
      ((raw(spec.atol) >> 63) && (raw(spec.atol) << 1)) ||
      ((raw(spec.rtol) >> 63) && (raw(spec.rtol) << 1)) ||
      (spec.table_dtype != ElementType::Float32 &&
       spec.table_dtype != ElementType::Float64) ||
      (spec.source_dtype != ElementType::Float32 &&
       spec.source_dtype != ElementType::Float64) ||
      spec.extra_points > 1048576 || spec.recipe_identity.size() != 64)
    return invalid();
  for (char c : spec.recipe_identity)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return invalid();
  for (const auto* description :
       {&spec.input_description, &spec.output_description}) {
    if (description->model != spec.input_description.model ||
        description->model == ColorModel::Cmyk ||
        description->association != ColorAssociation::None ||
        description->source_layout != ColorSourceLayout::Interleaved)
      return invalid("LUT3D bake requires same-model three-component colors");
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
  append(&metadata, raw(spec.atol));
  append(&metadata, raw(spec.rtol));
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
  std::uint64_t count = 1;
  for (auto n : spec.shape) {
    schema.domain.push_back({ResultExtentKind::Fixed, n});
    count *= n;
  }
  schema.fields = {
      {"colors", spec.table_dtype, {ResultExtentKind::Fixed, count}, {3}}};
  return Result<SchemaTemplate>(std::move(schema));
}
Result<Lut3dBakeDescription> lut3d_bake_description(
    const SchemaTemplate& schema) {
  using Answer = Result<Lut3dBakeDescription>;
  if ((schema.id != "curve.bake_lut3d.report" &&
       schema.id != "curve.bake_lut3d.table") ||
      schema.version != 1 || schema.metadata.size() != 1 ||
      schema.metadata[0].key != "curve.bake_lut3d.measured" ||
      schema.metadata[0].version != 1)
    return Answer(invalid());
  Reader reader{schema.metadata[0].payload};
  Lut3dBakeDescription spec;
  for (auto& n : spec.shape)
    n = reader.word();
  const auto method = reader.word();
  if (method < 1 || method > 2)
    return Answer(invalid());
  spec.interpolation = static_cast<Lut3dInterpolation>(method);
  const auto a = reader.word(), r = reader.word();
  std::memcpy(&spec.atol, &a, 8);
  std::memcpy(&spec.rtol, &r, 8);
  const auto table = reader.word(), source = reader.word();
  if ((table != 3 && table != 4) || (source != 3 && source != 4))
    return Answer(invalid());
  spec.table_dtype = static_cast<ElementType>(table);
  spec.source_dtype = static_cast<ElementType>(source);
  spec.extra_points = reader.word();
  auto input = color_array_from_parameter(reader.text()),
       output = color_array_from_parameter(reader.text());
  spec.recipe_identity = reader.text();
  if (!reader.valid || reader.offset != reader.bytes.size() || !input.ok() ||
      !output.ok())
    return Answer(invalid());
  spec.input_description = input.take_value();
  spec.output_description = output.take_value();
  auto expected = schema.id == "curve.bake_lut3d.report"
                      ? lut3d_bake_schema(spec)
                      : lut3d_bake_table_schema(spec);
  if (!expected.ok() || !schema.same_schema(expected.value()))
    return Answer(invalid());
  return Answer(std::move(spec));
}
Result<Lut3dBakeReport> read_lut3d_bake_report(
    const ResultRef& result, std::uint64_t maximum_window,
    const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) {
  using Answer = Result<Lut3dBakeReport>;
  if (!result.valid() || result.schema().id != "curve.bake_lut3d.report")
    return Answer(invalid());
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
      return Answer(invalid());
    std::memcpy(fields[field], window.value()->bytes().data(), sizes[field]);
  }
  auto expected = description.value().extra_points;
  std::uint64_t centers = 1;
  for (auto n : description.value().shape)
    centers *= n - 1;
  expected += centers;
  const auto malformed = [&] {
    return Answer(Status{ErrorCode::OperationFailed,
                         "invalid measured LUT3D report values",
                         FailureReason::InvalidDomain,
                         {FailureOrigin::Domain, FailureScope::Group}});
  };
  if (passed > 1 ||
      report.validation_count != static_cast<std::int64_t>(expected) ||
      report.failed_count < 0 ||
      report.failed_count > report.validation_count ||
      (passed != (report.failed_count == 0)))
    return malformed();
  report.passed = passed != 0;
  for (unsigned i = 0; i < 3; ++i) {
    plugin_internal::numeric_ops::UniformAxis grid(
        plugin_internal::numeric_ops::SequenceProfile::Strict);
    auto valid =
        grid.validate({raw(report.axis[3 * i]), raw(report.axis[3 * i + 1]),
                       raw(report.axis[3 * i + 2])},
                      description.value().shape[i], work);
    if (!valid.ok()) {
      if (valid.detail.origin == FailureOrigin::Domain)
        valid.detail.scope = FailureScope::Group;
      return Answer(valid);
    }
  }
  const auto valid_point = [&](const double* point) {
    using plugin_internal::numeric_ops::BinaryParts;
    for (unsigned c = 0; c < 3; ++c) {
      if (!finite(point[c]))
        return false;
      const auto first =
          BinaryParts::decode(raw(report.axis[3 * c]), false).order_key();
      const auto last =
          BinaryParts::decode(raw(report.axis[3 * c + 1]), false).order_key();
      const auto key = BinaryParts::decode(raw(point[c]), false).order_key();
      if (key < std::min(first, last) || key > std::max(first, last))
        return false;
    }
    return true;
  };
  const auto valid_chroma = [](const ColorArrayDescriptor& color,
                               double chroma) {
    return (color.model != ColorModel::Cielch &&
            color.model != ColorModel::Oklch) ||
           !(raw(chroma) >> 63) || !(raw(chroma) << 1);
  };
  if (!valid_chroma(description.value().input_description, report.axis[3]) ||
      !valid_chroma(description.value().input_description, report.axis[4]))
    return malformed();
  for (unsigned c = 0; c < 3; ++c)
    if (!valid_point(report.max_error_point.data() + 3 * c))
      return malformed();
  if (!report.passed && (!valid_point(report.first_failure_input.data()) ||
                         !valid_chroma(description.value().output_description,
                                       report.first_failure_reference[1]) ||
                         !valid_chroma(description.value().output_description,
                                       report.first_failure_lut[1])))
    return malformed();

  for (const auto* values : {&report.axis, &report.max_error_point})
    for (auto value : *values)
      if (!finite(value))
        return malformed();
  for (unsigned i = 0; i < 3; ++i) {
    const auto error = raw(report.max_abs_error[i]);
    if ((error >> 63) || error > UINT64_C(0x7ff0000000000000) ||
        report.max_error_index[i] < 0 ||
        report.max_error_index[i] >= report.validation_count)
      return malformed();
    for (auto value :
         {report.first_failure_input[i], report.first_failure_reference[i],
          report.first_failure_lut[i]})
      if (!finite(value) || (report.passed && raw(value)))
        return malformed();
  }
  if (report.passed ? report.first_failure_index != -1
                    : (report.first_failure_index < 0 ||
                       report.first_failure_index >= report.validation_count))
    return malformed();
  return Answer(report);
}
}  // namespace ps
