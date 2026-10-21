#include "engine_memory.h"  // NOLINT(build/include_subdir)

#include <pthread.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
PS_THREAD_LOCAL ps_engine_scope_v1* ps_engine_scope;
ps_engine_scope_v1** ps_engine_scope_address(void) {
  return &ps_engine_scope;
}
/* Foreign ABI/control allocations are recognized without reading before them.
 * Every tracked header is included in the allocation's capacity admission. */
typedef struct ps_engine_header {
  void* user;
  void* context;
  void (*release)(void*);
  struct ps_engine_header* next;
  size_t size;
  max_align_t alignment;
} ps_engine_header;
#define PS_BUCKETS 4096
static ps_engine_header* blocks[PS_BUCKETS];
static pthread_mutex_t blocks_mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t bucket(void* p) {
  return ((uintptr_t)p >> 4) % PS_BUCKETS;
}
void* ps_engine_aligned_alloc(size_t n, size_t alignment) {
  ps_engine_header* h;
  uintptr_t address;
  if (alignment < alignof(max_align_t)) {
    alignment = alignof(max_align_t);
  }
  if ((alignment & (alignment - 1)) || alignment > SIZE_MAX - sizeof(*h) ||
      n > SIZE_MAX - sizeof(*h) - alignment) {
    return NULL;
  }
  if (ps_engine_scope && ps_engine_scope->host.allocate) {
    h = ps_engine_scope->host.allocate(ps_engine_scope->host.context,
                                       sizeof(*h) + n + alignment,
                                       alignof(max_align_t));
    if (!h) {
      return NULL;
    }
    h->context = ps_engine_scope->host.context;
    h->release = ps_engine_scope->host.release;
  } else {
    h = malloc(sizeof(*h) + n + alignment);
    if (!h) {
      return NULL;
    }
    h->context = NULL;
    h->release = free;
  }
  address = ((uintptr_t)(h + 1) + alignment - 1) & ~(uintptr_t)(alignment - 1);
  h->user = (void*)address;
  h->size = n;
  pthread_mutex_lock(&blocks_mutex);
  h->next = blocks[bucket(h->user)];
  blocks[bucket(h->user)] = h;
  pthread_mutex_unlock(&blocks_mutex);
  return h->user;
}
void* ps_engine_malloc(size_t n) {
  return ps_engine_aligned_alloc(n, alignof(max_align_t));
}
static ps_engine_header* remove_block(void* p) {
  ps_engine_header** slot;
  ps_engine_header* found = NULL;
  pthread_mutex_lock(&blocks_mutex);
  for (slot = &blocks[bucket(p)]; *slot; slot = &(*slot)->next) {
    if ((*slot)->user == p) {
      found = *slot;
      *slot = found->next;
      break;
    }
  }
  pthread_mutex_unlock(&blocks_mutex);
  return found;
}
void ps_engine_free(void* p) {
  ps_engine_header* h;
  if (!p) {
    return;
  }
  h = remove_block(p);
  if (h) {
    h->release(h);
  } else {
    free(p);
  }
}
void ps_color_release_allocations_v1(void* context) {
  size_t i;
  if (!context) {
    return;
  }
  for (i = 0; i < PS_BUCKETS; ++i) {
    for (;;) {
      ps_engine_header** slot;
      ps_engine_header* found = NULL;
      pthread_mutex_lock(&blocks_mutex);
      for (slot = &blocks[i]; *slot; slot = &(*slot)->next) {
        if ((*slot)->context == context) {
          found = *slot;
          *slot = found->next;
          break;
        }
      }
      pthread_mutex_unlock(&blocks_mutex);
      if (!found) {
        break;
      }
      found->release(found);
    }
  }
}
void* ps_engine_calloc(size_t n, size_t w) {
  void* p;
  if (w && n > SIZE_MAX / w) {
    return NULL;
  }
  p = ps_engine_malloc(n * w);
  if (p) {
    memset(p, 0, n * w);
  }
  return p;
}
void* ps_engine_realloc(void* p, size_t n) {
  ps_engine_header* h;
  size_t previous = 0;
  void* next;
  if (!p) {
    return ps_engine_malloc(n);
  }
  if (!n) {
    ps_engine_free(p);
    return NULL;
  }
  pthread_mutex_lock(&blocks_mutex);
  for (h = blocks[bucket(p)]; h; h = h->next) {
    if (h->user == p) {
      previous = h->size;
      break;
    }
  }
  pthread_mutex_unlock(&blocks_mutex);
  if (!h) {
    return realloc(p, n);
  }
  next = ps_engine_malloc(n);
  if (!next) {
    return NULL;
  }
  memcpy(next, p, previous < n ? previous : n);
  ps_engine_free(p);
  return next;
}
char* ps_engine_strdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* out = ps_engine_malloc(n);
  if (out) {
    memcpy(out, s, n);
  }
  return out;
}
int ps_engine_posix_memalign(void** out, size_t alignment, size_t size) {
  void* p;
  if (alignment < sizeof(void*) || (alignment & (alignment - 1))) {
    return 22;
  }
  p = ps_engine_aligned_alloc(size, alignment);
  if (!p) {
    return 12;
  }
  *out = p;
  return 0;
}
void ps_engine_error(const char* text) {
  if (ps_engine_scope && ps_engine_scope->error &&
      ps_engine_scope->error_size && !ps_engine_scope->error[0]) {
    snprintf(ps_engine_scope->error, ps_engine_scope->error_size, "%s", text);
  }
}
