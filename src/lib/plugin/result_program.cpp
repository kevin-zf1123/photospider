#include "photospider/plugin/result_program.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "data/affine_view.hpp"
#include "data/input_validation.hpp"
#include "data/result_host_access.hpp"
#include "data/result_window_access.hpp"
#include "execution/result_native.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/failure_latch.hpp"
#include "plugin/operation_exception.hpp"
#include "plugin/result_payload_bound.hpp"

namespace ps {
Result<AtomDomain> result_observation_domain(const ResultProgramQuery& query) {
  using Answer = Result<AtomDomain>;
  if (!query.output.result_schema || query.output_index >= 64)
    return Answer(Status{ErrorCode::InvalidArgument,
                         "invalid Result observation domain"});
  const auto& schema = *query.output.result_schema;
  auto valid = schema.validate(true);
  if (!valid.ok())
    return Answer(valid);
  AtomDomain domain;
  domain.first.output_index = query.output_index;
  if (schema.tensors.empty()) {
    if (query.tensor_outputs || query.tensor_slot)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "invalid Result observation domain"});
    domain.first.rank = 1;
    domain.extent[0] = 1;
    return Answer(domain);
  }
  if (query.tensor_slot >= schema.tensors.size())
    return Answer(Status{ErrorCode::InvalidArgument,
                         "invalid Result observation tensor slot"});
  const auto& tensor = schema.tensors[query.tensor_slot];
  const auto tuple =
      input_internal::tuple_channel_axis(tensor.descriptor, tensor.facets);
  const auto rank = tensor.batch_axes.size() + tensor.descriptor.shape.size();
  for (std::size_t axis = 0; axis < rank; ++axis) {
    const bool grouped = axis >= rank - tensor.atomic_trailing_axes ||
                         (tuple && axis == *tuple + tensor.batch_axes.size());
    if (grouped)
      continue;
    domain.extent[domain.first.rank++] =
        axis < tensor.batch_axes.size()
            ? tensor.batch_axes[axis]
            : tensor.descriptor.shape[axis - tensor.batch_axes.size()];
  }
  if (!domain.first.rank) {
    domain.first.rank = 1;
    domain.extent[0] = 1;
  }
  return Answer(domain);
}
Result<AtomKey> result_atom_key(const ResultProgramQuery& query) {
  using Answer = Result<AtomKey>;
  const auto invalid = [] {
    return Answer(Status{ErrorCode::InvalidArgument,
                         "Result query requires one Atomic observation"});
  };
  if (!query.output.result_schema || query.output_index >= 64)
    return invalid();
  AtomKey key;
  key.output_index = query.output_index;
  const auto& schema = *query.output.result_schema;
  auto valid = schema.validate(true);
  if (!valid.ok())
    return Answer(valid);
  if (schema.tensors.empty()) {
    if (query.tensor_outputs || query.tensor_slot)
      return invalid();
    key.rank = 1;
    return Answer(key);
  }
  if (query.tensor_slot >= schema.tensors.size())
    return invalid();
  const auto& tensor = schema.tensors[query.tensor_slot];
  auto requested = query.tensor_outputs
                       ? Result<Footprint>(*query.tensor_outputs)
                       : Footprint::all(tensor.sample_shape());
  if (!requested.ok())
    return Answer(requested.status());
  auto closed = tensor.close_samples(requested.value());
  if (!closed.ok())
    return Answer(closed.status());
  if (closed.value().boxes().size() != 1 || closed.value().empty())
    return invalid();
  const auto& dimensions = closed.value().boxes()[0].dimensions();
  const auto tuple =
      input_internal::tuple_channel_axis(tensor.descriptor, tensor.facets);
  for (std::size_t axis = 0; axis < dimensions.size(); ++axis) {
    const bool grouped =
        axis >= dimensions.size() - tensor.atomic_trailing_axes ||
        (tuple && axis == *tuple + tensor.batch_axes.size());
    if (grouped)
      continue;
    if (dimensions[axis].extent != 1)
      return invalid();
    key.coordinate[key.rank++] = dimensions[axis].offset;
  }
  if (!key.rank)
    key.rank = 1;
  return Answer(key);
}
std::uint32_t ResultCheckpoint::phase() const {
  if (!valid())
    throw std::logic_error("invalid Result checkpoint");
  return phase_;
}
std::uint64_t ResultCheckpoint::sequence() const {
  if (!valid())
    throw std::logic_error("invalid Result checkpoint");
  return sequence_;
}
const ResultRef& ResultCheckpoint::state() const {
  if (!valid())
    throw std::logic_error("invalid Result checkpoint");
  return state_;
}
Status ResultTensorInput::read_granted(
    const std::vector<std::uint64_t>& coordinate, void* destination,
    std::size_t bytes, const CancellationToken& cancellation) const {
  if (!payload_authorized_ || !samples_.contains(coordinate))
    return Status{ErrorCode::InvalidArgument,
                  "unauthorized tensor Need read",
                  FailureReason::UnauthorizedRead,
                  {FailureOrigin::Protocol, FailureScope::Group}};
  if (!input_backing_.empty()) {
    if (cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    auto charged =
        resources_->consume({input_backing_.size() * (1 + coordinate.size())});
    if (!charged.ok())
      return charged;
    for (const auto& stored : input_backing_) {
      const auto& backing = stored->value;
      bool contains = true;
      for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
        const auto dimension = backing.region().dimensions()[axis];
        contains = contains && coordinate[axis] >= dimension.offset &&
                   coordinate[axis] - dimension.offset < dimension.extent;
      }
      if (!contains)
        continue;
      if (!destination ||
          bytes != Value::element_size(spec().descriptor.element_type))
        return Status{ErrorCode::InvalidArgument, "invalid tensor read size"};
      auto address = backing.byte_address(coordinate);
      if (!address.ok())
        return address.status();
      auto observed =
          data_internal::ResultHostAccessScope::observe(backing.storage());
      if (!observed.ok())
        return observed;
      std::memcpy(destination, backing.bytes().data() + address.value(), bytes);
      return Status::success();
    }
    return Status{ErrorCode::Internal, "prepared input backing has a hole"};
  }
  if (pieces_.empty())
    return result_.read_tensor(descriptor_, slot_, coordinate, destination,
                               bytes, cancellation);
  if (cancellation.cancelled())
    return Status{ErrorCode::Cancelled, {}};
  auto charged =
      resources_->consume({pieces_.size() * (1 + coordinate.size())});
  if (!charged.ok())
    return charged;
  for (const auto& piece : pieces_)
    if (piece.samples.contains(coordinate))
      return piece.result.read_tensor(piece.descriptor, slot_, coordinate,
                                      destination, bytes, cancellation);
  return Status{ErrorCode::InvalidArgument,
                "missing tensor Need piece",
                FailureReason::UnauthorizedRead,
                {FailureOrigin::Protocol, FailureScope::Group}};
}
Status ResultTensorInput::read(const std::vector<std::uint64_t>& coordinate,
                               void* destination, std::size_t bytes,
                               const CancellationToken& cancellation) const {
  auto status = read_granted(coordinate, destination, bytes, cancellation);
  if (!status.ok()) {
    if (observer_)
      observer_->record(status);
    if (failure_) {
      auto expected = ErrorCode::Ok;
      failure_->compare_exchange_strong(expected, status.code);
    }
  }
  return status;
}
Result<ResultTensorReadWindow> ResultTensorInput::acquire(
    const Region& region, const CancellationToken& cancellation) const try {
  using Answer = Result<ResultTensorReadWindow>;
  if (!payload_authorized_) {
    Status status{ErrorCode::InvalidArgument,
                  "descriptor-only image Need cannot read payload",
                  FailureReason::UnauthorizedRead,
                  {FailureOrigin::Protocol, FailureScope::Group}};
    if (observer_)
      observer_->record(status);
    if (failure_) {
      auto expected = ErrorCode::Ok;
      failure_->compare_exchange_strong(expected, status.code);
    }
    return Answer(status);
  }
  std::optional<ResourceAllocationScope> scope;
  if (resources_ &&
      (!resource_internal::metadata_budget() ||
       !resource_internal::metadata_budget()->same_owner(*resources_)))
    scope.emplace(*resources_);
  FootprintLimits limits;
  limits.cancellation = cancellation;
  if (resources_) {
    limits.consume_work = [root = *resources_](auto n) {
      return root.consume({n});
    };
  }
  auto requested = Footprint::from_regions(samples_.shape(), {region}, limits);
  Status status;
  if (!requested.ok()) {
    status = requested.status();
  } else {
    auto unauthorized = requested.value().subtract(samples_, limits);
    status = !unauthorized.ok() ? unauthorized.status()
             : !unauthorized.value().empty()
                 ? Status{ErrorCode::InvalidArgument,
                          "unauthorized tensor Need window",
                          FailureReason::UnauthorizedRead,
                          {FailureOrigin::Protocol, FailureScope::Group}}
                 : Status::success();
  }
  const auto acquire = [&]() -> Answer {
    if (!status.ok())
      return Answer(status);
    if (pieces_.empty())
      return result_.acquire_tensor(descriptor_, slot_, region, cancellation);
    ResourceVector<ResultTensorReadWindow> windows{
        ResourceAllocator<ResultTensorReadWindow>(*resources_)};
    auto charged = resources_->consume({pieces_.size()});
    if (!charged.ok())
      return Answer(charged);
    for (const auto& piece : pieces_) {
      auto covered = requested.value().intersect(piece.samples, limits);
      if (!covered.ok())
        return Answer(covered.status());
      for (const auto& box : covered.value().boxes()) {
        auto window = piece.result.acquire_tensor(piece.descriptor, slot_, box,
                                                  cancellation);
        if (!window.ok())
          return Answer(window.status());
        windows.push_back(window.take_value());
      }
    }
    if (windows.size() == 1)
      return Answer(std::move(windows.front()));
    return execution_internal::ResultWindowAccess::compose(
        *resources_, region, std::move(windows), cancellation);
  };
  auto result = acquire();
  if (result.ok() && !input_backing_.empty()) {
    auto prepared_window = [&]() -> Answer {
      auto scratch = resources_->reserve(ResourceCapacity::host(4096, 4096));
      if (!scratch.ok())
        return Answer(scratch.status());
      ResourceVector<ResourceLease> metadata{
          ResourceAllocator<ResourceLease>(*resources_)};
      ResourceVector<Value> parts{ResourceAllocator<Value>(*resources_)};
      for (const auto& stored : input_backing_) {
        auto charged = resources_->consume({region.rank() + 1});
        if (!charged.ok())
          return Answer(charged);
        const auto& backing = stored->value;
        std::vector<RegionDimension> dimensions;
        for (std::size_t axis = 0; axis < region.rank(); ++axis) {
          const auto a = region.dimensions()[axis];
          const auto b = backing.region().dimensions()[axis];
          const auto start = std::max(a.offset, b.offset);
          const auto end = std::min(a.offset + a.extent, b.offset + b.extent);
          dimensions.push_back({start, end > start ? end - start : 0});
        }
        Region intersection(std::move(dimensions));
        if (intersection.empty())
          continue;
        const auto bytes =
            2 * region.rank() *
            (3 * sizeof(std::uint64_t) + sizeof(RegionDimension));
        auto lease = resources_->reserve(ResourceCapacity::host(bytes, bytes));
        if (!lease.ok())
          return Answer(lease.status());
        metadata.push_back(lease.take_value());
        auto part = backing.view(intersection);
        if (!part.ok())
          return Answer(part.status());
        parts.push_back(part.take_value());
      }
      const auto bytes = 2 * region.rank() *
                         (3 * sizeof(std::uint64_t) + sizeof(RegionDimension));
      auto window_metadata =
          resources_->reserve(ResourceCapacity::host(bytes, bytes));
      if (!window_metadata.ok())
        return Answer(window_metadata.status());
      auto joined = input_internal::join_affine_view(
          {spec().descriptor.element_type, spec().sample_shape()}, region,
          parts, limits);
      if (!joined.ok())
        return Answer(joined.status());
      // A rectangle can span independently prepared boxes. Preserve its
      // authorized original compound window when no common mapping exists.
      return joined.value()
                 ? execution_internal::ResultWindowAccess::replace_backing(
                       result.take_value(), std::move(*joined.value()),
                       window_metadata.take_value())
                 : Answer(result.take_value());
    };
    result = prepared_window();
  }
  if (!result.ok()) {
    if (observer_)
      observer_->record(result.status());
    if (failure_) {
      auto expected = ErrorCode::Ok;
      failure_->compare_exchange_strong(expected, result.status().code);
    }
  }
  return result;
} catch (const std::bad_alloc&) {
  Status status{ErrorCode::ResourceExhausted, {}};
  if (observer_)
    observer_->record(status);
  if (failure_) {
    auto expected = ErrorCode::Ok;
    failure_->compare_exchange_strong(expected, status.code);
  }
  return Result<ResultTensorReadWindow>(status);
} catch (...) {
  Status status{ErrorCode::OperationFailed, {}};
  if (observer_)
    observer_->record(status);
  if (failure_) {
    auto expected = ErrorCode::Ok;
    failure_->compare_exchange_strong(expected, status.code);
  }
  return Result<ResultTensorReadWindow>(status);
}
Status ResultTensorInput::prepare_whole_view(
    bool require_view, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) try {
  if (!payload_authorized_ || samples_.empty() || !input_backing_.empty())
    return Status::success();
  if (!resources_ || !consume_work)
    return Status{ErrorCode::Internal, "missing input preparation Root"};
  ResourceAllocationScope scope(*resources_);
  auto scratch = resources_->reserve(ResourceCapacity::host(4096, 4096));
  if (!scratch.ok())
    return scratch.status();
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = consume_work;
  ResourceVector<std::shared_ptr<const InputBacking>> prepared{
      ResourceAllocator<std::shared_ptr<const InputBacking>>(*resources_)};
  prepared.reserve(samples_.boxes().size());
  const ValueDescriptor descriptor{spec().descriptor.element_type,
                                   spec().sample_shape()};
  const auto width = Value::element_size(descriptor.element_type);
  for (const auto& box : samples_.boxes()) {
    auto charged = consume_work(1);
    if (!charged.ok())
      return charged;
    // Retain capacity for Value's standard-container metadata independently of
    // the temporary coordinate scratch. Copies of the grant share this owner.
    const auto metadata_bytes =
        2 * box.rank() * (3 * sizeof(std::uint64_t) + sizeof(RegionDimension));
    auto metadata = resources_->reserve(
        ResourceCapacity::host(metadata_bytes, metadata_bytes));
    if (!metadata.ok())
      return metadata.status();
    const auto retain = [&](Value value) {
      return std::allocate_shared<const InputBacking>(
          ResourceAllocator<InputBacking>(*resources_), metadata.take_value(),
          std::move(value));
    };
    auto acquired = acquire(box, cancellation);
    if (!acquired.ok())
      return acquired.status();
    auto window = acquired.take_value();
    auto view =
        execution_internal::ResultWindowAccess::input_view(window, limits);
    if (!view.ok())
      return view.status();
    if (view.value()) {
      prepared.push_back(retain(std::move(*view.value())));
      continue;
    }
    if (require_view)
      return Status{ErrorCode::InvalidArgument,
                    "ViewUnavailable: Whole input needs collection",
                    FailureReason::InvalidDomain,
                    {FailureOrigin::Domain, FailureScope::Group}};
    auto count = box.element_count();
    if (!count.ok() || count.value() > UINT64_MAX / width)
      return Status{ErrorCode::ResourceExhausted, {}};
    auto lookup = execution_internal::ResultWindowAccess::read_work(window);
    if (!lookup.ok())
      return lookup.status();
    const auto per_sample = lookup.value() + width + box.rank();
    if (per_sample < lookup.value() || count.value() > UINT64_MAX / per_sample)
      return Status{ErrorCode::ResourceExhausted, {}};
    charged = consume_work(count.value() * per_sample);
    if (!charged.ok())
      return charged;
    auto allocated =
        MutableValue::allocate(descriptor, box, resources_->allocator());
    if (!allocated.ok())
      return allocated.status();
    auto writer = allocated.take_value();
    auto footprint = Footprint::from_regions(descriptor.shape, {box}, limits);
    if (!footprint.ok())
      return footprint.status();
    std::uint64_t offset = 0;
    auto copied = footprint.value().visit(
        [&](const auto& at) {
          auto active = consume_work(0);
          if (!active.ok())
            return active;
          auto row = window.row_run(at);
          if (!row.ok())
            return row.status();
          std::memcpy(writer.data() + offset, row.value().data, width);
          offset += width;
          return Status::success();
        },
        count.value(), cancellation);
    if (!copied.ok())
      return copied;
    auto frozen = std::move(writer).publish();
    if (!frozen.ok())
      return frozen.status();
    prepared.push_back(retain(frozen.take_value()));
  }
  if (cancellation.cancelled())
    return Status{ErrorCode::Cancelled, {}};
  input_backing_ = std::move(prepared);
  return Status::success();
} catch (const std::bad_alloc&) {
  return Status{ErrorCode::ResourceExhausted, {}};
}
Result<ResultTensorReadWindow> ResultProgramPhase::acquire_native_tensor(
    std::uint32_t input, std::uint32_t slot, const Region& region) const {
  using Answer = Result<ResultTensorReadWindow>;
  auto answer = [&]() -> Answer {
    try {
      if (!tensors || tensors->find({input, slot}) == tensors->end())
        return Answer(Status{ErrorCode::InvalidArgument,
                             "missing authorized native tensor input",
                             FailureReason::UnauthorizedRead,
                             {FailureOrigin::Protocol, FailureScope::Group}});
      auto acquired =
          tensors->at({input, slot}).acquire(region, query.cancellation);
      if (!acquired.ok())
        return acquired;
      if (!gpu || query.backend != Backend::Gpu)
        return Answer(Status{ErrorCode::BackendUnavailable,
                             "native tensor requires GPU callback"});
      auto window = acquired.take_value();
      auto backing = execution_internal::ResultNativeScope::input(
          window, consume_work, query.cancellation);
      if (!backing.ok())
        return Answer(backing.status());
      return execution_internal::ResultWindowAccess::replace_backing(
          std::move(window), backing.take_value());
    } catch (const std::bad_alloc&) {
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return Answer(Status{ErrorCode::OperationFailed, {}});
    }
  }();
  if (!answer.ok()) {
    if (failure_observer)
      failure_observer(answer.status());
    if (failure) {
      auto expected = ErrorCode::Ok;
      failure->compare_exchange_strong(expected, answer.status().code);
    }
  }
  return answer;
}
Result<std::shared_ptr<const FragmentAtlas>>
ResultProgramPhase::acquire_native_atlas(std::uint32_t input,
                                         std::uint32_t slot) const {
  using Answer = Result<std::shared_ptr<const FragmentAtlas>>;
  auto answer = [&]() -> Answer {
    try {
      if (!tensors || tensors->find({input, slot}) == tensors->end())
        return Answer(Status{ErrorCode::InvalidArgument,
                             "missing authorized atlas input",
                             FailureReason::UnauthorizedRead,
                             {FailureOrigin::Protocol, FailureScope::Group}});
      if (!tensors->at({input, slot}).payload_authorized_)
        return Answer(Status{ErrorCode::InvalidArgument,
                             "descriptor-only Need cannot create an atlas",
                             FailureReason::UnauthorizedRead,
                             {FailureOrigin::Protocol, FailureScope::Group}});
      if (!gpu || query.backend != Backend::Gpu)
        return Answer(Status{ErrorCode::InvalidArgument,
                             "native atlas requires GPU callback",
                             FailureReason::UnauthorizedRead,
                             {FailureOrigin::Protocol, FailureScope::Group}});
      return execution_internal::ResultNativeScope::atlas(
          input, slot, tensors->at({input, slot}), consume_work,
          query.cancellation);
    } catch (const std::bad_alloc&) {
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return Answer(Status{ErrorCode::OperationFailed, {}});
    }
  }();
  if (!answer.ok()) {
    if (failure_observer)
      failure_observer(answer.status());
    if (failure) {
      auto expected = ErrorCode::Ok;
      failure->compare_exchange_strong(expected, answer.status().code);
    }
  }
  return answer;
}
Status ResultProgramPhase::read_tensor(
    std::uint32_t input, std::uint32_t slot,
    const std::vector<std::uint64_t>& coordinate, void* destination,
    std::size_t bytes) const {
  Status status{ErrorCode::InvalidArgument, "missing authorized image input"};
  try {
    if (tensors) {
      auto found = tensors->find({input, slot});
      if (found != tensors->end())
        status = found->second.read(coordinate, destination, bytes,
                                    query.cancellation);
    }
  } catch (const std::bad_alloc&) {
    status = Status{ErrorCode::ResourceExhausted, {}};
  } catch (...) {
    status = Status{ErrorCode::OperationFailed, {}};
  }
  if (!status.ok()) {
    if (status.code == ErrorCode::InvalidArgument) {
      status.reason = FailureReason::UnauthorizedRead;
      status.detail = {FailureOrigin::Protocol, FailureScope::Group};
    }
    if (failure_observer)
      failure_observer(status);
    if (failure) {
      auto expected = ErrorCode::Ok;
      failure->compare_exchange_strong(expected, status.code);
    }
  }
  return status;
}
void ResultContinuation::reset() noexcept {
  if (destroy_)
    destroy_(storage_.data());
  destroy_ = nullptr;
  poll_ = nullptr;
  storage_ = {};
  prepared_.reset();
  definition_.reset();
  resources_ = {};
  payload_bound_.reset();
  payload_limit_.reset();
}
ResultContinuation::~ResultContinuation() noexcept {
  reset();
}
ResultContinuation::ResultContinuation(ResultContinuation&& other) noexcept {
  *this = std::move(other);
}
ResultContinuation& ResultContinuation::operator=(
    ResultContinuation&& other) noexcept {
  if (this != &other) {
    reset();
    storage_ = std::move(other.storage_);
    definition_ = std::move(other.definition_);
    prepared_ = std::move(other.prepared_);
    resources_ = std::move(other.resources_);
    payload_bound_ = std::move(other.payload_bound_);
    payload_limit_ = std::exchange(other.payload_limit_, {});
    destroy_ = std::exchange(other.destroy_, nullptr);
    poll_ = std::exchange(other.poll_, nullptr);
  }
  return *this;
}
Result<ResultProgramPoll> ResultContinuation::poll(
    const ResultProgramPhase& phase) {
  if (active_.test_and_set(std::memory_order_acquire)) {
    Status rejected{ErrorCode::InvalidArgument,
                    {},
                    FailureReason::None,
                    {FailureOrigin::Protocol, FailureScope::Group}};
    try {
      rejected.message = "concurrent or reentrant Result poll";
    } catch (const std::bad_alloc&) {
      // Preserve the protocol rejection even if its diagnostic cannot allocate.
    }
    return Result<ResultProgramPoll>(std::move(rejected));
  }
  struct Reset {
    std::atomic_flag& active;
    ~Reset() { active.clear(std::memory_order_release); }
  } reset{active_};
  // Keep admission until the callback's exception and publication fences
  // retire.
  return poll_guarded(phase);
}
Result<ResultProgramPoll> ResultContinuation::poll_guarded(
    const ResultProgramPhase& phase) try {
  if (!poll_)
    return Result<ResultProgramPoll>(Status{ErrorCode::Stale, {}});
  if (phase.failure_latch) {
    auto failed = phase.failure_latch->snapshot();
    if (!failed.ok())
      return Result<ResultProgramPoll>(std::move(failed));
  }
  if (phase.failure && phase.failure->load() != ErrorCode::Ok) {
    const auto code = phase.failure->load();
    const auto bounded =
        payload_bound_ ? payload_bound_->status() : Status::success();
    return Result<ResultProgramPoll>(
        !bounded.ok() && bounded.code == code ? bounded : Status{code, {}});
  }
  if (phase.query.cancellation.cancelled())
    return Result<ResultProgramPoll>(Status{ErrorCode::Cancelled, {}});
  const auto veto_retry = [&](const Status& status) {
    if (phase.failure_latch &&
        !plugin_internal::FailureLatch::retryable_backend_failure(status))
      phase.failure_latch->veto_backend_retry();
  };
  const auto record_returned_failure = [&](const Status& status) {
    if (phase.failure_latch &&
        (status.code != ErrorCode::OperationFailed ||
         status.reason != FailureReason::None ||
         status.detail.origin != FailureOrigin::Unspecified ||
         status.detail.scope != FailureScope::Unspecified)) {
      // Capture fixed failure fields before any later diagnostic can allocate.
      static_cast<void>(phase.failure_latch->record(status));
    }
  };
  if (!prepared_ && !resources_.size() && !phase.query.resources.size()) {
    auto answer = poll_(storage_.data(), phase);
    if (!answer.ok())
      record_returned_failure(answer.status());
    return answer;
  }
  auto query = phase.query;
  query.resources = resources_;
  query.prepared = prepared_;
  ResultProgramPhase normalized{query,           phase.results,
                                phase.io,        phase.allocator,
                                phase.resources, phase.consume_work,
                                phase.failure,   phase.failure_observer};
  normalized.failure_latch = phase.failure_latch;
  normalized.tensors = phase.tensors;
  normalized.cpu_parallel = phase.cpu_parallel;
  normalized.cpu_tiles = phase.cpu_tiles;
  normalized.gpu = phase.gpu;
  normalized.gpu_status = phase.gpu_status;
  normalized.association = phase.association;
  normalized.report_numeric = phase.report_numeric;
  normalized.checkpoint_before = phase.checkpoint_before;
  normalized.checkpoint_publish = phase.checkpoint_publish;
  normalized.block = phase.block;
  normalized.discover = phase.discover;
  const bool terminal =
      prepared_ && query.output_index < prepared_->traits().outputs.size() &&
      prepared_->traits().outputs[query.output_index].observation_kind ==
          ObservationKind::RequestRecord;
  const auto stopped = [&]() {
    if (normalized.failure_latch) {
      auto status = normalized.failure_latch->snapshot();
      if (!status.ok())
        return status;
    }
    auto bounded =
        payload_bound_ ? payload_bound_->status() : Status::success();
    if (normalized.failure && normalized.failure->load() != ErrorCode::Ok) {
      const auto code = normalized.failure->load();
      return !bounded.ok() && bounded.code == code ? bounded : Status{code, {}};
    }
    if (!bounded.ok())
      return bounded;
    return query.cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                          : Status::success();
  };
  const auto reject_bound = [&](Status status) {
    veto_retry(status);
    auto first = stopped();
    if (!first.ok())
      status = std::move(first);
    if (normalized.failure_latch)
      status = normalized.failure_latch->record(status);
    if (normalized.failure) {
      auto expected = ErrorCode::Ok;
      if (!normalized.failure->compare_exchange_strong(expected, status.code) &&
          expected != status.code)
        status = Status{expected, {}};
    }
    if (normalized.failure_observer) {
      try {
        normalized.failure_observer(status);
      } catch (...) {
        if (normalized.failure_latch)
          normalized.failure_latch->veto_backend_retry();
      }
    }
    return Result<ResultProgramPoll>(status);
  };
  if (terminal || payload_limit_) {
    auto status = stopped();
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
  }
  if (payload_limit_ && !payload_bound_) {
    try {
      payload_bound_ =
          std::allocate_shared<plugin_internal::ResultPayloadBound>(
              ResourceAllocator<plugin_internal::ResultPayloadBound>(
                  normalized.resources),
              *payload_limit_);
    } catch (const std::bad_alloc&) {
      return reject_bound(Status{ErrorCode::ResourceExhausted, {}});
    }
  }
  if (payload_bound_) {
    auto status = payload_bound_->capture(normalized);
    if (!status.ok())
      return reject_bound(status);
  }
  auto answer = [&]() -> Result<ResultProgramPoll> {
    try {
      return poll_(storage_.data(), normalized);
    } catch (...) {
      return reject_bound(plugin_internal::current_operation_exception());
    }
  }();
  if (!answer.ok())
    record_returned_failure(answer.status());
  if (terminal || payload_limit_) {
    auto status = stopped();
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
  }
  if (answer.ok() && payload_bound_) {
    if (const auto* publication =
            std::get_if<ResultPublication>(&answer.value())) {
      auto status = payload_bound_->check(publication->result, normalized);
      if (!status.ok())
        return reject_bound(status);
    }
  }
  if (answer.ok() && terminal) {
    auto outcome = answer.take_value();
    const auto reject = [&](const char* message) {
      return reject_bound(
          Status{ErrorCode::InvalidArgument,
                 message,
                 FailureReason::None,
                 {FailureOrigin::Protocol, FailureScope::Group}});
    };
    if (auto* publication = std::get_if<ResultPublication>(&outcome)) {
      if (!publication->complete || !publication->result.valid() ||
          !query.output.result_schema ||
          !publication->result.schema().same_schema(
              *query.output.result_schema) ||
          !publication->result.matches_scope(query.semantic_key) ||
          !publication->result.owned_by(normalized.resources))
        return reject("invalid terminal Result publication");
      auto descriptor = publication->result.descriptor(true);
      if (!descriptor.ok() &&
          descriptor.status().code == ErrorCode::ResourceExhausted)
        return Result<ResultProgramPoll>(descriptor.status());
      if (!descriptor.ok() || !descriptor.value().sealed())
        return reject("terminal Result requires a sealed publication");
      if (!query.output.result_schema->tensors.empty()) {
        if (query.tensor_slot >= query.output.result_schema->tensors.size())
          return reject("invalid terminal Result tensor slot");
        FootprintLimits limits;
        limits.consume_work = normalized.consume_work;
        limits.cancellation = query.cancellation;
        auto demand = query.tensor_outputs
                          ? Result<Footprint>(*query.tensor_outputs)
                          : Footprint::all(query.output.result_schema
                                               ->tensors[query.tensor_slot]
                                               .sample_shape(),
                                           limits);
        if (!demand.ok())
          return Result<ResultProgramPoll>(demand.status());
        if (descriptor.value().tensor_coverage(query.tensor_slot) !=
            demand.value())
          return reject("terminal Result coverage differs from captured query");
      }
      publication->result.request_record_ = true;
    }
    return Result<ResultProgramPoll>(std::move(outcome));
  }
  return answer;
} catch (...) {
  auto status = plugin_internal::current_operation_exception();
  if (phase.failure_latch)
    status = phase.failure_latch->record(status);
  if (phase.failure) {
    auto expected = ErrorCode::Ok;
    if (!phase.failure->compare_exchange_strong(expected, status.code) &&
        expected != status.code)
      status = Status{expected, {}};
  }
  // Observers belong to the host; their exceptions must not escape a fence.
  if (phase.failure_observer) {
    try {
      phase.failure_observer(status);
    } catch (...) {
      if (phase.failure_latch) {
        try {
          phase.failure_latch->veto_backend_retry();
        } catch (...) {
        }
      }
    }
  }
  return Result<ResultProgramPoll>(std::move(status));
}
}  // namespace ps
