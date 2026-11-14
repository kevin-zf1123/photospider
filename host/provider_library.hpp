#pragma once
#include <memory>
#include <string>

#include "photospider/plugin/fixed_invocation.hpp"
#include "photospider/plugin/operation_module_api.h"
namespace ps::host_internal {
/** @brief Loads one explicitly selected native library. Its last owner calls
 * destroy before unloading code. Callback strings are copied by the registry.
 */
class ProviderLibrary final {
 public:
  static Result<std::shared_ptr<ProviderLibrary>> open(const std::string& path,
                                                       bool operation = false);
  ~ProviderLibrary() noexcept;
  void* symbol(const char* name) const noexcept;
  void adopt(void* context, void (*destroy)(void*)) noexcept;

 private:
  bool operation_ = false;
  void* native_ = nullptr;
  void* context_ = nullptr;
  void (*destroy_)(void*) = nullptr;
};
Result<std::string> provider_text(ps_string_v8 view, bool optional = false);
template <class Table>
Result<const Table*> query_provider(ProviderLibrary& library, const char* name,
                                    uint32_t expected_version = 1) {
  auto entry = reinterpret_cast<const Table* (*)()>(library.symbol(name));
  if (!entry)
    return Result<const Table*>(
        {ErrorCode::TypeMismatch, "missing provider ABI entry"});
  const auto* table = entry();
  if (!table || reinterpret_cast<uintptr_t>(table) % alignof(Table) ||
      table->struct_size != sizeof(Table) ||
      table->abi_version != expected_version || !table->destroy)
    return Result<const Table*>(
        {ErrorCode::InvalidArgument, "invalid provider table"});
  library.adopt(table->context, table->destroy);
  return Result<const Table*>(table);
}
}  // namespace ps::host_internal
