#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/sequence_profiles.hpp"
#include "02-format-color/result_mapping.hpp"
#include "photospider/data/tensor_description.hpp"
#include "plugin/builtin_operations.hpp"

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
using format_result::require;
using format_result::take;
using Poll = Result<ResultProgramPoll>;
Status invalid(const std::string& message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
ResultBuilder builder(const ResultProgramPhase& phase, bool empty) {
  auto result = take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{},
      phase.query.tile_height, phase.query.tile_width, phase.query.resources));
  require(result.bind_descriptor_relation(take(ResultRelation::cartesian(
      phase.resources, 1,
      {0, 8, 0, empty ? 0U : 1U, ResultSupportTarget::Descriptor, 0}))));
  return result;
}
Poll empty_result(const ResultProgramPhase& phase) try {
  auto result = builder(phase, true);
  return Poll(ResultPublication{take(result.seal()), true});
} catch (const Status& status) {
  return Poll(status);
}
struct LiteralState final {
  LiteralPreparation state;
  bool requested = false;
  explicit LiteralState(LiteralPreparation value) : state(value) {}
  Poll poll(const ResultProgramPhase& phase) try {
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0,
           take(Footprint::none(
               phase.query.inputs[0].result_schema->tensors[0].sample_shape())),
           8});
      return Poll(std::move(need));
    }
    if (state.require_view)
      return Poll(unavailable());
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(8192, 8192)));
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    const auto output = phase.query.tensor_outputs
                            ? *phase.query.tensor_outputs
                            : take(Footprint::all(tensor.sample_shape()));
    auto result = builder(phase, false);
    const auto relation = take(ResultRelation::cartesian(
        phase.resources, take(tensor.sample_count()),
        {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0}));
    require(format_result::planes(tensor, output, [&](const Region& box) {
      return result.publish_tensor_kernel(
          0, box,
          [&](const auto& writers) {
            for (const auto& writer : writers) {
              const auto& dims = writer.region().dimensions();
              const auto axis = writer.sample_axis();
              std::vector<std::uint64_t> at;
              for (auto dim : dims)
                at.push_back(dim.offset);
              for (;;) {
                if (phase.query.cancellation.cancelled())
                  return Status{ErrorCode::Cancelled, "literal fill cancelled"};
                auto run = writer.row_run(at);
                if (!run.ok())
                  return run.status();
                const auto n =
                    std::min<std::uint64_t>(256, run.value().samples);
                auto status = phase.consume_work(n);
                if (!status.ok())
                  return status;
                if (run.value().sample_stride_bytes ==
                    static_cast<std::int64_t>(state.width)) {
                  fill(state, run.value().data, n);
                } else {
                  for (std::uint64_t i = 0; i < n; ++i)
                    std::memcpy(
                        run.value().data + static_cast<std::ptrdiff_t>(
                                               static_cast<__int128>(i) *
                                               run.value().sample_stride_bytes),
                        state.bits.data(), state.width);
                }
                at[axis] += n;
                if (at[axis] < dims[axis].offset + dims[axis].extent)
                  continue;
                at[axis] = dims[axis].offset;
                bool next = false;
                for (std::size_t i = dims.size(); i;) {
                  --i;
                  if (i == axis)
                    continue;
                  if (++at[i] < dims[i].offset + dims[i].extent) {
                    next = true;
                    break;
                  }
                  at[i] = dims[i].offset;
                }
                if (!next)
                  break;
              }
            }
            return Status::success();
          },
          relation, {true, true, true, true}, phase.query.cancellation);
    }));
    return Poll(ResultPublication{take(result.seal()), true});
  } catch (const Status& status) {
    return Poll(status);
  }
};
}  // namespace

Result<OperationPreparation> prepare_channel_literal_like(
    const std::vector<OperationMetadata>& inputs,
    const format_result::Params& p, numeric_ops::SequenceProfile profile) try {
  using R = Result<OperationPreparation>;
  using format_result::source_assertion;
  using format_result::text;
  auto status = numeric_ops::sequence_profile_available(profile);
  if (!status.ok()) {
    return R(status);
  }
  if (inputs.size() != 1 ||
      text(p, "expected_inputs") != source_assertion(inputs)) {
    return R(invalid(
        "literal-like requires its complete source descriptor assertion"));
  }
  status = tensor_ops::check_tensor(inputs[0]);
  if (!status.ok()) {
    return R(status);
  }
  const auto& input = inputs[0].result_schema->tensors[0];
  auto count = input.sample_count();
  if (!count.ok())
    return R(count.status());
  if (!count.value() || count.value() > (UINT64_C(1) << 40))
    return R(invalid("logical count is outside [1,2^40]"));
  LiteralPreparation s;
  s.width = Value::element_size(input.descriptor.element_type);
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
  auto schema = *inputs[0].result_schema;
  auto& tensor = schema.tensors[0];
  if (p.count("axis")) {
    const auto axis = std::get<std::int64_t>(p.at("axis"));
    const bool keep = std::get<bool>(p.at("keepdims"));
    if (axis < 0 ||
        static_cast<std::uint64_t>(axis) >= input.descriptor.shape.size() ||
        (!keep && input.descriptor.shape.size() == 1)) {
      return R(invalid("invalid literal-like axis/rank"));
    }
    if (keep) {
      tensor.descriptor.shape[axis] = 1;
    } else {
      if (static_cast<std::uint64_t>(axis) >=
          input.descriptor.shape.size() - tensor.atomic_trailing_axes)
        --tensor.atomic_trailing_axes;
      tensor.descriptor.shape.erase(tensor.descriptor.shape.begin() + axis);
    }
    if (tensor.layout.spatial) {
      auto& l = tensor.layout;
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
  if (tensor.layout.spatial) {
    tensor.layout.groups.clear();
  }
  auto description =
      tensor_description_from_parameter(text(p, "output_description"));
  if (!description.ok()) {
    return R(description.status());
  }
  status = validate_tensor_description(description.value(), tensor.descriptor);
  if (!status.ok()) {
    return R(status);
  }
  tensor.facets.erase(
      std::remove_if(tensor.facets.begin(), tensor.facets.end(),
                     [](const auto& f) {
                       return f.key == "photospider.tensor-description" ||
                              f.key == "photospider.semantic" ||
                              f.key == "photospider.image" ||
                              f.key == "photospider.color-array";
                     }),
      tensor.facets.end());
  auto facet = encode_tensor_description(description.value());
  if (!facet.ok())
    return R(facet.status());
  tensor.facets.push_back(facet.take_value());
  std::sort(tensor.facets.begin(), tensor.facets.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::make_shared<LiteralPreparation>(s);
  return R(std::move(result));
} catch (const Status& status) {
  return Result<OperationPreparation>(status);
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
    OperationPortConstraint port;
    port.kind = OperationPortKind::Result;
    port.element_type_mask = 127;
    t.input_schema = {port};
    t.cacheable = false;
    t.requires_metadata_specialization = true;
    for (const auto* key : {"bits", "expected_inputs", "output_description",
                            "layout", "authoring_member"}) {
      t.parameter_schema.push_back({key, OperationParameterType::String});
    }
    t.parameter_schema.push_back(
        {"axis", OperationParameterType::Int64, false});
    t.parameter_schema.push_back({"keepdims", OperationParameterType::Bool});
    auto& output = t.outputs[0];
    output.key = "values";
    output.output_schema = port;
    output.result_schema = tensor_ops::scalar_schema();
    output.region_rule = OperationRegionRule::Dependency;
    output.continuation_bytes = sizeof(LiteralState);
    output.maximum_dependency_stages = 2;
    d.prepare_static = [kind = profile.second](const auto& inputs,
                                               const auto& p) {
      return prepare_channel_literal_like(inputs, p, kind);
    };
    d.start_result = [](const ResultProgramQuery& q, const BufferAllocator& a) {
      if (q.tensor_outputs && q.tensor_outputs->empty())
        return ResultContinuation::stateless<empty_result>();
      return ResultContinuation::make<LiteralState>(
          a, *static_cast<const LiteralPreparation*>(q.prepared->state()));
    };
    auto status = registry->register_operation(std::move(d));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
