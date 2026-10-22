#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/perlin_metal.hpp"
#include "03-generation/runtime.hpp"
#include "common/registry.hpp"
#include "photospider/plugin/native_kernel_api.h"

namespace ps::plugin_internal::generation {
namespace {
constexpr const char* kNativeContract = "perlin2002-native-exact-v1";
struct NativePerlin final : Program {
  bool input_small, output_small;
  explicit NativePerlin(const std::vector<OperationMetadata>& inputs,
                        const Parameters& parameters) {
    const auto& source = tensor(inputs[0]);
    auto dimensions = source.sample_shape();
    require(dimensions.size() >= 2 && dimensions.back() == 3 &&
                floating(source.descriptor.element_type),
            "native Perlin requires Float32/64 coordinate triples");
    input_small = source.descriptor.element_type == ElementType::Float32;
    output_small = dtype(parameters) == ElementType::Float32;
    dimensions.pop_back();
    add_output(this, tensor_schema(dtype(parameters), dimensions));
  }
  unsigned words() const { return input_small ? 80 : 544; }
  std::uint64_t scratch(std::uint64_t count) const {
    return std::min(count, UINT64_C(64)) * 17 * (words() + 1) *
           sizeof(std::uint32_t);
  }
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override {
    throw Status{ErrorCode::BackendUnavailable, "native Perlin requires Metal"};
  }
};
std::shared_ptr<const SchemaTemplate> packed_schema(std::uint64_t count,
                                                    bool coordinates) {
  auto schema = tensor_schema(ElementType::Int64,
                              coordinates ? std::vector<std::uint64_t>{count, 3}
                                          : std::vector<std::uint64_t>{count});
  schema.tensors[0].atomic_trailing_axes = coordinates ? 2 : 1;
  return std::make_shared<const SchemaTemplate>(std::move(schema));
}
using Geometry = std::array<std::uint64_t, 17>;
Geometry geometry(const Region& region) {
  Geometry encoded{};
  encoded[0] = region.dimensions().size();
  for (unsigned axis = 0; axis < encoded[0]; ++axis) {
    encoded[1 + axis] = region.dimensions()[axis].offset;
    encoded[9 + axis] = region.dimensions()[axis].extent;
  }
  return encoded;
}
std::uint64_t count(const Geometry& geometry) {
  std::uint64_t result = 1;
  for (unsigned axis = 0; axis < geometry[0]; ++axis) {
    result *= geometry[9 + axis];
  }
  return result;
}
Result<OperationProgram> lower_native(const OperationLowering& query) {
  try {
    if (query.backend != physical::Backend::Gpu) {
      throw Status{ErrorCode::BackendUnavailable,
                   "strict_gpu requires NativeGpu execution mode"};
    }
    const auto& state = *static_cast<const NativePerlin*>(query.static_state);
    const auto input_grid = take(TileGrid::from_schema(
        *state.inputs[0].result_schema, query.tile_extent, 0));
    const auto output_grid = take(TileGrid::from_schema(
        *state.outputs[0].result_schema, query.tile_extent, 0));
    OperationProgramBuilder builder(query);
    for (const auto& request : query.requests[0].members) {
      for (auto tile : request.second) {
        const auto encoded = geometry(take(output_grid.region(tile)));
        const auto n = count(encoded);
        require(n <= UINT32_MAX,
                "native Perlin tile exceeds dispatch capacity");
        // A conservative integer-operation bound is checked before device
        // issue. It is independent of payload and does not silently reduce
        // precision.
        const auto work = static_cast<unsigned __int128>(n) * state.words() *
                          state.words() * 256;
        if (work > state.work_limit) {
          throw Status{ErrorCode::ResourceExhausted,
                       "native Perlin arithmetic work limit",
                       FailureReason::WorkLimit};
        }
        const auto coords = packed_schema(n, true),
                   values = packed_schema(n, false);
        const auto pack =
            take(builder.add_task("perlin_pack", kNativeContract));
        auto& parameters = builder.parameters(pack);
        parameters.resize(sizeof(encoded));
        std::memcpy(parameters.data(), encoded.data(), sizeof(encoded));
        // Whole means every original coordinate tile is a declared dependency.
        for (TileIndex input = 0; input < input_grid.tile_count(); ++input) {
          check(builder.read_input(pack, 0, 0, input));
        }
        const auto host_coordinates = take(builder.add_temporary(coords));
        const auto packed =
            take(builder.write_temporary(pack, host_coordinates, 0, 0));
        const auto upload = take(builder.add_copy(
            {physical::Backend::Cpu, physical::Domain::Shared, "cpu"}));
        check(builder.read_tile(upload, packed));
        const auto shared_coordinates = take(builder.add_temporary(coords));
        const auto uploaded =
            take(builder.write_temporary(upload, shared_coordinates, 0, 0));
        const auto dispatch = take(
            builder.add_task("perlin_metal", kNativeContract,
                             {physical::Backend::Gpu, physical::Domain::Shared,
                              "perlin2002-exact"},
                             {0, 0, state.scratch(n)}));
        check(builder.read_tile(dispatch, uploaded));
        const auto shared_values = take(builder.add_temporary(values));
        const auto generated =
            take(builder.write_temporary(dispatch, shared_values, 0, 0));
        const auto download = take(builder.add_copy());
        check(builder.read_tile(download, generated));
        const auto host_values = take(builder.add_temporary(values));
        const auto downloaded =
            take(builder.write_temporary(download, host_values, 0, 0));
        const auto scatter =
            take(builder.add_task("perlin_scatter", kNativeContract));
        check(builder.read_tile(scatter, downloaded));
        check(builder.write_output(scatter, 0, request.first, tile));
      }
    }
    return builder.finish();
  } catch (const Status& status) {
    return Result<OperationProgram>(status);
  }
}
struct Packing final {
  Kernel kernel;
  Geometry geometry{};
  bool scatter = false;
};
void packing_invoke(const ps_kernel_invocation_v8* invocation,
                    ps_kernel_status_v8* status) noexcept {
  try {
    const auto& state =
        *static_cast<const Packing*>(invocation->prepared_context);
    const auto& kernel = state.kernel;
    const Work work{*invocation, kernel.program->work_limit};
    work.poll();
    if (state.scatter) {
      const auto* input =
          static_cast<const std::uint8_t*>(invocation->inputs[0].allocation) +
          invocation->inputs[0].byte_offset;
      samples(invocation->outputs[0],
              [&](const Coordinate& at, std::uint64_t index) {
                work.poll();
                std::uint64_t value;
                std::memcpy(&value, input + index * 8, 8);
                store(invocation->outputs[0], at, value);
              });
    } else {
      const Reader reader{kernel, *invocation};
      for (std::uint64_t row = 0, n = count(state.geometry); row < n; ++row) {
        work.poll();
        auto remaining = row;
        Coordinate at{};
        for (unsigned axis = state.geometry[0]; axis-- > 0;) {
          at[axis] =
              state.geometry[1 + axis] + remaining % state.geometry[9 + axis];
          remaining /= state.geometry[9 + axis];
        }
        for (unsigned axis = 0; axis < 3; ++axis) {
          at[state.geometry[0]] = axis;
          store(invocation->outputs[0], {row, axis}, reader.bits(0, 0, at));
        }
      }
    }
    *status = {};
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "native Perlin packing failed");
  }
}
ps_kernel_view_spec_v8 native_view(const KernelView& view) {
  return {view.port,
          view.member,
          static_cast<std::uint32_t>(view.domain),
          view.tile_index,
          view.bytes,
          view.layout.byte_offset,
          kernel_layout(view.layout)};
}
void packed_view(const KernelView& view, std::uint64_t n, bool coordinates,
                 physical::Domain domain) {
  require(view.domain == domain && view.member == 0 && view.tile_index == 0 &&
              view.layout.element_type == ElementType::Int64 &&
              !view.layout.byte_offset &&
              view.layout.rank == (coordinates ? 2U : 1U) &&
              !view.layout.origin[0] && view.layout.shape[0] == n &&
              view.bytes == n * (coordinates ? 24 : 8) &&
              view.layout.strides[0] == (coordinates ? 24 : 8),
          "native Perlin packed layout mismatch");
  if (coordinates) {
    require(view.layout.shape[1] == 3 && !view.layout.origin[1] &&
                view.layout.strides[1] == 8,
            "native Perlin triple layout mismatch");
  }
}
Result<ps_prepared_kernel_v8> prepare_native(
    const physical::KernelDesc& description,
    const physical::Placement& placement, const KernelPreparation& query,
    const ps_native_gpu_service_v2* service) {
  using Answer = Result<ps_prepared_kernel_v8>;
  try {
    require(query.static_state && description.contract == kNativeContract &&
                query.writes.size() == 1,
            "native Perlin kernel contract mismatch");
    const auto& state = *static_cast<const NativePerlin*>(query.static_state);
    if (description.symbol == "perlin_metal") {
      if (!service || service->struct_size != sizeof(*service) ||
          service->abi_version != PS_NATIVE_GPU_ABI_VERSION_2 ||
          service->backend != PS_NATIVE_METAL_V2) {
        throw Status{ErrorCode::BackendUnavailable,
                     "strict GPU Perlin requires the native Metal service"};
      }
      require(placement.backend == physical::Backend::Gpu &&
                  placement.domain == physical::Domain::Shared &&
                  query.reads.size() == 1 && query.task_parameters.empty(),
              "native Perlin dispatch signature mismatch");
      const auto n = query.writes[0].layout.shape[0];
      require(n && n <= UINT32_MAX, "native Perlin dispatch extent");
      packed_view(query.reads[0], n, true, physical::Domain::Shared);
      packed_view(query.writes[0], n, false, physical::Domain::Shared);
      require(
          query.scratch == std::array<std::uint64_t, 3>{0, 0, state.scratch(n)},
          "native Perlin scratch mismatch");
      const auto read = native_view(query.reads[0]),
                 write = native_view(query.writes[0]);
      const std::uint32_t config[] = {
          static_cast<std::uint32_t>(n), state.input_small, state.output_small,
          static_cast<std::uint32_t>(std::min(n, UINT64_C(64)))};
      const ps_native_binding_v2 bindings[] = {
          {0, PS_NATIVE_INPUT_V2, 0, 0, read.bytes},
          {1, PS_NATIVE_OUTPUT_V2, 0, 0, write.bytes},
          {3, PS_NATIVE_SCRATCH_V2, 2, 0, state.scratch(n)}};
      const std::string source = "#define PS_PERLIN_WORDS " +
                                 std::to_string(state.words()) + "u\n" +
                                 perlin_metal_source;
      ps_native_program_v2 program{};
      program.struct_size = sizeof(program);
      program.code_format = PS_NATIVE_MSL_V2;
      program.source = {source.data(),
                        static_cast<std::uint32_t>(source.size())};
      constexpr const char* entry = "perlin2002_exact_bits";
      program.entry = {entry, static_cast<std::uint32_t>(std::strlen(entry))};
      program.bindings = bindings;
      program.binding_count = 3;
      program.constants = config;
      program.constant_bytes = sizeof(config);
      program.constant_index = 2;
      program.grid[0] = config[3];
      program.grid[1] = program.grid[2] = 1;
      ps_kernel_preparation_v8 preparation{};
      preparation.struct_size = sizeof(preparation);
      preparation.symbol = {
          description.symbol.data(),
          static_cast<std::uint32_t>(description.symbol.size())};
      preparation.contract = {
          description.contract.data(),
          static_cast<std::uint32_t>(description.contract.size())};
      preparation.placement.backend = PS_BACKEND_GPU_V8;
      preparation.placement.domain = PS_KERNEL_DOMAIN_SHARED_V8;
      preparation.tile_extent = query.tile_extent;
      preparation.reads = &read;
      preparation.read_count = 1;
      preparation.writes = &write;
      preparation.write_count = 1;
      preparation.scratch[2] = state.scratch(n);
      ps_prepared_kernel_v8 result{};
      const auto status = kernel_status(
          service->prepare(service->context, &program, &preparation, &result));
      if (!status.ok()) {
        if (result.release) {
          result.release(result.context);
        }
        return Answer(status);
      }
      return Answer(result);
    }
    require(placement.backend == physical::Backend::Cpu &&
                placement.domain == physical::Domain::Host &&
                query.scratch == std::array<std::uint64_t, 3>{},
            "native Perlin packing placement mismatch");
    auto packing = std::make_unique<Packing>();
    packing->kernel.program = &state;
    if (description.symbol == "perlin_pack") {
      require(query.task_parameters.size() == sizeof(Geometry),
              "native Perlin geometry encoding");
      std::memcpy(packing->geometry.data(), query.task_parameters.data(),
                  sizeof(Geometry));
      require(packing->geometry[0] > 0 && packing->geometry[0] <= 7,
              "native Perlin output rank");
      const auto dimensions =
          state.outputs[0].result_schema->tensors[0].sample_shape();
      require(packing->geometry[0] == dimensions.size(),
              "native Perlin geometry rank mismatch");
      for (unsigned axis = 0; axis < dimensions.size(); ++axis) {
        require(packing->geometry[9 + axis] &&
                    packing->geometry[1 + axis] < dimensions[axis] &&
                    packing->geometry[9 + axis] <=
                        dimensions[axis] - packing->geometry[1 + axis],
                "native Perlin geometry bounds");
      }
      const auto n = count(packing->geometry);
      packed_view(query.writes[0], n, true, physical::Domain::Host);
      const auto grid = take(TileGrid::from_schema(
          *state.inputs[0].result_schema, query.tile_extent, 0));
      require(query.reads.size() == grid.tile_count(),
              "native Perlin whole dependency mismatch");
      packing->kernel.input_base.push_back(0);
      packing->kernel.groups.push_back(
          {0,
           0,
           grid,
           state.input_small ? ElementType::Float32 : ElementType::Float64,
           {}});
      for (unsigned i = 0; i < query.reads.size(); ++i) {
        const auto& view = query.reads[i];
        require(view.port == 0 && view.member == 0 && view.tile_index == i &&
                    view.domain == physical::Domain::Host &&
                    fixed_tensor::matches_region(view.layout,
                                                 take(grid.region(i))) &&
                    view.layout.element_type == packing->kernel.groups[0].type,
                "native Perlin whole input layout");
        packing->kernel.groups[0].slots.emplace_back(i, i);
      }
    } else {
      require(description.symbol == "perlin_scatter" &&
                  query.reads.size() == 1 && query.task_parameters.empty(),
              "native Perlin scatter signature");
      packing->scatter = true;
      const auto& output = query.writes[0];
      require(output.port == 0 && output.member == 0 &&
                  output.domain == physical::Domain::Host &&
                  output.layout.element_type == (state.output_small
                                                     ? ElementType::Float32
                                                     : ElementType::Float64),
              "native Perlin scatter output");
      const auto grid = take(TileGrid::from_schema(
          *state.outputs[0].result_schema, query.tile_extent, 0));
      const auto region = take(grid.region(output.tile_index));
      require(fixed_tensor::matches_region(output.layout, region),
              "native Perlin scatter region");
      packed_view(query.reads[0], count(geometry(region)), false,
                  physical::Domain::Host);
    }
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context = packing.release();
    result.invoke_cpu = packing_invoke;
    result.release = [](const void* context) {
      delete static_cast<const Packing*>(context);
    };
    return Answer(result);
  } catch (const Status& failure) {
    return Answer(failure);
  }
}
}  // namespace
}  // namespace ps::plugin_internal::generation
namespace ps::plugin_internal {
Status register_perlin_gpu(OperationRegistry* registry) {
  using generation::lower_native;
  using generation::NativePerlin;
  using generation::operation;
  using generation::prepare_native;
  using generation::tensor_schema;
  try {
    auto definition =
        operation("noise.perlin2002_3d_v1_strict_gpu", 1,
                  {{"dtype", OperationParameterType::String, false}},
                  {{"values", tensor_schema(ElementType::Float64, {1})}},
                  [](const auto& inputs, const auto& parameters) {
                    return std::make_shared<NativePerlin>(inputs, parameters);
                  });
    definition.traits.supports_gpu = true;
    definition.lower = lower_native;
    definition.prepare_kernel = [](const auto& kernel, const auto& placement,
                                   const auto& query, const ResourceSet&,
                                   const ps_native_gpu_service_v2* service) {
      return prepare_native(kernel, placement, query, service);
    };
    return registry->register_operation(std::move(definition));
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
