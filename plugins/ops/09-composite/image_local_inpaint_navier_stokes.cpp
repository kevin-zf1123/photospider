#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "09-composite/inpaint_ns.hpp"
#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using inpaint_ns::check_stop;
float sample(const Value& input, const std::vector<std::uint64_t>& coordinate) {
  const auto address = input.byte_address(coordinate);
  if (!address.ok())
    throw address.status();
  float result;
  std::memcpy(&result, input.bytes().data() + address.value(), 4);
  return result;
}
Status profile(const OperationInvocation& call) {
  const auto& image = call.inputs[0];
  const auto& shape = image.descriptor().shape;
  if (shape.size() != 3 || shape[2] != 4 || shape[0] < 3 || shape[1] < 3 ||
      shape[0] > 32768 || shape[1] > 32768 || shape[0] > INT32_MAX / shape[1] ||
      call.inputs[1].descriptor().shape !=
          std::vector<std::uint64_t>{shape[0], shape[1]})
    return Status::failure(ErrorCode::TypeMismatch, "inpaint shape profile");
  auto semantic = decode_semantic(image.facets().at(0));
  if (!semantic.ok())
    return semantic.status();
  auto expected = rgba_semantics();
  expected.reference = semantic.value().reference;
  auto facet = encode_semantic(expected);
  if (!facet.ok() ||
      !input_internal::same_facets(image.facets(), {facet.value()}))
    return Status::failure(
        ErrorCode::TypeMismatch,
        "inpaint requires ordered linear premultiplied RGBA");
  return Status::success();
}
Result<Value> execute(const OperationInvocation& call, bool adapter) {
  try {
    check_stop(call.cancellation);
    input_internal::Float32Environment environment;
    if (!environment.active())
      throw inpaint_ns::NumericFailure{};
    auto valid = profile(call);
    if (!valid.ok())
      return Result<Value>(valid);
    const auto& image = call.inputs[0];
    const auto& hole_mask = call.inputs[1];
    const auto h = image.descriptor().shape[0], w = image.descriptor().shape[1];
    // Bounds above prove all products, padded extents, signed indices and
    // insertion orders below fit size_t/int32 on the required 64-bit host.
    const auto n = h * w, p = (h + 2) * (w + 2);
    std::vector<std::uint64_t> ic(3), mc(2);
    std::uint64_t holes = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      if ((i & 511U) == 0)
        check_stop(call.cancellation);
      ic[0] = mc[0] = i / w;
      ic[1] = mc[1] = i % w;
      const auto mask = sample(hole_mask, mc);
      if (mask != 0 && mask != 1)
        throw inpaint_ns::NumericFailure{};
      holes += mask != 0;
      for (ic[2] = 0; ic[2] < 4; ++ic[2]) {
        const auto value = sample(image, ic);
        if (!std::isfinite(value) || (ic[2] == 3 && value != 1))
          throw inpaint_ns::NumericFailure{};
      }
    }
    if (holes == n)
      return Result<Value>(Status::failure(ErrorCode::OperationFailed,
                                           "inpaint mask has no known pixels"));
    check_stop(call.cancellation);
    auto made = MutableValue::allocate(image.descriptor(), call.output_region,
                                       call.allocator);
    if (!made.ok())
      return Result<Value>(made.status());
    auto output = made.take_value();
    for (std::uint64_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0)
        check_stop(call.cancellation);
      ic[0] = i / w;
      ic[1] = i % w;
      for (ic[2] = 0; ic[2] < 4; ++ic[2]) {
        const auto value = sample(image, ic);
        std::memcpy(output.data() + (i * 4 + ic[2]) * 4, &value, 4);
      }
    }
    if (holes) {
      auto allocate = [&](std::uint64_t size) {
        check_stop(call.cancellation);
        auto buffer = call.allocator.allocate(size);
        if (!buffer.ok())
          throw buffer.status();
        check_stop(call.cancellation);
        return buffer.take_value();
      };
      auto mask_buffer = allocate(n), plane_buffer = allocate(4 * n);
      auto* mask = mask_buffer.data();
      auto* plane = reinterpret_cast<float*>(plane_buffer.data());
      MutableBuffer state, time, heap, second;
      if (adapter) {
        second = allocate(4 * n);
      } else {
        state = allocate(p);
        time = allocate(4 * p);
        heap = allocate(16 * n);
      }
      for (std::uint64_t i = 0; i < n; ++i) {
        if ((i & 4095U) == 0)
          check_stop(call.cancellation);
        mc[0] = i / w;
        mc[1] = i % w;
        mask[i] = sample(hole_mask, mc) == 0 ? 0 : 255;
      }
      const auto radius = static_cast<int>(
          std::get<std::int64_t>(call.parameters.at("radius")));
      for (std::uint64_t c = 0; c < 3; ++c) {
        check_stop(call.cancellation);
        for (std::uint64_t i = 0; i < n; ++i) {
          if ((i & 4095U) == 0)
            check_stop(call.cancellation);
          float value = 0;
          if (!mask[i])
            std::memcpy(&value, output.data() + (i * 4 + c) * 4, 4);
          new (plane + i) float{value};
        }
        std::feclearexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW);
        float* result = plane;
        if (adapter) {
#ifdef PHOTOSPIDER_HAS_INPAINT_OPENCV
          result = reinterpret_cast<float*>(second.data());
          check_stop(call.cancellation);
          inpaint_ns::opencv(plane, result, mask, static_cast<int>(h),
                             static_cast<int>(w), radius);
#else
          return Result<Value>(Status::failure(ErrorCode::BackendUnavailable,
                                               "OpenCV adapter disabled"));
#endif
        } else {
          inpaint_ns::native(plane, mask, static_cast<int>(h),
                             static_cast<int>(w), radius, state.data(),
                             reinterpret_cast<float*>(time.data()), heap.data(),
                             call.cancellation);
        }
        check_stop(call.cancellation);
        if (std::fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW))
          throw inpaint_ns::NumericFailure{};
        for (std::uint64_t i = 0; i < n; ++i) {
          if ((i & 4095U) == 0)
            check_stop(call.cancellation);
          if (!std::isfinite(result[i]))
            throw inpaint_ns::NumericFailure{};
          if (mask[i])
            std::memcpy(output.data() + (i * 4 + c) * 4, result + i, 4);
        }
      }
    }
    check_stop(call.cancellation);
    return std::move(output).publish(image.facets());
  } catch (const inpaint_ns::Stopped&) {
    return Result<Value>(
        Status::failure(ErrorCode::Cancelled, "inpaint cancelled"));
  } catch (const inpaint_ns::NumericFailure&) {
    return Result<Value>(
        Status::failure(ErrorCode::OperationFailed,
                        "inpaint requires binary coverage, opaque finite RGBA "
                        "and finite arithmetic"));
  } catch (const Status& status) {
    return Result<Value>(status);
  } catch (const std::bad_alloc&) {
    return Result<Value>(Status::failure(ErrorCode::ResourceExhausted,
                                         "inpaint allocation failed"));
  }
}
Status add(OperationRegistry* registry, const char* key, bool adapter) {
  OperationDefinition op;
  op.key = key;
  auto& t = op.traits;
  // A replaceable OpenCV binary is not covered by the kernel source build ID.
  // Disable reusable results for the adapter and its downstream consumers.
  t.cacheable = !adapter;
  t.input_count = 2;
  OperationPortConstraint image;
  image.kind = OperationPortKind::Typed;
  image.semantic_kind = static_cast<std::uint32_t>(SemanticKind::Image);
  image.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  image.rank = 3;
  t.input_schema = {image, {OperationPortKind::Float32Mask, 0, 0}};
  t.parameter_schema = {
      {"radius", OperationParameterType::Int64, true, true, 1, 32}};
  // Native scratch is 21*N+5*P. For H,W>=3 this is <35*N.
  // 2*input bytes + fixed headroom bounds it. Adapter host scratch is 9*N;
  // OpenCV's own allocation capacity is external and is not a host budget.
  t.workspace_input_multiplier = 2;
  t.workspace_bytes = 65536;
  auto& out = t.outputs[0];
  out.key = "image";
  out.output_element_type = ElementType::Float32;
  out.shape_rule = OperationShapeRule::PreserveFirstInput;
  out.region_rule = OperationRegionRule::Whole;
  out.output_semantic_rule = OperationSemanticRule::PreserveInput;
  out.output_schema = image;
  out.requires_dense_output = true;
  op.callback = [adapter](const OperationInvocation& call) {
    return execute(call, adapter);
  };
  return registry->register_operation(std::move(op));
}
}  // namespace
Status register_image_local_inpaint_navier_stokes(OperationRegistry* registry) {
  auto status =
      add(registry, "image.local_inpaint_navier_stokes_native_apple_silicon",
          false);
  if (!status.ok())
    return status;
#ifdef PHOTOSPIDER_HAS_INPAINT_OPENCV
  return add(registry, "image.local_inpaint_navier_stokes_openCV", true);
#else
  return status;
#endif
}
}  // namespace ps::plugin_internal
