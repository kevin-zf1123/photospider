#include <algorithm>
#include <cstdint>
#include <utility>

#include "photospider/data/result.hpp"
#include "photospider/data/tiles.hpp"

int main() {
  ps::SchemaTemplate schema;
  schema.id = "component.image";
  ps::ResultTensorSpec image;
  image.key = "rgb";
  image.descriptor = {ps::ElementType::UInt8, {2, 3, 3}};
  image.layout.spatial = true;
  image.layout.height_axis = 0;
  image.layout.width_axis = 1;
  image.layout.channel_axis = 2;
  schema.tensors.push_back(image);
  auto grid = ps::TileGrid::from_schema(schema, 4);
  if (!grid.ok())
    return 1;
  auto region = grid.value().region(0);
  if (!region.ok())
    return 1;
  auto layout = ps::tensor_tile_storage(image, region.value(), 4);
  if (!layout.ok())
    return 1;
  ps::ResourceBudget budget;
  auto invalid = budget.allocator().native_adapter(
      [](auto, const auto&, const auto&, const auto&) {
        return ps::Result<ps::MutableBuffer>(ps::MutableBuffer{});
      });
  if (invalid.allocate(16).ok())
    return 1;
  auto memory = budget.allocator().allocate(layout.value().bytes);
  if (!memory.ok())
    return 1;
  auto tile = memory.take_value();
  if (reinterpret_cast<uintptr_t>(tile.data()) % ps::host_page_alignment())
    return 1;
  auto& stride = layout.value().strides;
  tile.data()[stride[0] + stride[1] * 2 + stride[2]] = 37;
  auto builder = ps::ResultBuilder::start(budget, schema, "data-client", {}, 4);
  if (!builder.ok())
    return 1;
  auto b = builder.take_value();
  if (!b.publish_planar_tile(0, region.value(), stride[0],
                             std::move(tile).freeze())
           .ok())
    return 1;
  auto result = b.seal();
  if (!result.ok())
    return 1;
  auto desc = result.value().descriptor();
  if (!desc.ok())
    return 1;
  uint8_t sample = 0;
  return result.value()
                     .read_tensor(desc.value(), 0, {1, 2, 1}, &sample, 1)
                     .ok() &&
                 sample == 37
             ? 0
             : 1;
}
