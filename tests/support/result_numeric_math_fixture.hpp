#pragma once

#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11): C++17 floating-environment regression.
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/bezier.hpp"
#include "photospider/numeric/calculus.hpp"
#include "photospider/numeric/color_ramps.hpp"
#include "photospider/numeric/curves.hpp"
#include "photospider/numeric/expression.hpp"
#include "photospider/numeric/inverse_curves.hpp"
#include "photospider/numeric/lowpass.hpp"
#include "photospider/numeric/lut1d.hpp"
#include "photospider/numeric/lut3d.hpp"
#include "photospider/numeric/lut3d_baking.hpp"
#include "photospider/numeric/matrix.hpp"
#include "photospider/numeric/resampling.hpp"
#include "photospider/numeric/scans.hpp"
#include "photospider/numeric/sequences.hpp"
#include "photospider/numeric/shapers.hpp"
#include "photospider/photospider.hpp"

namespace ps::test_numeric {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(
        result.status().message.empty()
            ? "status code=" +
                  std::to_string(static_cast<int>(result.status().code)) +
                  " reason=" +
                  std::to_string(static_cast<int>(result.status().reason))
            : result.status().message);
  return result.take_value();
}
inline void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
inline std::vector<uint8_t> words(const std::vector<uint64_t>& bits,
                                  size_t width) {
  std::vector<uint8_t> data(bits.size() * width + 1);
  for (size_t i = 0; i < bits.size(); ++i)
    std::memcpy(data.data() + 1 + i * width, &bits[i], width);
  return data;
}
struct Driver {
  std::shared_ptr<OperationRegistry> registry;
  std::unique_ptr<ExecutionContext> context;
  ResourceBudget root;
  explicit Driver(uint64_t work = UINT64_MAX, uint64_t metadata = 16 * 1048576,
                  uint64_t cache_bytes = 0,
                  std::shared_ptr<OperationRegistry> operations =
                      make_default_operation_registry())
      : registry(std::move(operations)) {
    ExecutionContextConfig config;
    config.result_cache_bytes = cache_bytes;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_work = work;
    config.managed_resources->capacity[ResourceKind::Metadata] = metadata;
    context = std::make_unique<ExecutionContext>(registry, config);
    root = take(context->resource_budget());
  }
  ResultRef source(ValueDescriptor descriptor, std::vector<uint64_t> bits,
                   StridedLayout layout, std::vector<ValueFacet> facets = {},
                   std::vector<uint64_t> batches = {},
                   std::weak_ptr<const CpuStorage>* storage_weak = nullptr,
                   std::string schema_id = "test.numeric",
                   uint32_t schema_version = 1) {
    SchemaTemplate schema;
    schema.id = std::move(schema_id);
    schema.version = schema_version;
    ResultTensorSpec member;
    member.key = "arbitrary-member";
    member.descriptor = descriptor;
    member.facets = std::move(facets);
    member.batch_axes.assign(batches.begin(), batches.end());
    schema.tensors.push_back(std::move(member));
    auto data = words(bits, Value::element_size(descriptor.element_type));
    auto buffer = take(root.allocator().allocate(data.size()));
    std::memcpy(buffer.data(), data.data(), data.size());
    auto storage = std::move(buffer).freeze();
    if (storage_weak)
      *storage_weak = storage;
    auto builder = take(ResultBuilder::start(root, schema, "numeric.source"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "source descriptor");
    require(
        builder
            .publish_tensor(0, Region::whole(schema.tensors[0].sample_shape()),
                            layout, storage,
                            take(ResultRelation::cartesian(
                                root, take(schema.tensors[0].sample_count()),
                                {0, 1, 0, 0})),
                            {true, true, true, true})
            .ok(),
        "source storage");
    return take(builder.seal());
  }
  struct Prepared {
    std::shared_ptr<GraphContext> graph;
    ExecutionPlan plan;
    ExecutionBindings bindings;
  };
  Prepared prepare(const std::string& key, const std::vector<ResultRef>& inputs,
                   std::map<std::string, ParameterValue> parameters = {},
                   const std::string& selected = "values",
                   ResourceBindings resources = {}, bool gpu = false) {
    WorkflowDocument doc;
    WorkflowNode node;
    node.id = 7;
    node.operation = key;
    node.parameters = std::move(parameters);
    ExecutionBindings bindings;
    for (size_t i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration input;
      input.id = i + 11;
      input.name = "input" + std::to_string(i);
      input.result_schema =
          std::make_shared<SchemaTemplate>(inputs[i].schema());
      node.inputs.push_back(WorkflowInputReference{input.id});
      bindings.inputs.push_back({input.name, inputs[i]});
      doc.inputs.push_back(std::move(input));
    }
    doc.nodes.push_back(std::move(node));
    const auto output_key =
        selected == "values" &&
                (key == "numeric.mean" || key == "numeric.variance" ||
                 key == "numeric.ordered_scan")
            ? "value"
            : selected;
    doc.outputs = {{"out", 7, output_key}};
    auto graph = std::make_shared<GraphContext>(doc);
    PlanningOptions planning;
    if (gpu)
      planning.execution_mode = ExecutionMode::NativeGpu;
    auto compiled =
        take(Compiler(registry).compile(*graph, planning, resources));
    return {graph, std::move(compiled.plan), std::move(bindings)};
  }
  Result<DemandResult> run(
      const std::string& key, const std::vector<ResultRef>& inputs,
      std::optional<Footprint> query = {},
      std::map<std::string, ParameterValue> parameters = {},
      CancellationToken cancellation = {},
      const std::string& selected = "values") {
    auto prepared = prepare(key, inputs, std::move(parameters), selected);
    const auto shape = prepared.plan.steps()[0]
                           .output_result_schema->tensors[0]
                           .sample_shape();
    return context->execute_fragments(
        take(context->freeze(prepared.plan, prepared.bindings)),
        {{"out", query ? *query : take(Footprint::all(shape))}}, cancellation);
  }
};
inline uint64_t read_bits(const ResultRef& output, std::vector<uint64_t> at) {
  uint64_t bits = 0;
  require(
      output
          .read_tensor(take(output.descriptor()), 0, at, &bits,
                       Value::element_size(
                           output.schema().tensors[0].descriptor.element_type))
          .ok(),
      "numeric output read");
  return bits;
}
inline uint64_t double_bits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
inline uint64_t float_bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
inline bool math_profile_available(const Driver& driver,
                                   const WorkflowNode& node,
                                   const std::vector<ResultRef>& inputs) {
  std::vector<OperationMetadata> metadata(inputs.size());
  for (unsigned i = 0; i < inputs.size(); ++i)
    metadata[i].result_schema =
        std::make_shared<SchemaTemplate>(inputs[i].schema());
  auto resolved = driver.registry->resolve_traits(node.operation, metadata,
                                                  node.parameters);
  if (resolved.ok())
    return true;
  require(resolved.status().code == ErrorCode::BackendUnavailable &&
              node.operation.find("_strict") == std::string::npos,
          "only an unavailable accelerated profile may skip a workflow");
  return false;
}
struct FailingProducer {
  Result<ResultProgramPoll> poll(const ResultProgramPhase&) {
    return Result<ResultProgramPoll>(
        Status{ErrorCode::OperationFailed, "must stay lazy"});
  }
};
inline ColorArrayDescriptor lut3d_description(
    ColorModel model = ColorModel::Rgb) {
  ColorArrayDescriptor description;
  description.model = model;
  if (model == ColorModel::Rgb || model == ColorModel::Hsl ||
      model == ColorModel::Ycbcr) {
    description.primaries =
        take(color_primary_coordinates(ColorPrimaryPreset::Srgb)).primaries;
    description.transfer = ColorTransfer{ColorTransferKind::Linear, {}};
  }
  if (model == ColorModel::Cielch || model == ColorModel::Oklch ||
      model == ColorModel::Hsl)
    description.hue = ColorHueUnit::PiMultiple;
  if (model == ColorModel::Ycbcr)
    description.ncl_coefficients =
        take(color_ncl_coefficients(ColorNclPreset::Bt709));
  return description;
}
}  // namespace ps::test_numeric
