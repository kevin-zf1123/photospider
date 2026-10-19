#include "execution/result_run.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/resource_observation.hpp"
#include "execution/callback_pool.hpp"
#include "execution/cpu_tiles.hpp"
#include "execution/execution_bindings.hpp"
#include "execution/execution_device.hpp"
#include "execution/native_upload_codec.hpp"
#include "execution/result_cache.hpp"
#include "execution/result_native_upload.hpp"
#include "execution/result_stage_submission.hpp"
#include "execution/structured_execution.hpp"
#include "photospider/core/checked_math.hpp"
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif
namespace ps::execution_internal {
namespace {
using StructuredTask = std::function<Status(const StructuredServices&)>;
using NativeInput = Result<std::pair<TensorSpan, std::uint64_t>>;

// Borrows the queued callback's captures only for its synchronous work call.
struct StageContext {
  ThreadPool* pool;
  const std::shared_ptr<gpu_internal::Device>& native_device;
  const std::shared_ptr<ResourceBudget>& resources;
  const std::function<bool()>& current;
  const std::shared_ptr<NativeRunUploads>& uploads;
  ResultCache* dependency_cache;
  std::uint64_t cache_epoch;
};

struct UploadLookup {
  std::string view_key;
  std::shared_ptr<const CpuStorage> physical;
  std::string content_key;
};

Status find_run_upload(const StageContext& context, const TensorSpan& source,
                       const std::function<Status(std::uint64_t)>& cache_work,
                       UploadLookup* lookup) {
  if (!cache_work || !context.uploads)
    return Status::success();
  auto issued = cache_work(32 + 5 * source.region().rank() +
                           context.uploads->lookup_work());
  if (!issued.ok())
    return issued.code == ErrorCode::ResourceExhausted ? Status::success()
                                                       : issued;
  lookup->view_key = upload_view_key(source);
  lookup->physical = context.uploads->find(lookup->view_key, source.storage());
  return Status::success();
}

Status find_cached_upload(
    const StageContext& context, const TensorSpan& source, std::uint64_t bytes,
    const std::function<Status(std::uint64_t)>& cache_work,
    const std::function<ErrorCode()>& stop, UploadLookup* lookup) {
  if (lookup->physical || !context.dependency_cache || !cache_work ||
      lookup->view_key.empty())
    return Status::success();
  const auto count = source.region().element_count();
  const auto per_sample = 1 + source.region().rank();
  if (!count.ok())
    return count.status();
  if (!core_internal::can_multiply_add(count.value(), per_sample, bytes))
    return Status::success();
  const auto device_identity = context.native_device->identity();
  const auto framing = 32 + device_identity.size() + 3 * source.region().rank();
  const auto sample_work = bytes + count.value() * per_sample;
  auto issued = core_internal::can_add(sample_work, framing)
                    ? cache_work(sample_work + framing)
                    : Status{ErrorCode::ResourceExhausted, {}};
  if (!issued.ok())
    return issued.code == ErrorCode::ResourceExhausted ? Status::success()
                                                       : issued;
  auto key = upload_content_key(source, device_identity, stop);
  if (!key.ok())
    return key.status();
  lookup->content_key = key.take_value();
  auto retained =
      context.dependency_cache->get(lookup->content_key, context.cache_epoch);
  if (retained.valid() && context.native_device->owns(*retained.storage()))
    lookup->physical = retained.storage();
  return Status::success();
}

Result<TensorSpan> copy_native_input(const StageContext& context,
                                     const TensorSpan& source,
                                     std::uint64_t bytes,
                                     const BufferAllocator& allocator,
                                     const std::function<ErrorCode()>& stop,
                                     const std::string& content_key) {
  auto capacity = context.native_device->allocation_capacity(bytes);
  if (!capacity.ok())
    return Result<TensorSpan>(capacity.status());
  if (capacity.value() < bytes)
    return Result<TensorSpan>(
        Status{ErrorCode::OperationFailed, "native capacity is too small"});
  auto count = source.region().element_count();
  const auto per_sample = 1 + source.region().rank();
  if (!count.ok())
    return Result<TensorSpan>(count.status());
  if (!core_internal::can_multiply_add(count.value(), per_sample, bytes))
    return Result<TensorSpan>(Status{ErrorCode::ResourceExhausted, {}});
  auto issued =
      context.resources->consume({bytes + count.value() * per_sample});
  if (!issued.ok())
    return Result<TensorSpan>(issued);
  if (stop() != ErrorCode::Ok)
    return Result<TensorSpan>(Status{stop(), {}});
  auto copied = transfer_value(source, allocator, true, stop);
  if (!copied.ok())
    return copied;
  if (!content_key.empty() && stop() == ErrorCode::Ok)
    context.dependency_cache->put(content_key, copied.value(),
                                  context.cache_epoch, true);
  return copied;
}

NativeInput upload_native_input(
    const StageContext& context, const CancellationToken& token,
    const TensorSpan& source,
    const std::function<Status(std::uint64_t)>& cache_work,
    const BufferAllocator& allocator, std::uint64_t* upload_hits) {
  const std::function<ErrorCode()> stop = [&] {
    return token.cancelled()                       ? ErrorCode::Cancelled
           : context.current && !context.current() ? ErrorCode::Stale
                                                   : ErrorCode::Ok;
  };
  if (stop() != ErrorCode::Ok)
    return NativeInput(Status{stop(), {}});
  if (context.native_device->owns(*source.storage()))
    return NativeInput(std::make_pair(source, std::uint64_t{0}));
  auto bytes = region_bytes(source.descriptor(), source.region());
  if (!bytes.ok())
    return NativeInput(bytes.status());
  UploadLookup lookup;
  auto status = find_run_upload(context, source, cache_work, &lookup);
  if (!status.ok())
    return NativeInput(status);
  status = find_cached_upload(context, source, bytes.value(), cache_work, stop,
                              &lookup);
  if (!status.ok())
    return NativeInput(status);
  std::uint64_t copied_bytes = 0;
  if (lookup.physical && context.native_device->owns(*lookup.physical)) {
    ++*upload_hits;
  } else {
    auto copied = copy_native_input(context, source, bytes.value(), allocator,
                                    stop, lookup.content_key);
    if (!copied.ok())
      return NativeInput(copied.status());
    lookup.physical = copied.value().storage();
    copied_bytes = bytes.value();
  }
  if (stop() != ErrorCode::Ok)
    return NativeInput(Status{stop(), {}});
  auto rebound = uploaded_view(source, lookup.physical);
  if (!rebound.ok())
    return NativeInput(rebound.status());
  if (!lookup.view_key.empty())
    context.uploads->put(lookup.view_key, source.storage(), lookup.physical);
  return NativeInput(std::make_pair(rebound.take_value(), copied_bytes));
}

Status configure_native_services(
    const StageContext& context, Backend backend,
    const CancellationToken& token, StructuredServices* services,
    std::optional<gpu_internal::Invocation>* native,
    std::uint64_t* upload_hits) {
  if (backend != Backend::Gpu)
    return Status::success();
  if (!context.native_device || !context.native_device->available()) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    if (context.native_device)
#endif
      return Status{ErrorCode::BackendUnavailable,
                    "native GPU Result device became unavailable"};
  }
  // Scheduler fixtures borrow a GPU queue label without native buffers/services
  // or any claimed device dispatch work. Recheck availability before setup.
  if (!context.native_device || !context.native_device->available())
    return Status::success();
  services->allocator = context.native_device->allocator(services->allocator);
  native->emplace(context.native_device, token, services->allocator);
  services->gpu = native->value().service();
  services->gpu_status = [native] { return native->value().status(); };
  services->gpu_dispatches = [native] {
    return native->value().statistics().dispatches;
  };
  // The pointees and borrowed references live in run_queued_stage, not here.
  services->native_input =
      [&context, &token, services, upload_hits](
          const TensorSpan& source,
          const std::function<Status(std::uint64_t)>& work) {
        return upload_native_input(context, token, source, work,
                                   services->allocator, upload_hits);
      };
  services->observe = [native, upload_hits](auto& diagnostics) {
    const auto& stats = native->value().statistics();
    diagnostics.native_dispatch_count += stats.dispatches;
    diagnostics.native_submission_count += stats.submissions;
    diagnostics.native_compute_us += stats.device_us;
    diagnostics.native_constant_bytes += stats.constant_bytes;
    diagnostics.native_upload_hits += *upload_hits;
  };
  return Status::success();
}

Result<int> run_queued_stage(const StageContext& context, Backend backend,
                             bool native_gpu_available,
                             const CancellationToken& token,
                             const StructuredTask& task) {
  if (token.cancelled())
    return Result<int>(Status{ErrorCode::Cancelled, {}});
  if (context.current && !context.current())
    return Result<int>(Status{ErrorCode::Stale, {}});
  StructuredServices services;
  services.native_storage = [native_device =
                                 context.native_device](const auto& storage) {
    return native_device && native_device->owns(storage);
  };
  services.native_gpu_available = native_gpu_available;
  services.allocator = context.resources->allocator();
  std::optional<gpu_internal::Invocation> native;
  std::uint64_t upload_hits = 0;
  auto configured = configure_native_services(context, backend, token,
                                              &services, &native, &upload_hits);
  if (!configured.ok())
    return Result<int>(configured);
  CpuRangeScope ranges(context.pool->ranges(), token, context.resources.get(),
                       context.current);
  if (backend == Backend::Cpu)
    services.cpu_parallel = ranges.service();
  auto status = task(services);
  if (!ranges.status().ok())
    status = ranges.status();
  if (native && !native->status().ok())
    status = native->status();
  if (!status.ok())
    return Result<int>(status);
  return Result<int>(1);
}

Result<StructuredSubmission> run_inline_tiles(
    ThreadPool* pool, ThreadPool* gpu_pool,
    const std::shared_ptr<gpu_internal::Device>& native_device,
    const std::shared_ptr<ResourceBudget>& resources,
    const std::function<bool()>& current, std::uint32_t maximum_parallelism,
    const CancellationToken& token, const StructuredTask& task) {
  auto issued = resources->consume({0, 1});
  if (!issued.ok())
    return Result<StructuredSubmission>(issued);
  CpuTileScope scope(
      pool->ranges(), token, resources.get(),
      maximum_parallelism ? maximum_parallelism : pool->ranges().workers(),
      current);
  StructuredServices services;
  services.native_gpu_available = gpu_lane_available(gpu_pool, native_device);
  services.native_storage = [native_device](const auto& storage) {
    return native_device && native_device->owns(storage);
  };
  services.cpu_tiles = scope.service();
  services.allocator = resources->allocator();
  services.observe = [&](auto& diagnostics) {
    diagnostics.cpu_stage_count += scope.stages();
    diagnostics.cpu_tile_callback_count += scope.tiles();
  };
  auto status = task(services);
  return Result<StructuredSubmission>(
      StructuredSubmission{{},
                           {},
                           scope.status().ok() ? status : scope.status()});
}

Result<StructuredSubmission> submit_queued_stage(
    ThreadPool* pool, ThreadPool* gpu_pool,
    const std::shared_ptr<gpu_internal::Device>& native_device,
    const std::shared_ptr<ResourceBudget>& resources,
    WaitingAdmission* admission, const std::function<bool()>& current,
    const std::shared_ptr<NativeRunUploads>& uploads,
    ResultCache* dependency_cache, std::uint64_t cache_epoch, Backend backend,
    CancellationToken token, StructuredTask task) {
  auto submitted = submit_result_stage(
      backend == Backend::Gpu ? gpu_pool : pool, admission,
      [pool, native_device, resources, current, backend, uploads,
       dependency_cache, cache_epoch,
       native_gpu_available = gpu_lane_available(gpu_pool, native_device),
       token = std::move(token), task = std::move(task)] {
        const StageContext context{pool,       native_device, resources,
                                   current,    uploads,       dependency_cache,
                                   cache_epoch};
        return run_queued_stage(context, backend, native_gpu_available, token,
                                task);
      },
      resources.get());
  if (!submitted.ok())
    return Result<StructuredSubmission>(submitted.status());
  auto completion = submitted.take_value();
  auto future = completion->future.share();
  return Result<StructuredSubmission>(
      StructuredSubmission{std::move(completion), std::move(future), {}});
}
}  // namespace

Result<ExecutionResult> run_result_plan(
    ThreadPool* pool, ThreadPool* gpu_pool,
    const std::shared_ptr<gpu_internal::Device>& native_device,
    const std::shared_ptr<execution_internal::NativeUploadRegistry>&
        native_uploads,
    WaitingAdmission* admission,
    const std::shared_ptr<ResourceBudget>& stage_root,
    const std::shared_ptr<OperationRegistry>& operations,
    const ExecutionPlan& plan, std::function<bool()> current,
    std::vector<ExecutionBinding> bindings,
    const CancellationToken& caller_cancellation,
    const ExecutionOptions& options, const DemandQuery* requested,
    execution_internal::ResultCache* dependency_cache,
    execution_internal::ResultCheckpoints* result_checkpoints) {
  auto payload_observation =
      std::allocate_shared<core_internal::PayloadObservation>(
          ResourceAllocator<core_internal::PayloadObservation>(*stage_root));
  core_internal::ResourcePayloadScope payload_scope(
      *stage_root, core_internal::PayloadCapture{payload_observation});
  ErrorCode coordinator_metadata_failure = ErrorCode::Ok;
  std::optional<ResourceAllocationScope> coordinator_resources;
  // Result cache paths install their own optional-retention scopes;
  // keep their best-effort allocation failures outside this sticky fence.
  if (!dependency_cache)
    coordinator_resources.emplace(*stage_root, &coordinator_metadata_failure);
  auto combined_cancellation = CancellationToken::combine(
      {caller_cancellation, options.dependencies.sets.cancellation},
      *stage_root);
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
  auto admitted_resources = plan.resources().reference(*stage_root);
  if (!admitted_resources.ok())
    return fail(admitted_resources.status());
  const auto resources = admitted_resources.take_value();
  auto owned_inputs = validate_input_ownership(bindings, *stage_root);
  if (!owned_inputs.ok())
    return fail(owned_inputs);
  std::shared_ptr<execution_internal::NativeRunUploads> uploads;
  if (native_device && native_device->available() &&
      std::any_of(
          plan.steps().begin(), plan.steps().end(), [](const auto& step) {
            return step.backend == Backend::Gpu ||
                   std::any_of(step.shared_stages.begin(),
                               step.shared_stages.end(), [](const auto& stage) {
                                 return stage->backend == Backend::Gpu;
                               });
          }))
    uploads = std::allocate_shared<execution_internal::NativeRunUploads>(
        ResourceAllocator<execution_internal::NativeRunUploads>(*stage_root),
        native_uploads, *stage_root);
  const auto cache_epoch = dependency_cache ? dependency_cache->epoch() : 0;
  // Demand selection filters the stored instruction stream before the
  // interpreter is entered. Loading/running a plan never invokes a mapper.
  std::optional<ExecutionPlan> selected_plan;
  if (requested) {
    auto selected = plan.select_outputs(*requested);
    if (!selected.ok())
      return fail(selected.status());
    selected_plan.emplace(selected.take_value());
  }
  const auto& executable = selected_plan ? *selected_plan : plan;
  auto result = execution_internal::execute_structured(
      executable, std::move(bindings), operations, *stage_root, options,
      cancellation,
      [current, cancellation] {
        return cancellation.cancelled() ? ErrorCode::Cancelled
               : current && current()   ? ErrorCode::Ok
                                        : ErrorCode::Stale;
      },
      [pool, gpu_pool, native_device, stage_root, admission, current, uploads,
       dependency_cache, cache_epoch,
       maximum_parallelism = options.maximum_parallelism](
          Backend backend, bool tiles, CancellationToken token,
          std::function<Status(const execution_internal::StructuredServices&)>
              task) -> Result<execution_internal::StructuredSubmission> {
        using Answer = Result<StructuredSubmission>;
        if (backend == Backend::Gpu &&
            !gpu_lane_available(gpu_pool, native_device)) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
          execution_testing::notify_before_scheduler_failure(
              execution_testing::SchedulerFailurePoint::GpuBackendUnavailable);
#endif
          return Answer(Status{ErrorCode::BackendUnavailable,
                               "native GPU Result lane is unavailable"});
        }
        if (tiles)
          return run_inline_tiles(pool, gpu_pool, native_device, stage_root,
                                  current, maximum_parallelism, token, task);
        return submit_queued_stage(pool, gpu_pool, native_device, stage_root,
                                   admission, current, uploads,
                                   dependency_cache, cache_epoch, backend,
                                   std::move(token), std::move(task));
      },
      nullptr, result_checkpoints, dependency_cache,
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
