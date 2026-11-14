#include "psx/bindings_io.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#include "common/codec.hpp"
#include "common/process.hpp"

namespace ps::tools::psx {
ExecutionBindings read_bindings(const std::string& manifest,
                                const ExecutionPlan& plan, Files* files,
                                const ResourceBudget& root) {
  ExecutionBindings result;
  if (manifest.empty()) {
    if (!plan.input_declarations().empty())
      throw ToolError("Plan requires --bindings");
    return result;
  }
  JsonDocument document(files->read(manifest));
  auto json = document.root();
  json.fields({"format", "version", "inputs"});
  format(json, "photospider.bindings");
  std::map<std::string, Json> inputs;
  for (auto item : json.at("inputs").array()) {
    item.fields({"name", "tensors"});
    if (!inputs.emplace(item.at("name").text(), item).second)
      throw ToolError("duplicate input binding");
  }
  if (inputs.size() != plan.input_declarations().size())
    throw ToolError("binding count mismatch");
  for (std::size_t input = 0; input < plan.input_declarations().size();
       ++input) {
    const auto& declaration = plan.input_declarations()[input];
    auto found = inputs.find(declaration.name);
    if (found == inputs.end())
      throw ToolError("missing input binding: " + declaration.name);
    auto builder =
        take(ResultBuilder::start(root, *declaration.result_schema,
                                  "psx.binding", {}, plan.tile_extent()));
    std::map<std::uint32_t, Json> tensors;
    for (auto item : found->second.at("tensors").array()) {
      item.fields({"slot", "path", "encoding", "dtype", "shape"});
      if (!tensors.emplace(u32(item.at("slot")), item).second)
        throw ToolError("duplicate binding slot");
    }
    if (tensors.size() != declaration.result_schema->tensors.size())
      throw ToolError("binding tensor count mismatch");
    for (std::uint32_t slot = 0;
         slot < declaration.result_schema->tensors.size(); ++slot) {
      if (!tensors.count(slot))
        throw ToolError("missing binding slot");
      auto item = tensors.at(slot);
      const auto& spec = declaration.result_schema->tensors[slot];
      if (item.at("encoding").text() != "raw-le" ||
          dtype(item.at("dtype").text()) != spec.descriptor.element_type ||
          shape(item.at("shape")) != spec.sample_shape())
        throw ToolError("binding descriptor mismatch: " + declaration.name);
      auto region = Region::whole(spec.sample_shape());
      auto size = byte_count(region, spec.descriptor.element_type);
      if (size > (UINT64_MAX - 64) / 2)
        throw ToolError("binding size exceeds capacity");
      // Files::read grows its owning string while reading bounded chunks.
      auto capacity = ResourceCapacity::host(size * 2 + 64);
      capacity[ResourceKind::Payload] = size * 2 + 64;
      auto lease = take(root.reserve(capacity));
      auto bytes =
          files->read(relative(manifest, item.at("path").text()), size);
      if (bytes.size() != size)
        throw ToolError("binding byte length mismatch");
      endian(&bytes, TensorSpan::element_size(spec.descriptor.element_type));
      const auto width = TensorSpan::element_size(spec.descriptor.element_type);
      const auto shape = spec.sample_shape();
      std::array<std::uint64_t, 8> strides{};
      std::uint64_t stride = width;
      for (std::size_t axis = shape.size(); axis-- > 0;) {
        strides[axis] = stride;
        stride *= shape[axis];
      }
      const auto& program = plan.program();
      const auto& input_value = program.values[program.inputs[input]];
      if (input_value.input_layout == physical::InputLayout::DenseHost) {
        std::uint64_t alignment = 0;
        for (auto id : input_value.tiles)
          if (program.tiles[id].member == slot)
            alignment = std::max(alignment, program.tiles[id].alignment);
        if (alignment) {
          interrupted();
          auto buffer =
              take(root.allocator().allocate_aligned(size, alignment));
          std::memcpy(buffer.data(), bytes.data(), size);
          StridedLayout layout;
          layout.origin.resize(shape.size());
          for (std::size_t axis = 0; axis < shape.size(); ++axis)
            layout.byte_strides.push_back(
                static_cast<std::int64_t>(strides[axis]));
          check(builder.publish_tensor(slot, region, std::move(layout),
                                       std::move(buffer).freeze()));
        }
        continue;
      }
      for (auto id : program.values[program.inputs[input]].tiles) {
        const auto& tile = program.tiles[id];
        if (tile.member != slot)
          continue;
        interrupted();
        auto box =
            take(plan.input_tile_grids()[input][slot].region(tile.tile_index));
        const auto tile_size = byte_count(box, spec.descriptor.element_type);
        const auto& dimensions = box.dimensions();
        std::array<std::uint64_t, 8> at{};
        StridedLayout layout;
        layout.origin.resize(shape.size());
        layout.byte_strides.resize(shape.size());
        stride = width;
        for (std::size_t axis = shape.size(); axis-- > 0;) {
          at[axis] = dimensions[axis].offset;
          layout.origin[axis] = at[axis];
          layout.byte_strides[axis] = static_cast<std::int64_t>(stride);
          stride *= dimensions[axis].extent;
        }
        const auto& compiled = program.layouts[tile.layout];
        bool compatible = tile.domain == physical::Domain::Host &&
                          compiled.byte_offset == 0 && tile.bytes == tile_size;
        for (std::size_t axis = 0; compatible && axis < shape.size(); ++axis)
          compatible = dimensions[axis].extent == 1 ||
                       compiled.strides[axis] == layout.byte_strides[axis];
        if (!compatible)
          throw ToolError(
              "raw-le binding requires a dense Host input tile layout", 1,
              "PS-TypeMismatch");
        auto buffer =
            take(root.allocator().allocate_aligned(tile_size, tile.alignment));
        const auto row_bytes = dimensions.back().extent * width;
        for (std::uint64_t offset = 0; offset < tile_size;
             offset += row_bytes) {
          std::uint64_t source = 0;
          for (std::size_t axis = 0; axis < shape.size(); ++axis)
            source += at[axis] * strides[axis];
          std::memcpy(buffer.data() + offset, bytes.data() + source, row_bytes);
          for (std::size_t axis = shape.size() - 1; axis-- > 0;) {
            if (++at[axis] < dimensions[axis].offset + dimensions[axis].extent)
              break;
            at[axis] = dimensions[axis].offset;
          }
        }
        check(builder.publish_tensor(slot, box, std::move(layout),
                                     std::move(buffer).freeze()));
      }
    }
    result.inputs.push_back({declaration.name, take(builder.seal())});
  }
  return result;
}
}  // namespace ps::tools::psx
