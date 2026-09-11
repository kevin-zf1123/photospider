#include "plugin/dependency_plugin.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps::plugin_internal {
namespace {
Status invalid(const char* text) {
  return Status::failure(ErrorCode::InvalidArgument, text);
}
template <class T>
bool records(const T* data, std::uint64_t count, std::uint64_t limit) {
  return count <= limit && ((count == 0) == (data == nullptr)) &&
         (!data || reinterpret_cast<std::uintptr_t>(data) % alignof(T) == 0);
}
Status outcome(int code) {
  switch (code) {
    case PS_OPERATION_RESULT_SUCCESS_V8:
      return Status::success();
    case PS_OPERATION_RESULT_CANCELLED_V8:
      return Status{ErrorCode::Cancelled, {}};
    case PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V8:
      return Status{ErrorCode::BackendUnavailable, {}};
    case PS_DEPENDENCY_RESOURCE_EXHAUSTED_V8:
      return Status{ErrorCode::ResourceExhausted, {}};
    case PS_DEPENDENCY_TYPE_MISMATCH_V8:
      return Status{ErrorCode::TypeMismatch, {}};
    case PS_DEPENDENCY_INVALID_ARGUMENT_V8:
      return Status{ErrorCode::InvalidArgument, {}};
    default:
      return Status{ErrorCode::OperationFailed, {}};
  }
}
ps_dependency_run_v8 encode_run(const Region& region) {
  ps_dependency_run_v8 run{};
  run.struct_size = sizeof(run);
  run.rank = region.rank();
  for (std::size_t i = 0; i < region.rank(); ++i) {
    run.offsets[i] = region.dimensions()[i].offset;
    run.extents[i] = region.dimensions()[i].extent;
  }
  return run;
}
Result<Region> decode_run(const ps_dependency_run_v8* run,
                          const std::vector<std::uint64_t>& shape) {
  if (!records(run, 1, 1) || run->struct_size != sizeof(*run) ||
      run->rank != shape.size())
    return Result<Region>(invalid("invalid C dependency run"));
  std::vector<RegionDimension> dimensions;
  for (std::size_t i = 0; i < 8; ++i) {
    if (i < shape.size())
      dimensions.push_back({run->offsets[i], run->extents[i]});
    else if (run->offsets[i] || run->extents[i])
      return Result<Region>(invalid("nonzero unused C run axis"));
  }
  Region result(std::move(dimensions));
  auto status = result.validate(shape);
  if (!status.ok() || result.empty())
    return Result<Region>(invalid("C run is empty or outside its domain"));
  return Result<Region>(std::move(result));
}
struct Metadata {
  ps_dependency_metadata_query_v8 query{};
  std::vector<ps_dependency_metadata_v8> inputs;
  std::vector<std::vector<ps_operation_facet_view_v8>> facets;
  std::vector<ps_operation_parameter_value_v8> parameters;
  static ps_dependency_metadata_v8 encode(
      const OperationMetadata& input,
      std::vector<ps_operation_facet_view_v8>* facets) {
    ps_dependency_metadata_v8 result{};
    result.struct_size = sizeof(result);
    result.element_type =
        static_cast<std::uint32_t>(input.descriptor.element_type);
    result.rank = input.descriptor.shape.size();
    std::copy(input.descriptor.shape.begin(), input.descriptor.shape.end(),
              result.shape);
    for (const auto& facet : input.facets)
      facets->push_back({sizeof(ps_operation_facet_view_v8), facet.key.data(),
                         static_cast<std::uint32_t>(facet.key.size()),
                         facet.version,
                         facet.payload.empty() ? nullptr : facet.payload.data(),
                         static_cast<std::uint32_t>(facet.payload.size())});
    result.facet_count = facets->size();
    result.facets = facets->empty() ? nullptr : facets->data();
    return result;
  }
  Metadata(const std::vector<OperationMetadata>& metadata,
           const std::map<std::string, ParameterValue>& values) {
    facets.resize(metadata.size());
    for (std::size_t i = 0; i < metadata.size(); ++i)
      inputs.push_back(encode(metadata[i], &facets[i]));
    for (const auto& entry : values) {
      ps_operation_parameter_value_v8 parameter{};
      parameter.struct_size = sizeof(parameter);
      parameter.key = entry.first.data();
      parameter.key_size = entry.first.size();
      if (const auto* value = std::get_if<std::int64_t>(&entry.second)) {
        parameter.type = 1;
        parameter.int64_value = *value;
      } else if (const auto* value = std::get_if<double>(&entry.second)) {
        parameter.type = 2;
        parameter.float64_value = *value;
      } else if (const auto* value = std::get_if<bool>(&entry.second)) {
        parameter.type = 3;
        parameter.bool_value = *value;
      } else {
        const auto& string_value = std::get<std::string>(entry.second);
        if (string_value.size() > UINT32_MAX)
          throw std::length_error("C parameter exceeds uint32");
        parameter.type = 4;
        parameter.string_value = string_value.data();
        parameter.string_size = string_value.size();
      }
      parameters.push_back(parameter);
    }
    query.struct_size = sizeof(query);
    query.input_count = inputs.size();
    query.parameter_count = parameters.size();
    query.inputs = inputs.empty() ? nullptr : inputs.data();
    query.parameters = parameters.empty() ? nullptr : parameters.data();
  }
};
struct Query {
  Metadata metadata;
  std::vector<ps_operation_facet_view_v8> facets;
  std::vector<ps_dependency_run_v8> outputs;
  ps_dependency_query_v8 query{};
  explicit Query(const DependencyQuery& input)
      : metadata(input.inputs, input.parameters) {
    query.struct_size = sizeof(query);
    query.metadata = metadata.query;
    query.output = Metadata::encode(input.output, &facets);
    query.observation_kind = static_cast<std::uint32_t>(input.kind);
    query.backend = static_cast<std::uint32_t>(input.backend);
    for (const auto& box : input.outputs.boxes())
      outputs.push_back(encode_run(box));
    query.output_count = outputs.size();
    query.outputs = outputs.empty() ? nullptr : outputs.data();
  }
};
struct CState;
struct Phase {
  struct Output {
    MutableValue value;
    bool published = false;
  };
  const DependencyPhase& phase;
  CState& state;
  Status failure;
  DependencyNeedBatch needs;
  std::map<std::uint64_t, Output> outputs;
  std::vector<Value> published;
  std::vector<MutableBuffer> scratch;
  std::uint64_t metadata_entries = 0;
  bool reject(Status status) {
    if (failure.ok())
      failure = phase.report_failure(std::move(status));
    return false;
  }
  template <class Function>
  bool fence(Function function) noexcept {
    try {
      if (!failure.ok())
        return false;
      auto charged = phase.consume_work(1);
      if (!charged.ok())
        return reject(charged);
      return function();
    } catch (const std::bad_alloc&) {
      return reject(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return reject(Status{ErrorCode::OperationFailed, {}});
    }
  }
  Result<std::vector<std::uint64_t>> coordinate(const std::uint64_t* data,
                                                std::uint32_t rank,
                                                void* destination,
                                                std::uint64_t size) {
    if (!records(data, rank, 8) || !rank || !destination || size > SIZE_MAX)
      return Result<std::vector<std::uint64_t>>(
          invalid("invalid C sample address"));
    return Result<std::vector<std::uint64_t>>(
        std::vector<std::uint64_t>(data, data + rank));
  }
  const Value* fragment(std::uint32_t port, std::uint32_t index) {
    if (port >= phase.inputs.size() ||
        index >= phase.inputs[port].fragments().size()) {
      reject(invalid("invalid C fragment index"));
      return nullptr;
    }
    return &phase.inputs[port].fragments()[index];
  }
};
struct CState {
  ps_dependency_program_v8 program;
  void* user = nullptr;
  MutableBuffer payload;
  std::shared_ptr<const void> library;
  bool entered = false;
  std::uint64_t next = 1;
  std::map<std::uint64_t, ValueFragments> retained;
  CState(ps_dependency_program_v8 program, void* user, MutableBuffer payload,
         std::shared_ptr<const void> library)
      : program(program),
        user(user),
        payload(std::move(payload)),
        library(std::move(library)) {}
  CState(CState&& other) noexcept
      : program(other.program),
        user(other.user),
        payload(std::move(other.payload)),
        library(std::move(other.library)),
        entered(std::exchange(other.entered, false)),
        next(other.next),
        retained(std::move(other.retained)) {}
  ~CState() noexcept {
    if (entered) {
      try {
        program.destroy(payload.data(), user);
      } catch (...) {
      }
    }
  }
  std::uint64_t id(Phase* phase) {
    if (next == UINT64_MAX) {
      phase->reject(Status{ErrorCode::ResourceExhausted, {}});
      return 0;
    }
    return next++;
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase);
};
int associate(void* context,
              const ps_dependency_association_v8* association) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  return p->fence([&] {
    if (!records(association, 1, 1) ||
        association->struct_size != sizeof(*association) ||
        association->port >= p->phase.query.inputs.size() ||
        !association->roles || (association->roles & ~15U) ||
        !records(association->runs, association->run_count, 65536) ||
        !records(association->tags, association->tag_count, 65536))
      return p->reject(invalid("invalid C association"));
    const auto& a = *association;
    std::uint64_t entries = p->metadata_entries;
    if (!entries && p->phase.query.kind == ObservationKind::Atomic)
      entries = 1;
    const auto extra =
        static_cast<std::uint64_t>(a.run_count) + a.tag_count + 1;
    const auto maximum =
        std::min(p->phase.sets.maximum_boxes, p->phase.sets.maximum_work);
    for (std::uint32_t role = 1; role <= 8; role <<= 1) {
      if (!(a.roles & role))
        continue;
      if (extra > maximum || entries > maximum - extra)
        return p->reject(Status{ErrorCode::ResourceExhausted, {}});
      entries += extra;
    }
    auto charged = p->phase.consume_work(entries - p->metadata_entries);
    if (!charged.ok())
      return p->reject(charged);
    p->metadata_entries = entries;
    const bool terminal = p->phase.query.kind == ObservationKind::RequestRecord;
    const auto rank = terminal ? 0 : p->phase.query.observations.shape().size();
    if (a.output_rank != rank)
      return p->reject(invalid("C association observation rank"));
    std::vector<std::uint64_t> output;
    for (std::size_t i = 0; i < 8; ++i) {
      if (i < rank)
        output.push_back(a.output[i]);
      else if (a.output[i])
        return p->reject(invalid("nonzero unused observation axis"));
    }
    if (!terminal && !p->phase.query.observations.contains(output))
      return p->reject(invalid("C association outside observation"));
    std::vector<Region> boxes;
    const auto& shape = p->phase.query.inputs[a.port].descriptor.shape;
    for (std::uint32_t i = 0; i < a.run_count; ++i) {
      auto box = decode_run(&a.runs[i], shape);
      if (!box.ok())
        return p->reject(box.status());
      boxes.push_back(box.take_value());
    }
    auto samples = Footprint::from_regions(shape, boxes, p->phase.sets);
    if (!samples.ok())
      return p->reject(samples.status());
    DependencyNeed need{a.port, a.roles, samples.take_value(), {}};
    for (std::uint32_t i = 0; i < a.tag_count; ++i) {
      if (a.tags[i].struct_size != sizeof(ps_dependency_tag_v8) ||
          !a.tags[i].kind)
        return p->reject(invalid("invalid C dependency tag"));
      need.tags.push_back({a.tags[i].kind, a.tags[i].id});
    }
    if (terminal) {
      p->needs.request_needs.push_back(std::move(need));
    } else {
      if (p->needs.associations.empty())
        p->needs.associations.push_back({output, {}});
      p->needs.associations[0].inputs.push_back(std::move(need));
    }
    return true;
  });
}
int read(void* context, std::uint32_t port, const std::uint64_t* coordinate,
         std::uint32_t rank, void* destination, std::uint64_t size) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  return p->fence([&] {
    auto at = p->coordinate(coordinate, rank, destination, size);
    if (!at.ok())
      return p->reject(at.status());
    auto status = p->phase.read(port, at.value(), destination, size);
    return status.ok() || p->reject(status);
  });
}
std::uint32_t fragment_count(void* context, std::uint32_t port) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  std::uint32_t count = 0;
  p->fence([&] {
    if (port >= p->phase.inputs.size())
      return p->reject(invalid("invalid C fragment port"));
    if (p->phase.inputs[port].fragments().size() > UINT32_MAX)
      return p->reject(Status{ErrorCode::ResourceExhausted, {}});
    count = p->phase.inputs[port].fragments().size();
    return true;
  });
  return count;
}
int fragment(void* context, std::uint32_t port, std::uint32_t index,
             ps_dependency_fragment_v8* destination) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  return p->fence([&] {
    if (!records(destination, 1, 1) ||
        destination->struct_size != sizeof(*destination))
      return p->reject(invalid("invalid C fragment destination"));
    const auto* value = p->fragment(port, index);
    if (!value)
      return false;
    ps_dependency_fragment_v8 result{};
    result.struct_size = sizeof(result);
    result.rank = value->descriptor().shape.size();
    result.element_type =
        static_cast<std::uint32_t>(value->descriptor().element_type);
    for (std::size_t i = 0; i < result.rank; ++i) {
      result.shape[i] = value->descriptor().shape[i];
      result.origin[i] =
          value->layout().origin.empty() ? 0 : value->layout().origin[i];
      result.offsets[i] = value->region().dimensions()[i].offset;
      result.extents[i] = value->region().dimensions()[i].extent;
      result.strides[i] = value->layout().byte_strides[i];
    }
    result.byte_offset = value->layout().byte_offset;
    result.byte_size = value->bytes().size();
    result.data = value->bytes().data();
    *destination = result;
    return true;
  });
}
std::uint64_t retain_input(void* context, std::uint32_t port,
                           std::uint32_t index) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  std::uint64_t id = 0;
  const bool accepted = p->fence([&] {
    const auto* value = p->fragment(port, index);
    if (!value)
      return false;
    if (p->state.retained.size() >=
        std::min<std::uint64_t>(p->state.program.maximum_retained_owners,
                                p->phase.sets.maximum_boxes))
      return p->reject(Status{ErrorCode::ResourceExhausted, {}});
    auto set = Footprint::from_regions(value->descriptor().shape,
                                       {value->region()}, p->phase.sets);
    if (!set.ok())
      return p->reject(set.status());
    auto held =
        ValueFragments::create(value->descriptor(), value->facets(),
                               set.take_value(), {*value}, p->phase.sets);
    if (!held.ok())
      return p->reject(held.status());
    id = p->state.id(p);
    if (!id)
      return false;
    p->state.retained.emplace(id, held.take_value());
    return true;
  });
  return accepted ? id : 0;
}
int read_owner(void* context, std::uint64_t id, const std::uint64_t* coordinate,
               std::uint32_t rank, void* destination,
               std::uint64_t size) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  return p->fence([&] {
    auto found = p->state.retained.find(id);
    if (found == p->state.retained.end())
      return p->reject(invalid("unknown C retained owner"));
    auto at = p->coordinate(coordinate, rank, destination, size);
    if (!at.ok())
      return p->reject(at.status());
    auto status = found->second.read(at.value(), destination, size);
    return status.ok() || p->reject(status);
  });
}
int release_owner(void* context, std::uint64_t id) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  return p->fence([&] {
    return p->state.retained.erase(id) != 0 ||
           p->reject(invalid("unknown C retained owner"));
  });
}
std::uint8_t* allocate_output(void* context, const ps_dependency_run_v8* region,
                              std::uint64_t* handle) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  std::uint8_t* bytes = nullptr;
  p->fence([&] {
    if (!records(handle, 1, 1))
      return p->reject(invalid("invalid C output handle address"));
    if (p->outputs.size() >= p->phase.sets.maximum_boxes)
      return p->reject(Status{ErrorCode::ResourceExhausted, {}});
    auto box = decode_run(region, p->phase.query.output.descriptor.shape);
    if (!box.ok())
      return p->reject(box.status());
    if (!input_internal::complete_image_channels(
            p->phase.query.output.descriptor, p->phase.query.output.facets,
            box.value()))
      return p->reject(invalid("C output requires full image channels"));
    auto requested = Footprint::from_regions(
        p->phase.query.output.descriptor.shape, {box.value()}, p->phase.sets);
    if (!requested.ok())
      return p->reject(requested.status());
    auto outside =
        requested.value().subtract(p->phase.query.outputs, p->phase.sets);
    if (!outside.ok())
      return p->reject(outside.status());
    if (!outside.value().empty())
      return p->reject(invalid("C output outside request"));
    auto allocated = MutableValue::allocate(p->phase.query.output.descriptor,
                                            box.value(), p->phase.allocator);
    if (!allocated.ok())
      return p->reject(allocated.status());
    const auto id = p->state.id(p);
    if (!id)
      return false;
    auto inserted =
        p->outputs.emplace(id, Phase::Output{allocated.take_value()});
    bytes = inserted.first->second.value.data();
    *handle = id;
    return true;
  });
  return bytes;
}
int publish_output(void* context, std::uint64_t handle) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  return p->fence([&] {
    auto found = p->outputs.find(handle);
    if (found == p->outputs.end() || found->second.published)
      return p->reject(invalid("unknown or duplicate C output publication"));
    found->second.published = true;
    auto value =
        std::move(found->second.value).publish(p->phase.query.output.facets);
    if (!value.ok())
      return p->reject(value.status());
    p->published.push_back(value.take_value());
    return true;
  });
}
std::uint8_t* scratch(void* context, std::uint64_t size) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  std::uint8_t* bytes = nullptr;
  p->fence([&] {
    auto allocation = p->phase.allocator.allocate(size);
    if (!allocation.ok())
      return p->reject(allocation.status());
    p->scratch.push_back(allocation.take_value());
    bytes = p->scratch.back().data();
    return true;
  });
  return bytes;
}
int consume_work(void* context, std::uint64_t count) noexcept {
  auto* p = static_cast<Phase*>(context);
  if (!p)
    return {};
  return p->fence([&] {
    auto status = p->phase.consume_work(count);
    return status.ok() || p->reject(status);
  });
}
int is_cancelled(void* context) noexcept {
  return !context ||
         static_cast<Phase*>(context)->phase.query.cancellation.cancelled();
}
Result<DependencyPoll> CState::poll(const DependencyPhase& phase) {
  Phase p{phase, *this, {}, {}, {}, {}, {}};
  Query query(phase.query);
  const ps_dependency_services_v8 services{sizeof(ps_dependency_services_v8),
                                           0,
                                           &p,
                                           associate,
                                           read,
                                           fragment_count,
                                           fragment,
                                           retain_input,
                                           read_owner,
                                           release_owner,
                                           allocate_output,
                                           publish_output,
                                           scratch,
                                           consume_work,
                                           is_cancelled};
  const auto result =
      program.poll(&query.query, payload.data(), &services, user);
  if (!p.failure.ok())
    return Result<DependencyPoll>(p.failure);
  if (result == PS_DEPENDENCY_NEED_V8) {
    if (!p.outputs.empty())
      return Result<DependencyPoll>(
          invalid("C Need cannot publish or allocate output"));
    if (phase.query.kind == ObservationKind::Atomic &&
        p.needs.associations.empty()) {
      std::vector<std::uint64_t> atom;
      for (const auto& d : phase.query.observations.boxes()[0].dimensions())
        atom.push_back(d.offset);
      p.needs.associations.push_back({atom, {}});
    }
    return Result<DependencyPoll>(std::move(p.needs));
  }
  auto status = outcome(result);
  if (!status.ok())
    return Result<DependencyPoll>(status);
  if (!p.needs.associations.empty() || !p.needs.request_needs.empty())
    return Result<DependencyPoll>(
        invalid("C completion contains unresolved needs"));
  if (p.outputs.size() != p.published.size())
    return Result<DependencyPoll>(invalid("unpublished C output allocation"));
  auto value = ValueFragments::create(
      phase.query.output.descriptor, phase.query.output.facets,
      phase.query.outputs, p.published, phase.sets);
  if (!value.ok())
    return Result<DependencyPoll>(value.status());
  return Result<DependencyPoll>(value.take_value());
}
}  // namespace
Status prepare_dependency_plugin(OperationDefinition* definition,
                                 const ps_dependency_program_v8* pointer,
                                 void* user,
                                 std::shared_ptr<const void> library) {
  if (!records(pointer, 1, 1) || pointer->struct_size != sizeof(*pointer) ||
      pointer->reserved || !pointer->start || !pointer->poll ||
      !pointer->destroy || !pointer->state_bytes ||
      pointer->state_bytes > 1048576 || !pointer->maximum_stages ||
      pointer->maximum_stages > 1048576 ||
      pointer->maximum_retained_owners > 65536)
    return invalid("invalid C dependency program table");
  const auto program = *pointer;
  definition->traits.dependency_version = 1;
  definition->traits.continuation_bytes = program.state_bytes + sizeof(CState);
  definition->traits.maximum_dependency_stages = program.maximum_stages;
  if (program.validate) {
    definition->validate_dependency =
        [library, program, user](const auto& inputs, const auto& parameters) {
          Metadata metadata(inputs, parameters);
          return outcome(program.validate(&metadata.query, user));
        };
  }
  definition->start_dependency =
      [library, program, user](
          const DependencyQuery& query,
          const BufferAllocator& allocator) -> Result<DependencyContinuation> {
    auto allocated = allocator.allocate(program.state_bytes);
    if (!allocated.ok())
      return Result<DependencyContinuation>(allocated.status());
    CState state(program, user, allocated.take_value(), library);
    std::memset(state.payload.data(), 0, state.payload.size());
    Query input(query);
    state.entered = true;
    auto status = outcome(program.start(&input.query, state.payload.data(),
                                        state.payload.size(), user));
    if (!status.ok())
      return Result<DependencyContinuation>(status);
    return DependencyContinuation::make<CState>(allocator, std::move(state));
  };
  return Status::success();
}
}  // namespace ps::plugin_internal
