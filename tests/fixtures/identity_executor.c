#include <stdlib.h>
#include <string.h>

#include "photospider/plugin/executor_module_api.h"
/* Independent synchronous provider for a Plan which returns its sole input. */
typedef struct state {
  ps_executor_host_v1 host;
} state;
typedef struct run {
  ps_executor_host_v1 host;
  ps_executor_owner_v1 result;
  char name[129];
  unsigned length;
} run;
static ps_kernel_status_v8 status(uint32_t code) {
  ps_kernel_status_v8 s = {0};
  s.code = code;
  return s;
}
static void ignore(void* p) {
  (void)p;
}
static ps_kernel_status_v8 create(void* c, const ps_executor_host_v1* h,
                                  ps_executor_owner_v1* out) {
  (void)c;
  state* s = malloc(sizeof(*s));
  if (!s)
    return status(5);
  s->host = *h;
  *out = (ps_executor_owner_v1){s, free};
  return status(0);
}
static ps_kernel_status_v8 prepare(void* c, const ps_executor_plan_v1* p,
                                   ps_executor_owner_v1* out) {
  (void)c;
  if (p->tasks || p->inputs != 1 || p->outputs != 1)
    return status(4);
  ps_string_v8 name;
  ps_kernel_status_v8 s = p->output(p->handle, 0, &name);
  if (s.code)
    return s;
  if (name.size > 128)
    return status(1);
  char* copy = calloc(name.size + 1, 1);
  if (!copy)
    return status(5);
  memcpy(copy, name.data, name.size);
  *out = (ps_executor_owner_v1){copy, free};
  return status(0);
}
static void retire(void* object) {
  run* r = object;
  r->result.release(r->result.object);
  free(r);
}
static ps_kernel_status_v8 submit(void* c, void* p,
                                  const ps_executor_submit_v2* q,
                                  ps_executor_owner_v1* out) {
  if (q->cancelled(q->handle))
    return status(6);
  ps_string_v8 name;
  const void* input;
  ps_kernel_status_v8 s = q->input(q->handle, 0, &name, &input);
  if (s.code)
    return s;
  run* r = calloc(1, sizeof(*r));
  if (!r)
    return status(5);
  s = ((state*)c)->host.retain_result(input, &r->result);
  if (s.code) {
    free(r);
    return s;
  }
  r->host = ((state*)c)->host;
  r->length = (unsigned)strlen(p);
  memcpy(r->name, p, r->length);
  *out = (ps_executor_owner_v1){r, retire};
  return status(0);
}
static ps_kernel_status_v8 join(void* p, const ps_executor_results_v1* sink) {
  run* r = p;
  ps_executor_diagnostics_v1 d = {sizeof(d), 0, 0, 0, 1};
  r->host.report_diagnostics(sink->diagnostics, &d);
  return sink->emit(sink->context, (ps_string_v8){r->name, r->length},
                    r->result.object);
}
static unsigned zero(void* c) {
  (void)c;
  return 0;
}
static ps_kernel_status_v8 observation(void* c, ps_executor_owner_v1* out) {
  (void)c;
  static int marker;
  *out = (ps_executor_owner_v1){&marker, ignore};
  return status(0);
}
static void snapshot(void* c, void* s) {
  (void)c;
  (void)s;
}
static uint64_t trace(void* c, uint32_t k, void* out, uint64_t n) {
  (void)c;
  (void)k;
  (void)out;
  (void)n;
  return 0;
}
static ps_kernel_status_v8 description(void* c, void* x,
                                       void (*w)(void*, ps_string_v8)) {
  (void)c;
  w(x, (ps_string_v8){"independent", 11});
  return status(0);
}
PS_OPERATION_MODULE_EXPORT const ps_executor_module_v2*
ps_executor_module_get_api_v2(void) {
  static const ps_executor_module_v2 api = {sizeof(api),
                                            PS_EXECUTOR_MODULE_ABI_VERSION_2,
                                            5,
                                            1,
                                            1,
                                            1u << 8,
                                            {"identity", 8},
                                            0,
                                            create,
                                            ignore,
                                            prepare,
                                            submit,
                                            zero,
                                            join,
                                            observation,
                                            snapshot,
                                            trace,
                                            zero,
                                            zero,
                                            description,
                                            ignore};
  return &api;
}
