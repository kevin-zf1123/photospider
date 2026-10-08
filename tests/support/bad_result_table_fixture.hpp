#pragma once

#include <dlfcn.h>

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::test {
struct BadResultTableCase final {
  std::uint32_t id;
  const char* name;
  std::uint64_t expected_destroys = 1;
  ErrorCode expected_code = ErrorCode::InvalidArgument;
};
inline bool check_bad_result_tables(
    const char* path, std::initializer_list<BadResultTableCase> cases) {
  for (const auto& sample : cases) {
    // Destroy the registry and its candidate before releasing the observer;
    // release the last test observer handle for this case.
    std::unique_ptr<void, decltype(&dlclose)> library(
        dlopen(path, RTLD_NOW | RTLD_LOCAL), &dlclose);
    if (!library) {
      std::cerr << path << ": " << dlerror() << '\n';
      return false;
    }
    using Select = int (*)(std::uint32_t);
    using Count = std::uint64_t (*)();
    auto select = reinterpret_cast<Select>(
        dlsym(library.get(), "ps_test_select_bad_result_case"));
    auto count = reinterpret_cast<Count>(
        dlsym(library.get(), "ps_test_bad_result_destroy_count"));
    if (!select || !count || select(sample.id) != 0) {
      std::cerr << path << ": invalid case selector " << sample.id << '\n';
      return false;
    }
    const auto before = count();
    {
      OperationRegistry registry;
      const auto status = registry.load_plugin(path);
      if (status.code != sample.expected_code || !registry.keys().empty()) {
        std::cerr << path << " case " << sample.id << " (" << sample.name
                  << "): code=" << static_cast<int>(status.code)
                  << " reason=" << static_cast<int>(status.reason)
                  << " origin=" << static_cast<int>(status.detail.origin)
                  << " scope=" << static_cast<int>(status.detail.scope)
                  << " message=" << status.message << '\n';
        return false;
      }
    }
    if (count() - before != sample.expected_destroys) {
      std::cerr << path << " case " << sample.id << " (" << sample.name
                << "): destroy count=" << count() - before
                << ", expected=" << sample.expected_destroys << '\n';
      return false;
    }
  }
  return true;
}
}  // namespace ps::test
