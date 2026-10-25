#include "03-generation/runtime.hpp"

#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ps::plugin_internal::generation {
ResultTensorSpec member(const char* key, ElementType type,
                        const std::vector<std::uint64_t>& shape,
                        unsigned atomic, int component) {
  size_product(shape);
  ResultTensorSpec result;
  result.key = key;
  result.descriptor = {type, shape};
  result.atomic_trailing_axes = atomic;
  result.layout.channel_axis = component < 0
                                   ? std::optional<std::uint32_t>{}
                                   : std::optional<std::uint32_t>(component);
  return result;
}
SchemaTemplate tensor_schema(ElementType type,
                             const std::vector<std::uint64_t>& shape,
                             bool spatial, bool components) {
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  schema.tensors.push_back(
      member("samples", type, shape, components ? 1 : 0,
             components ? static_cast<int>(shape.size()) - 1 : -1));
  schema.tensors[0].layout.spatial = spatial;
  check(schema.validate());
  return schema;
}
SchemaTemplate bundle(const char* id, std::vector<ResultTensorSpec> members) {
  SchemaTemplate schema;
  schema.id = id;
  ResultPublicationScope scope;
  scope.policy = ResultPublicationPolicy::CompleteBundle;
  for (unsigned i = 0; i < members.size(); ++i) {
    schema.tensors.push_back(std::move(members[i]));
    scope.members.push_back(i);
  }
  schema.publication_scopes.push_back(std::move(scope));
  check(schema.validate());
  return schema;
}
std::vector<ReadRegion> Program::support(unsigned, const Region&) const {
  std::vector<ReadRegion> result;
  for (unsigned p = 0; p < inputs.size(); ++p) {
    for (unsigned m = 0; m < inputs[p].result_schema->tensors.size(); ++m) {
      result.push_back(
          {p, m,
           Region::whole(inputs[p].result_schema->tensors[m].sample_shape())});
    }
  }
  return result;
}
std::uint64_t Reader::bits(unsigned port, unsigned member,
                           const Coordinate& at) const {
  const auto& group =
      kernel.groups[kernel.input_base[port + input_offset] + member];
  const unsigned slot =
      group.slots.size() == 1
          ? group.slots[0].second
          : std::lower_bound(group.slots.begin(), group.slots.end(),
                             fixed_tensor::input_tile(group.grid, at),
                             [](const auto& entry, TileIndex tile) {
                               return entry.first < tile;
                             })
                ->second;
  const auto& view = invocation.inputs[slot];
  std::uint64_t value = 0;
  std::memcpy(&value,
              static_cast<const std::uint8_t*>(view.allocation) +
                  offset(*view.layout, view.byte_offset, at),
              width(group.type));
  return value;
}
double Reader::number(unsigned port, unsigned member,
                      const Coordinate& at) const {
  const auto value = bits(port, member, at);
  if (kernel.groups[kernel.input_base[port + input_offset] + member].type ==
      ElementType::Float32) {
    const auto raw = static_cast<std::uint32_t>(value);
    float result;
    std::memcpy(&result, &raw, 4);
    return result;
  }
  return generation::number(value);
}
namespace {
constexpr const char* kSymbol = "generation";
constexpr const char* kContract = "generation-unchecked-v1";
using Address = std::tuple<unsigned, unsigned, TileIndex>;
std::vector<Address> reads(const Program& program, unsigned port,
                           const Region& output, std::uint64_t extent,
                           const ResourceBudget& budget,
                           std::uint64_t maximum_records) {
  std::set<Address> selected;
  for (const auto& region : program.support(port, output)) {
    const auto grid = take(TileGrid::from_schema(
        *program.inputs[region.port].result_schema, extent, region.member));
    for (auto tile : take(sdk::tiles_for_region(budget, grid, region.region))) {
      check(budget.consume({1}));
      selected.emplace(region.port, region.member, tile);
      if (selected.size() > maximum_records) {
        throw Status{ErrorCode::ResourceExhausted,
                     "generation input record limit",
                     FailureReason::StageLimit};
      }
    }
  }
  return {selected.begin(), selected.end()};
}
template <class Function>
void each_write(const Program& program, std::uint64_t extent,
                Function function) {
  for (unsigned p = 0; p < program.outputs.size(); ++p) {
    for (unsigned m = 0; m < program.outputs[p].result_schema->tensors.size();
         ++m) {
      const auto grid = take(
          TileGrid::from_schema(*program.outputs[p].result_schema, extent, m));
      for (TileIndex i = 0; i < grid.tile_count(); ++i) {
        function(Address{p, m, i});
      }
    }
  }
}
Result<OperationProgram> lower(const OperationLowering& query) {
  try {
    const auto& state = *static_cast<const Program*>(query.static_state);
    OperationProgramBuilder builder(query);
    ResourceLimits limits;
    limits.maximum_work = query.limits.maximum_work;
    ResourceBudget budget(limits);
    bool requested = false;
    for (const auto& request : query.requests) {
      requested |= !request.empty();
    }
    if (!requested) {
      return builder.finish();
    }
    const auto add = [&](unsigned port, unsigned member, TileIndex tile) {
      Region region;
      if (!state.complete && !state.collective) {
        const auto grid = take(TileGrid::from_schema(
            *state.outputs[port].result_schema, query.tile_extent, member));
        region = take(grid.region(tile));
      }
      const auto task = take(builder.add_task(
          kSymbol, kContract, {}, {state.task_scratch(port, region), 0, 0}));
      for (const auto& read : reads(state, port, region, query.tile_extent,
                                    budget, query.limits.maximum_records)) {
        check(builder.read_input(task, std::get<0>(read), std::get<1>(read),
                                 std::get<2>(read)));
      }
      if (state.complete) {
        each_write(state, query.tile_extent, [&](const Address& write) {
          check(builder.write_output(task, std::get<0>(write),
                                     std::get<1>(write), std::get<2>(write)));
        });
      } else if (state.collective) {
        for (unsigned p = 0; p < query.requests.size(); ++p)
          for (const auto& m : query.requests[p].members)
            for (auto selected : m.second)
              check(builder.write_output(task, p, m.first, selected));
      } else {
        check(builder.write_output(task, port, member, tile));
      }
    };
    if (state.complete || state.collective) {
      add(0, 0, 0);
    } else {
      for (unsigned p = 0; p < query.requests.size(); ++p) {
        for (const auto& m : query.requests[p].members) {
          for (auto tile : m.second) {
            add(p, m.first, tile);
          }
        }
      }
    }
    auto result = builder.finish();
    if (result.ok() && state.complete) {
      auto program = result.take_value();
      program.program.calls[0].returns = program.outputs;
      check(physical::finalize_program(&program.program, query.limits));
      return Result<OperationProgram>(std::move(program));
    }
    return result;
  } catch (const Status& failure) {
    return Result<OperationProgram>(failure);
  }
}
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  try {
    FloatingEnvironment environment;
    if (!environment.active()) {
      throw Status{ErrorCode::OperationFailed,
                   "generation floating environment unavailable"};
    }
    const auto& kernel =
        *static_cast<const Kernel*>(invocation->prepared_context);
    Work{*invocation, kernel.program->work_limit}.poll();
    kernel.program->run(kernel, *invocation);
    *status = {};
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
    status->domain_code = failure.detail.domain_code;
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted,
                             "generation workspace capacity",
                             FailureReason::CapacityLimit);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "generation arithmetic failed");
  }
}
Result<ps_prepared_kernel_v8> prepare(const physical::KernelDesc& description,
                                      const physical::Placement& placement,
                                      const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  try {
    require(query.static_state && description.symbol == kSymbol &&
                description.contract == kContract,
            "generation kernel identity mismatch");
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host) {
      throw Status{ErrorCode::BackendUnavailable,
                   "generation kernel requires CPU Host"};
    }
    const auto& state = *static_cast<const Program*>(query.static_state);
    require(query.task_parameters.empty(),
            "generation task parameters mismatch");
    require(
        query.outputs.size() == state.outputs.size() && !query.writes.empty(),
        "generation output arity mismatch");
    for (unsigned p = 0; p < state.outputs.size(); ++p) {
      require(query.outputs[p].result_schema &&
                  query.outputs[p].result_schema->same_schema(
                      *state.outputs[p].result_schema),
              "generation output schema mismatch");
    }
    const auto& first = query.writes[0];
    const auto scratch_region = state.complete || state.collective
                                    ? Region{}
                                    : fixed_tensor::view_region(first.layout);
    require(query.scratch ==
                std::array<std::uint64_t, 3>{
                    state.task_scratch(first.port, scratch_region), 0, 0},
            "generation task scratch mismatch");
    unsigned write_index = 0;
    const auto validate_write = [&](const Address& expected) {
      require(write_index < query.writes.size(),
              "generation output tile arity mismatch");
      const auto& view = query.writes[write_index++];
      require(Address{view.port, view.member, view.tile_index} == expected &&
                  view.domain == physical::Domain::Host,
              "generation output tile mismatch");
      const auto grid =
          take(TileGrid::from_schema(*state.outputs[view.port].result_schema,
                                     query.tile_extent, view.member));
      require(fixed_tensor::matches_region(
                  view.layout, take(grid.region(view.tile_index))) &&
                  view.layout.element_type ==
                      state.outputs[view.port]
                          .result_schema->tensors[view.member]
                          .descriptor.element_type,
              "generation output layout mismatch");
    };
    if (state.complete) {
      each_write(state, query.tile_extent, validate_write);
    } else if (state.collective) {
      std::set<Address> unique;
      for (const auto& write : query.writes) {
        require(write.port < state.outputs.size() &&
                    write.member <
                        state.outputs[write.port].result_schema->tensors.size(),
                "collective output directory mismatch");
        const Address address{write.port, write.member, write.tile_index};
        require(unique.insert(address).second,
                "duplicate collective output tile");
        validate_write(address);
      }
    } else {
      validate_write(Address{first.port, first.member, first.tile_index});
    }
    require(write_index == query.writes.size(),
            "generation output tile arity mismatch");
    const auto expected_reads =
        reads(state, first.port,
              state.complete || state.collective
                  ? Region{}
                  : fixed_tensor::view_region(first.layout),
              query.tile_extent, ResourceBudget{}, query.reads.size());
    require(query.reads.size() == expected_reads.size(),
            "generation input tile arity mismatch");
    auto kernel = std::make_unique<Kernel>();
    kernel->program = &state;
    for (const auto& view : query.writes) {
      kernel->writes.emplace_back(view.port, view.member);
    }
    for (unsigned p = 0; p < state.inputs.size(); ++p) {
      kernel->input_base.push_back(kernel->groups.size());
      for (unsigned m = 0; m < state.inputs[p].result_schema->tensors.size();
           ++m) {
        kernel->groups.push_back(
            {p,
             m,
             take(TileGrid::from_schema(*state.inputs[p].result_schema,
                                        query.tile_extent, m)),
             state.inputs[p].result_schema->tensors[m].descriptor.element_type,
             {}});
      }
    }
    for (unsigned i = 0; i < expected_reads.size(); ++i) {
      const auto& view = query.reads[i];
      require(Address{view.port, view.member, view.tile_index} ==
                      expected_reads[i] &&
                  view.domain == physical::Domain::Host,
              "generation input tile mismatch");
      auto& group = kernel->groups[kernel->input_base[view.port] + view.member];
      require(view.layout.element_type == group.type &&
                  fixed_tensor::matches_region(
                      view.layout, take(group.grid.region(view.tile_index))),
              "generation input layout mismatch");
      group.slots.emplace_back(view.tile_index, i);
    }
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context = kernel.release();
    result.invoke_cpu = invoke;
    result.release = [](const void* context) {
      delete static_cast<const Kernel*>(context);
    };
    return Answer(result);
  } catch (const Status& failure) {
    return Answer(failure);
  }
}
}  // namespace
OperationDefinition budgeted_operation(OperationDefinition definition) {
  const auto lower = definition.lower;
  definition.lower =
      [lower](const OperationLowering& query) -> Result<OperationProgram> {
    auto result = lower(query);
    if (!result.ok())
      return result;
    auto program = result.take_value();
    for (auto& kernel : program.program.kernels)
      kernel.abi_version = 9;
    auto status = physical::finalize_program(&program.program, query.limits);
    if (!status.ok())
      return Result<OperationProgram>(status);
    return Result<OperationProgram>(std::move(program));
  };
  const auto prepare_v8 = definition.prepare_kernel;
  definition.prepare_kernel_v9 =
      [prepare_v8](const auto& description, const auto& placement,
                   const auto& query, const auto& resources,
                   const auto* native) -> Result<ps_prepared_kernel_v9> {
    auto prepared =
        prepare_v8(description, placement, query, resources, native);
    if (!prepared.ok())
      return Result<ps_prepared_kernel_v9>(prepared.status());
    const auto inner = prepared.take_value();
    ps_prepared_kernel_v9 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_9;
    result.context = inner.context;
    result.release = inner.release;
    result.invoke_cpu = [](const ps_kernel_invocation_v9* invocation,
                           ps_kernel_status_v8* status) {
      try {
        FloatingEnvironment environment;
        if (!environment.active())
          throw Status{ErrorCode::OperationFailed,
                       "generation floating environment unavailable"};
        const auto& kernel =
            *static_cast<const Kernel*>(invocation->views.prepared_context);
        Work{invocation->views, kernel.program->work_limit, 0, invocation->work}
            .poll();
        kernel.program->run_v9(kernel, *invocation);
        *status = {};
      } catch (const Status& failure) {
        *status = kernel_failure(failure.code, failure.message.c_str(),
                                 failure.reason);
        status->domain_code = failure.detail.domain_code;
      } catch (const std::bad_alloc&) {
        *status = kernel_failure(ErrorCode::ResourceExhausted,
                                 "generation workspace capacity",
                                 FailureReason::CapacityLimit);
      } catch (...) {
        *status = kernel_failure(ErrorCode::OperationFailed,
                                 "budgeted generation failed");
      }
    };
    return Result<ps_prepared_kernel_v9>(result);
  };
  // The legacy factory is private to v9 preparation, never an alternative
  // executable path that can silently omit Run numerical admission.
  definition.prepare_kernel = {};
  return definition;
}
OperationPreparation prepared(std::shared_ptr<Program> state) {
  OperationPreparation result;
  for (const auto& output : state->outputs) {
    OperationOutputSpecialization value;
    value.metadata = output;
    result.outputs.push_back(std::move(value));
  }
  result.state = std::move(state);
  return result;
}
OperationDefinition operation(
    const std::string& key, unsigned inputs,
    std::vector<OperationParameterSpec> parameters,
    const std::vector<std::pair<std::string, SchemaTemplate>>& outputs,
    Prepare make, bool repeated, bool deterministic) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = repeated ? 0 : inputs;
  traits.input_schema.resize(repeated ? 1 : inputs);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = 127;
  }
  if (key.find("path.evaluate_core_pathset_") == 0 ||
      key.find("path.fill_polygon_area_") == 0 ||
      key.find("path.evaluate_primitive_pathset_") == 0) {
    traits.input_schema[0].element_type_mask = 0;
    traits.input_schema[0].result_schema_id =
        key.find("path.evaluate_primitive_pathset_") == 0
            ? "photospider.ops.path_primitives"
            : "photospider.ops.path_core";
    traits.input_schema[0].result_schema_version = 1;
  }
  traits.deterministic = deterministic;
  if (repeated) {
    traits.repeated_minimum = 1;
    traits.repeated_maximum = 1024;
    traits.repeated_match = false;
  }
  traits.parameter_schema = std::move(parameters);
  traits.parameter_schema.push_back(
      {"max_work", OperationParameterType::Int64, false});
  traits.outputs.clear();
  for (const auto& output : outputs) {
    OperationOutputTraits value;
    value.key = output.first;
    value.result_schema = output.second;
    value.output_schema.result_schema_id = output.second.id.c_str();
    value.output_schema.result_schema_version = 1;
    traits.outputs.push_back(std::move(value));
  }
  definition.prepare_static =
      [make](const auto& inputs,
             const auto& p) -> Result<OperationPreparation> {
    try {
      auto state = make(inputs, p);
      state->inputs = inputs;
      const auto work = integer(p, "max_work", INT64_MAX);
      if (work < 0) {
        throw Status{ErrorCode::InvalidArgument,
                     "generation max_work must be nonnegative",
                     FailureReason::InvalidDomain};
      }
      state->work_limit = static_cast<std::uint64_t>(work);
      return Result<OperationPreparation>(prepared(std::move(state)));
    } catch (const Status& failure) {
      return Result<OperationPreparation>(failure);
    }
  };
  definition.lower = lower;
  definition.prepare_kernel = [](const auto& kernel, const auto& placement,
                                 const auto& query, const ResourceSet&,
                                 const ps_native_gpu_service_v2*) {
    return prepare(kernel, placement, query);
  };
  return definition;
}
std::vector<OperationParameterSpec> canvas_parameters(bool output_dtype) {
  std::vector<OperationParameterSpec> result{
      {"height", OperationParameterType::Int64},
      {"width", OperationParameterType::Int64},
      {"origin_x", OperationParameterType::Int64, false},
      {"origin_y", OperationParameterType::Int64, false}};
  if (output_dtype) {
    result.push_back({"dtype", OperationParameterType::String, false});
  }
  return result;
}
std::vector<OperationParameterSpec> rng_parameters() {
  return {{"seed", OperationParameterType::Int64},
          {"stream", OperationParameterType::Int64},
          {"frame", OperationParameterType::Int64, false}};
}
void append(std::vector<OperationParameterSpec>* into,
            const std::vector<OperationParameterSpec>& values) {
  into->insert(into->end(), values.begin(), values.end());
}
void add_output(Program* p, SchemaTemplate schema) {
  check(schema.validate());
  p->outputs.push_back(
      {std::make_shared<const SchemaTemplate>(std::move(schema))});
}
}  // namespace ps::plugin_internal::generation
