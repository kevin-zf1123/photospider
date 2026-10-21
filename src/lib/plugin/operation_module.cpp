#include "plugin/operation_module.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/callback_environment.hpp"
#include "photospider/core/utf8_validation.hpp"

namespace ps::plugin_internal {
namespace {
// Module records carry these values without changing their numeric encoding.
static_assert(
    static_cast<std::uint32_t>(ResultPublicationPolicy::CompleteBundle) ==
    PS_SCHEMA_COMPLETE_BUNDLE_V8);
static_assert(
    static_cast<std::uint32_t>(ResultPublicationPolicy::IndependentChunks) ==
    PS_SCHEMA_INDEPENDENT_CHUNKS_V8);
static_assert(
    static_cast<std::uint32_t>(ResultPublicationPolicy::StablePrefix) ==
    PS_SCHEMA_STABLE_PREFIX_V8);
static_assert(static_cast<std::uint32_t>(OperationParameterType::Int64) ==
              PS_PARAMETER_INT64_V8);
static_assert(static_cast<std::uint32_t>(OperationParameterType::Float64) ==
              PS_PARAMETER_FLOAT64_V8);
static_assert(static_cast<std::uint32_t>(OperationParameterType::Bool) ==
              PS_PARAMETER_BOOL_V8);
static_assert(static_cast<std::uint32_t>(OperationParameterType::String) ==
              PS_PARAMETER_STRING_V8);
static_assert(static_cast<std::uint32_t>(physical::Backend::Cpu) ==
              PS_BACKEND_CPU_V8);
static_assert(static_cast<std::uint32_t>(physical::Backend::Gpu) ==
              PS_BACKEND_GPU_V8);

static_assert(PS_ELEMENT_MASK_UINT8_V8 ==
              (UINT32_C(1) << (PS_ELEMENT_UINT8_V8 - 1)));
static_assert(PS_ELEMENT_MASK_INT64_V8 ==
              (UINT32_C(1) << (PS_ELEMENT_INT64_V8 - 1)));
static_assert(PS_ELEMENT_MASK_FLOAT64_V8 ==
              (UINT32_C(1) << (PS_ELEMENT_FLOAT64_V8 - 1)));
static_assert(PS_ELEMENT_MASK_FLOAT32_V8 ==
              (UINT32_C(1) << (PS_ELEMENT_FLOAT32_V8 - 1)));
static_assert(PS_ELEMENT_MASK_INT8_V8 ==
              (UINT32_C(1) << (PS_ELEMENT_INT8_V8 - 1)));
static_assert(PS_ELEMENT_MASK_UINT16_V8 ==
              (UINT32_C(1) << (PS_ELEMENT_UINT16_V8 - 1)));
static_assert(PS_ELEMENT_MASK_INT16_V8 ==
              (UINT32_C(1) << (PS_ELEMENT_INT16_V8 - 1)));
static_assert(PS_ELEMENT_MASK_ALL_V8 == UINT32_C(127));

struct Failure {
  Status status;
};
void require(bool valid, const char* message) {
  if (!valid)
    throw Failure{{ErrorCode::InvalidArgument, message}};
}
template <class T>
T take(Result<T> value) {
  if (!value.ok())
    throw Failure{value.status()};
  return value.take_value();
}
void check(Status status) {
  if (!status.ok())
    throw Failure{std::move(status)};
}
Status exception_status() noexcept {
  try {
    throw;
  } catch (Failure& failure) {
    return std::move(failure.status);
  } catch (const std::bad_alloc&) {
    return {ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit};
  } catch (...) {
    return {ErrorCode::OperationFailed, {}, FailureReason::HostException};
  }
}
template <class F>
Status callback_status(F&& call) {
  try {
    return kernel_status(call());
  } catch (...) {
    return exception_status();
  }
}
template <class T>
bool array(const T* data, std::uint64_t count, std::uint64_t limit) {
  return count <= limit && ((data == nullptr) == (count == 0)) &&
         (!data || reinterpret_cast<std::uintptr_t>(data) % alignof(T) == 0);
}
template <class T>
bool pointer(const T* data) {
  return data && reinterpret_cast<std::uintptr_t>(data) % alignof(T) == 0;
}
std::string text(ps_string_v8 value, bool optional = false,
                 std::uint32_t limit = 128) {
  require(array(value.data, value.size, limit) && (optional || value.size),
          "invalid module string");
  if (!value.size)
    return {};
  std::string result(value.data, value.size);
  require(core_internal::valid_utf8_key(result), "invalid module UTF-8 key");
  return result;
}
ps_string_v8 view(const std::string& value) {
  return {value.empty() ? nullptr : value.data(),
          static_cast<std::uint32_t>(value.size())};
}
template <class T, class Allocator>
const T* data(const std::vector<T, Allocator>& values) {
  return values.empty() ? nullptr : values.data();
}
SchemaTemplate schema(const ps_schema_v8* input) {
  require(pointer(input) && input->struct_size == sizeof(*input),
          "invalid module schema header");
  require(array(input->tensors, input->tensor_count, 16) &&
              input->tensor_count &&
              array(input->scopes, input->scope_count, 16),
          "invalid module schema arrays");
  SchemaTemplate result;
  result.id = text(input->id);
  result.version = input->version;
  for (unsigned i = 0; i < input->tensor_count; ++i) {
    const auto& source = input->tensors[i];
    require(source.struct_size == sizeof(source) && source.rank >= 1 &&
                source.rank <= 8 && source.batch_rank <= 8 - source.rank &&
                source.spatial <= 1,
            "invalid module tensor specification");
    ResultTensorSpec tensor;
    tensor.key = text(source.key);
    tensor.descriptor.element_type =
        static_cast<ElementType>(source.element_type);
    tensor.descriptor.shape.assign(source.shape, source.shape + source.rank);
    tensor.batch_axes.assign(source.batch_shape,
                             source.batch_shape + source.batch_rank);
    tensor.atomic_trailing_axes = source.atomic_trailing_axes;
    tensor.layout.spatial = source.spatial != 0;
    tensor.layout.height_axis = source.height_axis;
    tensor.layout.width_axis = source.width_axis;
    tensor.layout.channel_axis = source.channel_axis == UINT32_MAX
                                     ? std::optional<std::uint32_t>{}
                                     : source.channel_axis;
    result.tensors.push_back(std::move(tensor));
  }
  for (unsigned i = 0; i < input->scope_count; ++i) {
    const auto& source = input->scopes[i];
    require(
        array(source.members, source.member_count, 16) && source.member_count,
        "invalid module schema scope");
    ResultPublicationScope scope;
    scope.policy = static_cast<ResultPublicationPolicy>(source.policy);
    scope.members.assign(source.members, source.members + source.member_count);
    result.publication_scopes.push_back(std::move(scope));
  }
  check(result.validate());
  return result;
}
OperationPortConstraint port(const ps_port_contract_v8& input) {
  require(input.struct_size == sizeof(input), "invalid module port header");
  OperationPortConstraint result;
  result.result_schema_id = text(input.schema_id, true);
  result.result_schema_version = input.schema_version;
  result.tensor_key = text(input.tensor_key, true);
  result.element_type = input.element_type;
  result.rank = input.rank;
  result.element_type_mask = input.element_type_mask;
  return result;
}
std::optional<std::vector<std::uint32_t>> projection(const uint32_t* indices,
                                                     uint32_t count) {
  if (count == UINT32_MAX) {
    require(indices == nullptr, "invalid all-input projection");
    return {};
  }
  require(array(indices, count, 1024), "invalid input projection");
  std::vector<std::uint32_t> result;
  if (count)
    result.assign(indices, indices + count);
  return result;
}
struct MetadataViews {
  ResourceVector<ps_schema_v8> schemas;
  ResourceVector<ResourceVector<ps_tensor_spec_v8>> tensors;
  ResourceVector<ResourceVector<ps_schema_scope_v8>> scopes;
  explicit MetadataViews(const std::vector<OperationMetadata>& inputs)
      : schemas(inputs.size()), tensors(inputs.size()), scopes(inputs.size()) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      require(inputs[i].result_schema != nullptr, "missing static schema");
      const auto& source = *inputs[i].result_schema;
      tensors[i].reserve(source.tensors.size());
      scopes[i].reserve(source.publication_scopes.size());
      auto& target = schemas[i];
      target.struct_size = sizeof(target);
      target.id = {source.id.data(),
                   static_cast<std::uint32_t>(source.id.size())};
      target.version = source.version;
      for (const auto& member : source.tensors) {
        ps_tensor_spec_v8 value{};
        value.struct_size = sizeof(value);
        value.key = {member.key.data(),
                     static_cast<std::uint32_t>(member.key.size())};
        value.element_type =
            static_cast<std::uint32_t>(member.descriptor.element_type);
        value.rank = static_cast<std::uint32_t>(member.descriptor.shape.size());
        value.batch_rank = static_cast<std::uint32_t>(member.batch_axes.size());
        std::copy(member.descriptor.shape.begin(),
                  member.descriptor.shape.end(), value.shape);
        std::copy(member.batch_axes.begin(), member.batch_axes.end(),
                  value.batch_shape);
        value.atomic_trailing_axes = member.atomic_trailing_axes;
        value.spatial = member.layout.spatial;
        value.height_axis = member.layout.height_axis;
        value.width_axis = member.layout.width_axis;
        value.channel_axis = member.layout.channel_axis.value_or(UINT32_MAX);
        tensors[i].push_back(value);
      }
      for (const auto& scope : source.publication_scopes)
        scopes[i].push_back({static_cast<std::uint32_t>(scope.policy),
                             static_cast<std::uint32_t>(scope.members.size()),
                             scope.members.data()});
      target.tensors = data(tensors[i]);
      target.tensor_count = static_cast<std::uint32_t>(tensors[i].size());
      target.scopes = data(scopes[i]);
      target.scope_count = static_cast<std::uint32_t>(scopes[i].size());
    }
  }
};
struct StaticFrame {
  MetadataViews inputs;
  ResourceVector<ps_parameter_value_v8> parameters;
  ps_static_query_v8 query{};
  StaticFrame(const std::vector<OperationMetadata>& metadata,
              const std::map<std::string, ParameterValue>& source)
      : inputs(metadata) {
    parameters.reserve(source.size());
    for (const auto& item : source) {
      ps_parameter_value_v8 value{};
      value.key = view(item.first);
      if (const auto* integer = std::get_if<std::int64_t>(&item.second)) {
        value.type = PS_PARAMETER_INT64_V8;
        value.int64_value = *integer;
      } else if (const auto* number = std::get_if<double>(&item.second)) {
        value.type = PS_PARAMETER_FLOAT64_V8;
        std::memcpy(&value.float64_value, number, sizeof(*number));
      } else if (const auto* boolean = std::get_if<bool>(&item.second)) {
        value.type = PS_PARAMETER_BOOL_V8;
        value.bool_value = *boolean;
      } else {
        value.type = PS_PARAMETER_STRING_V8;
        value.string_value = view(std::get<std::string>(item.second));
      }
      parameters.push_back(value);
    }
    query.struct_size = sizeof(query);
    query.inputs = data(inputs.schemas);
    query.input_count = static_cast<std::uint32_t>(inputs.schemas.size());
    query.parameters = data(parameters);
    query.parameter_count = static_cast<std::uint32_t>(parameters.size());
  }
};
ResourceVector<ps_resource_view_v8> resource_views(
    const ResourceSet& resources) {
  ResourceVector<ps_resource_view_v8> result;
  result.reserve(resources.size());
  for (std::size_t i = 0; i < resources.size(); ++i) {
    const auto resource = take(resources.at(i));
    ps_resource_view_v8 value{};
    value.identity.byte_length = resource.identity().byte_length;
    std::copy(resource.identity().sha256.begin(),
              resource.identity().sha256.end(), value.identity.sha256);
    value.data = resource.bytes().data();
    result.push_back(value);
  }
  return result;
}
struct Definition {
  ps_operation_v8 api;
  std::shared_ptr<void> library;
  std::vector<OperationOutputTraits> outputs;
};
struct StaticOwner {
  std::shared_ptr<const Definition> definition;
  ps_operation_state_v8 state{};
  ~StaticOwner() {
    core_internal::CallbackEnvironment environment;
    if (state.release)
      state.release(state.context);
  }
};
// Serializes failure latching as well as builder mutation. Cross-thread service
// calls reject without entering the builder; the C callback owns this scope.
struct Services {
  std::mutex mutex;
  const std::thread::id thread = std::this_thread::get_id();
  Status failure;
  std::uint64_t calls = 0, limit;
  explicit Services(std::uint64_t limit) : limit(limit) {}
  template <class F>
  ps_kernel_status_v8 run(F&& action) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (failure.ok()) {
      try {
        require(thread == std::this_thread::get_id(),
                "service called on foreign thread");
        if (calls == limit)
          throw Failure{
              {ErrorCode::ResourceExhausted, "module service work limit"}};
        ++calls;
        action();
      } catch (...) {
        failure = exception_status();
      }
    }
    auto result =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
    result.domain_code = failure.detail.domain_code;
    return result;
  }
};
struct Preparation : Services {
  OperationPreparation result;
  std::vector<bool> supplied;
  explicit Preparation(const Definition& definition) : Services(1048576) {
    for (const auto& output : definition.outputs) {
      OperationOutputSpecialization value;
      value.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(*output.result_schema);
      result.outputs.push_back(std::move(value));
    }
    supplied.resize(result.outputs.size());
  }
  static ps_kernel_status_v8 set_output(void* raw, uint32_t index,
                                        const ps_schema_v8* source,
                                        const uint32_t* inputs,
                                        uint32_t count) noexcept {
    auto& self = *static_cast<Preparation*>(raw);
    return self.run([&] {
      require(index < self.result.outputs.size() && !self.supplied[index],
              "invalid or repeated prepared output");
      auto copied = schema(source);
      auto selected = projection(inputs, count);
      self.result.outputs[index] = {
          {std::make_shared<const SchemaTemplate>(std::move(copied))},
          std::move(selected)};
      self.supplied[index] = true;
    });
  }
  static ps_kernel_status_v8 require_resource(
      void* raw, const ps_resource_id_v8* source) noexcept {
    auto& self = *static_cast<Preparation*>(raw);
    return self.run([&] {
      require(pointer(source) && source->byte_length &&
                  self.result.resources.size() < 4096,
              "invalid resource identity or count");
      ResourceIdentity value;
      value.byte_length = source->byte_length;
      std::copy(std::begin(source->sha256), std::end(source->sha256),
                value.sha256.begin());
      self.result.resources.push_back(value);
    });
  }
};
Result<OperationPreparation> prepare(
    const std::shared_ptr<const Definition>& definition,
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  try {
    StaticFrame frame(inputs, parameters);
    Preparation sink(*definition);
    auto owner = std::make_shared<StaticOwner>();
    owner->definition = definition;
    ps_preparation_sink_v8 api{sizeof(api), &sink, Preparation::set_output,
                               Preparation::require_resource};
    auto outcome = callback_status([&] {
      return definition->api.prepare(definition->api.user_data, &frame.query,
                                     &api, &owner->state);
    });
    check(sink.failure);
    check(std::move(outcome));
    require(!owner->state.context || owner->state.release,
            "static context lacks release");
    sink.result.state =
        std::shared_ptr<const void>(owner, owner->state.context);
    return Result<OperationPreparation>(std::move(sink.result));
  } catch (...) {
    return Result<OperationPreparation>(exception_status());
  }
}
physical::Placement placement(const ps_placement_v8* source) {
  require(pointer(source) && source->backend <= PS_BACKEND_GPU_V8 &&
              source->domain <= PS_KERNEL_DOMAIN_SHARED_V8,
          "invalid module placement");
  return {static_cast<physical::Backend>(source->backend),
          static_cast<physical::Domain>(source->domain), text(source->profile),
          text(source->queue, true)};
}
struct Builder : Services {
  OperationProgramBuilder value;
  std::uint64_t parameter_bytes = 0;
  explicit Builder(const OperationLowering& query)
      : Services(query.limits.maximum_work), value(query) {}
  static ps_kernel_status_v8 add_task(void* raw, ps_string_v8 symbol,
                                      ps_string_v8 contract,
                                      const ps_placement_v8* where,
                                      const uint64_t* scratch, uint32_t effect,
                                      uint32_t* id) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] {
      require(pointer(id) && array(scratch, 3, 3) && effect <= 1,
              "invalid task arguments");
      *id = take(self.value.add_task(
          text(symbol), text(contract), placement(where),
          {scratch[0], scratch[1], scratch[2]}, effect != 0));
    });
  }
  static ps_kernel_status_v8 add_copy(void* raw, const ps_placement_v8* where,
                                      uint32_t* id) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] {
      require(pointer(id), "missing COPY result");
      *id = take(self.value.add_copy(placement(where)));
    });
  }
  static ps_kernel_status_v8 add_temporary(void* raw,
                                           const ps_schema_v8* source,
                                           uint32_t* id) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] {
      require(pointer(id), "missing temporary result");
      *id = take(self.value.add_temporary(
          std::make_shared<const SchemaTemplate>(schema(source))));
    });
  }
  static ps_kernel_status_v8 read_input(void* raw, uint32_t task, uint32_t port,
                                        uint32_t member, uint64_t tile,
                                        uint32_t domain) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] {
      require(domain <= PS_KERNEL_DOMAIN_SHARED_V8, "invalid input domain");
      check(self.value.read_input(task, port, member, tile,
                                  static_cast<physical::Domain>(domain)));
    });
  }
  static ps_kernel_status_v8 read_input_schema(void* raw, uint32_t task,
                                               uint32_t port) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] { check(self.value.read_input_schema(task, port)); });
  }
  static ps_kernel_status_v8 write_output(void* raw, uint32_t task,
                                          uint32_t port, uint32_t member,
                                          uint64_t tile) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run(
        [&] { check(self.value.write_output(task, port, member, tile)); });
  }
  static ps_kernel_status_v8 write_temporary(void* raw, uint32_t task,
                                             uint32_t value, uint32_t member,
                                             uint64_t tile,
                                             uint32_t* id) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] {
      require(pointer(id), "missing private tile result");
      *id = take(self.value.write_temporary(task, value, member, tile));
    });
  }
  static ps_kernel_status_v8 read_tile(void* raw, uint32_t task, uint32_t tile,
                                       uint32_t port) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] { check(self.value.read_tile(task, tile, port)); });
  }
  static ps_kernel_status_v8 depends_on(void* raw, uint32_t task,
                                        uint32_t predecessor) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] { check(self.value.depends_on(task, predecessor)); });
  }
  static ps_kernel_status_v8 set_parameters(void* raw, uint32_t task,
                                            const void* bytes,
                                            uint64_t count) noexcept {
    auto& self = *static_cast<Builder*>(raw);
    return self.run([&] {
      require(array(static_cast<const uint8_t*>(bytes), count, SIZE_MAX),
              "invalid parameter bytes");
      if (count > self.limit - self.parameter_bytes)
        throw Failure{
            {ErrorCode::ResourceExhausted, "module parameter work limit"}};
      self.parameter_bytes += count;
      try {
        auto& target = self.value.parameters(task);
        target.clear();
        if (count) {
          const auto* first = static_cast<const uint8_t*>(bytes);
          target.assign(first, first + count);
        }
      } catch (const std::out_of_range&) {
        throw Failure{{ErrorCode::InvalidArgument, "unknown parameter task"}};
      }
    });
  }
};
std::vector<ps_tile_grid_v8> grids(
    const std::vector<OperationMetadata>& metadata, std::uint64_t extent) {
  std::vector<ps_tile_grid_v8> result;
  for (unsigned p = 0; p < metadata.size(); ++p) {
    auto members = take(TileGrid::members(*metadata[p].result_schema, extent));
    for (unsigned m = 0; m < members.size(); ++m) {
      const auto& grid = members[m];
      ps_tile_grid_v8 value{};
      value.port = p;
      value.member = m;
      value.rank = grid.rank();
      value.batch_axis_mask = grid.batch_axis_mask();
      value.height_axis = grid.height_axis();
      value.width_axis = grid.width_axis();
      std::copy(grid.shape().begin(), grid.shape().end(), value.shape);
      value.tile_count = grid.tile_count();
      value.batch_count = grid.batch_count();
      value.rows = grid.tile_rows();
      value.columns = grid.tile_columns();
      result.push_back(value);
    }
  }
  return result;
}
Result<OperationProgram> lower(const Definition& definition,
                               const OperationLowering& query) {
  try {
    StaticFrame frame(query.inputs, query.parameters);
    MetadataViews outputs(query.outputs);
    auto input_grids = grids(query.inputs, query.tile_extent);
    auto output_grids = grids(query.outputs, query.tile_extent);
    std::vector<ps_tile_request_v8> requests;
    for (unsigned p = 0; p < query.requests.size(); ++p)
      for (const auto& member : query.requests[p].members)
        for (const auto& interval : member.second.intervals()) {
          if (requests.size() >= query.limits.maximum_work ||
              requests.size() == UINT32_MAX)
            throw Failure{
                {ErrorCode::ResourceExhausted, "module request work limit"}};
          requests.push_back({p, member.first, interval.begin, interval.end});
        }
    ps_lowering_query_v8 facts{};
    facts.struct_size = sizeof(facts);
    facts.facts = frame.query;
    facts.static_state = query.static_state;
    facts.outputs = data(outputs.schemas);
    facts.output_count = static_cast<std::uint32_t>(outputs.schemas.size());
    facts.backend = static_cast<std::uint32_t>(query.backend);
    facts.tile_extent = query.tile_extent;
    facts.maximum_records = query.limits.maximum_records;
    facts.maximum_edges = query.limits.maximum_edges;
    facts.maximum_work = query.limits.maximum_work;
    facts.requests = data(requests);
    facts.request_count = static_cast<std::uint32_t>(requests.size());
    facts.input_grids = data(input_grids);
    facts.input_grid_count = static_cast<std::uint32_t>(input_grids.size());
    facts.output_grids = data(output_grids);
    facts.output_grid_count = static_cast<std::uint32_t>(output_grids.size());
    Builder builder(query);
    ps_operation_builder_v8 api{sizeof(api),
                                &builder,
                                Builder::add_task,
                                Builder::add_copy,
                                Builder::add_temporary,
                                Builder::read_input,
                                Builder::read_input_schema,
                                Builder::write_output,
                                Builder::write_temporary,
                                Builder::read_tile,
                                Builder::depends_on,
                                Builder::set_parameters};
    auto outcome = callback_status([&] {
      return definition.api.lower(definition.api.user_data, &facts, &api);
    });
    check(builder.failure);
    check(std::move(outcome));
    return builder.value.finish();
  } catch (...) {
    return Result<OperationProgram>(exception_status());
  }
}
ResourceVector<ps_kernel_view_spec_v8> views(
    const std::vector<KernelView>& source) {
  ResourceVector<ps_kernel_view_spec_v8> result;
  result.reserve(source.size());
  for (const auto& v : source)
    result.push_back({v.port, v.member, static_cast<std::uint32_t>(v.domain),
                      v.tile_index, v.bytes, v.layout.byte_offset,
                      kernel_layout(v.layout)});
  return result;
}
Result<ps_prepared_kernel_v8> prepare_kernel(
    const Definition& definition, const physical::KernelDesc& kernel,
    const physical::Placement& where, const KernelPreparation& query,
    const ResourceSet& resources, const ps_native_gpu_service_v2* native_gpu) {
  struct Guard {
    ps_prepared_kernel_v8 kernel{};
    ~Guard() {
      core_internal::CallbackEnvironment environment;
      if (kernel.release)
        kernel.release(kernel.context);
    }
  } guard;
  try {
    StaticFrame frame(query.inputs, query.parameters);
    MetadataViews outputs(query.outputs);
    auto reads = views(query.reads), writes = views(query.writes);
    auto immutable_resources = resource_views(resources);
    ps_kernel_preparation_v8 facts{};
    facts.struct_size = sizeof(facts);
    facts.facts = frame.query;
    facts.static_state = query.static_state;
    facts.symbol = view(kernel.symbol);
    facts.contract = view(kernel.contract);
    facts.placement = {static_cast<std::uint32_t>(where.backend),
                       static_cast<std::uint32_t>(where.domain),
                       view(where.profile), view(where.queue)};
    facts.tile_extent = query.tile_extent;
    facts.outputs = data(outputs.schemas);
    facts.output_count = static_cast<std::uint32_t>(outputs.schemas.size());
    facts.reads = data(reads);
    facts.read_count = static_cast<std::uint32_t>(reads.size());
    facts.writes = data(writes);
    facts.write_count = static_cast<std::uint32_t>(writes.size());
    facts.parameters = data(query.task_parameters);
    facts.parameter_bytes = query.task_parameters.size();
    std::copy(query.scratch.begin(), query.scratch.end(), facts.scratch);
    facts.resources = data(immutable_resources);
    facts.resource_count =
        static_cast<std::uint32_t>(immutable_resources.size());
    facts.native_gpu = native_gpu;
    check(kernel_status(definition.api.prepare_kernel(definition.api.user_data,
                                                      &facts, &guard.kernel)));
    auto result = guard.kernel;
    guard.kernel = {};
    return Result<ps_prepared_kernel_v8>(result);
  } catch (...) {
    return Result<ps_prepared_kernel_v8>(exception_status());
  }
}
}  // namespace

Result<std::vector<OperationDefinition>> import_operation_module(
    const ps_operation_module_v8* api, std::shared_ptr<void> library) {
  try {
    require(pointer(api) && api->struct_size == sizeof(*api) &&
                api->abi_version == PS_OPERATION_MODULE_ABI_VERSION_8 &&
                api->destroy &&
                array(api->operations, api->operation_count, 1024) &&
                api->operation_count,
            "invalid operation module table");
    std::vector<OperationDefinition> result;
    for (unsigned i = 0; i < api->operation_count; ++i) {
      const auto& op = api->operations[i];
      require(op.struct_size == sizeof(op) && op.lower &&
                  !(op.flags &
                    ~(PS_OPERATION_DETERMINISTIC_V8 | PS_OPERATION_PURE_V8 |
                      PS_OPERATION_CPU_V8 | PS_OPERATION_GPU_V8)) &&
                  array(op.inputs, op.input_count, 1024) &&
                  array(op.outputs, op.output_count, 64) && op.output_count &&
                  array(op.parameters, op.parameter_count, 128) &&
                  op.repeated_match <= PS_REPEATED_MATCH_DTYPE_SHAPE_V8,
              "invalid operation declaration");
      require(op.repeated_maximum
                  ? (pointer(op.repeated_input) && op.repeated_minimum &&
                     op.repeated_minimum <= op.repeated_maximum &&
                     op.repeated_maximum <= 1024 - op.input_count)
                  : (!op.repeated_input && !op.repeated_minimum &&
                     !op.repeated_match),
              "invalid repeated input declaration");
      OperationDefinition definition;
      definition.key = text(op.key, false, 1024);
      auto& traits = definition.traits;
      traits.outputs.clear();
      traits.input_count = op.input_count;
      traits.repeated_minimum = op.repeated_minimum;
      traits.repeated_maximum = op.repeated_maximum;
      traits.repeated_match = op.repeated_match != 0;
      traits.supports_cpu = (op.flags & PS_OPERATION_CPU_V8) != 0;
      traits.supports_gpu = (op.flags & PS_OPERATION_GPU_V8) != 0;
      traits.side_effect_free = (op.flags & PS_OPERATION_PURE_V8) != 0;
      traits.deterministic = (op.flags & PS_OPERATION_DETERMINISTIC_V8) != 0;
      for (unsigned p = 0; p < op.input_count; ++p)
        traits.input_schema.push_back(port(op.inputs[p]));
      if (op.repeated_input)
        traits.input_schema.push_back(port(*op.repeated_input));
      for (unsigned p = 0; p < op.output_count; ++p) {
        const auto& output = op.outputs[p];
        require(output.struct_size == sizeof(output),
                "invalid output declaration");
        OperationOutputTraits value;
        value.key = text(output.key);
        value.output_schema = port(output.contract);
        value.result_schema = schema(output.prototype);
        value.input_indices =
            projection(output.input_indices, output.input_count);
        traits.outputs.push_back(std::move(value));
      }
      for (unsigned p = 0; p < op.parameter_count; ++p) {
        const auto& parameter = op.parameters[p];
        require(parameter.struct_size == sizeof(parameter) &&
                    parameter.required <= 1 && parameter.bounded <= 1,
                "invalid parameter declaration");
        traits.parameter_schema.push_back(
            {text(parameter.key),
             static_cast<OperationParameterType>(parameter.type),
             parameter.required != 0, parameter.bounded != 0, parameter.minimum,
             parameter.maximum});
      }
      auto owner =
          std::make_shared<Definition>(Definition{op, library, traits.outputs});
      if (op.prepare) {
        traits.requires_metadata_specialization = true;
        definition.prepare_static = [owner](const auto& inputs,
                                            const auto& parameters) {
          return prepare(owner, inputs, parameters);
        };
      }
      if (op.validate_resources) {
        definition.validate_resources = [owner](const void* state,
                                                const ResourceSet& resources) {
          auto views = resource_views(resources);
          return kernel_status(owner->api.validate_resources(
              owner->api.user_data, state, data(views),
              static_cast<std::uint32_t>(views.size())));
        };
      }
      definition.lower = [owner](const OperationLowering& query) {
        return lower(*owner, query);
      };
      if (op.prepare_kernel) {
        definition.prepare_kernel =
            [owner](const auto& kernel, const auto& placement,
                    const auto& query, const auto& resources,
                    const ps_native_gpu_service_v2* device) {
              return prepare_kernel(*owner, kernel, placement, query, resources,
                                    device);
            };
      }
      result.push_back(std::move(definition));
    }
    return Result<std::vector<OperationDefinition>>(std::move(result));
  } catch (...) {
    return Result<std::vector<OperationDefinition>>(exception_status());
  }
}
}  // namespace ps::plugin_internal
