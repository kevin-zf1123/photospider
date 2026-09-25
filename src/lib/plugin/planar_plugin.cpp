#include "plugin/planar_plugin.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps::plugin_internal {
namespace {
Status result(int code, const char* message) {
  ErrorCode kind = ErrorCode::OperationFailed;
  switch (code) {
    case 0:
      return Status::success();
    case 2:
      kind = ErrorCode::Cancelled;
      break;
    case 3:
      kind = ErrorCode::BackendUnavailable;
      break;
    case 4:
      kind = ErrorCode::ResourceExhausted;
      break;
    case 5:
      kind = ErrorCode::TypeMismatch;
      break;
    case 6:
      kind = ErrorCode::InvalidArgument;
      break;
  }
  return Status::failure(kind, message[0] ? message : "planar plugin failure");
}
using Facets = std::vector<ps_operation_facet_view_v9>;
ps_planar_metadata_v1 metadata(const ValueDescriptor& d,
                               const std::vector<ValueFacet>& facets,
                               const PlanarImageLayout& layout,
                               Facets* storage) {
  for (const auto& f : facets) {
    storage->push_back({sizeof(ps_operation_facet_view_v9), f.key.data(),
                        static_cast<uint32_t>(f.key.size()), f.version,
                        f.payload.data(),
                        static_cast<uint32_t>(f.payload.size())});
  }
  ps_planar_metadata_v1 out{};
  out.struct_size = sizeof(out);
  out.element_type = static_cast<uint32_t>(d.element_type);
  out.rank = static_cast<uint32_t>(d.shape.size());
  std::copy(d.shape.begin(), d.shape.end(), out.shape);
  out.facets = storage->data();
  out.facet_count = static_cast<uint32_t>(storage->size());
  out.order = static_cast<uint32_t>(layout.order);
  out.height_axis = layout.height_axis;
  out.width_axis = layout.width_axis;
  out.channel_axis = layout.channel_axis.value_or(UINT32_MAX);
  out.row_pitch_bytes = layout.row_pitch_bytes;
  return out;
}
std::vector<ps_operation_parameter_value_v9> parameters(
    const std::map<std::string, ParameterValue>& source) {
  std::vector<ps_operation_parameter_value_v9> out;
  for (const auto& [key, value] : source) {
    ps_operation_parameter_value_v9 p{};
    p.struct_size = sizeof(p);
    p.key = key.data();
    p.key_size = key.size();
    if (auto* v = std::get_if<std::int64_t>(&value)) {
      p.type = PS_OPERATION_PARAMETER_INT64_V9;
      p.int64_value = *v;
    } else if (auto* v = std::get_if<double>(&value)) {
      p.type = PS_OPERATION_PARAMETER_FLOAT64_V9;
      p.float64_value = *v;
    } else if (auto* v = std::get_if<bool>(&value)) {
      p.type = PS_OPERATION_PARAMETER_BOOL_V9;
      p.bool_value = *v;
    } else {
      const auto& text = std::get<std::string>(value);
      p.type = PS_OPERATION_PARAMETER_STRING_V9;
      p.string_value = text.data();
      p.string_size = text.size();
    }
    out.push_back(p);
  }
  return out;
}
struct Services {
  const PlanarOperationInvocation& call;
  Status failure = Status::success();
  std::map<uint8_t*, MutableBuffer> scratch;
  int fail(Status s) {
    if (failure.ok()) {
      failure = std::move(s);
    }
    return 0;
  }
  template <class F>
  int guard(F fn) noexcept {
    try {
      return fn();
    } catch (const std::bad_alloc&) {
      return fail(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return fail(Status{ErrorCode::OperationFailed, {}});
    }
  }
};
int read_row(void* p, uint32_t port, const uint64_t* at, uint32_t rank,
             const uint8_t** bytes, uint64_t* samples) noexcept {
  auto& s = *static_cast<Services*>(p);
  return s.guard([&] {
    if (!at || !bytes || !samples || port >= s.call.inputs.size() ||
        rank != s.call.inputs[port].descriptor().shape.size()) {
      return s.fail(Status{ErrorCode::InvalidArgument, "invalid planar read"});
    }
    auto row = s.call.inputs[port].row_run({at, at + rank});
    if (!row.ok()) {
      return s.fail(row.status());
    }
    *bytes = row.value().data;
    *samples = row.value().samples;
    return 1;
  });
}
int write_row(void* p, const uint64_t* at, uint32_t rank, uint8_t** bytes,
              uint64_t* samples) noexcept {
  auto& s = *static_cast<Services*>(p);
  return s.guard([&] {
    if (!at || !bytes || !samples || rank != s.call.output_region.rank()) {
      return s.fail(Status{ErrorCode::InvalidArgument, "invalid planar write"});
    }
    auto row = s.call.output.row_run({at, at + rank});
    if (!row.ok()) {
      return s.fail(row.status());
    }
    *bytes = row.value().data;
    *samples = row.value().samples;
    return 1;
  });
}
uint8_t* allocate(void* p, uint64_t bytes) noexcept {
  auto& s = *static_cast<Services*>(p);
  uint8_t* pointer = nullptr;
  s.guard([&] {
    if (s.scratch.size() >= 4096 || bytes == 0) {
      return s.fail(
          Status{ErrorCode::ResourceExhausted, "planar scratch slots"});
    }
    auto buffer = s.call.allocator.allocate(bytes);
    if (!buffer.ok()) {
      return s.fail(buffer.status());
    }
    auto owned = buffer.take_value();
    pointer = owned.data();
    s.scratch.emplace(pointer, std::move(owned));
    return 1;
  });
  return s.failure.ok() ? pointer : nullptr;
}
int release(void* p, uint8_t* pointer) noexcept {
  auto& s = *static_cast<Services*>(p);
  return s.guard([&] {
    if (!pointer || !s.scratch.erase(pointer)) {
      return s.fail(
          Status{ErrorCode::InvalidArgument, "unknown scratch pointer"});
    }
    return 1;
  });
}
int cancelled(void* p) noexcept {
  return static_cast<Services*>(p)->call.cancellation.cancelled();
}
}  // namespace
Status prepare_planar_plugin(OperationDefinition* definition,
                             const ps_planar_operation_v1& entry, void* user,
                             std::shared_ptr<void> library) {
  auto& t = definition->traits;
  if (entry.struct_size != sizeof(entry) || !entry.infer || !entry.execute ||
      t.outputs.size() != 1 || t.supports_gpu || t.input_count == 0 ||
      t.outputs[0].region_rule != OperationRegionRule::Whole ||
      t.outputs[0].dependency_version || t.repeated_maximum ||
      t.outputs[0].input_indices ||
      t.outputs[0].output_schema.kind != OperationPortKind::Value ||
      t.outputs[0].maximum_output_payload_bytes ||
      t.outputs[0].preserve_output_views || t.outputs[0].requires_input_views) {
    return Status{ErrorCode::InvalidArgument,
                  "invalid planar extension record"};
  }
  for (const auto& port : t.input_schema) {
    if (port.kind != OperationPortKind::Value) {
      return Status{ErrorCode::InvalidArgument,
                    "planar extension needs Value ports"};
    }
  }
  t.planar_storage_capable = true;
  t.requires_metadata_specialization = true;
  t.outputs[0].requires_dense_output = false;
  t.outputs[0].planar_layout = PlanarImageLayout{};
  definition->specialize_metadata =
      [entry, user, library](const std::vector<OperationMetadata>& inputs,
                             const auto& params)
      -> Result<std::vector<OperationOutputSpecialization>> {
    std::vector<Facets> facets(inputs.size());
    std::vector<ps_planar_metadata_v1> views;
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (!inputs[i].planar_layout) {
        return Result<std::vector<OperationOutputSpecialization>>(Status{
            ErrorCode::TypeMismatch, "planar plugin requires image storage"});
      }
      views.push_back(metadata(inputs[i].descriptor, inputs[i].facets,
                               *inputs[i].planar_layout, &facets[i]));
    }
    auto pp = parameters(params);
    ps_planar_metadata_v1 output{};
    output.struct_size = sizeof(output);
    char diagnostic[512]{};
    input_internal::Float32Environment environment;
    if (!environment.active()) {
      return Result<std::vector<OperationOutputSpecialization>>(Status{
          ErrorCode::OperationFailed, "floating environment unavailable"});
    }
    const int code =
        entry.infer(user, views.data(), views.size(), pp.data(), pp.size(),
                    &output, diagnostic, sizeof(diagnostic));
    diagnostic[sizeof(diagnostic) - 1] = 0;
    auto status = result(code, diagnostic);
    if (!status.ok()) {
      return Result<std::vector<OperationOutputSpecialization>>(status);
    }
    if (output.struct_size != sizeof(output) || output.rank < 2 ||
        output.rank > 3 || output.order > 1 || output.facet_count > 64 ||
        ((output.facet_count == 0) != (output.facets == nullptr)) ||
        (output.facets && reinterpret_cast<uintptr_t>(output.facets) %
                              alignof(ps_operation_facet_view_v9)) ||
        output.element_type < 1 || output.element_type > 7) {
      return Result<std::vector<OperationOutputSpecialization>>(
          Status{ErrorCode::TypeMismatch, "invalid planar output metadata"});
    }
    OperationOutputSpecialization out;
    out.metadata.descriptor = {static_cast<ElementType>(output.element_type),
                               {output.shape, output.shape + output.rank}};
    PlanarImageLayout layout;
    layout.order = static_cast<ImagePlaneOrder>(output.order);
    layout.height_axis = output.height_axis;
    layout.width_axis = output.width_axis;
    layout.channel_axis = output.channel_axis == UINT32_MAX
                              ? std::nullopt
                              : std::optional<uint32_t>(output.channel_axis);
    layout.row_pitch_bytes = output.row_pitch_bytes;
    auto valid = PlanarImage::validate_layout(out.metadata.descriptor, layout);
    if (!valid.ok()) {
      return Result<std::vector<OperationOutputSpecialization>>(valid);
    }
    out.metadata.planar_layout = layout;
    uint64_t facet_bytes = 0;
    for (uint32_t i = 0; i < output.facet_count; ++i) {
      const auto& f = output.facets[i];
      if (f.struct_size != sizeof(f) || !f.key || !f.key_size ||
          f.key_size > 1024 || f.payload_size > 65536 ||
          (facet_bytes += f.key_size + f.payload_size) > 1048576 ||
          (f.payload_size && !f.payload)) {
        return Result<std::vector<OperationOutputSpecialization>>(
            Status{ErrorCode::TypeMismatch, "invalid planar output facet"});
      }
      ValueFacet copied{std::string(f.key, f.key_size), f.version, {}};
      if (f.payload_size) {
        copied.payload.assign(f.payload, f.payload + f.payload_size);
      }
      out.metadata.facets.push_back(std::move(copied));
    }
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(out)});
  };
  definition->planar_callback = [entry, user, library](
                                    const PlanarOperationInvocation& call) {
    std::vector<Facets> facets(call.inputs.size());
    std::vector<ps_planar_metadata_v1> views;
    for (size_t i = 0; i < call.inputs.size(); ++i) {
      const auto& in = call.inputs[i];
      const auto& c = in.config();
      views.push_back(metadata(in.descriptor(), in.facets(),
                               {c.order, c.height_axis, c.width_axis,
                                c.channel_axis, c.row_pitch_bytes, c.groups},
                               &facets[i]));
    }
    auto pp = parameters(call.parameters);
    Facets output_facets;
    const auto& resolved = call.output_metadata;
    auto output = metadata(resolved.descriptor, resolved.facets,
                           *resolved.planar_layout, &output_facets);
    char diagnostic[512]{};
    Services state{call, Status::success(), {}};
    const ps_planar_services_v1 services{sizeof(services), &state,   read_row,
                                         write_row,        allocate, release,
                                         cancelled};
    const int code =
        entry.execute(user, views.data(), views.size(), pp.data(), pp.size(),
                      &output, &services, diagnostic, sizeof(diagnostic));
    diagnostic[sizeof(diagnostic) - 1] = 0;
    if (!state.failure.ok()) {
      return state.failure;
    }
    return result(code, diagnostic);
  };
  return Status::success();
}
}  // namespace ps::plugin_internal
