#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "plugin/operation_exception.hpp"
#include "plugin/result_c_codec.hpp"

namespace ps::plugin_internal::result_c {
Result<std::vector<ExtensionIdentity>> extension_ids(
    const ps_result_extension_identity_v4* values, uint32_t count) {
  using Answer = Result<std::vector<ExtensionIdentity>>;
  if (!array(values, count, 64))
    return Answer(invalid("invalid extension identity array"));
  std::vector<ExtensionIdentity> result;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& value = values[i];
    if (value.struct_size != sizeof(value) || !value.version)
      return Answer(invalid("invalid extension identity"));
    auto key = text(value.key, value.key_size, 1024);
    auto implementation =
        text(value.implementation, value.implementation_size, 1024);
    if (!key.ok() || !implementation.ok())
      return Answer(!key.ok() ? key.status() : implementation.status());
    result.push_back(
        {key.take_value(), value.version, implementation.take_value()});
  }
  return Answer(std::move(result));
}
ResourceVector<ps_result_resource_view_v4> resource_views(
    const ResourceSet& source) {
  ResourceVector<ps_result_resource_view_v4> result;
  for (size_t i = 0; i < source.size(); ++i) {
    const auto resource = source.at(i).take_value();
    ps_result_resource_view_v4 view{};
    view.identity.byte_length = resource.identity().byte_length;
    std::copy(resource.identity().sha256.begin(),
              resource.identity().sha256.end(), view.identity.sha256);
    view.bytes = resource.bytes().data();
    result.push_back(view);
  }
  return result;
}
namespace {
struct ValidationBridge {
  const ValidationContext& source;
  const std::thread::id thread = std::this_thread::get_id();
  std::mutex mutex;
  Status failure;
  explicit ValidationBridge(const ValidationContext& value) : source(value) {}
  template <class F>
  static int call(void* raw, F function) noexcept {
    auto& bridge = *static_cast<ValidationBridge*>(raw);
    std::lock_guard<std::mutex> lock(bridge.mutex);
    if (!bridge.failure.ok())
      return code(bridge.failure);
    try {
      if (bridge.thread != std::this_thread::get_id())
        bridge.failure =
            invalid("validation service called from foreign thread");
      else if (bridge.source.cancellation.cancelled())
        bridge.failure = {ErrorCode::Cancelled, {}};
      else
        bridge.failure = function(bridge.source);
    } catch (...) {
      bridge.failure = current_operation_exception();
    }
    return code(bridge.failure);
  }
  static int fail_domain(void* raw, uint32_t status_code,
                         uint64_t domain_code) {
    return call(raw, [&](const auto&) {
      auto status = outcome(static_cast<int>(status_code));
      if (status.ok() || !domain_code)
        return invalid("invalid domain failure");
      status.detail.domain_code = domain_code;
      return status;
    });
  }
  static int work(void* raw, uint64_t n) {
    return call(raw, [&](const auto& s) { return s.consume(n); });
  }
  static int cancelled(void* raw) {
    return static_cast<ValidationBridge*>(raw)->source.cancellation.cancelled();
  }
  static int regions(void* raw, uint32_t slot, ps_result_region_v4* regions,
                     uint32_t capacity, uint32_t* count) {
    return call(raw, [&](const auto& s) {
      if (!array(count, 1, 1) || !array(regions, capacity, 65536))
        return invalid("invalid validation region destination");
      const auto* coverage = s.tensor_coverage(slot);
      if (!coverage)
        return invalid("unauthorized validation slot");
      auto status = s.consume(1 + coverage->boxes().size());
      if (!status.ok())
        return status;
      *count = coverage->boxes().size();
      if (!capacity)
        return Status::success();
      if (capacity < *count)
        return invalid("short validation region array");
      for (uint32_t i = 0; i < *count; ++i) {
        const auto& box = coverage->boxes()[i];
        regions[i] = {};
        regions[i].struct_size = sizeof(regions[i]);
        regions[i].rank = box.rank();
        for (size_t axis = 0; axis < box.rank(); ++axis) {
          regions[i].offset[axis] = box.dimensions()[axis].offset;
          regions[i].extent[axis] = box.dimensions()[axis].extent;
        }
      }
      return Status::success();
    });
  }
  static int range(void* raw, uint32_t field, uint64_t* first, uint64_t* rows) {
    return call(raw, [&](const auto& s) {
      if (!array(first, 1, 1) || !array(rows, 1, 1))
        return invalid("invalid validation field range");
      auto value = s.field_range(field);
      if (!value.ok())
        return value.status();
      *first = value.value().first;
      *rows = value.value().second;
      return Status::success();
    });
  }
  static int field(void* raw, uint32_t slot, uint64_t first, uint64_t rows,
                   uint8_t* output, uint32_t bytes) {
    return call(raw, [&](const auto& s) {
      if (!array(output, bytes, 65536) || !bytes)
        return invalid("invalid validation read buffer");
      auto value = s.read_field(slot, first, rows);
      if (!value.ok())
        return value.status();
      if (value.value()->bytes().size() != bytes)
        return invalid("validation read size mismatch");
      auto status = s.consume(bytes);
      if (!status.ok())
        return status;
      std::memcpy(output, value.value()->bytes().data(), bytes);
      return Status::success();
    });
  }
  static int tensor(void* raw, uint32_t slot, const ps_result_region_v4* input,
                    uint8_t* output, uint32_t bytes) {
    return call(raw, [&](const auto& s) {
      if (!array(input, 1, 1) || input->struct_size != sizeof(*input) ||
          input->rank < 1 || input->rank > 8 || !array(output, bytes, 65536) ||
          !bytes || slot >= s.schema.tensors.size())
        return invalid("invalid validation tensor read");
      std::vector<RegionDimension> dimensions;
      for (uint32_t i = 0; i < input->rank; ++i)
        dimensions.push_back({input->offset[i], input->extent[i]});
      Region region(std::move(dimensions));
      auto count = region.element_count();
      const auto width =
          Value::element_size(s.schema.tensors[slot].descriptor.element_type);
      if (!count.ok() || count.value() != bytes / width || bytes % width)
        return invalid("validation tensor size mismatch");
      auto window = s.acquire_tensor(slot, region);
      if (!window.ok())
        return window.status();
      std::vector<uint64_t> at(input->offset, input->offset + input->rank);
      for (uint64_t i = 0; i < count.value(); ++i) {
        if (s.cancellation.cancelled())
          return Status{ErrorCode::Cancelled, {}};
        auto charged = s.consume(1);
        if (!charged.ok())
          return charged;
        auto run = window.value().row_run(at);
        if (!run.ok())
          return run.status();
        std::memcpy(output + i * width, run.value().data, width);
        for (size_t axis = at.size(); axis-- > 0;) {
          if (++at[axis] - input->offset[axis] < input->extent[axis])
            break;
          at[axis] = input->offset[axis];
        }
      }
      return Status::success();
    });
  }
};
struct PreparedState {
  std::shared_ptr<void> library;
  ps_result_validation_definition_v4 api;
  void* state = nullptr;
  ~PreparedState() noexcept {
    if (state) {
      try {
        api.destroy_prepared(api.user_data, state);
      } catch (...) {
      }
    }
  }
};
}  // namespace
Result<ValidationDefinition> validation_definition(
    const ps_result_validation_definition_v4& api,
    std::shared_ptr<void> library) {
  using Answer = Result<ValidationDefinition>;
  if (api.struct_size != sizeof(api))
    return Answer(invalid("invalid extension definition size"));
  auto ids = extension_ids(&api.identity, 1);
  if (!ids.ok() || !api.prepare || !api.destroy_prepared ||
      api.requires_complete > 1)
    return Answer(invalid("invalid validation definition"));
  ValidationDefinition result;
  result.owner = library;
  result.identity = ids.take_value()[0];
  result.scratch_bytes = api.scratch_bytes;
  result.requires_complete = api.requires_complete;
  result.prepare = [api, library](const ValidationPreparation& source)
      -> Result<std::shared_ptr<const void>> {
    using StateAnswer = Result<std::shared_ptr<const void>>;
    // Allocate the destructor owner before C can return any state, even on
    // failure.
    auto state = std::make_shared<PreparedState>();
    state->library = library;
    state->api = api;
    OperationMetadata metadata{
        std::shared_ptr<const SchemaTemplate>(&source.schema, [](auto*) {})};
    MetadataView schema(&metadata, 1);
    if (!schema.status.ok())
      return StateAnswer(schema.status);
    auto parameters = parameters_view(source.parameters);
    auto resources = resource_views(source.resources);
    ps_result_validation_preparation_v4 view{
        sizeof(view),
        source.output,
        source.port,
        schema.ports[0].schema,
        parameters.empty() ? nullptr : parameters.data(),
        static_cast<uint32_t>(parameters.size()),
        resources.empty() ? nullptr : resources.data(),
        static_cast<uint32_t>(resources.size())};
    auto status = outcome(api.prepare(api.user_data, &view, &state->state));
    return status.ok() ? StateAnswer(std::move(state)) : StateAnswer(status);
  };
  if (api.validate) {
    result.validate = [api, library](const void* raw,
                                     const ValidationContext& source) {
      const auto& state = *static_cast<const PreparedState*>(raw);
      OperationMetadata metadata{
          std::shared_ptr<const SchemaTemplate>(&source.schema, [](auto*) {})};
      MetadataView schema(&metadata, 1);
      if (!schema.status.ok())
        return schema.status;
      ValidationBridge bridge(source);
      ps_result_validation_context_v4 view{sizeof(view),
                                           source.complete,
                                           schema.ports[0].schema,
                                           &bridge,
                                           ValidationBridge::work,
                                           ValidationBridge::cancelled,
                                           ValidationBridge::regions,
                                           ValidationBridge::range,
                                           ValidationBridge::tensor,
                                           ValidationBridge::field,
                                           ValidationBridge::fail_domain};
      Status status;
      try {
        status = outcome(api.validate(api.user_data, state.state, &view));
      } catch (...) {
        status = current_operation_exception();
      }
      return bridge.failure.ok() ? status : bridge.failure;
    };
  }
  return Answer(std::move(result));
}
Result<DiagnosticDefinition> diagnostic_definition(
    const ps_result_diagnostic_definition_v4& api,
    std::shared_ptr<void> library) {
  using Answer = Result<DiagnosticDefinition>;
  if (api.struct_size != sizeof(api))
    return Answer(invalid("invalid extension definition size"));
  auto ids = extension_ids(&api.identity, 1);
  if (!ids.ok() || !api.validate || !api.merge)
    return Answer(invalid("invalid diagnostic definition"));
  DiagnosticDefinition result;
  result.owner = library;
  result.identity = ids.take_value()[0];
  result.payload_bytes = api.payload_bytes;
  result.scratch_bytes = api.scratch_bytes;
  result.validate = [api, library](ByteView bytes) {
    return outcome(api.validate(api.user_data, bytes.data(), bytes.size()));
  };
  result.merge = [api, library](ByteView prior, ByteView report,
                                DiagnosticDestination output) {
    return outcome(api.merge(api.user_data, prior.data(), report.data(),
                             output.data, output.size));
  };
  return Answer(std::move(result));
}
}  // namespace ps::plugin_internal::result_c
