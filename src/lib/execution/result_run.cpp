#include "execution/result_run.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "core/resource_observation.hpp"
#include "data/memory_budget.hpp"
#include "execution/callback_pool.hpp"
#include "execution/cpu_tiles.hpp"
#include "execution/execution_bindings.hpp"
#include "execution/execution_device.hpp"
#include "execution/native_upload_codec.hpp"
#include "execution/result_cache.hpp"
#include "execution/result_native_upload.hpp"
#include "execution/result_stage_submission.hpp"
#include "execution/structured_execution.hpp"
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif
namespace ps::execution_internal {
Result<ExecutionResult> run_result_plan(
    ThreadPool* pool, ThreadPool* gpu_pool,
    const std::shared_ptr<gpu_internal::Device>& native_device,
    const std::shared_ptr<execution_internal::NativeUploadRegistry>&
        native_uploads,
    WaitingAdmission* admission,
    const std::shared_ptr<data_internal::MemoryBudget>& budget,
    const std::shared_ptr<OperationRegistry>& operations,
    const ExecutionPlan& plan, std::function<bool()> current,
    std::vector<ExecutionBinding> bindings,
    const CancellationToken& caller_cancellation,
    const ExecutionOptions& options, const DemandQuery* requested,
    const std::string& snapshot_identity,
    execution_internal::ResultCache* dependency_cache,
    execution_internal::SharedResults* shared_results, bool atom_outcomes,
    execution_internal::ResultCheckpoints* result_checkpoints,
    std::shared_ptr<const ExecutionPlan> plan_owner) {
  auto payload_observation =
      budget->resources()
          ? std::allocate_shared<core_internal::PayloadObservation>(
                ResourceAllocator<core_internal::PayloadObservation>(
                    *budget->resources()))
          : std::make_shared<core_internal::PayloadObservation>();
  std::optional<core_internal::ResourcePayloadScope> payload_scope;
  if (budget->resources())
    payload_scope.emplace(
        *budget->resources(),
        core_internal::PayloadCapture{payload_observation, {}});
  ErrorCode coordinator_metadata_failure = ErrorCode::Ok;
  std::optional<ResourceAllocationScope> coordinator_resources;
  // Frozen cache/flight paths install their own optional-retention scopes;
  // keep their best-effort allocation failures outside this sticky fence.
  if (budget->resources() && !dependency_cache)
    coordinator_resources.emplace(*budget->resources(),
                                  &coordinator_metadata_failure);
  auto combined_cancellation =
      budget->resources()
          ? CancellationToken::combine(
                {caller_cancellation, options.dependencies.sets.cancellation},
                *budget->resources())
          : CancellationToken::combine(
                {caller_cancellation, options.dependencies.sets.cancellation});
  if (!combined_cancellation.ok())
    return Result<ExecutionResult>(combined_cancellation.status());
  const auto cancellation = combined_cancellation.take_value();
  const auto stop = [&] { return binding_stop(plan, cancellation); };
  const auto fail = [&](Status status) {
    const auto code = stop();
    if (code != ErrorCode::Ok &&
        status.detail.origin != FailureOrigin::Protocol) {
      status =
          Status{code,
                 {},
                 code == ErrorCode::Cancelled ? FailureReason::Cancelled
                                              : FailureReason::StaleVersion,
                 {FailureOrigin::Cancellation, FailureScope::Run}};
    }
    return Result<ExecutionResult>(std::move(status));
  };
  auto admitted_resources =
      budget->resources() ? plan.resources().reference(*budget->resources())
                          : Result<ResourceBindings>(plan.resources());
  if (!admitted_resources.ok())
    return fail(admitted_resources.status());
  const auto resources = admitted_resources.take_value();
  auto retained_inputs = retain_managed_inputs(&bindings, budget);
  if (!retained_inputs.ok())
    return fail(retained_inputs);
  if (atom_outcomes && !budget->resources())
    return fail(
        Status{ErrorCode::InvalidArgument,
               "atom execution requires a managed CPU Result dependency plan"});
  if (!budget->resources())
    return fail(Status{ErrorCode::InvalidArgument,
                       "structured execution requires managed_resources"});
  const auto stage_root = budget->resources()
                              ? budget->resources()
                              : std::make_shared<ResourceBudget>();
  std::shared_ptr<execution_internal::NativeRunUploads> uploads;
  if (native_device && native_device->available() &&
      std::any_of(
          plan.steps().begin(), plan.steps().end(),
          [](const auto& step) { return step.backend == Backend::Gpu; }))
    uploads = std::allocate_shared<execution_internal::NativeRunUploads>(
        ResourceAllocator<execution_internal::NativeRunUploads>(*stage_root),
        native_uploads, *stage_root);
  const auto cache_epoch = dependency_cache ? dependency_cache->epoch() : 0;
  auto result = execution_internal::execute_structured(
      plan, std::move(bindings), operations, *stage_root, options, cancellation,
      [current, cancellation] {
        return cancellation.cancelled() ? ErrorCode::Cancelled
               : current && current()   ? ErrorCode::Ok
                                        : ErrorCode::Stale;
      },
      [pool, gpu_pool, native_device, budget, stage_root, admission, current,
       uploads, dependency_cache, cache_epoch,
       maximum_parallelism = options.maximum_parallelism](
          Backend backend, bool whole, bool tiles, bool frozen_producer,
          CancellationToken token,
          std::function<Status(const execution_internal::StructuredServices&)>
              task) -> Result<execution_internal::StructuredSubmission> {
        using Submission = execution_internal::StructuredSubmission;
        using Answer = Result<Submission>;
        // A frozen shared producer serves peers independently of its
        // originating caller generation. Its shared token still controls
        // cancellation, and the caller retains its original final check.
        const auto callback_current =
            frozen_producer ? std::function<bool()>([] { return true; })
                            : current;
        if (backend == Backend::Gpu &&
            !gpu_lane_available(gpu_pool, native_device)) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
          execution_testing::notify_before_scheduler_failure(
              execution_testing::SchedulerFailurePoint::GpuBackendUnavailable);
#endif
          return Answer(Status{ErrorCode::BackendUnavailable,
                               "native GPU Result lane is unavailable"});
        }
        if (tiles) {
          auto issued = stage_root->consume({0, 0, 0, 1});
          if (!issued.ok())
            return Answer(issued);
          execution_internal::CpuTileScope scope(
              pool->ranges(), token, stage_root.get(),
              maximum_parallelism ? maximum_parallelism
                                  : pool->ranges().workers(),
              callback_current);
          execution_internal::StructuredServices services;
          services.native_gpu_available =
              gpu_lane_available(gpu_pool, native_device);
          services.native_storage = [native_device](const auto& storage) {
            return native_device && native_device->owns(storage);
          };
          services.cpu_tiles = scope.service();
          services.allocator = stage_root->allocator();
          services.observe = [&](auto& d) {
            d.cpu_stage_count += scope.stages();
            d.cpu_tile_callback_count += scope.tiles();
          };
          auto status = task(services);
          return Answer(
              Submission{{},
                         {},
                         scope.status().ok() ? status : scope.status()});
        }
        auto submitted = execution_internal::submit_result_stage(
            backend == Backend::Gpu ? gpu_pool : pool, admission,
            [pool, native_device, budget, stage_root, callback_current, backend,
             whole, uploads, dependency_cache, cache_epoch,
             native_gpu_available = gpu_lane_available(gpu_pool, native_device),
             token = std::move(token), task = std::move(task)] {
              if (token.cancelled())
                return Result<int>(Status{ErrorCode::Cancelled, {}});
              if (callback_current && !callback_current())
                return Result<int>(Status{ErrorCode::Stale, {}});
              execution_internal::StructuredServices services;
              services.native_storage = [native_device](const auto& storage) {
                return native_device && native_device->owns(storage);
              };
              services.native_gpu_available = native_gpu_available;
              services.allocator = stage_root->allocator();
              std::optional<gpu_internal::Invocation> native;
              std::uint64_t upload_hits = 0;
              if (backend == Backend::Gpu &&
                  (!native_device || !native_device->available())) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
                if (native_device)
#endif
                  return Result<int>(
                      Status{ErrorCode::BackendUnavailable,
                             "native GPU Result device became unavailable"});
              }
              // Scheduler fixtures borrow a GPU queue label without native
              // buffers/services or any claimed device dispatch work.
              if (backend == Backend::Gpu && native_device &&
                  native_device->available()) {
                services.allocator =
                    native_device->allocator(services.allocator);
                native.emplace(native_device, token, services.allocator);
                services.gpu = native->service();
                services.gpu_status = [&] { return native->status(); };
                services.gpu_dispatches = [&] {
                  return native->statistics().dispatches;
                };
                services.native_input =
                    [&](const Value& source,
                        const std::function<Status(std::uint64_t)>& cache_work)
                    -> Result<std::pair<Value, std::uint64_t>> {
                  using Answer = Result<std::pair<Value, std::uint64_t>>;
                  const auto stop = [&] {
                    return token.cancelled() ? ErrorCode::Cancelled
                           : callback_current && !callback_current()
                               ? ErrorCode::Stale
                               : ErrorCode::Ok;
                  };
                  if (stop() != ErrorCode::Ok)
                    return Answer(Status{stop(), {}});
                  if (native_device->owns(*source.storage()))
                    return Answer(std::make_pair(source, std::uint64_t{0}));
                  auto bytes =
                      region_bytes(source.descriptor(), source.region());
                  if (!bytes.ok())
                    return Answer(bytes.status());
                  std::string view_key;
                  std::shared_ptr<const CpuStorage> physical;
                  if (cache_work) {
                    auto issued = cache_work(32 + 5 * source.region().rank() +
                                             uploads->lookup_work());
                    if (!issued.ok() &&
                        issued.code != ErrorCode::ResourceExhausted)
                      return Answer(issued);
                    if (issued.ok()) {
                      view_key = upload_view_key(source);
                      physical = uploads->find(view_key, source.storage());
                    }
                  }
                  std::string content_key;
                  if (!physical && dependency_cache && cache_work &&
                      !view_key.empty()) {
                    const auto count = source.region().element_count();
                    const auto per_sample = 1 + source.region().rank();
                    if (!count.ok())
                      return Answer(count.status());
                    if (core_internal::can_multiply_add(
                            count.value(), per_sample, bytes.value())) {
                      const auto device_identity = native_device->identity();
                      const auto framing = 32 + device_identity.size() +
                                           3 * source.region().rank();
                      const auto sample_work =
                          bytes.value() + count.value() * per_sample;
                      auto issued =
                          core_internal::can_add(sample_work, framing)
                              ? cache_work(sample_work + framing)
                              : Status{ErrorCode::ResourceExhausted, {}};
                      if (!issued.ok() &&
                          issued.code != ErrorCode::ResourceExhausted)
                        return Answer(issued);
                      if (issued.ok()) {
                        auto key =
                            upload_content_key(source, device_identity, stop);
                        if (!key.ok())
                          return Answer(key.status());
                        content_key = key.take_value();
                        auto retained =
                            dependency_cache->get(content_key, cache_epoch);
                        if (retained.valid() &&
                            native_device->owns(*retained.storage()))
                          physical = retained.storage();
                      }
                    }
                  }
                  std::uint64_t copied_bytes = 0;
                  if (physical && native_device->owns(*physical)) {
                    ++upload_hits;
                  } else {
                    auto capacity =
                        native_device->allocation_capacity(bytes.value());
                    if (!capacity.ok())
                      return Answer(capacity.status());
                    if (capacity.value() < bytes.value())
                      return Answer(Status{ErrorCode::OperationFailed,
                                           "native capacity is too small"});
                    auto count = source.region().element_count();
                    const auto per_sample = 1 + source.region().rank();
                    if (!count.ok())
                      return Answer(count.status());
                    if (!core_internal::can_multiply_add(
                            count.value(), per_sample, bytes.value()))
                      return Answer(Status{ErrorCode::ResourceExhausted, {}});
                    auto issued = stage_root->consume(
                        {bytes.value() + count.value() * per_sample});
                    if (!issued.ok())
                      return Answer(issued);
                    if (stop() != ErrorCode::Ok)
                      return Answer(Status{stop(), {}});
                    auto copied =
                        transfer_value(source, services.allocator, true, stop);
                    if (!copied.ok())
                      return Answer(copied.status());
                    physical = copied.value().storage();
                    copied_bytes = bytes.value();
                    if (!content_key.empty() && stop() == ErrorCode::Ok)
                      dependency_cache->put(content_key, copied.value(),
                                            cache_epoch, true);
                  }
                  if (stop() != ErrorCode::Ok)
                    return Answer(Status{stop(), {}});
                  auto rebound = uploaded_view(source, physical);
                  if (!rebound.ok())
                    return Answer(rebound.status());
                  if (!view_key.empty())
                    uploads->put(view_key, source.storage(), physical);
                  return Answer(
                      std::make_pair(rebound.take_value(), copied_bytes));
                };
                services.observe = [&](auto& d) {
                  const auto& stats = native->statistics();
                  d.native_dispatch_count += stats.dispatches;
                  d.native_submission_count += stats.submissions;
                  d.native_compute_us += stats.device_us;
                  d.native_constant_bytes += stats.constant_bytes;
                  d.native_upload_hits += upload_hits;
                };
              }
              execution_internal::CpuRangeScope ranges(
                  pool->ranges(), token, stage_root.get(), callback_current);
              if (backend == Backend::Cpu && whole)
                services.cpu_parallel = ranges.service();
              auto status = task(services);
              if (!ranges.status().ok())
                status = ranges.status();
              if (native && !native->status().ok())
                status = native->status();
              return status.ok() ? Result<int>(1) : Result<int>(status);
            },
            stage_root.get());
        if (!submitted.ok())
          return Answer(submitted.status());
        auto completion = submitted.take_value();
        auto future = completion->future.share();
        return Answer(Submission{std::move(completion), std::move(future), {}});
      },
      requested, snapshot_identity, shared_results, result_checkpoints,
      dependency_cache, plan_owner, atom_outcomes,
      options.maximum_parallelism ? options.maximum_parallelism
                                  : pool->ranges().workers());
  if (!result.ok())
    return fail(result.status());
  auto completed = result.take_value();
  const auto peaks = payload_observation->peaks();
  completed.diagnostics.peak_live_bytes = peaks.first;
  completed.diagnostics.planned_peak_bytes = peaks.second;
  return Result<ExecutionResult>(std::move(completed));
}
}  // namespace ps::execution_internal
