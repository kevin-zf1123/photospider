#pragma once
#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>

#include "engine_api.h"  // NOLINT(build/include_subdir)
#ifdef __cplusplus
extern "C" {
#define PS_THREAD_LOCAL thread_local
#else
#define PS_THREAD_LOCAL _Thread_local
#endif
typedef struct ps_engine_scope_v1 {
  ps_color_host_v1 host;
  jmp_buf jump;
  int armed;
  char* error;
  size_t error_size;
} ps_engine_scope_v1;
#ifdef __cplusplus
ps_engine_scope_v1** ps_engine_scope_address(void);
#define ps_engine_scope (*ps_engine_scope_address())
#else
extern PS_THREAD_LOCAL ps_engine_scope_v1* ps_engine_scope;
#endif
void* ps_engine_malloc(size_t);
void* ps_engine_aligned_alloc(size_t, size_t);
char* ps_engine_strdup(const char*);
int ps_engine_posix_memalign(void**, size_t, size_t);
void* ps_engine_calloc(size_t, size_t);
void* ps_engine_realloc(void*, size_t);
void ps_engine_free(void*);
/* Inline the disabled apply-time path. This also lets the compiler hoist the
 * thread-local address out of shared evaluation loops. Construction still
 * performs exactly one host work admission per instrumented loop test. */
static inline void ps_engine_tick(void) {
  ps_engine_scope_v1* scope = ps_engine_scope;
  if (scope && scope->armed && scope->host.work &&
      !scope->host.work(scope->host.context, 1)) {
    longjmp(scope->jump, 1);
  }
}
void ps_engine_error(const char*);
#ifdef __cplusplus
}
#endif
