#include <pthread.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "runtime.hpp"  // NOLINT(build/include_subdir)

namespace {
struct Result {
  bool passed = false;
  const float* table = nullptr;
};
void* check(void* opaque) {
  auto& result = *static_cast<Result*>(opaque);
  try {
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
      const auto& laplacian = px::table("laplacian");
      const auto& bayer = px::table("bayer");
      const auto& exact = px::table("exact32");
      const auto& palette = px::table("interp256");
      if (laplacian.size() != 9 || laplacian.data()[4] != 4 ||
          bayer.size() != 64 || bayer.data()[0] != 0 ||
          bayer.data()[1] != .5F || exact.size() != 65 * 65 ||
          palette.size() != 256 * 3 ||
          (result.table && result.table != exact.data()))
        return nullptr;
      result.table = exact.data();
    }
    for (const auto* key : {"interp1", "interp257", "zz"}) {
      try {
        (void)px::table(key);
        return nullptr;
      } catch (const std::out_of_range&) {
      }
    }
    result.passed = true;
  } catch (...) {
  }
  return nullptr;
}
}  // namespace
int main() {
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes))
    return 1;
  if (pthread_attr_setstacksize(&attributes, 128 * 1024)) {
    pthread_attr_destroy(&attributes);
    return 1;
  }
  pthread_t threads[4]{};
  Result results[4];
  unsigned started = 0;
  for (; started < 4; ++started)
    if (pthread_create(&threads[started], &attributes, check,
                       &results[started]))
      break;
  pthread_attr_destroy(&attributes);
  for (unsigned i = 0; i < started; ++i)
    if (pthread_join(threads[i], nullptr))
      return 1;
  if (started != 4)
    return 1;
  for (const auto& result : results)
    if (!result.passed || result.table != results[0].table)
      return 1;
  std::cout
      << "PASS: immutable coefficients on four concurrent 128 KiB stacks\n";
  return 0;
}
