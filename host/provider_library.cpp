#include "host/provider_library.hpp"

#include <memory>
#include <string>
#include <utility>

#include "photospider/core/callback_environment.hpp"
#include "photospider/core/utf8_validation.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
#include "sdk/operation/library_test_hooks.hpp"
#endif
namespace ps::host_internal {
Result<std::shared_ptr<ProviderLibrary>> ProviderLibrary::open(
    const std::string& path, bool operation) {
  using Answer = Result<std::shared_ptr<ProviderLibrary>>;
  core_internal::CallbackEnvironment env;
  if (!env.active())
    return Answer({ErrorCode::OperationFailed, {}});
  if (path.size() > 4096 || path.empty() ||
      path.find('\0') != std::string::npos)
    return Answer({ErrorCode::InvalidArgument, "invalid provider path"});
  auto result = std::make_shared<ProviderLibrary>();
  result->operation_ = operation;
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
  if (operation)
    plugin_testing::notify_native_load(plugin_testing::LibraryKind::Operation);
#endif
#ifdef _WIN32
  result->native_ = LoadLibraryA(path.c_str());
#else
  result->native_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
  return result->native_
             ? Answer(std::move(result))
             : Answer({ErrorCode::NotFound, "cannot open provider: " + path});
}
void* ProviderLibrary::symbol(const char* name) const noexcept {
#ifdef _WIN32
  return reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(native_), name));
#else
  return dlsym(native_, name);
#endif
}
void ProviderLibrary::adopt(void* context, void (*destroy)(void*)) noexcept {
  context_ = context;
  destroy_ = destroy;
}
ProviderLibrary::~ProviderLibrary() noexcept {
  core_internal::CallbackEnvironment env;
  if (destroy_) {
    try {
      destroy_(context_);
    } catch (...) {
    }
  }
  if (!native_)
    return;
#ifdef _WIN32
  FreeLibrary(static_cast<HMODULE>(native_));
#else
  dlclose(native_);
#endif
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
  if (operation_)
    plugin_testing::notify_native_close(plugin_testing::LibraryKind::Operation);
#endif
}
Result<std::string> provider_text(ps_string_v8 s, bool optional) {
  if (!s.size && optional)
    return Result<std::string>(std::string{});
  if (!s.data || !s.size || s.size > 1024)
    return Result<std::string>(
        {ErrorCode::InvalidArgument, "invalid provider string"});
  std::string value(s.data, s.size);
  if (!core_internal::valid_utf8_key(value))
    return Result<std::string>(
        {ErrorCode::InvalidArgument, "invalid provider UTF-8"});
  return Result<std::string>(std::move(value));
}
}  // namespace ps::host_internal
