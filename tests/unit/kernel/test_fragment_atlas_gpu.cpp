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
#include "support/fixed_native_fixture.hpp"
#include "support/status_check.hpp"

namespace {
// MSL source strings are not namespace indentation.
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char shader[] = PS_FRAGMENT_ATLAS_MSL_V11
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
  const TensorDescriptor descriptor{type, shape};
  const auto width = TensorSpan::element_size(type);
  std::vector<TensorSpan> values;
  std::vector<Region> boxes;
  std::map<std::vector<std::uint64_t>, std::uint64_t> oracle;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    std::vector<RegionDimension> dimensions;
    for (auto at : sources[i])
      dimensions.push_back({at, 1});
    Region region(dimensions);
    auto writer = TensorWriter::allocate(descriptor, region, BufferAllocator{})
                      .take_value();
    const std::uint64_t bits =
        (width == 8 ? UINT64_C(0xdeadbeef00000000) : 0) + 100 + i;
    std::memcpy(writer.data(), &bits, width);
    values.push_back(std::move(writer).publish().take_value());
    boxes.push_back(region);
    oracle[sources[i]] = bits;
  }
  auto coverage = Footprint::from_regions(shape, boxes).take_value();
  auto input = Tensor::create(descriptor, coverage, values).take_value();
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
  std::array<ps_native_binding_v2, 4> buffers{};
  const std::uint8_t* pointers[]{atlas.payload.bytes().data(),
                                 atlas.directory.bytes().data(),
                                 indices.data()};
  const std::uint64_t sizes[]{atlas.payload.bytes().size(),
                              atlas.directory.bytes().size(), indices.size(),
                              output.size()};
  std::array<ps_kernel_view_spec_v8, 4> specifications{};
  std::array<ps_kernel_const_view_v8, 3> reads{};
  for (unsigned i = 0; i < 4; ++i) {
    buffers[i] = {i, i == 3 ? PS_NATIVE_OUTPUT_V2 : PS_NATIVE_INPUT_V2,
                  i == 3 ? 0 : i, 0, sizes[i]};
    auto& spec = specifications[i];
    spec.domain = PS_KERNEL_DOMAIN_SHARED_V8;
    spec.bytes = sizes[i];
    spec.layout.element_type = static_cast<std::uint32_t>(ElementType::UInt8);
    spec.layout.rank = 1;
    spec.layout.shape[0] = sizes[i];
    spec.layout.strides[0] = 1;
    if (i < 3) {
      reads[i].allocation = pointers[i];
      reads[i].bytes = sizes[i];
      reads[i].domain = spec.domain;
      reads[i].layout = &spec.layout;
    }
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
  ps_native_program_v2 command{};
  command.struct_size = sizeof(command);
  command.source = {shader, sizeof(shader) - 1};
  command.entry = {"lookup", 6};
  command.bindings = buffers.data();
  command.binding_count = buffers.size();
  command.constants = config.data();
  command.constant_bytes = sizeof(config);
  command.constant_index = 4;
  command.grid[0] = queries.size();
  command.grid[1] = command.grid[2] = 1;
  ps_kernel_preparation_v8 preparation{};
  preparation.struct_size = sizeof(preparation);
  preparation.placement.backend = PS_BACKEND_GPU_V8;
  preparation.placement.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  preparation.reads = specifications.data();
  preparation.read_count = 3;
  preparation.writes = &specifications[3];
  preparation.write_count = 1;
  test::NativeKernel kernel;
  PS_REQUIRE_OK(kernel.prepare(device, command, preparation));
  ps_kernel_mutable_view_v8 write{};
  write.allocation = output.data();
  write.bytes = output.size();
  write.domain = PS_KERNEL_DOMAIN_SHARED_V8;
  write.layout = &specifications[3].layout;
  ps_kernel_invocation_v8 invocation{};
  invocation.inputs = reads.data();
  invocation.input_count = reads.size();
  const std::array<std::uint64_t, 3> identities{1, 2, 3};
  invocation.input_object_ids = identities.data();
  invocation.outputs = &write;
  invocation.output_count = 1;
  PS_REQUIRE_OK(kernel.run(invocation));
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
  if (!device || !device->available() ||
      device->backend() != PS_NATIVE_METAL_V2) {
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
