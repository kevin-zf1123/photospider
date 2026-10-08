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

#include "data/input_validation.hpp"
#include "execution/result_callback_scope.hpp"
#include "plugin/builtin_operations.hpp"
#include "plugin/operation_exception.hpp"
#include "plugin/operation_resources.hpp"
#include "plugin/result_payload_bound.hpp"
#include "plugin/result_plugin.hpp"
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

Result<ResultProgramPoll> empty_tensor_result(const ResultProgramPhase& phase) {
  const auto stopped = [&]() {
    if (phase.failure_latch) {
      auto status = phase.failure_latch->snapshot();
      if (!status.ok())
        return status;
    }
    if (phase.failure && phase.failure->load() != ErrorCode::Ok)
      return Status{phase.failure->load(), {}};
    return phase.query.cancellation.cancelled()
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  auto active = stopped();
  if (!active.ok())
    return Result<ResultProgramPoll>(active);
  const auto& schema = *phase.query.output.result_schema;
  if (phase.query.tensor_slot >= schema.tensors.size() ||
      phase.query.tensor_outputs->shape() !=
          schema.tensors[phase.query.tensor_slot].sample_shape())
    return Result<ResultProgramPoll>(
        Status{ErrorCode::InvalidArgument, "invalid Empty tensor query"});
  auto started = ResultBuilder::start(
      phase.resources, schema, phase.query.semantic_key, {}, {},
      phase.query.tile_height, phase.query.tile_width, phase.query.resources);
  if (!started.ok())
    return Result<ResultProgramPoll>(started.status());
  auto builder = started.take_value();
  auto basis = ResultRelation::cartesian(phase.resources, 1, {});
  if (!basis.ok())
    return Result<ResultProgramPoll>(basis.status());
  auto bound = builder.bind_descriptor_relation(basis.take_value());
  if (!bound.ok())
    return Result<ResultProgramPoll>(bound);
  auto sealed = builder.seal();
  active = stopped();
  if (!active.ok())
    return Result<ResultProgramPoll>(active);
  return sealed.ok() ? Result<ResultProgramPoll>(
                           ResultPublication{sealed.take_value(), true})
                     : Result<ResultProgramPoll>(sealed.status());
}
bool empty_tensor_query(const ResultProgramQuery& query) {
  return query.output.result_schema &&
         query.output.result_schema->fields.empty() && query.tensor_outputs &&
         query.tensor_outputs->empty() && query.prepared &&
         query.output_index < query.prepared->traits().outputs.size() &&
         (query.prepared->traits()
                  .outputs[query.output_index]
                  .observation_kind == ObservationKind::RequestRecord ||
          query.prepared->traits().joint_contract == 2);
}

/**
 * @brief Returns whether a public key is bounded canonical strict UTF-8.
 * @param key Candidate operation/schema key.
 * @return True for a nonempty 1..1024-byte well-formed key without ASCII
 * control bytes.
 * @throws Nothing.
 * @note Unicode normalization is intentionally outside operation identity.
 */
bool valid_key(const std::string& key) noexcept {
  return plugin_internal::valid_utf8_key(key);
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
    if (result_api_ && result_api_->destroy) {
      try {
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
      const ps_result_operation_plugin_api_v2* api) noexcept {
    result_api_ = api;
  }

 private:
  /** @brief Platform native library handle. */
  void* handle_ = nullptr;
  /** @brief Mapped immutable plugin API table. */
  const ps_result_operation_plugin_api_v2* result_api_ = nullptr;
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
 * @brief Validates one complete version-nine semantic trait record.
 * @param traits Candidate copied record.
 * @return Success or precise consistency failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note At least one backend is required; cacheability requires deterministic,
 * side-effect-free behavior. Fixed shapes validate only rank/extents here;
 * actual C++ callback Value layout and storage are validated at publication.
 */
Status validate_selected_traits(const OperationTraits& traits) {
  bool known_shape = false;
  switch (traits.outputs[0].shape_rule) {
    case OperationShapeRule::Scalar:
    case OperationShapeRule::PreserveFirstInput:
    case OperationShapeRule::MatchAllInputs:
    case OperationShapeRule::Fixed:
    case OperationShapeRule::Axes:
    case OperationShapeRule::Shrink:
      known_shape = true;
      break;
  }
  bool known_region = false;
  switch (traits.outputs[0].region_rule) {
    case OperationRegionRule::Whole:
    case OperationRegionRule::Elementwise:
    case OperationRegionRule::Halo:
    case OperationRegionRule::Shrink:
    case OperationRegionRule::Dependency:
      known_region = true;
      break;
  }
  try {
    static_cast<void>(
        Value::element_size(traits.outputs[0].output_element_type));
  } catch (const std::invalid_argument&) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation output element type is unknown");
  }
  const Status schema_status =
      validate_parameter_schema(traits.parameter_schema);
  const bool fixed_shape_valid =
      traits.outputs[0].shape_rule == OperationShapeRule::Fixed
          ? (!traits.outputs[0].fixed_output_shape.empty() &&
             traits.outputs[0].fixed_output_shape.size() <= 8U &&
             std::none_of(traits.outputs[0].fixed_output_shape.begin(),
                          traits.outputs[0].fixed_output_shape.end(),
                          [](std::uint64_t extent) { return extent == 0U; }))
          : traits.outputs[0].fixed_output_shape.empty();
  if (traits.workspace_input_multiplier > 16 || traits.version != 24U ||
      traits.outputs[0].dependency_version != 2 ||
      (!traits.supports_cpu && !traits.supports_gpu) || !known_shape ||
      !known_region ||
      (traits.share_blocks_across_outputs &&
       (!traits.deterministic || !traits.side_effect_free ||
        traits.outputs[0].dependency_version != 2 ||
        traits.outputs[0].observation_kind != ObservationKind::Atomic ||
        traits.outputs[0].regional_atomic ||
        traits.outputs[0].static_dependency_pieces)) ||
      (traits.allows_cpu_fallback &&
       (!traits.supports_gpu || !traits.supports_cpu)) ||
      (traits.cacheable &&
       (!traits.deterministic || !traits.side_effect_free)) ||
      ((traits.outputs[0].shape_rule ==
            OperationShapeRule::PreserveFirstInput ||
        traits.outputs[0].shape_rule == OperationShapeRule::MatchAllInputs) &&
       traits.input_count == 0U && traits.repeated_minimum == 0U) ||
      (traits.outputs[0].region_rule == OperationRegionRule::Halo &&
       traits.outputs[0].halo_radius == 0U &&
       traits.outputs[0].halo_radius_parameter.empty()) ||
      (traits.outputs[0].region_rule != OperationRegionRule::Halo &&
       traits.outputs[0].halo_radius != 0U) ||
      !schema_status.ok() || !fixed_shape_valid ||
      !input_internal::validate_port_schema(traits).ok() ||
      !input_internal::validate_operation_contract(traits).ok()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation semantic traits are inconsistent");
  }
  const bool shrink =
      traits.outputs[0].shape_rule == OperationShapeRule::Shrink;
  if (shrink !=
          (traits.outputs[0].region_rule == OperationRegionRule::Shrink) ||
      shrink != !traits.outputs[0].spatial_factor_parameter.empty() ||
      traits.outputs[0].spatial_factor != 1)
    return Status::failure(ErrorCode::InvalidArgument,
                           "inconsistent shrink contract");
  if (shrink) {
    auto p = std::find_if(
        traits.parameter_schema.begin(), traits.parameter_schema.end(),
        [&](const OperationParameterSpec& spec) {
          return spec.key == traits.outputs[0].spatial_factor_parameter;
        });
    if (traits.input_count != 1 ||
        traits.outputs[0].output_schema.kind == OperationPortKind::Value ||
        p == traits.parameter_schema.end() || !p->required || !p->bounded ||
        p->type != OperationParameterType::Int64 || p->minimum < 1 ||
        p->maximum > 16)
      return Status::failure(ErrorCode::InvalidArgument,
                             "invalid shrink parameter");
  }
  if (!traits.outputs[0].halo_radius_parameter.empty()) {
    auto parameter = std::find_if(
        traits.parameter_schema.begin(), traits.parameter_schema.end(),
        [&](const OperationParameterSpec& spec) {
          return spec.key == traits.outputs[0].halo_radius_parameter;
        });
    if (traits.outputs[0].region_rule != OperationRegionRule::Halo ||
        traits.outputs[0].halo_radius != 0 ||
        parameter == traits.parameter_schema.end() || !parameter->required ||
        !parameter->bounded ||
        parameter->type != OperationParameterType::Int64 ||
        parameter->minimum < 1 || parameter->maximum > UINT32_MAX)
      return Status::failure(ErrorCode::InvalidArgument,
                             "invalid halo parameter binding");
  }
  return Status::success();
}

Status validate_traits(const OperationTraits& traits) {
  if (traits.outputs.empty() || traits.outputs.size() > 64 ||
      (traits.outputs.size() > 1 &&
       (!traits.deterministic || !traits.side_effect_free)))
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
    if (output.dependency_version != traits.outputs[0].dependency_version)
      return Status::failure(ErrorCode::InvalidArgument,
                             "outputs must share an execution protocol");
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
struct OperationRegistry::Impl final {
  const std::uint64_t identity = next_registry_identity();
  /** @brief Immutable owning handle for one published complete definition. */
  using DefinitionHandle = std::shared_ptr<const OperationDefinition>;

  /** @brief Serializes mutation and definition-handle lookup/copy. */
  mutable std::mutex mutex;
  /** @brief Sorted published immutable operation-definition handles. */
  std::map<std::string, DefinitionHandle> definitions;
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
};
PreparedOperation::PreparedOperation(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
const OperationTraits& PreparedOperation::traits() const noexcept {
  return impl_->traits;
}
const void* PreparedOperation::state() const noexcept {
  return impl_->state.get();
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

/**
 * @brief Implements atomic built-in/embedding operation registration.
 * @copydetails OperationRegistry::register_operation
 */
Status OperationRegistry::register_operation(OperationDefinition definition) {
  std::sort(
      definition.traits.parameter_schema.begin(),
      definition.traits.parameter_schema.end(),
      [](const OperationParameterSpec& left,
         const OperationParameterSpec& right) { return left.key < right.key; });
  const Status traits_status = validate_traits(definition.traits);
  if (!traits_status.ok())
    return traits_status;
  const bool structured = definition.traits.outputs[0].dependency_version == 2;
  if (!valid_key(definition.key) ||
      definition.traits.requires_metadata_specialization !=
          (static_cast<bool>(definition.specialize_metadata) ||
           static_cast<bool>(definition.prepare_static)) ||
      (definition.specialize_metadata && definition.prepare_static) ||
      (definition.prepare_static && (!definition.traits.deterministic ||
                                     !definition.traits.side_effect_free)) ||
      !definition.start_result)
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation definition is malformed");
  if (static_cast<bool>(definition.start_result_joint) !=
          (definition.traits.joint_contract != 0) ||
      definition.traits.joint_contract > 2 ||
      (definition.start_result_joint &&
       (!structured || !definition.traits.joint_contract ||
        (definition.traits.joint_contract == 1 &&
         definition.traits.outputs.size() < 2) ||
        !definition.traits.joint_continuation_bytes ||
        std::any_of(
            definition.traits.outputs.begin(), definition.traits.outputs.end(),
            [&](const auto& output) {
              return output.output_schema.kind != OperationPortKind::Result ||
                     output.observation_kind != ObservationKind::Atomic ||
                     output.failure_delivery !=
                         (definition.traits.joint_contract == 2
                              ? FailureDelivery::PerAtomOutcome
                              : FailureDelivery::RequestFailureOnly);
            }))) ||
      (!definition.start_result_joint &&
       (definition.traits.joint_continuation_bytes ||
        definition.traits.joint_workspace_bytes)))
    return Status{ErrorCode::InvalidArgument, "invalid joint contract"};
  auto immutable_definition =
      std::make_shared<const OperationDefinition>(std::move(definition));
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
  if (find_symbol(pending_library.handle(),
                  "ps_result_operation_plugin_get_api_v1"))
    return Status{ErrorCode::TypeMismatch, "Result plugins require ABI 2"};
  using ResultApiFunction = const ps_result_operation_plugin_api_v2* (*)();
  ResultApiFunction result_api_function = nullptr;
  void* result_symbol = find_symbol(pending_library.handle(),
                                    "ps_result_operation_plugin_get_api_v2");
  std::memcpy(&result_api_function, &result_symbol,
              sizeof(result_api_function));
  if (result_api_function) {
    const ps_result_operation_plugin_api_v2* table = nullptr;
    try {
      table = result_api_function();
    } catch (...) {
      return Status{ErrorCode::OperationFailed, "Result API entry threw"};
    }
    if (!table ||
        reinterpret_cast<std::uintptr_t>(table) %
            alignof(ps_result_operation_plugin_api_v2) ||
        table->struct_size != sizeof(*table) ||
        table->abi_version != PS_RESULT_OPERATION_ABI_VERSION_2)
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
    for (auto& definition : imported.value()) {
      auto status = candidate.register_operation(std::move(definition));
      if (!status.ok())
        return status;
    }
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
      impl_->definitions.swap(replacement);
    }
    return Status::success();
  }

  return Status{ErrorCode::InvalidArgument,
                "operation plugin must expose Result ABI 2"};
}

/**
 * @brief Implements the permanent operation-set mutation fence.
 * @copydetails OperationRegistry::freeze
 */
Status OperationRegistry::freeze() noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
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
    const std::map<std::string, ParameterValue>& parameters) const {
  using PreparedAnswer = Result<std::shared_ptr<const PreparedOperation>>;
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
        if (inputs[i].atomic_trailing_axes >
                inputs[i].descriptor.shape.size() ||
            (inputs[i].atomic_trailing_axes &&
             std::any_of(inputs[i].facets.begin(), inputs[i].facets.end(),
                         [](const auto& facet) {
                           return facet.key == "photospider.image";
                         })))
          return Answer(
              Status{ErrorCode::TypeMismatch,
                     "invalid input tuple observation metadata",
                     FailureReason::None,
                     {FailureOrigin::Schema, FailureScope::Unspecified}});
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
        auto prepared = definition->prepare_static(inputs, parameters);
        if (!prepared.ok())
          return Answer(prepared.status());
        auto result = prepared.take_value();
        if (result.additional_workspace_bytes >
            UINT64_MAX - traits.workspace_bytes)
          return Answer(Status{ErrorCode::ResourceExhausted,
                               "prepared workspace bound overflows",
                               FailureReason::CapacityLimit});
        traits.workspace_bytes += result.additional_workspace_bytes;
        program = std::move(result.state);
        specialized = Result<std::vector<OperationOutputSpecialization>>(
            std::move(result.outputs));
      } else {
        specialized = definition->specialize_metadata(inputs, parameters);
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
          if (output.region_rule != OperationRegionRule::Whole ||
              output.dependency_version != 2 || traits.supports_gpu)
            return Answer(
                Status{ErrorCode::InvalidArgument,
                       "specialized input projection requires CPU Whole"});
          if (output.input_indices)
            for (auto port : *specialization.input_indices)
              if (std::find(output.input_indices->begin(),
                            output.input_indices->end(),
                            port) == output.input_indices->end())
                return Answer(Status{ErrorCode::InvalidArgument,
                                     "specializer broadened input projection"});
          output.input_indices = std::move(specialization.input_indices);
        }

        if (output.result_schema || metadata.result_schema) {
          if (!output.result_schema || !metadata.result_schema ||
              output.dependency_version != 2 ||
              output.output_schema.kind != OperationPortKind::Result ||
              (!output.output_schema.result_schema_id.empty() &&
               (output.result_schema->id != metadata.result_schema->id ||
                output.result_schema->version !=
                    metadata.result_schema->version)) ||
              metadata.descriptor.element_type != ElementType::UInt8 ||
              !metadata.descriptor.shape.empty() || !metadata.facets.empty() ||
              metadata.atomic_trailing_axes || specialization.regional_atomic ||
              specialization.preserve_output_views ||
              specialization.requires_input_views ||
              specialization.maximum_output_payload_bytes ||
              specialization.static_dependency_pieces ||
              (specialization.data_movement != DataMovementKind::None &&
               specialization.data_movement !=
                   DataMovementKind::BitwiseMapped) ||
              (specialization.data_movement_view_policy !=
                   DataMovementViewPolicy::Auto &&
               specialization.data_movement_view_policy !=
                   DataMovementViewPolicy::RequireView &&
               specialization.data_movement_view_policy !=
                   DataMovementViewPolicy::Materialize))
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
          if (specialization.data_movement != DataMovementKind::None)
            output.data_movement = specialization.data_movement;
          output.data_movement_view_policy =
              specialization.data_movement_view_policy;
          continue;
        }
        output.shape_rule = OperationShapeRule::Fixed;
        output.fixed_output_shape = std::move(metadata.descriptor.shape);
        output.output_axes.clear();
        output.output_element_type = metadata.descriptor.element_type;
        output.output_dtype_rule = OperationDtypeRule::Declared;
        output.output_dtype_input = 0;
        output.output_dtype_parameter.clear();
        output.output_semantic_rule = metadata.facets.empty()
                                          ? OperationSemanticRule::Drop
                                          : OperationSemanticRule::Establish;
        output.output_semantic_input = 0;
        output.output_semantic_parameter.clear();
        output.output_facets = std::move(metadata.facets);
        output.atomic_trailing_axes = metadata.atomic_trailing_axes;
        output.regional_atomic = specialization.regional_atomic;
        output.preserve_output_views = specialization.preserve_output_views;
        output.requires_input_views = specialization.requires_input_views;
        output.maximum_output_payload_bytes =
            specialization.maximum_output_payload_bytes;
        output.static_dependency_pieces =
            std::move(specialization.static_dependency_pieces);
        output.data_movement = specialization.data_movement;
        output.data_movement_view_policy =
            specialization.data_movement_view_policy;
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
    return PreparedAnswer(std::shared_ptr<const PreparedOperation>(
        new PreparedOperation(std::move(prepared))));
  } catch (const std::bad_alloc&) {
    return PreparedAnswer(
        Status{ErrorCode::ResourceExhausted,
               {},
               FailureReason::CapacityLimit,
               {FailureOrigin::Resource, FailureScope::Unspecified}});
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
    if (a.descriptor.element_type != b.descriptor.element_type ||
        a.descriptor.shape != b.descriptor.shape ||
        a.atomic_trailing_axes != b.atomic_trailing_axes ||
        !input_internal::same_facets(a.facets, b.facets) ||
        static_cast<bool>(a.result_schema) !=
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
Status PreparedOperation::validate_result_query(
    const ResultProgramQuery& query) const {
  auto status = validate_inputs(query.inputs, query.parameters);
  if (!status.ok())
    return status;
  auto selected = select_operation_output(traits(), query.output_index);
  if (!selected.ok())
    return selected.status();
  auto inferred =
      infer_operation_output(selected.value(), query.inputs, query.parameters);
  if (!inferred.ok())
    return inferred.status();
  const auto& expected = inferred.value();
  if (query.backend != Backend::Cpu || !expected.result_schema ||
      !query.output.result_schema ||
      !expected.result_schema->same_schema(*query.output.result_schema) ||
      expected.descriptor.shape != query.output.descriptor.shape ||
      expected.descriptor.element_type !=
          query.output.descriptor.element_type ||
      !input_internal::same_facets(expected.facets, query.output.facets))
    return Status{ErrorCode::Stale, "Result joint poll metadata changed"};
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
        definition->traits.outputs[query.output_index].dependency_version !=
            2 ||
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
      auto created = prepare_operation(key, query.inputs, query.parameters);
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
    auto selected =
        select_operation_output(resolved, query.output_index).take_value();
    auto expected =
        infer_operation_output(selected, query.inputs, query.parameters);
    if (!expected.ok())
      return Answer(expected.status());
    const auto& metadata = expected.value();
    if (input_internal::structural_image_metadata(metadata) ||
        input_internal::structural_image_metadata(query.output) ||
        std::any_of(query.inputs.begin(), query.inputs.end(),
                    input_internal::structural_image_metadata))
      return Answer(Status{ErrorCode::TypeMismatch,
                           "image structured output requires Result schema"});
    if (!metadata.result_schema || !query.output.result_schema ||
        metadata.descriptor.shape != query.output.descriptor.shape ||
        metadata.descriptor.element_type !=
            query.output.descriptor.element_type ||
        !input_internal::same_facets(metadata.facets, query.output.facets) ||
        !metadata.result_schema->same_schema(*query.output.result_schema))
      return Answer(Status{ErrorCode::TypeMismatch,
                           "structured start metadata mismatch"});
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto admitted = plugin_internal::admit_operation_resources(
        query.resources, query.inputs, query.output);
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
  try {
    auto checked = prepare_result_query(key, query);
    if (!checked.ok())
      return Answer(checked.status());
    auto normalized = checked.take_value();
    const auto& selected =
        normalized.prepared->traits().outputs[query.output_index];
    auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
    auto scoped = allocator.limited(
        selected.continuation_bytes, [failure](ErrorCode code) {
          auto expected = ErrorCode::Ok;
          failure->compare_exchange_strong(expected, code);
        });
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto started = [&]() -> Answer {
      try {
        return empty_tensor_query(normalized)
                   ? ResultContinuation::stateless<empty_tensor_result>()
                   : definition->start_result(normalized, scoped);
      } catch (...) {
        return Answer(plugin_internal::current_operation_exception());
      }
    }();
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
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
    state.resources_ = normalized.resources;
    state.payload_limit_ = selected.maximum_output_payload_bytes;
    return Answer(std::move(state));
  } catch (...) {
    return Answer(plugin_internal::current_operation_exception());
  }
}

Result<ResultJointContinuation> OperationRegistry::start_result_joint(
    const std::string& key, const ResourceVector<ResultProgramQuery>& queries,
    const ResourceBudget& resources) const {
  return start_result_joint_impl(key, queries, resources,
                                 resources.allocator());
}
Result<ResultJointContinuation> OperationRegistry::start_result_joint_compiled(
    const std::string& key, const ResourceVector<ResultProgramQuery>& queries,
    const ResourceBudget& resources, const BufferAllocator& allocator) const {
  return start_result_joint_impl(key, queries, resources, allocator);
}
Result<ResultJointContinuation> OperationRegistry::start_result_joint_impl(
    const std::string& key, const ResourceVector<ResultProgramQuery>& queries,
    const ResourceBudget& resources,
    const BufferAllocator& state_allocator) const {
  using Answer = Result<ResultJointContinuation>;
  Impl::DefinitionHandle definition;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->definitions.find(key);
    if (found == impl_->definitions.end())
      return Answer(Status{ErrorCode::NotFound, {}});
    definition = found->second;
  }
  if (!definition->start_result_joint ||
      queries.size() < (definition->traits.joint_contract == 2 ? 1U : 2U) ||
      queries.size() > 64 || queries.front().snapshot_identity.empty() ||
      queries.front().snapshot_identity.size() > 4096)
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid Result joint start"});
  try {
    ErrorCode metadata_failure = ErrorCode::Ok;
    ResourceAllocationScope scope(resources, &metadata_failure);
    ResourceVector<ResultProgramQuery> normalized;
    ResourceVector<ResultJointContinuation::Member> members;
    std::uint64_t seen = 0;
    std::shared_ptr<const PreparedOperation> prepared;
    for (const auto& query : queries) {
      if (query.backend != Backend::Cpu || query.output_index >= 64 ||
          (definition->traits.joint_contract == 1 &&
           (seen & (std::uint64_t{1} << query.output_index))) ||
          !query.output.result_schema ||
          query.snapshot_identity != queries.front().snapshot_identity)
        return Answer(
            Status{ErrorCode::InvalidArgument, "invalid Result joint member"});
      seen |= std::uint64_t{1} << query.output_index;
      auto candidate = query;
      if (candidate.cancellation.cancelled())
        candidate.cancellation = {};
      if (candidate.prepared) {
        auto valid = validate_prepared(*candidate.prepared, key, query.inputs,
                                       query.parameters);
        if (!valid.ok())
          return Answer(valid);
      }
      if (prepared)
        candidate.prepared = prepared;
      auto checked = prepare_result_query(key, candidate);
      if (!checked.ok())
        return Answer(checked.status());
      auto selected = checked.take_value();
      selected.cancellation = query.cancellation;
      if (!prepared)
        prepared = selected.prepared;
      // All members must name the same immutable static invocation.
      auto shared =
          validate_prepared(*prepared, key, query.inputs, query.parameters);
      if (!shared.ok())
        return Answer(shared);
      if (query.tensor_slot >= query.output.result_schema->tensors.size() &&
          !query.output.result_schema->tensors.empty())
        return Answer(
            Status{ErrorCode::InvalidArgument, "invalid joint tensor slot"});
      if (query.tensor_outputs &&
          (!query.tensor_outputs->valid() ||
           query.output.result_schema->tensors.empty() ||
           query.tensor_outputs->shape() !=
               query.output.result_schema->tensors[query.tensor_slot]
                   .sample_shape()))
        return Answer(
            Status{ErrorCode::InvalidArgument, "invalid joint output demand"});
      if (!selected.output.result_schema->tensors.empty()) {
        const auto& tensor =
            selected.output.result_schema->tensors[selected.tensor_slot];
        FootprintLimits limits;
        limits.consume_work = [&](std::uint64_t amount) {
          return resources.consume(ResourceWork{amount});
        };
        auto demand = selected.tensor_outputs
                          ? Result<Footprint>(*selected.tensor_outputs)
                          : Footprint::all(tensor.sample_shape(), limits);
        if (!demand.ok())
          return Answer(demand.status());
        auto closed = tensor.close_samples(demand.value(), limits);
        if (!closed.ok())
          return Answer(closed.status());
        const auto& samples = closed.value();
        const auto tuple = input_internal::tuple_channel_axis(tensor.descriptor,
                                                              tensor.facets);
        if (samples.boxes().size() != 1 || samples.empty())
          return Answer(Status{ErrorCode::InvalidArgument,
                               "joint member requires one observation"});
        const auto shape = tensor.sample_shape();
        const auto& dimensions = samples.boxes()[0].dimensions();
        for (std::size_t axis = 0; axis < shape.size(); ++axis) {
          const bool grouped =
              axis >= shape.size() - tensor.atomic_trailing_axes ||
              (tuple && axis == *tuple + tensor.batch_axes.size());
          if (!grouped && dimensions[axis].extent != 1)
            return Answer(Status{ErrorCode::InvalidArgument,
                                 "joint member requires one observation"});
        }
        selected.tensor_outputs = closed.take_value();
      }
      ResultJointContinuation::Member member;
      auto atom = result_atom_key(selected);
      if (!atom.ok())
        return Answer(atom.status());
      member.key = atom.take_value();
      if (std::any_of(members.begin(), members.end(), [&](const auto& prior) {
            return prior.key.output_index == member.key.output_index &&
                   prior.tensor_slot != query.tensor_slot;
          }))
        return Answer(Status{ErrorCode::InvalidArgument,
                             "Result joint output mixes tensor slots"});
      if (std::any_of(members.begin(), members.end(), [&](const auto& prior) {
            return prior.key == member.key;
          }))
        return Answer(Status{ErrorCode::InvalidArgument,
                             "duplicate Result joint atom key"});
      member.tensor_slot = query.tensor_slot;
      member.semantic_key.assign(query.semantic_key.data(),
                                 query.semantic_key.size());
      member.snapshot_identity.assign(query.snapshot_identity.data(),
                                      query.snapshot_identity.size());
      if (query.tensor_outputs) {
        auto captured = Footprint::from_regions(query.tensor_outputs->shape(),
                                                query.tensor_outputs->boxes());
        if (!captured.ok())
          return Answer(captured.status());
        member.requested_outputs = captured.take_value();
      }
      member.outputs = selected.tensor_outputs;
      member.prepared = selected.prepared;
      const auto& output =
          selected.prepared->traits().outputs[query.output_index];
      member.payload_limit = output.maximum_output_payload_bytes;
      member.resources = selected.resources;
      member.cancellation = query.cancellation;
      members.push_back(std::move(member));
      if (!query.cancellation.cancelled())
        normalized.push_back(std::move(selected));
    }
    if (normalized.empty())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
    auto callback_latch = std::make_shared<plugin_internal::FailureLatch>();
    auto allocator = state_allocator.limited(
        prepared->traits().joint_continuation_bytes,
        [failure, callback_latch](ErrorCode code) {
          auto expected = ErrorCode::Ok;
          failure->compare_exchange_strong(expected, code);
          callback_latch->record(Status{code, {}});
        });
    ErrorCode callback_failure = ErrorCode::Ok;
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Answer(Status{ErrorCode::OperationFailed,
                           "joint floating environment unavailable"});
    auto started = [&] {
      execution_internal::ResultCallbackScope callback(&callback_failure,
                                                       callback_latch.get());
      try {
        return definition->start_result_joint(normalized, allocator);
      } catch (...) {
        return Answer(plugin_internal::current_operation_exception());
      }
    }();
    auto sticky = callback_latch->snapshot();
    if (!sticky.ok())
      return Answer(sticky);
    if (callback_failure != ErrorCode::Ok || metadata_failure != ErrorCode::Ok)
      return Answer(Status{callback_failure != ErrorCode::Ok ? callback_failure
                                                             : metadata_failure,
                           {}});
    if (failure->load() != ErrorCode::Ok)
      return Answer(Status{failure->load(), {}});
    if (!started.ok())
      return started;
    auto state = started.take_value();
    if (!state.valid() || !state.destroy_ ||
        !allocator.owns_allocation(state.storage_))
      return Answer(Status{ErrorCode::InvalidArgument,
                           "Result joint state must use host allocation"});
    state.definition_ = definition;
    state.root_ = resources;
    state.members_ = std::move(members);
    state.workspace_bytes_ = prepared->traits().joint_workspace_bytes;
    state.contract_ = prepared->traits().joint_contract;
    return Answer(std::move(state));
  } catch (...) {
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
  try {
    if (!definition->start_result ||
        query.output_index >= definition->traits.outputs.size() ||
        definition->traits.outputs[query.output_index].dependency_version !=
            2 ||
        !failure || query.semantic_key.empty() ||
        query.semantic_key.size() > 4096 || !query.page_bytes)
      return Answer(
          Status{ErrorCode::InvalidArgument, "invalid compiled result start"});
    if (input_internal::structural_image_metadata(query.output) ||
        std::any_of(query.inputs.begin(), query.inputs.end(),
                    input_internal::structural_image_metadata))
      return Answer(Status{ErrorCode::TypeMismatch,
                           "image structured output requires Result schema"});
    auto prepared = query.prepared;
    if (!prepared)
      return Answer(
          Status{ErrorCode::Stale, "compiled Result lacks prepared operation"});
    auto sealed =
        validate_prepared(*prepared, key, query.inputs, query.parameters);
    if (!sealed.ok())
      return Answer(sealed);
    const auto& resolved = prepared->traits();
    if (query.output_index >= resolved.outputs.size() ||
        resolved.outputs[query.output_index].dependency_version != 2)
      return Answer(Status{ErrorCode::Stale,
                           "compiled Result preparation protocol mismatch"});
    // Metadata and static validation were checked by Compiler. The context
    // verifies the plan belongs to this frozen registry before entering here.
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto scoped = allocator.limited(
        resolved.outputs[query.output_index].continuation_bytes,
        [failure](ErrorCode code) {
          auto expected = ErrorCode::Ok;
          failure->compare_exchange_strong(expected, code);
        });
    auto admitted_resources = plugin_internal::admit_operation_resources(
        query.resources, query.inputs, query.output);
    if (!admitted_resources.ok())
      return Answer(admitted_resources.status());
    auto normalized = query;
    normalized.resources = admitted_resources.take_value();
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto started = [&]() -> Answer {
      try {
        return empty_tensor_query(normalized)
                   ? ResultContinuation::stateless<empty_tensor_result>()
                   : definition->start_result(normalized, scoped);
      } catch (...) {
        return Answer(plugin_internal::current_operation_exception());
      }
    }();
    if (query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
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
    state.resources_ = normalized.resources;
    const auto& output = resolved.outputs[query.output_index];
    state.payload_limit_ = output.maximum_output_payload_bytes;
    return Answer(std::move(state));
  } catch (...) {
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

/**
 * @brief Implements the maintained frozen built-in operation set.
 * @copydetails make_default_operation_registry
 */
std::shared_ptr<OperationRegistry> make_default_operation_registry(
    bool freeze) {
  auto registry = std::make_shared<OperationRegistry>();
  const auto status =
      plugin_internal::register_builtin_operations(registry.get());
  if (!status.ok())
    throw std::logic_error(status.message);
  if (freeze)
    registry->freeze();
  return registry;
}

}  // namespace ps
