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
#include "plugin/dense_layout_validation.hpp"
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
        api_(std::exchange(other.api_, nullptr)) {}

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

  /**
   * @brief Attaches an API table whose exact structure prefix is readable.
   * @param api Mapped API table, possibly carrying malformed later fields.
   * @throws Nothing.
   * @note A nonnull destroy callback becomes part of rollback immediately.
   */
  void attach_api(const ps_operation_plugin_api_v2* api) noexcept {
    api_ = api;
  }

 private:
  /** @brief Platform native library handle. */
  void* handle_ = nullptr;
  /** @brief Mapped immutable plugin API table. */
  const ps_operation_plugin_api_v2* api_ = nullptr;
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
 * @brief Converts one C element value into the public C++ enum.
 * @param type Numeric C ABI element value.
 * @return ElementType or a validation failure.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note Unknown numeric values fail closed.
 */
Result<ElementType> decode_element_type(std::uint32_t type) {
  switch (type) {
    case PS_OPERATION_ELEMENT_UINT8_V2:
      return Result<ElementType>(ElementType::UInt8);
    case PS_OPERATION_ELEMENT_INT64_V2:
      return Result<ElementType>(ElementType::Int64);
    case PS_OPERATION_ELEMENT_FLOAT64_V2:
      return Result<ElementType>(ElementType::Float64);
    default:
      return Result<ElementType>(Status::failure(
          ErrorCode::TypeMismatch, "plugin output uses unknown element type"));
  }
}

/**
 * @brief Decodes one closed C ABI output-shape inference rule.
 * @param value Numeric version-two rule.
 * @return Typed rule or invalid-argument failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Unknown values fail closed.
 */
Result<OperationShapeRule> decode_shape_rule(std::uint32_t value) {
  switch (value) {
    case PS_OPERATION_SHAPE_SCALAR_V2:
      return Result<OperationShapeRule>(OperationShapeRule::Scalar);
    case PS_OPERATION_SHAPE_PRESERVE_FIRST_V2:
      return Result<OperationShapeRule>(OperationShapeRule::PreserveFirstInput);
    case PS_OPERATION_SHAPE_MATCH_INPUTS_V2:
      return Result<OperationShapeRule>(OperationShapeRule::MatchAllInputs);
    case PS_OPERATION_SHAPE_FIXED_V2:
      return Result<OperationShapeRule>(OperationShapeRule::Fixed);
    default:
      return Result<OperationShapeRule>(
          Status::failure(ErrorCode::InvalidArgument,
                          "operation plugin shape rule is unknown"));
  }
}

/**
 * @brief Decodes one closed C ABI Region propagation rule.
 * @param value Numeric version-two rule.
 * @return Typed rule or invalid-argument failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Unknown values fail closed.
 */
Result<OperationRegionRule> decode_region_rule(std::uint32_t value) {
  switch (value) {
    case PS_OPERATION_REGION_WHOLE_V2:
      return Result<OperationRegionRule>(OperationRegionRule::Whole);
    case PS_OPERATION_REGION_ELEMENTWISE_V2:
      return Result<OperationRegionRule>(OperationRegionRule::Elementwise);
    case PS_OPERATION_REGION_HALO_V2:
      return Result<OperationRegionRule>(OperationRegionRule::Halo);
    default:
      return Result<OperationRegionRule>(
          Status::failure(ErrorCode::InvalidArgument,
                          "operation plugin Region rule is unknown"));
  }
}

/**
 * @brief Decodes one closed C ABI source-parameter type.
 * @param value Numeric operation ABI v2 parameter type.
 * @return Typed parameter kind or invalid-argument failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Unknown numeric values fail before registry publication.
 */
Result<OperationParameterType> decode_parameter_type(std::uint32_t value) {
  switch (value) {
    case PS_OPERATION_PARAMETER_INT64_V2:
      return Result<OperationParameterType>(OperationParameterType::Int64);
    case PS_OPERATION_PARAMETER_FLOAT64_V2:
      return Result<OperationParameterType>(OperationParameterType::Float64);
    case PS_OPERATION_PARAMETER_BOOL_V2:
      return Result<OperationParameterType>(OperationParameterType::Bool);
    case PS_OPERATION_PARAMETER_STRING_V2:
      return Result<OperationParameterType>(OperationParameterType::String);
    default:
      return Result<OperationParameterType>(
          Status::failure(ErrorCode::InvalidArgument,
                          "operation plugin parameter type is unknown"));
  }
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
 * @brief Validates one complete version-two semantic trait record.
 * @param traits Candidate copied record.
 * @return Success or precise consistency failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note CPU support is mandatory and cacheability requires deterministic,
 * side-effect-free behavior. Fixed shapes validate only rank/extents here;
 * actual C++ callback Value layout and storage are validated at publication.
 */
Status validate_traits(const OperationTraits& traits) {
  bool known_shape = false;
  switch (traits.shape_rule) {
    case OperationShapeRule::Scalar:
    case OperationShapeRule::PreserveFirstInput:
    case OperationShapeRule::MatchAllInputs:
    case OperationShapeRule::Fixed:
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
  const Status schema_status =
      validate_parameter_schema(traits.parameter_schema);
  const bool fixed_shape_valid =
      traits.shape_rule == OperationShapeRule::Fixed
          ? (!traits.fixed_output_shape.empty() &&
             traits.fixed_output_shape.size() <= 8U &&
             std::none_of(traits.fixed_output_shape.begin(),
                          traits.fixed_output_shape.end(),
                          [](std::uint64_t extent) { return extent == 0U; }))
          : traits.fixed_output_shape.empty();
  if (traits.version != 2U || !traits.supports_cpu || !known_shape ||
      !known_region || (traits.allows_cpu_fallback && !traits.supports_gpu) ||
      (traits.cacheable &&
       (!traits.deterministic || !traits.side_effect_free)) ||
      ((traits.shape_rule == OperationShapeRule::PreserveFirstInput ||
        traits.shape_rule == OperationShapeRule::MatchAllInputs) &&
       traits.input_count == 0U) ||
      (traits.region_rule == OperationRegionRule::Halo &&
       traits.halo_radius == 0U) ||
      (traits.region_rule != OperationRegionRule::Halo &&
       traits.halo_radius != 0U) ||
      !schema_status.ok() || !fixed_shape_valid) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation semantic traits are inconsistent");
  }
  return Status::success();
}

/**
 * @brief Precomputes one callback's statically expected output descriptor.
 * @param traits Frozen operation traits.
 * @param inputs Exact invocation inputs.
 * @return Expected descriptor, `InvalidArgument` for a default input Value,
 * `TypeMismatch` for Preserve/Match incompatibility, or `Internal` for an
 * impossible published trait invariant.
 * @throws std::bad_alloc If descriptor or diagnostic storage cannot allocate.
 * @note The result is computed before callback entry and reused for output
 * validation. Preserve/Match registration already forbids zero inputs; this
 * helper still fails closed if that private invariant is violated.
 */
Result<ValueDescriptor> expected_callback_output_descriptor(
    const OperationTraits& traits, const std::vector<Value>& inputs) {
  switch (traits.shape_rule) {
    case OperationShapeRule::Scalar:
      return Result<ValueDescriptor>(
          ValueDescriptor{traits.output_element_type, {1U}});
    case OperationShapeRule::Fixed:
      return Result<ValueDescriptor>(ValueDescriptor{
          traits.output_element_type, traits.fixed_output_shape});
    case OperationShapeRule::PreserveFirstInput:
    case OperationShapeRule::MatchAllInputs:
      break;
  }
  if (traits.shape_rule != OperationShapeRule::PreserveFirstInput &&
      traits.shape_rule != OperationShapeRule::MatchAllInputs) {
    return Result<ValueDescriptor>(Status::failure(
        ErrorCode::Internal, "registered operation shape rule is unknown"));
  }
  if (inputs.empty()) {
    return Result<ValueDescriptor>(Status::failure(
        ErrorCode::Internal, "typed operation has no inferred input"));
  }
  if (!inputs.front().valid()) {
    return Result<ValueDescriptor>(Status::failure(
        ErrorCode::InvalidArgument, "operation input Value is invalid"));
  }
  const ValueDescriptor expected = inputs.front().descriptor();
  if (expected.element_type != traits.output_element_type) {
    return Result<ValueDescriptor>(Status::failure(
        ErrorCode::TypeMismatch, "operation input type contradicts traits"));
  }
  if (traits.shape_rule == OperationShapeRule::MatchAllInputs) {
    for (const Value& input : inputs) {
      if (!input.valid()) {
        return Result<ValueDescriptor>(Status::failure(
            ErrorCode::InvalidArgument, "operation input Value is invalid"));
      }
      const ValueDescriptor& descriptor = input.descriptor();
      if (descriptor.element_type != expected.element_type ||
          descriptor.shape != expected.shape) {
        return Result<ValueDescriptor>(
            Status::failure(ErrorCode::TypeMismatch,
                            "operation input descriptors do not match"));
      }
    }
  }
  return Result<ValueDescriptor>(expected);
}

/**
 * @brief Validates a callback output against a precomputed descriptor.
 * @param expected Descriptor validated before callback entry.
 * @param output Published callback output, possibly default-invalid.
 * @return Success or type/shape/whole-Region mismatch.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note The current executor publishes complete Values only, so output Region
 * must cover the complete descriptor. A default output remains a safe
 * `TypeMismatch` rather than an accessor exception.
 */
Status validate_callback_output(const ValueDescriptor& expected,
                                const Value& output) {
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
 * @brief Canonical dense layout derived from one logical shape.
 * @note The byte count and signed strides are checked before publication.
 */
struct DenseLayout final {
  /** @brief Row-major signed byte stride for every axis. */
  std::vector<std::int64_t> byte_strides;
  /** @brief Complete dense logical byte count. */
  std::uint64_t byte_size = 0U;
};

/**
 * @brief Builds canonical contiguous strides and byte count for one shape.
 * @param shape Nonzero rank-general extents.
 * @param element_size Physical scalar width.
 * @return Dense layout or signed-stride, uint64 product, signed final-offset,
 * or host allocation-size representability failure.
 * @throws std::bad_alloc If result allocation fails.
 * @note Axis order is row-major with the final axis contiguous. Every product
 * uses division-guarded uint64 arithmetic; the complete byte count must be
 * positive, have `byte_size - 1 <= INT64_MAX`, and fit host `size_t`.
 */
Result<DenseLayout> dense_layout(const std::vector<std::uint64_t>& shape,
                                 std::size_t element_size) {
  DenseLayout layout;
  layout.byte_strides.resize(shape.size(), 0);
  std::uint64_t stride = element_size;
  for (std::size_t reverse = shape.size(); reverse > 0U; --reverse) {
    const std::size_t axis = reverse - 1U;
    if (stride >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return Result<DenseLayout>(Status::failure(
          ErrorCode::ResourceExhausted, "contiguous stride exceeds int64"));
    }
    layout.byte_strides[axis] = static_cast<std::int64_t>(stride);
    if (shape[axis] != 0U &&
        stride > std::numeric_limits<std::uint64_t>::max() / shape[axis]) {
      return Result<DenseLayout>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "contiguous byte size overflows uint64"));
    }
    stride *= shape[axis];
  }
  if (!plugin_internal::dense_byte_size_representable<std::size_t>(stride)) {
    return Result<DenseLayout>(
        Status::failure(ErrorCode::ResourceExhausted,
                        "contiguous byte range is not host-addressable"));
  }
  layout.byte_size = stride;
  return Result<DenseLayout>(std::move(layout));
}

/**
 * @brief State used by one C ABI output sink invocation.
 *
 * @note The sink records the first publication attempt before validation,
 * accepts at most one complete output, records every later call as a sticky
 * invocation-local contract violation, and retains no DSO pointer.
 */
struct OutputSinkState final {
  /** @brief Published output when validation succeeds. */
  Result<Value> result{Status::failure(ErrorCode::OperationFailed,
                                       "operation did not publish output")};
  /** @brief True after the first sink call, whether accepted or rejected. */
  bool published = false;
  /** @brief True after any second or later call with this invocation state. */
  bool duplicate_publish_attempted = false;
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
 * @return One on first-call success and zero for null state, validation/
 * allocation failure, or every duplicate call.
 * @throws Nothing; this C ABI sink is `noexcept`.
 * @note Null state has no side effect. A duplicate only sets the sticky flag
 * and never replaces the first `Result`; every allocating/throwing operation
 * remains inside the first-call exception fence.
 */
int publish_plugin_output(void* context, std::uint32_t element_type,
                          const std::uint64_t* shape, std::uint32_t rank,
                          const ps_operation_facet_view_v2* facets,
                          std::uint32_t facet_count, const std::uint8_t* data,
                          std::uint64_t byte_size) noexcept {
  auto* state = static_cast<OutputSinkState*>(context);
  if (!state) {
    return 0;
  }
  if (state->published) {
    state->duplicate_publish_attempted = true;
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
                             alignof(ps_operation_facet_view_v2) !=
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
    auto layout =
        dense_layout(owned_shape, Value::element_size(decoded_type.value()));
    if (!layout.ok()) {
      state->result = Result<Value>(layout.status());
      return 0;
    }
    DenseLayout owned_layout = layout.take_value();
    if (owned_layout.byte_size != byte_size) {
      state->result = Result<Value>(Status::failure(
          ErrorCode::TypeMismatch, "plugin output byte size mismatches shape"));
      return 0;
    }
    std::vector<ValueFacet> owned_facets;
    owned_facets.reserve(facet_count);
    for (std::uint32_t index = 0U; index < facet_count; ++index) {
      const ps_operation_facet_view_v2& facet = facets[index];
      if (facet.struct_size != sizeof(ps_operation_facet_view_v2) ||
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
    state->result =
        Value::create(ValueDescriptor{decoded_type.value(), owned_shape},
                      Region::whole(owned_shape),
                      StridedLayout{0U, std::move(owned_layout.byte_strides)},
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
 * @brief Reads one required integer parameter after schema validation.
 * @param parameters Canonical parameter map.
 * @param key Parameter name.
 * @return Integer value or `InvalidArgument` when absent/wrong-type.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note The map is never modified and no default value is synthesized.
 */
Result<std::int64_t> integer_parameter(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& key) {
  const auto iterator = parameters.find(key);
  if (iterator == parameters.end()) {
    return Result<std::int64_t>(Status::failure(
        ErrorCode::InvalidArgument, "required operation parameter is missing"));
  }
  if (const auto* value = std::get_if<std::int64_t>(&iterator->second)) {
    return Result<std::int64_t>(*value);
  }
  return Result<std::int64_t>(Status::failure(
      ErrorCode::InvalidArgument, "operation parameter is not int64"));
}

/**
 * @brief Reads one required Float64 parameter after schema validation.
 * @param parameters Canonical parameter map.
 * @param key Parameter name.
 * @return Float64 value or `InvalidArgument` when absent/wrong-type.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note Numeric alternatives are not coerced and no default is synthesized.
 */
Result<double> floating_parameter(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& key) {
  const auto iterator = parameters.find(key);
  if (iterator == parameters.end()) {
    return Result<double>(Status::failure(
        ErrorCode::InvalidArgument, "required operation parameter is missing"));
  }
  if (const auto* value = std::get_if<double>(&iterator->second)) {
    return Result<double>(*value);
  }
  return Result<double>(Status::failure(ErrorCode::InvalidArgument,
                                        "operation parameter is not Float64"));
}

}  // namespace

/**
 * @brief Implements exact fail-closed operation parameter validation.
 * @copydetails validate_operation_parameters
 */
Status validate_operation_parameters(
    const OperationTraits& traits,
    const std::map<std::string, ParameterValue>& parameters) {
  const Status schema_status =
      validate_parameter_schema(traits.parameter_schema);
  if (!schema_status.ok() ||
      parameters.size() > traits.parameter_schema.size()) {
    return Status::failure(
        ErrorCode::InvalidArgument,
        "operation parameters exceed or contradict the published schema");
  }
  for (const OperationParameterSpec& declaration : traits.parameter_schema) {
    const auto parameter = parameters.find(declaration.key);
    if (parameter == parameters.end()) {
      if (declaration.required) {
        return Status::failure(
            ErrorCode::InvalidArgument,
            "required operation parameter is missing: " + declaration.key);
      }
      continue;
    }
    if (!parameter_type_matches(parameter->second, declaration.type)) {
      return Status::failure(
          ErrorCode::InvalidArgument,
          "operation parameter has the wrong type: " + declaration.key);
    }
    if (declaration.type == OperationParameterType::String &&
        std::get<std::string>(parameter->second).size() > 8192U) {
      return Status::failure(
          ErrorCode::InvalidArgument,
          "operation string parameter exceeds bounds: " + declaration.key);
    }
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
      return Status::failure(
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
struct OperationRegistry::Impl final {
  /** @brief Immutable owning handle for one published complete definition. */
  using DefinitionHandle = std::shared_ptr<const OperationDefinition>;

  /** @brief Serializes mutation and definition-handle lookup/copy. */
  mutable std::mutex mutex;
  /** @brief Sorted published immutable operation-definition handles. */
  std::map<std::string, DefinitionHandle> definitions;
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
  std::sort(
      definition.traits.parameter_schema.begin(),
      definition.traits.parameter_schema.end(),
      [](const OperationParameterSpec& left,
         const OperationParameterSpec& right) { return left.key < right.key; });
  const Status traits_status = validate_traits(definition.traits);
  if (!valid_key(definition.key) || !definition.callback ||
      !traits_status.ok()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation definition is malformed");
  }
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

  using VersionFunction = std::uint32_t (*)();
  using ApiFunction = const ps_operation_plugin_api_v2* (*)();
  VersionFunction version = nullptr;
  ApiFunction get_api = nullptr;
  void* version_symbol = find_symbol(pending_library.handle(),
                                     "ps_operation_plugin_get_abi_version");
  void* api_symbol =
      find_symbol(pending_library.handle(), "ps_operation_plugin_get_api_v2");
  static_assert(sizeof(version) == sizeof(version_symbol),
                "function/data pointer sizes must match on supported targets");
  std::memcpy(&version, &version_symbol, sizeof(version));
  std::memcpy(&get_api, &api_symbol, sizeof(get_api));
  if (!version || !get_api) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation plugin ABI version is unsupported");
  }
  const ps_operation_plugin_api_v2* api = nullptr;
  try {
    if (version() != PS_OPERATION_ABI_VERSION_2) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "operation plugin ABI version is unsupported");
    }
    api = get_api();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return Status::failure(
        ErrorCode::OperationFailed,
        "operation plugin ABI entry point raised an exception");
  }
  if (!api ||
      reinterpret_cast<std::uintptr_t>(api) %
              alignof(ps_operation_plugin_api_v2) !=
          0U ||
      api->struct_size != sizeof(ps_operation_plugin_api_v2)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation plugin API table is malformed");
  }
  pending_library.attach_api(api);
  if (api->operation_count == 0U || api->operation_count > 1024U ||
      !api->operations ||
      reinterpret_cast<std::uintptr_t>(api->operations) %
              alignof(ps_operation_descriptor_v2) !=
          0U ||
      !api->destroy) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "operation plugin API table is malformed");
  }
#if defined(PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
  plugin_testing::invoke_before_owner_allocation(
      plugin_testing::LibraryKind::Operation);
#endif
  // Transfer destroy-before-unload ownership only after the heap lease exists.
  // Allocation failure leaves the stack owner intact for exact rollback.
  auto library = std::make_shared<OperationLibrary>(std::move(pending_library));

  std::vector<std::shared_ptr<OperationDefinition>> staged;
  staged.reserve(api->operation_count);
  for (std::uint32_t index = 0; index < api->operation_count; ++index) {
    const ps_operation_descriptor_v2& descriptor = api->operations[index];
    if (descriptor.struct_size != sizeof(ps_operation_descriptor_v2) ||
        !descriptor.key || descriptor.key_size == 0U ||
        descriptor.key_size > 1024U || !descriptor.execute ||
        descriptor.input_count > 1024U || descriptor.cacheable > 1U ||
        descriptor.output_rank > 8U ||
        ((descriptor.output_rank == 0U) !=
         (descriptor.output_shape == nullptr)) ||
        (descriptor.output_shape &&
         reinterpret_cast<std::uintptr_t>(descriptor.output_shape) %
                 alignof(std::uint64_t) !=
             0U) ||
        descriptor.parameter_count > 128U ||
        ((descriptor.parameter_count == 0U) !=
         (descriptor.parameters == nullptr)) ||
        (descriptor.parameters &&
         reinterpret_cast<std::uintptr_t>(descriptor.parameters) %
                 alignof(ps_operation_parameter_descriptor_v2) !=
             0U) ||
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
          return candidate->key == definition.key;
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
    if (descriptor.output_rank != 0U) {
      definition.traits.fixed_output_shape.assign(
          descriptor.output_shape,
          descriptor.output_shape + descriptor.output_rank);
    }
    definition.traits.shape_rule = shape_rule.value();
    definition.traits.region_rule = region_rule.value();
    definition.traits.halo_radius = descriptor.halo_radius;
    definition.traits.cacheable = descriptor.cacheable != 0U;
    definition.traits.parameter_schema.reserve(descriptor.parameter_count);
    for (std::uint32_t parameter_index = 0U;
         parameter_index < descriptor.parameter_count; ++parameter_index) {
      const ps_operation_parameter_descriptor_v2& parameter =
          descriptor.parameters[parameter_index];
      if (parameter.struct_size !=
              sizeof(ps_operation_parameter_descriptor_v2) ||
          !parameter.key || parameter.key_size == 0U ||
          parameter.key_size > 1024U || parameter.required > 1U) {
        return Status::failure(
            ErrorCode::InvalidArgument,
            "operation plugin parameter descriptor is malformed");
      }
      auto parameter_type = decode_parameter_type(parameter.type);
      if (!parameter_type.ok()) {
        return parameter_type.status();
      }
      OperationParameterSpec copied;
      copied.key.assign(parameter.key, parameter.key_size);
      copied.type = parameter_type.value();
      copied.required = parameter.required != 0U;
      definition.traits.parameter_schema.push_back(std::move(copied));
    }
    std::sort(definition.traits.parameter_schema.begin(),
              definition.traits.parameter_schema.end(),
              [](const OperationParameterSpec& left,
                 const OperationParameterSpec& right) {
                return left.key < right.key;
              });
    const Status traits_status = validate_traits(definition.traits);
    if (!traits_status.ok()) {
      return traits_status;
    }
    if (definition.traits.shape_rule == OperationShapeRule::Fixed) {
      auto fixed_layout = dense_layout(
          definition.traits.fixed_output_shape,
          Value::element_size(definition.traits.output_element_type));
      if (!fixed_layout.ok()) {
        return Status::failure(
            ErrorCode::InvalidArgument,
            "operation plugin fixed output is not densely representable");
      }
    }
    staged.push_back(
        std::make_shared<OperationDefinition>(std::move(definition)));
  }

  for (std::uint32_t index = 0; index < api->operation_count; ++index) {
    const ps_operation_descriptor_v2* descriptor = &api->operations[index];
    staged[index]->callback =
        [library,
         descriptor](const OperationInvocation& invocation) -> Result<Value> {
      if (invocation.inputs.size() != descriptor->input_count ||
          invocation.input_demands.size() != invocation.inputs.size()) {
        return Result<Value>(
            Status::failure(ErrorCode::InvalidArgument,
                            "plugin invocation input/demand count mismatch"));
      }
      std::uint32_t plugin_backend = 0U;
      switch (invocation.backend) {
        case Backend::Cpu:
          plugin_backend = 1U;
          break;
        case Backend::Gpu:
          plugin_backend = 2U;
          break;
        default:
          return Result<Value>(
              Status::failure(ErrorCode::InvalidArgument,
                              "plugin invocation backend is unknown"));
      }
      std::vector<ps_operation_value_view_v2> views;
      std::vector<std::vector<ps_operation_facet_view_v2>> facet_views;
      std::vector<std::vector<std::uint64_t>> demand_offsets;
      std::vector<std::vector<std::uint64_t>> demand_extents;
      views.reserve(invocation.inputs.size());
      facet_views.reserve(invocation.inputs.size());
      demand_offsets.reserve(invocation.inputs.size());
      demand_extents.reserve(invocation.inputs.size());
      for (std::size_t input_index = 0U; input_index < invocation.inputs.size();
           ++input_index) {
        const Value& input = invocation.inputs[input_index];
        const Region& demand = invocation.input_demands[input_index];
        if (!input.valid()) {
          return Result<Value>(
              Status::failure(ErrorCode::InvalidArgument,
                              "plugin invocation input Value is invalid"));
        }
        const ValueDescriptor& input_descriptor = input.descriptor();
        auto layout =
            dense_layout(input_descriptor.shape,
                         Value::element_size(input_descriptor.element_type));
        if (!layout.ok() ||
            input.region().rank() != input_descriptor.shape.size() ||
            demand.rank() != input_descriptor.shape.size() ||
            !demand.validate(input_descriptor.shape).ok() ||
            input.layout().byte_offset > input.bytes().size() ||
            input.layout().byte_strides != layout.value().byte_strides) {
          return Result<Value>(Status::failure(
              ErrorCode::TypeMismatch,
              "operation plugin requires contiguous input Values"));
        }
        for (std::size_t axis = 0U; axis < input_descriptor.shape.size();
             ++axis) {
          const RegionDimension& dimension = input.region().dimensions()[axis];
          if (dimension.offset != 0U ||
              dimension.extent != input_descriptor.shape[axis]) {
            return Result<Value>(Status::failure(
                ErrorCode::TypeMismatch,
                "operation plugin requires whole input Regions"));
          }
        }
        const std::uint64_t logical_bytes = layout.value().byte_size;
        if (logical_bytes !=
            input.bytes().size() - input.layout().byte_offset) {
          return Result<Value>(
              Status::failure(ErrorCode::TypeMismatch,
                              "operation plugin input bytes are incomplete"));
        }
        auto& input_facets = facet_views.emplace_back();
        input_facets.reserve(input.facets().size());
        for (const ValueFacet& facet : input.facets()) {
          input_facets.push_back(ps_operation_facet_view_v2{
              sizeof(ps_operation_facet_view_v2), facet.key.data(),
              static_cast<std::uint32_t>(facet.key.size()), facet.version,
              facet.payload.empty() ? nullptr : facet.payload.data(),
              static_cast<std::uint32_t>(facet.payload.size())});
        }
        ps_operation_value_view_v2 view{};
        view.struct_size = sizeof(view);
        view.element_type =
            static_cast<std::uint32_t>(input_descriptor.element_type);
        view.rank = static_cast<std::uint32_t>(input_descriptor.shape.size());
        view.byte_size = logical_bytes;
        view.shape = input_descriptor.shape.data();
        auto& offsets = demand_offsets.emplace_back();
        auto& extents = demand_extents.emplace_back();
        offsets.reserve(demand.rank());
        extents.reserve(demand.rank());
        for (const RegionDimension& dimension : demand.dimensions()) {
          offsets.push_back(dimension.offset);
          extents.push_back(dimension.extent);
        }
        view.demand_offsets = offsets.data();
        view.demand_extents = extents.data();
        view.data = input.bytes().data() + input.layout().byte_offset;
        view.facet_count = static_cast<std::uint32_t>(input_facets.size());
        view.facets = input_facets.empty() ? nullptr : input_facets.data();
        views.push_back(view);
      }
      std::vector<ps_operation_parameter_value_v2> parameter_views;
      parameter_views.reserve(invocation.parameters.size());
      for (const auto& parameter : invocation.parameters) {
        ps_operation_parameter_value_v2 view{};
        view.struct_size = sizeof(view);
        view.key = parameter.first.data();
        view.key_size = static_cast<std::uint32_t>(parameter.first.size());
        if (const auto* value = std::get_if<std::int64_t>(&parameter.second)) {
          view.type = PS_OPERATION_PARAMETER_INT64_V2;
          view.int64_value = *value;
        } else if (const auto* value = std::get_if<double>(&parameter.second)) {
          view.type = PS_OPERATION_PARAMETER_FLOAT64_V2;
          view.float64_value = *value;
        } else if (const auto* value = std::get_if<bool>(&parameter.second)) {
          view.type = PS_OPERATION_PARAMETER_BOOL_V2;
          view.bool_value = *value ? 1U : 0U;
        } else {
          const std::string& string_value =
              std::get<std::string>(parameter.second);
          view.type = PS_OPERATION_PARAMETER_STRING_V2;
          view.string_value = string_value.data();
          view.string_size = static_cast<std::uint32_t>(string_value.size());
        }
        parameter_views.push_back(view);
      }
      OutputSinkState output;
      ps_operation_output_sink_v2 sink{};
      sink.struct_size = sizeof(sink);
      sink.context = &output;
      sink.publish = publish_plugin_output;
      char diagnostic[4097]{};
      const int code = descriptor->execute(
          descriptor->user_data, views.data(),
          static_cast<std::uint32_t>(views.size()),
          parameter_views.empty() ? nullptr : parameter_views.data(),
          static_cast<std::uint32_t>(parameter_views.size()), plugin_backend,
          plugin_cancelled,
          const_cast<CancellationToken*>(&invocation.cancellation), &sink,
          diagnostic, sizeof(diagnostic));
      diagnostic[sizeof(diagnostic) - 1U] = '\0';
      if (invocation.cancellation.cancelled()) {
        return Result<Value>(
            Status::failure(ErrorCode::Cancelled, "operation was cancelled"));
      }
      if (output.duplicate_publish_attempted) {
        return Result<Value>(Status::failure(
            ErrorCode::OperationFailed,
            "operation plugin violated output sink at-most-once contract"));
      }
      if (code == PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V2 &&
          output.published) {
        if (!output.result.ok()) {
          return output.result;
        }
        return Result<Value>(Status::failure(
            ErrorCode::OperationFailed,
            "operation plugin published output before reporting backend "
            "unavailable"));
      }
      if (code != PS_OPERATION_RESULT_SUCCESS_V2) {
        ErrorCode error_code = ErrorCode::OperationFailed;
        const char* default_diagnostic = "operation plugin callback failed";
        if (code == PS_OPERATION_RESULT_CANCELLED_V2) {
          error_code = ErrorCode::Cancelled;
          default_diagnostic = "operation plugin callback was cancelled";
        } else if (code == PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V2) {
          error_code = ErrorCode::BackendUnavailable;
          default_diagnostic = "operation plugin backend is unavailable";
        }
        return Result<Value>(
            Status::failure(error_code, diagnostic[0] ? std::string(diagnostic)
                                                      : default_diagnostic));
      }
      return output.result;
    };
  }

  std::vector<Impl::DefinitionHandle> immutable_staged;
  immutable_staged.reserve(staged.size());
  for (auto& definition : staged) {
    immutable_staged.emplace_back(std::move(definition));
  }

  std::map<std::string, Impl::DefinitionHandle> replacement;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->frozen) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "operation registry froze during plugin load");
    }
    for (const Impl::DefinitionHandle& definition : immutable_staged) {
      if (impl_->definitions.count(definition->key) != 0U) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "operation plugin collides with registered key");
      }
    }
    replacement = impl_->definitions;
    for (const Impl::DefinitionHandle& definition : immutable_staged) {
      const auto inserted = replacement.emplace(definition->key, definition);
      if (!inserted.second) {
        return Status::failure(ErrorCode::Internal,
                               "operation plugin staging duplicated a key");
      }
    }
    impl_->definitions.swap(replacement);
  }
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
  return Result<OperationTraits>(iterator->second->traits);
}

/**
 * @brief Implements validated, exception-fenced operation invocation.
 * @copydetails OperationRegistry::invoke
 */
Result<Value> OperationRegistry::invoke(
    const std::string& key, const OperationInvocation& invocation) const {
  Impl::DefinitionHandle definition;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto iterator = impl_->definitions.find(key);
    if (iterator == impl_->definitions.end()) {
      return Result<Value>(Status::failure(ErrorCode::NotFound,
                                           "operation key is not registered"));
    }
    definition = iterator->second;
  }
  if (invocation.inputs.size() != definition->traits.input_count) {
    return Result<Value>(Status::failure(ErrorCode::InvalidArgument,
                                         "operation input count mismatch"));
  }
  if (invocation.input_demands.size() != invocation.inputs.size()) {
    return Result<Value>(
        Status::failure(ErrorCode::InvalidArgument,
                        "operation input demand count does not match inputs"));
  }
  for (std::size_t index = 0U; index < invocation.inputs.size(); ++index) {
    if (!invocation.inputs[index].valid()) {
      return Result<Value>(Status::failure(ErrorCode::InvalidArgument,
                                           "operation input Value is invalid"));
    }
    const Status demand_status = invocation.input_demands[index].validate(
        invocation.inputs[index].descriptor().shape);
    if (!demand_status.ok()) {
      return Result<Value>(demand_status);
    }
  }
  const Status parameter_status =
      validate_operation_parameters(definition->traits, invocation.parameters);
  if (!parameter_status.ok()) {
    return Result<Value>(parameter_status);
  }
  if (invocation.cancellation.cancelled()) {
    return Result<Value>(
        Status::failure(ErrorCode::Cancelled, "operation was cancelled"));
  }
  switch (invocation.backend) {
    case Backend::Cpu:
    case Backend::Gpu:
      break;
    default:
      return Result<Value>(Status::failure(ErrorCode::InvalidArgument,
                                           "operation backend is unknown"));
  }
  if ((invocation.backend == Backend::Cpu &&
       !definition->traits.supports_cpu) ||
      (invocation.backend == Backend::Gpu &&
       !definition->traits.supports_gpu)) {
    return Result<Value>(Status::failure(ErrorCode::BackendUnavailable,
                                         "operation backend is unavailable"));
  }
  auto expected_output = expected_callback_output_descriptor(definition->traits,
                                                             invocation.inputs);
  if (!expected_output.ok()) {
    return Result<Value>(expected_output.status());
  }
  try {
    auto result = definition->callback(invocation);
    if (!result.ok()) {
      return result;
    }
    const Status output_status =
        validate_callback_output(expected_output.value(), result.value());
    return output_status.ok() ? result : Result<Value>(output_status);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    const char* diagnostic = error.what();
    return Result<Value>(Status::failure(ErrorCode::OperationFailed,
                                         diagnostic ? diagnostic : ""));
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
      OperationTraits{0U,
                      true,
                      true,
                      true,
                      true,
                      true,
                      sizeof(double),
                      2U,
                      true,
                      ElementType::Float64,
                      OperationShapeRule::Scalar,
                      OperationRegionRule::Whole,
                      0U,
                      {OperationParameterSpec{
                          "value", OperationParameterType::Float64, true}},
                      {}},
      [](const OperationInvocation& invocation) -> Result<Value> {
        auto value = floating_parameter(invocation.parameters, "value");
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
      OperationTraits{1U,
                      true,
                      true,
                      true,
                      true,
                      true,
                      0U,
                      2U,
                      true,
                      ElementType::Float64,
                      OperationShapeRule::PreserveFirstInput,
                      OperationRegionRule::Elementwise,
                      0U,
                      {},
                      {}},
      [](const OperationInvocation& invocation) -> Result<Value> {
        return Result<Value>(invocation.inputs.front());
      }});
  if (!status.ok()) {
    throw std::logic_error(status.message);
  }

  status = registry->register_operation(OperationDefinition{
      "math.add",
      OperationTraits{2U,
                      true,
                      true,
                      true,
                      true,
                      true,
                      sizeof(double),
                      2U,
                      true,
                      ElementType::Float64,
                      OperationShapeRule::MatchAllInputs,
                      OperationRegionRule::Elementwise,
                      0U,
                      {},
                      {}},
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
      OperationTraits{1U,
                      true,
                      true,
                      true,
                      false,
                      false,
                      0U,
                      2U,
                      false,
                      ElementType::Float64,
                      OperationShapeRule::PreserveFirstInput,
                      OperationRegionRule::Whole,
                      0U,
                      {OperationParameterSpec{
                          "milliseconds", OperationParameterType::Int64, true}},
                      {}},
      [](const OperationInvocation& invocation) -> Result<Value> {
        auto milliseconds =
            integer_parameter(invocation.parameters, "milliseconds");
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
      OperationTraits{1U,
                      true,
                      true,
                      true,
                      true,
                      true,
                      0U,
                      2U,
                      true,
                      ElementType::Float64,
                      OperationShapeRule::PreserveFirstInput,
                      OperationRegionRule::Elementwise,
                      0U,
                      {},
                      {}},
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
