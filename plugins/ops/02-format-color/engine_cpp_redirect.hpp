#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
// These are private symbol names, never process-wide replacements.
#if defined(__APPLE__)
#define PS_ENGINE_ASM(name) __asm__("_" name)
#else
#define PS_ENGINE_ASM(name) __asm__(name)
#endif
void* operator new(std::size_t) PS_ENGINE_ASM("ps_engine_new");
void* operator new[](std::size_t) PS_ENGINE_ASM("ps_engine_new");
void* operator new(std::size_t, const std::nothrow_t&) noexcept
    PS_ENGINE_ASM("ps_engine_new_nothrow");
void* operator new[](std::size_t, const std::nothrow_t&) noexcept
    PS_ENGINE_ASM("ps_engine_new_nothrow");
void* operator new(std::size_t, std::align_val_t)
    PS_ENGINE_ASM("ps_engine_new_aligned");
void* operator new[](std::size_t, std::align_val_t)
    PS_ENGINE_ASM("ps_engine_new_aligned");
void* operator new(std::size_t, std::align_val_t,
                   const std::nothrow_t&) noexcept
    PS_ENGINE_ASM("ps_engine_new_aligned_nothrow");
void* operator new[](std::size_t, std::align_val_t,
                     const std::nothrow_t&) noexcept
    PS_ENGINE_ASM("ps_engine_new_aligned_nothrow");
// Assembler-linked function declaration, not a cast.
// NOLINTNEXTLINE(readability/casting)
void operator delete(void*) noexcept PS_ENGINE_ASM("ps_engine_delete");
// NOLINTNEXTLINE(readability/casting)
void operator delete[](void*) noexcept PS_ENGINE_ASM("ps_engine_delete");
void operator delete(void*, std::size_t) noexcept
    PS_ENGINE_ASM("ps_engine_delete_sized");
void operator delete[](void*, std::size_t) noexcept
    PS_ENGINE_ASM("ps_engine_delete_sized");
void operator delete(void*, std::align_val_t) noexcept
    PS_ENGINE_ASM("ps_engine_delete_sized");
void operator delete[](void*, std::align_val_t) noexcept
    PS_ENGINE_ASM("ps_engine_delete_sized");
void operator delete(void*, std::size_t, std::align_val_t) noexcept
    PS_ENGINE_ASM("ps_engine_delete_sized_aligned");
void operator delete[](void*, std::size_t, std::align_val_t) noexcept
    PS_ENGINE_ASM("ps_engine_delete_sized_aligned");
void operator delete(void*, const std::nothrow_t&) noexcept
    PS_ENGINE_ASM("ps_engine_delete_nothrow");
void operator delete[](void*, const std::nothrow_t&) noexcept
    PS_ENGINE_ASM("ps_engine_delete_nothrow");
extern "C" {
void* malloc(std::size_t) PS_ENGINE_ASM("ps_engine_malloc");
void* calloc(std::size_t, std::size_t) PS_ENGINE_ASM("ps_engine_calloc");
void* realloc(void*, std::size_t) PS_ENGINE_ASM("ps_engine_realloc");
// NOLINTNEXTLINE(readability/casting)
void free(void*) PS_ENGINE_ASM("ps_engine_free");
char* strdup(const char*) PS_ENGINE_ASM("ps_engine_strdup");
int posix_memalign(void**, std::size_t, std::size_t)
    PS_ENGINE_ASM("ps_engine_posix_memalign");
void ps_engine_cpp_tick(std::uint64_t);
void ps_engine_ocio_bridge(const char*);
void ps_engine_ocio_record(const char*, const char*);
int ps_engine_ocio_file_enter(const void*);
void ps_engine_ocio_file_leave(int);
int ps_engine_ocio_strict_interpolation(void);
}

#undef PS_ENGINE_ASM
