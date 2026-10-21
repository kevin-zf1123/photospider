#include "photospider/ops/format/rgb_basis.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/rgb_basis_math.hpp"
#include "common/domain/rgb_geometry_codec.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"
#include "photospider/ops/registry.hpp"
#include "photospider/plugin/tile_geometry.hpp"

namespace ps::plugin_internal::basis_ops {
namespace {
using Parameters = std::map<std::string, ParameterValue>;
using data_internal::format_numeric::ExactWorkFailure;
std::string text(const Parameters& params, const char* key,
                 const char* default_value = "") {
  const auto it = params.find(key);
  return it == params.end() ? default_value : std::get<std::string>(it->second);
}
unsigned method_index(const std::string& method) {
  if (method == "xyz_scaling")
    return 0;
  if (method == "bradford")
    return 1;
  if (method == "cat02")
    return 2;
  if (method == "cat16")
    return 3;
  throw CheckedFailure{
      invalid("method must be xyz_scaling, bradford, cat02 or cat16")};
}
std::array<std::uint64_t, 3> components(const std::string& value) {
  std::array<std::uint64_t, 3> out{};
  std::size_t at = 0;
  for (unsigned j = 0; j < 3; ++j) {
    const auto end = value.find(',', at);
    require((j == 2) == (end == std::string::npos),
            "components require exactly three indices");
    const auto token =
        value.substr(at, end == std::string::npos ? end : end - at);
    const auto parsed =
        std::from_chars(token.data(), token.data() + token.size(), out[j]);
    require(!token.empty() && parsed.ec == std::errc{} &&
                parsed.ptr == token.data() + token.size() &&
                token == std::to_string(out[j]),
            "noncanonical component index");
    at = end == std::string::npos ? value.size() : end + 1;
  }
  require(out[0] != out[1] && out[0] != out[2] && out[1] != out[2],
          "repeated component index");
  return out;
}
TensorInterpretation global_interpretation(const TensorDescription& d) {
  TensorInterpretation out;
  out.model = d.model;
  out.primaries = d.primaries;
  out.transfer = d.transfer;
  out.reference = d.reference;
  out.association = d.association;
  out.white = d.white;
  out.primaries_xy = d.primaries_xy;
  out.profile = d.profile;
  out.convention = d.convention;
  out.configured = d.configured;
  out.analytic_binding = d.analytic_binding;
  return out;
}
bool described(const TensorInterpretation& value) {
  return !value.model.empty() || !value.primaries.empty() ||
         !value.transfer.empty() || !value.reference.empty() ||
         !value.association.empty() || value.white || value.primaries_xy ||
         value.profile || value.configured || value.analytic_binding;
}
void overlay(TensorInterpretation* target, const TensorInterpretation& source) {
  // A default-constructed, absent global/channel record makes no convention
  // assertion. Explicit records must agree, including opaque resource identity.
  const bool had_description = described(*target);
  if (described(source)) {
    typed(!had_description || target->convention == source.convention,
          "conflicting coordinate conventions");
    if (!had_description)
      target->convention = source.convention;
  }
  typed(!target->profile || !source.profile ||
            *target->profile == *source.profile,
        "conflicting profile identity");
  typed(!target->configured || !source.configured ||
            *target->configured == *source.configured,
        "conflicting configuration identity");
  typed(!target->analytic_binding || !source.analytic_binding ||
            *target->analytic_binding == *source.analytic_binding,
        "conflicting analytic binding");
  for (auto pair :
       {std::make_pair(&target->model, &source.model),
        std::make_pair(&target->primaries, &source.primaries),
        std::make_pair(&target->transfer, &source.transfer),
        std::make_pair(&target->reference, &source.reference),
        std::make_pair(&target->association, &source.association)}) {
    typed(pair.first->empty() || pair.second->empty() ||
              *pair.first == *pair.second,
          "conflicting effective interpretation");
    if (pair.first->empty())
      *pair.first = *pair.second;
  }
  typed(!target->white || !source.white || *target->white == *source.white,
        "conflicting effective white");
  typed(!target->primaries_xy || !source.primaries_xy ||
            *target->primaries_xy == *source.primaries_xy,
        "conflicting effective primaries");
  if (!target->white)
    target->white = source.white;
  if (!target->primaries_xy)
    target->primaries_xy = source.primaries_xy;
  if (!target->profile)
    target->profile = source.profile;
  if (!target->configured)
    target->configured = source.configured;
  if (!target->analytic_binding)
    target->analytic_binding = source.analytic_binding;
}
Rational endpoint(const TensorEndpoint& value) {
  if (const auto* v = std::get_if<std::int64_t>(&value))
    return Rational::integer(*v);
  if (const auto* v = std::get_if<double>(&value))
    return number(*v);
  const auto& v = std::get<TensorRationalEndpoint>(value);
  Rational out;
  out.n.words.assign(v.numerator.begin(), v.numerator.end());
  out.d.words.assign(v.denominator.begin(), v.denominator.end());
  out.n.trim();
  out.d.trim();
  out.negative = v.negative;
  return out;
}
void native_encoding(const std::optional<TensorEncoding>& value) {
  if (!value)
    return;
  typed(
      endpoint(value->stored[0]).compare(endpoint(value->decoded[0])) == 0 &&
          endpoint(value->stored[1]).compare(endpoint(value->decoded[1])) == 0,
      "semantic samples must use native numeric encoding; decode explicitly");
}
struct Preparation final {
  std::optional<TensorDescription> output_description;
  std::shared_ptr<const std::array<ExactRow, 3>> rows;
  std::shared_ptr<const SchemaTemplate> output_schema;
  std::array<std::uint64_t, 3> selected{};
  unsigned axis = 0;
  std::string layout;
  struct Span final {
    Region region;
    unsigned terms;
  };
  std::vector<Span> spans;
  bool narrow = false, is_identity = false;
  SequenceProfile profile = SequenceProfile::Strict;
  std::array<double, 2> source_white{};
  Preparation(const Matrix& matrix)
      : rows(std::make_shared<const std::array<ExactRow, 3>>(
            std::array<ExactRow, 3>{{ExactRow(matrix, 0), ExactRow(matrix, 1),
                                     ExactRow(matrix, 2)}})) {
    is_identity = identity(matrix);
  }
  int row(std::uint64_t channel) const {
    for (unsigned i = 0; i < 3; ++i)
      if (channel == selected[i])
        return static_cast<int>(i);
    return -1;
  }
};

OperationPreparation prepare_impl(const std::vector<OperationMetadata>& inputs,
                                  const Parameters& params, unsigned member,
                                  SequenceProfile profile) {
  plugin_internal::FloatingEnvironment environment;
  typed(environment.active(), "floating environment unavailable");
  checked(numeric_ops::sequence_profile_available(profile));
  require(inputs.size() == 1, "one input required");
  checked(tensor_ops::check_tensor(inputs[0]));
  const auto& input = inputs[0].result_schema->tensors[0];
  const auto& shape = input.descriptor.shape;
  const auto full_shape = input.sample_shape();
  typed(!shape.empty() && full_shape.size() <= 8 &&
            (input.descriptor.element_type == ElementType::Float32 ||
             input.descriptor.element_type == ElementType::Float64),
        "requires Float32/64 rank 1..8");
  std::uint64_t count = 1;
  for (const auto extent : full_shape) {
    typed(extent && extent <= (UINT64_C(1) << 40) / count,
          "tensor exceeds 2^40 samples");
    count *= extent;
  }
  const bool semantic = params.count("input_description") != 0;
  const auto layout = text(params, "layout", "auto");
  require(layout == "auto" || layout == "view" || layout == "materialize",
          "unknown layout");
  std::optional<TensorDescription> desc;
  if (semantic)
    desc = checked(
        tensor_description_from_parameter(text(params, "input_description")));
  if (desc)
    checked(validate_tensor_description(*desc, input.descriptor));
  std::array<std::uint64_t, 3> selected{};
  unsigned axis = 0, group_index = 0;
  TensorInterpretation effective;
  std::array<std::string, 3> selected_units;
  const std::array<std::string, 3> rgb_roles{{"red", "green", "blue"}},
      xyz_roles{{"x", "y", "z"}};
  const auto& source_roles = member == 0 ? rgb_roles : xyz_roles;
  if (!semantic) {
    require(!params.count("group") && params.count("components"),
            "raw needs components and forbids group");
    selected = components(text(params, "components"));
    if (params.count("axis")) {
      const auto a = std::get<std::int64_t>(params.at("axis"));
      require(a >= 0 && static_cast<std::uint64_t>(a) < shape.size(),
              "axis outside rank");
      axis = static_cast<unsigned>(a);
    } else {
      require(desc && desc->channel_axis,
              "raw needs explicit or described axis");
      axis = *desc->channel_axis;
    }
  } else {
    require(!params.count("components") && !params.count("axis"),
            "semantic group forbids raw selectors");
    require(desc && desc->channel_axis && params.count("group"),
            "semantic input needs v4 channel axis and explicit group");
    axis = *desc->channel_axis;
    bool found = false;
    for (unsigned i = 0; i < desc->groups.size(); ++i)
      if (desc->groups[i].name == text(params, "group")) {
        require(!found, "ambiguous group");
        found = true;
        group_index = i;
      }
    require(found, "selected group is absent");
    auto& group = desc->groups[group_index];
    effective = group.interpretation;
    overlay(&effective, global_interpretation(*desc));
    typed(effective.model == (member == 0 ? "rgb" : "xyz") &&
              group.indices.size() == 3,
          "selected group has wrong model");
    native_encoding(desc->encoding);
    // Resolve every selected channel before consuming a binding. In particular,
    // never index an inherited binding before checking its complete role order.
    for (unsigned j = 0; j < 3; ++j) {
      const auto it = std::find_if(
          group.components.begin(), group.components.end(),
          [&](const auto& c) { return c.role == source_roles[j]; });
      require(it != group.components.end(), "incomplete color roles");
      const auto k = static_cast<unsigned>(it - group.components.begin());
      selected[j] = group.indices[k];
      native_encoding(it->encoding);
      selected_units[j] = it->unit;
      if (selected[j] < desc->channels.size()) {
        const auto& channel = desc->channels[selected[j]];
        native_encoding(channel.encoding);
        if (selected_units[j].empty())
          selected_units[j] = channel.unit;
        if (channel.interpretation)
          overlay(&effective, *channel.interpretation);
      }
    }
    if (effective.profile || effective.configured) {
      typed(effective.analytic_binding.has_value(),
            "profile/configured source needs explicit analytic binding");
      const auto& b = *effective.analytic_binding;
      typed(b.model == effective.model && b.roles.size() == 3 &&
                b.units.size() == 3,
            "analytic binding model/order mismatch");
      for (unsigned k = 0; k < 3; ++k)
        typed(b.roles[k] == group.components[k].role,
              "analytic binding role order mismatch");
      TensorInterpretation analytic;
      analytic.model = b.model;
      analytic.primaries = b.primaries;
      analytic.transfer = b.transfer;
      analytic.reference = b.reference;
      analytic.white = b.white;
      analytic.primaries_xy = b.primaries_xy;
      analytic.convention = b.convention;
      // The explicit caller binding, not the ICC/OCIO label, asserts analytic
      // native coordinates. No profile engine, PCS white guess or transform
      // runs.
      effective.convention = b.convention;
      overlay(&effective, analytic);
    }
    typed(effective.convention == "relative-v1",
          "unsupported coordinate convention");
    std::string unit;
    for (unsigned j = 0; j < 3; ++j) {
      auto& current = selected_units[j];
      if (effective.analytic_binding) {
        const auto k = static_cast<unsigned>(
            std::find(group.indices.begin(), group.indices.end(), selected[j]) -
            group.indices.begin());
        const auto& declared = effective.analytic_binding->units[k];
        typed(current.empty() || current == declared,
              "analytic binding unit mismatch");
        if (current.empty())
          current = declared;
      }
      typed(current == "1" || current == "cd/m2",
            "native component unit must be 1 or cd/m2");
      typed(unit.empty() || unit == current, "component units disagree");
      unit = current;
    }
    const auto& ref = effective.reference;
    typed(ref == "scene_relative" || ref == "display_relative" ||
              ref == "absolute_display_cd_m2" || ref == "scene" ||
              ref == "display",
          "explicit scene/display reference required");
    typed(unit == "1" ? ref != "absolute_display_cd_m2"
                      : (ref == "absolute_display_cd_m2" || ref == "display"),
          "unit/reference combination is incoherent");
    typed(
        member == 0
            ? effective.transfer == "linear"
            : (effective.transfer.empty() || effective.transfer == "linear") &&
                  effective.primaries.empty() && !effective.primaries_xy,
        "requires native linear RGB or coherent XYZ");
    for (unsigned i = 0; i < desc->groups.size(); ++i)
      if (i != group_index)
        for (auto c : desc->groups[i].indices)
          require(
              std::find(selected.begin(), selected.end(), c) == selected.end(),
              "selected group overlaps another group");
  }
  require(axis < shape.size(), "axis outside rank");
  for (auto c : selected)
    require(c < shape[axis], "component outside channel extent");
  if (input.layout.spatial)
    typed(input.layout.channel_axis && *input.layout.channel_axis == axis,
          "selected axis must be structural image channel axis");
  Matrix matrix;
  std::optional<ResolvedBasis> source, target;
  std::array<double, 2> source_white{}, target_white{};
  if (member == 0) {
    if (semantic) {
      source = resolve(
          {effective.primaries, effective.primaries_xy, effective.white});
      if (params.count("source_basis")) {
        const auto assertion = parse_basis(text(params, "source_basis"));
        require(assertion.xy == source->xy && assertion.white == source->white,
                "source basis assertion disagrees");
      }
    } else {
      require(params.count("source_basis"), "raw A requires source_basis");
      source = parse_basis(text(params, "source_basis"));
    }
    source_white = source->white;
    matrix = rgb_matrix(source->xy, source->white);
    target_white = source_white;
  } else if (member == 1) {
    require(params.count("target_basis"), "B requires target_basis");
    target = parse_basis(text(params, "target_basis"));
    target_white = target->white;
    if (params.count("target_white"))
      require(parse_white(text(params, "target_white")) == target_white,
              "duplicate target white disagrees");
    if (semantic) {
      typed(effective.white.has_value(), "XYZ source white is required");
      source_white = *effective.white;
      (void)white_xyz(source_white);
      if (params.count("source_white"))
        require(parse_white(text(params, "source_white")) == source_white,
                "source white assertion disagrees");
      const auto policy = text(params, "white_handling", "require_match");
      require(policy == "require_match" || policy == "preserve_xyz",
              "B never adapts white");
      typed(policy == "preserve_xyz" || source_white == target_white,
            "source/target whites differ under require_match");
    } else {
      require(!params.count("white_handling") && !params.count("source_white"),
              "raw B forbids semantic white assertions/policy");
    }
    matrix = inverse(rgb_matrix(target->xy, target->white));
  } else {
    require(params.count("target_white") && params.count("method"),
            "C requires target_white and method");
    target_white = parse_white(text(params, "target_white"));
    if (semantic) {
      typed(effective.white.has_value(), "XYZ source white is required");
      source_white = *effective.white;
      if (params.count("source_white"))
        require(parse_white(text(params, "source_white")) == source_white,
                "source white assertion disagrees");
    } else {
      require(params.count("source_white"), "raw C requires source_white");
      source_white = parse_white(text(params, "source_white"));
    }
    matrix = adaptation(source_white, target_white,
                        method_index(text(params, "method")));
  }
  auto state = std::make_shared<Preparation>(matrix);
  state->axis = static_cast<unsigned>(input.batch_axes.size()) + axis;
  state->layout = layout;
  state->selected = selected;
  state->narrow = input.descriptor.element_type == ElementType::Float32;
  state->profile = profile;
  state->source_white = source_white;
  require(layout != "view" || state->is_identity,
          "ViewUnavailable: nonidentity transform");
  OperationOutputSpecialization output;
  output.metadata = inputs[0];
  auto schema = *inputs[0].result_schema;
  auto& output_tensor = schema.tensors[0];
  output_tensor.atomic_trailing_axes = 0;
  if (semantic) {
    auto& group = desc->groups[group_index];
    TensorInterpretation updated = effective;
    updated.model = member == 1 ? "rgb" : "xyz";
    updated.white = target_white;
    updated.profile.reset();
    updated.configured.reset();
    updated.analytic_binding.reset();
    updated.primaries = target ? target->preset : "";
    updated.primaries_xy =
        target ? std::optional<std::array<double, 6>>(target->xy)
               : std::nullopt;
    updated.transfer = member == 1 ? "linear" : "";
    const auto& target_roles = member == 1 ? rgb_roles : xyz_roles;
    const auto global = global_interpretation(*desc);
    // Preserve inherited descriptions of unrelated groups before clearing
    // global RGB-only fields that can no longer describe a mixed-model tensor.
    for (auto& other : desc->groups)
      overlay(&other.interpretation, global);
    group.interpretation = updated;
    for (unsigned j = 0; j < 3; ++j) {
      const auto it =
          std::find(group.indices.begin(), group.indices.end(), selected[j]);
      const auto k = static_cast<unsigned>(it - group.indices.begin());
      group.components[k].role = target_roles[j];
      // Units previously supplied solely by a dropped binding must survive.
      group.components[k].unit = selected_units[j];
      if (selected[j] < desc->channels.size()) {
        auto& channel = desc->channels[selected[j]];
        channel.role = target_roles[j];
        channel.unit = selected_units[j];
        if (channel.interpretation)
          channel.interpretation = updated;
      }
    }
    if (desc->groups.size() == 1) {
      desc->model = updated.model;
      desc->primaries = updated.primaries;
      desc->transfer = updated.transfer;
      desc->reference = updated.reference;
      desc->association = updated.association;
      desc->white = updated.white;
      desc->primaries_xy = updated.primaries_xy;
    } else {
      desc->model.clear();
      desc->primaries.clear();
      desc->transfer.clear();
      desc->reference.clear();
      desc->association.clear();
      desc->white.reset();
      desc->primaries_xy.reset();
    }
    desc->profile.reset();
    desc->configured.reset();
    desc->analytic_binding.reset();
    desc->convention = "relative-v1";
    checked(validate_tensor_description(*desc, input.descriptor));
    state->output_description = *desc;
  }
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  state->output_schema = output.metadata.result_schema;
  std::vector<std::uint64_t> boundaries{0, shape[axis]};
  for (auto c : selected) {
    boundaries.push_back(c);
    boundaries.push_back(c + 1);
  }
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                   boundaries.end());
  for (unsigned i = 1; i < boundaries.size(); ++i) {
    auto dims = Region::whole(full_shape).dimensions();
    dims[state->axis] = {boundaries[i - 1], boundaries[i] - boundaries[i - 1]};
    const bool color = state->row(boundaries[i - 1]) >= 0;
    state->spans.push_back(
        {Region(std::move(dims)), color && !state->is_identity ? 3U : 1U});
  }
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::move(state);
  return result;
}
Result<OperationPreparation> prepare(
    const std::vector<OperationMetadata>& inputs, const Parameters& params,
    unsigned member, SequenceProfile profile) {
  try {
    return Result<OperationPreparation>(
        prepare_impl(inputs, params, member, profile));
  } catch (const CheckedFailure& f) {
    return Result<OperationPreparation>(f.status);
  } catch (const GeometryError&) {
    return Result<OperationPreparation>(invalid(
        "invalid white, singular basis, or nonpositive adaptation response"));
  } catch (const ExactWorkFailure& f) {
    return Result<OperationPreparation>(f.status);
  }
}
struct Workspace final {
  CandidateBlock candidate;
  std::array<std::array<std::uint64_t, block_size>, 3> raw{};
  std::array<std::uint64_t, block_size> result{}, packed_offsets{};
  std::array<bool, block_size> finite{};
  RowWorkspace exact;
};
bool calculates(const Preparation& state, const Region& output) {
  if (state.is_identity || state.layout == "view")
    return false;
  const auto c = output.dimensions()[state.axis];
  for (auto selected : state.selected)
    if (selected >= c.offset && selected - c.offset < c.extent)
      return true;
  return false;
}
std::uint64_t scratch_bytes(const Preparation& state, const Region& output) {
  return calculates(state, output) ? sizeof(Workspace) : 0;
}
Result<std::set<TileIndex>> required_tiles(const Preparation& state,
                                           const Region& output,
                                           const TileGrid& grid,
                                           const ResourceBudget& budget,
                                           std::uint64_t maximum_tiles) {
  using Answer = Result<std::set<TileIndex>>;
  std::set<TileIndex> result;
  if (state.layout == "view")
    return Answer(std::move(result));
  for (const auto& span : state.spans) {
    auto charged = budget.consume({1});
    if (!charged.ok())
      return Answer(charged);
    auto dimensions = output.dimensions();
    auto& c = dimensions[state.axis];
    const auto s = span.region.dimensions()[state.axis];
    const auto first = std::max(c.offset, s.offset);
    const auto end = std::min(c.offset + c.extent, s.offset + s.extent);
    if (first >= end)
      continue;
    c = {first, end - first};
    for (unsigned term = 0; term < span.terms; ++term) {
      auto source = dimensions;
      if (span.terms == 3)
        source[state.axis] = {state.selected[term], 1};
      Region region(std::move(source));
      // Bound the rectangle before enumerating its tiles, including imported
      // tasks with fewer read slots than the source closure requires.
      std::uint64_t count = 1;
      for (unsigned axis = 0; axis < grid.rank(); ++axis) {
        const auto d = region.dimensions()[axis];
        std::uint64_t extent = 1;
        if (grid.batch_axis_mask() & (1U << axis))
          extent = d.extent;
        else if (axis == grid.height_axis() || axis == grid.width_axis())
          extent = (d.offset + d.extent - 1) / grid.tile_extent() -
                   d.offset / grid.tile_extent() + 1;
        if (extent > maximum_tiles / count)
          return Answer({ErrorCode::ResourceExhausted, "RGB read tile limit"});
        count *= extent;
      }
      auto tiles = sdk::tiles_for_region(budget, grid, region);
      if (!tiles.ok())
        return Answer(tiles.status());
      for (auto index : tiles.value()) {
        auto status = budget.consume({1});
        if (!status.ok())
          return Answer(status);
        result.insert(index);
        if (result.size() > maximum_tiles)
          return Answer({ErrorCode::ResourceExhausted, "RGB read union limit"});
      }
    }
  }
  return Answer(std::move(result));
}
constexpr const char* basis_contract = "rgb-basis-fixed-v1";
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const Preparation*>(query.static_state);
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!input.ok())
    return Answer(input.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      auto task =
          builder.add_task("rgb-basis", basis_contract, {},
                           {scratch_bytes(state, region.value()), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      auto reads = required_tiles(state, region.value(), input.value(), budget,
                                  query.limits.maximum_records);
      if (!reads.ok())
        return Answer(reads.status());
      if (reads.value().empty()) {
        auto status = builder.read_input_schema(task.value(), 0);
        if (!status.ok())
          return Answer(status);
      }
      for (auto index : reads.value()) {
        auto status = builder.read_input(task.value(), 0, 0, index);
        if (!status.ok())
          return Answer(status);
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
struct PreparedBasis final {
  bool narrow, is_identity, view, calculates;
  unsigned axis;
  SequenceProfile profile;
  std::array<std::uint64_t, 3> selected;
  std::shared_ptr<const std::array<ExactRow, 3>> rows;
  fixed_tensor::PointInputs<1> inputs;
  int row(std::uint64_t channel) const noexcept {
    for (unsigned i = 0; i < 3; ++i)
      if (selected[i] == channel)
        return static_cast<int>(i);
    return -1;
  }
};
Status evaluate(Workspace* work, unsigned count, unsigned row,
                const PreparedBasis& state,
                const core_internal::WorkConsumer& consume) {
  const auto& coefficients = (*state.rows)[row];
  bool use_candidate = false;
#ifndef PHOTOSPIDER_FMT10_EXACT_ONLY
  use_candidate = coefficients.candidate_available &&
                  (state.narrow || state.profile != SequenceProfile::Strict);
#endif
  std::fill_n(work->finite.begin(), count, true);
  for (unsigned j = 0; j < 3; ++j)
    for (unsigned i = 0; i < count; ++i) {
      const bool finite = finite_bits(work->raw[j][i], state.narrow);
      work->finite[i] = work->finite[i] && finite;
      // Classify nonfinite bits before any floating conversion, including
      // signaling NaNs. Strict binary64 does not need a floating candidate.
      if (!use_candidate)
        continue;
      if (!finite) {
        work->candidate.input[j][i] = 0;
      } else if (state.narrow) {
        const auto raw = static_cast<std::uint32_t>(work->raw[j][i]);
        float value;
        std::memcpy(&value, &raw, sizeof(value));
        work->candidate.input[j][i] = value;
      } else {
        std::memcpy(&work->candidate.input[j][i], &work->raw[j][i], 8);
      }
    }
  if (use_candidate)
    candidates(&work->candidate, count, coefficients, state.profile);
  for (unsigned i = 0; i < count; ++i) {
    if (!work->finite[i] || !use_candidate ||
        !certify(coefficients, work->candidate, i, state.narrow, state.profile,
                 &work->result[i])) {
      auto result = coefficients.evaluate(
          {work->raw[0][i], work->raw[1][i], work->raw[2][i]}, state.narrow,
          &work->exact, consume);
      if (!result.ok())
        return result.status();
      work->result[i] = result.value();
    }
  }
  return Status::success();
}
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const PreparedBasis*>(invocation->prepared_context);
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& result) {
    if (!result.ok())
      *status =
          kernel_failure(result.code, result.message.c_str(), result.reason);
    return result.ok();
  };
  if (!accept(consume(0)))
    return;
  if (state.view) {
    *status = kernel_failure(ErrorCode::InvalidArgument, "ViewUnavailable",
                             FailureReason::ViewUnavailable);
    return;
  }
  FloatingEnvironment environment;
  if (state.calculates && !environment.active()) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "RGB numeric environment");
    return;
  }
  Workspace* work = state.calculates ? new (invocation->scratch[0].allocation)
                                           Workspace
                                     : nullptr;
  fixed_tensor::PointReader<1> reader{state.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  const unsigned width = state.narrow ? 4 : 8;
  auto* destination = static_cast<std::uint8_t*>(output.allocation);
  const auto first = layout.origin[state.axis];
  const auto channels = layout.shape[state.axis];
  std::uint64_t points = 1;
  for (unsigned a = 0; a < layout.rank; ++a)
    if (a != state.axis)
      points *= layout.shape[a];
  for (auto channel = first; channel < first + channels; ++channel) {
    const int row = state.row(channel);
    const bool arithmetic = row >= 0 && !state.is_identity;
    for (std::uint64_t begin = 0; begin < points; begin += block_size) {
      if (!accept(consume(0)))
        return;
      const auto count = static_cast<unsigned>(
          std::min<std::uint64_t>(block_size, points - begin));
      for (unsigned lane = 0; lane < count; ++lane) {
        auto index = begin + lane;
        std::array<std::uint64_t, 8> at{};
        std::uint64_t offset = output.byte_offset;
        for (unsigned a = layout.rank; a-- > 0;) {
          if (a == state.axis) {
            at[a] = channel;
          } else {
            at[a] = layout.origin[a] + index % layout.shape[a];
            index /= layout.shape[a];
          }
          offset += (at[a] - layout.origin[a]) * layout.strides[a];
        }
        if (!arithmetic) {
          const auto bits = reader.bits(0, at, width);
          std::memcpy(destination + offset, &bits, width);
        } else {
          for (unsigned j = 0; j < 3; ++j) {
            at[state.axis] = state.selected[j];
            work->raw[j][lane] = reader.bits(0, at, width);
          }
          work->packed_offsets[lane] = offset;
        }
      }
      if (arithmetic) {
        if (!accept(evaluate(work, count, static_cast<unsigned>(row), state,
                             consume)))
          return;
        for (unsigned lane = 0; lane < count; ++lane)
          std::memcpy(destination + work->packed_offsets[lane],
                      &work->result[lane], width);
      }
    }
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid RGB basis signature"});
  };
  const auto* state = static_cast<const Preparation*>(query.static_state);
  if (!state || query.inputs.size() != 1 || query.outputs.size() != 1 ||
      query.writes.size() != 1 || !query.task_parameters.empty() ||
      !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output_schema))
    return invalid();
  const auto& write = query.writes[0];
  auto grid =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto region = grid.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (query.scratch !=
          std::array<std::uint64_t, 3>{scratch_bytes(*state, region.value()), 0,
                                       0} ||
      write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type !=
          (state->narrow ? ElementType::Float32 : ElementType::Float64) ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  auto prepared = std::make_unique<PreparedBasis>();
  prepared->narrow = state->narrow;
  prepared->is_identity = state->is_identity;
  prepared->view = state->layout == "view";
  prepared->calculates = calculates(*state, region.value());
  prepared->axis = state->axis;
  prepared->profile = state->profile;
  prepared->selected = state->selected;
  prepared->rows = state->rows;
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!input.ok())
    return Answer(input.status());
  prepared->inputs.grids[0] = input.take_value();
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  ResourceBudget budget(limits);
  auto reads = required_tiles(*state, region.value(), prepared->inputs.grids[0],
                              budget, query.reads.size());
  if (!reads.ok())
    return Answer(reads.status());
  std::size_t slot = 0;
  for (auto index : reads.value()) {
    const auto& read = query.reads[slot];
    auto expected = prepared->inputs.grids[0].region(index);
    if (!expected.ok())
      return Answer(expected.status());
    if (read.port || read.member || read.tile_index != index ||
        read.domain != physical::Domain::Host ||
        read.layout.element_type != write.layout.element_type ||
        !fixed_tensor::matches_region(read.layout, expected.value()))
      return invalid();
    prepared->inputs.slots[0].push_back(
        {index, static_cast<std::uint32_t>(slot++)});
  }
  if (slot != query.reads.size())
    return invalid();
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = prepared.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedBasis*>(context);
  };
  return Answer(kernel);
}
OperationDefinition definition(unsigned member, const std::string& suffix,
                               SequenceProfile profile) {
  const std::array<const char*, 3> names{
      {"rgb_to_xyz", "xyz_to_rgb", "adapt_xyz_white"}};
  OperationDefinition op;
  op.key = std::string("color.") + names[member] + '_' + suffix;
  auto& traits = op.traits;
  traits.input_count = 1;
  OperationPortConstraint port;

  port.element_type_mask = 127;
  traits.input_schema = {port};
  traits.requires_metadata_specialization = true;
  for (const auto* name :
       {"group", "components", "input_description", "layout"})
    traits.parameter_schema.push_back(
        {name, OperationParameterType::String, false});
  traits.parameter_schema.push_back(
      {"axis", OperationParameterType::Int64, false});
  if (member == 0) {
    traits.parameter_schema.push_back(
        {"source_basis", OperationParameterType::String, false});
  } else {
    traits.parameter_schema.push_back(
        {"source_white", OperationParameterType::String, false});
    traits.parameter_schema.push_back(
        {"target_white", OperationParameterType::String, member == 2});
    if (member == 1) {
      traits.parameter_schema.push_back(
          {"target_basis", OperationParameterType::String, true});
      traits.parameter_schema.push_back(
          {"white_handling", OperationParameterType::String, false});
    } else {
      traits.parameter_schema.push_back(
          {"method", OperationParameterType::String, true});
    }
  }
  auto& output = traits.outputs[0];
  output.key = "values";
  output.output_schema = port;
  output.result_schema = tensor_ops::scalar_schema();
  op.prepare_static = [member, profile](const auto& inputs,
                                        const auto& params) {
    return prepare(inputs, params, member, profile);
  };
  op.lower = lower;
  op.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "rgb-basis" || kernel.contract != basis_contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid RGB basis kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "RGB basis requires CPU Host"});
    return prepare_kernel(query);
  };
  return op;
}

// Resolve real graph metadata without trusting a supplied RGB interpretation.
// An explicit DFS stack bounds storage by reachable nodes, not C++ call depth.
OperationMetadata edge_metadata(const WorkflowDocument& document,
                                const WorkflowInput& edge,
                                const OperationRegistry& registry) {
  std::map<std::uint64_t, const WorkflowNode*> nodes;
  std::map<std::uint64_t, OperationMetadata> inputs;
  for (const auto& declaration : document.inputs) {
    auto canonical = declaration;
    require(canonical.id && canonical.result_schema, "invalid graph input");
    checked(canonical.result_schema->validate());
    OperationMetadata m;
    m.result_schema = canonical.result_schema;
    require(inputs.emplace(canonical.id, std::move(m)).second,
            "duplicate graph input id");
  }
  for (const auto& n : document.nodes)
    require(n.id && nodes.emplace(n.id, &n).second,
            "duplicate/zero graph node id");
  std::map<std::uint64_t, std::map<std::string, OperationMetadata>> outputs;
  std::map<std::uint64_t, unsigned> status;
  const auto resolve_edge =
      [&](const WorkflowInput& source) -> OperationMetadata {
    if (const auto* ref = std::get_if<WorkflowInputReference>(&source)) {
      const auto it = inputs.find(ref->input_id);
      require(it != inputs.end(), "input edge declaration is absent");
      return it->second;
    }
    const auto& ref = std::get<WorkflowNodeOutput>(source);
    const auto it = outputs.find(ref.source_node);
    require(it != outputs.end() && it->second.count(ref.source_port),
            "node/port is absent");
    return it->second.at(ref.source_port);
  };
  if (std::holds_alternative<WorkflowInputReference>(edge))
    return resolve_edge(edge);
  struct Frame final {
    std::uint64_t id;
    std::size_t next = 0;
  };
  std::vector<Frame> stack;
  const auto push = [&](std::uint64_t id) {
    require(nodes.count(id), "producer node is absent");
    require(status[id] != 1, "cycle in source graph");
    status[id] = 1;
    stack.push_back({id, 0});
  };
  push(std::get<WorkflowNodeOutput>(edge).source_node);
  while (!stack.empty()) {
    auto& frame = stack.back();
    const auto& n = *nodes.at(frame.id);
    bool descended = false;
    while (frame.next < n.inputs.size()) {
      const auto& dependency = n.inputs[frame.next++];
      if (const auto* ref = std::get_if<WorkflowNodeOutput>(&dependency)) {
        if (status[ref->source_node] != 2) {
          push(ref->source_node);
          descended = true;
          break;
        }
      }
    }
    if (descended)
      continue;
    std::vector<OperationMetadata> source;
    for (const auto& ref : n.inputs)
      source.push_back(resolve_edge(ref));
    const auto prepared =
        checked(registry.prepare_operation(n.operation, source, n.parameters));
    auto inferred = checked(
        infer_operation_outputs(prepared->traits(), source, n.parameters));
    for (unsigned i = 0; i < inferred.size(); ++i)
      outputs[n.id].emplace(prepared->traits().outputs[i].key,
                            std::move(inferred[i]));
    status[n.id] = 2;
    stack.pop_back();
  }
  return resolve_edge(edge);
}
Parameters parameters(const format::RgbBasisOptions& options) {
  require(options.profile == "strict" ||
              options.profile == "accelerated_apple_silicon" ||
              options.profile == "accelerated_x86_64",
          "unknown CPU profile");
  Parameters out{{"layout", options.layout}};
  if (!options.group.empty())
    out.emplace("group", options.group);
  if (options.components) {
    const auto& c = *options.components;
    out.emplace("components", std::to_string(c[0]) + ',' +
                                  std::to_string(c[1]) + ',' +
                                  std::to_string(c[2]));
  }
  if (options.axis)
    out.emplace("axis", static_cast<std::int64_t>(*options.axis));
  if (options.source_basis)
    out.emplace("source_basis", basis_codec(resolve(*options.source_basis)));
  if (options.target_basis)
    out.emplace("target_basis", basis_codec(resolve(*options.target_basis)));
  if (options.source_white) {
    (void)white_xyz(*options.source_white);
    out.emplace("source_white", white_codec(*options.source_white));
  }
  if (options.target_white) {
    (void)white_xyz(*options.target_white);
    out.emplace("target_white", white_codec(*options.target_white));
  }
  if (options.method)
    out.emplace("method", *options.method);
  if (options.white_handling)
    out.emplace("white_handling", *options.white_handling);
  if (options.input_description)
    out.emplace(
        "input_description",
        checked(tensor_description_parameter(*options.input_description)));
  return out;
}
struct Appended final {
  WorkflowNodeOutput edge;
  OperationMetadata metadata;
};
Appended append(WorkflowDocument* document, WorkflowInput input,
                const OperationMetadata& source, unsigned member,
                const format::RgbBasisOptions& options,
                const OperationRegistry& registry) {
  const std::array<const char*, 3> names{
      {"rgb_to_xyz", "xyz_to_rgb", "adapt_xyz_white"}};
  auto params = parameters(options);
  const auto key =
      std::string("color.") + names[member] + '_' + options.profile;
  auto prepared = checked(registry.prepare_operation(key, {source}, params));
  auto inferred =
      checked(infer_operation_outputs(prepared->traits(), {source}, params));
  const auto id =
      checked(numeric::available_workflow_node_ids(*document, 1, {input}))[0];
  document->nodes.push_back({id, key, {std::move(input)}, std::move(params)});
  return {{id, "values"}, std::move(inferred[0])};
}
Result<WorkflowNodeOutput> author(WorkflowDocument& document,
                                  WorkflowInput input,
                                  const format::RgbBasisOptions& options,
                                  std::shared_ptr<OperationRegistry> registry,
                                  unsigned member) {
  using Answer = Result<WorkflowNodeOutput>;
  try {
    plugin_internal::FloatingEnvironment environment;
    typed(environment.active(), "floating environment unavailable");
    if (!registry)
      registry = make_default_operation_registry();
    const auto source = edge_metadata(document, input, *registry);
    WorkflowDocument staged = document;
    if (member < 3) {
      const auto out =
          append(&staged, input, source, member, options, *registry);
      document = std::move(staged);
      return Answer(out.edge);
    }
    require(options.target_basis.has_value(), "D requires target_basis");
    require(!options.source_white, "D source white belongs in source_basis");
    const auto target = resolve(*options.target_basis);
    (void)rgb_matrix(target.xy, target.white);
    if (options.target_white)
      require(*options.target_white == target.white,
              "D duplicate target white disagrees");
    const auto policy = options.white_handling.value_or("require_match");
    require(policy == "require_match" || policy == "preserve_xyz" ||
                policy == "adapt",
            "unknown D white policy");
    require((policy == "adapt") == options.method.has_value(),
            "D method is required exactly for adapt");
    auto a = options;
    a.target_basis.reset();
    a.target_white.reset();
    a.white_handling.reset();
    a.method.reset();
    // A caller-owned registry can legally bind this key to another operation.
    // Its opaque state has no internal type guarantee (and can be null).
    // Resolve the helper's source contract using our own preparation instead;
    // only the result of this direct, private call has Preparation provenance.
    const auto builtin_a =
        prepare_impl({source}, parameters(a), 0, SequenceProfile::Strict);
    const auto& prepared_a =
        *static_cast<const Preparation*>(builtin_a.state.get());
    auto first = append(&staged, input, source, 0, a, *registry);
    typed(policy != "require_match" || prepared_a.source_white == target.white,
          "D require_match rejects unequal whites");
    auto current = first;
    auto current_description = prepared_a.output_description;
    if (policy == "adapt") {
      auto c = options;
      c.input_description = current_description;
      c.source_basis.reset();
      c.target_basis.reset();
      c.white_handling.reset();
      c.target_white = target.white;
      if (!c.input_description)
        c.source_white = prepared_a.source_white;
      const auto converted = prepare_impl({current.metadata}, parameters(c), 2,
                                          SequenceProfile::Strict);
      current_description =
          static_cast<const Preparation*>(converted.state.get())
              ->output_description;
      current =
          append(&staged, current.edge, current.metadata, 2, c, *registry);
    }
    auto b = options;
    b.input_description = current_description;
    b.source_basis.reset();
    b.source_white.reset();
    b.target_white.reset();
    b.method.reset();
    if (!b.input_description)
      b.white_handling.reset();
    else
      b.white_handling =
          policy == "preserve_xyz" ? "preserve_xyz" : "require_match";
    current = append(&staged, current.edge, current.metadata, 1, b, *registry);
    document = std::move(staged);
    return Answer(current.edge);
  } catch (const CheckedFailure& f) {
    return Answer(f.status);
  } catch (const GeometryError&) {
    return Answer(
        invalid("invalid/singular geometry or nonpositive white response"));
  } catch (const ExactWorkFailure& f) {
    return Answer(f.status);
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         "FMT-10 authoring capacity exhausted",
                         FailureReason::CapacityLimit});
  }
}
}  // namespace
Status register_members(OperationRegistry* registry) {
  for (const auto& p :
       {std::make_pair("strict", SequenceProfile::Strict),
        std::make_pair("accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (unsigned member = 0; member < 3; ++member) {
      auto status =
          registry->register_operation(definition(member, p.first, p.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal::basis_ops

namespace ps::plugin_internal {
Status register_rgb_basis(OperationRegistry* registry) {
  return basis_ops::register_members(registry);
}
}  // namespace ps::plugin_internal

namespace ps::format {
Result<WorkflowNodeOutput> rgb_to_xyz(WorkflowDocument& d, WorkflowInput input,
                                      const RgbBasisOptions& o,
                                      std::shared_ptr<OperationRegistry> r) {
  return plugin_internal::basis_ops::author(d, std::move(input), o,
                                            std::move(r), 0);
}
Result<WorkflowNodeOutput> xyz_to_rgb(WorkflowDocument& d, WorkflowInput input,
                                      const RgbBasisOptions& o,
                                      std::shared_ptr<OperationRegistry> r) {
  return plugin_internal::basis_ops::author(d, std::move(input), o,
                                            std::move(r), 1);
}
Result<WorkflowNodeOutput> adapt_xyz_white(
    WorkflowDocument& d, WorkflowInput input, const RgbBasisOptions& o,
    std::shared_ptr<OperationRegistry> r) {
  return plugin_internal::basis_ops::author(d, std::move(input), o,
                                            std::move(r), 2);
}
Result<WorkflowNodeOutput> convert_linear_rgb(
    WorkflowDocument& d, WorkflowInput input, const RgbBasisOptions& o,
    std::shared_ptr<OperationRegistry> r) {
  return plugin_internal::basis_ops::author(d, std::move(input), o,
                                            std::move(r), 3);
}
}  // namespace ps::format
