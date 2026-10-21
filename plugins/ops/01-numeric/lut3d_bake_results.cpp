#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_bake_error.hpp"
#include "01-numeric/lut3d_bake_common.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"
#include "photospider/ops/errors.hpp"

namespace ps::plugin_internal {
namespace {
using namespace bake_ops;  // NOLINT(build/namespaces)
enum class Kind { Pack, Measure, Unpack, Gate };
Lut3dBakeDescription placeholder() {
  Lut3dBakeDescription result;
  result.recipe_identity.assign(64, '0');
  return result;
}
Result<std::vector<OperationOutputSpecialization>> specialize(
    Kind kind, const std::vector<OperationMetadata>& inputs,
    const Parameters& parameters) {
  using Answer = Result<std::vector<OperationOutputSpecialization>>;
  const unsigned count = kind == Kind::Measure ? 6 : kind == Kind::Gate ? 2 : 1;
  if (inputs.size() != count)
    return Answer(mismatch("bake Result input count"));
  auto decoded = description(parameters);
  if (!decoded.ok())
    return Answer(decoded.status());
  const auto& spec = decoded.value();
  auto table_schema = lut3d_bake_table_schema(spec),
       report_schema = lut3d_bake_schema(spec);
  if (!table_schema.ok() || !report_schema.ok())
    return Answer(!table_schema.ok() ? table_schema.status()
                                     : report_schema.status());
  const TensorDescriptor table{
      spec.table_dtype,
      {spec.shape[0], spec.shape[1], spec.shape[2], 3}};
  OperationOutputSpecialization output;
  if (kind == Kind::Pack) {
    if (!sole_tensor(inputs[0]) ||
        tensor_descriptor(inputs[0]).element_type != table.element_type ||
        tensor_descriptor(inputs[0]).shape != table.shape)
      return Answer(mismatch("owned bake table shape/dtype"));
    auto valid = color_metadata(inputs[0], spec.output_description);
    if (!valid.ok())
      return Answer(valid);
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(table_schema.take_value());
  } else if (kind == Kind::Measure) {
    const auto points = product(spec.shape, 1) + spec.extra_points;
    const std::array<TensorDescriptor, 6> expected{
        TensorDescriptor{ElementType::Float64, {3, 3}},
        TensorDescriptor{ElementType::Float64, table.shape},
        table,
        TensorDescriptor{ElementType::Float64, {points, 3}},
        TensorDescriptor{spec.source_dtype, {points, 3}},
        TensorDescriptor{spec.table_dtype, {points, 3}}};
    for (unsigned port = 0; port < 6; ++port) {
      if (port == 2) {
        if (!inputs[port].result_schema ||
            !inputs[port].result_schema->same_schema(table_schema.value()))
          return Answer(mismatch("measurement owned table schema"));
        continue;
      }
      if (!sole_tensor(inputs[port]) ||
          tensor_descriptor(inputs[port]).element_type !=
              expected[port].element_type ||
          tensor_descriptor(inputs[port]).shape != expected[port].shape)
        return Answer(mismatch("measurement port shape/dtype"));
      if (port) {
        auto checked =
            color_metadata(inputs[port], port < 4 ? spec.input_description
                                                  : spec.output_description);
        if (!checked.ok())
          return Answer(checked);
      }
    }
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(report_schema.take_value());
  } else {
    if (!inputs[0].result_schema ||
        !inputs[0].result_schema->same_schema(table_schema.value()))
      return Answer(mismatch("bake table schema mismatch"));
    if (kind == Kind::Gate &&
        (!inputs[1].result_schema ||
         !inputs[1].result_schema->same_schema(report_schema.value())))
      return Answer(mismatch(
          "gated report must describe this exact bake recipe/dtype/method"));
    auto schema =
        numeric_ops::numeric_tensor_schema(table.element_type, table.shape);
    schema.tensors[0].atomic_trailing_axes = 1;
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
  }
  return Answer(std::vector<OperationOutputSpecialization>{std::move(output)});
}
struct BakeProgram {
  Kind kind;
  std::uint64_t atol, rtol, points;
  std::array<char, 64> configuration;
  std::shared_ptr<const SchemaTemplate> output;
};
struct Measurement {
  Lut3dBakeReport report;
  numeric_ops::ExactBakeError arithmetic;
};
std::uint64_t scratch_bytes(Kind kind) {
  return kind == Kind::Measure ? sizeof(Measurement)
         : kind == Kind::Gate  ? sizeof(numeric_ops::ExactBakeError)
                               : 0;
}
constexpr const char* contract = "bake-result-fixed-v1";
struct ReadRegion {
  unsigned port, member;
  Region region;
};
unsigned member_slot(Kind kind, unsigned port, unsigned member) {
  return kind == Kind::Gate && port == 1 ? 1 + member : port;
}
Result<std::vector<ReadRegion>> source_regions(
    const BakeProgram& state, const std::vector<OperationMetadata>& inputs,
    const Region& output) {
  using Answer = Result<std::vector<ReadRegion>>;
  std::vector<ReadRegion> regions;
  if (state.kind == Kind::Measure) {
    // Complete owned-table readiness is part of the measurement contract.
    // Its views supply identity; points/reference/applied supply the errors.
    for (unsigned port : {0U, 2U, 3U, 4U, 5U})
      regions.push_back(
          {port, 0,
           Region::whole(
               inputs[port].result_schema->tensors[0].sample_shape())});
  } else {
    regions.push_back({0, 0, output});
    if (state.kind == Kind::Gate)
      for (unsigned member : {0U, 7U, 8U, 9U, 10U, 11U, 12U})
        regions.push_back(
            {1, member,
             Region::whole(
                 inputs[1].result_schema->tensors[member].sample_shape())});
  }
  return Answer(std::move(regions));
}
Status check_count(const TileGrid& grid, const Region& region,
                   std::uint64_t maximum) {
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < grid.rank(); ++axis) {
    const auto d = region.dimensions()[axis];
    std::uint64_t extent = 1;
    if (grid.batch_axis_mask() & (1U << axis))
      extent = d.extent;
    else if (axis == grid.width_axis() || axis == grid.height_axis())
      extent = (d.offset + d.extent - 1) / grid.tile_extent() -
               d.offset / grid.tile_extent() + 1;
    if (extent > maximum / count)
      return {ErrorCode::ResourceExhausted, "bake read tile limit"};
    count *= extent;
  }
  return Status::success();
}
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const BakeProgram*>(query.static_state);
  OperationProgramBuilder builder(query);
  if (query.requests[0].empty())
    return builder.finish();
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  const auto add_reads = [&](physical::Id task, const Region& output) {
    auto regions = source_regions(state, query.inputs, output);
    if (!regions.ok())
      return regions.status();
    for (const auto& region : regions.value()) {
      auto grid =
          TileGrid::from_schema(*query.inputs[region.port].result_schema,
                                query.tile_extent, region.member);
      if (!grid.ok())
        return grid.status();
      auto count = check_count(grid.value(), region.region,
                               query.limits.maximum_records);
      if (!count.ok())
        return count;
      auto tiles = sdk::tiles_for_region(budget, grid.value(), region.region);
      if (!tiles.ok())
        return tiles.status();
      for (auto tile : tiles.value()) {
        auto status = budget.consume({1});
        if (status.ok())
          status = builder.read_input(task, region.port, region.member, tile);
        if (!status.ok())
          return status;
      }
    }
    return state.kind == Kind::Measure ? builder.read_input_schema(task, 1)
                                       : Status::success();
  };
  if (state.kind == Kind::Measure) {
    auto task = builder.add_task("bake-result", contract, {},
                                 {scratch_bytes(state.kind), 0, 0});
    if (!task.ok())
      return Answer(task.status());
    auto read = add_reads(task.value(), Region{});
    if (!read.ok())
      return Answer(read);
    for (unsigned member = 0; member < state.output->tensors.size(); ++member) {
      auto grid =
          TileGrid::from_schema(*state.output, query.tile_extent, member);
      if (!grid.ok())
        return Answer(grid.status());
      for (TileIndex tile = 0; tile < grid.value().tile_count(); ++tile) {
        auto status = builder.write_output(task.value(), 0, member, tile);
        if (!status.ok())
          return Answer(status);
      }
    }
  } else {
    auto grid = TileGrid::from_schema(*state.output, query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto region = grid.value().region(tile);
        if (!region.ok())
          return Answer(region.status());
        auto task = builder.add_task("bake-result", contract, {},
                                     {scratch_bytes(state.kind), 0, 0});
        if (!task.ok())
          return Answer(task.status());
        auto status = add_reads(task.value(), region.value());
        if (status.ok())
          status = builder.write_output(task.value(), 0, member.first, tile);
        if (!status.ok())
          return Answer(status);
      }
  }
  auto finished = builder.finish();
  if (!finished.ok() || state.kind != Kind::Measure)
    return finished;
  auto program = finished.take_value();
  program.program.calls[0].returns = program.outputs;
  auto checked = physical::finalize_program(&program.program);
  if (!checked.ok())
    return Answer(checked);
  return Answer(std::move(program));
}
struct BakeKernel {
  const BakeProgram* program;
  fixed_tensor::PointInputs<14> inputs;
  bool source_narrow = false, table_narrow = false;
  unsigned table_slot = 0;
};
using Reader = fixed_tensor::PointReader<14>;
std::uint64_t read_color(Reader& reader, unsigned port, std::uint64_t row,
                         unsigned component, bool narrow) {
  return promote(reader.bits(port, {row, component}, narrow ? 4 : 8), narrow);
}
std::array<const void*, 11> report_fields(const Lut3dBakeReport& report,
                                          const std::uint8_t& passed) {
  return {&passed,
          report.axis.data(),
          &report.validation_count,
          &report.failed_count,
          report.max_abs_error.data(),
          report.max_error_point.data(),
          report.max_error_index.data(),
          &report.first_failure_index,
          report.first_failure_input.data(),
          report.first_failure_reference.data(),
          report.first_failure_lut.data()};
}
bool measure(const BakeKernel& state, const ps_kernel_invocation_v8& invocation,
             Reader& reader, Measurement& scratch, ps_kernel_status_v8* status,
             const core_internal::WorkConsumer& consume) {
  const auto& program = *state.program;
  auto& report = scratch.report;
  auto& arithmetic = scratch.arithmetic;
  const auto accept = [status](const Status& value) {
    if (!value.ok())
      *status = kernel_failure(value.code, nullptr, value.reason);
    return value.ok();
  };
  for (unsigned i = 0; i < 9; ++i) {
    const auto bits = reader.bits(0, {i / 3, i % 3}, 8);
    std::memcpy(&report.axis[i], &bits, 8);
  }
  report.validation_count = program.points;
  for (std::uint64_t row = 0; row < program.points; ++row) {
    if (!accept(consume(9)))
      return false;
    std::array<std::uint64_t, 3> point{}, reference{}, value{};
    bool passed = true;
    for (unsigned c = 0; c < 3; ++c) {
      point[c] = read_color(reader, 3, row, c, false);
      reference[c] = read_color(reader, 4, row, c, state.source_narrow);
      value[c] = read_color(reader, 5, row, c, state.table_narrow);
    }
    for (unsigned c = 0; c < 3; ++c) {
      auto accepted = arithmetic.check(reference[c], value[c], program.atol,
                                       program.rtol, consume);
      if (!accept(accepted.status()))
        return false;
      passed &= accepted.value();
      if (!row ||
          arithmetic.compare(arithmetic.difference, arithmetic.maxima[c]) > 0) {
        arithmetic.maxima[c] = arithmetic.difference;
        report.max_error_index[c] = row;
        std::memcpy(report.max_error_point.data() + 3 * c, point.data(), 24);
      }
    }
    if (!passed) {
      ++report.failed_count;
      if (report.first_failure_index < 0) {
        report.first_failure_index = row;
        std::memcpy(report.first_failure_input.data(), point.data(), 24);
        std::memcpy(report.first_failure_reference.data(), reference.data(),
                    24);
        std::memcpy(report.first_failure_lut.data(), value.data(), 24);
      }
    }
  }
  report.passed = report.failed_count == 0;
  for (unsigned c = 0; c < 3; ++c) {
    auto value = arithmetic.upward(c, consume);
    if (!accept(value.status()))
      return false;
    const auto bits = value.value();
    std::memcpy(&report.max_abs_error[c], &bits, 8);
  }
  const std::uint8_t passed = report.passed;
  auto fields = report_fields(report, passed);
  std::array<std::uint8_t, 8> table_id{};
  for (unsigned i = 0; i < 8; ++i)
    table_id[i] = invocation.input_object_ids[state.table_slot] >> (8 * i);
  for (unsigned slot = 0; slot < invocation.output_count; ++slot) {
    if (!accept(consume(1)))
      return false;
    const auto& output = invocation.outputs[slot];
    const auto member = output.member;
    const void* source =
        member < 11    ? fields[member]
        : member == 11 ? static_cast<const void*>(program.configuration.data())
                       : table_id.data();
    const auto width = member == 0 || member >= 11 ? 1U : 8U;
    const auto& shape = program.output->tensors[member].descriptor.shape;
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < output.layout->rank; ++axis)
      count *= output.layout->shape[axis];
    for (std::uint64_t i = 0; i < count; ++i) {
      auto remaining = i;
      std::uint64_t index = 0, stride = 1;
      for (unsigned axis = output.layout->rank; axis-- > 0;) {
        index += (output.layout->origin[axis] +
                  remaining % output.layout->shape[axis]) *
                 stride;
        remaining /= output.layout->shape[axis];
        stride *= shape[axis];
      }
      std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                      output.byte_offset + i * width,
                  static_cast<const std::uint8_t*>(source) + index * width,
                  width);
    }
  }
  return true;
}
bool gate(const BakeKernel& state, const ps_kernel_invocation_v8& invocation,
          Reader& reader, numeric_ops::ExactBakeError& arithmetic,
          ps_kernel_status_v8* status,
          const core_internal::WorkConsumer& consume) {
  const auto& program = *state.program;
  std::uint64_t measured = 0;
  for (unsigned i = 0; i < 8; ++i)
    measured |= reader.bits(13, {0, i}, 1) << (8 * i);
  bool configured = true;
  for (unsigned i = 0; i < 64; ++i)
    configured &= reader.bits(12, {0, i}, 1) ==
                  static_cast<std::uint8_t>(program.configuration[i]);
  if (!configured) {
    *status = kernel_failure(ErrorCode::TypeMismatch,
                             "bake report configuration mismatch");
    return false;
  }
  if (measured != invocation.input_object_ids[state.table_slot]) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "bake report/table object mismatch");
    status->domain_code = ops_error::InvalidAssociation.value;
    return false;
  }
  if (reader.bits(1, {0}, 1))
    return true;
  const auto index = reader.bits(8, {0}, 8);
  std::array<std::uint64_t, 3> point{}, reference{}, lut{};
  for (unsigned c = 0; c < 3; ++c) {
    point[c] = reader.bits(9, {0, c}, 8);
    reference[c] = reader.bits(10, {0, c}, 8);
    lut[c] = reader.bits(11, {0, c}, 8);
  }
  for (unsigned c = 0; c < 3; ++c) {
    auto accepted = arithmetic.check(reference[c], lut[c], program.atol,
                                     program.rtol, consume);
    if (!accepted.ok()) {
      *status = kernel_failure(accepted.status().code, nullptr,
                               accepted.status().reason);
      return false;
    }
    if (accepted.value())
      continue;
    arithmetic.maxima[0] = arithmetic.difference;
    auto error = arithmetic.upward(0, consume);
    if (!error.ok()) {
      *status =
          kernel_failure(error.status().code, nullptr, error.status().reason);
      return false;
    }
    *status = kernel_failure(ErrorCode::OperationFailed, nullptr,
                             FailureReason::InvalidDomain);
    char* cursor = status->message;
    char* const end = status->message + sizeof(status->message) - 1;
    const auto text = [&](const char* value) {
      while (*value && cursor != end)
        *cursor++ = *value++;
    };
    const auto number = [&](std::uint64_t value) {
      cursor = std::to_chars(cursor, end, value).ptr;
    };
    text("LutApproximationToleranceExceeded index=");
    number(index);
    text(" component=");
    number(c);
    text(" point_bits=");
    number(point[0]);
    text(",");
    number(point[1]);
    text(",");
    number(point[2]);
    text(" reference_bits=");
    number(reference[c]);
    text(" lut_bits=");
    number(lut[c]);
    text(" error_upper_bits=");
    number(error.value());
    *cursor = '\0';
    return false;
  }
  *status = kernel_failure(ErrorCode::OperationFailed,
                           "inconsistent failed bake report",
                           FailureReason::InvalidDomain);
  return false;
}
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const BakeKernel*>(invocation->prepared_context);
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  auto cancelled = consume(0);
  if (!cancelled.ok()) {
    *status = kernel_failure(cancelled.code);
    return;
  }
  Reader reader{state.inputs, *invocation};
  if (state.program->kind == Kind::Measure) {
    auto* scratch = new (invocation->scratch[0].allocation) Measurement;
    const auto success =
        measure(state, *invocation, reader, *scratch, status, consume);
    scratch->~Measurement();
    if (success)
      *status = {};
    return;
  }
  if (state.program->kind == Kind::Gate) {
    auto* scratch =
        new (invocation->scratch[0].allocation) numeric_ops::ExactBakeError;
    const auto success =
        gate(state, *invocation, reader, *scratch, status, consume);
    scratch->~ExactBakeError();
    if (!success)
      return;
  }
  const auto& output = invocation->outputs[0];
  const auto width = state.table_narrow ? 4U : 8U;
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < output.layout->rank; ++axis)
    count *= output.layout->shape[axis];
  for (std::uint64_t i = 0; i < count; ++i) {
    cancelled = consume(1);
    if (!cancelled.ok()) {
      *status = kernel_failure(cancelled.code);
      return;
    }
    auto remaining = i;
    std::array<std::uint64_t, 8> at{};
    for (unsigned axis = output.layout->rank; axis-- > 0;) {
      at[axis] =
          output.layout->origin[axis] + remaining % output.layout->shape[axis];
      remaining /= output.layout->shape[axis];
    }
    const auto bits = reader.bits(0, at, width);
    std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                    output.byte_offset + i * width,
                &bits, width);
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(mismatch("invalid bake Result signature"));
  };
  const auto* state = static_cast<const BakeProgram*>(query.static_state);
  if (!state ||
      query.inputs.size() != (state->kind == Kind::Measure ? 6U
                              : state->kind == Kind::Gate  ? 2U
                                                           : 1U) ||
      query.outputs.size() != 1 || query.writes.empty() ||
      !query.task_parameters.empty() || !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output) ||
      query.scratch !=
          std::array<std::uint64_t, 3>{scratch_bytes(state->kind), 0, 0})
    return invalid();
  if (state->kind != Kind::Measure && query.writes.size() != 1)
    return invalid();
  std::size_t write_slot = 0;
  Region output_region;
  for (unsigned member = 0; member < state->output->tensors.size(); ++member) {
    auto grid =
        TileGrid::from_schema(*state->output, query.tile_extent, member);
    if (!grid.ok())
      return Answer(grid.status());
    const auto count =
        state->kind == Kind::Measure ? grid.value().tile_count() : 1;
    if (count > query.writes.size() - write_slot)
      return invalid();
    for (TileIndex tile = 0; tile < count; ++tile) {
      const auto& write = query.writes[write_slot++];
      const auto index = state->kind == Kind::Measure ? tile : write.tile_index;
      auto region = grid.value().region(index);
      if (!region.ok())
        return Answer(region.status());
      if (write.port || write.member != member || write.tile_index != index ||
          write.domain != physical::Domain::Host ||
          write.layout.element_type !=
              state->output->tensors[member].descriptor.element_type ||
          !fixed_tensor::matches_region(write.layout, region.value()))
        return invalid();
      output_region = region.take_value();
    }
  }
  if (write_slot != query.writes.size())
    return invalid();
  auto regions = source_regions(*state, query.inputs, output_region);
  if (!regions.ok())
    return Answer(regions.status());
  auto prepared = std::make_unique<BakeKernel>();
  prepared->program = state;
  prepared->table_narrow =
      (state->kind == Kind::Measure ? query.inputs[2] : query.inputs[0])
          .result_schema->tensors[0]
          .descriptor.element_type == ElementType::Float32;
  prepared->source_narrow =
      state->kind == Kind::Measure &&
      query.inputs[4].result_schema->tensors[0].descriptor.element_type ==
          ElementType::Float32;
  std::size_t slot = 0;
  for (const auto& region : regions.value()) {
    const auto input = member_slot(state->kind, region.port, region.member);
    auto grid = TileGrid::from_schema(*query.inputs[region.port].result_schema,
                                      query.tile_extent, region.member);
    if (!grid.ok())
      return Answer(grid.status());
    auto& target = prepared->inputs;
    target.grids[input] = grid.take_value();
    auto count = check_count(target.grids[input], region.region,
                             query.reads.size() - slot);
    if (!count.ok())
      return Answer(count);
    auto tiles = sdk::tiles_for_region(ResourceBudget{}, target.grids[input],
                                       region.region);
    if (!tiles.ok())
      return Answer(tiles.status());
    if (tiles.value().size() > query.reads.size() - slot)
      return invalid();
    if (region.port == (state->kind == Kind::Measure ? 2U : 0U))
      prepared->table_slot = slot;
    for (auto tile : tiles.value()) {
      const auto& read = query.reads[slot];
      auto expected = target.grids[input].region(tile);
      if (!expected.ok())
        return Answer(expected.status());
      if (read.port != region.port || read.member != region.member ||
          read.tile_index != tile || read.domain != physical::Domain::Host ||
          read.layout.element_type != query.inputs[region.port]
                                          .result_schema->tensors[region.member]
                                          .descriptor.element_type ||
          !fixed_tensor::matches_region(read.layout, expected.value()))
        return invalid();
      target.slots[input].push_back({tile, static_cast<std::uint32_t>(slot++)});
    }
  }
  if (slot != query.reads.size())
    return invalid();
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = 8;
  result.context = prepared.release();
  result.invoke_cpu = invoke;
  result.release = [](const void* context) {
    delete static_cast<const BakeKernel*>(context);
  };
  return Answer(result);
}
OperationDefinition operation(Kind kind) {
  OperationDefinition result;
  result.key = kind == Kind::Pack      ? "curve.pack_lut3d"
               : kind == Kind::Measure ? "curve.measure_lut3d"
               : kind == Kind::Gate    ? "curve.gate_lut3d"
                                       : "curve.unpack_lut3d";
  auto& traits = result.traits;
  traits.input_count = kind == Kind::Measure ? 6 : kind == Kind::Gate ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  if (kind == Kind::Measure)
    for (unsigned port : {0U, 1U, 3U})
      traits.input_schema[port].element_type_mask = 4;
  const auto object = [&](unsigned port, const char* id) {
    traits.input_schema[port].element_type_mask = 0;
    traits.input_schema[port].result_schema_id = id;
    traits.input_schema[port].result_schema_version = 3;
  };
  if (kind == Kind::Measure)
    object(2, "curve.bake_lut3d.table");
  if (kind == Kind::Unpack || kind == Kind::Gate)
    object(0, "curve.bake_lut3d.table");
  if (kind == Kind::Gate)
    object(1, "curve.bake_lut3d.report");
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = report_parameters();
  if (kind == Kind::Measure || kind == Kind::Gate) {
    traits.deterministic = false;
  }
  auto& out = traits.outputs[0];
  out.key = kind == Kind::Measure ? "report"
            : kind == Kind::Pack  ? "table"
                                  : "values";
  if (kind == Kind::Pack || kind == Kind::Measure) {
    auto schema = kind == Kind::Pack ? lut3d_bake_table_schema(placeholder())
                                     : lut3d_bake_schema(placeholder());
    if (!schema.ok())
      throw std::logic_error(schema.status().message);
    out.result_schema = schema.take_value();
    out.output_schema.result_schema_id = std::string(out.result_schema->id);
    out.output_schema.result_schema_version = out.result_schema->version;
  } else {
    out.output_schema = tensor_ops::port();
    out.result_schema = tensor_ops::scalar_schema();
  }
  result.prepare_static =
      [kind](const auto& inputs,
             const auto& parameters) -> Result<OperationPreparation> {
    auto output = specialize(kind, inputs, parameters);
    if (!output.ok())
      return Result<OperationPreparation>(output.status());
    auto decoded = description(parameters);
    if (!decoded.ok())
      return Result<OperationPreparation>(decoded.status());
    const auto& spec = decoded.value();
    auto identity = lut3d_bake_configuration_id(spec);
    if (!identity.ok())
      return Result<OperationPreparation>(identity.status());
    OperationPreparation prepared;
    prepared.outputs = output.take_value();
    auto state = std::make_shared<BakeProgram>();
    state->kind = kind;
    state->atol = raw(spec.atol);
    state->rtol = raw(spec.rtol);
    state->points = product(spec.shape, 1) + spec.extra_points;
    std::copy_n(identity.value().data(), 64, state->configuration.data());
    state->output = prepared.outputs[0].metadata.result_schema;
    prepared.state = std::move(state);
    return Result<OperationPreparation>(std::move(prepared));
  };
  result.lower = lower;
  result.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "bake-result" || kernel.contract != contract)
      return Result<ps_prepared_kernel_v8>(
          mismatch("unknown bake Result kernel"));
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "bake Result requires CPU Host"});
    return prepare_kernel(query);
  };
  return result;
}
}  // namespace
Status register_lut3d_bake_results(OperationRegistry* registry) {
  for (auto kind : {Kind::Pack, Kind::Measure, Kind::Unpack, Kind::Gate}) {
    auto status = registry->register_operation(operation(kind));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
