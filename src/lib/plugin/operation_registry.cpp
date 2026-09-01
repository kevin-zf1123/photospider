#include "photospider/plugin/operation_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_plugin_api.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {
namespace {

/**
 * @brief Returns whether a public key is bounded canonical UTF-8-like text.
 * @param key Candidate operation/schema key.
 * @return True for a nonempty 1..1024-byte key without ASCII control bytes.
 * @throws Nothing.
 * @note Full Unicode normalization is intentionally outside operation identity.
 */
bool valid_key(const std::string& key) noexcept {
  if (key.empty() || key.size() > 1024U) {
    return false;
  }
  return std::none_of(key.begin(), key.end(), [](unsigned char byte) {
    return byte < 0x20U || byte == 0x7fU;
  });
}

/**
 * @brief Owns one trusted native library and its plugin record lifetime.
 *
 * @note Destruction calls the plugin destroy hook before unloading the DSO.
 */
class OperationLibrary final {
 public:
  /**
   * @brief Takes ownership of one opened library and validated API table.
   * @param handle Platform library handle.
   * @param api Validated plugin table whose records remain mapped.
   * @throws Nothing.
   * @note Handle and API must be nonnull.
   */
  OperationLibrary(void* handle, const ps_operation_plugin_api_v1* api) noexcept
      : handle_(handle), api_(api) {}

  /**
   * @brief Releases records and unloads the native library.
   * @throws Nothing.
   * @note Exceptions cannot cross a C destroy callback; a misbehaving native
   * plugin remains outside the correctness guarantee of the host process.
   */
  ~OperationLibrary() noexcept {
    if (api_ && api_->destroy) {
      try {
        api_->destroy(api_->operations, api_->operation_count);
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
  }

  OperationLibrary(const OperationLibrary&) = delete;
  OperationLibrary& operator=(const OperationLibrary&) = delete;

 private:
  /** @brief Platform native library handle. */
  void* handle_ = nullptr;
  /** @brief Mapped immutable plugin API table. */
  const ps_operation_plugin_api_v1* api_ = nullptr;
};

/**
 * @brief Opens one explicit native library path.
 * @param path Caller-provided startup path.
 * @return Native handle or a typed load failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note The function performs no path trust/signature/admission operation.
 */
Result<void*> open_library(const std::string& path) {
  if (path.empty() || path.size() > 4096U) {
    return Result<void*>(
        Status::failure(ErrorCode::InvalidArgument,
                        "operation plugin path is empty or too long"));
  }
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
 * @brief Closes a library handle whose ownership was not published.
 * @param handle Native handle, possibly null.
 * @throws Nothing.
 * @note Used only during pre-publication rollback.
 */
void close_unpublished_library(void* handle) noexcept {
#if defined(_WIN32)
  if (handle) {
    FreeLibrary(static_cast<HMODULE>(handle));
  }
#else
  if (handle) {
    dlclose(handle);
  }
#endif
}

/**
 * @brief Converts one C element value into the public C++ enum.
 * @param type Numeric C ABI element value.
 * @return ElementType or a validation failure.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note Unknown numeric values fail closed.
 */
Result<ElementType> decode_element_type(std::uint32_t type) {
  switch (type) {
    case PS_OPERATION_ELEMENT_UINT8_V1:
      return Result<ElementType>(ElementType::UInt8);
    case PS_OPERATION_ELEMENT_INT64_V1:
      return Result<ElementType>(ElementType::Int64);
    case PS_OPERATION_ELEMENT_FLOAT64_V1:
      return Result<ElementType>(ElementType::Float64);
    default:
      return Result<ElementType>(Status::failure(
          ErrorCode::TypeMismatch, "plugin output uses unknown element type"));
  }
}

/**
 * @brief Decodes one closed C ABI output-shape inference rule.
 * @param value Numeric version-one rule.
 * @return Typed rule or invalid-argument failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Unknown values fail closed.
 */
Result<OperationShapeRule> decode_shape_rule(std::uint32_t value) {
  switch (value) {
    case PS_OPERATION_SHAPE_SCALAR_V1:
      return Result<OperationShapeRule>(OperationShapeRule::Scalar);
    case PS_OPERATION_SHAPE_PRESERVE_FIRST_V1:
      return Result<OperationShapeRule>(OperationShapeRule::PreserveFirstInput);
    case PS_OPERATION_SHAPE_MATCH_INPUTS_V1:
      return Result<OperationShapeRule>(OperationShapeRule::MatchAllInputs);
    default:
      return Result<OperationShapeRule>(
          Status::failure(ErrorCode::InvalidArgument,
                          "operation plugin shape rule is unknown"));
  }
}

/**
 * @brief Decodes one closed C ABI Region propagation rule.
 * @param value Numeric version-one rule.
 * @return Typed rule or invalid-argument failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Unknown values fail closed.
 */
Result<OperationRegionRule> decode_region_rule(std::uint32_t value) {
  switch (value) {
    case PS_OPERATION_REGION_WHOLE_V1:
      return Result<OperationRegionRule>(OperationRegionRule::Whole);
    case PS_OPERATION_REGION_ELEMENTWISE_V1:
      return Result<OperationRegionRule>(OperationRegionRule::Elementwise);
    case PS_OPERATION_REGION_HALO_V1:
      return Result<OperationRegionRule>(OperationRegionRule::Halo);
    default:
      return Result<OperationRegionRule>(
          Status::failure(ErrorCode::InvalidArgument,
                          "operation plugin Region rule is unknown"));
  }
}

/**
 * @brief Validates one complete version-one semantic trait record.
 * @param traits Candidate copied record.
 * @return Success or precise consistency failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note CPU support is mandatory and cacheability requires deterministic,
 * side-effect-free behavior.
 */
Status validate_traits(const OperationTraits& traits) {
  bool known_shape = false;
  switch (traits.shape_rule) {
    case OperationShapeRule::Scalar:
    case OperationShapeRule::PreserveFirstInput:
    case OperationShapeRule::MatchAllInputs:
      known_shape = true;
      break;
  }
  bool known_region = false;
  switch (traits.region_rule) {
    case OperationRegionRule::Whole:
    case OperationRegionRule::Elementwise:
    case OperationRegionRule::Halo:
      known_region = true;
      break;
  }
  try {
    static_cast<void>(Value::element_size(traits.output_element_type));
  } catch (const std::invalid_argument&) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation output element type is unknown");
  }
  if (traits.version != 1U || !traits.supports_cpu || !known_shape ||
      !known_region || (traits.allows_cpu_fallback && !traits.supports_gpu) ||
      (traits.cacheable &&
       (!traits.deterministic || !traits.side_effect_free)) ||
      (traits.shape_rule != OperationShapeRule::Scalar &&
       traits.input_count == 0U) ||
      (traits.region_rule == OperationRegionRule::Halo &&
       traits.halo_radius == 0U) ||
      (traits.region_rule != OperationRegionRule::Halo &&
       traits.halo_radius != 0U)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation semantic traits are inconsistent");
  }
  return Status::success();
}

/**
 * @brief Validates a callback output against compiler-visible static traits.
 * @param traits Frozen operation traits.
 * @param inputs Exact invocation inputs.
 * @param output Published callback output.
 * @return Success or type/shape mismatch.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note The current executor publishes complete Values only, so output Region
 * must cover the complete inferred descriptor.
 */
Status validate_callback_output(const OperationTraits& traits,
                                const std::vector<Value>& inputs,
                                const Value& output) {
  ValueDescriptor expected{traits.output_element_type, {1U}};
  if (traits.shape_rule == OperationShapeRule::PreserveFirstInput ||
      traits.shape_rule == OperationShapeRule::MatchAllInputs) {
    if (inputs.empty()) {
      return Status::failure(ErrorCode::Internal,
                             "typed operation has no inferred input");
    }
    expected = inputs.front().descriptor();
    if (expected.element_type != traits.output_element_type) {
      return Status::failure(ErrorCode::TypeMismatch,
                             "operation input type contradicts traits");
    }
  }
  if (traits.shape_rule == OperationShapeRule::MatchAllInputs) {
    for (const Value& input : inputs) {
      if (input.descriptor().element_type != expected.element_type ||
          input.descriptor().shape != expected.shape) {
        return Status::failure(ErrorCode::TypeMismatch,
                               "operation input descriptors do not match");
      }
    }
  }
  if (!output.valid() ||
      output.descriptor().element_type != expected.element_type ||
      output.descriptor().shape != expected.shape) {
    return Status::failure(ErrorCode::TypeMismatch,
                           "operation output contradicts static descriptor");
  }
  if (output.region().rank() != expected.shape.size()) {
    return Status::failure(ErrorCode::TypeMismatch,
                           "operation output Region rank is incomplete");
  }
  for (std::size_t axis = 0U; axis < expected.shape.size(); ++axis) {
    const RegionDimension& dimension = output.region().dimensions()[axis];
    if (dimension.offset != 0U || dimension.extent != expected.shape[axis]) {
      return Status::failure(ErrorCode::TypeMismatch,
                             "operation output Region is not whole");
    }
  }
  return Status::success();
}

/**
 * @brief Builds canonical contiguous strides for one shape.
 * @param shape Nonzero rank-general extents.
 * @param element_size Physical scalar width.
 * @return Strides or overflow failure.
 * @throws std::bad_alloc If result allocation fails.
 * @note Axis order is row-major with the final axis contiguous.
 */
Result<std::vector<std::int64_t>> contiguous_strides(
    const std::vector<std::uint64_t>& shape, std::size_t element_size) {
  std::vector<std::int64_t> strides(shape.size(), 0);
  std::uint64_t stride = element_size;
  for (std::size_t reverse = shape.size(); reverse > 0U; --reverse) {
    const std::size_t axis = reverse - 1U;
    if (stride >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return Result<std::vector<std::int64_t>>(Status::failure(
          ErrorCode::ResourceExhausted, "contiguous stride exceeds int64"));
    }
    strides[axis] = static_cast<std::int64_t>(stride);
    if (shape[axis] != 0U &&
        stride > std::numeric_limits<std::uint64_t>::max() / shape[axis]) {
      return Result<std::vector<std::int64_t>>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "contiguous byte size overflows uint64"));
    }
    stride *= shape[axis];
  }
  return Result<std::vector<std::int64_t>>(std::move(strides));
}

/**
 * @brief State used by one C ABI output sink invocation.
 *
 * @note The sink accepts at most one complete output and retains no DSO
 * pointer.
 */
struct OutputSinkState final {
  /** @brief Published output when validation succeeds. */
  Result<Value> result{Status::failure(ErrorCode::OperationFailed,
                                       "operation did not publish output")};
  /** @brief Monotonic single-publication guard. */
  bool published = false;
};

/**
 * @brief C callback that copies and validates one DSO output.
 * @param context Pointer to `OutputSinkState`.
 * @param element_type Numeric C ABI element type.
 * @param shape Rank-sized shape array.
 * @param rank Rank in 1..8.
 * @param facets Bounded candidate facet array.
 * @param facet_count Number of candidate facet records.
 * @param data Complete contiguous payload bytes.
 * @param byte_size Exact payload size.
 * @return One on success and zero on failure/duplicate publication.
 * @note Every exception is fenced and translated into the sink result.
 */
int publish_plugin_output(void* context, std::uint32_t element_type,
                          const std::uint64_t* shape, std::uint32_t rank,
                          const ps_operation_facet_view_v1* facets,
                          std::uint32_t facet_count, const std::uint8_t* data,
                          std::uint64_t byte_size) noexcept {
  auto* state = static_cast<OutputSinkState*>(context);
  if (!state || state->published) {
    return 0;
  }
  state->published = true;
  try {
    if (!shape ||
        reinterpret_cast<std::uintptr_t>(shape) % alignof(std::uint64_t) !=
            0U ||
        rank == 0U || rank > 8U || facet_count > 64U ||
        (facet_count != 0U &&
         (!facets || reinterpret_cast<std::uintptr_t>(facets) %
                             alignof(ps_operation_facet_view_v1) !=
                         0U)) ||
        (byte_size != 0U && !data) ||
        byte_size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max())) {
      state->result = Result<Value>(
          Status::failure(ErrorCode::InvalidArgument,
                          "plugin output pointers/rank are invalid"));
      return 0;
    }
    auto decoded_type = decode_element_type(element_type);
    if (!decoded_type.ok()) {
      state->result = Result<Value>(decoded_type.status());
      return 0;
    }
    std::vector<std::uint64_t> owned_shape(shape, shape + rank);
    if (std::any_of(owned_shape.begin(), owned_shape.end(),
                    [](std::uint64_t extent) { return extent == 0U; })) {
      state->result = Result<Value>(Status::failure(
          ErrorCode::InvalidArgument, "plugin output shape contains zero"));
      return 0;
    }
    auto strides = contiguous_strides(
        owned_shape, Value::element_size(decoded_type.value()));
    if (!strides.ok()) {
      state->result = Result<Value>(strides.status());
      return 0;
    }
    std::uint64_t expected = Value::element_size(decoded_type.value());
    for (std::uint64_t extent : owned_shape) {
      if (expected > std::numeric_limits<std::uint64_t>::max() / extent) {
        state->result = Result<Value>(Status::failure(
            ErrorCode::ResourceExhausted, "plugin output byte size overflows"));
        return 0;
      }
      expected *= extent;
    }
    if (expected != byte_size) {
      state->result = Result<Value>(Status::failure(
          ErrorCode::TypeMismatch, "plugin output byte size mismatches shape"));
      return 0;
    }
    std::vector<ValueFacet> owned_facets;
    owned_facets.reserve(facet_count);
    for (std::uint32_t index = 0U; index < facet_count; ++index) {
      const ps_operation_facet_view_v1& facet = facets[index];
      if (facet.struct_size != sizeof(ps_operation_facet_view_v1) ||
          !facet.key || facet.key_size == 0U || facet.key_size > 256U ||
          facet.version == 0U || facet.payload_size > 64U * 1024U ||
          (facet.payload_size != 0U && !facet.payload)) {
        state->result = Result<Value>(Status::failure(
            ErrorCode::InvalidArgument, "plugin output facet is malformed"));
        return 0;
      }
      ValueFacet owned;
      owned.key.assign(facet.key, facet.key_size);
      owned.version = facet.version;
      if (facet.payload_size != 0U) {
        owned.payload.assign(facet.payload, facet.payload + facet.payload_size);
      }
      owned_facets.push_back(std::move(owned));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byte_size));
    if (byte_size != 0U) {
      std::memcpy(bytes.data(), data, bytes.size());
    }
    state->result = Value::create(
        ValueDescriptor{decoded_type.value(), owned_shape},
        Region::whole(owned_shape), StridedLayout{0U, strides.take_value()},
        std::move(bytes), std::move(owned_facets));
    return state->result.ok() ? 1 : 0;
  } catch (const std::bad_alloc&) {
    Status failure;
    failure.code = ErrorCode::ResourceExhausted;
    state->result = Result<Value>(std::move(failure));
    return 0;
  } catch (...) {
    Status failure;
    failure.code = ErrorCode::Internal;
    state->result = Result<Value>(std::move(failure));
    return 0;
  }
}

/**
 * @brief C cancellation bridge for one operation invocation.
 * @param context Pointer to an immutable CancellationToken.
 * @return Nonzero when cancellation was requested.
 * @throws Nothing.
 * @note The pointer is callback-local and never retained by the host.
 */
int plugin_cancelled(void* context) noexcept {
  const auto* token = static_cast<const CancellationToken*>(context);
  return token && token->cancelled() ? 1 : 0;
}

/**
 * @brief Reads an integer parameter with one default.
 * @param parameters Canonical parameter map.
 * @param key Parameter name.
 * @param fallback Returned when the key is absent.
 * @return Integer value or `InvalidArgument` for another parameter type.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note The map is never modified.
 */
Result<std::int64_t> integer_parameter(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& key, std::int64_t fallback) {
  const auto iterator = parameters.find(key);
  if (iterator == parameters.end()) {
    return Result<std::int64_t>(fallback);
  }
  if (const auto* value = std::get_if<std::int64_t>(&iterator->second)) {
    return Result<std::int64_t>(*value);
  }
  return Result<std::int64_t>(Status::failure(
      ErrorCode::InvalidArgument, "operation parameter is not int64"));
}

/**
 * @brief Reads a floating parameter with one default.
 * @param parameters Canonical parameter map.
 * @param key Parameter name.
 * @param fallback Returned when the key is absent.
 * @return Float64 value or `InvalidArgument` for another parameter type.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note Int64 values are exactly converted when representable by double.
 */
Result<double> floating_parameter(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& key, double fallback) {
  const auto iterator = parameters.find(key);
  if (iterator == parameters.end()) {
    return Result<double>(fallback);
  }
  if (const auto* value = std::get_if<double>(&iterator->second)) {
    return Result<double>(*value);
  }
  if (const auto* value = std::get_if<std::int64_t>(&iterator->second)) {
    return Result<double>(static_cast<double>(*value));
  }
  return Result<double>(Status::failure(ErrorCode::InvalidArgument,
                                        "operation parameter is not numeric"));
}

}  // namespace

/**
 * @brief Private synchronized implementation of OperationRegistry.
 * @note Stored callbacks retain DSO leases until every copied definition
 * retires during registry destruction.
 */
struct OperationRegistry::Impl final {
  /** @brief Serializes mutation and definition lookup/copy. */
  mutable std::mutex mutex;
  /** @brief Sorted published operation definitions. */
  std::map<std::string, OperationDefinition> definitions;
  /** @brief Monotonic mutation fence. */
  bool frozen = false;
};

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
  const Status traits_status = validate_traits(definition.traits);
  if (!valid_key(definition.key) || !definition.callback ||
      !traits_status.ok()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation definition is malformed");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->frozen) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation registry is frozen");
  }
  if (impl_->definitions.count(definition.key) != 0U) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation key is already registered");
  }
  impl_->definitions.emplace(definition.key, std::move(definition));
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
  void* handle = opened.value();

  using VersionFunction = std::uint32_t (*)();
  using ApiFunction = const ps_operation_plugin_api_v1* (*)();
  VersionFunction version = nullptr;
  ApiFunction get_api = nullptr;
  void* version_symbol =
      find_symbol(handle, "ps_operation_plugin_get_abi_version");
  void* api_symbol = find_symbol(handle, "ps_operation_plugin_get_api_v1");
  static_assert(sizeof(version) == sizeof(version_symbol),
                "function/data pointer sizes must match on supported targets");
  std::memcpy(&version, &version_symbol, sizeof(version));
  std::memcpy(&get_api, &api_symbol, sizeof(get_api));
  if (!version || !get_api) {
    close_unpublished_library(handle);
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation plugin ABI version is unsupported");
  }
  const ps_operation_plugin_api_v1* api = nullptr;
  try {
    if (version() != PS_OPERATION_ABI_VERSION_1) {
      close_unpublished_library(handle);
      return Status::failure(ErrorCode::InvalidArgument,
                             "operation plugin ABI version is unsupported");
    }
    api = get_api();
  } catch (const std::bad_alloc&) {
    close_unpublished_library(handle);
    throw;
  } catch (...) {
    close_unpublished_library(handle);
    return Status::failure(
        ErrorCode::OperationFailed,
        "operation plugin ABI entry point raised an exception");
  }
  if (!api ||
      reinterpret_cast<std::uintptr_t>(api) %
              alignof(ps_operation_plugin_api_v1) !=
          0U ||
      api->struct_size != sizeof(ps_operation_plugin_api_v1) ||
      api->operation_count == 0U || api->operation_count > 1024U ||
      !api->operations ||
      reinterpret_cast<std::uintptr_t>(api->operations) %
              alignof(ps_operation_descriptor_v1) !=
          0U ||
      !api->destroy) {
    close_unpublished_library(handle);
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation plugin API table is malformed");
  }
  // Establish destroy-before-unload ownership before any allocating
  // descriptor validation. A later return or exception now performs exact
  // rollback through OperationLibrary.
  auto library =
      std::shared_ptr<OperationLibrary>(new OperationLibrary(handle, api));

  std::vector<OperationDefinition> staged;
  staged.reserve(api->operation_count);
  for (std::uint32_t index = 0; index < api->operation_count; ++index) {
    const ps_operation_descriptor_v1& descriptor = api->operations[index];
    if (descriptor.struct_size != sizeof(ps_operation_descriptor_v1) ||
        !descriptor.key || descriptor.key_size == 0U ||
        descriptor.key_size > 1024U || !descriptor.execute ||
        descriptor.input_count > 1024U || descriptor.cacheable > 1U ||
        (descriptor.flags &
         ~(PS_OPERATION_FLAG_DETERMINISTIC |
           PS_OPERATION_FLAG_SIDE_EFFECT_FREE | PS_OPERATION_FLAG_CPU |
           PS_OPERATION_FLAG_GPU | PS_OPERATION_FLAG_CPU_FALLBACK)) != 0U) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "operation plugin descriptor is malformed");
    }
    OperationDefinition definition;
    definition.key.assign(descriptor.key, descriptor.key_size);
    if (!valid_key(definition.key) ||
        std::any_of(staged.begin(), staged.end(), [&](const auto& candidate) {
          return candidate.key == definition.key;
        })) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "operation plugin key is invalid or duplicated");
    }
    definition.traits.input_count = descriptor.input_count;
    definition.traits.deterministic =
        (descriptor.flags & PS_OPERATION_FLAG_DETERMINISTIC) != 0U;
    definition.traits.side_effect_free =
        (descriptor.flags & PS_OPERATION_FLAG_SIDE_EFFECT_FREE) != 0U;
    definition.traits.supports_cpu =
        (descriptor.flags & PS_OPERATION_FLAG_CPU) != 0U;
    definition.traits.supports_gpu =
        (descriptor.flags & PS_OPERATION_FLAG_GPU) != 0U;
    definition.traits.allows_cpu_fallback =
        (descriptor.flags & PS_OPERATION_FLAG_CPU_FALLBACK) != 0U;
    definition.traits.estimated_bytes = descriptor.estimated_bytes;
    auto output_type = decode_element_type(descriptor.output_element_type);
    auto shape_rule = decode_shape_rule(descriptor.shape_rule);
    auto region_rule = decode_region_rule(descriptor.region_rule);
    if (!output_type.ok() || !shape_rule.ok() || !region_rule.ok()) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "operation plugin backend traits are malformed");
    }
    definition.traits.output_element_type = output_type.value();
    definition.traits.shape_rule = shape_rule.value();
    definition.traits.region_rule = region_rule.value();
    definition.traits.halo_radius = descriptor.halo_radius;
    definition.traits.cacheable = descriptor.cacheable != 0U;
    const Status traits_status = validate_traits(definition.traits);
    if (!traits_status.ok()) {
      return traits_status;
    }
    staged.push_back(std::move(definition));
  }

  for (std::uint32_t index = 0; index < api->operation_count; ++index) {
    const ps_operation_descriptor_v1* descriptor = &api->operations[index];
    staged[index].callback =
        [library,
         descriptor](const OperationInvocation& invocation) -> Result<Value> {
      if (invocation.inputs.size() != descriptor->input_count) {
        return Result<Value>(
            Status::failure(ErrorCode::InvalidArgument,
                            "plugin invocation input count mismatch"));
      }
      std::vector<ps_operation_value_view_v1> views;
      std::vector<std::vector<ps_operation_facet_view_v1>> facet_views;
      views.reserve(invocation.inputs.size());
      facet_views.reserve(invocation.inputs.size());
      for (const Value& input : invocation.inputs) {
        auto strides = contiguous_strides(
            input.descriptor().shape,
            Value::element_size(input.descriptor().element_type));
        if (!strides.ok() ||
            input.region().rank() != input.descriptor().shape.size() ||
            input.layout().byte_offset > input.bytes().size() ||
            input.layout().byte_strides != strides.value()) {
          return Result<Value>(Status::failure(
              ErrorCode::TypeMismatch,
              "operation plugin requires contiguous input Values"));
        }
        for (std::size_t axis = 0U; axis < input.descriptor().shape.size();
             ++axis) {
          const RegionDimension& dimension = input.region().dimensions()[axis];
          if (dimension.offset != 0U ||
              dimension.extent != input.descriptor().shape[axis]) {
            return Result<Value>(Status::failure(
                ErrorCode::TypeMismatch,
                "operation plugin requires whole input Regions"));
          }
        }
        std::uint64_t logical_bytes =
            Value::element_size(input.descriptor().element_type);
        for (std::uint64_t extent : input.descriptor().shape) {
          if (logical_bytes >
              std::numeric_limits<std::uint64_t>::max() / extent) {
            return Result<Value>(
                Status::failure(ErrorCode::ResourceExhausted,
                                "operation plugin input byte size overflows"));
          }
          logical_bytes *= extent;
        }
        if (logical_bytes !=
            input.bytes().size() - input.layout().byte_offset) {
          return Result<Value>(
              Status::failure(ErrorCode::TypeMismatch,
                              "operation plugin input bytes are incomplete"));
        }
        auto& input_facets = facet_views.emplace_back();
        input_facets.reserve(input.facets().size());
        for (const ValueFacet& facet : input.facets()) {
          input_facets.push_back(ps_operation_facet_view_v1{
              sizeof(ps_operation_facet_view_v1), facet.key.data(),
              static_cast<std::uint32_t>(facet.key.size()), facet.version,
              facet.payload.empty() ? nullptr : facet.payload.data(),
              static_cast<std::uint32_t>(facet.payload.size())});
        }
        ps_operation_value_view_v1 view{};
        view.struct_size = sizeof(view);
        view.element_type =
            static_cast<std::uint32_t>(input.descriptor().element_type);
        view.rank = static_cast<std::uint32_t>(input.descriptor().shape.size());
        view.byte_size = logical_bytes;
        view.shape = input.descriptor().shape.data();
        view.data = input.bytes().data() + input.layout().byte_offset;
        view.facet_count = static_cast<std::uint32_t>(input_facets.size());
        view.facets = input_facets.empty() ? nullptr : input_facets.data();
        views.push_back(view);
      }
      OutputSinkState output;
      ps_operation_output_sink_v1 sink{};
      sink.struct_size = sizeof(sink);
      sink.context = &output;
      sink.publish = publish_plugin_output;
      char diagnostic[4097]{};
      const int code = descriptor->execute(
          descriptor->user_data, views.data(),
          static_cast<std::uint32_t>(views.size()),
          invocation.backend == Backend::Cpu ? 1U : 2U, plugin_cancelled,
          const_cast<CancellationToken*>(&invocation.cancellation), &sink,
          diagnostic, sizeof(diagnostic));
      diagnostic[sizeof(diagnostic) - 1U] = '\0';
      if (invocation.cancellation.cancelled()) {
        return Result<Value>(
            Status::failure(ErrorCode::Cancelled, "operation was cancelled"));
      }
      if (code != 0) {
        return Result<Value>(Status::failure(
            ErrorCode::OperationFailed,
            diagnostic[0] ? std::string(diagnostic)
                          : "operation plugin callback failed"));
      }
      return output.result;
    };
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->frozen) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation registry froze during plugin load");
  }
  for (const OperationDefinition& definition : staged) {
    if (impl_->definitions.count(definition.key) != 0U) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "operation plugin collides with registered key");
    }
  }
  auto replacement = impl_->definitions;
  for (OperationDefinition& definition : staged) {
    const auto inserted =
        replacement.emplace(definition.key, std::move(definition));
    if (!inserted.second) {
      return Status::failure(ErrorCode::Internal,
                             "operation plugin staging duplicated a key");
    }
  }
  impl_->definitions.swap(replacement);
  return Status::success();
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
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto iterator = impl_->definitions.find(key);
  if (iterator == impl_->definitions.end()) {
    return Result<OperationTraits>(Status::failure(
        ErrorCode::NotFound, "operation key is not registered"));
  }
  return Result<OperationTraits>(iterator->second.traits);
}

/**
 * @brief Implements validated, exception-fenced operation invocation.
 * @copydetails OperationRegistry::invoke
 */
Result<Value> OperationRegistry::invoke(
    const std::string& key, const OperationInvocation& invocation) const {
  OperationDefinition definition;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto iterator = impl_->definitions.find(key);
    if (iterator == impl_->definitions.end()) {
      return Result<Value>(Status::failure(ErrorCode::NotFound,
                                           "operation key is not registered"));
    }
    definition = iterator->second;
  }
  if (invocation.inputs.size() != definition.traits.input_count) {
    return Result<Value>(Status::failure(ErrorCode::InvalidArgument,
                                         "operation input count mismatch"));
  }
  if (invocation.cancellation.cancelled()) {
    return Result<Value>(
        Status::failure(ErrorCode::Cancelled, "operation was cancelled"));
  }
  if ((invocation.backend == Backend::Cpu && !definition.traits.supports_cpu) ||
      (invocation.backend == Backend::Gpu && !definition.traits.supports_gpu)) {
    return Result<Value>(Status::failure(ErrorCode::BackendUnavailable,
                                         "operation backend is unavailable"));
  }
  try {
    auto result = definition.callback(invocation);
    if (!result.ok()) {
      return result;
    }
    const Status output_status = validate_callback_output(
        definition.traits, invocation.inputs, result.value());
    return output_status.ok() ? result : Result<Value>(output_status);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return Result<Value>(
        Status::failure(ErrorCode::OperationFailed, error.what()));
  } catch (...) {
    return Result<Value>(
        Status::failure(ErrorCode::OperationFailed,
                        "operation raised a nonstandard exception"));
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
std::shared_ptr<OperationRegistry> make_default_operation_registry() {
  auto registry = std::make_shared<OperationRegistry>();

  Status status = registry->register_operation(OperationDefinition{
      "core.constant",
      OperationTraits{0U, true, true, true, true, true, sizeof(double), 1U,
                      true, ElementType::Float64, OperationShapeRule::Scalar,
                      OperationRegionRule::Whole, 0U},
      [](const OperationInvocation& invocation) -> Result<Value> {
        auto value = floating_parameter(invocation.parameters, "value", 0.0);
        if (!value.ok()) {
          return Result<Value>(value.status());
        }
        return Result<Value>(Value::from_float64(value.value()));
      }});
  if (!status.ok()) {
    throw std::logic_error(status.message);
  }

  status = registry->register_operation(OperationDefinition{
      "core.identity",
      OperationTraits{1U, true, true, true, true, true, 0U, 1U, true,
                      ElementType::Float64,
                      OperationShapeRule::PreserveFirstInput,
                      OperationRegionRule::Elementwise, 0U},
      [](const OperationInvocation& invocation) -> Result<Value> {
        return Result<Value>(invocation.inputs.front());
      }});
  if (!status.ok()) {
    throw std::logic_error(status.message);
  }

  status = registry->register_operation(OperationDefinition{
      "math.add",
      OperationTraits{2U, true, true, true, true, true, sizeof(double), 1U,
                      true, ElementType::Float64,
                      OperationShapeRule::MatchAllInputs,
                      OperationRegionRule::Elementwise, 0U},
      [](const OperationInvocation& invocation) -> Result<Value> {
        auto left = invocation.inputs[0].as_float64();
        auto right = invocation.inputs[1].as_float64();
        if (!left.ok()) {
          return Result<Value>(left.status());
        }
        if (!right.ok()) {
          return Result<Value>(right.status());
        }
        return Result<Value>(Value::from_float64(left.value() + right.value()));
      }});
  if (!status.ok()) {
    throw std::logic_error(status.message);
  }

  status = registry->register_operation(OperationDefinition{
      "core.delay",
      OperationTraits{1U, true, true, true, false, false, 0U, 1U, false,
                      ElementType::Float64,
                      OperationShapeRule::PreserveFirstInput,
                      OperationRegionRule::Whole, 0U},
      [](const OperationInvocation& invocation) -> Result<Value> {
        auto milliseconds =
            integer_parameter(invocation.parameters, "milliseconds", 0);
        if (!milliseconds.ok() || milliseconds.value() < 0 ||
            milliseconds.value() > 5000) {
          return Result<Value>(Status::failure(
              ErrorCode::InvalidArgument,
              "delay milliseconds must be an int64 in 0..5000"));
        }
        for (std::int64_t elapsed = 0; elapsed < milliseconds.value();
             ++elapsed) {
          if (invocation.cancellation.cancelled()) {
            return Result<Value>(
                Status::failure(ErrorCode::Cancelled, "delay was cancelled"));
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return Result<Value>(invocation.inputs.front());
      }});
  if (!status.ok()) {
    throw std::logic_error(status.message);
  }

  status = registry->register_operation(OperationDefinition{
      "core.gpu_fallback_probe",
      OperationTraits{1U, true, true, true, true, true, 0U, 1U, true,
                      ElementType::Float64,
                      OperationShapeRule::PreserveFirstInput,
                      OperationRegionRule::Elementwise, 0U},
      [](const OperationInvocation& invocation) -> Result<Value> {
        if (invocation.backend == Backend::Gpu) {
          return Result<Value>(Status::failure(ErrorCode::BackendUnavailable,
                                               "probe rejects GPU execution"));
        }
        return Result<Value>(invocation.inputs.front());
      }});
  if (!status.ok()) {
    throw std::logic_error(status.message);
  }

  registry->freeze();
  return registry;
}

}  // namespace ps
