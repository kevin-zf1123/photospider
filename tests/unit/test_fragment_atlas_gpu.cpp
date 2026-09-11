#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "execution/native_gpu.hpp"
#include "photospider/photospider.hpp"
#include "photospider/plugin/fragment_atlas_msl.h"
#include "support/test_support.hpp"

namespace {
// MSL continuation strings are not namespace indentation.
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char shader[] = PS_FRAGMENT_ATLAS_MSL_V9
    "kernel void lookup(device const uchar* data [[buffer(0)]],\n"
    " device const ulong* directory [[buffer(1)]],\n"
    " device const ulong* coordinates [[buffer(2)]],\n"
    " device ulong* output [[buffer(3)]], constant ulong* config "
    "[[buffer(4)]],\n"
    " uint id [[thread_position_in_grid]]) {\n"
    " ulong at[8]={0}, address=0;\n"
    " for (uint a=0; a<config[0]; ++a) at[a]=coordinates[id*config[0]+a];\n"
    " bool found=ps_atlas_address(directory,config[1],config+4,config+12,\n"
    "  uint(config[0]),at,config[2],uint(config[3]),address);\n"
    " output[id*2]=found ? 1 : 0;\n"
    " ulong bits=0; if (found) for (uint b=0; b<config[3]; ++b)\n"
    "  bits|=ulong(data[address+b])<<(8*b);\n"
    " output[id*2+1]=bits;\n"
    "}\n";
// NOLINTEND
using namespace ps;  // NOLINT(build/namespaces)
int run(const std::shared_ptr<gpu_internal::Device>& device,
        const std::vector<std::uint64_t>& shape,
        const std::vector<std::uint64_t>& geometry,
        const std::vector<std::vector<std::uint64_t>>& sources,
        const std::vector<std::vector<std::uint64_t>>& queries,
        ElementType type = ElementType::Float32) {
  const ValueDescriptor descriptor{type, shape};
  const auto width = Value::element_size(type);
  std::vector<Value> values;
  std::vector<Region> boxes;
  std::map<std::vector<std::uint64_t>, std::uint64_t> oracle;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    std::vector<RegionDimension> dimensions;
    for (auto at : sources[i])
      dimensions.push_back({at, 1});
    Region region(dimensions);
    auto writer = MutableValue::allocate(descriptor, region, BufferAllocator{})
                      .take_value();
    const std::uint64_t bits =
        (width == 8 ? UINT64_C(0xdeadbeef00000000) : 0) + 100 + i;
    std::memcpy(writer.data(), &bits, width);
    values.push_back(std::move(writer).publish().take_value());
    boxes.push_back(region);
    oracle[sources[i]] = bits;
  }
  auto coverage = Footprint::from_regions(shape, boxes).take_value();
  auto input =
      ValueFragments::create(descriptor, {}, coverage, values).take_value();
  auto plan = FragmentAtlasPlan::prepare(input, geometry).take_value();
  std::vector<std::uint64_t> coordinates;
  for (const auto& at : queries)
    coordinates.insert(coordinates.end(), at.begin(), at.end());
  const auto atlas_capacity =
      gpu_internal::allocation_capacity(plan.payload_allocation_bytes()) +
      gpu_internal::allocation_capacity(plan.directory_allocation_bytes());
  PS_CHECK(plan.materialize(input, device->allocator(BufferAllocator{}.limited(
                                       atlas_capacity - 1)))
               .status()
               .code == ErrorCode::ResourceExhausted);
  const auto capacity =
      atlas_capacity +
      gpu_internal::allocation_capacity(coordinates.size() * 8) +
      gpu_internal::allocation_capacity(queries.size() * 16);
  auto allocator = device->allocator(BufferAllocator{}.limited(capacity));
  auto packed = plan.materialize(input, allocator);
  PS_CHECK(packed.ok());
  auto atlas = packed.take_value();
  PS_CHECK(device->owns(*atlas.payload.storage()) &&
           device->owns(*atlas.directory.storage()));
  PS_CHECK(
      atlas.directory.storage()->capacity() ==
      gpu_internal::allocation_capacity(plan.directory_allocation_bytes()));
  auto indices = allocator.allocate(coordinates.size() * 8).take_value();
  std::memcpy(indices.data(), coordinates.data(), coordinates.size() * 8);
  auto output = allocator.allocate(queries.size() * 16).take_value();
  gpu_internal::Invocation invocation(device, {});
  const auto* service = invocation.service();
  std::array<ps_gpu_buffer_binding_v9, 4> buffers{};
  const std::uint8_t* pointers[]{atlas.payload.bytes().data(),
                                 atlas.directory.bytes().data(), indices.data(),
                                 output.data()};
  const std::uint64_t sizes[]{atlas.payload.bytes().size(),
                              atlas.directory.bytes().size(), indices.size(),
                              output.size()};
  for (unsigned i = 0; i < 4; ++i) {
    std::uint64_t token = 0;
    PS_CHECK(service->buffer(service->context, pointers[i], sizes[i], i == 3,
                             &token) == 0);
    buffers[i] = {
        sizeof(ps_gpu_buffer_binding_v9), i, token, 0, sizes[i], i == 3};
  }
  std::array<std::uint64_t, 20> config{};
  config[0] = shape.size();
  config[1] = atlas.slot_count;
  config[2] = atlas.payload_bytes;
  config[3] = width;
  for (std::size_t axis = 0; axis < shape.size(); ++axis) {
    config[4 + axis] = shape[axis];
    config[12 + axis] = atlas.tile_shape[axis];
  }
  ps_gpu_dispatch_v9 command{};
  command.struct_size = sizeof(command);
  command.source = shader;
  command.source_size = sizeof(shader) - 1;
  command.entry = "lookup";
  command.entry_size = 6;
  command.buffers = buffers.data();
  command.buffer_count = buffers.size();
  command.constants = config.data();
  command.constant_size = sizeof(config);
  command.constant_index = 4;
  command.grid[0] = queries.size();
  command.grid[1] = command.grid[2] = 1;
  const auto code = service->execute(service->context, &command, 1);
  if (code)
    std::cerr << invocation.status().message << '\n';
  PS_CHECK(code == 0 && invocation.statistics().dispatches == 1);
  for (std::size_t i = 0; i < queries.size(); ++i) {
    std::uint64_t found = 0, bits = 0;
    std::memcpy(&found, output.data() + i * 16, 8);
    std::memcpy(&bits, output.data() + i * 16 + 8, 8);
    const auto expected = oracle.find(queries[i]);
    PS_CHECK(found == (expected != oracle.end()) &&
             bits == (expected == oracle.end() ? 0 : expected->second));
  }
  return 0;
}
}  // namespace
int main() {
  auto device = ps::gpu_internal::Device::create();
  if (!device || !device->available()) {
    std::cout << "SKIP: native Metal unavailable\n";
    return 77;
  }
  PS_CHECK(run(device, {1000}, {64}, {{0}, {2}, {63}, {64}, {999}},
               {{0}, {1}, {2}, {63}, {64}, {65}, {999}, {1000}}) == 0);
  std::vector<std::vector<std::uint64_t>> sources, queries;
  for (std::uint64_t i = 0; i < 65; ++i) {
    sources.push_back({i * 128});
    queries.push_back({i * 128});
    queries.push_back({i * 128 + 1});
  }
  PS_CHECK(run(device, {16384}, {64}, sources, queries) == 0);
  sources.clear();
  queries.clear();
  for (std::uint64_t c = 0; c < 4; ++c) {
    sources.push_back({0, 0, c});
    sources.push_back({100000, 100000, c});
    queries.push_back({0, 0, c});
    queries.push_back({100000, 100000, c});
    queries.push_back({0, 1, c});
  }
  PS_CHECK(run(device, {100001, 100001, 4}, {2, 8, 4}, sources, queries) == 0);
  const std::vector<std::uint64_t> shape(8, UINT64_C(1) << 40), first(8, 0),
      last(8, (UINT64_C(1) << 40) - 1);
  auto hole = last;
  --hole[0];
  PS_CHECK(run(device, shape, {}, {first, last}, {first, last, hole}) == 0);
  for (const auto type : {ps::ElementType::UInt8, ps::ElementType::Int64,
                          ps::ElementType::Float64})
    PS_CHECK(run(device, {1000}, {64}, {{0}, {63}, {999}},
                 {{0}, {1}, {63}, {999}}, type) == 0);
  std::cout << "native_atlas: " << device->identity()
            << ", dispatches=7, ranks=1/3/8, widths=1/4/8, fragments=65, "
               "holes=checked\n";
  return 0;
}
