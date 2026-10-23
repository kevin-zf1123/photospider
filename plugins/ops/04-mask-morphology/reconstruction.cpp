#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "04-mask-morphology/lattice.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
struct Reconstruction final : Program {
  bool erode, diagonal;
  std::uint64_t height, width, count;
  ElementType type;
  Reconstruction(bool inverse, const std::vector<OperationMetadata>& in,
                 const Parameters& p)
      : erode(inverse), diagonal(component_diagonal(p, "connectivity")) {
    const auto& s = image(in[0], 2);
    same(in[0], in[1]);
    height = s.sample_shape()[0];
    width = s.sample_shape()[1];
    count = size_product({height, width});
    type = s.descriptor.element_type;
    collective = true;
    scratch_bytes =
        bytes(static_cast<unsigned __int128>(count) *
                  (generation::width(type) * 2 + sizeof(std::uint64_t) + 1) +
              64);
    add_output(this, spatial_schema(type, {height, width}));
  }
  template <class T>
  void run_t(const Kernel& kernel, const ps_kernel_invocation_v8& inv) const {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    auto* state = static_cast<T*>(inv.scratch[0].allocation);
    auto* limit = state + count;
    auto* queue = reinterpret_cast<std::uint64_t*>(limit + count);
    auto* pending = reinterpret_cast<std::uint8_t*>(queue + count);
    for (unsigned port = 0; port < 2; ++port)
      component_read_rows(
          reader, port, 0, height, width, work,
          [&](const void* data, std::ptrdiff_t stride, auto y, auto x, auto n) {
            T* out = (port ? limit : state) + y * width + x;
            for (std::uint64_t i = 0; i < n; ++i) {
              const auto value = load<T>(data, stride, i);
              out[i] = value == 0 ? T(0) : value;
            }
          });
    // A fair monotone relaxation has the same least/greatest fixed point as
    // synchronous geodesic iteration. Values are selected from input levels;
    // no arithmetic or approximate convergence test is needed. At most one
    // pending entry per pixel bounds this circular queue by N.
    for (std::uint64_t i = 0; i < count; ++i) {
      visits.step();
      queue[i] = i;
      pending[i] = 1;
    }
    std::uint64_t head = 0, tail = 0, queued = count;
    while (queued) {
      visits.step();
      const auto index = queue[head];
      if (++head == count)
        head = 0;
      --queued;
      pending[index] = 0;
      const auto y = index / width, x = index % width;
      const auto value = state[index];
      const auto relax = [&](std::uint64_t target) {
        visits.step();
        const auto candidate = erode ? std::max(limit[target], value)
                                     : std::min(limit[target], value);
        if (!(erode ? candidate < state[target] : candidate > state[target]))
          return;
        state[target] = candidate;
        if (!pending[target]) {
          pending[target] = 1;
          queue[tail] = target;
          if (++tail == count)
            tail = 0;
          ++queued;
        }
      };
      if (y)
        relax(index - width);
      if (x)
        relax(index - 1);
      if (x + 1 < width)
        relax(index + 1);
      if (y + 1 < height)
        relax(index + width);
      if (diagonal) {
        if (y && x)
          relax(index - width - 1);
        if (y && x + 1 < width)
          relax(index - width + 1);
        if (y + 1 < height && x)
          relax(index + width - 1);
        if (y + 1 < height && x + 1 < width)
          relax(index + width + 1);
      }
    }
    for (unsigned j = 0; j < inv.output_count; ++j)
      lattice_output(inv.outputs[j], work, [&](auto y, auto x, bool narrow) {
        return convert(state[y * width + x], narrow);
      });
    visits.flush();
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      run_t<float>(kernel, inv);
    else
      run_t<double>(kernel, inv);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_reconstruction(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    for (bool inverse : {false, true})
      check(registry->register_operation(
          definition(inverse ? "reconstruct_erode" : "reconstruct_dilate", 2,
                     {{"connectivity", Type::Int64}},
                     [inverse](const auto& in, const auto& p) {
                       return std::make_shared<Reconstruction>(inverse, in, p);
                     })));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
