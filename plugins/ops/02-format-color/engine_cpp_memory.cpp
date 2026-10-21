#include <exception>
#include <new>

#include "engine_memory.h"  // NOLINT(build/include_subdir)
extern "C" {
void* ps_engine_new(size_t n) {
  if (void* p = ps_engine_malloc(n ? n : 1)) {
    return p;
  }
  throw std::bad_alloc();
}
void* ps_engine_new_nothrow(size_t n, const void*) noexcept {
  return ps_engine_malloc(n ? n : 1);
}
void* ps_engine_new_aligned(size_t n, size_t alignment) {
  if (void* p = ps_engine_aligned_alloc(n ? n : 1, alignment)) {
    return p;
  }
  throw std::bad_alloc();
}
void* ps_engine_new_aligned_nothrow(size_t n, size_t alignment,
                                    const void*) noexcept {
  return ps_engine_aligned_alloc(n ? n : 1, alignment);
}
void ps_engine_delete(void* p) noexcept {
  ps_engine_free(p);
}
void ps_engine_delete_sized(void* p, size_t) noexcept {
  ps_engine_free(p);
}
void ps_engine_delete_sized_aligned(void* p, size_t, size_t) noexcept {
  ps_engine_free(p);
}
void ps_engine_delete_nothrow(void* p, const void*) noexcept {
  ps_engine_free(p);
}
void ps_engine_cpp_tick(uint64_t n) {
  if (ps_engine_scope && ps_engine_scope->armed &&
      !std::uncaught_exceptions() && ps_engine_scope->host.work &&
      !ps_engine_scope->host.work(ps_engine_scope->host.context, n)) {
    throw std::bad_alloc();
  }
}
}
