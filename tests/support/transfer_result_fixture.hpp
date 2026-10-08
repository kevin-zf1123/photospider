#pragma once

#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "channel_extraction_workflow/source.hpp"

namespace transfer_fixture {
using namespace ps;  // NOLINT(build/namespaces)
using channel_fixture::take;
inline void take(Status status) {
  channel_fixture::require(status);
}
inline void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
inline const ResultTensorSpec& spec(const ResultRef& value) {
  return value.schema().tensors[0];
}
inline OperationMetadata metadata(const ResultRef& value) {
  OperationMetadata result;
  result.result_schema = std::make_shared<SchemaTemplate>(value.schema());
  return result;
}
inline channel_fixture::Source source(const Value& value) {
  auto result = channel_fixture::source(value.descriptor(), value.facets());
  result.bytes.assign(value.bytes().data(),
                      value.bytes().data() + value.bytes().size());
  result.layout = value.layout();
  result.coverage = {value.region()};
  result.resources = value.resources();
  return result;
}
inline Result<channel_fixture::Source> spatial_source(
    const ValueDescriptor& descriptor, const PlanarImageConfig& c,
    const std::vector<ValueFacet>& facets = {}) {
  ResultTensorLayout layout;
  layout.spatial = true;
  layout.order = c.order;
  layout.height_axis = c.height_axis;
  layout.width_axis = c.width_axis;
  layout.channel_axis = c.channel_axis;
  layout.row_pitch_bytes = c.row_pitch_bytes;
  layout.groups = c.groups;
  auto result = channel_fixture::source(descriptor, facets, layout);
  result.tile_height = c.tile_height;
  result.tile_width = c.tile_width;
  result.coverage.clear();
  return Result<channel_fixture::Source>(std::move(result));
}
inline Status publish(channel_fixture::Source& source, const Region& region,
                      const std::uint8_t* bytes, std::size_t count) {
  const auto& tensor = source.schema.tensors[0];
  const auto width = Value::element_size(tensor.descriptor.element_type);
  if (count != take(region.element_count()) * width)
    return {ErrorCode::InvalidArgument, "test source size"};
  auto samples = take(Footprint::from_regions(tensor.sample_shape(), {region}));
  std::size_t i = 0;
  auto status = samples.visit(
      [&](const auto& at) {
        std::memcpy(source.bytes.data() + channel_fixture::address(source, at),
                    bytes + i * width, width);
        ++i;
        return Status::success();
      },
      UINT64_MAX);
  if (status.ok())
    source.coverage.push_back(region);
  return status;
}
inline channel_fixture::Source source(const channel_fixture::Source& original,
                                      const Region& region) {
  const auto available = take(Footprint::from_regions(
      original.schema.tensors[0].sample_shape(), original.coverage));
  const auto requested =
      take(Footprint::from_regions(available.shape(), {region}));
  require(take(requested.subtract(available)).empty(),
          "test request exceeds published source coverage");
  auto result = original;
  result.coverage = {region};
  return result;
}
inline Status read(const ResultRef& result, const Region& region,
                   std::uint8_t* out, std::size_t bytes) {
  auto window = result.acquire_tensor(take(result.descriptor()), 0, region);
  if (!window.ok())
    return window.status();
  const auto packed = channel_fixture::read(result, region);
  if (packed.size() != bytes)
    return {ErrorCode::InvalidArgument, "test read size"};
  std::memcpy(out, packed.data(), bytes);
  return Status::success();
}
struct Hooks {
  std::function<void(const ResultProgramNeed&)> need;
  std::function<void(const ResultProgramPhase&)> before, after;
  std::function<Status(std::uint64_t, const ResultProgramPhase&)> charge;
  std::function<Status(const NumericDiagnostics&, const ResultProgramPhase&)>
      report;
};
struct Hooked {
  ResultContinuation inner;
  std::shared_ptr<Hooks> hooks;
  Hooked(ResultContinuation c, std::shared_ptr<Hooks> h)
      : inner(std::move(c)), hooks(std::move(h)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!phase.tensors || phase.tensors->empty()) {
      auto result = inner.poll(phase);
      if (result.ok() && hooks->need) {
        if (const auto* need = std::get_if<ResultProgramNeed>(&result.value()))
          hooks->need(*need);
      }
      return result;
    }
    auto forwarded = phase;
    if (hooks->charge) {
      forwarded.consume_work = [&](std::uint64_t n) {
        return hooks->charge(n, phase);
      };
    }
    if (hooks->report) {
      forwarded.report_numeric = [&](const NumericDiagnostics& report) {
        return hooks->report(report, phase);
      };
    }
    if (hooks->before)
      hooks->before(phase);
    auto result = inner.poll(forwarded);
    if (hooks->after)
      hooks->after(phase);
    return result;
  }
};
inline OperationDefinition instrument(OperationDefinition op,
                                      std::shared_ptr<Hooks> hooks) {
  auto start = op.start_result;
  op.traits.outputs[0].continuation_bytes += sizeof(Hooked);
  op.start_result = [start, hooks](const auto& query, const auto& allocator) {
    auto inner = start(query, allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<Hooked>(allocator, inner.take_value(),
                                            hooks);
  };
  return op;
}
struct PreparedHook {
  std::shared_ptr<const PreparedOperation> inner;
};
inline std::shared_ptr<OperationRegistry> registry(
    const std::string& key, std::shared_ptr<Hooks> hooks) {
  auto base = make_default_operation_registry();
  auto result = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = key;
  op.traits = take(base->find_traits(key));
  op.prepare_static = [base, key](const auto& inputs, const auto& parameters) {
    auto inner = base->prepare_operation(key, inputs, parameters);
    if (!inner.ok())
      return Result<OperationPreparation>(inner.status());
    OperationPreparation prepared;
    prepared.outputs.resize(1);
    prepared.outputs[0].metadata.result_schema =
        std::make_shared<SchemaTemplate>(
            *inner.value()->traits().outputs[0].result_schema);
    prepared.state =
        std::make_shared<PreparedHook>(PreparedHook{inner.take_value()});
    return Result<OperationPreparation>(std::move(prepared));
  };
  op.start_result = [base, key](const auto& query, const auto& allocator) {
    auto forwarded = query;
    forwarded.prepared =
        static_cast<const PreparedHook*>(query.prepared->state())->inner;
    return base->start_result(key, forwarded, allocator);
  };
  take(result->register_operation(instrument(std::move(op), std::move(hooks))));
  take(result->freeze());
  return result;
}
inline WorkflowDocument document(
    const ResultRef& input, const std::string& key,
    const std::map<std::string, ParameterValue>& parameters) {
  WorkflowDocument doc;
  WorkflowInputDeclaration d;
  d.id = 1;
  d.name = "input";
  d.result_schema = std::make_shared<SchemaTemplate>(input.schema());
  doc.inputs = {d};
  doc.nodes = {{1, key, {WorkflowInputReference{1}}, parameters}};
  doc.outputs = {{"result", 1, "values"}};
  return doc;
}
inline ExecutionBindings bindings(const ResultRef& input) {
  ExecutionBindings result;
  result.inputs.push_back({"input", input});
  return result;
}
}  // namespace transfer_fixture
