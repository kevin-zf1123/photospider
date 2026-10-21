#include "photospider/ops/numeric/lut3d_bake.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/numeric_bits.hpp"
#include "photospider/core/content_digest.hpp"
#include "photospider/core/status_helpers.hpp"

namespace ps {
namespace {

SchemaTemplate report_schema() {
  SchemaTemplate schema;
  schema.id = "curve.bake_lut3d.report";
  schema.version = 4;
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
  return schema;
}
Status validate(const Lut3dBakeDescription& spec) {
  for (auto n : spec.shape)
    if (n < 2 || n > 256)
      return core_internal::invalid_schema_domain(
          "LUT3D bake shape must be 2..256");
  if ((spec.interpolation != Lut3dInterpolation::Trilinear &&
       spec.interpolation != Lut3dInterpolation::Tetrahedral) ||
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
  std::string model;
  for (const auto* description :
       {&spec.input_description, &spec.output_description}) {
    auto group = numeric::resolve_color_tuple(*description);
    if (!group.ok())
      return group.status();
    if (model.empty())
      model = group.value().interpretation.model;
    if (group.value().interpretation.model != model ||
        group.value().components.size() != 3 || group.value().alpha)
      return core_internal::invalid_schema_domain(
          "LUT3D bake requires same-model three-component colors");
    for (const auto& component : group.value().components)
      if (component.unit == "rational_pi")
        return core_internal::invalid_schema_domain(
            "LUT3D bake requires floating hue coordinates");
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
  hash.text("photospider.bake-configuration.v2");
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
    auto bytes = encode_tensor_description(*description);
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
  return Result<SchemaTemplate>(report_schema());
}

Result<SchemaTemplate> lut3d_bake_table_schema(
    const Lut3dBakeDescription& spec) {
  auto made = lut3d_bake_schema(spec);
  if (!made.ok())
    return made;
  auto schema = made.take_value();
  schema.id = "curve.bake_lut3d.table";
  schema.version = 4;
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
Result<Lut3dBakeReportLayout> prepare_lut3d_bake_report_layout(
    const SchemaTemplate& schema) {
  using Answer = Result<Lut3dBakeReportLayout>;
  if (!schema.same_schema(report_schema()))
    return Answer(
        Status{ErrorCode::TypeMismatch, "LUT3D report structure mismatch"});
  Lut3dBakeReportLayout layout;
  for (unsigned i = 0; i < layout.regions.size(); ++i)
    layout.regions[i] = Region::whole(schema.tensors[i].sample_shape());
  return Answer(std::move(layout));
}
Result<Lut3dBakeReport> read_lut3d_bake_report(
    const ResultRef& result, const Lut3dBakeReportLayout& layout,
    std::uint64_t maximum_window, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) {
  using Answer = Result<Lut3dBakeReport>;
  if (cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  auto descriptor = result.descriptor();
  if (!descriptor.ok())
    return Answer(descriptor.status());
  if (consume_work) {
    auto charged = consume_work(361);
    if (!charged.ok())
      return Answer(charged);
  }
  Lut3dBakeReport report;
  std::uint8_t passed = 0;
  std::array<std::uint8_t, 8> measured{};
  const std::array<void*, 13> fields{&passed,
                                     report.axis.data(),
                                     &report.validation_count,
                                     &report.failed_count,
                                     report.max_abs_error.data(),
                                     report.max_error_point.data(),
                                     report.max_error_index.data(),
                                     &report.first_failure_index,
                                     report.first_failure_input.data(),
                                     report.first_failure_reference.data(),
                                     report.first_failure_lut.data(),
                                     report.configuration_id.data(),
                                     measured.data()};
  const std::array<std::uint64_t, 13> sizes{1, 72, 8,  8,  24, 72, 24,
                                            8, 24, 24, 24, 64, 8};
  for (unsigned field = 0; field < fields.size(); ++field) {
    if (sizes[field] > maximum_window)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    auto window = result.acquire_tensor(descriptor.value(), field,
                                        layout.regions[field], cancellation);
    if (!window.ok())
      return Answer(window.status());
    auto copied = window.value().copy_to(
        static_cast<std::uint8_t*>(fields[field]), sizes[field]);
    if (!copied.ok())
      return Answer(copied);
  }
  report.passed = passed != 0;
  for (unsigned i = 0; i < measured.size(); ++i)
    report.measured_table_id |= std::uint64_t{measured[i]} << (8 * i);
  return Answer(std::move(report));
}
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
  auto a = tensor_description_from_parameter(
           std::get<std::string>(parameters.at("input_color_description"))),
       b = tensor_description_from_parameter(
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
