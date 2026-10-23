#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/tensor_program.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::generation {
using Parameters = std::map<std::string, ParameterValue>;
using Coordinate = std::array<std::uint64_t, 8>;
inline void require(bool valid, const char* message) {
  if (!valid) {
    throw Status{ErrorCode::TypeMismatch, message};
  }
}
template <class T>
T take(Result<T> value) {
  if (!value.ok()) {
    throw value.status();
  }
  return value.take_value();
}
inline void check(Status value) {
  if (!value.ok()) {
    throw value;
  }
}
inline std::int64_t integer(const Parameters& p, const char* key,
                            std::int64_t fallback = 0) {
  const auto it = p.find(key);
  return it == p.end() ? fallback : std::get<std::int64_t>(it->second);
}
inline double real(const Parameters& p, const char* key, double fallback = 0) {
  const auto it = p.find(key);
  return it == p.end() ? fallback : std::get<double>(it->second);
}
inline std::string text(const Parameters& p, const char* key,
                        const char* fallback = "") {
  const auto it = p.find(key);
  return it == p.end() ? fallback : std::get<std::string>(it->second);
}
inline std::uint64_t size_product(const std::vector<std::uint64_t>& shape,
                                  std::uint64_t limit = UINT64_C(1) << 40) {
  require(!shape.empty() && shape.size() <= 8, "generation rank must be 1..8");
  std::uint64_t count = 1;
  for (const auto n : shape) {
    require(n && n <= limit / count, "generation extent/product limit");
    count *= n;
  }
  return count;
}
inline std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
  require(b <= (UINT64_C(1) << 40) - a, "generation capacity overflow");
  return a + b;
}
inline unsigned width(ElementType type) {
  return type == ElementType::UInt8 || type == ElementType::Int8     ? 1U
         : type == ElementType::UInt16 || type == ElementType::Int16 ? 2U
         : type == ElementType::Float32                              ? 4U
                                                                     : 8U;
}
inline ElementType dtype(const Parameters& p) {
  const auto value = text(p, "dtype", "float64");
  require(value == "float32" || value == "float64",
          "generation dtype must be float32 or float64");
  return value == "float32" ? ElementType::Float32 : ElementType::Float64;
}
inline bool floating(ElementType type) {
  return type == ElementType::Float32 || type == ElementType::Float64;
}
inline const ResultTensorSpec& tensor(const OperationMetadata& input,
                                      bool allow_batch = false) {
  require(input.result_schema && input.result_schema->tensors.size() == 1,
          "generation input requires exactly one tensor");
  check(input.result_schema->validate());
  const auto& result = input.result_schema->tensors[0];
  require(allow_batch || result.batch_axes.empty(),
          "generation inputs use explicit logical axes");
  size_product(result.sample_shape());
  return result;
}
inline void shape(const OperationMetadata& input,
                  const std::vector<std::uint64_t>& expected,
                  bool floats = true) {
  const auto& spec = tensor(input);
  require(spec.sample_shape() == expected, "generation input shape mismatch");
  require(!floats || floating(spec.descriptor.element_type),
          "generation input requires Float32/64");
}
SchemaTemplate tensor_schema(ElementType type,
                             const std::vector<std::uint64_t>& shape,
                             bool spatial = false, bool components = false);
SchemaTemplate bundle(const char* id, std::vector<ResultTensorSpec> members);
ResultTensorSpec member(const char* key, ElementType type,
                        const std::vector<std::uint64_t>& shape,
                        unsigned atomic = 0, int component = -1);
struct ReadRegion final {
  unsigned port, member;
  Region region;
};
struct Kernel;
struct Program {
  std::vector<OperationMetadata> inputs, outputs;
  std::uint64_t scratch_bytes = 0;
  std::uint64_t work_limit = INT64_MAX;
  bool complete = false;
  // One invocation shares global work and writes only requested output tiles.
  // Its support and scratch are independent of the individual tile/port.
  bool collective = false;
  bool whole_inputs = false;
  virtual ~Program() = default;
  // Operator-owned scratch is specialized to the retained output tile.
  virtual std::uint64_t task_scratch(unsigned, const Region&) const {
    return scratch_bytes;
  }
  virtual std::vector<ReadRegion> support(unsigned output,
                                          const Region& region) const;
  virtual void run(const Kernel& kernel,
                   const ps_kernel_invocation_v8& invocation) const = 0;
};
struct ReadGroup final {
  unsigned port, member;
  TileGrid grid;
  ElementType type;
  std::vector<std::pair<TileIndex, unsigned>> slots;
};
struct Kernel final {
  const Program* program;
  std::vector<ReadGroup> groups;
  std::vector<unsigned> input_base;
  std::vector<std::pair<unsigned, unsigned>> writes;
};
struct Work final {
  const ps_kernel_invocation_v8& invocation;
  std::uint64_t limit;
  mutable std::uint64_t used = 0;
  Status operator()(std::uint64_t amount) const {
    if (invocation.cancellation.is_cancelled &&
        invocation.cancellation.is_cancelled(invocation.cancellation.context)) {
      return {ErrorCode::Cancelled, {}, FailureReason::Cancelled};
    }
    if (amount > limit - used) {
      return {ErrorCode::ResourceExhausted, "generation arithmetic work limit",
              FailureReason::WorkLimit};
    }
    used += amount;
    return Status::success();
  }
  void poll(std::uint64_t amount = 1) const { check((*this)(amount)); }
};
struct Reader final {
  const Kernel& kernel;
  const ps_kernel_invocation_v8& invocation;
  unsigned input_offset = 0;
  std::uint64_t bits(unsigned port, unsigned member,
                     const Coordinate& at) const;
  double number(unsigned port, unsigned member, const Coordinate& at) const;
  std::int64_t index(unsigned port, unsigned member,
                     const Coordinate& at) const {
    const auto value = bits(port, member, at);
    std::int64_t result;
    std::memcpy(&result, &value, 8);
    return result;
  }
};
inline std::uint64_t bits(double value) {
  std::uint64_t result;
  std::memcpy(&result, &value, 8);
  return result;
}
inline double number(std::uint64_t value) {
  double result;
  std::memcpy(&result, &value, 8);
  return result;
}
inline std::uint64_t convert(double value, bool narrow) {
  if (!narrow) {
    return bits(value);
  }
  const auto sample = static_cast<float>(value);
  std::uint32_t result;
  std::memcpy(&result, &sample, 4);
  return result;
}
inline std::ptrdiff_t offset(const ps_kernel_layout_v8& layout,
                             std::uint64_t base, const Coordinate& at) {
  __int128 result = base;
  for (unsigned axis = 0; axis < layout.rank; ++axis) {
    result += static_cast<__int128>(at[axis] - layout.origin[axis]) *
              layout.strides[axis];
  }
  return static_cast<std::ptrdiff_t>(result);
}
inline void store(const ps_kernel_mutable_view_v8& view, const Coordinate& at,
                  std::uint64_t value) {
  std::memcpy(static_cast<std::uint8_t*>(view.allocation) +
                  offset(*view.layout, view.byte_offset, at),
              &value,
              width(static_cast<ElementType>(view.layout->element_type)));
}
template <class Function>
void samples(const ps_kernel_mutable_view_v8& view, Function function) {
  Coordinate at{};
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < view.layout->rank; ++axis) {
    at[axis] = view.layout->origin[axis];
    count *= view.layout->shape[axis];
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    function(at, i);
    for (unsigned axis = view.layout->rank; axis-- > 0;) {
      if (++at[axis] < view.layout->origin[axis] + view.layout->shape[axis]) {
        break;
      }
      at[axis] = view.layout->origin[axis];
    }
  }
}
using Prepare = std::function<std::shared_ptr<Program>(
    const std::vector<OperationMetadata>&, const Parameters&)>;
OperationDefinition operation(
    const std::string& key, unsigned inputs,
    std::vector<OperationParameterSpec> parameters,
    const std::vector<std::pair<std::string, SchemaTemplate>>& outputs,
    Prepare prepare, bool repeated = false, bool deterministic = true);
OperationPreparation prepared(std::shared_ptr<Program> state);
std::vector<OperationParameterSpec> canvas_parameters(bool output_dtype = true);
std::vector<OperationParameterSpec> rng_parameters();
void append(std::vector<OperationParameterSpec>* into,
            const std::vector<OperationParameterSpec>& values);
void add_output(Program* p, SchemaTemplate schema);
struct Canvas {
  std::uint64_t height, width;
  std::int64_t origin_x, origin_y;
  ElementType type;
  explicit Canvas(const Parameters& p)
      : height(integer(p, "height")),
        width(integer(p, "width")),
        origin_x(integer(p, "origin_x")),
        origin_y(integer(p, "origin_y")),
        type(dtype(p)) {
    size_product({height, width});
  }
};
}  // namespace ps::plugin_internal::generation
