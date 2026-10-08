// Copyright 2026 Photospider contributors

#include <cstdint>

extern "C" {
extern std::uintptr_t ___asan_globals_registered;
void __asan_unregister_image_globals(std::uintptr_t*) noexcept;
}

namespace {

// C++ atexit retirement runs while the module is still mapped. compiler-rt
// resets the image flag, making its generated destructor's later call harmless.
struct ImageRetirement {
  ~ImageRetirement() {
    __asan_unregister_image_globals(&___asan_globals_registered);
  }
};

ImageRetirement retirement;

}  // namespace
