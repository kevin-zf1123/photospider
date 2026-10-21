#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/domain/model_coordinates.hpp"
#include "common/domain/rgb_geometry_codec.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/tensor_program.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps::plugin_internal::color_point {
using Parameters = std::map<std::string, ParameterValue>;
using basis_ops::checked;
inline void require(bool condition, const std::string& message) {
  if (!condition) {
    throw basis_ops::CheckedFailure{
        Status{ErrorCode::InvalidArgument,
               "color: " + message,
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}}};
  }
}
inline void typed(bool condition, const std::string& message) {
  if (!condition) {
    throw basis_ops::CheckedFailure{
        Status{ErrorCode::TypeMismatch,
               "color: " + message,
               FailureReason::None,
               {FailureOrigin::Schema, FailureScope::Unspecified}}};
  }
}
inline std::string text(const Parameters& p, const char* key,
                        const char* fallback = "") {
  const auto it = p.find(key);
  return it == p.end() ? fallback : std::get<std::string>(it->second);
}
inline double real(const Parameters& p, const char* key, double fallback) {
  const auto it = p.find(key);
  return it == p.end() ? fallback : std::get<double>(it->second);
}
inline bool boolean(const Parameters& p, const char* key,
                    bool fallback = false) {
  const auto it = p.find(key);
  return it == p.end() ? fallback : std::get<bool>(it->second);
}
inline std::vector<std::uint64_t> indices(const std::string& value) {
  std::vector<std::uint64_t> result;
  std::size_t first = 0;
  do {
    const auto end = value.find(',', first);
    const auto token =
        value.substr(first, end == std::string::npos ? end : end - first);
    std::uint64_t index = 0;
    const auto parsed =
        std::from_chars(token.data(), token.data() + token.size(), index);
    require(!token.empty() && parsed.ec == std::errc{} &&
                parsed.ptr == token.data() + token.size() &&
                token == std::to_string(index),
            "invalid component index");
    require(std::find(result.begin(), result.end(), index) == result.end(),
            "duplicate component index");
    result.push_back(index);
    require(result.size() <= 4, "at most four color coordinates");
    if (end == std::string::npos) {
      break;
    }
    first = end + 1;
  } while (true);
  return result;
}
inline TensorInterpretation global(const TensorDescription& d) {
  return {d.model,       d.primaries,  d.transfer,         d.reference,
          d.association, d.white,      d.primaries_xy,     d.profile,
          d.convention,  d.configured, d.analytic_binding, d.coordinates};
}
inline void merge(TensorInterpretation* a, const TensorInterpretation& b) {
  const bool a_asserts = !a->model.empty() || a->profile || a->configured;
  const bool b_asserts = !b.model.empty() || b.profile || b.configured;
  require(!a_asserts || !b_asserts || a->convention == b.convention,
          "conflicting coordinate convention");
  if (!a_asserts && b_asserts) {
    a->convention = b.convention;
  }
  for (auto pair : {std::make_pair(&a->model, &b.model),
                    std::make_pair(&a->primaries, &b.primaries),
                    std::make_pair(&a->transfer, &b.transfer),
                    std::make_pair(&a->reference, &b.reference),
                    std::make_pair(&a->association, &b.association)}) {
    require(pair.first->empty() || pair.second->empty() ||
                *pair.first == *pair.second,
            "conflicting color interpretation");
    if (pair.first->empty()) {
      *pair.first = *pair.second;
    }
  }
  const auto optional = [](auto* destination, const auto& source) {
    require(!*destination || !source || **destination == *source,
            "conflicting color configuration");
    if (!*destination) {
      *destination = source;
    }
  };
  optional(&a->white, b.white);
  optional(&a->primaries_xy, b.primaries_xy);
  optional(&a->profile, b.profile);
  optional(&a->configured, b.configured);
  optional(&a->analytic_binding, b.analytic_binding);
  require(data_internal::overlay_model_coordinates(&a->coordinates,
                                                   b.coordinates, true),
          "conflicting model coordinates");
}
inline basis_ops::Rational endpoint(const TensorEndpoint& x) {
  if (const auto* i = std::get_if<std::int64_t>(&x)) {
    return basis_ops::Rational::integer(*i);
  }
  if (const auto* d = std::get_if<double>(&x)) {
    return basis_ops::number(*d);
  }
  const auto& r = std::get<TensorRationalEndpoint>(x);
  basis_ops::Rational result;
  result.n.words.assign(r.numerator.begin(), r.numerator.end());
  result.d.words.assign(r.denominator.begin(), r.denominator.end());
  result.negative = r.negative;
  return result;
}
inline void native_encoding(const std::optional<TensorEncoding>& e) {
  if (!e) {
    return;
  }
  for (unsigned i = 0; i != 2; ++i) {
    require(endpoint(e->stored[i]).compare(endpoint(e->decoded[i])) == 0,
            "decode numeric encoding before color processing");
  }
}
struct Selection final {
  int axis = -1;
  unsigned batch_rank = 0;
  std::vector<std::uint64_t> channels;
  TensorInterpretation interpretation;
  std::vector<std::string> units;
  bool semantic = false;
  std::optional<TensorDescription> description;
};
inline Selection select(const OperationMetadata& input, const Parameters& p,
                        const std::vector<std::string>& expected_roles = {},
                        bool native = true) {
  checked(tensor_ops::check_tensor(input));
  const auto& tensor = input.result_schema->tensors[0];
  const auto& shape = tensor.descriptor.shape;
  typed(!shape.empty() && tensor.sample_shape().size() <= 8 &&
            (tensor.descriptor.element_type == ElementType::Float32 ||
             tensor.descriptor.element_type == ElementType::Float64),
        "color operations require Float32/64 rank 1..8");
  const auto count = checked(tensor.sample_count());
  typed(count && count <= (UINT64_C(1) << 40),
        "color sample count exceeds 2^40");
  Selection s;
  s.batch_rank = static_cast<unsigned>(tensor.batch_axes.size());
  s.semantic = p.count("input_description") != 0;
  if (!s.semantic) {
    require(!p.count("group"), "raw selection forbids group");
    if (p.count("axis")) {
      const auto axis = std::get<std::int64_t>(p.at("axis"));
      require(axis >= 0 && static_cast<std::size_t>(axis) < shape.size(),
              "axis outside cell rank");
      s.axis = static_cast<int>(axis) + static_cast<int>(s.batch_rank);
      require(p.count("components"), "raw channel axis requires components");
      s.channels = indices(text(p, "components"));
      for (auto c : s.channels) {
        require(c < shape[axis], "component outside channel axis");
      }
    } else {
      require(!p.count("components"), "axis-free Gray forbids components");
      s.channels = {0};
    }
    if (!expected_roles.empty()) {
      typed(s.channels.size() == expected_roles.size(),
            "wrong color component count");
    }
    s.units.assign(s.channels.size(), text(p, "units"));
    if (tensor.layout.spatial) {
      require(
          tensor.layout.channel_axis ==
              (s.axis < 0
                   ? std::optional<std::uint32_t>{}
                   : std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                         s.axis - s.batch_rank)}),
          "color selector must use spatial channel axis");
    }
    return s;
  }
  require(!p.count("axis") && !p.count("components"),
          "description forbids raw selectors");
  auto d =
      checked(tensor_description_from_parameter(text(p, "input_description")));
  checked(validate_tensor_description(d, tensor.descriptor));
  native_encoding(d.encoding);
  s.interpretation = global(d);
  std::vector<TensorChannelDescription> components;
  if (d.channel_axis || !d.groups.empty() || p.count("group")) {
    require(p.count("group"), "explicit color group required");
    if (d.channel_axis) {
      s.axis = static_cast<int>(s.batch_rank + *d.channel_axis);
    }
    const TensorColorGroup* found = nullptr;
    for (const auto& group : d.groups) {
      if (group.name == text(p, "group")) {
        require(!found, "ambiguous color group");
        found = &group;
      }
    }
    require(found != nullptr, "color group not found");
    merge(&s.interpretation, found->interpretation);
    s.channels = found->indices;
    components = found->components;
    if (!d.channel_axis && d.component) {
      const auto& c = *d.component;
      if (c.interpretation) {
        merge(&s.interpretation, *c.interpretation);
      }
      native_encoding(c.encoding);
      require(c.role.empty() || c.role == components[0].role,
              "conflicting Gray role");
      require(c.unit.empty() || components[0].unit.empty() ||
                  c.unit == components[0].unit,
              "conflicting Gray units");
      if (components[0].unit.empty()) {
        components[0].unit = c.unit;
      }
    }
    for (const auto& other : d.groups) {
      if (&other != found) {
        for (auto c : other.indices) {
          require(std::find(s.channels.begin(), s.channels.end(), c) ==
                      s.channels.end(),
                  "selected color group overlaps another group");
        }
      }
    }
    require(s.channels.size() <= 4 && !s.channels.empty(),
            "unsupported color group size");
    for (unsigned i = 0; i < s.channels.size(); ++i) {
      if (s.channels[i] < d.channels.size()) {
        const auto& c = d.channels[s.channels[i]];
        if (c.interpretation) {
          merge(&s.interpretation, *c.interpretation);
        }
        native_encoding(c.encoding);
        require(c.role.empty() || components[i].role.empty() ||
                    c.role == components[i].role,
                "conflicting component roles");
        require(c.unit.empty() || components[i].unit.empty() ||
                    c.unit == components[i].unit,
                "conflicting component units");
        if (components[i].role.empty()) {
          components[i].role = c.role;
        }
        if (components[i].unit.empty()) {
          components[i].unit = c.unit;
        }
      }
    }
  } else {
    require(!p.count("group") && d.component,
            "axis-free input requires component");
    components = {*d.component};
    s.channels = {0};
  }
  for (const auto& c : components) {
    if (c.interpretation) {
      merge(&s.interpretation, *c.interpretation);
    }
    native_encoding(c.encoding);
    s.units.push_back(c.unit);
  }
  if (native && (s.interpretation.profile || s.interpretation.configured)) {
    require(s.interpretation.analytic_binding.has_value(),
            "profile/configured source requires analytic endpoint binding");
    const auto binding = *s.interpretation.analytic_binding;
    require(binding.roles.size() == components.size() &&
                binding.units.size() == components.size(),
            "analytic binding component count disagrees");
    TensorInterpretation native;
    native.model = binding.model;
    native.primaries = binding.primaries;
    native.transfer = binding.transfer;
    native.reference = binding.reference;
    native.white = binding.white;
    native.primaries_xy = binding.primaries_xy;
    native.convention = binding.convention;
    // The binding asserts identical numerical coordinates, not a conversion.
    // Resolve its native convention separately from the configured identity.
    s.interpretation.convention = native.convention;
    s.interpretation.profile.reset();
    s.interpretation.configured.reset();
    s.interpretation.analytic_binding.reset();
    merge(&s.interpretation, native);
    for (unsigned i = 0; i < components.size(); ++i) {
      require(components[i].role == binding.roles[i],
              "analytic binding role order disagrees");
      require(s.units[i].empty() || s.units[i] == binding.units[i],
              "analytic binding units disagree");
      s.units[i] = binding.units[i];
    }
  }
  require(s.interpretation.association.empty() ||
              s.interpretation.association == "straight",
          "straight color required");
  require(!native || s.interpretation.convention == "relative-v1",
          "native coordinate convention required");
  if (!expected_roles.empty()) {
    typed(components.size() == expected_roles.size(),
          "wrong color component count");
    std::vector<std::uint64_t> ordered;
    std::vector<std::string> units;
    for (const auto& role : expected_roles) {
      unsigned index = 0;
      while (index < components.size() && components[index].role != role) {
        ++index;
      }
      require(index < components.size(), "missing color role");
      ordered.push_back(s.channels[index]);
      units.push_back(s.units[index]);
    }
    s.channels = std::move(ordered);
    s.units = std::move(units);
  }
  if (tensor.layout.spatial) {
    require(tensor.layout.channel_axis == d.channel_axis,
            "description must use spatial channel axis");
  }
  s.description = std::move(d);
  return s;
}

// A span either copies contiguous source slots or names one transformed output.
// Only a bounded number of spans is needed, even with a large AOV axis.
struct Span final {
  std::uint64_t first = 0, count = 1, source = 0;
  int component = -1;
};
struct Plan {
  Selection selection;
  int output_axis = -1;
  std::vector<Span> spans;
  std::shared_ptr<const SchemaTemplate> input_schema, output_schema;
  std::string symbol, contract;
  unsigned output_components = 3;
  bool narrow = false, componentwise = false;
  std::uint64_t scratch = 0;
};
inline void same_shape(Plan* p, const OperationMetadata& input) {
  p->input_schema = input.result_schema;
  auto schema = *input.result_schema;
  schema.tensors[0].atomic_trailing_axes = 0;
  p->output_schema = std::make_shared<const SchemaTemplate>(std::move(schema));
  p->output_axis = p->selection.axis;
  p->output_components = static_cast<unsigned>(p->selection.channels.size());
  p->narrow = input.result_schema->tensors[0].descriptor.element_type ==
              ElementType::Float32;
  if (p->output_axis < 0) {
    p->spans = {{0, 1, 0, 0}};
    return;
  }
  const auto shape = input.result_schema->tensors[0].sample_shape();
  auto ordered = p->selection.channels;
  std::sort(ordered.begin(), ordered.end());
  std::uint64_t first = 0;
  for (auto c : ordered) {
    if (first < c) {
      p->spans.push_back({first, c - first, first, -1});
    }
    const auto i = std::find(p->selection.channels.begin(),
                             p->selection.channels.end(), c);
    p->spans.push_back(
        {c, 1, c, static_cast<int>(i - p->selection.channels.begin())});
    first = c + 1;
  }
  if (first < shape[p->output_axis]) {
    p->spans.push_back({first, shape[p->output_axis] - first, first, -1});
  }
}
inline void mask_shape(Plan* p, const OperationMetadata& input, bool keepdims) {
  p->input_schema = input.result_schema;
  auto schema = *input.result_schema;
  auto& tensor = schema.tensors[0];
  tensor.atomic_trailing_axes = 0;
  const auto axis =
      static_cast<unsigned>(p->selection.axis) - p->selection.batch_rank;
  require(p->selection.axis >= 0 &&
              (keepdims || tensor.descriptor.shape.size() > 1),
          "rank-one mask requires keepdims");
  if (keepdims) {
    tensor.descriptor.shape[axis] = 1;
    p->output_axis = p->selection.axis;
  } else {
    tensor.descriptor.shape.erase(tensor.descriptor.shape.begin() + axis);
    p->output_axis = -1;
    if (tensor.layout.spatial) {
      if (tensor.layout.height_axis > axis) {
        --tensor.layout.height_axis;
      }
      if (tensor.layout.width_axis > axis) {
        --tensor.layout.width_axis;
      }
      tensor.layout.channel_axis.reset();
    }
  }
  p->narrow = tensor.descriptor.element_type == ElementType::Float32;
  p->output_schema = std::make_shared<const SchemaTemplate>(std::move(schema));
  p->output_components = 1;
  p->spans = {{0, 1, 0, 0}};
}
inline bool intersects(const Plan& p, const Span& s, const Region& r) {
  if (p.output_axis < 0) {
    return true;
  }
  const auto c = r.dimensions()[p.output_axis];
  return c.offset < s.first + s.count && s.first < c.offset + c.extent;
}
inline bool calculates(const Plan& p, const Region& r) {
  for (const auto& s : p.spans) {
    if (s.component >= 0 && intersects(p, s, r)) {
      return true;
    }
  }
  return false;
}
inline std::uint64_t scratch_bytes(const Plan& p, const Region& r) {
  return calculates(p, r) ? p.scratch : 0;
}
inline std::vector<RegionDimension> source_dimensions(const Plan& p,
                                                      const Region& out) {
  auto result = out.dimensions();
  if (p.output_axis >= 0) {
    result.erase(result.begin() + p.output_axis);
  }
  if (p.selection.axis >= 0) {
    result.insert(result.begin() + p.selection.axis, {0, 1});
  }
  return result;
}
inline Result<std::set<TileIndex>> required_tiles(const Plan& p,
                                                  const Region& out,
                                                  const TileGrid& grid,
                                                  const ResourceBudget& budget,
                                                  std::uint64_t maximum) {
  using Answer = Result<std::set<TileIndex>>;
  std::set<TileIndex> result;
  for (const auto& s : p.spans) {
    if (!intersects(p, s, out)) {
      continue;
    }
    std::uint64_t first = s.first, count = s.count;
    if (p.output_axis >= 0) {
      const auto c = out.dimensions()[p.output_axis];
      first = std::max(first, c.offset);
      count = std::min(s.first + s.count, c.offset + c.extent) - first;
    }
    const unsigned terms =
        s.component < 0 || p.componentwise
            ? 1U
            : static_cast<unsigned>(p.selection.channels.size());
    for (unsigned term = 0; term < terms; ++term) {
      auto dims = source_dimensions(p, out);
      if (p.selection.axis >= 0) {
        dims[p.selection.axis] =
            s.component < 0
                ? RegionDimension{s.source + first - s.first, count}
                : RegionDimension{
                      p.selection
                          .channels[p.componentwise ? s.component : term],
                      1};
      }
      std::uint64_t total = 1;
      for (unsigned a = 0; a < grid.rank(); ++a) {
        const auto d = dims[a];
        const auto n = (grid.batch_axis_mask() & (1U << a)) ? d.extent
                       : (a == grid.height_axis() || a == grid.width_axis())
                           ? (d.offset + d.extent - 1) / grid.tile_extent() -
                                 d.offset / grid.tile_extent() + 1
                           : 1;
        if (n > maximum / total) {
          return Answer(Status{ErrorCode::ResourceExhausted,
                               "color input tile limit",
                               FailureReason::CapacityLimit});
        }
        total *= n;
      }
      auto tiles = sdk::tiles_for_region(budget, grid, Region(std::move(dims)));
      if (!tiles.ok()) {
        return Answer(tiles.status());
      }
      for (auto tile : tiles.value()) {
        const auto status = budget.consume({1});
        if (!status.ok()) {
          return Answer(status);
        }
        result.insert(tile);
        if (result.size() > maximum) {
          return Answer(Status{ErrorCode::ResourceExhausted,
                               "color tile union limit",
                               FailureReason::CapacityLimit});
        }
      }
    }
  }
  return Answer(std::move(result));
}
template <class State>
Result<OperationProgram> lower(const OperationLowering& q) {
  using Answer = Result<OperationProgram>;
  const Plan& p = *static_cast<const State*>(q.static_state);
  auto output =
      TileGrid::from_schema(*q.outputs[0].result_schema, q.tile_extent);
  auto input = TileGrid::from_schema(*q.inputs[0].result_schema, q.tile_extent);
  if (!output.ok()) {
    return Answer(output.status());
  }
  if (!input.ok()) {
    return Answer(input.status());
  }
  ResourceLimits limits;
  limits.maximum_work = q.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(q);
  for (const auto& member : q.requests[0].members) {
    for (auto tile : member.second) {
      auto region = output.value().region(tile);
      if (!region.ok()) {
        return Answer(region.status());
      }
      auto task = builder.add_task(p.symbol, p.contract, {},
                                   {scratch_bytes(p, region.value()), 0, 0});
      if (!task.ok()) {
        return Answer(task.status());
      }
      auto reads = required_tiles(p, region.value(), input.value(), budget,
                                  q.limits.maximum_records);
      if (!reads.ok()) {
        return Answer(reads.status());
      }
      for (auto index : reads.value()) {
        const auto status = builder.read_input(task.value(), 0, 0, index);
        if (!status.ok()) {
          return Answer(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok()) {
        return Answer(status);
      }
    }
  }
  return builder.finish();
}
inline Status prepare_inputs(const Plan& p, const KernelPreparation& q,
                             fixed_tensor::PointInputs<1>* inputs) {
  if (q.inputs.size() != 1 || q.outputs.size() != 1 || q.writes.size() != 1 ||
      !q.task_parameters.empty() || !q.inputs[0].result_schema ||
      !q.inputs[0].result_schema->same_schema(*p.input_schema) ||
      !q.outputs[0].result_schema ||
      !q.outputs[0].result_schema->same_schema(*p.output_schema)) {
    return {ErrorCode::TypeMismatch, "invalid color kernel signature"};
  }
  auto grid = TileGrid::from_schema(*p.output_schema, q.tile_extent);
  if (!grid.ok()) {
    return grid.status();
  }
  const auto& write = q.writes[0];
  auto region = grid.value().region(write.tile_index);
  if (!region.ok()) {
    return region.status();
  }
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type !=
          (p.narrow ? ElementType::Float32 : ElementType::Float64) ||
      !fixed_tensor::matches_region(write.layout, region.value()) ||
      q.scratch != std::array<std::uint64_t, 3>{
                       scratch_bytes(p, region.value()), 0, 0}) {
    return {ErrorCode::TypeMismatch, "invalid color output signature"};
  }
  auto source =
      TileGrid::from_schema(*q.inputs[0].result_schema, q.tile_extent);
  if (!source.ok()) {
    return source.status();
  }
  inputs->grids[0] = source.take_value();
  auto reads = required_tiles(p, region.value(), inputs->grids[0],
                              ResourceBudget{}, q.reads.size());
  if (!reads.ok()) {
    return reads.status();
  }
  if (reads.value().size() != q.reads.size()) {
    return {ErrorCode::TypeMismatch, "invalid color read count"};
  }
  unsigned slot = 0;
  for (auto index : reads.value()) {
    const auto& read = q.reads[slot];
    auto expected = inputs->grids[0].region(index);
    if (!expected.ok()) {
      return expected.status();
    }
    if (read.port || read.member || read.tile_index != index ||
        read.domain != physical::Domain::Host ||
        read.layout.element_type != write.layout.element_type ||
        !fixed_tensor::matches_region(read.layout, expected.value())) {
      return {ErrorCode::TypeMismatch, "invalid color read signature"};
    }
    inputs->slots[0].push_back({index, slot++});
  }
  return Status::success();
}
inline std::array<std::uint64_t, 8> source_position(
    const Plan& p, const std::array<std::uint64_t, 8>& at,
    unsigned output_rank) {
  std::array<std::uint64_t, 8> source{}, spatial{};
  unsigned n = 0;
  for (unsigned a = 0; a < output_rank; ++a) {
    if (static_cast<int>(a) != p.output_axis) {
      spatial[n++] = at[a];
    }
  }
  unsigned i = 0;
  for (unsigned a = 0; a < n + (p.selection.axis >= 0 ? 1U : 0U); ++a) {
    if (static_cast<int>(a) != p.selection.axis) {
      source[a] = spatial[i++];
    }
  }
  return source;
}
inline std::uint64_t offset(const ps_kernel_mutable_view_v8& out,
                            const std::array<std::uint64_t, 8>& at) {
  std::uint64_t value = out.byte_offset;
  for (unsigned a = 0; a < out.layout->rank; ++a) {
    value += (at[a] - out.layout->origin[a]) * out.layout->strides[a];
  }
  return value;
}
inline OperationTraits traits(const char* output) {
  auto t = tensor_ops::traits(1);
  OperationPortConstraint port;
  port.element_type_mask = 127;
  t.input_schema[0] = port;
  t.outputs[0].output_schema = port;
  t.outputs[0].key = output;
  t.requires_metadata_specialization = true;
  for (auto key : {"input_description", "group", "components", "layout"}) {
    t.parameter_schema.push_back({key, OperationParameterType::String, false});
  }
  t.parameter_schema.push_back({"axis", OperationParameterType::Int64, false});
  return t;
}
}  // namespace ps::plugin_internal::color_point
