#include "photospider/plugin/data_definition_registry.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/data_provider_api.h"
#include "plugin/utf8_validation.hpp"

#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
#include "plugin/library_test_hooks.hpp"
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {
namespace {

/**
 * @brief Validates one bounded schema key.
 * @param key Candidate strict UTF-8 key.
 * @return True for 1..1024 well-formed bytes without ASCII control
 * characters.
 * @throws Nothing.
 * @note Key normalization remains provider/application policy.
 */
bool valid_schema_key(const std::string& key) noexcept {
  return plugin_internal::valid_utf8_key(key);
}

/**
 * @brief Converts a provider ABI element value to ElementType.
 * @param value Numeric provider record value.
 * @return ElementType or a typed validation failure.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note Unknown values fail closed.
 */
Result<ElementType> provider_element_type(std::uint32_t value) {
  switch (value) {
    case 1U:
      return Result<ElementType>(ElementType::UInt8);
    case 2U:
      return Result<ElementType>(ElementType::Int64);
    case 3U:
      return Result<ElementType>(ElementType::Float64);
    default:
      return Result<ElementType>(Status::failure(
          ErrorCode::TypeMismatch, "provider schema element type is unknown"));
  }
}

/**
 * @brief Owns one trusted provider library and mapped API records.
 *
 * @note Provider destroy runs before the native library unloads.
 */
class ProviderLibrary final {
 public:
  /**
   * @brief Takes immediate stack ownership of one opened provider library.
   * @param handle Platform library handle.
   * @throws Nothing.
   * @note Destroy ownership is attached after the API structure prefix is
   * proven readable; until then destruction closes only the native handle.
   */
  explicit ProviderLibrary(void* handle) noexcept : handle_(handle) {}

  /**
   * @brief Transfers pending ownership into its published heap lease.
   * @param other Source owner left empty.
   * @throws Nothing.
   * @note Handle and destroy responsibility move together exactly once.
   */
  ProviderLibrary(ProviderLibrary&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)),
        api_(std::exchange(other.api_, nullptr)) {}

  /**
   * @brief Calls provider destroy and unloads the library exactly once.
   * @throws Nothing.
   * @note A nonconforming C++ provider exception is fenced so the native
   * mapping can still be released.
   */
  ~ProviderLibrary() noexcept {
    if (api_ && api_->destroy) {
      try {
        api_->destroy(api_->schemas, api_->schema_count);
      } catch (...) {
      }
    }
#if defined(_WIN32)
    if (handle_) {
      FreeLibrary(static_cast<HMODULE>(handle_));
    }
#else
    if (handle_) {
      dlclose(handle_);
    }
#endif
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
    if (handle_) {
      plugin_testing::notify_native_close(
          plugin_testing::LibraryKind::Provider);
    }
#endif
  }

  /**
   * @brief Forbids duplicating native-library/destroy-hook ownership.
   * @param other Source owner that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Shared ownership is established only after heap allocation succeeds.
   */
  ProviderLibrary(const ProviderLibrary& other) = delete;
  /**
   * @brief Forbids assigning native-library/destroy-hook ownership.
   * @param other Source owner that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Exactly-one destroy-before-unload ownership never changes.
   */
  ProviderLibrary& operator=(const ProviderLibrary& other) = delete;
  /**
   * @brief Forbids replacing an established provider lease by move.
   * @param other Source owner that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Publication uses move construction exactly once.
   */
  ProviderLibrary& operator=(ProviderLibrary&& other) = delete;

  /**
   * @brief Returns the borrowed native handle for symbol lookup.
   * @return Nonnull handle while ownership is active.
   * @throws Nothing.
   * @note The caller never closes or transfers the borrowed value.
   */
  [[nodiscard]] void* handle() const noexcept { return handle_; }

  /**
   * @brief Attaches an API table whose exact structure prefix is readable.
   * @param api Mapped API table, possibly carrying malformed later fields.
   * @throws Nothing.
   * @note A nonnull destroy callback becomes part of rollback immediately.
   */
  void attach_api(const ps_data_provider_api_v1* api) noexcept { api_ = api; }

 private:
  /** @brief Platform library handle. */
  void* handle_ = nullptr;
  /** @brief Mapped provider record table. */
  const ps_data_provider_api_v1* api_ = nullptr;
};

/**
 * @brief Opens one explicit provider DSO.
 * @param path Exact startup-configured 1..4096-byte path with no embedded NUL.
 * @return Native handle, `InvalidArgument` for a malformed path, or `NotFound`
 * when the exact valid path cannot be loaded.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Path validation completes before any platform loader call. No
 * signature, certificate, or sandbox is involved.
 */
Result<void*> open_provider_library(const std::string& path) {
  if (path.empty() || path.size() > 4096U ||
      path.find('\0') != std::string::npos) {
    return Result<void*>(Status::failure(
        ErrorCode::InvalidArgument,
        "provider path is empty, too long, or contains an embedded NUL"));
  }
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
  plugin_testing::notify_native_load(plugin_testing::LibraryKind::Provider);
#endif
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(path.c_str());
  if (!handle) {
    return Result<void*>(Status::failure(
        ErrorCode::NotFound, "provider library could not be loaded"));
  }
  return Result<void*>(static_cast<void*>(handle));
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* error = dlerror();
    return Result<void*>(Status::failure(
        ErrorCode::NotFound,
        error ? std::string(error) : "provider library could not be loaded"));
  }
  return Result<void*>(handle);
#endif
}

/**
 * @brief Looks up one provider symbol.
 * @param handle Open native library.
 * @param name Exact symbol name.
 * @return Address or null.
 * @throws Nothing.
 * @note Caller converts the address to a function pointer with memcpy.
 */
void* provider_symbol(void* handle, const char* name) noexcept {
#if defined(_WIN32)
  return reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

}  // namespace

/**
 * @brief Private synchronized implementation of DataDefinitionRegistry.
 * @note Provider leases are declared before copied schemas so reverse member
 * destruction retires every copied record before provider destroy/unload.
 */
struct DataDefinitionRegistry::Impl final {
  /**
   * @brief Retires copied schemas before automatic provider-lease destruction.
   * @throws Nothing.
   * @note The explicit clear supplies a deterministic test observation point;
   * member declaration order independently preserves the same sequence.
   */
  ~Impl() noexcept {
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
    const std::size_t retired_schema_count = schemas.size();
#endif
    schemas.clear();
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
    plugin_testing::notify_provider_schemas_retired(retired_schema_count);
#endif
  }

  /** @brief Serializes registry mutation and lookup. */
  mutable std::mutex mutex;
  /** @brief Provider leases that must outlive every copied schema record. */
  std::vector<std::shared_ptr<ProviderLibrary>> providers;
  /** @brief Sorted copied schemas destroyed before the provider leases. */
  std::map<std::string, DataSchemaDefinition> schemas;
  /** @brief Monotonic mutation fence. */
  bool frozen = false;
};

/**
 * @brief Implements empty mutable data-definition registry construction.
 * @copydetails DataDefinitionRegistry::DataDefinitionRegistry
 */
DataDefinitionRegistry::DataDefinitionRegistry()
    : impl_(std::make_unique<Impl>()) {}

/**
 * @brief Implements copied-record retirement before provider destroy/unload.
 * @copydetails DataDefinitionRegistry::~DataDefinitionRegistry
 */
DataDefinitionRegistry::~DataDefinitionRegistry() noexcept = default;

/**
 * @brief Implements atomic copied schema registration.
 * @copydetails DataDefinitionRegistry::register_schema
 */
Status DataDefinitionRegistry::register_schema(
    DataSchemaDefinition definition) {
  if (!valid_schema_key(definition.key) || definition.maximum_rank == 0U ||
      definition.maximum_rank > 8U) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "data schema definition is malformed");
  }
  try {
    static_cast<void>(Value::element_size(definition.element_type));
  } catch (const std::invalid_argument&) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "data schema element type is unknown");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->frozen) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "data definition registry is frozen");
  }
  if (impl_->schemas.count(definition.key) != 0U) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "data schema key is already registered");
  }
  impl_->schemas.emplace(definition.key, std::move(definition));
  return Status::success();
}

/**
 * @brief Implements validated transactional provider DSO loading.
 * @copydetails DataDefinitionRegistry::load_provider
 */
Status DataDefinitionRegistry::load_provider(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->frozen) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "data definition registry is frozen");
    }
  }
  auto opened = open_provider_library(path);
  if (!opened.ok()) {
    return opened.status();
  }
  ProviderLibrary pending_provider(opened.value());
  using VersionFunction = std::uint32_t (*)();
  using ApiFunction = const ps_data_provider_api_v1* (*)();
  VersionFunction version = nullptr;
  ApiFunction get_api = nullptr;
  void* version_address = provider_symbol(pending_provider.handle(),
                                          "ps_data_provider_get_abi_version");
  void* api_address =
      provider_symbol(pending_provider.handle(), "ps_data_provider_get_api_v1");
  static_assert(sizeof(version) == sizeof(version_address),
                "supported targets require equal function/data pointer size");
  std::memcpy(&version, &version_address, sizeof(version));
  std::memcpy(&get_api, &api_address, sizeof(get_api));
  if (!version || !get_api) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "data provider ABI version is unsupported");
  }
  const ps_data_provider_api_v1* api = nullptr;
  try {
    if (version() != PS_DATA_PROVIDER_ABI_VERSION_1) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "data provider ABI version is unsupported");
    }
    api = get_api();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return Status::failure(ErrorCode::OperationFailed,
                           "data provider ABI entry point raised an exception");
  }
  if (!api ||
      reinterpret_cast<std::uintptr_t>(api) %
              alignof(ps_data_provider_api_v1) !=
          0U ||
      api->struct_size != sizeof(ps_data_provider_api_v1)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "data provider API table is malformed");
  }
  pending_provider.attach_api(api);
  if (api->schema_count == 0U || api->schema_count > 1024U || !api->schemas ||
      reinterpret_cast<std::uintptr_t>(api->schemas) %
              alignof(ps_data_schema_v1) !=
          0U ||
      !api->destroy) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "data provider API table is malformed");
  }
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
  plugin_testing::invoke_before_owner_allocation(
      plugin_testing::LibraryKind::Provider);
#endif
  // Transfer destroy-before-unload ownership only after the heap lease exists.
  // Allocation failure leaves the stack owner intact for exact rollback.
  auto provider =
      std::make_shared<ProviderLibrary>(std::move(pending_provider));

  std::vector<DataSchemaDefinition> staged;
  staged.reserve(api->schema_count);
  for (std::uint32_t index = 0; index < api->schema_count; ++index) {
    const ps_data_schema_v1& schema = api->schemas[index];
    if (schema.struct_size != sizeof(ps_data_schema_v1) || !schema.key ||
        schema.key_size == 0U || schema.key_size > 1024U ||
        schema.maximum_rank == 0U || schema.maximum_rank > 8U) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "data provider schema is malformed");
    }
    auto element_type = provider_element_type(schema.element_type);
    DataSchemaDefinition definition;
    definition.key.assign(schema.key, schema.key_size);
    definition.maximum_rank = schema.maximum_rank;
    if (!element_type.ok() || !valid_schema_key(definition.key) ||
        std::any_of(staged.begin(), staged.end(), [&](const auto& candidate) {
          return candidate.key == definition.key;
        })) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "data provider schema is invalid or duplicated");
    }
    definition.element_type = element_type.value();
    staged.push_back(std::move(definition));
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->frozen) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "data registry froze during provider load");
  }
  for (const DataSchemaDefinition& definition : staged) {
    if (impl_->schemas.count(definition.key) != 0U) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "data provider collides with registered schema");
    }
  }
  auto replacement_schemas = impl_->schemas;
  auto replacement_providers = impl_->providers;
  for (DataSchemaDefinition& definition : staged) {
    const auto inserted =
        replacement_schemas.emplace(definition.key, std::move(definition));
    if (!inserted.second) {
      return Status::failure(ErrorCode::Internal,
                             "data provider staging duplicated a schema");
    }
  }
  replacement_providers.push_back(std::move(provider));
  impl_->schemas.swap(replacement_schemas);
  impl_->providers.swap(replacement_providers);
  return Status::success();
}

/**
 * @brief Implements the permanent data-definition mutation fence.
 * @copydetails DataDefinitionRegistry::freeze
 */
Status DataDefinitionRegistry::freeze() noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->frozen = true;
  return Status::success();
}

/**
 * @brief Implements copied immutable schema lookup.
 * @copydetails DataDefinitionRegistry::find
 */
Result<DataSchemaDefinition> DataDefinitionRegistry::find(
    const std::string& key) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto iterator = impl_->schemas.find(key);
  if (iterator == impl_->schemas.end()) {
    return Result<DataSchemaDefinition>(Status::failure(
        ErrorCode::NotFound, "data schema key is not registered"));
  }
  return Result<DataSchemaDefinition>(iterator->second);
}

}  // namespace ps
