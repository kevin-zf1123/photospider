#include "photospider/plugin/dependency_program.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/dependency_block.hpp"
#include "plugin/dependency_discovery.hpp"
#include "plugin/failure_latch.hpp"
#include "plugin/joint_member_phase.hpp"
#include "plugin/operation_identity.hpp"

namespace ps {
namespace {
Status invalid(const char* text) {
  return Status::failure(ErrorCode::InvalidArgument, text);
}
bool image_metadata(const OperationMetadata& metadata) {
  return std::any_of(
      metadata.facets.begin(), metadata.facets.end(),
      [](const auto& facet) { return facet.key == "photospider.image"; });
}
Status validate_metadata(OperationMetadata* metadata) {
  auto shape = Footprint::none(metadata->descriptor.shape);
  if (!shape.ok())
    return shape.status();
  try {
    static_cast<void>(Value::element_size(metadata->descriptor.element_type));
  } catch (const std::invalid_argument&) {
    return invalid("unknown dependency dtype");
  }
  auto status = input_internal::canonicalize_facets(&metadata->facets);
  if (!status.ok())
    return status;
  for (const auto& facet : metadata->facets)
    if (facet.key == "photospider.image" ||
        facet.key == "photospider.semantic") {
      auto semantic = decode_semantic(facet);
      if (!semantic.ok())
        return semantic.status();
      status =
          validate_semantic_descriptor(semantic.value(), metadata->descriptor);
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace
Result<AtomKey> dependency_atom_key(const DependencyQuery& query) {
  if (query.kind != ObservationKind::Atomic || !query.observations.valid() ||
      query.observations.boxes().size() != 1)
    return Result<AtomKey>(invalid("one atomic observation required"));
  AtomKey key;
  key.output_index = query.output_index;
  const auto& dimensions = query.observations.boxes()[0].dimensions();
  if (dimensions.empty() || dimensions.size() > 8)
    return Result<AtomKey>(invalid("invalid observation rank"));
  key.rank = static_cast<std::uint32_t>(dimensions.size());
  for (std::uint32_t i = 0; i < key.rank; ++i) {
    if (dimensions[i].extent != 1)
      return Result<AtomKey>(invalid("batch member has multiple observations"));
    key.coordinate[i] = dimensions[i].offset;
  }
  return key.canonical() ? Result<AtomKey>(key)
                         : Result<AtomKey>(invalid("invalid atom key"));
}
Result<Footprint> operation_observations(const OperationMetadata& output,
                                         const Footprint& samples,
                                         const FootprintLimits& limits) {
  auto metadata = output;
  auto status = validate_metadata(&metadata);
  if (!status.ok())
    return Result<Footprint>(status);
  if (!samples.valid() || samples.shape() != output.descriptor.shape)
    return Result<Footprint>(invalid("output footprint domain mismatch"));
  if (!image_metadata(output))
    return Result<Footprint>(samples);
  std::vector<Region> rectangles;
  for (const auto& box : samples.boxes()) {
    if (!input_internal::complete_image_channels(output.descriptor,
                                                 output.facets, box))
      return Result<Footprint>(
          invalid("an image observation requires complete channels"));
    auto dimensions = box.dimensions();
    dimensions.pop_back();
    rectangles.emplace_back(std::move(dimensions));
  }
  auto shape = output.descriptor.shape;
  shape.pop_back();
  return Footprint::from_regions(std::move(shape), rectangles, limits);
}
Result<Footprint> observation_samples(const OperationMetadata& output,
                                      const Footprint& observations,
                                      const FootprintLimits& limits) {
  auto metadata = output;
  auto status = validate_metadata(&metadata);
  if (!status.ok())
    return Result<Footprint>(status);
  auto shape = output.descriptor.shape;
  if (image_metadata(output))
    shape.pop_back();
  if (!observations.valid() || observations.shape() != shape)
    return Result<Footprint>(invalid("observation domain mismatch"));
  if (!image_metadata(output))
    return Result<Footprint>(observations);
  std::vector<Region> rectangles;
  for (const auto& box : observations.boxes()) {
    auto dimensions = box.dimensions();
    dimensions.push_back({0, output.descriptor.shape.back()});
    rectangles.emplace_back(std::move(dimensions));
  }
  return Footprint::from_regions(output.descriptor.shape, rectangles, limits);
}
Status DependencyPhase::read(std::uint32_t port,
                             const std::vector<std::uint64_t>& coordinate,
                             void* destination, std::size_t size) const {
  try {
    auto status = consume_work(1);
    if (!status.ok())
      return status;
    if (port >= inputs.size())
      return report_failure(
          Status{ErrorCode::InvalidArgument,
                 {},
                 FailureReason::UnauthorizedRead,
                 {FailureOrigin::Protocol, FailureScope::Atom}});
    status = inputs[port].read(coordinate, destination, size);
    if (!status.ok() && status.code == ErrorCode::InvalidArgument) {
      status.reason = FailureReason::UnauthorizedRead;
      status.detail.origin = FailureOrigin::Protocol;
      status.detail.scope = FailureScope::Atom;
    }
    return status.ok() ? status : report_failure(std::move(status));
  } catch (const std::bad_alloc&) {
    return report_failure(Status{ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    return report_failure(
        Status{ErrorCode::OperationFailed, {}, FailureReason::HostException});
  }
}

void DependencyContinuation::reset() noexcept {
  if (destroy_)
    destroy_(storage_.data());
  destroy_ = nullptr;
  poll_ = nullptr;
  storage_ = MutableBuffer{};
}
DependencyContinuation::~DependencyContinuation() noexcept {
  reset();
}
DependencyContinuation::DependencyContinuation(
    DependencyContinuation&& other) noexcept
    : storage_(std::move(other.storage_)),
      destroy_(std::exchange(other.destroy_, nullptr)),
      poll_(std::exchange(other.poll_, nullptr)) {
  // Moved-from callbacks no longer own the transferred state.
}
DependencyContinuation& DependencyContinuation::operator=(
    DependencyContinuation&& other) noexcept {
  if (this != &other) {
    reset();
    storage_ = std::move(other.storage_);
    destroy_ = std::exchange(other.destroy_, nullptr);
    poll_ = std::exchange(other.poll_, nullptr);
  }
  return *this;
}
struct DependencyCheckpoint::Impl {
  std::uint32_t phase = 0;
  std::uint64_t sequence = 0, entries = 0;
  std::string identity;
  Value state;
  std::vector<DependencyNeed> witness;
};
std::uint32_t DependencyCheckpoint::phase() const {
  if (!impl_)
    throw std::logic_error("invalid dependency checkpoint");
  return impl_->phase;
}
std::uint64_t DependencyCheckpoint::sequence() const {
  if (!impl_)
    throw std::logic_error("invalid dependency checkpoint");
  return impl_->sequence;
}
const Value& DependencyCheckpoint::state() const {
  if (!impl_)
    throw std::logic_error("invalid dependency checkpoint");
  return impl_->state;
}
std::uint64_t DependencyCheckpoint::metadata_entries() const {
  if (!impl_)
    throw std::logic_error("invalid dependency checkpoint");
  return impl_->entries;
}
struct DependencySession::Impl {
  // State is declared last so its plugin destructor runs before the definition.
  std::shared_ptr<const void> definition;
  OperationTraits traits;
  DependencyQuery query;
  DependencyLimits limits;
  CancellationToken auxiliary_cancellation;
  mutable std::recursive_mutex mutex;
  bool active_call = false;
  // Only private joint-owned proxies opt in. They never escape the group,
  // whose mutex/active guard serializes every call throughout nested polls.
  bool joint_serialized = false, joint_prepared = false;
  std::uint64_t remaining_work = 0;
  std::function<Status(std::uint64_t)> shared_work;
  std::uint32_t polls = 0;
  std::string certificate_identity, block_identity;
  std::shared_ptr<plugin_internal::FailureLatch> service_failure =
      std::make_shared<plugin_internal::FailureLatch>();
  bool waiting = false, terminal = false;
  std::vector<ValueFragments> ready;
  std::vector<DependencyNeed> pending;
  std::vector<AtomCertificate> rows;
  std::vector<DependencyNeed> terminal_needs;
  DependencyContinuation state;
  Status stop() const {
    if (query.cancellation.cancelled() || auxiliary_cancellation.cancelled())
      return Status::failure(ErrorCode::Cancelled,
                             "dependency invocation cancelled");
    return Status::success();
  }
  Status record_failure(Status status) {
    if (status.detail.scope == FailureScope::Atom && !status.detail.atom) {
      if (query.kind == ObservationKind::Atomic) {
        auto key = dependency_atom_key(query);
        if (key.ok())
          status.detail.atom = key.take_value();
      } else {
        status.detail.scope = FailureScope::Group;
      }
    }
    return service_failure->record(std::move(status));
  }

  Status service_status() const {
    auto status = service_failure->snapshot();
    if (status.ok())
      return status;
    if (status.detail.origin == FailureOrigin::Unspecified) {
      if (status.code == ErrorCode::InvalidArgument ||
          status.code == ErrorCode::TypeMismatch) {
        status.detail.origin = FailureOrigin::Protocol;
        if (status.reason == FailureReason::None)
          status.reason = FailureReason::UnauthorizedRead;
      } else if (status.code == ErrorCode::ResourceExhausted) {
        status.detail.origin = FailureOrigin::Resource;
        if (status.reason == FailureReason::None)
          status.reason = FailureReason::CapacityLimit;
      } else if (status.code == ErrorCode::Cancelled ||
                 status.code == ErrorCode::Stale) {
        status.detail.origin = FailureOrigin::Cancellation;
        if (status.reason == FailureReason::None)
          status.reason = status.code == ErrorCode::Cancelled
                              ? FailureReason::Cancelled
                              : FailureReason::StaleVersion;
      } else {
        status.detail.origin = FailureOrigin::Backend;
        if (status.reason == FailureReason::None)
          status.reason = FailureReason::HostException;
      }
    }
    if (status.detail.scope == FailureScope::Unspecified)
      status.detail.scope = query.kind == ObservationKind::Atomic
                                ? FailureScope::Atom
                                : FailureScope::Group;
    if (status.detail.scope == FailureScope::Atom && !status.detail.atom) {
      auto key = dependency_atom_key(query);
      if (key.ok())
        status.detail.atom = key.take_value();
    }
    return status;
  }
  Status consume(std::uint64_t count) {
    auto status = stop();
    if (!status.ok())
      return record_failure(status);
    const auto code = service_failure->load();
    if (code != ErrorCode::Ok)
      return service_status();
    if (count > remaining_work)
      return record_failure(
          Status{ErrorCode::ResourceExhausted,
                 "dependency discovery fuel exhausted",
                 FailureReason::WorkLimit,
                 {FailureOrigin::Resource, FailureScope::Atom}});
    if (shared_work) {
      try {
        auto shared = shared_work(count);
        if (!shared.ok())
          return record_failure(shared);
      } catch (const std::bad_alloc&) {
        return record_failure(Status{ErrorCode::ResourceExhausted, {}});
      } catch (...) {
        return record_failure(Status{ErrorCode::OperationFailed, {}});
      }
    }
    remaining_work -= count;
    return Status::success();
  }
  Status retire(Status status) {
    terminal = true;
    waiting = false;
    ready.clear();
    pending.clear();
    state = DependencyContinuation{};
    const auto stopped = stop();
    return stopped.ok() || status.detail.origin == FailureOrigin::Protocol
               ? status
               : stopped;
  }
  std::vector<std::vector<std::uint64_t>> input_shapes() const {
    std::vector<std::vector<std::uint64_t>> result;
    for (const auto& input : query.inputs)
      result.push_back(input.descriptor.shape);
    return result;
  }
  Result<DependencyCertificate> certificate() const {
    return DependencyCertificate::create(certificate_identity,
                                         query.observations, input_shapes(),
                                         rows, limits.sets);
  }
  Result<std::vector<DependencyNeed>> projection(
      const DependencyNeedBatch& batch) const {
    // A terminal record uses one non-spatial request observation only inside
    // this temporary fetch projection; it never publishes an atomic
    // certificate.
    if (query.kind == ObservationKind::RequestRecord) {
      auto coverage = Footprint::all({1}, limits.sets);
      if (!coverage.ok())
        return Result<std::vector<DependencyNeed>>(coverage.status());
      auto projected = DependencyCertificate::create(
          certificate_identity, coverage.value(), input_shapes(),
          {{{0}, batch.request_needs}}, limits.sets);
      if (!projected.ok())
        return Result<std::vector<DependencyNeed>>(projected.status());
      return projected.value().backward(coverage.value(), limits.sets);
    }
    auto projected = DependencyCertificate::create(
        certificate_identity, query.observations, input_shapes(),
        batch.associations, limits.sets);
    if (!projected.ok())
      return Result<std::vector<DependencyNeed>>(projected.status());
    return projected.value().backward(query.observations, limits.sets);
  }
};
DependencySession::DependencySession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
DependencySession::~DependencySession() noexcept = default;
Status DependencySession::validate_static(
    const DependencyValidator& validate,
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    const CancellationToken& cancellation) {
  if (cancellation.cancelled())
    return Status{ErrorCode::Cancelled, {}};
  Status status;
  try {
    if (validate)
      status = validate(inputs, parameters);
  } catch (const std::bad_alloc&) {
    status = Status{ErrorCode::ResourceExhausted, {}};
  } catch (...) {
    status = Status{ErrorCode::OperationFailed, {}};
  }
  return cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}} : status;
}
Result<std::shared_ptr<DependencySession>> DependencySession::create(
    const std::string& operation, OperationTraits traits,
    const DependencyStart& start, const DependencyValidator& validate,
    DependencyRequest request, const BufferAllocator& allocator,
    std::shared_ptr<const void> definition, std::uint64_t host_proxy_bytes,
    std::function<Status(std::uint64_t)> shared_work, bool joint_serialized) {
  auto selected = select_operation_output(traits, request.output_index);
  if (!selected.ok())
    return Result<std::shared_ptr<DependencySession>>(selected.status());
  traits = selected.take_value();
  if (!start || traits.outputs[0].dependency_version != 1 ||
      !traits.outputs[0].continuation_bytes ||
      !traits.outputs[0].maximum_dependency_stages ||
      request.snapshot_identity.empty() ||
      request.snapshot_identity.size() > 4096)
    return Result<std::shared_ptr<DependencySession>>(
        invalid("invalid dependency start contract"));
  if (request.backend != Backend::Cpu && request.backend != Backend::Gpu)
    return Result<std::shared_ptr<DependencySession>>(
        invalid("unknown dependency backend"));
  if ((request.backend == Backend::Cpu && !traits.supports_cpu) ||
      (request.backend == Backend::Gpu && !traits.supports_gpu))
    return Result<std::shared_ptr<DependencySession>>(Status::failure(
        ErrorCode::BackendUnavailable, "dependency backend unavailable"));
  auto resolved = resolve_operation_traits(traits, request.inputs.size(),
                                           request.parameters);
  if (!resolved.ok())
    return Result<std::shared_ptr<DependencySession>>(resolved.status());
  for (auto& metadata : request.inputs) {
    auto status = validate_metadata(&metadata);
    if (!status.ok())
      return Result<std::shared_ptr<DependencySession>>(status);
  }
  auto output = infer_operation_output(resolved.value(), request.inputs,
                                       request.parameters);
  if (!output.ok())
    return Result<std::shared_ptr<DependencySession>>(output.status());
  const auto static_status = validate_static(
      validate, request.inputs, request.parameters, request.cancellation);
  if (!static_status.ok())
    return Result<std::shared_ptr<DependencySession>>(static_status);
  auto observations = operation_observations(output.value(), request.outputs,
                                             request.limits.sets);
  if (!observations.ok())
    return Result<std::shared_ptr<DependencySession>>(observations.status());
  if (traits.outputs[0].observation_kind == ObservationKind::Atomic) {
    auto count = observations.value().element_count();
    if (!count.ok())
      return Result<std::shared_ptr<DependencySession>>(count.status());
    if (count.value() > 1)
      return Result<std::shared_ptr<DependencySession>>(
          invalid("request-only atomic start requires one observation"));
  } else if (traits.outputs[0].observation_kind !=
             ObservationKind::RequestRecord) {
    return Result<std::shared_ptr<DependencySession>>(
        invalid("unknown observation kind"));
  }
  if (!request.limits.maximum_stages || !request.limits.maximum_state_bytes ||
      !request.limits.maximum_work)
    return Result<std::shared_ptr<DependencySession>>(Status::failure(
        ErrorCode::ResourceExhausted, "zero dependency execution limit"));
  auto impl = std::make_unique<Impl>();
  impl->definition = std::move(definition);
  impl->shared_work = std::move(shared_work);
  impl->joint_serialized = joint_serialized;
  impl->traits = resolved.take_value();
  impl->limits = request.limits;
  impl->auxiliary_cancellation = request.limits.sets.cancellation;
  impl->limits.sets.cancellation = request.cancellation;
  impl->remaining_work = request.limits.maximum_work;
  impl->query = {std::move(request.inputs),
                 output.take_value(),
                 std::move(request.parameters),
                 std::move(request.outputs),
                 observations.take_value(),
                 std::move(request.snapshot_identity),
                 traits.outputs[0].observation_kind,
                 request.backend,
                 request.cancellation,
                 request.output_index};
  content_internal::Sha256 identity;
  identity.text("photospider.dependency-contract.v1");
  identity.text(operation);
  identity.integer(static_cast<std::uint32_t>(impl->query.backend));
  contract_internal::append_traits(&identity, impl->traits);

  identity.integer(impl->query.parameters.size());
  for (const auto& entry : impl->query.parameters) {
    identity.text(entry.first);
    identity.integer(entry.second.index());
    if (const auto* number = std::get_if<std::int64_t>(&entry.second)) {
      identity.integer(static_cast<std::uint64_t>(*number));
    } else if (const auto* number = std::get_if<double>(&entry.second)) {
      std::uint64_t bits;
      std::memcpy(&bits, number, sizeof(bits));
      identity.integer(bits);
    } else if (const auto* boolean = std::get_if<bool>(&entry.second)) {
      identity.integer(*boolean);
    } else {
      identity.text(std::get<std::string>(entry.second));
    }
  }
  auto metadata_identity = [&](const OperationMetadata& metadata) {
    identity.integer(
        static_cast<std::uint32_t>(metadata.descriptor.element_type));
    identity.integer(metadata.descriptor.shape.size());
    for (auto n : metadata.descriptor.shape)
      identity.integer(n);
    contract_internal::append_facets(&identity, metadata.facets);
  };
  metadata_identity(impl->query.output);
  for (const auto& input : impl->query.inputs)
    metadata_identity(input);
  impl->block_identity = identity.finish();
  content_internal::Sha256 scoped;
  scoped.text(impl->block_identity);
  scoped.text(impl->query.snapshot_identity);
  impl->certificate_identity = scoped.finish();
  auto status = impl->stop();
  if (!status.ok())
    return Result<std::shared_ptr<DependencySession>>(status);
  if (impl->query.kind == ObservationKind::Atomic) {
    status = impl->query.observations.visit(
        [&](const auto& coordinate) {
          impl->rows.push_back({coordinate, {}});
          return Status::success();
        },
        impl->limits.sets.maximum_boxes, request.cancellation);
    if (!status.ok())
      return Result<std::shared_ptr<DependencySession>>(status);
  }
  // Descriptor evidence is independent from pixel transport and is retained
  // even when a data-dependent map resolves to no source pixels.
  for (std::uint32_t port = 0; port < impl->query.inputs.size(); ++port) {
    auto empty = Footprint::none(impl->query.inputs[port].descriptor.shape,
                                 impl->limits.sets);
    if (!empty.ok())
      return Result<std::shared_ptr<DependencySession>>(empty.status());
    DependencyNeed descriptor{port, 8, empty.take_value(), {{1, 0}}};
    if (impl->query.kind == ObservationKind::Atomic) {
      for (auto& row : impl->rows)
        row.inputs.push_back(descriptor);
    } else {
      impl->terminal_needs.push_back(std::move(descriptor));
    }
  }
  if (!impl->query.outputs.empty()) {
    status = impl->consume(1);
    if (!status.ok())
      return Result<std::shared_ptr<DependencySession>>(status);
    try {
      auto limit = allocator.limited(
          std::min(host_proxy_bytes ? host_proxy_bytes
                                    : traits.outputs[0].continuation_bytes,
                   impl->limits.maximum_state_bytes),
          [failure = impl->service_failure](ErrorCode code) {
            auto expected = ErrorCode::Ok;
            failure->compare_exchange_strong(expected, code);
          });
      auto state = start(impl->query, limit);
      if (!state.ok())
        return Result<std::shared_ptr<DependencySession>>(
            impl->retire(state.status()));
      impl->state = state.take_value();
      if (impl->service_failure->load() != ErrorCode::Ok)
        return Result<std::shared_ptr<DependencySession>>(
            impl->retire(Status{impl->service_failure->load(), {}}));
      if (!impl->state.valid() || !limit.owns_allocation(impl->state.storage_))
        return Result<std::shared_ptr<DependencySession>>(impl->retire(
            invalid("dependency state must use its host allocator")));
    } catch (const std::bad_alloc&) {
      return Result<std::shared_ptr<DependencySession>>(
          impl->retire(Status::failure(ErrorCode::ResourceExhausted,
                                       "dependency start allocation failed")));
    } catch (const std::exception& error) {
      return Result<std::shared_ptr<DependencySession>>(
          impl->retire(Status::failure(ErrorCode::OperationFailed,
                                       error.what() ? error.what() : "")));
    } catch (...) {
      return Result<std::shared_ptr<DependencySession>>(
          impl->retire(Status::failure(ErrorCode::OperationFailed,
                                       "dependency start exception")));
    }
  }
  status = impl->stop();
  if (!status.ok())
    return Result<std::shared_ptr<DependencySession>>(impl->retire(status));
  return Result<std::shared_ptr<DependencySession>>(
      std::shared_ptr<DependencySession>(
          new DependencySession(std::move(impl))));
}
Status DependencySession::begin_joint_phase(
    plugin_internal::JointMemberPhase* slot, const BufferAllocator& host) {
  if (!slot || !impl_->joint_serialized || impl_->active_call ||
      impl_->terminal || impl_->waiting || impl_->query.backend != Backend::Cpu)
    return invalid("invalid prepared joint member phase");
  auto stopped = impl_->stop();
  if (!stopped.ok())
    return impl_->retire(stopped);
  auto charged = impl_->consume(1);
  if (!charged.ok())
    return impl_->retire(charged);
  if (impl_->polls >=
      std::min(impl_->traits.outputs[0].maximum_dependency_stages,
               impl_->limits.maximum_stages))
    return impl_->retire(
        Status{ErrorCode::ResourceExhausted, {}, FailureReason::StageLimit});
  auto elements = impl_->query.outputs.element_count();
  const auto width =
      Value::element_size(impl_->query.output.descriptor.element_type);
  if (!elements.ok() || elements.value() > UINT64_MAX / width)
    return impl_->retire(Status{ErrorCode::ResourceExhausted, {}});
  auto capacity =
      std::max(impl_->traits.estimated_bytes, elements.value() * width);
  if (impl_->traits.workspace_bytes > UINT64_MAX - capacity)
    return impl_->retire(Status{ErrorCode::ResourceExhausted, {}});
  capacity += impl_->traits.workspace_bytes;
  for (const auto& input : impl_->ready) {
    auto count = input.coverage().element_count();
    const auto factor = Value::element_size(input.descriptor().element_type) *
                        impl_->traits.workspace_input_multiplier;
    if (factor &&
        (!count.ok() || count.value() > (UINT64_MAX - capacity) / factor))
      return impl_->retire(Status{ErrorCode::ResourceExhausted, {}});
    if (factor)
      capacity += count.value() * factor;
  }
  slot->allocator = host.limited(
      capacity, [failure = impl_->service_failure](ErrorCode code) {
        auto expected = ErrorCode::Ok;
        failure->compare_exchange_strong(expected, code);
      });
  slot->consume = [this](std::uint64_t count) { return impl_->consume(count); };
  slot->report = [this](Status failure) {
    return impl_->record_failure(std::move(failure));
  };
  slot->phase.emplace(DependencyPhase{impl_->query,
                                      impl_->ready,
                                      slot->allocator,
                                      slot->consume,
                                      slot->report,
                                      impl_->limits.sets,
                                      {},
                                      {},
                                      {},
                                      {},
                                      {},
                                      {},
                                      {}});
  auto& phase = *slot->phase;
  const auto checkpoint_allowed = [this]() {
    auto status = impl_->consume(1);
    if (!status.ok())
      return status;
    if (!impl_->traits.deterministic || !impl_->traits.side_effect_free)
      return impl_->record_failure(
          invalid("checkpoint requires pure atomic program"));
    return Status::success();
  };
  phase.checkpoint_before = [checkpoint_allowed](std::uint32_t, std::uint64_t) {
    auto status = checkpoint_allowed();
    return status.ok() ? Result<std::optional<DependencyCheckpoint>>(
                             std::optional<DependencyCheckpoint>{})
                       : Result<std::optional<DependencyCheckpoint>>(status);
  };
  phase.checkpoint_publish = [this, slot, checkpoint_allowed](
                                 std::uint32_t, std::uint64_t,
                                 const Value& state) {
    auto status = checkpoint_allowed();
    if (!status.ok())
      return status;
    if (!state.valid() || !slot->allocator.owns_allocation(*state.storage()))
      return impl_->record_failure(
          invalid("checkpoint state must use host allocator"));
    return Status::success();
  };
  phase.block = [this, slot](std::uint32_t kind, std::uint64_t begin,
                             std::uint64_t end, std::uint64_t mode,
                             const Value& incoming,
                             const std::function<Result<Value>()>& compute) {
    try {
      return plugin_internal::evaluate_dependency_block(
          impl_->block_identity, *slot->phase, {}, kind, begin, end, mode,
          incoming, compute);
    } catch (const std::bad_alloc&) {
      return Result<Value>(
          impl_->record_failure(Status{ErrorCode::ResourceExhausted, {}}));
    } catch (...) {
      return Result<Value>(
          impl_->record_failure(Status{ErrorCode::OperationFailed,
                                       {},
                                       FailureReason::HostException}));
    }
  };
  const auto no_gpu = [this]() {
    return impl_->record_failure(
        Status{ErrorCode::InvalidArgument,
               "native service on CPU dependency",
               FailureReason::UnauthorizedRead,
               {FailureOrigin::Protocol, FailureScope::Atom}});
  };
  phase.atlas = [no_gpu](std::uint32_t) {
    return Result<FragmentAtlas>(no_gpu());
  };
  phase.gpu_buffer = [no_gpu](const std::uint8_t*, std::uint64_t, bool) {
    return Result<std::uint64_t>(no_gpu());
  };
  phase.gpu_execute = [no_gpu](const ps_gpu_dispatch_v9*, std::uint32_t) {
    return no_gpu();
  };
  phase.discover =
      [no_gpu](std::uint32_t, std::uint32_t,
               const std::function<Status(const DependencyGpuRequestTable&)>&) {
        return no_gpu();
      };
  ++impl_->polls;
  impl_->joint_prepared = true;
  impl_->active_call = true;
  return Status::success();
}
void DependencySession::end_joint_phase() noexcept {
  impl_->active_call = false;
}
Status DependencySession::joint_service_failure() const {
  return impl_->service_status();
}
Status DependencySession::retire_joint_member(Status failure) {
  if (!impl_->joint_serialized || impl_->active_call || !impl_->waiting ||
      failure.ok())
    return invalid("joint scope retirement requires a waiting member");
  return impl_->retire(std::move(failure));
}
Result<std::optional<Status>> DependencySession::preflight_joint_reply(
    const Result<DependencyPoll>& reply) {
  using Answer = Result<std::optional<Status>>;
  const auto protocol = [](const char* message) {
    return Answer(Status{ErrorCode::InvalidArgument,
                         message,
                         FailureReason::MalformedEnvelope,
                         {FailureOrigin::Protocol, FailureScope::Group}});
  };
  const auto local = [&](Status status, FailureOrigin origin) {
    status.detail.origin = origin;
    status.detail.scope = FailureScope::Atom;
    status.detail.atom = dependency_atom_key(impl_->query).value();
    return Answer(std::optional<Status>(std::move(status)));
  };
  if (!impl_->joint_serialized || !impl_->active_call || impl_->terminal ||
      impl_->waiting)
    return protocol("member is not a currently ready joint phase");
  if (!reply.ok()) {
    auto sticky = impl_->service_status();
    return Answer(sticky.ok() ? std::optional<Status>{}
                              : std::optional<Status>(sticky));
  }
  const auto& event = reply.value();
  if (const auto* need = std::get_if<DependencyNeedBatch>(&event)) {
    if (!need->request_needs.empty())
      return protocol("atomic batch contains RequestRecord needs");
    auto projected = impl_->projection(*need);
    if (!projected.ok()) {
      if (projected.status().code == ErrorCode::ResourceExhausted)
        return local(projected.status(), FailureOrigin::Resource);
      if (projected.status().code == ErrorCode::Cancelled) {
        auto sticky = impl_->service_status();
        return sticky.detail.origin == FailureOrigin::Protocol
                   ? Answer(std::optional<Status>(sticky))
                   : local(projected.status(), FailureOrigin::Cancellation);
      }
      return protocol("invalid batch member Need payload");
    }
    if (projected.value().empty())
      return protocol("empty batch member Need");
    for (const auto& fetch : projected.value()) {
      const auto& allowed = impl_->traits.outputs[0].input_indices;
      if (allowed && std::find(allowed->begin(), allowed->end(), fetch.port) ==
                         allowed->end())
        return protocol("batch member reads excluded input");
      auto charged =
          impl_->consume(fetch.tags.size() + fetch.samples.boxes().size() + 1);
      if (!charged.ok())
        return Answer(std::optional<Status>(impl_->service_status()));
      for (const auto& box : fetch.samples.boxes())
        if (!input_internal::complete_image_channels(
                impl_->query.inputs[fetch.port].descriptor,
                impl_->query.inputs[fetch.port].facets, box))
          return protocol("batch member omits complete input observation");
    }
  } else {
    const auto& result = std::get<ValueFragments>(event);
    if (!result.valid() || result.coverage() != impl_->query.outputs ||
        result.descriptor().element_type !=
            impl_->query.output.descriptor.element_type ||
        result.descriptor().shape != impl_->query.output.descriptor.shape ||
        !input_internal::same_facets(result.facets(),
                                     impl_->query.output.facets))
      return protocol("batch Success payload differs from its atom contract");
    for (const auto& fragment : result.fragments()) {
      auto checked = input_internal::validate_port_value(
          impl_->traits.outputs[0].output_schema, fragment,
          ErrorCode::OperationFailed, [&] { return impl_->stop().code; });
      if (!checked.ok())
        return local(checked, checked.code == ErrorCode::Cancelled
                                  ? FailureOrigin::Cancellation
                                  : FailureOrigin::Domain);
    }
  }
  auto sticky = impl_->service_status();
  return Answer(sticky.ok() ? std::optional<Status>{}
                            : std::optional<Status>(sticky));
}
Result<DependencyProgress> DependencySession::poll(
    const BufferAllocator& allocator,
    const DependencyCheckpointServices& checkpoints,
    const DependencyBlockServices& blocks, const DependencyGpuServices& gpu) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::defer_lock);
  const bool serialized = impl_->joint_serialized || lock.try_lock();
  if (!serialized || impl_->active_call)
    return Result<DependencyProgress>(
        invalid("concurrent or reentrant dependency poll"));
  struct Active {
    bool& value;
    explicit Active(bool& v) : value(v) { value = true; }
    ~Active() { value = false; }
  } active(impl_->active_call);
  const bool prepared = std::exchange(impl_->joint_prepared, false);
  if (impl_->terminal)
    return Result<DependencyProgress>(
        invalid("dependency session is terminal"));
  const auto prepared_failure = impl_->service_status();
  if (prepared_failure.detail.origin == FailureOrigin::Protocol)
    return Result<DependencyProgress>(impl_->retire(prepared_failure));
  auto status = impl_->stop();
  if (!status.ok())
    return Result<DependencyProgress>(impl_->retire(status));
  if (impl_->waiting)
    return Result<DependencyProgress>(
        impl_->retire(invalid("dependency poll is awaiting supply")));
  if (!prepared) {
    status = impl_->consume(1);
    if (!status.ok())
      return Result<DependencyProgress>(impl_->retire(status));
    if (impl_->polls >=
        std::min(impl_->traits.outputs[0].maximum_dependency_stages,
                 impl_->limits.maximum_stages))
      return Result<DependencyProgress>(impl_->retire(Status::failure(
          ErrorCode::ResourceExhausted, "dependency phase limit")));
  }
  try {
    Result<DependencyPoll> polled(invalid("uninitialized poll"));
    std::vector<DependencyNeed> discovered;
    std::uint64_t discovery_metadata = 1;
    bool block_active = false, discovery_active = false;
    if (impl_->query.outputs.empty()) {
      auto empty = ValueFragments::create(
          impl_->query.output.descriptor, impl_->query.output.facets,
          impl_->query.outputs, {}, impl_->limits.sets);
      if (!empty.ok())
        return Result<DependencyProgress>(impl_->retire(empty.status()));
      polled = Result<DependencyPoll>(empty.take_value());
    } else {
      const std::function<Status(std::uint64_t)> consume = [&](auto count) {
        return impl_->consume(count);
      };
      const std::function<Status(Status)> report = [&](Status failure) {
        return impl_->record_failure(std::move(failure));
      };
      auto elements = impl_->query.outputs.element_count();
      const auto width =
          Value::element_size(impl_->query.output.descriptor.element_type);
      if (!elements.ok() || elements.value() > UINT64_MAX / width)
        return Result<DependencyProgress>(impl_->retire(
            Status::failure(ErrorCode::ResourceExhausted,
                            "dependency output capacity overflow")));
      std::uint64_t output_bytes = elements.value() * width;
      if (impl_->query.backend == Backend::Gpu) {
        if (!gpu.allocation_capacity || !gpu.materialize || !gpu.buffer ||
            !gpu.execute)
          return Result<DependencyProgress>(impl_->retire(
              Status::failure(ErrorCode::BackendUnavailable,
                              "native dependency services unavailable")));
        output_bytes = 0;
        for (const auto& box : impl_->query.outputs.boxes()) {
          const auto count = box.element_count().value();
          const auto bytes = gpu.allocation_capacity(count * width);
          if (!bytes || bytes > UINT64_MAX - output_bytes)
            return Result<DependencyProgress>(impl_->retire(
                Status::failure(ErrorCode::ResourceExhausted,
                                "native dependency capacity overflow")));
          output_bytes += bytes;
        }
      }
      std::uint64_t capacity =
          std::max(impl_->traits.estimated_bytes, output_bytes);
      if (impl_->traits.workspace_bytes > UINT64_MAX - capacity)
        return Result<DependencyProgress>(impl_->retire(Status::failure(
            ErrorCode::ResourceExhausted, "dependency workspace overflow")));
      capacity += impl_->traits.workspace_bytes;
      for (const auto& input : impl_->ready) {
        auto samples = input.coverage().element_count();
        const auto factor =
            Value::element_size(input.descriptor().element_type) *
            impl_->traits.workspace_input_multiplier;
        if (factor && (!samples.ok() ||
                       samples.value() > (UINT64_MAX - capacity) / factor))
          return Result<DependencyProgress>(impl_->retire(
              Status::failure(ErrorCode::ResourceExhausted,
                              "dependency input workspace overflow")));
        if (factor)
          capacity += samples.value() * factor;
      }
      auto stage_allocator = allocator.limited(
          capacity, [failure = impl_->service_failure](ErrorCode code) {
            auto expected = ErrorCode::Ok;
            failure->compare_exchange_strong(expected, code);
          });
      const auto checkpoint_allowed = [&]() -> Status {
        auto status = impl_->consume(1);
        if (!status.ok())
          return status;
        if ((checkpoints.find || checkpoints.publish) &&
            (checkpoints.identity.empty() ||
             checkpoints.identity.size() > 4096))
          return impl_->record_failure(
              invalid("invalid checkpoint host scope"));
        if (impl_->query.kind != ObservationKind::Atomic ||
            !impl_->traits.deterministic || !impl_->traits.side_effect_free)
          return impl_->record_failure(
              invalid("checkpoint requires pure atomic program"));
        return Status::success();
      };
      const auto checkpoint_find_impl = [&](std::uint32_t phase,
                                            std::uint64_t before)
          -> Result<std::optional<DependencyCheckpoint>> {
        using Answer = Result<std::optional<DependencyCheckpoint>>;
        auto status = checkpoint_allowed();
        if (!status.ok())
          return Answer(status);
        if (!checkpoints.find)
          return Answer(std::optional<DependencyCheckpoint>{});
        auto found = checkpoints.find(phase, before);
        if (!found.ok())
          return Answer(impl_->record_failure(found.status()));
        if (!found.value())
          return found;
        const auto& checkpoint = *found.value();
        if (!checkpoint.valid() || checkpoint.phase() != phase ||
            checkpoint.sequence() > before ||
            checkpoint.impl_->identity !=
                impl_->certificate_identity + "/" + checkpoints.identity)
          return Answer(
              impl_->record_failure(invalid("checkpoint scope mismatch")));
        status = impl_->consume(checkpoint.metadata_entries());
        if (!status.ok())
          return Answer(status);
        std::uint64_t raw = impl_->rows.size();
        const auto count_raw = [&](const std::vector<DependencyNeed>& needs) {
          for (const auto& need : needs) {
            const auto cost =
                1 + need.tags.size() + need.samples.boxes().size();
            if (cost > impl_->limits.sets.maximum_boxes ||
                raw > impl_->limits.sets.maximum_boxes - cost)
              return false;
            raw += cost;
          }
          return true;
        };
        for (const auto& row : impl_->rows)
          if (!count_raw(row.inputs) || !count_raw(checkpoint.impl_->witness))
            return Answer(impl_->record_failure(
                Status{ErrorCode::ResourceExhausted, {}}));
        status = impl_->consume(raw);
        if (!status.ok())
          return Answer(status);
        for (auto& row : impl_->rows)
          row.inputs.insert(row.inputs.end(), checkpoint.impl_->witness.begin(),
                            checkpoint.impl_->witness.end());
        auto certificate = impl_->certificate();
        if (!certificate.ok())
          return Answer(impl_->record_failure(certificate.status()));
        impl_->rows = certificate.value().rows();
        return found;
      };
      const auto checkpoint_publish_impl = [&](std::uint32_t phase,
                                               std::uint64_t sequence,
                                               const Value& state) -> Status {
        auto status = checkpoint_allowed();
        if (!status.ok())
          return status;
        if (!state.valid() ||
            !stage_allocator.owns_allocation(*state.storage()))
          return impl_->record_failure(
              invalid("checkpoint state must use its host allocator"));
        if (!checkpoints.publish)
          return Status::success();
        // Atomic sessions have exactly one already-canonical history row.
        // Borrow it here; do not rebuild/project a large certificate before
        // checking the remaining discovery fuel or before copying its witness.
        if (impl_->rows.size() != 1)
          return impl_->record_failure(
              invalid("checkpoint requires one history row"));
        const auto& witness = impl_->rows.front().inputs;
        status = impl_->consume(witness.size() + 1);
        if (!status.ok())
          return status;
        std::uint64_t entries = 1 + state.descriptor().shape.size() * 4 +
                                impl_->certificate_identity.size() +
                                checkpoints.identity.size();
        status = impl_->consume(entries + state.facets().size());
        if (!status.ok())
          return status;
        for (const auto& facet : state.facets()) {
          const auto cost = 1 + facet.key.size() + facet.payload.size();
          status = impl_->consume(cost);
          if (!status.ok())
            return status;
          entries += cost;
        }
        std::uint64_t raw = 1;
        for (const auto& need : witness) {
          const auto raw_cost =
              1 + need.tags.size() + need.samples.boxes().size();
          if (raw_cost > impl_->limits.sets.maximum_boxes ||
              raw > impl_->limits.sets.maximum_boxes - raw_cost)
            return impl_->record_failure(
                Status{ErrorCode::ResourceExhausted, {}});
          raw += raw_cost;
          const auto cost = 1 + need.tags.size() * 2 +
                            need.samples.shape().size() +
                            need.samples.boxes().size() *
                                (1 + 2 * need.samples.shape().size());
          status = impl_->consume(cost);
          if (!status.ok())
            return status;
          entries += cost;
        }
        auto stored = std::make_shared<DependencyCheckpoint::Impl>();
        stored->phase = phase;
        stored->sequence = sequence;
        stored->entries = entries;
        stored->identity =
            impl_->certificate_identity + "/" + checkpoints.identity;
        stored->state = state;
        stored->witness = witness;
        DependencyCheckpoint checkpoint;
        checkpoint.impl_ = std::move(stored);
        status = checkpoints.publish(checkpoint);
        return status.ok() ? status : impl_->record_failure(status);
      };
      const auto checkpoint_find = [&](std::uint32_t phase,
                                       std::uint64_t before)
          -> Result<std::optional<DependencyCheckpoint>> {
        try {
          return checkpoint_find_impl(phase, before);
        } catch (const std::bad_alloc&) {
          return Result<std::optional<DependencyCheckpoint>>(
              impl_->record_failure(Status{ErrorCode::ResourceExhausted, {}}));
        } catch (...) {
          return Result<std::optional<DependencyCheckpoint>>(
              impl_->record_failure(Status{ErrorCode::OperationFailed, {}}));
        }
      };
      const auto checkpoint_publish = [&](std::uint32_t phase,
                                          std::uint64_t sequence,
                                          const Value& state) -> Status {
        try {
          return checkpoint_publish_impl(phase, sequence, state);
        } catch (const std::bad_alloc&) {
          return impl_->record_failure(
              Status{ErrorCode::ResourceExhausted, {}});
        } catch (...) {
          return impl_->record_failure(Status{ErrorCode::OperationFailed, {}});
        }
      };
      DependencyPhase phase{impl_->query,
                            impl_->ready,
                            stage_allocator,
                            consume,
                            report,
                            impl_->limits.sets,
                            checkpoint_find,
                            checkpoint_publish,
                            {},
                            {},
                            {},
                            {},
                            {}};
      phase.block = [&](std::uint32_t kind, std::uint64_t begin,
                        std::uint64_t end, std::uint64_t mode,
                        const Value& incoming,
                        const std::function<Result<Value>()>& compute) {
        try {
          if (discovery_active)
            return Result<Value>(impl_->record_failure(
                invalid("pure block inside GPU discovery")));
          struct BlockScope {
            bool& active;
            bool prior;
            explicit BlockScope(bool& value) : active(value), prior(value) {
              active = true;
            }
            ~BlockScope() { active = prior; }
          } scope(block_active);
          return plugin_internal::evaluate_dependency_block(
              impl_->block_identity, phase, blocks, kind, begin, end, mode,
              incoming, compute);
        } catch (const std::bad_alloc&) {
          return Result<Value>(
              impl_->record_failure(Status{ErrorCode::ResourceExhausted, {}}));
        } catch (...) {
          return Result<Value>(
              impl_->record_failure(Status{ErrorCode::OperationFailed, {}}));
        }
      };
      std::map<std::uint32_t, FragmentAtlas> atlases;
      const auto native_allowed = [&]() {
        auto status = impl_->consume(1);
        if (!status.ok())
          return status;
        if (impl_->query.backend != Backend::Gpu)
          return impl_->record_failure(
              invalid("native service on CPU dependency"));
        return Status::success();
      };
      phase.atlas = [&](std::uint32_t port) -> Result<FragmentAtlas> {
        try {
          auto status = native_allowed();
          if (!status.ok())
            return Result<FragmentAtlas>(status);
          if (port >= impl_->ready.size())
            return Result<FragmentAtlas>(impl_->record_failure(
                invalid("native atlas port is not supplied")));
          const auto found = atlases.find(port);
          if (found != atlases.end())
            return Result<FragmentAtlas>(found->second);
          auto limits = impl_->limits.sets;
          limits.maximum_work =
              std::min(limits.maximum_work, impl_->remaining_work);
          auto prepared =
              FragmentAtlasPlan::prepare(impl_->ready[port], {}, limits);
          if (!prepared.ok())
            return Result<FragmentAtlas>(
                impl_->record_failure(prepared.status()));
          auto plan = prepared.take_value();
          status = impl_->consume(plan.preparation_work());
          if (status.ok())
            status = impl_->consume(plan.materialization_work());
          if (!status.ok())
            return Result<FragmentAtlas>(status);
          auto packed = gpu.materialize(plan, impl_->ready[port], limits);
          if (!packed.ok())
            return Result<FragmentAtlas>(
                impl_->record_failure(packed.status()));
          atlases.emplace(port, packed.value());
          return packed;
        } catch (const std::bad_alloc&) {
          return Result<FragmentAtlas>(
              impl_->record_failure(Status{ErrorCode::ResourceExhausted, {}}));
        } catch (...) {
          return Result<FragmentAtlas>(
              impl_->record_failure(Status{ErrorCode::OperationFailed, {}}));
        }
      };
      phase.gpu_buffer = [&](const std::uint8_t* bytes, std::uint64_t size,
                             bool writable) -> Result<std::uint64_t> {
        try {
          auto status = native_allowed();
          if (!status.ok())
            return Result<std::uint64_t>(status);
          auto view = gpu.buffer(bytes, size, writable);
          if (!view.ok())
            return Result<std::uint64_t>(impl_->record_failure(view.status()));
          return view;
        } catch (const std::bad_alloc&) {
          return Result<std::uint64_t>(
              impl_->record_failure(Status{ErrorCode::ResourceExhausted, {}}));
        } catch (...) {
          return Result<std::uint64_t>(
              impl_->record_failure(Status{ErrorCode::OperationFailed, {}}));
        }
      };
      phase.gpu_execute = [&](const ps_gpu_dispatch_v9* commands,
                              std::uint32_t count) -> Status {
        try {
          auto status = native_allowed();
          if (status.ok())
            status = gpu.execute(commands, count);
          return status.ok() ? status : impl_->record_failure(status);
        } catch (const std::bad_alloc&) {
          return impl_->record_failure(
              Status{ErrorCode::ResourceExhausted, {}});
        } catch (...) {
          return impl_->record_failure(Status{ErrorCode::OperationFailed, {}});
        }
      };
      phase.discover =
          [&](std::uint32_t capacity, std::uint32_t candidates,
              const std::function<Status(const DependencyGpuRequestTable&)>&
                  compute) -> Status {
        try {
          auto status = native_allowed();
          if (!status.ok())
            return status;
          if (block_active || discovery_active || !compute || !candidates)
            return impl_->record_failure(
                invalid("invalid GPU discovery callback"));
          if (!capacity || capacity > 65536 ||
              capacity > impl_->limits.maximum_gpu_requests ||
              capacity > impl_->limits.sets.maximum_boxes)
            return impl_->record_failure(Status{ErrorCode::ResourceExhausted,
                                                "GPU discovery table limit"});
          if (!gpu.allocate_discovery)
            return impl_->record_failure(
                Status{ErrorCode::BackendUnavailable,
                       "GPU discovery allocator unavailable"});
          const auto bytes = 16 + static_cast<std::uint64_t>(capacity) * 144;
          status = impl_->consume(candidates);
          if (status.ok())
            status = impl_->consume(bytes * 2);
          if (!status.ok())
            return status;
          auto allocated = gpu.allocate_discovery(bytes);
          if (!allocated.ok())
            return impl_->record_failure(allocated.status());
          auto table = allocated.take_value();
          if (table.size() != bytes)
            return impl_->record_failure(
                invalid("GPU discovery allocator span mismatch"));
          status = impl_->consume(0);
          if (!status.ok())
            return status;
          std::memset(table.data(), 0, static_cast<std::size_t>(bytes));
          struct DiscoveryScope {
            bool& active;
            explicit DiscoveryScope(bool& value) : active(value) {
              active = true;
            }
            ~DiscoveryScope() { active = false; }
          } scope(discovery_active);
          status = compute({table.data(), bytes, capacity});
          auto frozen = std::move(table).freeze();
          if (!status.ok())
            return impl_->record_failure(status);
          status = impl_->consume(0);
          if (!status.ok())
            return status;
          auto limits = impl_->limits.sets;
          limits.maximum_work =
              std::min(limits.maximum_work, impl_->remaining_work);
          auto decoded = plugin_internal::decode_discovery(
              *frozen, capacity, candidates, impl_->query, limits,
              &impl_->remaining_work, &discovery_metadata,
              [&](std::uint64_t count) { return impl_->consume(count); });
          if (!decoded.ok())
            return impl_->record_failure(decoded.status());
          if (decoded.value().size() > limits.maximum_boxes ||
              discovered.size() > limits.maximum_boxes - decoded.value().size())
            return impl_->record_failure(Status{ErrorCode::ResourceExhausted,
                                                "GPU discovery need limit"});
          for (auto& need : decoded.value())
            discovered.push_back(std::move(need));
          return Status::success();
        } catch (const std::bad_alloc&) {
          return impl_->record_failure(
              Status{ErrorCode::ResourceExhausted, {}});
        } catch (...) {
          return impl_->record_failure(Status{ErrorCode::OperationFailed, {}});
        }
      };
      if (!prepared)
        ++impl_->polls;
      std::optional<input_internal::Float32Environment> environment;
      if (!impl_->query.output.facets.empty() ||
          impl_->traits.outputs[0].output_schema.kind !=
              OperationPortKind::Value) {
        environment.emplace();
        if (!environment->active())
          return Result<DependencyProgress>(impl_->retire(Status::failure(
              ErrorCode::OperationFailed, "binary32 environment unavailable")));
      }
      polled = impl_->state.poll_(impl_->state.storage_.data(), phase);
    }
    // Returning Need relinquishes stage input leases; state-retained owners
    // remain explicit real allocations, not merely a sealed reservation.
    impl_->ready.clear();
    const auto host_failure = impl_->service_status();
    if (host_failure.detail.origin == FailureOrigin::Protocol)
      return Result<DependencyProgress>(impl_->retire(host_failure));
    status = impl_->stop();
    if (!status.ok())
      return Result<DependencyProgress>(impl_->retire(status));
    if (impl_->service_failure->load() != ErrorCode::Ok) {
      // A callback cannot replace a prior Protocol reason/scope by returning
      // a different failure with the same coarse ErrorCode.
      return Result<DependencyProgress>(impl_->retire(impl_->service_status()));
    }
    if (!polled.ok())
      return Result<DependencyProgress>(impl_->retire(polled.status()));
    auto value = polled.take_value();
    if (!discovered.empty()) {
      auto* need = std::get_if<DependencyNeedBatch>(&value);
      if (!need)
        return Result<DependencyProgress>(impl_->retire(
            invalid("GPU discovery requires supply before completion")));
      if (need->associations.size() > impl_->limits.sets.maximum_boxes ||
          need->request_needs.size() > impl_->limits.sets.maximum_boxes)
        return Result<DependencyProgress>(
            impl_->retire(Status{ErrorCode::ResourceExhausted,
                                 "GPU discovery raw attachment limit"}));
      // Charge the association search before inspecting caller rows. The later
      // normal protocol traversal has its own existing work charge.
      status = impl_->consume(need->associations.size());
      if (!status.ok())
        return Result<DependencyProgress>(impl_->retire(status));
      auto append = [&](std::vector<DependencyNeed>* destination) {
        const auto maximum = impl_->limits.sets.maximum_boxes;
        if (discovered.size() > maximum ||
            destination->size() > maximum - discovered.size())
          return Status{ErrorCode::ResourceExhausted,
                        "GPU discovery attachment limit"};
        for (auto& request : discovered)
          destination->push_back(std::move(request));
        return Status::success();
      };
      if (impl_->query.kind == ObservationKind::RequestRecord) {
        status = append(&need->request_needs);
      } else {
        std::vector<std::uint64_t> atom;
        for (const auto& axis :
             impl_->query.observations.boxes()[0].dimensions())
          atom.push_back(axis.offset);
        auto row = std::find_if(
            need->associations.begin(), need->associations.end(),
            [&](const auto& candidate) { return candidate.output == atom; });
        if (row == need->associations.end()) {
          if (need->associations.size() >= impl_->limits.sets.maximum_boxes)
            return Result<DependencyProgress>(
                impl_->retire(Status{ErrorCode::ResourceExhausted, {}}));
          need->associations.push_back(
              {std::move(atom), std::move(discovered)});
        } else {
          status = append(&row->inputs);
        }
      }
      if (!status.ok())
        return Result<DependencyProgress>(impl_->retire(status));
    }
    if (auto* need = std::get_if<DependencyNeedBatch>(&value)) {
      if ((impl_->query.kind == ObservationKind::Atomic &&
           !need->request_needs.empty()) ||
          (impl_->query.kind == ObservationKind::RequestRecord &&
           !need->associations.empty()))
        return Result<DependencyProgress>(impl_->retire(
            invalid("atomic/terminal dependency protocol mismatch")));
      status = impl_->consume(need->associations.size() +
                              need->request_needs.size());
      if (!status.ok())
        return Result<DependencyProgress>(impl_->retire(status));
      auto projected = impl_->projection(*need);
      if (!projected.ok())
        return Result<DependencyProgress>(impl_->retire(projected.status()));
      for (const auto& fetch : projected.value()) {
        const auto& allowed = impl_->traits.outputs[0].input_indices;
        if (allowed && std::find(allowed->begin(), allowed->end(),
                                 fetch.port) == allowed->end())
          return Result<DependencyProgress>(impl_->retire(
              invalid("dependency reads an excluded input port")));

        status = impl_->consume(fetch.tags.size() +
                                fetch.samples.boxes().size() + 1);
        if (!status.ok())
          return Result<DependencyProgress>(impl_->retire(status));
        for (const auto& box : fetch.samples.boxes())
          if (!input_internal::complete_image_channels(
                  impl_->query.inputs[fetch.port].descriptor,
                  impl_->query.inputs[fetch.port].facets, box))
            return Result<DependencyProgress>(impl_->retire(
                invalid("input image dependency omits channel closure")));
      }
      if (impl_->query.kind == ObservationKind::Atomic) {
        for (const auto& row : need->associations) {
          auto found = std::find_if(
              impl_->rows.begin(), impl_->rows.end(),
              [&](const auto& prior) { return prior.output == row.output; });
          if (found == impl_->rows.end())
            return Result<DependencyProgress>(
                impl_->retire(invalid("unknown dependency observation")));
          found->inputs.insert(found->inputs.end(), row.inputs.begin(),
                               row.inputs.end());
        }
        auto certificate = impl_->certificate();
        if (!certificate.ok())
          return Result<DependencyProgress>(
              impl_->retire(certificate.status()));
        impl_->rows = certificate.value().rows();
      } else {
        impl_->terminal_needs.insert(impl_->terminal_needs.end(),
                                     need->request_needs.begin(),
                                     need->request_needs.end());
        auto all = impl_->projection({{}, impl_->terminal_needs});
        if (!all.ok())
          return Result<DependencyProgress>(impl_->retire(all.status()));
        impl_->terminal_needs = all.take_value();
      }
      impl_->pending = projected.take_value();
      impl_->waiting = true;
      return Result<DependencyProgress>(*need);
    }
    auto result = std::get<ValueFragments>(std::move(value));
    if (!result.valid() || result.coverage() != impl_->query.outputs ||
        result.descriptor().element_type !=
            impl_->query.output.descriptor.element_type ||
        result.descriptor().shape != impl_->query.output.descriptor.shape ||
        !input_internal::same_facets(result.facets(),
                                     impl_->query.output.facets))
      return Result<DependencyProgress>(impl_->retire(
          Status::failure(ErrorCode::TypeMismatch,
                          "dependency result differs from inferred output")));
    for (const auto& fragment : result.fragments()) {
      status = input_internal::validate_port_value(
          impl_->traits.outputs[0].output_schema, fragment,
          ErrorCode::OperationFailed, [&] { return impl_->stop().code; });
      if (!status.ok())
        return Result<DependencyProgress>(impl_->retire(status));
    }
    DependencyResult complete{std::move(result),
                              impl_->query.outputs,
                              impl_->query.kind,
                              {},
                              {}};
    if (impl_->query.kind == ObservationKind::Atomic) {
      auto certificate = impl_->certificate();
      if (!certificate.ok())
        return Result<DependencyProgress>(impl_->retire(certificate.status()));
      complete.certificate = certificate.take_value();
    } else {
      complete.request_dependencies = impl_->terminal_needs;
    }
    status = impl_->retire(Status::success());
    if (!status.ok())
      return Result<DependencyProgress>(status);
    return Result<DependencyProgress>(std::move(complete));
  } catch (const std::bad_alloc&) {
    return Result<DependencyProgress>(impl_->retire(Status::failure(
        ErrorCode::ResourceExhausted, "dependency poll allocation failed")));
  } catch (const std::exception& error) {
    return Result<DependencyProgress>(impl_->retire(Status::failure(
        ErrorCode::OperationFailed, error.what() ? error.what() : "")));
  } catch (...) {
    return Result<DependencyProgress>(impl_->retire(Status::failure(
        ErrorCode::OperationFailed, "dependency poll exception")));
  }
}
Status DependencySession::supply(std::vector<ValueFragments> inputs,
                                 const std::string& identity) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->active_call)
    return invalid("concurrent or reentrant dependency supply");
  struct Active {
    bool& value;
    explicit Active(bool& v) : value(v) { value = true; }
    ~Active() { value = false; }
  } active(impl_->active_call);
  if (impl_->terminal)
    return invalid("dependency session is terminal");
  if (!impl_->waiting)
    return impl_->retire(invalid("dependency session is not awaiting supply"));
  auto status = impl_->stop();
  if (!status.ok())
    return impl_->retire(status);
  try {
    if (identity != impl_->query.snapshot_identity ||
        inputs.size() != impl_->query.inputs.size())
      return impl_->retire(
          invalid("dependency supply identity/count mismatch"));
    for (std::size_t port = 0; port < inputs.size(); ++port) {
      const auto& metadata = impl_->query.inputs[port];
      if (!inputs[port].valid() ||
          inputs[port].descriptor().element_type !=
              metadata.descriptor.element_type ||
          inputs[port].descriptor().shape != metadata.descriptor.shape ||
          !input_internal::same_facets(inputs[port].facets(), metadata.facets))
        return impl_->retire(Status::failure(
            ErrorCode::TypeMismatch, "dependency supply metadata mismatch"));
      auto expected =
          Footprint::none(metadata.descriptor.shape, impl_->limits.sets);
      if (!expected.ok())
        return impl_->retire(expected.status());
      for (const auto& need : impl_->pending)
        if (need.port == port) {
          auto joined =
              expected.value().unite(need.samples, impl_->limits.sets);
          if (!joined.ok())
            return impl_->retire(joined.status());
          expected = std::move(joined);
        }
      if (inputs[port].coverage() != expected.value())
        return impl_->retire(invalid("dependency supply coverage mismatch"));
      for (const auto& fragment : inputs[port].fragments()) {
        status = input_internal::validate_port_value(
            impl_->traits.input_schema[port], fragment,
            ErrorCode::OperationFailed, [&] { return impl_->stop().code; });
        if (!status.ok())
          return impl_->retire(status);
      }
    }
    impl_->ready = std::move(inputs);
    impl_->pending.clear();
    impl_->waiting = false;
    return Status::success();
  } catch (const std::bad_alloc&) {
    return impl_->retire(Status::failure(
        ErrorCode::ResourceExhausted, "dependency supply allocation failed"));
  } catch (const std::exception& error) {
    return impl_->retire(Status::failure(ErrorCode::OperationFailed,
                                         error.what() ? error.what() : ""));
  } catch (...) {
    return impl_->retire(Status::failure(ErrorCode::OperationFailed,
                                         "dependency supply exception"));
  }
}
Result<std::vector<DependencyNeed>> DependencySession::pending_reads() const {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->active_call || impl_->terminal ||
      !impl_->waiting)
    return Result<std::vector<DependencyNeed>>(
        invalid("no pending dependency reads"));
  return Result<std::vector<DependencyNeed>>(impl_->pending);
}
const DependencyQuery& DependencySession::query() const noexcept {
  return impl_->query;
}
std::uint64_t DependencySession::consumed_work() const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  return impl_->limits.maximum_work - impl_->remaining_work;
}
std::uint32_t DependencySession::poll_count() const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  return impl_->polls;
}
}  // namespace ps
