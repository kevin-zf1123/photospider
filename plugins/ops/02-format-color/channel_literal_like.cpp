#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/sequence_profiles.hpp"
#include "02-format-color/result_mapping.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "photospider/core/status_helpers.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps::plugin_internal {
namespace {
struct LiteralPreparation final {
  std::array<std::uint8_t, 8> bits{};
  std::size_t width = 0;
  bool require_view = false;
};
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
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task("fill", "literal-bits-v1");
      if (!task.ok())
        return Answer(task.status());
      auto status = builder.read_input_schema(task.value(), 0);
      if (status.ok())
        status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto cancelled = [&] {
    return invocation->cancellation.is_cancelled &&
           invocation->cancellation.is_cancelled(
               invocation->cancellation.context);
  };
  if (cancelled()) {
    *status = kernel_failure(ErrorCode::Cancelled);
    return;
  }
  const auto& state =
      *static_cast<const LiteralPreparation*>(invocation->prepared_context);
  if (state.require_view) {
    *status = kernel_failure(
        ErrorCode::InvalidArgument,
        "ViewUnavailable: absent opaque pixels require generation",
        FailureReason::InvalidDomain);
    return;
  }
  const auto& output = invocation->outputs[0];
  std::uint64_t samples = 1;
  for (unsigned axis = 0; axis < output.layout->rank; ++axis)
    samples *= output.layout->shape[axis];
  auto* destination =
      static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
  // The physical Plan proves dense positive output strides. A constant fills
  // the complete logical tile linearly, independent of its schema axis order.
  // Poll between at most 256 bitwise writes, including very long rows.
  for (std::uint64_t done = 0; done < samples;) {
    if (cancelled()) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    const auto count = std::min<std::uint64_t>(256, samples - done);
    fill(state, destination + done * state.width, count);
    done += count;
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid literal fill signature"});
  };
  if (!query.static_state || query.inputs.size() != 1 ||
      query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.reads.empty() || !query.task_parameters.empty() ||
      query.scratch != std::array<std::uint64_t, 3>{})
    return invalid();
  const auto& write = query.writes[0];
  const auto& schema = *query.outputs[0].result_schema;
  auto grid = TileGrid::from_schema(schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto region = grid.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type != schema.tensors[0].descriptor.element_type ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = new LiteralPreparation(
      *static_cast<const LiteralPreparation*>(query.static_state));
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const LiteralPreparation*>(context);
  };
  return Answer(kernel);
}
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
    return R(core_internal::invalid_schema_domain(
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
    return R(core_internal::invalid_schema_domain(
        "logical count is outside [1,2^40]"));
  LiteralPreparation s;
  s.width = TensorSpan::element_size(input.descriptor.element_type);
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
    return R(core_internal::invalid_schema_domain(
        "literal width differs from dtype"));
  }
  for (std::size_t i = 0; i < s.width; ++i) {
    const auto a = hex(bits[i * 2]), b = hex(bits[i * 2 + 1]);
    if (a < 0 || b < 0) {
      return R(core_internal::invalid_schema_domain(
          "invalid literal hexadecimal bytes"));
    }
    s.bits[i] = static_cast<std::uint8_t>((a << 4) | b);
  }
  const auto policy = text(p, "layout");
  if (policy != "auto" && policy != "view" && policy != "materialize") {
    return R(core_internal::invalid_schema_domain("unknown literal layout"));
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
      return R(core_internal::invalid_schema_domain(
          "invalid literal-like axis/rank"));
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
        return R(core_internal::invalid_schema_domain(
            "literal-like cannot erase a planar spatial axis"));
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

    port.element_type_mask = 127;
    t.input_schema = {port};
    t.requires_metadata_specialization = true;
    for (const auto* key :
         {"bits", "expected_inputs", "layout", "authoring_member"}) {
      t.parameter_schema.push_back({key, OperationParameterType::String});
    }
    t.parameter_schema.push_back(
        {"axis", OperationParameterType::Int64, false});
    t.parameter_schema.push_back({"keepdims", OperationParameterType::Bool});
    auto& output = t.outputs[0];
    output.key = "values";
    output.output_schema = port;
    output.result_schema = tensor_ops::scalar_schema();
    d.prepare_static = [kind = profile.second](const auto& inputs,
                                               const auto& p) {
      return prepare_channel_literal_like(inputs, p, kind);
    };
    d.lower = lower;
    d.prepare_kernel =
        [](const physical::KernelDesc& kernel,
           const physical::Placement& placement, const KernelPreparation& query,
           const ResourceSet&,
           const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
      if (kernel.symbol != "fill" || kernel.contract != "literal-bits-v1")
        return Result<ps_prepared_kernel_v8>(
            {ErrorCode::TypeMismatch, "unknown literal fill kernel"});
      if (placement.backend != physical::Backend::Cpu ||
          placement.domain != physical::Domain::Host)
        return Result<ps_prepared_kernel_v8>(
            {ErrorCode::BackendUnavailable, "literal fill requires CPU Host"});
      return prepare_kernel(query);
    };
    auto status = registry->register_operation(std::move(d));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
