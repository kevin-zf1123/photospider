#include "photospider/plugin/operation_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/core/checked_math.hpp"
#include "photospider/core/utf8_validation.hpp"
#include "plugin/failure_latch.hpp"
#include "plugin/operation_exception.hpp"
#include "plugin/operation_resources.hpp"
#include "plugin/port_validation.hpp"
#include "plugin/result_payload_bound.hpp"
#include "plugin/result_plugin.hpp"

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
 * @brief Returns whether a public key is bounded canonical strict UTF-8.
 * @param key Candidate operation/schema key.
 * @return True for a nonempty 1..1024-byte well-formed key without ASCII
 * control bytes.
 * @throws Nothing.
 * @note Unicode normalization is intentionally outside operation identity.
 */
bool valid_key(const std::string& key) noexcept {
  return core_internal::valid_utf8_key(key);
}

/**
 * @brief Owns one trusted native library and its plugin record lifetime.
 *
 * @note Destruction calls the plugin destroy hook before unloading the DSO.
 */
class OperationLibrary final {
 public:
  /**
   * @brief Takes immediate stack ownership of one opened native library.
   * @param handle Platform library handle.
   * @throws Nothing.
   * @note The API destroy callback is attached only after its table prefix is
   * safe to read; until then destruction closes only the native handle.
   */
  explicit OperationLibrary(void* handle) noexcept : handle_(handle) {}

  /**
   * @brief Transfers pending ownership into its published heap lease.
   * @param other Source owner left empty.
   * @throws Nothing.
   * @note Handle and destroy responsibility move together exactly once.
   */
  OperationLibrary(OperationLibrary&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)),
        result_api_(std::exchange(other.result_api_, nullptr)) {}

  /**
   * @brief Releases records and unloads the native library.
   * @throws Nothing.
   * @note Exceptions cannot cross a C destroy callback; a misbehaving native
   * plugin remains outside the correctness guarantee of the host process.
   */
  ~OperationLibrary() noexcept {
    core_internal::CallbackEnvironment environment;
    if (result_api_ && result_api_->destroy) {
      try {
        core_internal::CallbackEnvironment environment;
        result_api_->destroy(result_api_->context);
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
          plugin_testing::LibraryKind::Operation);
    }
#endif
  }

  /**
   * @brief Forbids duplicating native-library/destroy-hook ownership.
   * @param other Source owner that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Shared ownership is established outside this lifetime object.
   */
  OperationLibrary(const OperationLibrary& other) = delete;
  /**
   * @brief Forbids assigning native-library/destroy-hook ownership.
   * @param other Source owner that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Exactly-one destroy-before-unload ownership never changes.
   */
  OperationLibrary& operator=(const OperationLibrary& other) = delete;
  /**
   * @brief Forbids replacing an established native-library lease by move.
   * @param other Source owner that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Publication uses move construction exactly once.
   */
  OperationLibrary& operator=(OperationLibrary&& other) = delete;

  /**
   * @brief Returns the borrowed native handle for symbol lookup.
   * @return Nonnull handle while ownership is active.
   * @throws Nothing.
   * @note The caller never closes or transfers the borrowed value.
   */
  [[nodiscard]] void* handle() const noexcept { return handle_; }

  void attach_result_api(
      const ps_result_operation_plugin_api_v5* api) noexcept {
    result_api_ = api;
  }

 private:
  /** @brief Platform native library handle. */
  void* handle_ = nullptr;
  /** @brief Mapped immutable plugin API table. */
  const ps_result_operation_plugin_api_v5* result_api_ = nullptr;
};

/**
 * @brief Opens one explicit native library path.
 * @param path Exact caller-provided 1..4096-byte path with no embedded NUL.
 * @return Native handle, `InvalidArgument` for a malformed path, or `NotFound`
 * when the exact valid path cannot be loaded.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Path validation completes before any platform loader call. The
 * function performs no path trust/signature/admission operation.
 */
Result<void*> open_library(const std::string& path) {
  core_internal::CallbackEnvironment environment;
  if (!environment.active())
    return Result<void*>(Status{ErrorCode::OperationFailed, {}});
  if (path.empty() || path.size() > 4096U ||
      path.find('\0') != std::string::npos) {
    return Result<void*>(
        Status::failure(ErrorCode::InvalidArgument,
                        "operation plugin path is empty, too long, or contains "
                        "an embedded NUL"));
  }
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
  plugin_testing::notify_native_load(plugin_testing::LibraryKind::Operation);
#endif
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(path.c_str());
  if (!handle) {
    return Result<void*>(Status::failure(
        ErrorCode::NotFound, "operation plugin could not be loaded"));
  }
  return Result<void*>(static_cast<void*>(handle));
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* error = dlerror();
    return Result<void*>(Status::failure(
        ErrorCode::NotFound,
        error ? std::string(error) : "operation plugin could not be loaded"));
  }
  return Result<void*>(handle);
#endif
}

/**
 * @brief Looks up one required native symbol.
 * @param handle Open library handle.
 * @param name Exact C symbol name.
 * @return Symbol address or null.
 * @throws Nothing.
 * @note Symbol type conversion is performed by the caller with memcpy.
 */
void* find_symbol(void* handle, const char* name) noexcept {
#if defined(_WIN32)
  return reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

/**
 * @brief Validates a canonical sorted parameter-schema vocabulary.
 * @param schema Candidate operation parameter declarations.
 * @return Success or malformed/duplicate declaration failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Callers sort copied records before invoking this validator.
 */
Status validate_parameter_schema(
    const std::vector<OperationParameterSpec>& schema) {
  if (schema.size() > 128U) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation parameter schema exceeds 128 records");
  }
  std::string previous;
  for (const OperationParameterSpec& parameter : schema) {
    bool known_type = false;
    switch (parameter.type) {
      case OperationParameterType::Int64:
      case OperationParameterType::Float64:
      case OperationParameterType::Bool:
      case OperationParameterType::String:
        known_type = true;
        break;
    }
    const bool numeric = parameter.type == OperationParameterType::Int64 ||
                         parameter.type == OperationParameterType::Float64;
    if ((parameter.bounded && (!numeric || !std::isfinite(parameter.minimum) ||
                               !std::isfinite(parameter.maximum) ||
                               parameter.minimum > parameter.maximum)) ||
        (!parameter.bounded &&
         (parameter.minimum != 0 || parameter.maximum != 0 ||
          std::signbit(parameter.minimum) ||
          std::signbit(parameter.maximum))) ||
        (parameter.bounded && parameter.type == OperationParameterType::Int64 &&
         (std::abs(parameter.minimum) > 9007199254740991.0 ||
          std::abs(parameter.maximum) > 9007199254740991.0 ||
          std::floor(parameter.minimum) != parameter.minimum ||
          std::floor(parameter.maximum) != parameter.maximum)))
      return Status::failure(ErrorCode::InvalidArgument,
                             "invalid parameter interval");
    if (!known_type || !valid_key(parameter.key) ||
        (!previous.empty() && parameter.key <= previous)) {
      return Status::failure(
          ErrorCode::InvalidArgument,
          "operation parameter schema is malformed or conflicting");
    }
    previous = parameter.key;
  }
  return Status::success();
}

/**
 * @brief Reports whether one source value has the schema-declared exact type.
 * @param value Closed source parameter variant.
 * @param type Declared exact parameter type.
 * @return True only for the matching variant alternative.
 * @throws Nothing.
 * @note Numeric alternatives are never coerced.
 */
bool parameter_type_matches(const ParameterValue& value,
                            OperationParameterType type) noexcept {
  switch (type) {
    case OperationParameterType::Int64:
      return std::holds_alternative<std::int64_t>(value);
    case OperationParameterType::Float64:
      return std::holds_alternative<double>(value);
    case OperationParameterType::Bool:
      return std::holds_alternative<bool>(value);
    case OperationParameterType::String:
      return std::holds_alternative<std::string>(value);
  }
  return false;
}

/**
 * @brief Validates one complete Result operation trait record.
 * @param traits Candidate copied record.
 * @return Success or precise consistency failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note At least one backend is required; cacheability requires deterministic,
 * side-effect-free behavior. Result schemas define output tensor metadata;
 * actual callback backing is validated at publication.
 */
Status validate_selected_traits(const OperationTraits& traits) {
  const Status schema_status =
      validate_parameter_schema(traits.parameter_schema);
  if (traits.workspace_input_multiplier > 16 || traits.version != 27U ||
      (!traits.supports_cpu && !traits.supports_gpu) ||
      (traits.share_blocks_across_outputs &&
       (!traits.deterministic || !traits.side_effect_free)) ||
      (traits.allows_cpu_fallback &&
       (!traits.supports_gpu || !traits.supports_cpu)) ||
      (traits.cacheable &&
       (!traits.deterministic || !traits.side_effect_free)) ||
      !schema_status.ok() ||
      !input_internal::validate_port_schema(traits).ok() ||
      !input_internal::validate_operation_contract(traits).ok()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation semantic traits are inconsistent");
  }
  return Status::success();
}

Status validate_traits(const OperationTraits& traits) {
  if (traits.outputs.empty() || traits.outputs.size() > 64)
    return Status::failure(ErrorCode::InvalidArgument,
                           "invalid multi-output contract");
  std::vector<std::string> names;
  for (std::uint32_t i = 0; i < traits.outputs.size(); ++i) {
    const auto& output = traits.outputs[i];
    if (!valid_key(output.key) || output.key.size() > 128 ||
        std::find(names.begin(), names.end(), output.key) != names.end())
      return Status::failure(ErrorCode::InvalidArgument,
                             "invalid or duplicate output name");
    names.push_back(output.key);
    if (output.input_indices) {
      std::vector<std::uint32_t> seen;
      for (auto port : *output.input_indices) {
        if (port >= traits.input_schema.size() ||
            std::find(seen.begin(), seen.end(), port) != seen.end())
          return Status::failure(ErrorCode::InvalidArgument,
                                 "invalid input projection");
        seen.push_back(port);
      }
    }
    auto selected = select_operation_output(traits, i);
    auto status = validate_selected_traits(selected.value());
    if (!status.ok())
      return status;
  }
  return Status::success();
}

}  // namespace

/**
 * @brief Implements exact fail-closed operation parameter validation.
 * @copydetails validate_operation_parameters
 */
Status validate_operation_parameters(
    const OperationTraits& traits,
    const std::map<std::string, ParameterValue>& parameters) {
  const auto schema_error = [](ErrorCode code, std::string message) {
    return Status{code,
                  std::move(message),
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  const Status schema_status =
      validate_parameter_schema(traits.parameter_schema);
  if (!schema_status.ok() ||
      parameters.size() > traits.parameter_schema.size()) {
    return schema_error(
        ErrorCode::InvalidArgument,
        "operation parameters exceed or contradict the published schema");
  }
  for (const OperationParameterSpec& declaration : traits.parameter_schema) {
    const auto parameter = parameters.find(declaration.key);
    if (parameter == parameters.end()) {
      if (declaration.required) {
        return schema_error(
            ErrorCode::InvalidArgument,
            "required operation parameter is missing: " + declaration.key);
      }
      continue;
    }
    if (!parameter_type_matches(parameter->second, declaration.type)) {
      return schema_error(
          ErrorCode::InvalidArgument,
          "operation parameter has the wrong type: " + declaration.key);
    }
    if (declaration.type == OperationParameterType::String &&
        std::get<std::string>(parameter->second).size() > 8192U) {
      return schema_error(
          ErrorCode::InvalidArgument,
          "operation string parameter exceeds bounds: " + declaration.key);
    }
  }
  for (const auto& spec : traits.parameter_schema) {
    const auto found = parameters.find(spec.key);
    if (!spec.bounded || found == parameters.end())
      continue;
    bool valid = false;
    if (spec.type == OperationParameterType::Int64) {
      const auto value = std::get<std::int64_t>(found->second);
      valid = value >= static_cast<std::int64_t>(spec.minimum) &&
              value <= static_cast<std::int64_t>(spec.maximum);
    } else {
      const auto value = std::get<double>(found->second);
      valid = std::isfinite(value) && value >= spec.minimum &&
              value <= spec.maximum;
    }
    if (!valid)
      return schema_error(ErrorCode::InvalidArgument,
                          "parameter outside finite interval: " + spec.key);
  }
  for (const auto& parameter : parameters) {
    const auto declaration = std::lower_bound(
        traits.parameter_schema.begin(), traits.parameter_schema.end(),
        parameter.first,
        [](const OperationParameterSpec& candidate, const std::string& key) {
          return candidate.key < key;
        });
    if (declaration == traits.parameter_schema.end() ||
        declaration->key != parameter.first) {
      return schema_error(
          ErrorCode::InvalidArgument,
          "operation parameter key is unknown: " + parameter.first);
    }
  }
  return Status::success();
}

/**
 * @brief Private synchronized implementation of OperationRegistry.
 * @note Immutable definition handles retain callbacks and any captured DSO
 * lease. Registry snapshots copy only handles, so user callable copy/destructor
 * code and final native-library release stay outside the registry mutex.
 */
namespace {
std::uint64_t next_registry_identity() {
  static std::atomic<std::uint64_t> next{1};
  auto value = next.load();
  do {
    if (value == UINT64_MAX)
      throw std::runtime_error("registry identities exhausted");
  } while (!next.compare_exchange_weak(value, value + 1));
  return value;
}
}  // namespace
const auto valid_extension_references = [](const auto& references,
                                           const auto& definitions) {
  if (references.size() > 64)
    return false;
  for (std::size_t i = 0; i < references.size(); ++i) {
    const auto& reference = references[i];
    auto found = definitions.find(reference.key);
    if (found == definitions.end() || !(found->second->identity == reference))
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (references[j].key == reference.key)
        return false;
  }
  return true;
};
struct OperationRegistry::Impl final {
  const std::uint64_t identity = next_registry_identity();
  /** @brief Immutable owning handle for one published complete definition. */
  using DefinitionHandle = std::shared_ptr<const OperationDefinition>;

  /** @brief Serializes mutation and definition-handle lookup/copy. */
  mutable std::mutex mutex;
  /** @brief Sorted published immutable operation-definition handles. */
  std::map<std::string, DefinitionHandle> definitions;
  std::map<std::string, std::shared_ptr<const ValidationDefinition>> validators;
  std::map<std::string, std::shared_ptr<const DiagnosticDefinition>>
      diagnostics;
  /** @brief Monotonic mutation fence. */
  bool frozen = false;
};

struct PreparedOperation::Impl final {
  struct Validation {
    std::shared_ptr<const ValidationDefinition> definition;
    std::shared_ptr<const void> state;
  };
  // Program destructors run before the definition/library lease retires.
  std::shared_ptr<const OperationDefinition> definition;
  std::uint64_t registry = 0;
  OperationTraits traits;
  std::vector<OperationMetadata> inputs;
  std::map<std::string, ParameterValue> parameters;
  std::shared_ptr<const void> state;
  std::vector<ResourceIdentity> resource_ids;
  ResourceSet resources;
  std::vector<std::vector<Validation>> input_validators, output_validators,
      stage_validators;
  std::vector<std::shared_ptr<const DiagnosticDefinition>> diagnostics;
};
PreparedOperation::PreparedOperation(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
const OperationTraits& PreparedOperation::traits() const noexcept {
  return impl_->traits;
}
const void* PreparedOperation::state() const noexcept {
  return impl_->state.get();
}
const std::vector<ResourceIdentity>& PreparedOperation::resource_ids()
    const noexcept {  // NOLINT(whitespace/indent_namespace)
  return impl_->resource_ids;
}
const std::vector<std::shared_ptr<const DiagnosticDefinition>>&
PreparedOperation::diagnostic_definitions() const noexcept {
  return impl_->diagnostics;
}
bool PreparedOperation::has_validators(bool output,
                                       std::uint32_t port) const noexcept {
  const auto& ports =
      output ? impl_->output_validators : impl_->input_validators;
  return port < ports.size() && !ports[port].empty();
}
bool PreparedOperation::requires_complete_validation(
    bool output, std::uint32_t port) const noexcept {
  const auto& ports =
      output ? impl_->output_validators : impl_->input_validators;
  if (port >= ports.size())
    return false;
  return std::any_of(ports[port].begin(), ports[port].end(),
                     [](const auto& bound) {
                       return bound.definition->requires_complete &&
                              static_cast<bool>(bound.definition->validate);
                     });
}
namespace {
template <class Bindings>
Status validate_bound_contract(const Bindings& bindings,
                               const ValidationContext& context) try {
  if (!context.consume)
    return {ErrorCode::InvalidArgument, "invalid validator context"};
  for (const auto& bound : bindings) {
    const auto& definition = *bound.definition;
    if (!definition.validate)
      continue;
    if (definition.requires_complete && !context.complete)
      return {ErrorCode::TypeMismatch, "validator requires a complete result"};
    if (context.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    auto status = context.consume(1);
    if (!status.ok())
      return status;
    auto scratch = context.resources.reserve(ResourceCapacity::host(
        definition.scratch_bytes, definition.scratch_bytes));
    if (!scratch.ok())
      return scratch.status();
    status = core_internal::invoke_callback(
        [&] { return definition.validate(bound.state.get(), context); });
    if (context.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    if (!status.ok())
      return status;
  }
  return Status::success();
} catch (...) {
  return plugin_internal::current_operation_exception();
}
}  // namespace
Status PreparedOperation::validate_port(
    bool output, std::uint32_t port, const ValidationContext& context) const {
  const auto& ports =
      output ? impl_->output_validators : impl_->input_validators;
  if (port >= ports.size())
    return {ErrorCode::InvalidArgument, "invalid validator port"};
  return validate_bound_contract(ports[port], context);
}
bool PreparedOperation::has_stage_validators(
    std::uint32_t stage) const noexcept {
  return stage < impl_->stage_validators.size() &&
         !impl_->stage_validators[stage].empty();
}
bool PreparedOperation::requires_complete_stage_validation(
    std::uint32_t stage) const noexcept {
  if (stage >= impl_->stage_validators.size())
    return false;
  return std::any_of(impl_->stage_validators[stage].begin(),
                     impl_->stage_validators[stage].end(),
                     [](const auto& bound) {
                       return bound.definition->requires_complete &&
                              static_cast<bool>(bound.definition->validate);
                     });
}
Status PreparedOperation::validate_stage(
    std::uint32_t stage, const ValidationContext& context) const {
  if (stage >= impl_->stage_validators.size())
    return {ErrorCode::InvalidArgument, "invalid validator stage"};
  return validate_bound_contract(impl_->stage_validators[stage], context);
}

/**
 * @brief Implements empty mutable operation registry construction.
 * @copydetails OperationRegistry::OperationRegistry
 */
OperationRegistry::OperationRegistry() : impl_(std::make_unique<Impl>()) {}

/**
 * @brief Implements callback-record release before DSO unload.
 * @copydetails OperationRegistry::~OperationRegistry
 */
OperationRegistry::~OperationRegistry() noexcept = default;

Status OperationRegistry::register_validator(ValidationDefinition incoming) {
  auto owned = core_internal::own_callbacks(std::move(incoming));
  auto& definition = *owned;
  if (!valid_key(definition.identity.key) || !definition.identity.version ||
      !valid_key(definition.identity.implementation) || !definition.prepare)
    return {ErrorCode::InvalidArgument, "invalid validator definition"};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->frozen || impl_->validators.count(owned->identity.key))
    return {ErrorCode::InvalidArgument, "duplicate or frozen validator"};
  impl_->validators.emplace(owned->identity.key, owned);
  return Status::success();
}
Status OperationRegistry::register_diagnostic(DiagnosticDefinition incoming) {
  auto owned = core_internal::own_callbacks(std::move(incoming));
  auto& definition = *owned;
  if (!valid_key(definition.identity.key) || !definition.identity.version ||
      !valid_key(definition.identity.implementation) ||
      !definition.payload_bytes || definition.payload_bytes > 65536 ||
      !definition.validate || !definition.merge)
    return {ErrorCode::InvalidArgument, "invalid diagnostic definition"};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->frozen || impl_->diagnostics.count(owned->identity.key))
    return {ErrorCode::InvalidArgument, "duplicate or frozen diagnostic"};
  impl_->diagnostics.emplace(owned->identity.key, owned);
  return Status::success();
}

/**
 * @brief Implements atomic built-in/embedding operation registration.
 * @copydetails OperationRegistry::register_operation
 */
Status OperationRegistry::register_operation(OperationDefinition incoming) {
  auto immutable_definition = core_internal::own_callbacks(std::move(incoming));
  auto& definition = *immutable_definition;
  std::sort(
      definition.traits.parameter_schema.begin(),
      definition.traits.parameter_schema.end(),
      [](const OperationParameterSpec& left,
         const OperationParameterSpec& right) { return left.key < right.key; });
  const Status traits_status = validate_traits(definition.traits);
  if (!traits_status.ok())
    return traits_status;
  if (!valid_key(definition.key) ||
      definition.traits.requires_metadata_specialization !=
          (static_cast<bool>(definition.specialize_metadata) ||
           static_cast<bool>(definition.prepare_static)) ||
      (definition.specialize_metadata && definition.prepare_static) ||
      !definition.start_result)
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation definition is malformed");
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->frozen) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation registry is frozen");
  }
  if (impl_->definitions.count(immutable_definition->key) != 0U) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation key is already registered");
  }
  const std::string& immutable_key = immutable_definition->key;
  impl_->definitions.emplace(immutable_key, immutable_definition);
  return Status::success();
}

/**
 * @brief Implements validated transactional operation DSO loading.
 * @copydetails OperationRegistry::load_plugin
 */
Status OperationRegistry::load_plugin(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->frozen) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "operation registry is frozen");
    }
  }
  auto opened = open_library(path);
  if (!opened.ok()) {
    return opened.status();
  }
  OperationLibrary pending_library(opened.value());

  for (const auto* symbol : {"ps_operation_plugin_get_planar_api_v1",
                             "ps_operation_plugin_get_planar_api_v2",
                             "ps_operation_plugin_get_planar_api_v3"})
    if (find_symbol(pending_library.handle(), symbol))
      return Status{ErrorCode::TypeMismatch,
                    "image plugins require the Result ABI"};
  for (const auto* symbol : {"ps_result_operation_plugin_get_api_v1",
                             "ps_result_operation_plugin_get_api_v2",
                             "ps_result_operation_plugin_get_api_v3",
                             "ps_result_operation_plugin_get_api_v4"})
    if (find_symbol(pending_library.handle(), symbol))
      return Status{ErrorCode::TypeMismatch, "Result plugins require ABI 5"};
  using ResultApiFunction = const ps_result_operation_plugin_api_v5* (*)();
  ResultApiFunction result_api_function = nullptr;
  void* result_symbol = find_symbol(pending_library.handle(),
                                    "ps_result_operation_plugin_get_api_v5");
  std::memcpy(&result_api_function, &result_symbol,
              sizeof(result_api_function));
  if (result_api_function) {
    const ps_result_operation_plugin_api_v5* table = nullptr;
    try {
      core_internal::CallbackEnvironment environment;
      if (!environment.active())
        return Status{ErrorCode::OperationFailed, {}};
      table = result_api_function();
    } catch (...) {
      return Status{ErrorCode::OperationFailed, "Result API entry threw"};
    }
    if (!table ||
        reinterpret_cast<std::uintptr_t>(table) %
            alignof(ps_result_operation_plugin_api_v5) ||
        table->struct_size != sizeof(*table) ||
        table->abi_version != PS_RESULT_OPERATION_ABI_VERSION_5)
      return Status{ErrorCode::InvalidArgument,
                    "unsupported Result API version or size"};
    pending_library.attach_result_api(table);
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
    plugin_testing::invoke_before_owner_allocation(
        plugin_testing::LibraryKind::Operation);
#endif
    auto owner = std::make_shared<OperationLibrary>(std::move(pending_library));
    auto imported = plugin_internal::import_result_plugin(table, owner);
    if (!imported.ok())
      return imported.status();
    OperationRegistry candidate;
    for (auto& definition : imported.value().operations) {
      auto status = candidate.register_operation(std::move(definition));
      if (!status.ok())
        return status;
    }
    for (auto& definition : imported.value().validators) {
      auto status = candidate.register_validator(std::move(definition));
      if (!status.ok())
        return status;
    }
    for (auto& definition : imported.value().diagnostics) {
      auto status = candidate.register_diagnostic(std::move(definition));
      if (!status.ok())
        return status;
    }
    decltype(impl_->validators) validators;
    decltype(impl_->diagnostics) diagnostics;
    std::map<std::string, Impl::DefinitionHandle> replacement;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->frozen)
        return Status{ErrorCode::InvalidArgument,
                      "registry froze during Result module load"};
      replacement = impl_->definitions;
      for (const auto& definition : candidate.impl_->definitions)
        if (!replacement.emplace(definition.first, definition.second).second)
          return Status{ErrorCode::InvalidArgument,
                        "duplicate Result module operation"};
      validators = impl_->validators;
      diagnostics = impl_->diagnostics;
      for (const auto& item : candidate.impl_->validators)
        if (!validators.emplace(item).second)
          return Status{ErrorCode::InvalidArgument,
                        "duplicate module validator"};
      for (const auto& item : candidate.impl_->diagnostics)
        if (!diagnostics.emplace(item).second)
          return Status{ErrorCode::InvalidArgument,
                        "duplicate module diagnostic"};
      for (const auto& item : candidate.impl_->definitions) {
        const auto& traits = item.second->traits;
        for (const auto& stage : traits.shared_stages)
          if (!valid_extension_references(stage.validators, validators))
            return Status{ErrorCode::InvalidArgument,
                          "unresolved shared stage validators"};
        if (!valid_extension_references(traits.diagnostics, diagnostics))
          return Status{ErrorCode::InvalidArgument,
                        "unresolved module diagnostic"};
        for (const auto& port : traits.input_schema)
          if (!valid_extension_references(port.validators, validators))
            return Status{ErrorCode::InvalidArgument,
                          "unresolved module input validator"};
        for (const auto& output : traits.outputs)
          if (!valid_extension_references(output.output_schema.validators,
                                          validators))
            return Status{ErrorCode::InvalidArgument,
                          "unresolved module output validator"};
      }
      impl_->validators.swap(validators);
      impl_->diagnostics.swap(diagnostics);
      impl_->definitions.swap(replacement);
    }
    return Status::success();
  }

  return Status{ErrorCode::InvalidArgument,
                "operation plugin must expose Result ABI 5"};
}

/**
 * @brief Implements the permanent operation-set mutation fence.
 * @copydetails OperationRegistry::freeze
 */
Status OperationRegistry::freeze() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto& entry : impl_->definitions) {
    const auto& traits = entry.second->traits;
    for (const auto& stage : traits.shared_stages)
      if (!valid_extension_references(stage.validators, impl_->validators))
        return Status{ErrorCode::InvalidArgument,
                      "unresolved shared stage validators"};
    if (!valid_extension_references(traits.diagnostics, impl_->diagnostics))
      return {ErrorCode::InvalidArgument, "unresolved diagnostic contract"};
    for (const auto& port : traits.input_schema)
      if (!valid_extension_references(port.validators, impl_->validators))
        return {ErrorCode::InvalidArgument,
                "unresolved input validation contract"};
    for (const auto& output : traits.outputs)
      if (!valid_extension_references(output.output_schema.validators,
                                      impl_->validators))
        return {ErrorCode::InvalidArgument,
                "unresolved output validation contract"};
  }
  impl_->frozen = true;
  return Status::success();
}

/**
 * @brief Implements concurrent frozen-state observation.
 * @copydetails OperationRegistry::frozen
 */
bool OperationRegistry::frozen() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->frozen;
}

/**
 * @brief Implements copied semantic-trait lookup.
 * @copydetails OperationRegistry::find_traits
 */
Result<OperationTraits> OperationRegistry::find_traits(
    const std::string& key) const {
  Impl::DefinitionHandle definition;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto iterator = impl_->definitions.find(key);
    if (iterator == impl_->definitions.end()) {
      return Result<OperationTraits>(Status::failure(
          ErrorCode::NotFound, "operation key is not registered"));
    }
    definition = iterator->second;
  }
  // Published definitions are immutable. Retain the owner while trait vectors
  // and strings are copied without serializing unrelated registry readers.
  return Result<OperationTraits>(definition->traits);
}

Result<OperationTraits> OperationRegistry::resolve_traits(
    const std::string& key, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) const {
  auto prepared = prepare_operation(key, inputs, parameters);
  return prepared.ok() ? Result<OperationTraits>(prepared.value()->traits())
                       : Result<OperationTraits>(prepared.status());
}
Result<std::shared_ptr<const PreparedOperation>>
OperationRegistry::prepare_operation(
    const std::string& key, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    const ResourceSet& resources) const {
  using PreparedAnswer = Result<std::shared_ptr<const PreparedOperation>>;
  // Returned schema/state owners may run plugin deleters after callback return.
  // Keep restoration outside every local owner on both success and rejection.
  core_internal::CallbackEnvironment environment;
  if (!environment.active())
    return PreparedAnswer(
        Status{ErrorCode::OperationFailed, "cannot save floating environment"});
  Impl::DefinitionHandle definition;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->definitions.find(key);
    if (found == impl_->definitions.end())
      return PreparedAnswer(
          Status{ErrorCode::NotFound, "operation key is not registered"});
    definition = found->second;
  }
  std::shared_ptr<const void> program;
  std::vector<ResourceIdentity> resource_ids;
  const auto resolve = [&]() -> Result<OperationTraits> {
    using Answer = Result<OperationTraits>;
    try {
      auto resolved = resolve_operation_traits(definition->traits,
                                               inputs.size(), parameters);
      if (!resolved.ok())
        return resolved;
      auto traits = resolved.take_value();
      if (inputs.size() != traits.input_schema.size())
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "specializer input count mismatch",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
      for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto status = input_internal::validate_port_metadata(
            traits.input_schema[i], inputs[i]);
        if (!status.ok()) {
          if (status.detail.origin == FailureOrigin::Unspecified)
            status.detail.origin = FailureOrigin::Schema;
          return Answer(std::move(status));
        }
      }
      if (!definition->specialize_metadata && !definition->prepare_static)
        return Answer(std::move(traits));
      Result<std::vector<OperationOutputSpecialization>> specialized(
          Status{ErrorCode::Internal, "uninitialized preparation"});
      if (definition->prepare_static) {
        auto prepared = core_internal::invoke_callback(
            [&] { return definition->prepare_static(inputs, parameters); });
        if (!prepared.ok())
          return Answer(prepared.status());
        auto result = prepared.take_value();
        result.state =
            core_internal::protect_callback_state(std::move(result.state));
        if (result.shared_stages)
          traits.shared_stages = std::move(*result.shared_stages);
        if (!core_internal::can_add(result.additional_workspace_bytes,
                                    traits.workspace_bytes))
          return Answer(Status{ErrorCode::ResourceExhausted,
                               "prepared workspace bound overflows",
                               FailureReason::CapacityLimit});
        traits.workspace_bytes += result.additional_workspace_bytes;
        program = std::move(result.state);
        resource_ids = std::move(result.resources);
        specialized = Result<std::vector<OperationOutputSpecialization>>(
            std::move(result.outputs));
      } else {
        specialized = core_internal::invoke_callback([&] {
          return definition->specialize_metadata(inputs, parameters);
        });
      }
      if (!specialized.ok())
        return Answer(specialized.status());
      if (specialized.value().size() != traits.outputs.size())
        return Answer(
            Status{ErrorCode::InvalidArgument,
                   "specializer changed output count",
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
      for (std::size_t i = 0; i < traits.outputs.size(); ++i) {
        auto& output = traits.outputs[i];
        auto& specialization = specialized.value()[i];
        auto& metadata = specialization.metadata;
        if (specialization.input_indices) {
          if (traits.supports_gpu)
            return Answer(Status{ErrorCode::InvalidArgument,
                                 "specialized input projection requires CPU"});
          if (output.input_indices)
            for (auto port : *specialization.input_indices)
              if (std::find(output.input_indices->begin(),
                            output.input_indices->end(),
                            port) == output.input_indices->end())
                return Answer(Status{ErrorCode::InvalidArgument,
                                     "specializer broadened input projection"});
          output.input_indices = std::move(specialization.input_indices);
        }

        if (!metadata.result_schema ||
            (!output.output_schema.result_schema_id.empty() &&
             (output.result_schema->id != metadata.result_schema->id ||
              output.result_schema->version !=
                  metadata.result_schema->version)))
          return Answer(
              Status{ErrorCode::TypeMismatch,
                     "Result specialization must preserve its registered "
                     "kind/id/version",
                     FailureReason::None,
                     {FailureOrigin::Schema, FailureScope::Unspecified}});
        auto valid = input_internal::validate_port_metadata(
            output.output_schema, metadata);
        if (!valid.ok())
          return Answer(valid);
        output.result_schema = *metadata.result_schema;
        output.output_schema.result_schema_id =
            std::string(metadata.result_schema->id);
        output.output_schema.result_schema_version =
            metadata.result_schema->version;
      }
      traits.requires_metadata_specialization = false;
      auto expanded_validation = traits;
      expanded_validation.repeated_minimum = 0;
      expanded_validation.repeated_maximum = 0;
      expanded_validation.repeated_resolved = 0;
      expanded_validation.repeated_match = false;
      const auto valid = validate_traits(expanded_validation);
      if (!valid.ok())
        return Answer(valid);
      const auto inferred = infer_operation_outputs(traits, inputs, parameters);
      if (!inferred.ok())
        return Answer(inferred.status());
      return Answer(std::move(traits));
    } catch (const std::bad_alloc&) {
      return Answer(
          Status{ErrorCode::ResourceExhausted,
                 {},
                 FailureReason::CapacityLimit,
                 {FailureOrigin::Resource, FailureScope::Unspecified}});
    } catch (...) {
      return Answer(Status{ErrorCode::OperationFailed,
                           "metadata specialization raised an exception",
                           FailureReason::HostException,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    }
  };
  auto resolved = resolve();
  if (!resolved.ok())
    return PreparedAnswer(resolved.status());
  try {
    auto prepared = std::make_shared<PreparedOperation::Impl>();
    prepared->definition = definition;
    prepared->registry = impl_->identity;
    prepared->traits = resolved.take_value();
    prepared->inputs = inputs;
    prepared->parameters = parameters;
    prepared->state = std::move(program);
    std::sort(resource_ids.begin(), resource_ids.end());
    resource_ids.erase(std::unique(resource_ids.begin(), resource_ids.end()),
                       resource_ids.end());
    auto selected_resources = resources.select(resource_ids);
    if (!selected_resources.ok())
      return PreparedAnswer(selected_resources.status());
    prepared->resources = selected_resources.take_value();
    prepared->resource_ids = std::move(resource_ids);
    const auto bind =
        [&](const OperationPortConstraint& port, const SchemaTemplate& schema,
            bool output, std::uint32_t index,
            std::vector<PreparedOperation::Impl::Validation>* bound,
            std::uint32_t stage = UINT32_MAX) -> Status {
      if (port.validators.size() > 64)
        return {ErrorCode::InvalidArgument, "too many port validators"};
      for (const auto& identity : port.validators) {
        std::shared_ptr<const ValidationDefinition> validator;
        {
          std::lock_guard<std::mutex> lock(impl_->mutex);
          auto found = impl_->validators.find(identity.key);
          if (found == impl_->validators.end() ||
              !(found->second->identity == identity))
            return {ErrorCode::InvalidArgument,
                    "unresolved validation contract"};
          validator = found->second;
        }
        if (std::any_of(bound->begin(), bound->end(), [&](const auto& b) {
              return b.definition->identity.key == identity.key;
            }))
          return {ErrorCode::InvalidArgument,
                  "duplicate or incompatible validator"};
        auto state = core_internal::invoke_callback([&] {
          return validator->prepare(
              {schema, parameters, prepared->resources, output, index, stage});
        });
        if (!state.ok())
          return state.status();
        bound->push_back(
            {std::move(validator),
             core_internal::protect_callback_state(state.take_value())});
      }
      return Status::success();
    };
    prepared->input_validators.resize(inputs.size());
    for (std::uint32_t i = 0; i < inputs.size(); ++i) {
      auto status =
          bind(prepared->traits.input_schema[i], *inputs[i].result_schema,
               false, i, &prepared->input_validators[i]);
      if (!status.ok())
        return PreparedAnswer(status);
    }
    auto outputs =
        infer_operation_outputs(prepared->traits, inputs, parameters);
    if (!outputs.ok())
      return PreparedAnswer(outputs.status());
    prepared->output_validators.resize(outputs.value().size());
    for (std::uint32_t i = 0; i < outputs.value().size(); ++i) {
      auto status = bind(prepared->traits.outputs[i].output_schema,
                         *outputs.value()[i].result_schema, true, i,
                         &prepared->output_validators[i]);
      if (!status.ok())
        return PreparedAnswer(status);
    }
    prepared->stage_validators.resize(prepared->traits.shared_stages.size());
    for (std::uint32_t i = 0; i < prepared->traits.shared_stages.size(); ++i) {
      const auto& stage = prepared->traits.shared_stages[i];
      OperationPortConstraint contract;
      contract.validators = stage.validators;
      auto status = bind(contract, stage.result_schema, true, UINT32_MAX,
                         &prepared->stage_validators[i], i);
      if (!status.ok())
        return PreparedAnswer(status);
    }
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      for (const auto& identity : prepared->traits.diagnostics) {
        auto found = impl_->diagnostics.find(identity.key);
        if (found == impl_->diagnostics.end() ||
            !(found->second->identity == identity))
          return PreparedAnswer(Status{ErrorCode::InvalidArgument,
                                       "unresolved diagnostic contract"});
        prepared->diagnostics.push_back(found->second);
      }
    }
    return PreparedAnswer(std::shared_ptr<const PreparedOperation>(
        new PreparedOperation(std::move(prepared))));
  } catch (const std::bad_alloc&) {
    return PreparedAnswer(
        Status{ErrorCode::ResourceExhausted,
               {},
               FailureReason::CapacityLimit,
               {FailureOrigin::Resource, FailureScope::Unspecified}});
  } catch (...) {
    return PreparedAnswer(plugin_internal::current_operation_exception());
  }
}
Status OperationRegistry::validate_prepared(
    const PreparedOperation& prepared, const std::string& key,
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) const {
  const auto stale = [] {
    return Status{ErrorCode::Stale,
                  "prepared operation does not match static inputs/registry"};
  };
  const auto& stored = *prepared.impl_;
  if (stored.registry != impl_->identity ||
      inputs.size() != stored.inputs.size() ||
      parameters.size() != stored.parameters.size())
    return stale();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->definitions.find(key);
    if (found == impl_->definitions.end() || found->second != stored.definition)
      return stale();
  }
  return prepared.validate_inputs(inputs, parameters);
}

Status PreparedOperation::validate_inputs(
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) const {
  const auto stale = [] {
    return Status{ErrorCode::Stale, "prepared operation static inputs changed"};
  };
  const auto& stored = *impl_;
  if (inputs.size() != stored.inputs.size() ||
      parameters.size() != stored.parameters.size())
    return stale();
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto& a = inputs[i];
    const auto& b = stored.inputs[i];
    if (static_cast<bool>(a.result_schema) !=
            static_cast<bool>(b.result_schema) ||
        (a.result_schema && !a.result_schema->same_schema(*b.result_schema)))
      return stale();
    if (a.result_schema) {
      for (std::size_t slot = 0; slot < a.result_schema->tensors.size();
           ++slot) {
        const auto& left = a.result_schema->tensors[slot].layout;
        const auto& right = b.result_schema->tensors[slot].layout;
        if (left.order != right.order ||
            left.row_pitch_bytes != right.row_pitch_bytes)
          return stale();
      }
    }
  }
  auto b = stored.parameters.begin();
  for (const auto& a : parameters) {
    if (a.first != b->first || a.second.index() != b->second.index())
      return stale();
    if (const auto* value = std::get_if<double>(&a.second)) {
      std::uint64_t left = 0, right = 0;
      std::memcpy(&left, value, 8);
      const auto other = std::get<double>(b->second);
      std::memcpy(&right, &other, 8);
      if (left != right)
        return stale();
    } else if (a.second != b->second) {
      return stale();
    }
    ++b;
  }
  return Status::success();
}
Result<ResultProgramQuery> OperationRegistry::prepare_result_query(
    const std::string& key, const ResultProgramQuery& query) const {
  using Answer = Result<ResultProgramQuery>;
  Impl::DefinitionHandle definition;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->definitions.find(key);
    if (found == impl_->definitions.end())
      return Answer(Status{ErrorCode::NotFound, {}});
    definition = found->second;
  }
  try {
    if (!definition->start_result ||
        query.output_index >= definition->traits.outputs.size() ||
        query.semantic_key.empty() || query.semantic_key.size() > 4096 ||
        !query.page_bytes)
      return Answer(
          Status{ErrorCode::InvalidArgument, "invalid structured start"});
    if (query.backend != Backend::Cpu && query.backend != Backend::Gpu)
      return Answer(
          Status{ErrorCode::InvalidArgument, "invalid Result backend"});
    if ((query.backend == Backend::Cpu && !definition->traits.supports_cpu) ||
        (query.backend == Backend::Gpu && !definition->traits.supports_gpu))
      return Answer(Status{ErrorCode::BackendUnavailable,
                           "operation does not support the requested backend"});
    auto prepared = query.prepared;
    if (!prepared) {
      auto created = prepare_operation(key, query.inputs, query.parameters,
                                       query.resources);
      if (!created.ok())
        return Answer(created.status());
      prepared = created.take_value();
    } else {
      auto status =
          validate_prepared(*prepared, key, query.inputs, query.parameters);
      if (!status.ok())
        return Answer(status);
    }
    const auto& resolved = prepared->traits();
    std::shared_ptr<const SchemaTemplate> expected;
    if (query.shared_stage != UINT32_MAX) {
      if (query.shared_stage >= resolved.shared_stages.size())
        return Answer(
            Status{ErrorCode::InvalidArgument, "unknown shared stage"});
      expected = std::shared_ptr<const SchemaTemplate>(
          prepared, &resolved.shared_stages[query.shared_stage].result_schema);
    } else {
      auto selected =
          select_operation_output(resolved, query.output_index).take_value();
      auto metadata =
          infer_operation_output(selected, query.inputs, query.parameters);
      if (!metadata.ok())
        return Answer(metadata.status());
      expected = metadata.value().result_schema;
    }
    if (!expected || !query.output.result_schema ||
        !expected->same_schema(*query.output.result_schema))
      return Answer(Status{ErrorCode::TypeMismatch,
                           "structured start metadata mismatch"});
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto admitted = plugin_internal::admit_operation_resources(
        query.resources, prepared->resource_ids());
    if (!admitted.ok())
      return Answer(admitted.status());
    auto normalized = query;
    normalized.prepared = std::move(prepared);
    normalized.resources = admitted.take_value();

    return Answer(std::move(normalized));
  } catch (...) {
    return Answer(plugin_internal::current_operation_exception());
  }
}

Result<ResultContinuation> OperationRegistry::start_result(
    const std::string& key, const ResultProgramQuery& query,
    const BufferAllocator& allocator) const {
  using Answer = Result<ResultContinuation>;
  Impl::DefinitionHandle definition;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->definitions.find(key);
    if (found == impl_->definitions.end())
      return Answer(Status{ErrorCode::NotFound, {}});
    definition = found->second;
  }
  std::shared_ptr<plugin_internal::FailureLatch> failure;
  try {
    failure = std::make_shared<plugin_internal::FailureLatch>();
    auto checked = prepare_result_query(key, query);
    if (!checked.ok())
      return Answer(checked.status());
    auto normalized = checked.take_value();
    const auto& selected =
        normalized.prepared->traits().outputs[query.output_index];
    const auto* stage =
        query.shared_stage == UINT32_MAX
            ? nullptr
            : &normalized.prepared->traits().shared_stages[query.shared_stage];
    auto scoped = allocator.limited(
        stage ? stage->continuation_bytes : selected.continuation_bytes,
        [failure](const Status& status) { failure->record(status); });
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto started = [&]() -> Answer {
      try {
        return core_internal::invoke_callback(
            [&] { return definition->start_result(normalized, scoped); });
      } catch (...) {
        return Answer(plugin_internal::current_operation_exception());
      }
    }();
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    if (failure->load() != ErrorCode::Ok)
      return Answer(failure->snapshot());
    if (!started.ok())
      return started;
    auto state = started.take_value();
    if (!state.valid() ||
        (state.destroy_ && !scoped.owns_allocation(state.storage_)))
      return Answer(Status{ErrorCode::InvalidArgument,
                           "structured state must use host allocation"});
    state.definition_ = definition;
    state.prepared_ = normalized.prepared;
    state.output_index_ = query.output_index;
    state.shared_stage_ = query.shared_stage;
    state.output_schema_ = query.output.result_schema;
    state.semantic_key_ = query.semantic_key;
    state.resources_ = normalized.resources;
    state.payload_limit_ =
        stage
            ? std::optional<std::uint64_t>(stage->maximum_output_payload_bytes)
            : selected.maximum_output_payload_bytes;
    return Answer(std::move(state));
  } catch (...) {
    if (failure && failure->load() != ErrorCode::Ok)
      return Answer(failure->snapshot());
    return Answer(plugin_internal::current_operation_exception());
  }
}

Result<ResultContinuation> OperationRegistry::start_result_compiled(
    const std::string& key, const ResultProgramQuery& query,
    const BufferAllocator& allocator,
    std::shared_ptr<std::atomic<ErrorCode>> failure) const {
  using Answer = Result<ResultContinuation>;
  Impl::DefinitionHandle definition;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto found = impl_->definitions.find(key);
    if (found == impl_->definitions.end())
      return Answer(Status{ErrorCode::NotFound, {}});
    definition = found->second;
  }
  std::shared_ptr<plugin_internal::FailureLatch> allocation_failure;
  try {
    allocation_failure = std::make_shared<plugin_internal::FailureLatch>();
    if (!definition->start_result ||
        query.output_index >= definition->traits.outputs.size() || !failure ||
        query.semantic_key.empty() || query.semantic_key.size() > 4096 ||
        !query.page_bytes)
      return Answer(
          Status{ErrorCode::InvalidArgument, "invalid compiled result start"});
    auto prepared = query.prepared;
    if (!prepared)
      return Answer(
          Status{ErrorCode::Stale, "compiled Result lacks prepared operation"});
    auto sealed =
        validate_prepared(*prepared, key, query.inputs, query.parameters);
    if (!sealed.ok())
      return Answer(sealed);
    const auto& resolved = prepared->traits();
    if (query.output_index >= resolved.outputs.size())
      return Answer(Status{ErrorCode::Stale,
                           "compiled Result preparation protocol mismatch"});
    // Metadata and static validation were checked by Compiler. The context
    // verifies the plan belongs to this frozen registry before entering here.
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    const OperationSharedStage* shared = nullptr;
    if (query.shared_stage != UINT32_MAX) {
      if (query.shared_stage >= resolved.shared_stages.size())
        return Answer(
            Status{ErrorCode::InvalidArgument, "unknown shared stage"});
      shared = &resolved.shared_stages[query.shared_stage];
      if (!query.output.result_schema ||
          !query.output.result_schema->same_schema(shared->result_schema))
        return Answer(
            Status{ErrorCode::TypeMismatch, "shared stage schema mismatch"});
    }
    auto scoped = allocator.limited(
        shared ? shared->continuation_bytes
               : resolved.outputs[query.output_index].continuation_bytes,
        [failure, allocation_failure](const Status& status) {
          allocation_failure->record(status);
          auto expected = ErrorCode::Ok;
          failure->compare_exchange_strong(expected, status.code);
        });
    auto admitted_resources = plugin_internal::admit_operation_resources(
        query.resources, query.prepared->resource_ids());
    if (!admitted_resources.ok())
      return Answer(admitted_resources.status());
    auto normalized = query;
    normalized.resources = admitted_resources.take_value();
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto started = [&]() -> Answer {
      try {
        return core_internal::invoke_callback(
            [&] { return definition->start_result(normalized, scoped); });
      } catch (...) {
        return Answer(plugin_internal::current_operation_exception());
      }
    }();
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    if (allocation_failure->load() != ErrorCode::Ok)
      return Answer(allocation_failure->snapshot());
    if (failure->load() != ErrorCode::Ok)
      return Answer(Status{failure->load(), {}});
    if (!started.ok())
      return started;
    auto state = started.take_value();
    if (!state.valid() ||
        (state.destroy_ && !scoped.owns_allocation(state.storage_)))
      return Answer(Status{ErrorCode::InvalidArgument,
                           "structured state must use host allocation"});
    state.definition_ = definition;
    state.prepared_ = normalized.prepared;
    state.output_index_ = query.output_index;
    state.shared_stage_ = query.shared_stage;
    state.output_schema_ = query.output.result_schema;
    state.semantic_key_ = query.semantic_key;
    state.host_validates_ = true;
    state.resources_ = normalized.resources;
    const auto& output = resolved.outputs[query.output_index];
    state.payload_limit_ =
        shared
            ? std::optional<std::uint64_t>(shared->maximum_output_payload_bytes)
            : output.maximum_output_payload_bytes;
    return Answer(std::move(state));
  } catch (...) {
    if (allocation_failure && allocation_failure->load() != ErrorCode::Ok)
      return Answer(allocation_failure->snapshot());
    return Answer(plugin_internal::current_operation_exception());
  }
}

/**
 * @brief Implements sorted immutable operation-key inventory.
 * @copydetails OperationRegistry::keys
 */
std::vector<std::string> OperationRegistry::keys() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::string> result;
  result.reserve(impl_->definitions.size());
  for (const auto& entry : impl_->definitions) {
    result.push_back(entry.first);
  }
  return result;
}

}  // namespace ps
