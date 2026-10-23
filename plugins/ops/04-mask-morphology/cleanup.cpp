#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "04-mask-morphology/components.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
enum class CleanupKind { FillHoles, RemoveSmall, FillSmallHoles };
struct Cleanup final : Program {
  CleanupKind kind;
  ElementType type;
  std::uint64_t height, width, count, maximum_area;
  bool diagonal, identity;
  Cleanup(CleanupKind k, const std::vector<OperationMetadata>& in,
          const Parameters& p)
      : kind(k) {
    const auto& source = image(in[0], 2);
    const auto dims = source.sample_shape();
    height = dims[0];
    width = dims[1];
    count = size_product(dims);
    type = source.descriptor.element_type;
    const auto foreground = component_diagonal(p, "foreground_connectivity");
    diagonal = kind == CleanupKind::RemoveSmall ? foreground : !foreground;
    maximum_area =
        kind == CleanupKind::FillHoles
            ? count
            : static_cast<std::uint64_t>(integer(
                  p, kind == CleanupKind::RemoveSmall ? "maximum_removed_area"
                                                      : "maximum_area"));
    identity = maximum_area == 0;
    collective = true;
    scratch_bytes = identity ? 0
                             : bytes(static_cast<unsigned __int128>(count) * 2 *
                                     sizeof(std::uint64_t));
    add_output(this, spatial_schema(type, dims));
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    if (identity) {
      const auto copy_bits = [&](auto value_type) {
        using T = decltype(value_type);
        for (unsigned j = 0; j < inv.output_count; ++j) {
          unary_rows<T>(
              reader, inv.outputs[j], work,
              [&](const void* source, std::ptrdiff_t source_stride,
                  void* target, std::ptrdiff_t target_stride,
                  std::uint64_t length) {
                if (source_stride == sizeof(T) && target_stride == sizeof(T)) {
                  std::memcpy(target, source, length * sizeof(T));
                } else {
                  for (std::uint64_t i = 0; i < length; ++i)
                    save<T>(target, target_stride, i,
                            load<T>(source, source_stride, i));
                }
              });
        }
      };
      if (type == ElementType::Float32)
        copy_bits(std::uint32_t{});
      else
        copy_bits(std::uint64_t{});
      visits.flush();
      return;
    }
    auto* labels = static_cast<std::uint64_t*>(inv.scratch[0].allocation);
    auto* queue = labels + count;
    const bool remove = kind == CleanupKind::RemoveSmall;
    component_input(reader, labels, height, width, remove, &visits);
    component_scan(labels, queue, height, width, diagonal, true, count, &visits,
                   [&](std::uint64_t, std::uint64_t, const ComponentBounds& b,
                       const std::uint64_t* pixels) {
                     const bool exterior =
                         !b.y0 || !b.x0 || b.y1 == height || b.x1 == width;
                     const bool select =
                         remove ? b.area > maximum_area
                                : !exterior && b.area <= maximum_area;
                     for (std::uint64_t i = 0; i < b.area; ++i) {
                       labels[pixels[i]] = select ? 2 : 1;
                       visits.step();
                     }
                   });
    const auto selected = [&](std::uint64_t pixel) {
      return remove ? labels[pixel] == 2 : labels[pixel] != 1;
    };
    for (unsigned j = 0; j < inv.output_count; ++j) {
      if (type == ElementType::Float32)
        component_write_image<float>(inv.outputs[j], width, work, selected);
      else
        component_write_image<double>(inv.outputs[j], width, work, selected);
    }
    visits.flush();
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_cleanup(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    for (auto entry :
         {std::make_pair("fill_holes", CleanupKind::FillHoles),
          std::make_pair("remove_small", CleanupKind::RemoveSmall),
          std::make_pair("fill_small_holes", CleanupKind::FillSmallHoles)}) {
      std::vector<Parameter> parameters{
          {"foreground_connectivity", Type::Int64}};
      if (entry.second != CleanupKind::FillHoles)
        parameters.push_back({entry.second == CleanupKind::RemoveSmall
                                  ? "maximum_removed_area"
                                  : "maximum_area",
                              Type::Int64});
      check(registry->register_operation(
          definition(entry.first, 1, std::move(parameters),
                     [kind = entry.second](const auto& in, const auto& p) {
                       return std::make_shared<Cleanup>(kind, in, p);
                     })));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
