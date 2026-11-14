#include <stdlib.h>

#include "photospider/plugin/executor_module_api.h"

/* Synthetic synchronous event producer, not a scheduling implementation. */
typedef struct prepared {
  uint32_t call, tasks, exported;
  ps_executor_plan_v1 model;
} prepared;
static ps_kernel_status_v8 status(uint32_t code) {
  const ps_kernel_status_v8 result = {.code = code};
  return result;
}
static ps_kernel_status_v8 task_failure(void) {
  ps_kernel_status_v8 result = status(PS_KERNEL_OPERATION_FAILED_V8);
  result.reason = PS_KERNEL_REASON_INVALID_DOMAIN_V8;
  result.domain_code = 77;
  return result;
}
static void ignore(void* p) {
  (void)p;
}
typedef struct fixture_context {
  ps_executor_host_v1 host;
  unsigned mode;
} fixture_context;
enum {
  valid_export = 8,
  missing_export,
  wrong_id,
  missing_coverage,
  wrong_schema,
  bad_prepare,
  observer_throws,
  self_join,
  intermediate_export
};
static ps_kernel_status_v8 create(void* context, const ps_executor_host_v1* h,
                                  ps_executor_owner_v1* out) {
  (void)context;
  fixture_context* state = malloc(sizeof(*state));
  if (!state)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  state->host = *h;
  state->mode = h->cpu_workers - 1;
  *out = (ps_executor_owner_v1){state, free};
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 prepare(void* context, const ps_executor_plan_v1* p,
                                   ps_executor_owner_v1* out) {
  const fixture_context* state = context;
  if (state->mode == bad_prepare) {
    ps_executor_kernel_v1 kernel = {0};
    ps_kernel_status_v8 result =
        state->host.prepare_kernel(state->host.handle, p, 0, NULL, &kernel);
    if (kernel.owner.release)
      kernel.owner.release(kernel.owner.object);
    if (result.code)
      return result;
  }
  if (p->tasks != 2)
    return status(PS_KERNEL_TYPE_MISMATCH_V8);
  ps_executor_task_v1 task;
  ps_kernel_status_v8 result = p->task_info(p->handle, 0, &task);
  if (result.code)
    return result;
  prepared* owner = malloc(sizeof(*owner));
  if (!owner)
    return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
  if (task.write_count != 1) {
    free(owner);
    return status(PS_KERNEL_INVALID_ARGUMENT_V8);
  }
  ps_executor_tile_v1 tile;
  result = p->tile(p->handle, task.writes[0], &tile);
  if (result.code) {
    free(owner);
    return result;
  }
  *owner = (prepared){task.call, p->tasks, tile.value, *p};
  *out = (ps_executor_owner_v1){owner, free};
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 succeeded(const fixture_context* ctx,
                                     const prepared* plan,
                                     const ps_executor_submit_v2* q) {
  const unsigned mode = ctx->mode;
  uint32_t value = mode == wrong_id ? UINT32_MAX : plan->exported;
  ps_executor_result_builder_v1 builder = {0};
  ps_executor_owner_v1 result = {0};
  const ps_string_v8 first = {"one", 3}, second = {"two", 3};
  ps_string_v8 name = mode == wrong_schema ? second : first;
  ps_kernel_status_v8 s;
  if (mode == intermediate_export) {
    const void* service = NULL;
    const char key[] = PS_EXECUTOR_VALUE_RESULTS_V1;
    s = ctx->host.query_extension(ctx->host.handle,
                                  (ps_string_v8){key, sizeof(key) - 1},
                                  (ps_string_v8){"2", 1}, &service);
    if (s.code != PS_KERNEL_TYPE_MISMATCH_V8 || service)
      return status(PS_KERNEL_OPERATION_FAILED_V8);
    s = ctx->host.query_extension(ctx->host.handle,
                                  (ps_string_v8){key, sizeof(key) - 1},
                                  (ps_string_v8){"1", 1}, &service);
    if (s.code)
      return s;
    const ps_executor_value_results_v1* api = service;
    if (!api || api->struct_size != sizeof(*api) || !api->make_result)
      return status(PS_KERNEL_TYPE_MISMATCH_V8);
    s = api->make_result(ctx->host.handle, &plan->model, UINT32_MAX, &builder);
    if (s.code != PS_KERNEL_INVALID_ARGUMENT_V8 || builder.owner.object)
      return status(PS_KERNEL_OPERATION_FAILED_V8);
    s = api->make_result(ctx->host.handle, &plan->model, value, &builder);
  } else {
    s = ctx->host.make_result(ctx->host.handle, &plan->model, name, &builder);
  }
  if (s.code)
    return s;
  if (mode != missing_coverage) {
    ps_mapping_region_v1 region = {.rank = 1, .extent = {1}};
    const double data = 4;
    uint32_t bytes = mode == wrong_schema ? 1 : sizeof(data);
    s = builder.publish(builder.owner.object, 0, &region, (const uint8_t*)&data,
                        bytes);
  }
  if (!s.code)
    s = builder.seal(builder.owner.object, &result);
  if (!s.code) {
    const void* ref = result.object;
    s = q->retire(q->handle, 7, 3, 1, plan->call, PS_EXECUTOR_CALL_SUCCEEDED_V2,
                  PS_EXECUTOR_TASK_NONE_V2, status(PS_KERNEL_OK_V8), &value,
                  &ref, mode == missing_export ? 0 : 1);
  }
  if (result.release)
    result.release(result.object);
  builder.owner.release(builder.owner.object);
  return s;
}
typedef struct deferred_event {
  ps_executor_submit_v2 request;
  ps_executor_owner_v1 retained;
  uint32_t call;
} deferred_event;
static int synchronous_marker;
static void release_event(void* raw) {
  deferred_event* event = raw;
  event->retained.release(event->retained.object);
  free(event);
}
static ps_kernel_status_v8 submit(void* context, void* p,
                                  const ps_executor_submit_v2* q,
                                  ps_executor_owner_v1* out) {
  const fixture_context* state = context;
  const unsigned mode = state->mode;
  const prepared* plan = p;
  if (mode == self_join) {
    deferred_event* event = malloc(sizeof(*event));
    if (!event)
      return status(PS_KERNEL_RESOURCE_EXHAUSTED_V8);
    event->request = *q;
    event->call = plan->call;
    ps_kernel_status_v8 s = q->retain(q->handle, &event->retained);
    if (s.code) {
      free(event);
      return s;
    }
    *out = (ps_executor_owner_v1){event, release_event};
    return status(PS_KERNEL_OK_V8);
  }
  uint32_t call = plan->call, task = 0;
  uint32_t outcome = PS_EXECUTOR_CALL_FAILED_V2;
  uint64_t source = 1;
  ps_kernel_status_v8 result = task_failure();
  switch (mode) {
    case 1:
      task = PS_EXECUTOR_TASK_NONE_V2;
      break;
    case 2:
      task = plan->tasks;
      break;
    case 3:
      outcome = PS_EXECUTOR_CALL_SUCCEEDED_V2;
      result = status(PS_KERNEL_OK_V8);
      break;
    case 4:
      result = status(PS_KERNEL_OK_V8);
      break;
    case 5:
      task = 1;
      break; /* In range, but owned by the second call. */
    case 6:
      call = UINT32_MAX;
      break;
    case 7:
      source = 99;
      break;
    default:
      break;
  }
  if ((mode >= valid_export && mode <= wrong_schema) ||
      mode == intermediate_export) {
    result = succeeded(state, plan, q);
  } else {
    result = q->retire(q->handle, 7, 3, source, call, outcome, task, result,
                       NULL, NULL, 0);
    if (mode == observer_throws) {
      // A provider retry must not call a failed observer a second time.
      (void)q->retire(q->handle, 8, 3, source, call, outcome, task,
                      task_failure(), NULL, NULL, 0);
      result = status(PS_KERNEL_OK_V8);
    }
  }
  if (result.code)
    return result;
  *out = (ps_executor_owner_v1){&synchronous_marker, ignore};
  return status(PS_KERNEL_OK_V8);
}
static ps_kernel_status_v8 join(void* p, const ps_executor_results_v1* sink) {
  if (p != &synchronous_marker) {
    deferred_event* e = p;
    ps_kernel_status_v8 retired = e->request.retire(
        e->request.handle, 7, 3, 1, e->call, PS_EXECUTOR_CALL_FAILED_V2, 0,
        task_failure(), NULL, NULL, 0);
    if (retired.code)
      return retired;
  }
  sink->failure->flags = 4;
  sink->failure->task = 0;
  sink->failure->node = 1;
  return task_failure();
}
static unsigned zero(void* p) {
  (void)p;
  return 0;
}
static ps_kernel_status_v8 observe(void* p, ps_executor_owner_v1* out) {
  *out = (ps_executor_owner_v1){p, ignore};
  return status(PS_KERNEL_OK_V8);
}
static void snapshot(void* p, void* out) {
  (void)p;
  (void)out;
}
static uint64_t trace(void* p, uint32_t kind, void* out, uint64_t count) {
  (void)p;
  (void)kind;
  (void)out;
  /* Exercise the host's public bound on copied trace records. */
  return count + 1;
}
static ps_kernel_status_v8 description(void* p, void* context,
                                       void (*write)(void*, ps_string_v8)) {
  (void)p;
  write(context, (ps_string_v8){"retirement fixture", 18});
  return status(PS_KERNEL_OK_V8);
}
PS_OPERATION_MODULE_EXPORT const ps_executor_module_v2*
ps_executor_module_get_api_v2(void) {
  static const ps_executor_module_v2 api = {.struct_size = sizeof(api),
#ifdef PS_BAD_EXECUTOR_VERSION
                                            .abi_version = 1,
#else
                                            .abi_version =
                                                PS_EXECUTOR_MODULE_ABI_VERSION_2,
#endif
                                            .physical_version = 5,
                                            .backend_mask = 1,
                                            .domain_mask = 1,
                                            .invocation_mask = 1U << 8,
                                            .name = {"retirement", 10},
                                            .create = create,
                                            .close = ignore,
                                            .prepare = prepare,
                                            .submit = submit,
                                            .cancel = zero,
                                            .join = join,
                                            .observe = observe,
                                            .snapshot = snapshot,
                                            .trace = trace,
                                            .release_cache = zero,
                                            .workers = zero,
                                            .description = description,
                                            .destroy = ignore};
  return &api;
}
