#include "plugin/result_c_member.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "data/result_window_access.hpp"
#include "execution/cpu_range_context.hpp"
#include "plugin/result_c_poll_lease.hpp"

namespace ps::plugin_internal::result_c {
namespace {
constexpr std::uint64_t quality_handle_base = std::uint64_t{1} << 63;
Result<ResourceLease> bridge_capacity(uint64_t bytes) {
  if (const auto* root = resource_internal::metadata_budget())
    return root->reserve(ResourceCapacity::host(bytes, bytes));
  return Result<ResourceLease>(ResourceLease{});
}
template <class Function>
Status with_coordinate(const uint64_t* at, uint32_t rank, Function function) {
  auto lease = bridge_capacity(rank * sizeof(uint64_t));
  if (!lease.ok())
    return lease.status();
  return function(std::vector<uint64_t>(at, at + rank));
}
struct ManagedRegion {
  ResourceLease lease;
  Region region;
};
Result<ManagedRegion> region(const ps_result_region_v2* source,
                             const std::vector<std::uint64_t>& shape) {
  if (!source ||
      reinterpret_cast<std::uintptr_t>(source) % alignof(ps_result_region_v2) ||
      source->struct_size != sizeof(*source) || source->rank != shape.size())
    return Result<ManagedRegion>(invalid("invalid Result region"));
  auto admitted = bridge_capacity(source->rank * sizeof(RegionDimension));
  if (!admitted.ok())
    return Result<ManagedRegion>(admitted.status());
  std::vector<RegionDimension> dimensions;
  dimensions.reserve(source->rank);
  for (std::uint32_t i = 0; i < source->rank; ++i)
    dimensions.push_back({source->offset[i], source->extent[i]});
  Region target(std::move(dimensions));
  auto valid = target.validate(shape);
  return valid.ok() ? Result<ManagedRegion>(ManagedRegion{admitted.take_value(),
                                                          std::move(target)})
                    : Result<ManagedRegion>(valid);
}
}  // namespace
class CResultMemberBridge final {
 public:
  CResultMemberBridge(std::shared_ptr<const Definition> definition,
                      MutableBuffer bytes, bool owns_payload = true,
                      std::shared_ptr<std::uint64_t> handles = {},
                      std::shared_ptr<FailureLatch> joint_failure = {})
      : definition_(std::move(definition)),
        bytes_(std::move(bytes)),
        retained_(std::less<std::uint64_t>{},
                  ResourceAllocator<Retained::value_type>{}),
        owns_payload_(owns_payload),
        joint_failure_(std::move(joint_failure)),
        handles_(std::move(handles)),
        next_handle_(handles_ ? *handles_ : owned_next_handle_) {}
  ~CResultMemberBridge() noexcept {
    if (entered_ && owns_payload_) {
      try {
        definition_->api.destroy(definition_->api.user_data, bytes_.data());
      } catch (...) {
      }
    }
  }
  using Borrow = std::function<int(const ps_result_query_v2*,
                                   const ps_result_services_v2*)>;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase,
                                 const Borrow& borrow = {}) {
    phase_ = &phase;
    owner_ = std::this_thread::get_id();
    active_ = true;
    auto lease =
        std::allocate_shared<Lease>(ResourceAllocator<Lease>(phase.resources));
    lease->owner = this;
    lease->cancellation = phase.query.cancellation;
    leases_.push_back(lease);
    struct Exit {
      CResultMemberBridge& self;
      std::shared_ptr<Lease> lease;
      ~Exit() {
        lease->retire();
        self.active_ = false;
        self.phase_ = nullptr;
        self.scratch_.clear();
      }
    } exit{*this, lease};
    need_ = {};
    published_ = {};
    published_complete_ = false;
    discovery_pending_ = false;
    if (!entered_) {
      window_failure_ =
          phase.failure_latch
              ? phase.failure_latch
              : std::allocate_shared<FailureLatch>(
                    ResourceAllocator<FailureLatch>(phase.resources));
      windows_ =
          Windows(std::less<std::uint64_t>{},
                  ResourceAllocator<Windows::value_type>(phase.resources));
      relations_ =
          Relations(std::less<std::uint64_t>{},
                    ResourceAllocator<Relations::value_type>(phase.resources));
      window_records_ = ResourceVector<std::shared_ptr<WindowRecord>>(
          ResourceAllocator<std::shared_ptr<WindowRecord>>(phase.resources));
      retained_ =
          Retained(std::less<std::uint64_t>{},
                   ResourceAllocator<Retained::value_type>(phase.resources));
      block_states_ = BlockStates(
          std::less<std::uint64_t>{},
          ResourceAllocator<BlockStates::value_type>(phase.resources));
      discoveries_ = Discoveries(
          std::less<std::uint64_t>{},
          ResourceAllocator<Discoveries::value_type>(phase.resources));
    }
    auto services = services_for(lease.get(), phase);
    QueryFrame frame(phase.query, *definition_, phase.resources);
    if (!frame.status.ok())
      return Result<ResultProgramPoll>(frame.status);
    const auto& query = frame.query;
    if (!entered_) {
      entered_ = true;
      if (!borrow) {
        const auto result = definition_->api.start(
            definition_->api.user_data, bytes_.data(), &query, &services);
        auto status = callback_failure();
        if (status.ok())
          status = callback_outcome(result);
        if (!status.ok())
          return Result<ResultProgramPoll>(status);
        lease->retire();
        lease = std::allocate_shared<Lease>(
            ResourceAllocator<Lease>(phase.resources));
        lease->owner = this;
        lease->cancellation = phase.query.cancellation;
        leases_.push_back(lease);
        exit.lease = lease;
        services = services_for(lease.get(), phase);
      }
    }
    const auto result =
        borrow ? borrow(&query, &services)
               : definition_->api.poll(definition_->api.user_data,
                                       bytes_.data(), &query, &services);
    auto failure = callback_failure();
    if (!failure.ok())
      return Result<ResultProgramPoll>(failure);
    if (result == PS_RESULT_NEED_V2) {
      if (published_.valid())
        return Result<ResultProgramPoll>(invalid("Need after publication"));
      return Result<ResultProgramPoll>(std::move(need_));
    }
    if (result != PS_RESULT_PUBLISH_V2)
      return Result<ResultProgramPoll>(callback_outcome(result ? result : 1));
    if (!need_.tensors.empty() || !need_.results.empty() || !need_.io.empty())
      return Result<ResultProgramPoll>(
          invalid("publication with pending Need"));
    if (!published_.valid())
      return Result<ResultProgramPoll>(invalid("missing Result publication"));
    return Result<ResultProgramPoll>(
        ResultPublication{published_, published_complete_});
  }

  bool terminal_failure_conflicts() const noexcept {
    return published_.valid() || !need_.tensors.empty() ||
           !need_.results.empty() || !need_.io.empty();
  }
  Status failure_status() const {
    if (violation_.load())
      return {ErrorCode::InvalidArgument,
              "expired C Result member services",
              FailureReason::UnauthorizedRead,
              {FailureOrigin::Protocol, FailureScope::Group}};
    return callback_failure();
  }

 private:
  Status callback_failure() const {
    if (window_failure_) {
      auto status = window_failure_->snapshot();
      if (!status.ok())
        return status;
    }
    if (violation_.load())
      return {ErrorCode::InvalidArgument,
              "Result service thread/lease violation",
              FailureReason::UnauthorizedRead,
              {FailureOrigin::Protocol, FailureScope::Group}};
    return failure_;
  }
  Status callback_outcome(int result) const {
    auto status = outcome(result);
    if (status.code == ErrorCode::BackendUnavailable && publication_started_)
      return {
          ErrorCode::OperationFailed,
          "Result plugin reported backend unavailability after publication"};
    return status;
  }
  Status publication(Status status) {
    if (discovery_pending_)
      return invalid("GPU discovery requires supply before publication");
    if (status.ok())
      publication_started_ = true;
    return status;
  }
  using Lease = CResultPollLease;
  using Retained = std::map<
      std::uint64_t, ResultTensorInput, std::less<std::uint64_t>,
      ResourceAllocator<std::pair<const std::uint64_t, ResultTensorInput>>>;
  struct WindowRecord {
    ResultTensorReadWindow window;
    ResourceBudget budget;
    CancellationToken cancellation;
    std::shared_ptr<std::atomic<ErrorCode>> failure;
    std::shared_ptr<FailureLatch> first_failure, joint_failure;
    std::atomic<bool> active{true};
    std::uint64_t handles = 1;
    bool group_operational_failures = false;
    Status record(Status status) const {
      if (joint_failure) {
        auto shared = joint_failure->snapshot();
        if (!shared.ok())
          return shared;
      }
      if (!status.ok()) {
        if (status.code == ErrorCode::InvalidArgument &&
            status.reason == FailureReason::None) {
          status.reason = FailureReason::UnauthorizedRead;
          status.detail = {FailureOrigin::Protocol, FailureScope::Group};
        }
        if (first_failure)
          status = first_failure->record(status);
        if (group_operational_failures && joint_failure &&
            ((status.code != ErrorCode::Cancelled &&
              status.code != ErrorCode::Stale) ||
             status.detail.scope == FailureScope::Run ||
             status.detail.scope == FailureScope::Group))
          status = joint_failure->record(status);
        if (failure) {
          auto expected = ErrorCode::Ok;
          failure->compare_exchange_strong(expected, status.code);
        }
      }
      return status;
    }
  };
  using Windows = std::map<
      std::uint64_t, std::shared_ptr<WindowRecord>, std::less<std::uint64_t>,
      ResourceAllocator<
          std::pair<const std::uint64_t, std::shared_ptr<WindowRecord>>>>;
  using Relations = std::map<
      std::uint64_t, ResultRelation, std::less<std::uint64_t>,
      ResourceAllocator<std::pair<const std::uint64_t, ResultRelation>>>;
  using BlockStates =
      std::map<std::uint64_t, ResultRef, std::less<std::uint64_t>,
               ResourceAllocator<std::pair<const std::uint64_t, ResultRef>>>;
  using Discoveries = std::map<
      std::uint64_t, std::shared_ptr<const ResultDiscoveryReceipt>,
      std::less<std::uint64_t>,
      ResourceAllocator<std::pair<
          const std::uint64_t, std::shared_ptr<const ResultDiscoveryReceipt>>>>;
  static Status window_read(
      WindowRecord& record, const std::uint64_t* at, std::uint32_t rank,
      const std::function<Status(const std::vector<std::uint64_t>&)>& read,
      unsigned lookups = 1) {
    if (record.joint_failure) {
      auto shared = record.joint_failure->snapshot();
      if (!shared.ok())
        return shared;
    }
    if (!record.active.load() || !array(at, rank, 8))
      return record.record(invalid("expired or malformed image window read"));
    if (record.cancellation.cancelled())
      return record.record(Status{ErrorCode::Cancelled, {}});
    auto work = record.budget.consume({rank + 1});
    if (!work.ok())
      return record.record(work);
    auto lookup =
        execution_internal::ResultWindowAccess::read_work(record.window);
    if (!lookup.ok())
      return record.record(lookup.status());
    for (unsigned i = 0; i < lookups; ++i) {
      work = record.budget.consume({lookup.value()});
      if (!work.ok())
        return record.record(work);
    }
    auto capacity = record.budget.reserve(ResourceCapacity::host(
        rank * sizeof(std::uint64_t), rank * sizeof(std::uint64_t)));
    if (!capacity.ok())
      return record.record(capacity.status());
    return record.record(read(std::vector<std::uint64_t>(at, at + rank)));
  }
  static int window_row(void* raw, const std::uint64_t* at, std::uint32_t rank,
                        ps_result_tensor_row_v2* output) noexcept {
    auto& record = *static_cast<WindowRecord*>(raw);
    try {
      if (!output ||
          reinterpret_cast<std::uintptr_t>(output) %
              alignof(ps_result_tensor_row_v2) ||
          output->struct_size != sizeof(*output))
        return code(record.record(invalid("invalid image row destination")));
      return code(window_read(record, at, rank, [&](const auto& coordinate) {
        auto run = record.window.row_run(coordinate);
        if (!run.ok())
          return run.status();
        *output = {sizeof(*output), run.value().data, run.value().samples,
                   run.value().bytes, run.value().sample_stride_bytes};
        return Status::success();
      }));
    } catch (const std::bad_alloc&) {
      return code(record.record({ErrorCode::ResourceExhausted, {}}));
    } catch (...) {
      return code(record.record({ErrorCode::OperationFailed, {}}));
    }
  }
  static int window_rectangle(void* raw, const std::uint64_t* at,
                              std::uint32_t rank,
                              ps_result_tensor_rectangle_v2* output) noexcept {
    auto& record = *static_cast<WindowRecord*>(raw);
    try {
      if (!output ||
          reinterpret_cast<std::uintptr_t>(output) %
              alignof(ps_result_tensor_rectangle_v2) ||
          output->struct_size != sizeof(*output))
        return code(
            record.record(invalid("invalid image rectangle destination")));
      return code(window_read(
          record, at, rank,
          [&](const auto& coordinate) {
            auto run = record.window.rectangle_run(coordinate);
            if (!run.ok())
              return run.status();
            *output = {sizeof(*output),
                       {sizeof(ps_result_tensor_row_v2), run.value().row.data,
                        run.value().row.samples, run.value().row.bytes,
                        run.value().row.sample_stride_bytes},
                       run.value().rows,
                       run.value().row_stride_bytes};
            return Status::success();
          },
          2));
    } catch (const std::bad_alloc&) {
      return code(record.record({ErrorCode::ResourceExhausted, {}}));
    } catch (...) {
      return code(record.record({ErrorCode::OperationFailed, {}}));
    }
  }
  Status acquire_window(
      const ResultTensorInput& source, const ps_result_region_v2* requested,
      ps_result_tensor_window_v2* output, std::uint64_t* handle,
      std::optional<std::pair<std::uint32_t, std::uint32_t>> native = {}) {
    if (!output || !handle ||
        reinterpret_cast<std::uintptr_t>(output) %
            alignof(ps_result_tensor_window_v2) ||
        output->struct_size != sizeof(*output) ||
        next_handle_ >= quality_handle_base || windows_.size() >= 1024)
      return invalid("invalid image window request");
    auto bounds = region(requested, source.spec().sample_shape());
    if (!bounds.ok())
      return bounds.status();
    auto acquired =
        native
            ? phase_->acquire_native_tensor(native->first, native->second,
                                            bounds.value().region)
            : source.acquire(bounds.value().region, phase_->query.cancellation);
    if (!acquired.ok())
      return acquired.status();
    auto record = std::allocate_shared<WindowRecord>(
        ResourceAllocator<WindowRecord>(phase_->resources));
    record->window = acquired.take_value();
    record->budget = phase_->resources;
    record->cancellation = phase_->query.cancellation;
    record->failure = phase_->failure;
    record->first_failure = window_failure_;
    record->joint_failure = joint_failure_;
    record->group_operational_failures = definition_->joint.contract == 2;
    const auto token = next_handle_++;
    windows_.emplace(token, record);
    window_records_.push_back(record);
    const auto& layout = source.spec().layout;
    *output = {sizeof(*output),
               record->window.row_axis().value_or(UINT32_MAX),
               record->window.sample_axis(),
               layout.spatial && layout.channel_axis
                   ? *layout.channel_axis + static_cast<std::uint32_t>(
                                                source.spec().batch_axes.size())
                   : UINT32_MAX,
               source.object_id(),
               *requested,
               record.get(),
               window_row,
               window_rectangle};
    *handle = token;
    return Status::success();
  }
  static int acquire_tensor_window(void* raw, std::uint32_t input,
                                   std::uint32_t slot,
                                   const ps_result_region_v2* requested,
                                   ps_result_tensor_window_v2* output,
                                   std::uint64_t* handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      if (!state.phase_->tensors)
        return invalid("missing image Need");
      auto found = state.phase_->tensors->find({input, slot});
      return found == state.phase_->tensors->end()
                 ? invalid("missing image Need")
                 : state.acquire_window(found->second, requested, output,
                                        handle);
    });
  }
  static int acquire_native_tensor_window(void* raw, std::uint32_t input,
                                          std::uint32_t slot,
                                          const ps_result_region_v2* requested,
                                          ps_result_tensor_window_v2* output,
                                          std::uint64_t* handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      if (!state.phase_->tensors)
        return invalid("missing native tensor capability");
      auto found = state.phase_->tensors->find({input, slot});
      return found == state.phase_->tensors->end()
                 ? invalid("missing native tensor capability")
                 : state.acquire_window(found->second, requested, output,
                                        handle, std::make_pair(input, slot));
    });
  }
  static int acquire_retained_window(void* raw, std::uint64_t input,
                                     const ps_result_region_v2* requested,
                                     ps_result_tensor_window_v2* output,
                                     std::uint64_t* handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      auto found = state.retained_.find(input);
      return found == state.retained_.end()
                 ? invalid("expired image capability")
                 : state.acquire_window(found->second, requested, output,
                                        handle);
    });
  }
  static int retain_window(void* raw, std::uint64_t handle,
                           std::uint64_t* retained) {
    return call(raw, [&](CResultMemberBridge& state) {
      auto found = state.windows_.find(handle);
      if (!retained || found == state.windows_.end() ||
          state.next_handle_ >= quality_handle_base ||
          state.windows_.size() >= 1024)
        return invalid("expired image window handle");
      auto record = found->second;
      const auto token = state.next_handle_++;
      state.windows_.emplace(token, record);
      ++record->handles;
      *retained = token;
      return Status::success();
    });
  }
  static int release_window(void* raw, std::uint64_t handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      auto found = state.windows_.find(handle);
      if (found == state.windows_.end())
        return invalid("expired image window handle");
      auto record = found->second;
      state.windows_.erase(found);
      if (!--record->handles) {
        record->active = false;
        record->window = {};
      }
      return Status::success();
    });
  }
  static int make_mapping(void* raw, std::uint32_t output_slot,
                          std::uint32_t input, std::uint32_t target,
                          std::uint32_t input_slot, std::uint32_t roles,
                          const ps_result_region_v2* output,
                          const ps_result_mapped_axis_v2* axes,
                          std::uint32_t count, std::uint64_t* handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      if (!handle || !array(axes, count, 8) ||
          input >= state.phase_->query.inputs.size() ||
          state.next_handle_ >= quality_handle_base ||
          state.relations_.size() >= 1024)
        return invalid("invalid mapped relation request");
      const auto& query = state.phase_->query;
      auto scratch = bridge_capacity(16 * sizeof(std::uint64_t) +
                                     count * sizeof(ResultMappedAxis));
      if (!scratch.ok())
        return scratch.status();
      std::vector<std::uint64_t> output_shape, input_shape;
      if (!query.output.result_schema ||
          output_slot >= query.output.result_schema->tensors.size())
        return invalid("invalid mapping output slot");
      output_shape =
          query.output.result_schema->tensors[output_slot].sample_shape();
      const auto& source = query.inputs[input];
      if (target == PS_RESULT_TARGET_TENSOR_V2 && source.result_schema &&
          input_slot < source.result_schema->tensors.size())
        input_shape = source.result_schema->tensors[input_slot].sample_shape();
      else
        return invalid("invalid mapping input target");
      if (count != input_shape.size())
        return invalid("mapping input rank mismatch");
      auto coverage = region(output, output_shape);
      if (!coverage.ok())
        return coverage.status();
      ResourceVector<ResultMappedAxis> mapping(
          ResourceAllocator<ResultMappedAxis>(state.phase_->resources));
      for (std::uint32_t axis = 0; axis < count; ++axis)
        mapping.push_back({axes[axis].output_axis, axes[axis].source_origin,
                           axes[axis].step, axes[axis].extent,
                           axes[axis].output_origin});
      auto made = ResultRelation::mapped(
          state.phase_->resources, output_shape, coverage.value().region,
          input_shape,
          std::vector<ResultMappedAxis>(mapping.begin(), mapping.end()),
          {input, roles, 0, 0, static_cast<ResultSupportTarget>(target),
           input_slot});
      if (!made.ok())
        return made.status();
      const auto token = state.next_handle_++;
      state.relations_.emplace(token, made.take_value());
      *handle = token;
      return Status::success();
    });
  }
  static int make_tensor_cartesian(void* raw, uint32_t output_slot,
                                   uint32_t input, uint32_t input_slot,
                                   uint32_t roles, uint64_t first,
                                   uint64_t count, uint32_t guarantee,
                                   uint64_t* handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      const auto& query = state.phase_->query;
      if (!handle || !query.output.result_schema ||
          output_slot >= query.output.result_schema->tensors.size() ||
          input >= query.inputs.size() || !query.inputs[input].result_schema ||
          input_slot >= query.inputs[input].result_schema->tensors.size() ||
          !roles || (roles & ~7U) ||
          (guarantee != PS_RESULT_EXACT_V2 &&
           guarantee != PS_RESULT_CONSERVATIVE_V2) ||
          state.next_handle_ >= quality_handle_base ||
          state.relations_.size() >= 1024)
        return invalid("invalid Cartesian tensor relation request");
      const auto cardinality =
          [](const ResultTensorSpec& tensor) -> Result<uint64_t> {
        uint64_t count = 1;
        const auto rank =
            tensor.batch_axes.size() + tensor.descriptor.shape.size();
        for (std::size_t axis = 0; axis < rank; ++axis) {
          const auto extent =
              axis < tensor.batch_axes.size()
                  ? tensor.batch_axes[axis]
                  : tensor.descriptor.shape[axis - tensor.batch_axes.size()];
          if (extent && count > UINT64_MAX / extent)
            return Result<uint64_t>(Status{ErrorCode::ResourceExhausted,
                                           "Cartesian tensor domain overflow"});
          count *= extent;
        }
        return Result<uint64_t>(count);
      };
      const auto source_count =
          cardinality(query.inputs[input].result_schema->tensors[input_slot]);
      const auto output_count =
          cardinality(query.output.result_schema->tensors[output_slot]);
      if (!source_count.ok())
        return source_count.status();
      if (!output_count.ok())
        return output_count.status();
      if (first > source_count.value() || count > source_count.value() - first)
        return invalid("Cartesian tensor support exceeds source domain");
      auto relation = ResultRelation::cartesian(
          state.phase_->resources, output_count.value(),
          {input, roles, first, count, ResultSupportTarget::Tensor, input_slot},
          static_cast<DependencyGuarantee>(guarantee));
      if (!relation.ok())
        return relation.status();
      const auto token = state.next_handle_++;
      state.relations_.emplace(token, relation.take_value());
      *handle = token;
      return Status::success();
    });
  }
  static int make_reshape(void* raw, uint32_t output_slot, uint32_t input,
                          uint32_t input_slot, uint32_t roles,
                          const ps_result_region_v2* output,
                          const ps_result_region_v2* source_window,
                          uint64_t* handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      const auto& query = state.phase_->query;
      if (!handle || !query.output.result_schema ||
          output_slot >= query.output.result_schema->tensors.size() ||
          input >= query.inputs.size() || !query.inputs[input].result_schema ||
          input_slot >= query.inputs[input].result_schema->tensors.size() ||
          state.next_handle_ >= quality_handle_base ||
          state.relations_.size() >= 1024)
        return invalid("invalid reshape relation request");
      auto shape_lease = bridge_capacity(16 * sizeof(uint64_t));
      if (!shape_lease.ok())
        return shape_lease.status();
      const auto output_shape =
          query.output.result_schema->tensors[output_slot].sample_shape();
      const auto input_shape =
          query.inputs[input].result_schema->tensors[input_slot].sample_shape();
      auto coverage = region(output, output_shape);
      if (!coverage.ok())
        return coverage.status();
      auto source = region(source_window, input_shape);
      if (!source.ok())
        return source.status();
      auto made = ResultRelation::reshape(
          state.phase_->resources, output_shape, coverage.value().region,
          input_shape, source.value().region,
          {input, roles, 0, 0, ResultSupportTarget::Tensor, input_slot});
      if (!made.ok())
        return made.status();
      const auto token = state.next_handle_++;
      state.relations_.emplace(token, made.take_value());
      *handle = token;
      return Status::success();
    });
  }
  static int make_neighborhood(void* raw, uint32_t output_slot, uint32_t input,
                               uint32_t input_slot, uint32_t roles,
                               const uint64_t* radii, uint32_t rank,
                               uint32_t periodic, uint64_t* handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      const auto& query = state.phase_->query;
      if (!handle || !rank || !array(radii, rank, 8) || periodic > 1 ||
          !query.output.result_schema ||
          output_slot >= query.output.result_schema->tensors.size() ||
          input >= query.inputs.size() || !query.inputs[input].result_schema ||
          input_slot >= query.inputs[input].result_schema->tensors.size() ||
          state.next_handle_ >= quality_handle_base ||
          state.relations_.size() >= 1024)
        return invalid("invalid neighborhood relation request");
      auto scratch = bridge_capacity(24 * sizeof(uint64_t));
      if (!scratch.ok())
        return scratch.status();
      const auto output_shape =
          query.output.result_schema->tensors[output_slot].sample_shape();
      const auto input_shape =
          query.inputs[input].result_schema->tensors[input_slot].sample_shape();
      if (output_shape != input_shape || rank != input_shape.size())
        return invalid("neighborhood tensor domains differ");
      auto made = ResultRelation::neighborhood(
          state.phase_->resources, output_shape,
          std::vector<uint64_t>(radii, radii + rank), periodic != 0,
          {input, roles, 0, 0, ResultSupportTarget::Tensor, input_slot});
      if (!made.ok())
        return made.status();
      const auto token = state.next_handle_++;
      state.relations_.emplace(token, made.take_value());
      *handle = token;
      return Status::success();
    });
  }
  static int release_relation(void* raw, std::uint64_t handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      return state.relations_.erase(handle)
                 ? Status::success()
                 : invalid("expired relation handle");
    });
  }
  static int make_prefix(void* raw, uint32_t output_slot, uint32_t input,
                         uint32_t input_slot, uint32_t roles,
                         uint64_t* handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      const auto& query = state.phase_->query;
      if (!handle || !query.output.result_schema ||
          output_slot >= query.output.result_schema->tensors.size() ||
          input >= query.inputs.size() || !query.inputs[input].result_schema ||
          input_slot >= query.inputs[input].result_schema->tensors.size() ||
          state.next_handle_ >= quality_handle_base ||
          state.relations_.size() >= 1024)
        return invalid("invalid prefix relation request");
      auto lease = bridge_capacity(16 * sizeof(uint64_t));
      if (!lease.ok())
        return lease.status();
      const auto output_shape =
          query.output.result_schema->tensors[output_slot].sample_shape();
      const auto input_shape =
          query.inputs[input].result_schema->tensors[input_slot].sample_shape();
      if (output_shape.size() != 1 || input_shape != output_shape)
        return invalid("prefix requires equal rank-one tensor domains");
      auto made = ResultRelation::prefix(
          state.phase_->resources, output_shape[0], input, roles,
          ResultSupportTarget::Tensor, input_slot);
      if (!made.ok())
        return made.status();
      const auto token = state.next_handle_++;
      state.relations_.emplace(token, made.take_value());
      *handle = token;
      return Status::success();
    });
  }
  static int publish_tensor_with_relation(void* raw, std::uint32_t slot,
                                          const ps_result_region_v2* box,
                                          const std::uint8_t* bytes,
                                          std::uint64_t size,
                                          std::uint64_t handle,
                                          std::uint32_t finality) {
    return call(raw, [&](CResultMemberBridge& state) {
      const auto schema = state.phase_->query.output.result_schema;
      auto found = state.relations_.find(handle);
      if (!schema || slot >= schema->tensors.size() ||
          found == state.relations_.end() || finality != PS_RESULT_FINAL_V2 ||
          size > SIZE_MAX || (!bytes && size))
        return invalid("invalid mapped image publication");
      auto coverage = region(box, schema->tensors[slot].sample_shape());
      if (!coverage.ok())
        return coverage.status();
      return state.publication(state.builder_.publish_tensor(
          slot, coverage.value().region, ByteView(bytes, size), found->second,
          {true, true, true, true}, state.phase_->query.cancellation));
    });
  }
  static int publish_tensor_view(void* raw, std::uint32_t slot,
                                 const ps_result_region_v2* box,
                                 const std::uint64_t* handles,
                                 std::uint32_t count,
                                 const ps_result_tensor_transform_v2* transform,
                                 std::uint64_t handle, std::uint32_t finality) {
    return call(
        raw,
        [&](CResultMemberBridge& state) {
          const auto schema = state.phase_->query.output.result_schema;
          auto found = state.relations_.find(handle);
          if (!schema || slot >= schema->tensors.size() ||
              found == state.relations_.end() || !array(handles, count, 16) ||
              !count || finality != PS_RESULT_FINAL_V2)
            return invalid("invalid image view publication");
          auto coverage = region(box, schema->tensors[slot].sample_shape());
          if (!coverage.ok())
            return coverage.status();
          ResourceVector<const ResultTensorReadWindow*> windows(
              ResourceAllocator<const ResultTensorReadWindow*>(
                  state.phase_->resources));
          for (std::uint32_t i = 0; i < count; ++i) {
            auto source = state.windows_.find(handles[i]);
            if (source == state.windows_.end())
              return invalid("expired image view source");
            windows.push_back(&source->second->window);
          }
          if (transform) {
            if (reinterpret_cast<std::uintptr_t>(transform) %
                    alignof(ps_result_tensor_transform_v2) ||
                transform->struct_size != sizeof(*transform) || count != 1 ||
                transform->reshape > 1 ||
                !array(transform->axes, transform->source_rank, 8) ||
                (transform->reshape &&
                 (transform->source_rank || transform->axes)))
              return invalid("invalid affine view transform");
            if (!transform->reshape &&
                transform->source_rank != windows.front()->region().rank())
              return invalid("affine view source rank mismatch");
            for (std::uint32_t i = 0; i < transform->source_rank; ++i)
              if (transform->axes[i].extent != 1 ||
                  transform->axes[i].output_axis < -1 ||
                  transform->axes[i].output_axis >=
                      static_cast<std::int32_t>(coverage.value().region.rank()))
                return invalid("invalid affine view point axis");
            ResultTensorViewTransform mapping;
            mapping.reshape = transform->reshape != 0;
            for (std::uint32_t i = 0; i < transform->source_rank; ++i) {
              const auto& axis = transform->axes[i];
              mapping.source_axes.push_back({axis.output_axis,
                                             axis.source_origin, axis.step,
                                             axis.extent, axis.output_origin});
            }
            return state.publication(state.builder_.publish_tensor_view(
                slot, coverage.value().region, *windows.front(), mapping,
                found->second, {true, true, true, true},
                state.phase_->query.cancellation));
          }
          return state.publication(state.builder_.publish_tensor_view(
              slot, coverage.value().region,
              std::vector<const ResultTensorReadWindow*>(windows.begin(),
                                                         windows.end()),
              found->second, {true, true, true, true},
              state.phase_->query.cancellation));
        },
        true);
  }
  static int report_numeric(void* raw,
                            const ps_result_numeric_report_v2* source) {
    return call(raw, [&](CResultMemberBridge& state) {
      if (!source ||
          reinterpret_cast<std::uintptr_t>(source) %
              alignof(ps_result_numeric_report_v2) ||
          source->struct_size != sizeof(*source) ||
          !state.phase_->report_numeric)
        return invalid("invalid numeric report");
      NumericDiagnostics report;
      report.profile = static_cast<CpuNumericProfile>(source->profile);
      std::copy(source->implementation, source->implementation + 256,
                report.implementation.begin());
      report.evaluated_values = source->evaluated_values;
      report.strict_fallbacks = source->strict_fallbacks;
      report.view_elements = source->view_elements;
      report.copied_elements = source->copied_elements;
      report.strict_math_calls = source->strict_math_calls;
      std::copy(source->fallback_reasons, source->fallback_reasons + 4,
                report.fallback_reasons.begin());
      for (unsigned function = 0; function < 8; ++function)
        std::copy(source->function_fallbacks[function],
                  source->function_fallbacks[function] + 4,
                  report.function_fallbacks[function].begin());
      return state.phase_->report_numeric(report);
    });
  }
  static int acquire_native_atlas(void* raw, std::uint32_t input,
                                  std::uint32_t slot,
                                  ps_result_native_atlas_v2* output) {
    auto* lease = static_cast<Lease*>(raw);
    return call(
        raw,
        [&](CResultMemberBridge& state) {
          if (!array(output, 1, 1) || output->struct_size != sizeof(*output) ||
              output->reserved)
            return invalid("invalid C atlas destination");
          const Lease::AtlasKey key{input, slot};
          auto existing = lease->atlases.find(key);
          if (existing != lease->atlases.end()) {
            auto payload =
                lease->native_views.find(existing->second.payload_token);
            auto directory =
                lease->native_views.find(existing->second.directory_token);
            if (payload != lease->native_views.end() &&
                directory != lease->native_views.end()) {
              *output = existing->second;
              return Status::success();
            }
            // Reacquisition after explicit release creates fresh handles;
            // retained tokens must never revive the released generation.
            for (auto token : {existing->second.payload_token,
                               existing->second.directory_token}) {
              auto view = lease->native_views.find(token);
              if (view != lease->native_views.end()) {
                auto status = gpu_outcome(
                    state, lease->host_gpu->release(lease->host_gpu->context,
                                                    view->second));
                if (!status.ok())
                  return status;
                lease->native_views.erase(view);
              }
            }
            lease->atlases.erase(existing);
          }
          auto packed = state.phase_->acquire_native_atlas(input, slot);
          if (!packed.ok())
            return packed.status();
          const auto& atlas = *packed.value();
          ps_result_native_atlas_v2 value{};
          value.struct_size = sizeof(value);
          value.rank = atlas.descriptor.shape.size();
          value.element_type =
              static_cast<std::uint32_t>(atlas.descriptor.element_type);
          std::copy(atlas.descriptor.shape.begin(),
                    atlas.descriptor.shape.end(), value.shape);
          std::copy(atlas.tile_shape.begin(), atlas.tile_shape.end(),
                    value.tile_shape);
          value.slot_count = atlas.slot_count;
          value.payload_sample_bytes = atlas.payload_bytes;
          value.payload_byte_size = atlas.payload.bytes().size();
          value.directory_byte_size = atlas.directory.bytes().size();
          auto result =
              gpu_buffer(raw, atlas.payload.bytes().data(),
                         value.payload_byte_size, 0, &value.payload_token);
          if (!result)
            result = gpu_buffer(raw, atlas.directory.bytes().data(),
                                value.directory_byte_size, 0,
                                &value.directory_token);
          if (result)
            return state.callback_failure();
          lease->atlases.emplace(key, value);
          *output = value;
          return Status::success();
        },
        false, true, true);
  }
  Status retain_block_state(ResultRef result, std::uint64_t* handle) {
    if (!array(handle, 1, 1) || next_handle_ >= quality_handle_base ||
        block_states_.size() >= 1024)
      return invalid("invalid Result block state handle destination");
    const auto token = next_handle_++;
    block_states_.emplace(token, std::move(result));
    *handle = token;
    return Status::success();
  }
  static int create_block_state(void* raw, const ps_result_schema_v2* source,
                                const std::uint8_t* bytes, std::uint64_t size,
                                std::uint64_t* handle) {
    return call(
        raw,
        [&](CResultMemberBridge& state) {
          if (!array(source, 1, 1) || source->struct_size != sizeof(*source) ||
              source->publication != PS_RESULT_COMPLETE_BUNDLE_V2 ||
              source->field_count || source->metadata_count ||
              source->domain_rank || source->tensor_count != 1 ||
              !array(source->tensors, 1, 1) || source->tensors[0].facet_count ||
              source->tensors[0].group_count || source->tensors[0].batch_rank ||
              source->tensors[0].spatial || !bytes || !size ||
              size > SIZE_MAX || !array(handle, 1, 1))
            return invalid("invalid generic Result block state");
          auto admission = bridge_capacity(
              sizeof(SchemaTemplate) + sizeof(ResultTensorSpec) +
              2 * 8 * sizeof(std::uint64_t) + 2 * 129);
          if (!admission.ok())
            return admission.status();
          auto copied = schema(source);
          if (!copied.ok())
            return copied.status();
          auto metadata = copied.take_value();
          const auto& descriptor = metadata.tensors[0].descriptor;
          auto count = metadata.tensors[0].sample_count();
          const auto width = Value::element_size(descriptor.element_type);
          if (!count.ok())
            return count.status();
          if (!width || count.value() > UINT64_MAX / width ||
              size != count.value() * width)
            return invalid("Result block state payload size mismatch");
          auto charged = state.phase_->consume_work(size);
          if (!charged.ok())
            return charged;
          auto builder = ResultBuilder::start(state.phase_->resources, metadata,
                                              "c.block.state");
          if (!builder.ok())
            return builder.status();
          auto writer = builder.take_value();
          auto backing = MutableValue::allocate(descriptor,
                                                Region::whole(descriptor.shape),
                                                state.phase_->allocator);
          if (!backing.ok())
            return backing.status();
          auto buffer = backing.take_value();
          std::memcpy(buffer.data(), bytes, size);
          auto published = std::move(buffer).publish();
          if (!published.ok())
            return published.status();
          auto facts =
              ResultRelation::cartesian(state.phase_->resources, 1, {});
          auto relation = ResultRelation::cartesian(state.phase_->resources,
                                                    count.value(), {});
          if (!facts.ok() || !relation.ok())
            return !facts.ok() ? facts.status() : relation.status();
          auto status = writer.bind_descriptor_relation(facts.take_value());
          if (status.ok())
            status = writer.publish_tensor(
                0, Region::whole(descriptor.shape), published.value().layout(),
                published.value().storage(), relation.take_value(),
                {true, true, true, true}, state.phase_->query.cancellation);
          if (!status.ok())
            return status;
          auto result = writer.seal();
          return result.ok()
                     ? state.retain_block_state(result.take_value(), handle)
                     : result.status();
        },
        false, true);
  }
  static int read_block_state(void* raw, std::uint64_t handle,
                              std::uint8_t* bytes, std::uint64_t size) {
    return call(
        raw,
        [&](CResultMemberBridge& state) {
          auto found = state.block_states_.find(handle);
          if (found == state.block_states_.end() || !bytes || size > SIZE_MAX)
            return invalid("invalid Result block state read");
          auto facts = found->second.descriptor();
          if (!facts.ok())
            return facts.status();
          const auto& spec = found->second.schema().tensors[0];
          auto count = spec.sample_count();
          const auto width = Value::element_size(spec.descriptor.element_type);
          if (!count.ok() || count.value() > UINT64_MAX / width ||
              size != count.value() * width)
            return invalid("Result block state read size mismatch");
          auto window = found->second.acquire_tensor(
              facts.value(), 0, Region::whole(spec.sample_shape()),
              state.phase_->query.cancellation);
          if (!window.ok())
            return window.status();
          auto work =
              execution_internal::ResultWindowAccess::read_work(window.value());
          if (!work.ok())
            return work.status();
          if (work.value() > UINT64_MAX - width ||
              count.value() > UINT64_MAX / (work.value() + width))
            return Status{ErrorCode::ResourceExhausted, {}};
          auto charged = state.phase_->consume_work(count.value() *
                                                    (work.value() + width));
          if (!charged.ok())
            return charged;
          std::uint64_t offset = 0;
          return facts.value().tensor_coverage(0).visit(
              [&](const auto& coordinate) {
                auto row = window.value().row_run(coordinate);
                if (!row.ok())
                  return row.status();
                std::memcpy(bytes + offset, row.value().data, width);
                offset += width;
                return Status::success();
              },
              count.value(), state.phase_->query.cancellation);
        },
        false, true);
  }
  static Result<std::uint64_t> checkpoint_bytes(const ResultRef& result) {
    auto facts = result.descriptor();
    if (!facts.ok())
      return Result<std::uint64_t>(facts.status());
    const auto& schema = result.schema();
    if (schema.publication != PublishPolicy::CompleteBundle ||
        !schema.fields.empty() || !schema.metadata.empty() ||
        !schema.domain.empty() || schema.tensors.size() != 1)
      return Result<std::uint64_t>(
          invalid("invalid C checkpoint representation"));
    const auto& tensor = schema.tensors[0];
    if (!tensor.facets.empty() || !tensor.layout.groups.empty() ||
        !tensor.batch_axes.empty() || tensor.layout.spatial)
      return Result<std::uint64_t>(
          invalid("C checkpoint requires generic tensor state"));
    const auto& coverage = facts.value().tensor_coverage(0);
    if (coverage.shape() != tensor.descriptor.shape ||
        coverage.boxes().size() != 1 ||
        coverage.boxes()[0].rank() != tensor.descriptor.shape.size())
      return Result<std::uint64_t>(
          invalid("C checkpoint requires complete tensor coverage"));
    for (std::size_t axis = 0; axis < tensor.descriptor.shape.size(); ++axis) {
      const auto dimension = coverage.boxes()[0].dimensions()[axis];
      if (dimension.offset || dimension.extent != tensor.descriptor.shape[axis])
        return Result<std::uint64_t>(
            invalid("C checkpoint requires complete tensor coverage"));
    }
    auto count = tensor.sample_count();
    auto width = Value::element_size(tensor.descriptor.element_type);
    if (!count.ok())
      return Result<std::uint64_t>(count.status());
    if (!width || count.value() > UINT64_MAX / width)
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
    return Result<std::uint64_t>(count.value() * width);
  }
  static int checkpoint_before(void* raw, std::uint32_t phase,
                               std::uint64_t before,
                               ps_result_checkpoint_v2* destination) {
    auto* lease = static_cast<Lease*>(raw);
    return call(raw, [&](CResultMemberBridge& state) {
      if (!array(destination, 1, 1) ||
          destination->struct_size != sizeof(*destination) ||
          destination->reserved || !state.phase_->checkpoint_before)
        return invalid("invalid C Result checkpoint destination");
      auto found = state.phase_->checkpoint_before(phase, before);
      if (!found.ok())
        return found.status();
      ps_result_checkpoint_v2 result{};
      result.struct_size = sizeof(result);
      if (found.value()) {
        if (found.value()->phase() != phase ||
            found.value()->sequence() > before ||
            !found.value()->state().owned_by(state.phase_->resources))
          return invalid("invalid C Result checkpoint scope");
        if (state.next_handle_ >= quality_handle_base ||
            lease->checkpoints.size() >= 65536)
          return Status{ErrorCode::ResourceExhausted, {}};
        auto bytes = checkpoint_bytes(found.value()->state());
        if (!bytes.ok())
          return bytes.status();
        result.handle = state.next_handle_++;
        result.sequence = found.value()->sequence();
        result.byte_size = bytes.value();
        lease->checkpoints.emplace(result.handle, std::move(*found.value()));
      }
      *destination = result;
      return Status::success();
    });
  }
  static int checkpoint_read(void* raw, std::uint64_t handle,
                             std::uint64_t offset, void* destination,
                             std::uint64_t size) {
    auto* lease = static_cast<Lease*>(raw);
    return call(raw, [&](CResultMemberBridge& state) {
      auto found = lease->checkpoints.find(handle);
      if (found == lease->checkpoints.end() || !destination || !size ||
          size > SIZE_MAX)
        return invalid("invalid C Result checkpoint read");
      const auto& result = found->second.state();
      auto bytes = checkpoint_bytes(result);
      if (!bytes.ok())
        return bytes.status();
      if (offset > bytes.value() || size > bytes.value() - offset)
        return invalid("C checkpoint byte interval exceeds state");
      auto facts = result.descriptor();
      if (!facts.ok())
        return facts.status();
      const auto& tensor = result.schema().tensors[0];
      const auto& shape = tensor.descriptor.shape;
      auto capacity = bridge_capacity(
          shape.size() * (sizeof(std::uint64_t) + sizeof(RegionDimension)));
      if (!capacity.ok())
        return capacity.status();
      auto window =
          result.acquire_tensor(facts.value(), 0, Region::whole(shape),
                                state.phase_->query.cancellation);
      if (!window.ok())
        return window.status();
      const auto width = Value::element_size(tensor.descriptor.element_type);
      const auto first = offset / width;
      const auto last = (offset + size - 1) / width;
      auto lookup =
          execution_internal::ResultWindowAccess::read_work(window.value());
      if (!lookup.ok())
        return lookup.status();
      auto per_sample = lookup.value();
      if (per_sample > UINT64_MAX - width - shape.size())
        return Status{ErrorCode::ResourceExhausted, {}};
      per_sample += width + shape.size();
      if (last - first + 1 > UINT64_MAX / per_sample)
        return Status{ErrorCode::ResourceExhausted, {}};
      auto charged =
          state.phase_->consume_work((last - first + 1) * per_sample);
      if (!charged.ok())
        return charged;
      std::vector<std::uint64_t> coordinate(shape.size());
      auto* output = static_cast<std::uint8_t*>(destination);
      std::uint64_t copied = 0;
      for (auto sample = first;; ++sample) {
        auto remaining = sample;
        for (auto axis = shape.size(); axis-- > 0;) {
          coordinate[axis] = remaining % shape[axis];
          remaining /= shape[axis];
        }
        auto row = window.value().row_run(coordinate);
        if (!row.ok())
          return row.status();
        const auto skip = sample == first ? offset % width : 0;
        const auto count = std::min<std::uint64_t>(width - skip, size - copied);
        std::memcpy(output + copied, row.value().data + skip, count);
        copied += count;
        if (sample == last)
          break;
      }
      return Status::success();
    });
  }
  static int checkpoint_publish(void* raw, std::uint32_t phase,
                                std::uint64_t sequence, std::uint64_t handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      auto found = state.block_states_.find(handle);
      if (found == state.block_states_.end() ||
          !state.phase_->checkpoint_publish)
        return invalid("invalid C Result checkpoint state handle");
      return state.phase_->checkpoint_publish(phase, sequence, found->second);
    });
  }
  static int release_block_state(void* raw, std::uint64_t handle) {
    return call(
        raw,
        [&](CResultMemberBridge& state) {
          return state.block_states_.erase(handle)
                     ? Status::success()
                     : invalid("expired Result block state handle");
        },
        false, true);
  }
  static int block(void* raw, std::uint32_t kind, std::uint64_t begin,
                   std::uint64_t end, std::uint64_t mode,
                   std::uint64_t incoming, ps_result_block_compute_v2 compute,
                   void* user, std::uint64_t* outgoing) {
    auto* lease = static_cast<Lease*>(raw);
    return call(raw, [&](CResultMemberBridge& state) {
      auto found = state.block_states_.find(incoming);
      if (!array(outgoing, 1, 1) || !compute || !state.phase_->block ||
          found == state.block_states_.end())
        return invalid("invalid Result pure block request");
      const auto initial = found->second;
      auto result = state.phase_->block(
          kind, begin, end, mode, initial, [&]() -> Result<ResultRef> {
            struct Exit {
              bool& active;
              ~Exit() { active = false; }
            } exit{state.in_block_};
            state.in_block_ = true;
            ps_result_block_services_v2 services{
                sizeof(services),
                raw,
                read_tensor,
                create_block_state,
                read_block_state,
                release_block_state,
                acquire_native_atlas,
                allocate,
                release_scratch,
                work,
                cancelled,
                lease->host_gpu ? &lease->gpu : nullptr};
            std::uint64_t handle = 0;
            auto status = outcome(compute(&services, incoming, &handle, user));
            auto failure = state.callback_failure();
            if (!failure.ok())
              return Result<ResultRef>(failure);
            if (!status.ok())
              return Result<ResultRef>(status);
            auto output = state.block_states_.find(handle);
            if (output == state.block_states_.end())
              return Result<ResultRef>(
                  invalid("missing Result block publication"));
            auto published = output->second;
            if (handle != incoming)
              state.block_states_.erase(output);
            return Result<ResultRef>(std::move(published));
          });
      return result.ok()
                 ? state.retain_block_state(result.take_value(), outgoing)
                 : result.status();
    });
  }
  static int discover(void* raw, std::uint32_t capacity,
                      std::uint32_t candidates,
                      ps_result_discovery_compute_v2 compute, void* user,
                      std::uint64_t* handle) {
    auto* lease = static_cast<Lease*>(raw);
    return call(raw, [&](CResultMemberBridge& state) {
      if (!compute || !array(handle, 1, 1) || !state.phase_->discover ||
          state.next_handle_ >= quality_handle_base ||
          state.discoveries_.size() >= 1024)
        return invalid("invalid Result GPU discovery request");
      auto result = state.phase_->discover(
          capacity, candidates, [&](const ResultGpuRequestTable& table) {
            struct Scope {
              bool& active;
              ~Scope() { active = false; }
            } scope{state.in_discovery_};
            state.in_discovery_ = true;
            const ps_result_discovery_services_v2 services{
                sizeof(services),
                raw,
                read_tensor,
                acquire_native_atlas,
                allocate,
                release_scratch,
                work,
                cancelled,
                lease->host_gpu ? &lease->gpu : nullptr};
            auto status = outcome(compute(
                &services, table.bytes, table.byte_size, table.capacity, user));
            auto failure = state.callback_failure();
            return failure.ok() ? status : failure;
          });
      if (!result.ok())
        return result.status();
      state.discovery_pending_ |= !result.value()->tensors.empty();
      const auto token = state.next_handle_++;
      state.discoveries_.emplace(token, result.take_value());
      *handle = token;
      return Status::success();
    });
  }
  static int discovery_requests(void* raw, std::uint64_t handle,
                                ps_result_discovery_request_v2* output,
                                std::uint32_t capacity, std::uint32_t* count) {
    return call(raw, [&](CResultMemberBridge& state) {
      auto found = state.discoveries_.find(handle);
      if (found == state.discoveries_.end() ||
          !array(output, capacity, 65536) || !array(count, 1, 1))
        return invalid("invalid Result discovery receipt");
      const auto& needs = found->second->tensors;
      auto charged = state.phase_->consume_work(needs.size());
      if (!charged.ok())
        return charged;
      std::uint64_t size = 0, units = 0;
      for (const auto& need : needs) {
        size += need.samples.boxes().size();
        units +=
            need.samples.boxes().size() * (1 + 2 * need.samples.shape().size());
      }
      if (size > 65536 || (capacity && capacity < size))
        return Status{ErrorCode::ResourceExhausted,
                      "Result discovery receipt capacity"};
      if (capacity) {
        charged = state.phase_->consume_work(units);
        if (!charged.ok())
          return charged;
        for (std::uint64_t i = 0; i < size; ++i)
          if (output[i].struct_size != sizeof(output[i]))
            return invalid("invalid Result discovery record destination");
        std::uint64_t index = 0;
        for (const auto& need : needs) {
          for (const auto& box : need.samples.boxes()) {
            auto& record = output[index++];
            record = {};
            record.struct_size = sizeof(record);
            record.input = need.input;
            record.slot = need.slot;
            record.roles = need.roles;
            record.region.struct_size = sizeof(record.region);
            record.region.rank = box.rank();
            for (std::uint32_t axis = 0; axis < box.rank(); ++axis) {
              record.region.offset[axis] = box.dimensions()[axis].offset;
              record.region.extent[axis] = box.dimensions()[axis].extent;
            }
          }
        }
      }
      *count = static_cast<std::uint32_t>(size);
      return Status::success();
    });
  }
  static int release_discovery(void* raw, std::uint64_t handle) {
    return call(raw, [&](CResultMemberBridge& state) {
      return state.discoveries_.erase(handle)
                 ? Status::success()
                 : invalid("expired Result discovery receipt");
    });
  }
  static ps_result_services_v2 services_for(Lease* lease,
                                            const ResultProgramPhase& phase) {
    lease->native_views = Lease::NativeViews(
        std::less<std::uint64_t>{},
        ResourceAllocator<Lease::NativeViews::value_type>(phase.resources));
    lease->atlases = Lease::Atlases(
        std::less<Lease::AtlasKey>{},
        ResourceAllocator<Lease::Atlases::value_type>(phase.resources));
    lease->checkpoints = Lease::Checkpoints(
        std::less<std::uint64_t>{},
        ResourceAllocator<Lease::Checkpoints::value_type>(phase.resources));
    auto table = service(lease);
    lease->host_parallel = phase.cpu_parallel;
    lease->host_tiles = phase.cpu_tiles;
    lease->host_gpu = phase.gpu;
    if (phase.cpu_parallel) {
      lease->parallel = *phase.cpu_parallel;
      lease->parallel.context = lease;
      lease->parallel.run = parallel;
      table.cpu_parallel = &lease->parallel;
    }
    if (phase.cpu_tiles) {
      lease->tiles = *phase.cpu_tiles;
      lease->tiles.context = lease;
      lease->tiles.run = tiles;
      table.cpu_tiles = &lease->tiles;
    }
    if (phase.gpu) {
      lease->gpu = *phase.gpu;
      lease->gpu.context = lease;
      lease->gpu.buffer = gpu_buffer;
      lease->gpu.execute = gpu_execute;
      lease->gpu.release = gpu_release;
      table.gpu = &lease->gpu;
    }
    return table;
  }
  static int parallel(void* raw, uint64_t count, uint64_t grain,
                      uint32_t workers, ps_cpu_range_callback_v1 callback,
                      void* user) {
    auto* lease = static_cast<Lease*>(raw);
    return call(raw, [&](CResultMemberBridge&) {
      return outcome(lease->host_parallel->run(lease->host_parallel->context,
                                               count, grain, workers, callback,
                                               user));
    });
  }
  static int tiles(void* raw, const ps_cpu_tile_stage_v1* stage,
                   ps_cpu_tile_callback_v1 callback, void* user) {
    auto* lease = static_cast<Lease*>(raw);
    return call(raw, [&](CResultMemberBridge&) {
      return outcome(lease->host_tiles->run(lease->host_tiles->context, stage,
                                            callback, user));
    });
  }
  static Status gpu_outcome(CResultMemberBridge& state, int result) {
    if (state.phase_->gpu_status) {
      auto status = state.phase_->gpu_status();
      if (!status.ok())
        return status;
    }
    return outcome(result);
  }
  static int gpu_code(int result) {
    return result == 0 || result == 2 || result == 3 ? result : 1;
  }
  static int gpu_buffer(void* raw, const uint8_t* bytes, uint64_t count,
                        uint32_t writable, uint64_t* token) {
    auto* lease = static_cast<Lease*>(raw);
    return gpu_code(call(
        raw,
        [&](CResultMemberBridge& state) {
          if (!array(token, 1, 1) || writable > 1 || !lease->host_gpu)
            return invalid("invalid C native view destination");
          if (lease->native_views.size() >= 1024 ||
              state.next_handle_ >= quality_handle_base)
            return Status{ErrorCode::ResourceExhausted, {}};
          std::uint64_t native = 0;
          auto status = gpu_outcome(
              state, lease->host_gpu->buffer(lease->host_gpu->context, bytes,
                                             count, writable, &native));
          if (!status.ok())
            return status;
          const auto handle = state.next_handle_++;
          lease->native_views.emplace(handle, native);
          *token = handle;
          return Status::success();
        },
        false, true, true));
  }
  static int gpu_execute(void* raw, const ps_gpu_dispatch_v1* commands,
                         uint32_t count) {
    auto* lease = static_cast<Lease*>(raw);
    return gpu_code(call(
        raw,
        [&](CResultMemberBridge& state) {
          if (!lease->host_gpu || !count || !array(commands, count, 32))
            return invalid("invalid C native dispatch list");
          auto charged = state.phase_->consume_work(count);
          if (!charged.ok())
            return charged;
          std::uint64_t total = 0;
          for (std::uint32_t i = 0; i < count; ++i) {
            if (commands[i].struct_size != sizeof(commands[i]) ||
                !array(commands[i].buffers, commands[i].buffer_count, 31))
              return invalid("invalid C native binding list");
            total += commands[i].buffer_count;
          }
          charged = state.phase_->consume_work(total);
          if (!charged.ok())
            return charged;
          ResourceVector<ps_gpu_dispatch_v1> translated(
              ResourceAllocator<ps_gpu_dispatch_v1>(state.phase_->resources));
          ResourceVector<ps_gpu_buffer_binding_v1> bindings(
              ResourceAllocator<ps_gpu_buffer_binding_v1>(
                  state.phase_->resources));
          translated.assign(commands, commands + count);
          bindings.reserve(total);
          for (std::uint32_t i = 0; i < count; ++i) {
            const auto first = bindings.size();
            for (std::uint32_t j = 0; j < commands[i].buffer_count; ++j) {
              const auto& binding = commands[i].buffers[j];
              if (binding.struct_size != sizeof(binding))
                return invalid("invalid C native binding");
              auto token = lease->native_views.find(binding.token);
              if (token == lease->native_views.end())
                return invalid("native token is not from current C poll");
              bindings.push_back(binding);
              bindings.back().token = token->second;
            }
            translated[i].buffers =
                commands[i].buffer_count ? bindings.data() + first : nullptr;
          }
          return gpu_outcome(
              state, lease->host_gpu->execute(lease->host_gpu->context,
                                              translated.data(), count));
        },
        false, true, true));
  }
  static int gpu_release(void* raw, uint64_t token) {
    auto* lease = static_cast<Lease*>(raw);
    return gpu_code(call(
        raw,
        [&](CResultMemberBridge& state) {
          auto found = lease->native_views.find(token);
          if (!lease->host_gpu || found == lease->native_views.end())
            return invalid("native token is not from current C poll");
          auto status =
              gpu_outcome(state, lease->host_gpu->release(
                                     lease->host_gpu->context, found->second));
          lease->native_views.erase(found);
          return status;
        },
        false, true, true));
  }
  static bool ready(CResultMemberBridge& state) {
    if (!state.active_ || std::this_thread::get_id() != state.owner_ ||
        execution_internal::in_cpu_range) {
      state.violation_ = true;
      return false;
    }
    return true;
  }
  template <class Function>
  static int call(void* context, Function function, bool view_probe = false,
                  bool block_service = false,
                  bool discovery_service = false) noexcept {
    if (!context)
      return 6;
    auto& lease = *static_cast<Lease*>(context);
    auto& state = *lease.owner;
    if (!lease.active.load() || !ready(state)) {
      state.violation_ = true;
      return 6;
    }
    Status status;
    if (state.joint_failure_) {
      status = state.joint_failure_->snapshot();
      if (!status.ok())
        return code(status);
    }
    if (state.window_failure_)
      status = state.window_failure_->snapshot();
    if (state.violation_.load())
      status = {ErrorCode::InvalidArgument,
                {},
                FailureReason::UnauthorizedRead,
                {FailureOrigin::Protocol, FailureScope::Group}};
    else if (!state.failure_.ok())
      return code(state.failure_);
    try {
      if (status.ok() && state.in_block_ && !block_service)
        status = invalid("service unavailable inside pure Result block");
      if (status.ok() && state.in_discovery_ && !discovery_service)
        status = invalid("service unavailable inside Result GPU discovery");
      if (status.ok())
        status = function(state);
    } catch (const std::bad_alloc&) {
      status = {ErrorCode::ResourceExhausted, {}};
    } catch (...) {
      status = {ErrorCode::OperationFailed, {}};
    }
    if (state.violation_.load())
      status = {ErrorCode::InvalidArgument,
                {},
                FailureReason::UnauthorizedRead,
                {FailureOrigin::Protocol, FailureScope::Group}};
    if (view_probe && status.code == ErrorCode::InvalidArgument &&
        status.message.find("ViewUnavailable") != std::string::npos)
      return PS_RESULT_VIEW_UNAVAILABLE_V2;
    if (!status.ok()) {
      if (state.failure_.ok())
        state.failure_ = std::move(status);
      if (state.joint_failure_ && state.definition_->joint.contract == 2 &&
          ((state.failure_.code != ErrorCode::Cancelled &&
            state.failure_.code != ErrorCode::Stale) ||
           state.failure_.detail.scope == FailureScope::Run ||
           state.failure_.detail.scope == FailureScope::Group)) {
        if (state.failure_.code == ErrorCode::InvalidArgument &&
            state.failure_.detail.origin == FailureOrigin::Unspecified)
          state.failure_.detail = {FailureOrigin::Protocol,
                                   FailureScope::Group};
        state.failure_ = state.joint_failure_->record(state.failure_);
      }
      try {
        if (state.phase_->failure_observer)
          state.phase_->failure_observer(state.failure_);
      } catch (...) {
      }
      if (state.phase_->failure) {
        auto expected = ErrorCode::Ok;
        state.phase_->failure->compare_exchange_strong(expected,
                                                       state.failure_.code);
      }
    }
    return code(state.failure_.ok() ? status : state.failure_);
  }
  Result<Footprint> samples(std::uint32_t input, std::uint32_t slot,
                            const ps_result_region_v2* regions,
                            std::uint32_t count) {
    if (input >= phase_->query.inputs.size() || !array(regions, count, 65536))
      return Result<Footprint>(invalid("invalid tensor Need"));
    const auto& metadata = phase_->query.inputs[input];
    if (!metadata.result_schema ||
        slot >= metadata.result_schema->tensors.size())
      return Result<Footprint>(invalid("invalid image Need slot"));
    auto shape_admission = bridge_capacity(8 * sizeof(uint64_t));
    if (!shape_admission.ok())
      return Result<Footprint>(shape_admission.status());
    const auto shape = metadata.result_schema->tensors[slot].sample_shape();
    auto charged = phase_->consume_work(count * (1 + 2 * shape.size()));
    if (!charged.ok())
      return Result<Footprint>(charged);
    auto admitted = bridge_capacity(
        count * (sizeof(Region) + shape.size() * sizeof(RegionDimension)));
    if (!admitted.ok())
      return Result<Footprint>(admitted.status());
    std::vector<Region> boxes;
    boxes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto box = region(regions + i, shape);
      if (!box.ok())
        return Result<Footprint>(box.status());
      boxes.push_back(std::move(box.value().region));
    }
    FootprintLimits limits;
    limits.consume_work = phase_->consume_work;
    limits.cancellation = phase_->query.cancellation;
    return Footprint::from_regions(shape, boxes, limits);
  }
  Result<ResultRelation> relation(std::uint64_t outputs,
                                  const ps_result_relation_row_v2* rows,
                                  std::uint32_t count,
                                  std::uint32_t guarantee) {
    if (!array(rows, count, 65536) || guarantee < 1 || guarantee > 3)
      return Result<ResultRelation>(invalid("invalid Result relation rows"));
    if (guarantee == PS_RESULT_UNKNOWN_V2)
      return count ? Result<ResultRelation>(invalid("Unknown has rows"))
                   : ResultRelation::unknown(phase_->resources, outputs);
    if (!count)
      return ResultRelation::cartesian(
          phase_->resources, outputs, {0, 1, 0, 0},
          static_cast<DependencyGuarantee>(guarantee));
    return ResultRelation::sample_rows(
        phase_->resources, outputs, count,
        [&](std::uint64_t i) {
          const auto& row = rows[i];
          if (row.target != PS_RESULT_TARGET_FIELD_V2 &&
              row.target != PS_RESULT_TARGET_TENSOR_V2 &&
              row.target != PS_RESULT_TARGET_DESCRIPTOR_V2)
            return Result<ResultRelationRow>(
                invalid("invalid Result relation target"));
          return Result<ResultRelationRow>(
              {row.output,
               {row.input, row.roles, row.first, row.count,
                static_cast<ResultSupportTarget>(row.target), row.slot}});
        },
        static_cast<DependencyGuarantee>(guarantee));
  }
  static int need_tensor(void* c, std::uint32_t input, std::uint32_t slot,
                         std::uint32_t roles, const ps_result_region_v2* boxes,
                         std::uint32_t count) {
    return call(c, [&](CResultMemberBridge& s) {
      if (!roles || (roles & ~15U))
        return invalid("invalid image Need roles");
      auto samples = s.samples(input, slot, boxes, count);
      if (!samples.ok())
        return samples.status();
      s.need_.tensors.push_back({input, slot, samples.take_value(), roles});
      return Status::success();
    });
  }
  static int need_result(void* c, std::uint32_t input, std::uint32_t field,
                         std::uint32_t complete, std::uint64_t rows) {
    return call(c, [&](CResultMemberBridge& s) {
      if (complete > 1)
        return invalid("invalid Result completion enum");
      s.need_.results.push_back({input, field, complete != 0, rows});
      return Status::success();
    });
  }
  static int read_tensor(void* c, std::uint32_t input, std::uint32_t slot,
                         const std::uint64_t* at, std::uint32_t rank,
                         void* bytes, std::uint64_t size) {
    return call(
        c,
        [&](CResultMemberBridge& s) {
          if (!array(at, rank, 8) || !bytes || size > SIZE_MAX)
            return invalid("invalid image read pointers");
          return with_coordinate(at, rank, [&](const auto& coordinate) {
            return s.phase_->read_tensor(input, slot, coordinate, bytes, size);
          });
        },
        false, true, true);
  }
  static int retain_tensor(void* c, std::uint32_t input, std::uint32_t slot,
                           std::uint64_t* handle) {
    return call(c, [&](CResultMemberBridge& s) {
      if (!handle || !s.phase_->tensors ||
          s.next_handle_ >= quality_handle_base || s.retained_.size() >= 1024)
        return invalid("invalid retained image request");
      auto found = s.phase_->tensors->find({input, slot});
      if (found == s.phase_->tensors->end())
        return invalid("image input is not ready");
      auto token = s.next_handle_++;
      s.retained_.emplace(token, found->second);
      *handle = token;
      return Status::success();
    });
  }
  static int read_retained(void* c, std::uint64_t handle,
                           const std::uint64_t* at, std::uint32_t rank,
                           void* bytes, std::uint64_t size) {
    return call(c, [&](CResultMemberBridge& s) {
      auto found = s.retained_.find(handle);
      if (found == s.retained_.end() || !array(at, rank, 8) || !bytes ||
          size > SIZE_MAX)
        return invalid("invalid retained image handle/read");
      return with_coordinate(at, rank, [&](const auto& coordinate) {
        return found->second.read(coordinate, bytes, size,
                                  s.phase_->query.cancellation);
      });
    });
  }
  static int release_tensor(void* c, std::uint64_t handle) {
    return call(c, [&](CResultMemberBridge& s) {
      return s.retained_.erase(handle) ? Status::success()
                                       : invalid("expired image handle");
    });
  }
  static int allocate(void* c, std::uint64_t bytes,
                      std::uint8_t** destination) {
    return call(
        c,
        [&](CResultMemberBridge& s) {
          if (!destination || !bytes)
            return invalid("invalid scratch request");
          auto made = s.phase_->allocator.allocate(bytes);
          if (!made.ok())
            return made.status();
          auto buffer = made.take_value();
          *destination = buffer.data();
          s.scratch_.push_back(std::move(buffer));
          return Status::success();
        },
        false, true, true);
  }
  static int release_scratch(void* c, std::uint8_t* pointer) {
    return call(
        c,
        [&](CResultMemberBridge& s) {
          auto found =
              std::find_if(s.scratch_.begin(), s.scratch_.end(),
                           [&](auto& b) { return b.data() == pointer; });
          if (found == s.scratch_.end())
            return invalid("expired scratch pointer");
          s.scratch_.erase(found);
          return Status::success();
        },
        false, true, true);
  }
  static int work(void* c, std::uint64_t units) {
    return call(
        c,
        [&](CResultMemberBridge& s) { return s.phase_->consume_work(units); },
        false, true, true);
  }
  static int cancelled(void* c) {
    auto& lease = *static_cast<Lease*>(c);
    if (!lease.active.load()) {
      lease.owner->violation_ = true;
      return 1;
    }
    return lease.cancellation.cancelled();
  }

  static int begin(void* c) {
    return call(c, [&](CResultMemberBridge& s) {
      if (s.builder_.reference().valid() ||
          !s.phase_->query.output.result_schema)
        return invalid("invalid Result begin");
      auto admitted =
          bridge_capacity(s.phase_->association
                              ? s.phase_->association->size() * sizeof(uint64_t)
                              : 0);
      if (!admitted.ok())
        return admitted.status();
      std::vector<std::uint64_t> association;
      if (s.phase_->association)
        association.assign(s.phase_->association->begin(),
                           s.phase_->association->end());
      auto made = ResultBuilder::start(
          s.phase_->resources, *s.phase_->query.output.result_schema,
          s.phase_->query.semantic_key, {}, std::move(association),
          s.phase_->query.tile_height, s.phase_->query.tile_width,
          s.phase_->query.resources);
      if (!made.ok())
        return made.status();
      s.builder_ = made.take_value();
      return Status::success();
    });
  }
  static int descriptor(void* c, const ps_result_relation_row_v2* rows,
                        std::uint32_t count, std::uint32_t guarantee) {
    return call(c, [&](CResultMemberBridge& s) {
      auto relation = s.relation(1, rows, count, guarantee);
      if (!relation.ok())
        return relation.status();
      return s.builder_.bind_descriptor_relation(relation.take_value());
    });
  }
  static int publish_tensor(void* c, std::uint32_t slot,
                            const ps_result_region_v2* box,
                            const std::uint8_t* bytes, std::uint64_t size,
                            const ps_result_relation_row_v2* rows,
                            std::uint32_t count, std::uint32_t guarantee,
                            std::uint32_t finality) {
    return call(c, [&](CResultMemberBridge& s) {
      const auto& schema = s.phase_->query.output.result_schema;
      if (!schema || slot >= schema->tensors.size() || size > SIZE_MAX ||
          (!bytes && size) || finality != 15)
        return invalid("invalid image publication envelope");
      auto shape_admission = bridge_capacity(8 * sizeof(uint64_t));
      if (!shape_admission.ok())
        return shape_admission.status();
      auto coverage = region(box, schema->tensors[slot].sample_shape());
      if (!coverage.ok())
        return coverage.status();
      auto relation = s.relation(schema->tensors[slot].sample_count().value(),
                                 rows, count, guarantee);
      if (!relation.ok())
        return relation.status();
      return s.publication(s.builder_.publish_tensor(
          slot, coverage.value().region, ByteView(bytes, size),
          relation.take_value(), {true, true, true, true},
          s.phase_->query.cancellation));
    });
  }
  static int publish_tensor_buffer(void* raw, std::uint32_t slot,
                                   const ps_result_region_v2* box,
                                   std::uint8_t* bytes, std::uint64_t size,
                                   const ps_result_relation_row_v2* rows,
                                   std::uint32_t count, std::uint32_t guarantee,
                                   std::uint32_t finality) {
    return call(raw, [&](CResultMemberBridge& state) {
      const auto& schema = state.phase_->query.output.result_schema;
      auto found =
          std::find_if(state.scratch_.begin(), state.scratch_.end(),
                       [&](auto& buffer) { return buffer.data() == bytes; });
      if (!schema || slot >= schema->tensors.size() ||
          finality != PS_RESULT_FINAL_V2 || found == state.scratch_.end() ||
          found->size() != size)
        return invalid("invalid Result buffer publication");
      auto admission = bridge_capacity(
          8 * (2 * sizeof(std::uint64_t) + sizeof(std::int64_t)));
      if (!admission.ok())
        return admission.status();
      const auto& spec = schema->tensors[slot];
      auto region_value = region(box, spec.sample_shape());
      if (!region_value.ok())
        return region_value.status();
      auto samples = region_value.value().region.element_count();
      const auto width = Value::element_size(spec.descriptor.element_type);
      if (!samples.ok() || !width || samples.value() > UINT64_MAX / width ||
          samples.value() * width != size)
        return invalid("Result buffer publication size mismatch");
      auto domain = spec.sample_count();
      if (!domain.ok())
        return domain.status();
      auto relation = state.relation(domain.value(), rows, count, guarantee);
      if (!relation.ok())
        return relation.status();
      StridedLayout layout{
          0, std::vector<std::int64_t>(box->rank),
          std::vector<std::uint64_t>(box->offset, box->offset + box->rank)};
      std::uint64_t stride = width;
      for (std::uint32_t i = box->rank; i-- > 0;) {
        if (stride > INT64_MAX)
          return Status{ErrorCode::ResourceExhausted, {}};
        layout.byte_strides[i] = static_cast<std::int64_t>(stride);
        stride *= box->extent[i];
      }
      auto buffer = std::move(*found).freeze();
      state.scratch_.erase(found);
      return state.publication(state.builder_.publish_tensor(
          slot, region_value.value().region, layout, std::move(buffer),
          relation.take_value(), {true, true, true, true},
          state.phase_->query.cancellation));
    });
  }
  static int append_field(void* c, std::uint32_t field, std::uint64_t rows,
                          const std::uint8_t* bytes, std::uint64_t size) {
    return call(c, [&](CResultMemberBridge& s) {
      if (!bytes || !size || size > SIZE_MAX)
        return invalid("invalid field payload");
      auto made = s.phase_->allocator.allocate(size);
      if (!made.ok())
        return made.status();
      auto copy = made.take_value();
      auto work = s.phase_->consume_work(size);
      if (!work.ok())
        return work;
      std::memcpy(copy.data(), bytes, size);
      auto write =
          s.builder_.prepare_append(field, rows, std::move(copy).freeze());
      if (!write.ok())
        return write.status();
      s.need_.io.push_back(write.take_value());
      return s.publication(Status::success());
    });
  }
  static int publish_field(void* c, std::uint32_t field, std::uint64_t end,
                           const ps_result_relation_row_v2* rows,
                           std::uint32_t count, std::uint32_t guarantee,
                           std::uint32_t finality) {
    return call(c, [&](CResultMemberBridge& s) {
      if (finality != 15)
        return invalid("incomplete field finality");
      auto relation = s.relation(end, rows, count, guarantee);
      return relation.ok() ? s.publication(s.builder_.publish(
                                 field, end, relation.take_value(),
                                 {true, true, true, true}))
                           : relation.status();
    });
  }
  static int need_field_read(void* c, std::uint32_t input, std::uint32_t field,
                             std::uint64_t first, std::uint64_t rows) {
    return call(c, [&](CResultMemberBridge& s) {
      auto found = s.phase_->results.find(input);
      if (found == s.phase_->results.end())
        return invalid("Result field input is not ready");
      auto facts = found->second.descriptor(false);
      if (!facts.ok())
        return facts.status();
      auto read = found->second.prepare_read(facts.value(), field, first, rows);
      if (!read.ok())
        return read.status();
      s.need_.io.push_back(read.take_value());
      return Status::success();
    });
  }
  static int read_io(void* c, std::uint32_t index, std::uint64_t offset,
                     std::uint8_t* bytes, std::uint64_t size) {
    return call(c, [&](CResultMemberBridge& s) {
      if (index >= s.phase_->io.size() || !bytes || !size || size > SIZE_MAX)
        return invalid("invalid I/O reply read");
      const auto* window =
          std::get_if<std::shared_ptr<const CpuStorage>>(&s.phase_->io[index]);
      if (!window || offset > (*window)->bytes().size() ||
          size > (*window)->bytes().size() - offset)
        return invalid("I/O reply bounds");
      auto work = s.phase_->consume_work(size);
      if (!work.ok())
        return work;
      std::memcpy(bytes, (*window)->bytes().data() + offset, size);
      return Status::success();
    });
  }
  static int publish_result(void* c, std::uint32_t complete) {
    return call(c, [&](CResultMemberBridge& s) {
      if (complete > 1)
        return invalid("invalid Result publication");
      if (!complete &&
          (!s.owns_payload_ ||
           s.definition_->traits.outputs[s.phase_->query.output_index]
                   .observation_kind == ObservationKind::RequestRecord))
        return invalid("terminal Result requires complete publication");
      if (s.published_.valid())
        return Status{ErrorCode::OperationFailed,
                      "Result plugin published more than once in one poll"};
      if (complete) {
        auto sealed = s.builder_.seal();
        if (!sealed.ok())
          return sealed.status();
        s.published_ = sealed.take_value();
      } else {
        s.published_ = s.builder_.reference();
        if (!s.published_.valid())
          return invalid("missing Result builder");
        auto facts = s.published_.descriptor(false);
        if (!facts.ok())
          return facts.status();
      }
      s.published_complete_ = complete != 0;
      return s.publication(Status::success());
    });
  }
  static int result_descriptor(void* c, std::uint32_t input,
                               ps_result_descriptor_v2* output) {
    return call(c, [&](CResultMemberBridge& s) {
      if (!output ||
          reinterpret_cast<std::uintptr_t>(output) %
              alignof(ps_result_descriptor_v2) ||
          output->struct_size != sizeof(*output))
        return invalid("invalid Result descriptor destination");
      auto found = s.phase_->results.find(input);
      if (found == s.phase_->results.end())
        return Status{ErrorCode::InvalidArgument,
                      "Result descriptor Need is absent",
                      FailureReason::UnauthorizedRead,
                      {FailureOrigin::Protocol, FailureScope::Group}};
      auto facts = found->second.descriptor(false);
      if (!facts.ok())
        return facts.status();
      *output = {};
      output->struct_size = sizeof(*output);
      output->sealed = facts.value().sealed();
      output->field_count = facts.value().field_count();
      output->tensor_count = facts.value().tensor_count();
      output->object_id = facts.value().object_id();
      output->revision = facts.value().revision();
      for (std::uint32_t i = 0; i < output->field_count; ++i)
        output->rows[i] = facts.value().rows(i);
      return Status::success();
    });
  }

  static ps_result_services_v2 service(Lease* lease) {
    return {sizeof(ps_result_services_v2),
            PS_RESULT_OPERATION_ABI_VERSION_2,
            lease,
            need_tensor,
            need_result,
            read_tensor,
            retain_tensor,
            read_retained,
            release_tensor,
            allocate,
            release_scratch,
            work,
            cancelled,
            begin,
            descriptor,
            publish_tensor,
            append_field,
            publish_field,
            need_field_read,
            read_io,
            publish_result,
            result_descriptor,
            acquire_tensor_window,
            acquire_retained_window,
            retain_window,
            release_window,
            make_mapping,
            make_reshape,
            release_relation,
            publish_tensor_with_relation,
            publish_tensor_view,
            report_numeric,
            make_prefix,
            make_neighborhood,
            nullptr,
            nullptr,
            nullptr,
            make_tensor_cartesian,
            acquire_native_tensor_window,
            acquire_native_atlas,
            create_block_state,
            read_block_state,
            release_block_state,
            block,
            publish_tensor_buffer,
            discover,
            discovery_requests,
            release_discovery,
            checkpoint_before,
            checkpoint_read,
            checkpoint_publish};
  }
  std::shared_ptr<const Definition> definition_;
  MutableBuffer bytes_;
  Retained retained_;
  Windows windows_;
  Relations relations_;
  BlockStates block_states_;
  bool in_block_ = false;
  Discoveries discoveries_;
  bool in_discovery_ = false, discovery_pending_ = false;
  ResourceVector<std::shared_ptr<WindowRecord>> window_records_;
  std::shared_ptr<FailureLatch> window_failure_;
  ResourceVector<MutableBuffer> scratch_;
  ResultProgramNeed need_;
  ResultBuilder builder_;
  ResultRef published_;
  const ResultProgramPhase* phase_ = nullptr;
  Status failure_;
  std::thread::id owner_;
  std::atomic<bool> violation_{false};
  std::atomic<bool> active_{false};
  bool entered_ = false, published_complete_ = false;
  bool publication_started_ = false;
  ResourceVector<std::shared_ptr<Lease>> leases_;
  bool owns_payload_;
  std::shared_ptr<FailureLatch> joint_failure_;
  std::shared_ptr<std::uint64_t> handles_;
  std::uint64_t owned_next_handle_ = 1;
  std::uint64_t& next_handle_;
};

std::uint64_t c_member_storage_bytes() noexcept {
  return sizeof(CResultMemberBridge);
}
Result<ResultContinuation> start_c_member(
    std::shared_ptr<const Definition> owner, const BufferAllocator& allocator) {
  auto allocated =
      allocator.allocate(std::max<std::uint64_t>(1, owner->api.state_bytes));
  if (!allocated.ok())
    return Result<ResultContinuation>(allocated.status());
  auto buffer = allocated.take_value();
  if (buffer.size())
    std::memset(buffer.data(), 0, buffer.size());
  return ResultContinuation::make<CResultMemberBridge>(allocator, owner,
                                                       std::move(buffer));
}
std::shared_ptr<CResultMemberBridge> make_c_joint_member(
    std::shared_ptr<const Definition> owner, const ResourceBudget& resources,
    std::shared_ptr<std::uint64_t> handles,
    std::shared_ptr<FailureLatch> failure) {
  return std::allocate_shared<CResultMemberBridge>(
      ResourceAllocator<CResultMemberBridge>(resources), std::move(owner),
      MutableBuffer{}, false, std::move(handles), std::move(failure));
}
Result<ResultProgramPoll> poll_c_member(CResultMemberBridge& owner,
                                        const ResultProgramPhase& phase,
                                        const MemberBorrow& borrow) {
  return owner.poll(phase, borrow);
}
Status c_member_failure(const CResultMemberBridge& owner) {
  return owner.failure_status();
}
bool c_member_terminal_conflicts(const CResultMemberBridge& owner) noexcept {
  return owner.terminal_failure_conflicts();
}
}  // namespace ps::plugin_internal::result_c
