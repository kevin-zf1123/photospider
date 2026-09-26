#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "02-format-color/alpha_lowering.hpp"
#include "photospider/data/region_runs.hpp"

namespace ps::plugin_internal {
namespace {
struct LiteralPreparation final {
  std::array<std::uint8_t, 8> bits{};
  std::size_t width = 0;
  bool require_view = false;
};
Status unavailable() {
  return {ErrorCode::InvalidArgument,
          "ViewUnavailable: absent opaque pixels require generation",
          FailureReason::InvalidDomain,
          {FailureOrigin::Domain, FailureScope::Run}};
}
// Replicate a bounded register-sized seed. This is bitwise, including sNaNs.
void fill(const LiteralPreparation& s, std::uint8_t* out, std::uint64_t n) {
  if (!n) {
    return;
  }
  std::memcpy(out, s.bits.data(), s.width);
  for (std::uint64_t ready = 1; ready < n;) {
    const auto next = std::min(ready, n - ready);
    std::memcpy(out + ready * s.width, out, next * s.width);
    ready += next;
  }
}
struct LiteralState final {
  const LiteralPreparation* state;
  bool requested = false;
  explicit LiteralState(const LiteralPreparation* s) : state(s) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using R = Result<DependencyPoll>;
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return R(std::move(batch));
    }
    if (state->require_view) {
      return R(unavailable());
    }
    const auto& descriptor = phase.query.output.descriptor;
    const auto& facets = phase.query.output.facets;
    numeric_ops::ArrayPublication publication(
        phase.query.outputs.boxes().size(), descriptor.shape.size());
    std::vector<Value> values;
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok()) {
        return R(allocated.status());
      }
      auto writer = allocated.take_value();
      const auto count = box.element_count().value();
      for (std::uint64_t i = 0; i < count; i += 256) {
        const auto n = std::min<std::uint64_t>(256, count - i);
        auto status = phase.consume_work(n);
        if (!status.ok()) {
          return R(status);
        }
        if (phase.query.cancellation.cancelled()) {
          return R(Status{ErrorCode::Cancelled, "literal fill cancelled"});
        }
        fill(*state, writer.data() + i * state->width, n);
      }
      auto value = std::move(writer).publish(facets, phase.query.resources);
      if (!value.ok()) {
        return R(value.status());
      }
      auto retained = publication.retain(value.take_value());
      if (!retained.ok()) {
        return R(retained.status());
      }
      values.push_back(retained.take_value());
    }
    auto result = publication.finish(descriptor, phase.query.outputs,
                                     values.data(), values.size(), phase.sets,
                                     facets, phase.query.resources);
    return result.ok() ? R(result.take_value()) : R(result.status());
  }
};
Status literal_planar(const PlanarOperationInvocation& call) {
  const auto& s =
      *static_cast<const LiteralPreparation*>(call.prepared->state());
  if (s.require_view) {
    return unavailable();
  }
  const auto& dims = call.output_region.dimensions();
  const auto& layout = *call.output_metadata.planar_layout;
  auto rows = dims;
  rows[layout.width_axis].extent = 1;
  Region row_region(rows);
  std::vector<std::uint64_t> at(dims.size());
  for (std::uint64_t row = 0; row < row_region.element_count().value(); ++row) {
    region_run_coordinate(row_region, row, &at);
    const auto x = dims[layout.width_axis];
    for (std::uint64_t i = 0; i < x.extent;) {
      if (call.cancellation.cancelled()) {
        return Status{ErrorCode::Cancelled, "planar literal fill cancelled"};
      }
      at[layout.width_axis] = x.offset + i;
      auto out = call.output.row_run(at);
      if (!out.ok()) {
        return out.status();
      }
      const auto n =
          std::min<std::uint64_t>({256, x.extent - i, out.value().samples});
      if (const auto* budget = resource_internal::metadata_budget()) {
        auto status = budget->consume({n});
        if (!status.ok()) {
          return status;
        }
      }
      fill(s, out.value().data, n);
      i += n;
    }
  }
  return Status::success();
}
}  // namespace

Result<OperationPreparation> prepare_channel_literal_like(
    const std::vector<OperationMetadata>& inputs, const alpha_ops::Params& p,
    numeric_ops::SequenceProfile profile) {
  using R = Result<OperationPreparation>;
  using alpha_ops::invalid;
  using alpha_ops::output_facets;
  using alpha_ops::shape_valid;
  using alpha_ops::source_assertion;
  using alpha_ops::text;
  auto status = numeric_ops::sequence_profile_available(profile);
  if (!status.ok()) {
    return R(status);
  }
  if (inputs.size() != 1 ||
      text(p, "expected_inputs") != source_assertion(inputs)) {
    return R(invalid(
        "literal-like requires its complete source descriptor assertion"));
  }
  status = shape_valid(inputs[0]);
  if (!status.ok()) {
    return R(status);
  }
  LiteralPreparation s;
  s.width = Value::element_size(inputs[0].descriptor.element_type);
  const auto bits = text(p, "bits");
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    return -1;
  };
  if (bits.size() != s.width * 2) {
    return R(invalid("literal width differs from dtype"));
  }
  for (std::size_t i = 0; i < s.width; ++i) {
    const auto a = hex(bits[i * 2]), b = hex(bits[i * 2 + 1]);
    if (a < 0 || b < 0) {
      return R(invalid("invalid literal hexadecimal bytes"));
    }
    s.bits[i] = static_cast<std::uint8_t>((a << 4) | b);
  }
  const auto policy = text(p, "layout");
  if (policy != "auto" && policy != "view" && policy != "materialize") {
    return R(invalid("unknown literal layout"));
  }
  s.require_view = policy == "view";
  OperationOutputSpecialization output;
  output.metadata = inputs[0];
  if (p.count("axis")) {
    const auto axis = std::get<std::int64_t>(p.at("axis"));
    const bool keep = std::get<bool>(p.at("keepdims"));
    if (axis < 0 ||
        static_cast<std::uint64_t>(axis) >= inputs[0].descriptor.shape.size() ||
        (!keep && inputs[0].descriptor.shape.size() == 1)) {
      return R(invalid("invalid literal-like axis/rank"));
    }
    if (keep) {
      output.metadata.descriptor.shape[axis] = 1;
    } else {
      output.metadata.descriptor.shape.erase(
          output.metadata.descriptor.shape.begin() + axis);
    }
    if (output.metadata.planar_layout) {
      auto& l = *output.metadata.planar_layout;
      if (!l.channel_axis ||
          *l.channel_axis != static_cast<std::uint32_t>(axis)) {
        return R(invalid("literal-like cannot erase a planar spatial axis"));
      }
      if (!keep) {
        l.channel_axis.reset();
        if (l.height_axis > axis) {
          --l.height_axis;
        }
        if (l.width_axis > axis) {
          --l.width_axis;
        }
      }
    }
  }
  if (output.metadata.planar_layout) {
    output.metadata.planar_layout->groups.clear();
  }
  auto description =
      tensor_description_from_parameter(text(p, "output_description"));
  if (!description.ok()) {
    return R(description.status());
  }
  status = validate_tensor_description(description.value(),
                                       output.metadata.descriptor);
  if (!status.ok()) {
    return R(status);
  }
  output.metadata.facets = output_facets(inputs[0], description.value());
  auto coverage = Footprint::all(output.metadata.descriptor.shape);
  if (!coverage.ok()) {
    return R(coverage.status());
  }
  DependencyMappedNeed descriptor;
  descriptor.port = 0;
  descriptor.roles = static_cast<std::uint32_t>(DependencyRole::Descriptor);
  descriptor.tags = {{1, 0}};
  output.static_dependency_pieces =
      std::vector<DependencyMapPiece>{{coverage.take_value(), {descriptor}}};
  output.regional_atomic = true;
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::make_shared<LiteralPreparation>(s);
  return R(std::move(result));
}
Status register_channel_literal_like(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("strict", numeric_ops::SequenceProfile::Strict),
        std::make_pair("accelerated_apple_silicon",
                       numeric_ops::SequenceProfile::AppleSilicon),
        std::make_pair("accelerated_x86_64",
                       numeric_ops::SequenceProfile::X86Avx2)}) {
    OperationDefinition d;
    d.key = std::string("channel.literal_like_") + profile.first;
    auto& t = d.traits;
    t.input_count = 1;
    t.input_schema.resize(1);
    t.cacheable = false;
    t.planar_storage_capable = true;
    t.planar_exact_dependencies = true;
    t.requires_metadata_specialization = true;
    t.workspace_bytes = 4096;
    for (const auto* key : {"bits", "expected_inputs", "output_description",
                            "layout", "authoring_member"}) {
      t.parameter_schema.push_back({key, OperationParameterType::String});
    }
    t.parameter_schema.push_back(
        {"axis", OperationParameterType::Int64, false});
    t.parameter_schema.push_back({"keepdims", OperationParameterType::Bool});
    auto& output = t.outputs[0];
    output.key = "values";
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {1};
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(LiteralState);
    output.maximum_dependency_stages = 2;
    d.prepare_static = [kind = profile.second](const auto& inputs,
                                               const auto& p) {
      return prepare_channel_literal_like(inputs, p, kind);
    };
    d.start_dependency = [](const DependencyQuery& q,
                            const BufferAllocator& a) {
      return DependencyContinuation::make<LiteralState>(
          a, static_cast<const LiteralPreparation*>(q.prepared->state()));
    };
    d.planar_callback = literal_planar;
    auto status = registry->register_operation(std::move(d));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
