#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "photospider/data/result_device_access.hpp"
#include "sdk/execution/provider.hpp"
#include "sdk/program/value_tiles.hpp"
namespace ps::executor_sdk {
Status validate_bindings(const ExecutionPlan& plan,
                         const ExecutionBindings& bindings) {
  const auto& declarations = plan.input_declarations();
  const auto& program = plan.program();
  if (bindings.inputs.size() != declarations.size())
    return {ErrorCode::InvalidArgument, "binding count mismatch"};
  std::map<std::string, const ResultRef*> names;
  for (const auto& input : bindings.inputs)
    if (!input.result.valid() ||
        !names.emplace(input.name, &input.result).second)
      return {ErrorCode::InvalidArgument, "invalid or duplicate input binding"};
  std::set<const void*> direct_owners;
  for (unsigned i = 0; i < declarations.size(); ++i) {
    auto pos = names.find(declarations[i].name);
    if (pos == names.end())
      return {ErrorCode::InvalidArgument, "missing input binding"};
    const auto& result = *pos->second;
    if (!result.schema().same_schema(*declarations[i].result_schema))
      return {ErrorCode::TypeMismatch, "input schema mismatch"};
    auto descriptor = result.descriptor();
    if (!descriptor.ok())
      return descriptor.status();
    for (auto id : program.values[program.inputs[i]].tiles) {
      const auto& t = program.tiles[id];
      auto region = plan.input_tile_grids()[i][t.member].region(t.tile_index);
      if (!region.ok())
        return region.status();
      const auto& layout = program.layouts[t.layout];
      const bool direct = program.values[program.inputs[i]].input_layout ==
                          physical::InputLayout::DirectTiles;
      auto validate = [&](const auto& span, uint64_t bytes,
                          const uint8_t* base_address) {
        for (unsigned axis = 0; axis < layout.rank; ++axis)
          if (layout.shape[axis] > 1 &&
              span.layout().byte_strides[axis] != layout.strides[axis])
            return Status{ErrorCode::TypeMismatch,
                          "input stride differs from compiled view"};
        auto start = span.byte_address(layout.origin.data(), layout.rank);
        if (!start.ok())
          return start.status();
        if (start.value() < layout.byte_offset)
          return Status{ErrorCode::TypeMismatch, "input view origin mismatch"};
        const auto offset = start.value() - layout.byte_offset;
        auto alignment =
            base_address && direct
                ? std::max<uint64_t>(t.alignment, host_page_alignment())
                : t.alignment;
        if (offset > bytes || t.bytes > bytes - offset ||
            (reinterpret_cast<uintptr_t>(base_address) + offset) % alignment)
          return Status{ErrorCode::TypeMismatch,
                        "input allocation cannot bind compiled tile"};
        if (direct && !direct_owners.insert(span.storage().get()).second)
          return Status{ErrorCode::TypeMismatch, "aliased DirectTiles input"};
        return Status{};
      };
      Status checked;
      if (t.domain == physical::Domain::Device) {
        auto span = data_internal::ResultDeviceAccess::acquire(
            result, descriptor.value(), t.member, region.value());
        if (!span.ok())
          return span.status();
        checked =
            validate(span.value(), span.value().storage()->size(), nullptr);
      } else {
        auto window =
            result.acquire_tensor(descriptor.value(), t.member, region.value());
        if (!window.ok())
          return window.status();
        auto span = window.value().affine_view();
        if (!span.ok())
          return span.status();
        auto& storage = *span.value().storage();
        if (t.domain == physical::Domain::Shared && !storage.native_handle())
          return {ErrorCode::TypeMismatch,
                  "shared tile requires native backing"};
        checked = validate(span.value(), storage.bytes().size(),
                           storage.bytes().data());
      }
      if (!checked.ok())
        return checked;
    }
  }
  return {};
}
namespace {
Status validate_coverage(const ResultRef& result, const SchemaTemplate& schema,
                         const std::vector<TileGrid>& grids,
                         const TileRequest& request) {
  if (!result.valid())
    return {ErrorCode::InvalidArgument, "missing executor result"};
  if (!result.schema().same_schema(schema))
    return {ErrorCode::TypeMismatch, "executor return schema mismatch"};
  auto descriptor = result.descriptor();
  if (!descriptor.ok())
    return descriptor.status();
  for (std::size_t member = 0; member < grids.size(); ++member) {
    const auto found = request.members.find(member);
    const auto& actual = descriptor.value().tensor_coverage(member);
    if (found == request.members.end()) {
      if (actual.empty())
        continue;
    } else {
      auto wanted = grids[member].coverage(found->second);
      if (!wanted.ok())
        return wanted.status();
      if (wanted.value() == actual)
        continue;
    }
    return {ErrorCode::InvalidArgument,
            "executor result differs from compiled coverage"};
  }
  return {};
}
}  // namespace

Status validate_exports(const ExecutionPlan& plan, const CallRecord& record) {
  const auto& program = plan.program();
  const auto& expected = program.calls[record.call].returns;
  if (record.values.size() != expected.size())
    return {ErrorCode::InvalidArgument, "incorrect number of retired values"};
  for (const auto id : expected) {
    auto found = record.values.find(id);
    if (found == record.values.end())
      return {ErrorCode::InvalidArgument, "missing retired value"};
    const auto& value = program.values[id];
    const auto& schema = *program.schemas[value.schema];
    auto grids = TileGrid::members(schema, program.tile_extent);
    if (!grids.ok())
      return grids.status();
    auto selection = program_internal::request_from_value(program, id);
    if (!selection.ok())
      return selection.status();
    auto status = validate_coverage(found->second, schema, grids.value(),
                                    selection.value());
    if (!status.ok()) {
      if (status.code == ErrorCode::ResourceExhausted)
        return status;
      return {ErrorCode::InvalidArgument,
              "retired value does not match its compiled export"};
    }
  }
  return {};
}
Status validate_results(const ExecutionPlan& plan,
                        const ExecutionResult& result) {
  if (plan.outputs().size() != result.results.size())
    return {ErrorCode::InvalidArgument, "incomplete executor returns"};
  for (const auto& output : plan.outputs()) {
    auto found = result.results.find(output.first);
    if (found == result.results.end())
      return {ErrorCode::InvalidArgument, "missing executor result"};
    const auto& declaration = plan.output(output.first);
    auto status = validate_coverage(found->second, *declaration.result_schema,
                                    declaration.tile_grids,
                                    plan.output_requests().at(output.first));
    if (!status.ok())
      return status;
  }
  return {};
}
}  // namespace ps::executor_sdk
