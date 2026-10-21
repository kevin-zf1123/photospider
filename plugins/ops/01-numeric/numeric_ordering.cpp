#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_quantile.hpp"
#include "01-numeric/stable_order.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
struct OrderingMetadata final {
  unsigned axis = 0;
  TensorDescriptor source, output;
};
Result<OrderingMetadata> metadata(
    bool quantile, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<OrderingMetadata>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid ordering input schema"});
  };
  if (inputs.size() != (quantile ? 2U : 1U))
    return invalid();
  for (const auto& input : inputs) {
    auto status = tensor_ops::check_tensor(input);
    if (!status.ok())
      return Answer(status);
  }
  const auto& tensor = inputs[0].result_schema->tensors[0];
  OrderingMetadata result;
  result.source = {tensor.descriptor.element_type, tensor.sample_shape()};
  const auto type = result.source.element_type;
  if ((type != ElementType::UInt8 && type != ElementType::Int64 &&
       type != ElementType::Float32 && type != ElementType::Float64) ||
      result.source.shape.empty() || result.source.shape.size() > 8)
    return invalid();
  std::uint64_t count = 1;
  for (auto extent : result.source.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      return invalid();
    count *= extent;
  }
  const auto axis = std::get<std::int64_t>(parameters.at("axis"));
  if (axis < 0 ||
      static_cast<std::uint64_t>(axis) >= result.source.shape.size())
    return Answer(
        numeric_ops::array_parameter_error("ordering axis outside rank"));
  result.axis = static_cast<unsigned>(axis);
  result.output = result.source;
  if (quantile) {
    const auto& q = inputs[1].result_schema->tensors[0];
    if (q.sample_shape() != std::vector<std::uint64_t>{1} ||
        (q.descriptor.element_type != ElementType::Float32 &&
         q.descriptor.element_type != ElementType::Float64))
      return invalid();
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype != "float32" && dtype != "float64")
      return Answer(numeric_ops::array_parameter_error(
          "quantile requires floating dtype"));
    result.output.element_type =
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
    result.output.shape[result.axis] = 1;
  }
  return Answer(std::move(result));
}
SchemaTemplate output_schema(const OrderingMetadata& description,
                             unsigned port) {
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = description.output;
  if (port)
    schema.tensors[0].descriptor.element_type = ElementType::Int64;
  return schema;
}
using GroupKey = std::array<std::uint64_t, 16>;
GroupKey group_key(const Region& region, unsigned selected) {
  GroupKey key{};
  for (unsigned axis = 0; axis < region.rank(); ++axis)
    if (axis != selected) {
      key[axis] = region.dimensions()[axis].offset;
      key[8 + axis] = region.dimensions()[axis].extent;
    }
  return key;
}
Region source_region(const Region& output,
                     const OrderingMetadata& description) {
  auto dimensions = output.dimensions();
  dimensions[description.axis] = {0,
                                  description.source.shape[description.axis]};
  return Region(std::move(dimensions));
}
std::uint64_t ordering_scratch(bool quantile, std::uint64_t count) {
  static_assert(sizeof(numeric_ops::StableOrderWorkspace) %
                    alignof(numeric_ops::ExactQuantile) ==
                0);
  // count <= 2^40, so both word buffers and fixed state fit uint64_t.
  return sizeof(numeric_ops::StableOrderWorkspace) +
         (quantile ? sizeof(numeric_ops::ExactQuantile) : 0) + count * 16;
}
Result<OperationProgram> lower_ordering(bool quantile,
                                        const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto parsed = metadata(quantile, query.inputs, query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  const auto& description = parsed.value();
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  if (!input.ok())
    return Answer(input.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  using Write = std::pair<unsigned, TileIndex>;
  std::map<GroupKey, std::vector<Write>> groups;
  for (unsigned port = 0; port < query.requests.size(); ++port)
    for (const auto& member : query.requests[port].members)
      for (auto tile : member.second) {
        auto charged = budget.consume({1});
        if (!charged.ok())
          return Answer(charged);
        auto region = output.value().region(tile);
        if (!region.ok())
          return Answer(region.status());
        groups[group_key(region.value(), description.axis)].push_back(
            {port, tile});
      }
  OperationProgramBuilder builder(query);
  const auto count = description.source.shape[description.axis];
  for (const auto& item : groups) {
    auto task = builder.add_task(quantile ? "quantile" : "sort",
                                 "ordering-unchecked-v2", {},
                                 {ordering_scratch(quantile, count), 0, 0});
    if (!task.ok())
      return Answer(task.status());
    const auto region = output.value().region(item.second.front().second);
    if (!region.ok())
      return Answer(region.status());
    auto reads = sdk::tiles_for_region(
        budget, input.value(), source_region(region.value(), description));
    if (!reads.ok())
      return Answer(reads.status());
    for (auto tile : reads.value()) {
      auto status = budget.consume({1});
      if (status.ok())
        status = builder.read_input(task.value(), 0, 0, tile);
      if (!status.ok())
        return Answer(status);
    }
    if (quantile) {
      auto status = count == 1 ? builder.read_input_schema(task.value(), 1)
                               : builder.read_input(task.value(), 1, 0, 0);
      if (!status.ok())
        return Answer(status);
    }
    for (const auto& write : item.second) {
      auto status =
          builder.write_output(task.value(), write.first, 0, write.second);
      if (!status.ok())
        return Answer(status);
    }
  }
  return builder.finish();
}
struct OrderingWrite final {
  unsigned port;
  Region region;
};
template <std::size_t Ports>
struct PreparedOrdering final {
  SequenceProfile profile;
  OrderingMetadata description;
  Region source;
  ElementType probability_type = ElementType::Float64;
  fixed_tensor::PointInputs<Ports> inputs;
  std::vector<OrderingWrite> writes;
};
template <std::size_t Ports>
void ordering_invoke(const ps_kernel_invocation_v8* invocation,
                     ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    constexpr bool quantile = Ports == 2;
    const auto& prepared = *static_cast<const PreparedOrdering<Ports>*>(
        invocation->prepared_context);
    const auto& description = prepared.description;
    const auto axis = description.axis;
    const auto count = description.source.shape[axis];
    const auto type = description.source.element_type;
    const auto source_width = TensorSpan::element_size(type);
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    const auto require = [](Status checked) {
      if (!checked.ok())
        throw checked;
    };
    require(consume(1));
    fixed_tensor::PointReader<Ports> reader{prepared.inputs, *invocation};
    numeric_ops::QuantilePosition position;
    if constexpr (quantile) {
      if (count > 1) {
        position = numeric_ops::quantile_position(
            reader.bits(1, {},
                        TensorSpan::element_size(prepared.probability_type)),
            prepared.probability_type, count);
      }
    }
    auto* scratch =
        static_cast<std::uint8_t*>(invocation->scratch[0].allocation);
    using Order = numeric_ops::StableOrderWorkspace;
    static_assert(alignof(Order) <= alignof(std::max_align_t));
    static_assert(alignof(numeric_ops::ExactQuantile) <=
                  alignof(std::max_align_t));
    auto* order = new (scratch) Order{};
    const auto destroy_order = [](Order* value) { value->~Order(); };
    std::unique_ptr<Order, decltype(destroy_order)> order_guard(order,
                                                                destroy_order);
    scratch += sizeof(Order);
    const auto destroy_math = [](numeric_ops::ExactQuantile* value) {
      value->~ExactQuantile();
    };
    std::unique_ptr<numeric_ops::ExactQuantile, decltype(destroy_math)>
        arithmetic(nullptr, destroy_math);
    if constexpr (quantile) {
      arithmetic.reset(new (scratch)
                           numeric_ops::ExactQuantile(prepared.profile));
      scratch += sizeof(numeric_ops::ExactQuantile);
    }
    numeric_ops::OrderWords permutation{scratch}, keys{scratch + count * 8};
    std::array<std::uint64_t, 8> at{};
    const auto& dimensions = prepared.source.dimensions();
    std::uint64_t lines = 1;
    for (unsigned j = 0; j < dimensions.size(); ++j) {
      at[j] = dimensions[j].offset;
      if (j != axis)
        lines *= dimensions[j].extent;
    }
    for (std::uint64_t line = 0; line < lines; ++line) {
      bool has_nan = false;
      std::uint64_t first_nan = 0;
      const auto read = [&](std::uint64_t index, std::uint64_t* bits) {
        at[axis] = index;
        *bits = reader.bits(0, at, source_width);
        if constexpr (quantile) {
          if (!has_nan &&
              (type == ElementType::Float32 || type == ElementType::Float64) &&
              numeric_ops::BinaryParts::decode(*bits,
                                               type == ElementType::Float32)
                  .nan) {
            first_nan = *bits;
            has_nan = true;
          }
        }
        return Status::success();
      };
      require(order->build(count, type, prepared.profile, permutation, keys,
                           consume, read));
      std::uint64_t quantile_bits = 0;
      if constexpr (quantile) {
        if (has_nan) {
          quantile_bits = numeric_ops::converted_nan(
              first_nan, type, description.output.element_type);
        } else {
          std::uint64_t a = 0, b = 0;
          require(read(permutation.get(position.index), &a));
          if (position.fractional())
            require(read(permutation.get(position.index + 1), &b));
          auto result = arithmetic->finish(
              a, b, type, description.output.element_type, position, consume);
          require(result.status());
          quantile_bits = result.value();
        }
      }
      for (unsigned slot = 0; slot < prepared.writes.size(); ++slot) {
        const auto& write = prepared.writes[slot];
        const auto& output = invocation->outputs[slot];
        const auto width = TensorSpan::element_size(
            write.port ? ElementType::Int64 : description.output.element_type);
        const auto range = write.region.dimensions()[axis];
        for (std::uint64_t index = range.offset;
             index < range.offset + range.extent; ++index) {
          require(consume(1));
          std::uint64_t bits = quantile_bits;
          if constexpr (!quantile) {
            bits = permutation.get(index);
            if (!write.port)
              require(read(bits, &bits));
          }
          at[axis] = index;
          std::uint64_t offset = output.byte_offset;
          for (unsigned j = 0; j < dimensions.size(); ++j)
            offset +=
                (at[j] - output.layout->origin[j]) * output.layout->strides[j];
          std::memcpy(static_cast<std::uint8_t*>(output.allocation) + offset,
                      &bits, width);
        }
      }
      at[axis] = 0;
      for (unsigned j = static_cast<unsigned>(dimensions.size()); j-- > 0;)
        if (j != axis) {
          if (++at[j] < dimensions[j].offset + dimensions[j].extent)
            break;
          at[j] = dimensions[j].offset;
        }
    }
    *status = {};
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "ordering arithmetic failed");
  }
}
template <std::size_t Ports>
Result<ps_prepared_kernel_v8> prepare_ordering(SequenceProfile profile,
                                               const KernelPreparation& query) {
  constexpr bool quantile = Ports == 2;
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid ordering invocation"});
  };
  if (query.outputs.size() != (quantile ? 1U : 2U) || query.writes.empty())
    return invalid();
  auto parsed = metadata(quantile, query.inputs, query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  auto state = std::make_unique<PreparedOrdering<Ports>>();
  state->profile = profile;
  state->description = parsed.take_value();
  const auto& description = state->description;
  for (unsigned port = 0; port < query.outputs.size(); ++port)
    if (!query.outputs[port].result_schema ||
        !query.outputs[port].result_schema->same_schema(
            output_schema(description, port)))
      return invalid();
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  GroupKey key{};
  for (std::size_t slot = 0; slot < query.writes.size(); ++slot) {
    const auto& write = query.writes[slot];
    auto region = output.value().region(write.tile_index);
    if (!region.ok())
      return Answer(region.status());
    const auto current = group_key(region.value(), description.axis);
    if (!slot) {
      key = current;
      state->source = source_region(region.value(), description);
    }
    if (write.port >= query.outputs.size() || write.member ||
        write.domain != physical::Domain::Host ||
        write.layout.element_type != (write.port
                                          ? ElementType::Int64
                                          : description.output.element_type) ||
        !fixed_tensor::matches_region(write.layout, region.value()) ||
        current != key ||
        (slot && std::make_pair(query.writes[slot - 1].port,
                                query.writes[slot - 1].tile_index) >=
                     std::make_pair(write.port, write.tile_index)))
      return invalid();
    state->writes.push_back({write.port, region.take_value()});
  }
  const auto count = description.source.shape[description.axis];
  if (!query.task_parameters.empty() ||
      query.scratch !=
          std::array<std::uint64_t, 3>{ordering_scratch(quantile, count), 0, 0})
    return invalid();
  std::array<Region, Ports> regions;
  regions[0] = state->source;
  if constexpr (quantile) {
    state->probability_type =
        query.inputs[1].result_schema->tensors[0].descriptor.element_type;
    regions[1] = count == 1 ? Region({{0, 0}}) : Region::whole({1});
  }
  auto valid =
      fixed_tensor::prepare_input_regions(query, &state->inputs, regions);
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result.context = state.release();
  result.invoke_cpu = ordering_invoke<Ports>;
  result.release = [](const void* context) {
    delete static_cast<const PreparedOrdering<Ports>*>(context);
  };
  return Answer(result);
}
OperationDefinition ordering_operation(const std::string& key, bool quantile,
                                       SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = quantile ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  traits.input_schema[0].element_type_mask = 15;
  if (quantile) {
    traits.input_schema[1].rank = 1;
    traits.input_schema[1].element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64}};
  if (quantile)
    traits.parameter_schema.push_back(
        {"dtype", OperationParameterType::String});
  traits.outputs.resize(quantile ? 1 : 2);
  for (unsigned port = 0; port < traits.outputs.size(); ++port) {
    traits.outputs[port].key = port ? "indices" : "values";
    traits.outputs[port].output_schema = tensor_ops::port();
    traits.outputs[port].result_schema = tensor_ops::scalar_schema();
  }
  operation.specialize_metadata = [quantile, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto parsed = metadata(quantile, inputs, parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    std::vector<OperationOutputSpecialization> results(quantile ? 1 : 2);
    for (unsigned port = 0; port < results.size(); ++port)
      results[port].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(
              output_schema(parsed.value(), port));
    return Answer(std::move(results));
  };
  operation.lower = [quantile](const OperationLowering& query) {
    return lower_ordering(quantile, query);
  };
  operation.prepare_kernel =
      [quantile, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != (quantile ? "quantile" : "sort") ||
        kernel.contract != "ordering-unchecked-v2")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid ordering kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable,
           "ordering requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Result<ps_prepared_kernel_v8>(available);
    return quantile ? prepare_ordering<2>(profile, query)
                    : prepare_ordering<1>(profile, query);
  };
  return operation;
}
}  // namespace
Status register_numeric_ordering(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool quantile : {false, true}) {
      auto status = registry->register_operation(ordering_operation(
          std::string(quantile ? "numeric.quantile" : "array.sort") +
              profile.first,
          quantile, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
