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

#include "core/callback_environment.hpp"
#include "core/resource_observation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/utf8_validation.hpp"
#include "plugin/operation_exception.hpp"
#include "plugin/operation_module.hpp"
#include "plugin/port_validation.hpp"

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
        api_(std::exchange(other.api_, nullptr)) {}

  /**
   * @brief Releases records and unloads the native library.
   * @throws Nothing.
   * @note Exceptions cannot cross a C destroy callback; a misbehaving native
   * plugin remains outside the correctness guarantee of the host process.
   */
  ~OperationLibrary() noexcept {
    core_internal::CallbackEnvironment environment;
    if (api_ && api_->destroy) {
      try {
        core_internal::CallbackEnvironment environment;
        api_->destroy(api_->context);
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

  void attach_api(const ps_operation_module_v8* api) noexcept { api_ = api; }

 private:
  /** @brief Platform native library handle. */
  void* handle_ = nullptr;
  /** @brief Mapped immutable plugin API table. */
  const ps_operation_module_v8* api_ = nullptr;
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
  if (traits.version != 30U)
    return {ErrorCode::TypeMismatch, "operation traits require version 30"};
  if (!traits.supports_cpu && !traits.supports_gpu)
    return {ErrorCode::InvalidArgument, "operation has no executable backend"};
  auto status = validate_parameter_schema(traits.parameter_schema);
  if (status.ok())
    status = input_internal::validate_port_schema(traits);
  if (status.ok())
    status = input_internal::validate_operation_contract(traits);
  if (!status.ok())
    return status;
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
  std::map<std::string, std::shared_ptr<const DiagnosticDefinition>>
      diagnostics;
  /** @brief Monotonic mutation fence. */
  bool frozen = false;
};

struct PreparedOperation::Impl final {
  // Program destructors run before the definition/library lease retires.
  std::shared_ptr<const OperationDefinition> definition;
  std::uint64_t registry = 0;
  OperationTraits traits;
  std::vector<OperationMetadata> inputs;
  std::map<std::string, ParameterValue> parameters;
  std::shared_ptr<const void> state;
  std::vector<ResourceIdentity> resource_ids;
  ResourceSet resources;
  std::vector<std::shared_ptr<const DiagnosticDefinition>> diagnostics;
};
PreparedOperation::PreparedOperation(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
Result<OperationProgram> PreparedOperation::lower(
    const std::vector<TileRequest>& requests, std::uint64_t tile_extent,
    physical::Backend backend, const physical::ValidationLimits& limits) const {
  using Answer = Result<OperationProgram>;
  if (!impl_->definition->lower ||
      requests.size() != impl_->traits.outputs.size())
    return Answer({ErrorCode::InvalidArgument,
                   "operation has no fixed lowering callback"});
  if (backend != physical::Backend::Cpu && backend != physical::Backend::Gpu)
    return Answer({ErrorCode::InvalidArgument, "invalid lowering backend"});
  if ((backend == physical::Backend::Cpu && !impl_->traits.supports_cpu) ||
      (backend == physical::Backend::Gpu && !impl_->traits.supports_gpu))
    return Answer(
        {ErrorCode::BackendUnavailable, "operation placement is unavailable"});
  try {
    OperationLowering query;
    query.operation = impl_->definition->key;
    query.inputs = impl_->inputs;
    query.parameters = impl_->parameters;
    query.requests = requests;
    query.tile_extent = tile_extent;
    query.backend = backend;
    query.static_state = state();
    query.limits = limits;
    for (const auto& output : impl_->traits.outputs)
      query.outputs.push_back(
          {std::make_shared<const SchemaTemplate>(*output.result_schema)});
    auto lowered = core_internal::invoke_callback(
        [&] { return impl_->definition->lower(query); });
    if (!lowered.ok())
      return lowered;
    auto result = lowered.take_value();
    auto& p = result.program;
    if (p.blocks.size() != 1 || p.calls.size() != 1 || p.entry != 0 ||
        p.blocks[0].end != physical::EndKind::Return ||
        p.tile_extent != tile_extent ||
        p.inputs.size() != query.inputs.size() ||
        result.outputs.size() != query.outputs.size() ||
        p.blocks[0].return_values != result.outputs)
      return Answer({ErrorCode::InvalidArgument,
                     "operation changed its fixed signature"});
    auto status = physical::validate_program(p, limits);
    if (!status.ok())
      return Answer(status);
    for (std::size_t i = 0; i < query.inputs.size(); ++i)
      if (!p.schemas[p.values[p.inputs[i]].schema]->same_schema(
              *query.inputs[i].result_schema))
        return Answer(
            {ErrorCode::TypeMismatch, "lowered input schema mismatch"});
    for (std::size_t i = 0; i < query.outputs.size(); ++i) {
      const auto vi = result.outputs[i];
      if (vi >= p.values.size() ||
          p.values[vi].kind != physical::ValueKind::Produced ||
          !p.schemas[p.values[vi].schema]->same_schema(
              *query.outputs[i].result_schema))
        return Answer(
            {ErrorCode::TypeMismatch, "lowered output schema mismatch"});
      auto grids =
          TileGrid::members(*query.outputs[i].result_schema, tile_extent);
      if (!grids.ok())
        return Answer(grids.status());
      auto request = requests[i].resolve(grids.value());
      if (!request.ok())
        return Answer(request.status());
      std::map<std::uint32_t, std::vector<TileIndex>> actual;
      for (auto ti : p.values[vi].tiles)
        actual[p.tiles[ti].member].push_back(p.tiles[ti].tile_index);
      for (const auto& member : request.value().members) {
        auto tiles =
            TileSet::from_indices(ResourceBudget{}, actual[member.first]);
        if (!tiles.ok())
          return Answer(tiles.status());
        if (!tiles.value().contains(member.second))
          return Answer({ErrorCode::InvalidArgument,
                         "lowering omitted requested output coverage"});
      }
    }
    const bool effect = p.calls[0].effect_task != physical::none;
    if (effect == impl_->traits.side_effect_free)
      return Answer(
          {ErrorCode::InvalidArgument, "lowered effect contract mismatch"});
    for (const auto& kernel : p.kernels)
      if (kernel.operation != query.operation)
        return Answer({ErrorCode::InvalidArgument,
                       "lowering references a foreign kernel"});
    for (const auto& task : p.tasks) {
      const auto actual = p.placements[task.placement].backend;
      if (backend == physical::Backend::Cpu && actual != backend)
        return Answer(
            {ErrorCode::InvalidArgument, "CPU lowering emitted a GPU task"});
      if (task.kind != physical::TaskKind::Copy &&
          ((actual == physical::Backend::Cpu && !impl_->traits.supports_cpu) ||
           (actual == physical::Backend::Gpu && !impl_->traits.supports_gpu)))
        return Answer({ErrorCode::InvalidArgument,
                       "lowered task backend is unsupported by operation"});
    }
    return Answer(std::move(result));
  } catch (...) {
    return Answer(plugin_internal::current_operation_exception());
  }
}

Result<PreparedKernel> PreparedOperation::prepare_kernel(
    const physical::Program& program, physical::Id task_id,
    const std::vector<physical::Id>& outputs,
    const ps_native_gpu_service_v2* native_gpu,
    std::shared_ptr<const void> device_owner,
    const ResourceBudget& budget) const {
  using Answer = Result<PreparedKernel>;
  if (task_id >= program.tasks.size())
    return Answer({ErrorCode::InvalidArgument, "invalid prepared task"});
  const auto& task = program.tasks[task_id];
  if (task.kernel >= program.kernels.size() ||
      task.placement >= program.placements.size())
    return Answer({ErrorCode::InvalidArgument, "invalid prepared kernel"});
  const auto& kernel = program.kernels[task.kernel];
  const auto& placement = program.placements[task.placement];
  if (kernel.operation != impl_->definition->key || kernel.abi_version != 8 ||
      !impl_->definition->prepare_kernel)
    return Answer({ErrorCode::TypeMismatch, "kernel contract is not linked"});
  try {
    std::uint64_t bytes = sizeof(KernelPreparation);
    auto count = [&](std::uint64_t size, std::uint64_t width) {
      if (!core_internal::checked_multiply_add(size, width, bytes, &bytes))
        throw std::bad_alloc();
    };
    count(impl_->traits.outputs.size(), sizeof(OperationMetadata));
    count(task.reads.size(), sizeof(KernelView));
    count(task.writes.size(), sizeof(KernelView));
    auto metadata = budget.reserve(ResourceCapacity::host(bytes, bytes));
    if (!metadata.ok())
      return Answer(metadata.status());
    auto query_lease = metadata.take_value();
    KernelPreparation query(impl_->definition->key, impl_->inputs,
                            impl_->parameters, task.parameters);
    const auto reserve = [&](auto& values, std::size_t count) {
      const auto previous = values.capacity();
      values.reserve(count);
      core_internal::ResourceMetadataAccess::commit(
          query_lease, (values.capacity() - previous) * sizeof(values[0]));
    };
    reserve(query.reads, task.reads.size());
    reserve(query.writes, task.writes.size());
    reserve(query.outputs, impl_->traits.outputs.size());
    query.static_state = state();
    query.tile_extent = program.tile_extent;
    query.scratch = task.scratch;
    for (const auto& read : task.reads) {
      const auto& tile = program.tiles.at(read.tile);
      query.reads.push_back({read.port, tile.member, tile.tile_index,
                             tile.domain, tile.bytes,
                             program.layouts.at(read.layout)});
    }
    for (auto id : task.writes) {
      const auto& tile = program.tiles.at(id);
      const auto found = std::find(outputs.begin(), outputs.end(), tile.value);
      const auto port =
          found == outputs.end()
              ? physical::none
              : static_cast<physical::Id>(found - outputs.begin());
      query.writes.push_back({port, tile.member, tile.tile_index, tile.domain,
                              tile.bytes, program.layouts.at(tile.layout)});
    }
    for (const auto& output : impl_->traits.outputs)
      query.outputs.push_back({std::shared_ptr<const SchemaTemplate>(
          impl_, &*output.result_schema)});
    struct Owners {
      ResourceLease metadata;
      std::shared_ptr<const Impl> operation;
      std::shared_ptr<const void> device;
    };
    auto owner_lease = budget.reserve(
        ResourceCapacity::host(sizeof(Owners) + 64, sizeof(Owners) + 64));
    if (!owner_lease.ok())
      return Answer(owner_lease.status());
    auto owners = std::make_shared<Owners>(
        Owners{owner_lease.take_value(), impl_, std::move(device_owner)});
    core_internal::ResourceMetadataAccess::commit(owners->metadata,
                                                  sizeof(Owners));
    ResourceAllocationScope bridge_scope(budget);
    auto prepared = core_internal::invoke_callback([&] {
      return impl_->definition->prepare_kernel(kernel, placement, query,
                                               impl_->resources, native_gpu);
    });
    if (!prepared.ok())
      return Answer(prepared.status());
    auto raw = prepared.take_value();
    auto result = PreparedKernel::adopt(raw, owners, budget);
    if (!result.ok() && raw.release) {
      core_internal::CallbackEnvironment environment;
      raw.release(raw.context);
    }
    if (result.ok() && !result.value().supports(placement.backend))
      return Answer({ErrorCode::BackendUnavailable,
                     "prepared kernel lacks selected backend"});
    return result;
  } catch (...) {
    return Answer(plugin_internal::current_operation_exception());
  }
}

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
      !definition.lower)
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

  using ApiFunction = const ps_operation_module_v8* (*)();
  ApiFunction entry = nullptr;
  void* symbol =
      find_symbol(pending_library.handle(), "ps_operation_module_get_api_v8");
  static_assert(sizeof(entry) == sizeof(symbol));
  std::memcpy(&entry, &symbol, sizeof(entry));
  if (!entry)
    return {ErrorCode::TypeMismatch, "operation module requires ABI 8"};
  const ps_operation_module_v8* table = nullptr;
  try {
    core_internal::CallbackEnvironment environment;
    if (!environment.active())
      return {ErrorCode::OperationFailed, {}};
    table = entry();
  } catch (...) {
    return {ErrorCode::OperationFailed, "operation module entry threw"};
  }
  if (!table ||
      reinterpret_cast<std::uintptr_t>(table) %
          alignof(ps_operation_module_v8) ||
      table->struct_size != sizeof(*table) ||
      table->abi_version != PS_OPERATION_MODULE_ABI_VERSION_8)
    return {ErrorCode::InvalidArgument,
            "unsupported operation module version or size"};
  pending_library.attach_api(table);
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
  plugin_testing::invoke_before_owner_allocation(
      plugin_testing::LibraryKind::Operation);
#endif
  auto owner = std::make_shared<OperationLibrary>(std::move(pending_library));
  auto imported = plugin_internal::import_operation_module(table, owner);
  if (!imported.ok())
    return imported.status();
  OperationRegistry candidate;
  for (auto& definition : imported.value()) {
    auto status = candidate.register_operation(std::move(definition));
    if (!status.ok())
      return status;
  }
  std::map<std::string, Impl::DefinitionHandle> replacement;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->frozen)
      return {ErrorCode::InvalidArgument, "registry froze during module load"};
    replacement = impl_->definitions;
    for (const auto& item : candidate.impl_->definitions)
      if (!replacement.emplace(item).second)
        return {ErrorCode::InvalidArgument, "duplicate module operation"};
    impl_->definitions.swap(replacement);
  }
  return Status::success();
}

/**
 * @brief Implements the permanent operation-set mutation fence.
 * @copydetails OperationRegistry::freeze
 */
Status OperationRegistry::freeze() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto& entry : impl_->definitions) {
    const auto& traits = entry.second->traits;
    if (!valid_extension_references(traits.diagnostics, impl_->diagnostics))
      return {ErrorCode::InvalidArgument, "unresolved diagnostic contract"};
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

Result<std::shared_ptr<const PreparedOperation>>
OperationRegistry::prepare_operation(
    const std::string& key, const std::vector<OperationMetadata>& source_inputs,
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
  // Snapshot before calling the specializer: its retained schema handles and
  // subsequent lowering must refer to the same immutable metadata, even when
  // the caller retains a mutable alias to the original schema.
  std::vector<OperationMetadata> inputs;
  try {
    inputs.reserve(source_inputs.size());
    for (const auto& input : source_inputs)
      inputs.push_back(
          {input.result_schema
               ? std::make_shared<const SchemaTemplate>(*input.result_schema)
               : nullptr});
  } catch (const std::bad_alloc&) {
    return PreparedAnswer(
        Status{ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit});
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
    if (definition->validate_resources) {
      auto status = core_internal::invoke_callback([&] {
        return definition->validate_resources(prepared->state.get(),
                                              prepared->resources);
      });
      if (!status.ok())
        return PreparedAnswer(status);
    }
    auto outputs =
        infer_operation_outputs(prepared->traits, inputs, parameters);
    if (!outputs.ok())
      return PreparedAnswer(outputs.status());
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
