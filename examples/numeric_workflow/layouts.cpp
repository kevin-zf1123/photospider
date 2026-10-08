#include "photospider/numeric/layouts.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
namespace rf = numeric_result_fixture;
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(
        "layout status code=" +
        std::to_string(static_cast<unsigned>(result.status().code)) +
        " reason=" +
        std::to_string(static_cast<unsigned>(result.status().reason)) + " " +
        result.status().message);
  return result.take_value();
}
ps::Value array(const std::vector<std::uint64_t>& shape,
                const std::vector<std::int64_t>& values,
                std::vector<std::int64_t> strides = {}) {
  auto buffer = take(ps::BufferAllocator{}.allocate(values.size() * 8));
  std::memcpy(buffer.data(), values.data(), values.size() * 8);
  if (strides.empty()) {
    strides.resize(shape.size());
    std::int64_t step = 8;
    for (std::size_t j = shape.size(); j; --j) {
      strides[j - 1] = step;
      step *= shape[j - 1];
    }
  }
  return take(ps::Value::from_storage({ps::ElementType::Int64, shape},
                                      ps::Region::whole(shape), {0, strides},
                                      std::move(buffer).freeze()));
}
ps::ResultTensorReadWindow window(const ps::ResultRef& result) {
  return take(result.acquire_tensor(
      take(result.descriptor()), 0,
      ps::Region::whole(result.schema().tensors[0].sample_shape())));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry;
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  std::optional<ps::ResourceBudget> last_root;
  Fixture(ps::WorkflowNode node, std::vector<ps::Value> inputs,
          std::shared_ptr<ps::OperationRegistry> operations = {})
      : registry(operations ? std::move(operations)
                            : ps::make_default_operation_registry()),
        backing(std::move(inputs)) {
    rf::declare_sources(&document, backing);
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::ExecutionBindings bind(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::DemandResult> run(const ps::Footprint& demand,
                                   std::uint64_t capacity = 262144,
                                   std::uint64_t work = 1048576) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = capacity;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    last_root = take(execution.resource_budget());
    auto frozen = execution.freeze(plan.value().plan, bind(*last_root));
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    ps::ExecutionOptions options;
    options.dependencies.maximum_work = work;
    options.maximum_dependency_work = work;
    options.maximum_dependency_cache_work = 0;
    ps::DemandQuery query{{"values", demand}};
    for (const auto& output : document.outputs) {
      if (output.name == "values")
        continue;
      const auto& metadata = plan.value().plan.steps().at(
          plan.value().plan.outputs().at(output.name));
      query.emplace(
          output.name,
          take(ps::Footprint::all(
              metadata.output_result_schema->tensors[0].sample_shape())));
    }
    return execution.execute_fragments(frozen.value(), query, {}, options);
  }
};
// Reuse one execution worker for the finite-address oracle; each graph still
// compiles, freezes and executes through the public Result entry point.
struct Driver {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  std::unique_ptr<ps::ExecutionContext> context;
  ps::ResourceBudget root;
  Driver() {
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    context = std::make_unique<ps::ExecutionContext>(registry, config);
    root = take(context->resource_budget());
  }
  ps::Result<ps::ExecutionResult> run(ps::WorkflowNode node,
                                      const std::vector<ps::Value>& values) {
    Fixture fixture(std::move(node), values, registry);
    ps::GraphContext graph(fixture.document);
    auto compiled = ps::Compiler(registry).compile(graph);
    if (!compiled.ok())
      return ps::Result<ps::ExecutionResult>(compiled.status());
    ps::ExecutionOptions options;
    options.dependencies.maximum_work = UINT64_C(1) << 50;
    options.maximum_dependency_work = UINT64_C(1) << 50;
    options.maximum_dependency_cache_work = 0;
    return context->execute(compiled.value().plan, fixture.bind(root), {},
                            options);
  }
};
void reshape(ps::CpuNumericProfile profile) {
  const auto source = array({2, 3}, {0, 1, 2, 3, 4, 5});
  for (auto layout :
       {ps::numeric::TransformLayout::Auto, ps::numeric::TransformLayout::View,
        ps::numeric::TransformLayout::Dense}) {
    Fixture fixture(
        take(ps::numeric::reshape_node(1, ps::WorkflowInputReference{1}, {3, 2},
                                       layout, profile)),
        {source});
    auto result = take(fixture.run(take(ps::Footprint::all({3, 2}))));
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned j = 0; j < 2; ++j) {
        std::int64_t value = -1;
        require(rf::read(result.results.at("values"), {i, j}, &value, 8).ok() &&
                    value == 2 * i + j,
                "reshape logical sequence");
      }
    require(result.diagnostics.operation_timings.size() == 1,
            "one layout operation timing");
    const auto& numeric = result.diagnostics.operation_timings[0].numeric;
    require(numeric.profile == profile &&
                numeric.view_elements ==
                    (layout == ps::numeric::TransformLayout::Dense ? 0U : 6U) &&
                numeric.copied_elements ==
                    (layout == ps::numeric::TransformLayout::Dense ? 6U : 0U),
            "reshape reports actual profile and view/copy element counts");
  }
  // A public transpose node creates the physically
  // strided intermediate whose complete-output viewability reshape must
  // inspect.
  const auto backing = array({3, 2}, {0, 3, 1, 4, 2, 5});
  Fixture unavailable(take(ps::numeric::reshape_node(
                          2, ps::WorkflowNodeOutput{1, "values"}, {3, 2},
                          ps::numeric::TransformLayout::View, profile)),
                      {backing});
  unavailable.document.nodes.insert(
      unavailable.document.nodes.begin(),
      take(ps::numeric::transpose_node(1, ps::WorkflowInputReference{1}, {1, 0},
                                       ps::numeric::TransformLayout::View,
                                       profile)));
  auto whole = unavailable.run(take(ps::Footprint::all({3, 2})));
  require(
      whole.status().code == ps::ErrorCode::InvalidArgument &&
          whole.status().message.find("ViewUnavailable") != std::string::npos,
      "reshape cannot manufacture tiny views for a non-affine rectangle");
  const auto one_row =
      take(ps::Footprint::from_regions({3, 2}, {ps::Region({{0, 1}, {0, 2}})}));
  auto regional = unavailable.run(one_row);
  require(!regional.ok() && regional.status().message.find("ViewUnavailable") !=
                                std::string::npos,
          "partial request still requires complete affine output");
  unavailable.document.nodes[1].parameters["layout"] = std::string("auto");
  auto fallback = take(unavailable.run(take(ps::Footprint::all({3, 2}))));
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = 0; j < 2; ++j) {
      std::int64_t value = -1;
      require(rf::read(fallback.results.at("values"), {i, j}, &value, 8).ok() &&
                  value == 2 * i + j,
              "auto packs the non-affine rectangle");
    }
  std::cout << "reshape: [2,3]->[3,2] raw logical order; complete-output "
               "view/auto/dense passed\n";
}
void transpose(ps::CpuNumericProfile profile) {
  std::vector<std::int64_t> values;
  for (unsigned i = 0; i < 2; ++i)
    for (unsigned j = 0; j < 3; ++j)
      for (unsigned k = 0; k < 4; ++k)
        values.push_back(100 * i + 10 * j + k);
  const auto source = array({2, 3, 4}, values);
  Fixture fixture(take(ps::numeric::transpose_node(
                      1, ps::WorkflowInputReference{1}, {2, 0, 1},
                      ps::numeric::TransformLayout::View, profile)),
                  {source});
  auto result = take(fixture.run(take(ps::Footprint::all({4, 2, 3}))));
  for (unsigned k = 0; k < 4; ++k)
    for (unsigned i = 0; i < 2; ++i)
      for (unsigned j = 0; j < 3; ++j) {
        std::int64_t value = -1;
        require(
            rf::read(result.results.at("values"), {k, i, j}, &value, 8).ok() &&
                value == 100 * i + 10 * j + k,
            "independent transpose coordinate oracle");
      }
  std::cout
      << "transpose: permutation [2,0,1], values[k,i,j]=100*i+10*j+k passed\n";
}
void slice(ps::CpuNumericProfile profile) {
  const auto input = array({6}, {0, 1, 2, 3, 4, 5});
  Fixture fixture(
      take(ps::numeric::slice_node(
          1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
          ps::WorkflowInputReference{3}, {3},
          ps::numeric::TransformLayout::Auto, profile)),
      {input, array({1}, {4}), array({1}, {-2})});
  auto result = take(fixture.run(take(ps::Footprint::all({3}))));
  for (unsigned i = 0; i < 3; ++i) {
    std::int64_t value = -1;
    require(rf::read(result.results.at("values"), {i}, &value, 8).ok() &&
                value == 4 - 2 * static_cast<std::int64_t>(i),
            "reverse stride slice values");
  }
  const auto support = take(result.dependencies.source_support());
  require(support.at("input0") == take(ps::Footprint::all({6})),
          "Whole slice reads complete source");
  for (std::uint64_t changed : {1, 2})
    require(take(result.dependencies.potential_dirty(
                     "input0", take(ps::Footprint::from_regions(
                                   {6}, {ps::Region({{changed, 1}})}))))
                    .at("values") == take(ps::Footprint::all({3})),
            "any source edit invalidates complete slice");
  auto one = take(fixture.run(
      take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}))));
  auto one_window = window(one.results.at("values"));
  require(take(one_window.row_run({1})).sample_stride_bytes == -16 &&
              take(one.results.at("values").descriptor()).tensor_coverage(0) ==
                  take(ps::Footprint::all({3})),
          "partial query retains complete Result and global slice stride");
  fixture.backing[2] = array({1}, {1});
  auto invalid = fixture.run(
      take(ps::Footprint::from_regions({3}, {ps::Region({{0, 1}})})));
  require(
      invalid.status().code == ps::ErrorCode::InvalidArgument &&
          invalid.status().message.find("InvalidSlice") != std::string::npos,
      "partial request still validates the full slice domain");
  fixture.document.nodes[0].parameters["counts"] = std::string("1");
  Fixture malformed(fixture.document.nodes[0],
                    {input, array({1}, {4}), array({2}, {0, 0})});
  require(malformed.run(take(ps::Footprint::all({1}))).status().code ==
              ps::ErrorCode::TypeMismatch,
          "excluded singleton step retains static shape validation");
  unsigned step_reads = 0;
  fixture.registry = ps::make_default_operation_registry(false);
  ps::OperationDefinition failure;
  failure.key = "manual.layout_failed_step";
  failure.traits.input_count = 0;
  failure.traits.input_schema.clear();
  auto& output = failure.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.result_schema = *fixture.document.inputs[2].result_schema;
  output.output_schema.result_schema_id = output.result_schema->id;
  output.output_schema.result_schema_version = output.result_schema->version;
  output.region_rule = ps::OperationRegionRule::Whole;
  output.continuation_bytes = 1;
  output.maximum_dependency_stages = 1;
  failure.start_result =
      [&](const auto&, const auto&) -> ps::Result<ps::ResultContinuation> {
    ++step_reads;
    return ps::Result<ps::ResultContinuation>(
        ps::Status{ps::ErrorCode::OperationFailed, "ignored singleton step"});
  };
  require(fixture.registry->register_operation(std::move(failure)).ok() &&
              fixture.registry->freeze().ok(),
          "register Result step");
  fixture.document.inputs.pop_back();
  fixture.backing.pop_back();
  fixture.document.nodes[0].inputs[2] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.layout_failed_step", {}, {}});
  auto singleton = take(fixture.run(take(ps::Footprint::all({1}))));
  std::int64_t value = 0;
  require(step_reads == 0 &&
              rf::read(singleton.results.at("values"), {0}, &value, 8).ok() &&
              value == 4,
          "singleton count ignores failing step producer");
  fixture.document.nodes[0].parameters["counts"] = std::string("3");
  auto required = fixture.run(take(ps::Footprint::all({3})));
  require(!required.ok() && step_reads == 1 &&
              required.status().code == ps::ErrorCode::OperationFailed &&
              required.status().message == "ignored singleton step",
          "non-singleton slice retains required step producer failure");
  std::cout << "slice: [4,2,0], exact support/dirty, full-domain validation "
               "and ignored singleton step passed\n";
}
struct LayoutFragments {
  bool shared;
  explicit LayoutFragments(bool value) : shared(value) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    auto builder = take(ps::ResultBuilder::start(phase.resources, schema,
                                                 phase.query.semantic_key));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "fragment source descriptor");
    const std::int64_t values[] = {0, 1, 2, 3};
    std::shared_ptr<const ps::CpuStorage> common;
    if (shared) {
      auto storage = take(phase.resources.allocator().allocate(33));
      std::memcpy(storage.data() + 1, values, 32);
      common = std::move(storage).freeze();
    }
    for (std::uint64_t row = 0; row < 2; ++row) {
      auto chosen = common;
      if (!shared) {
        auto buffer = take(phase.resources.allocator().allocate(16));
        std::memcpy(buffer.data(), values + 2, 16);
        chosen = std::move(buffer).freeze();
      }
      require(
          builder
              .publish_tensor(
                  0, ps::Region({{row, 1}, {0, 2}}),
                  {shared ? 1 + row * 16 : 0, {0, 8}, {row, 0}}, chosen,
                  take(ps::ResultRelation::cartesian(phase.resources, 4, {})),
                  {true, true, true, true})
              .ok(),
          "fragment source publication");
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
void physical_layouts(ps::CpuNumericProfile profile) {
  Driver driver;
  auto buffer = take(ps::BufferAllocator{}.allocate(49));
  const std::int64_t raw[] = {0, 1, 2, 3, 4, 5};
  std::memcpy(buffer.data() + 1, raw, 48);
  auto owner = std::move(buffer).freeze();
  for (bool zero : {false, true}) {
    auto source = take(ps::Value::from_storage(
        {ps::ElementType::Int64, {2, 3}}, ps::Region::whole({2, 3}),
        {25, {zero ? 0 : -24, 8}}, owner));
    for (auto layout : {ps::numeric::TransformLayout::Auto,
                        ps::numeric::TransformLayout::View,
                        ps::numeric::TransformLayout::Dense}) {
      auto node = take(ps::numeric::transpose_node(
          1, ps::WorkflowInputReference{1}, {1, 0}, layout, profile));
      auto result = take(driver.run(node, {source})).results.at("values");
      auto output_window = window(result);
      for (std::uint64_t j = 0; j < 3; ++j)
        for (std::uint64_t i = 0; i < 2; ++i) {
          std::int64_t value = -1;
          require(rf::read(result, {j, i}, &value, 8).ok(),
                  "physical view Result read");
          require(value == raw[(zero ? 1 : 1 - i) * 3 + j],
                  "unaligned negative/zero stride transpose");
        }
      require(layout == ps::numeric::TransformLayout::Dense ||
                  output_window.storage_owner_token() == owner.get(),
              "public Result view retains actual source owner");
    }
  }
  // Compatible same-owner and multiple-owner fragment cases are exercised
  // through the public graph below, so the Whole collection boundary is real.
  for (bool shared : {true, false}) {
    auto operations = ps::make_default_operation_registry(false);
    ps::OperationDefinition split;
    split.key = "manual.layout_split";
    split.traits.input_count = 0;
    split.traits.input_schema.clear();
    auto& output = split.traits.outputs[0];
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.regional_atomic = true;
    output.output_schema.kind = ps::OperationPortKind::Result;
    output.result_schema = rf::source_schema(array({2, 2}, {0, 1, 2, 3}));
    output.output_schema.result_schema_id = output.result_schema->id;
    output.output_schema.result_schema_version = output.result_schema->version;
    output.maximum_output_payload_bytes = shared ? 33 : 32;
    output.continuation_bytes = sizeof(LayoutFragments);
    output.maximum_dependency_stages = 1;
    split.start_result = [shared](const auto&, const auto& allocator) {
      return ps::ResultContinuation::make<LayoutFragments>(allocator, shared);
    };
    require(operations->register_operation(std::move(split)).ok() &&
                operations->freeze().ok(),
            "register layout fragments");
    for (auto layout : {ps::numeric::TransformLayout::View,
                        ps::numeric::TransformLayout::Auto,
                        ps::numeric::TransformLayout::Dense}) {
      Fixture fixture(
          take(ps::numeric::reshape_node(2, ps::WorkflowNodeOutput{1, "value"},
                                         {4}, layout, profile)),
          {});
      fixture.registry = operations;
      fixture.document.outputs.push_back({"source", 1, "value"});
      fixture.document.nodes.insert(fixture.document.nodes.begin(),
                                    {1, "manual.layout_split", {}, {}});
      auto result = fixture.run(take(ps::Footprint::all({4})));
      if (!shared && layout == ps::numeric::TransformLayout::View) {
        require(!result.ok() && result.status().message.find(
                                    "ViewUnavailable") != std::string::npos,
                result.ok() ? "unexpected multi-owner View success"
                            : result.status().message.c_str());
      } else {
        auto ready = take(std::move(result));
        for (std::uint64_t i = 0; i < 4; ++i) {
          std::int64_t value = 0;
          require(
              rf::read(ready.results.at("values"), {i}, &value, 8).ok() &&
                  value == static_cast<std::int64_t>(shared ? i : 2 + i % 2),
              "Whole physical fragment order");
        }
        auto source_window = window(ready.results.at("source"));
        auto output_window = window(ready.results.at("values"));
        const bool viewed =
            shared && layout != ps::numeric::TransformLayout::Dense;
        require((source_window.storage_owner_token() ==
                 output_window.storage_owner_token()) == viewed &&
                    (shared || source_window.storage_owner_token() == nullptr),
                "fragment view or dense copy has the expected actual owner");
        if (viewed)
          require(take(source_window.row_run({0, 0})).data ==
                      take(output_window.row_run({0})).data,
                  "same-owner fragment view keeps original first address");
      }
    }
  }
  driver.context.reset();
  point_math_checks::released(driver.root);
  std::cout << "physical layouts: unaligned/negative/zero strides, shared "
               "and independent fragment owners passed\n";
}
void boundaries(ps::CpuNumericProfile profile) {
  auto reshape = take(
      ps::numeric::reshape_node(1, ps::WorkflowInputReference{1}, {3, 2},
                                ps::numeric::TransformLayout::Auto, profile));
  Fixture fixture(reshape, {array({2, 3}, {0, 1, 2, 3, 4, 5})});
  for (const auto* shape :
       {"", "01,6", "0,6", "3,3", "1099511627777", "1,1,1,1,1,1,1,1,1"}) {
    fixture.document.nodes[0].parameters["shape"] = std::string(shape);
    ps::GraphContext graph(fixture.document);
    auto result = ps::Compiler(fixture.registry).compile(graph);
    require(result.status().code == ps::ErrorCode::InvalidArgument &&
                result.status().detail.origin == ps::FailureOrigin::Schema,
            "malformed/rank/product/count mismatch is a schema failure");
  }
  fixture.document.nodes[0] = reshape;
  auto control = std::make_shared<point_math_checks::Control>();
  control->maximum_work = 1;
  {
    point_math_checks::Workflow checked(reshape, fixture.backing, {}, control);
    auto stopped = checked.run();
    require(!stopped.ok() &&
                stopped.status().code == ps::ErrorCode::ResourceExhausted &&
                stopped.status().reason == ps::FailureReason::WorkLimit &&
                control->computation_polls > 0,
            "layout callback work exhaustion is not an auto fallback");
  }
  auto empty = take(fixture.run(take(ps::Footprint::none({3, 2}))));
  require(
      take(empty.results.at("values").descriptor()).tensor_coverage(0).empty(),
      "Empty skips Whole sample work");
  for (const auto& timing : empty.diagnostics.operation_timings)
    require(timing.computed_elements == 0, "Empty performs no computation");
  for (const auto& input : take(empty.dependencies.source_support()))
    require(input.second.empty(), "Empty has no input sample support");
  ps::CancellationSource cancellation;
  cancellation.cancel();
  point_math_checks::Workflow cancelled(reshape, fixture.backing);
  require(cancelled.run(cancellation.token()).status().code ==
              ps::ErrorCode::Cancelled,
          "pre-cancelled Whole layout");
  Fixture large(take(ps::numeric::transpose_node(
                    2, ps::WorkflowNodeOutput{1, "values"}, {1, 0},
                    ps::numeric::TransformLayout::Auto, profile)),
                {array({1}, {7})});
  large.document.nodes.insert(large.document.nodes.begin(),
                              take(ps::numeric::constant_node(
                                  1, ps::WorkflowInputReference{1}, {64, 64},
                                  ps::numeric::ArrayLayout::View, profile)));
  auto view = take(large.run(take(ps::Footprint::all({64, 64})), 4096));
  std::int64_t last = 0;
  require(
      rf::read(view.results.at("values"), {63, 63}, &last, 8).ok() &&
          last == 7 &&
          large.last_root->statistics().live[ps::ResourceKind::Payload] == 8,
      "view admission never reserves the 32768-byte dense output");
  large.document.nodes[1].parameters["layout"] = std::string("dense");
  auto dense = large.run(take(ps::Footprint::all({64, 64})), 4096);
  require(dense.status().code == ps::ErrorCode::ResourceExhausted,
          "dense output admits its actual payload capacity");
  std::cout << "schema, Empty, WorkLimit, cancellation and low-budget "
               "public constant/transpose view passed\n";
}
void typed_and_lifetime(ps::CpuNumericProfile profile) {
  ps::ColorArrayDescriptor color;
  color.model = ps::ColorModel::Rgb;
  color.association = ps::ColorAssociation::Straight;
  color.primaries =
      take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
          .primaries;
  color.transfer = ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
  for (unsigned kind = 0; kind < 3; ++kind) {
    auto node =
        kind == 0 ? take(ps::numeric::reshape_node(
                        1, ps::WorkflowInputReference{1}, {4},
                        ps::numeric::TransformLayout::View, profile))
        : kind == 1
            ? take(ps::numeric::transpose_node(
                  1, ps::WorkflowInputReference{1}, {2, 0, 1},
                  ps::numeric::TransformLayout::View, profile))
            : take(ps::numeric::slice_node(
                  1, ps::WorkflowInputReference{1},
                  ps::WorkflowInputReference{2}, ps::WorkflowInputReference{3},
                  {1, 1, 1}, ps::numeric::TransformLayout::View, profile));
    const float rgba[] = {1, 0, 0, 2};
    auto bytes = take(ps::BufferAllocator{}.allocate(16));
    std::memcpy(bytes.data(), rgba, 16);
    auto invalid = take(ps::Value::from_storage(
        {ps::ElementType::Float32, {1, 1, 4}}, ps::Region::whole({1, 1, 4}),
        {0, {16, 16, 4}}, std::move(bytes).freeze()));
    std::vector<ps::Value> inputs{invalid};
    if (kind == 2) {
      inputs.push_back(array({3}, {0, 0, 0}));
      inputs.push_back(array({3}, {0, 0, 0}));
    }
    Fixture fixture(node, inputs);
    auto schema = rf::source_schema(invalid);
    schema.tensors[0].facets = {take(ps::encode_color_array(color))};
    schema.tensors[0].atomic_trailing_axes = 1;
    fixture.document.inputs[0].result_schema =
        std::make_shared<ps::SchemaTemplate>(schema);
    const auto shape = kind == 0   ? std::vector<std::uint64_t>{4}
                       : kind == 1 ? std::vector<std::uint64_t>{4, 1, 1}
                                   : std::vector<std::uint64_t>{1, 1, 1};
    auto bad = fixture.run(take(ps::Footprint::all(shape)));
    require(!bad.ok() && bad.status().code == ps::ErrorCode::InvalidArgument &&
                bad.status().reason == ps::FailureReason::InvalidDomain &&
                bad.status().detail.input_id == 1,
            "Whole layouts validate complete typed source tuple");
    auto empty = take(fixture.run(take(ps::Footprint::none(shape))));
    require(take(empty.results.at("values").descriptor())
                .tensor_coverage(0)
                .empty(),
            "Empty layout skips invalid typed alpha");
    auto legal_bytes = take(ps::BufferAllocator{}.allocate(16));
    const float legal[] = {1, 0, 0, 1};
    std::memcpy(legal_bytes.data(), legal, 16);
    fixture.backing[0] = take(ps::Value::from_storage(
        invalid.descriptor(), invalid.region(), invalid.layout(),
        std::move(legal_bytes).freeze()));
    auto valid = take(fixture.run(take(ps::Footprint::all(shape))));
    require(valid.results.at("values").schema().tensors[0].facets.empty(),
            "layout output drops typed facets");
    for (std::uint64_t channel = 0; channel < (kind == 2 ? 1U : 4U);
         ++channel) {
      const auto at = kind == 0   ? std::vector<std::uint64_t>{channel}
                      : kind == 1 ? std::vector<std::uint64_t>{channel, 0, 0}
                                  : std::vector<std::uint64_t>{0, 0, 0};
      std::uint32_t actual = 0, expected = 0;
      std::memcpy(&expected, &legal[channel], 4);
      require(rf::read(valid.results.at("values"), at, &actual, 4).ok() &&
                  actual == expected,
              "typed layout same-schema positive control bits");
    }
  }
  Fixture fixture(take(ps::numeric::reshape_node(
                      1, ps::WorkflowInputReference{1}, {3, 2},
                      ps::numeric::TransformLayout::View, profile)),
                  {array({2, 3}, {0, 1, 2, 3, 4, 5})});
  ps::ResultRef held;
  ps::ResultTensorReadWindow held_window;
  ps::ResourceBudget root;
  {
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(fixture.registry, config);
    root = take(execution.resource_budget());
    auto bindings = fixture.bind(root);
    auto source_window = window(bindings.inputs[0].result);
    auto frozen = take(execution.freeze(plan.plan, bindings));
    auto result = take(execution.execute_fragments(
        frozen, {{"values", take(ps::Footprint::all({3, 2}))}}));
    held = result.results.at("values");
    held_window = window(held);
    require(held_window.storage_owner_token() ==
                    source_window.storage_owner_token() &&
                take(held_window.row_run({0, 0})).data ==
                    take(source_window.row_run({0, 0})).data &&
                held.association() ==
                    ps::ResourceVector<std::uint64_t>{
                        bindings.inputs[0].result.object_id()},
            "mapped output keeps actual input owner/address and association");
  }
  std::int64_t value = -1;
  require(rf::read(held, {2, 1}, &value, 8).ok() && value == 5,
          "returned view survives context destruction");
  held = {};
  auto row = take(held_window.row_run({2, 1}));
  std::memcpy(&value, row.data, 8);
  require(
      value == 5 && root.statistics().live[ps::ResourceKind::Referenced] >= 48,
      "escaped read window pins mapped source after Result release");
  held_window = {};
  point_math_checks::released(root);
  std::cout << "all layout typed-validation closures and escaped Result/window "
               "publication lifetime passed\n";
}
void affine_oracle(ps::CpuNumericProfile profile) {
  Driver driver;
  std::uint64_t cases = 0;
  for (const std::vector<std::uint64_t>& shape :
       {std::vector<std::uint64_t>{2, 3}, {1, 2, 3}, {2, 2, 3}}) {
    std::uint64_t count = 1, combinations = 1;
    for (auto extent : shape) {
      count *= extent;
      combinations *= 7;
    }
    const std::int64_t options[] = {-48, -24, -8, 0, 8, 24, 48};
    for (std::uint64_t trial = 0; trial < combinations; ++trial) {
      auto digits = trial;
      std::vector<std::int64_t> strides(shape.size());
      std::int64_t low = 0, high = 0;
      for (std::size_t j = 0; j < shape.size(); ++j) {
        strides[j] = options[digits % 7];
        digits /= 7;
        const auto end = static_cast<std::int64_t>(shape[j] - 1) * strides[j];
        low += std::min<std::int64_t>(0, end);
        high += std::max<std::int64_t>(0, end);
      }
      auto buffer = take(ps::BufferAllocator{}.allocate(high - low + 9));
      for (std::uint64_t i = 0;
           i <= static_cast<std::uint64_t>((high - low) / 8); ++i) {
        const std::int64_t bits = 1000 + i;
        std::memcpy(buffer.data() + 1 + 8 * i, &bits, 8);
      }
      auto input = take(ps::Value::from_storage(
          {ps::ElementType::Int64, shape}, ps::Region::whole(shape),
          {static_cast<std::uint64_t>(1 - low), strides},
          std::move(buffer).freeze()));
      // Independent finite enumeration of physical addresses; no chunk rules.
      std::vector<std::int64_t> addresses(count);
      for (std::uint64_t i = 0; i < count; ++i) {
        auto index = i;
        addresses[i] = 1 - low;
        for (std::size_t j = shape.size(); j; --j) {
          addresses[i] +=
              static_cast<std::int64_t>(index % shape[j - 1]) * strides[j - 1];
          index /= shape[j - 1];
        }
      }
      for (const std::vector<std::uint64_t>& target :
           {std::vector<std::uint64_t>{count},
            {count / 2, 2},
            {1, count},
            {2, count / 2}}) {
        std::vector<std::int64_t> inferred(target.size(), 0);
        std::uint64_t suffix = 1;
        for (std::size_t j = target.size(); j; --j) {
          if (target[j - 1] > 1)
            inferred[j - 1] = addresses[suffix] - addresses[0];
          suffix *= target[j - 1];
        }
        bool affine = true;
        for (std::uint64_t i = 0; i < count; ++i) {
          auto index = i;
          auto expected = addresses[0];
          for (std::size_t j = target.size(); j; --j) {
            expected += static_cast<std::int64_t>(index % target[j - 1]) *
                        inferred[j - 1];
            index /= target[j - 1];
          }
          affine &= expected == addresses[i];
        }
        for (auto mode : {ps::numeric::TransformLayout::View,
                          ps::numeric::TransformLayout::Auto,
                          ps::numeric::TransformLayout::Dense}) {
          auto node = take(ps::numeric::reshape_node(
              1, ps::WorkflowInputReference{1}, target, mode, profile));
          auto executed = driver.run(node, {input});
          ps::Result<ps::ResultRef> result =
              executed.ok() ? ps::Result<ps::ResultRef>(
                                  executed.value().results.at("values"))
                            : ps::Result<ps::ResultRef>(executed.status());
          require(result.ok() ==
                      (affine || mode != ps::numeric::TransformLayout::View),
                  "independent complete affine address oracle");
          if (!result.ok())
            continue;
          auto output_window = window(result.value());
          require(
              (output_window.storage_owner_token() == input.storage().get()) ==
                  (affine && mode != ps::numeric::TransformLayout::Dense),
              "affine mode retains exact owner");
          for (std::uint64_t i = 0; i < count; ++i) {
            std::vector<std::uint64_t> at(target.size());
            auto index = i;
            for (std::size_t j = target.size(); j; --j) {
              at[j - 1] = index % target[j - 1];
              index /= target[j - 1];
            }
            std::int64_t actual = 0, expected = 0;
            std::memcpy(&expected, input.bytes().data() + addresses[i], 8);
            require(rf::read(result.value(), at, &actual, 8).ok(),
                    "affine oracle Result read");
            require(actual == expected,
                    "independent strided reshape raw bytes");
          }
          ++cases;
        }
      }
    }
  }
  driver.context.reset();
  point_math_checks::released(driver.root);
  std::cout << "finite-address affine oracle: " << cases
            << " successful layouts plus expected View rejections passed\n";
}
void active_layout_budgets(ps::CpuNumericProfile profile) {
  std::vector<std::int64_t> values(16384);
  for (unsigned i = 0; i < values.size(); ++i)
    values[i] = i;
  const auto input = array({16384}, values);
  for (unsigned kind = 0; kind < 3; ++kind) {
    auto node =
        kind == 0 ? take(ps::numeric::reshape_node(
                        1, ps::WorkflowInputReference{1}, {16384},
                        ps::numeric::TransformLayout::Dense, profile))
        : kind == 1
            ? take(ps::numeric::transpose_node(
                  1, ps::WorkflowInputReference{1}, {0},
                  ps::numeric::TransformLayout::Dense, profile))
            : take(ps::numeric::slice_node(
                  1, ps::WorkflowInputReference{1},
                  ps::WorkflowInputReference{2}, ps::WorkflowInputReference{3},
                  {16384}, ps::numeric::TransformLayout::Dense, profile));
    std::vector<ps::Value> inputs{input};
    if (kind == 2) {
      inputs.push_back(array({1}, {0}));
      inputs.push_back(array({1}, {1}));
    }
    point_math_checks::resources(node, inputs);
  }
}
void floating_environment(ps::CpuNumericProfile profile) {
  const std::uint64_t raw[] = {UINT64_C(0x7ff0000000000001),
                               UINT64_C(0x8000000000000000),
                               UINT64_C(0x7ff0000000000000), 1};
  auto buffer = take(ps::BufferAllocator{}.allocate(sizeof(raw)));
  std::memcpy(buffer.data(), raw, sizeof(raw));
  const auto source = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {4}}, ps::Region::whole({4}), {0, {8}},
      std::move(buffer).freeze()));
  const auto starts = array({1}, {0}), steps = array({1}, {1});
  const int original = fegetround();
  for (int mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0, "set caller rounding mode");
    for (auto layout : {ps::numeric::TransformLayout::View,
                        ps::numeric::TransformLayout::Dense}) {
      for (unsigned kind = 0; kind < 3; ++kind) {
        const auto node =
            kind == 0 ? take(ps::numeric::reshape_node(
                            1, ps::WorkflowInputReference{1}, {2, 2}, layout,
                            profile))
            : kind == 1
                ? take(ps::numeric::transpose_node(
                      1, ps::WorkflowInputReference{1}, {0}, layout, profile))
                : take(ps::numeric::slice_node(1, ps::WorkflowInputReference{1},
                                               ps::WorkflowInputReference{2},
                                               ps::WorkflowInputReference{3},
                                               {4}, layout, profile));
        std::vector<ps::Value> inputs{source};
        if (kind == 2) {
          inputs.push_back(starts);
          inputs.push_back(steps);
        }
        require(feclearexcept(FE_ALL_EXCEPT) == 0 &&
                    feraiseexcept(FE_DIVBYZERO) == 0,
                "set prior floating flag");
        auto control = std::make_shared<point_math_checks::Control>();
        control->rounding = mode;
        point_math_checks::Workflow workflow(node, inputs, {}, control);
        auto value = take(workflow.run()).results.at("values");
        require(fegetround() == mode &&
                    fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO &&
                    control->computation_polls > 0,
                "layout preserves caller rounding and never evaluates sNaN");
        for (std::uint64_t j = 0; j < 4; ++j) {
          const auto coordinate = kind == 0
                                      ? std::vector<std::uint64_t>{j / 2, j % 2}
                                      : std::vector<std::uint64_t>{j};
          std::uint64_t bits = 0;
          require(
              rf::read(value, coordinate, &bits, 8).ok() && bits == raw[j],
              "raw sNaN/zero/infinity/subnormal bits survive all fenv modes");
        }
      }
    }
  }
  require(fesetround(original) == 0, "restore caller rounding mode");
  std::cout << "all layouts: sNaN/zero/infinity/subnormal and four caller "
               "rounding modes passed\n";
}
std::vector<std::uint64_t> list(const std::string& text) {
  std::vector<std::uint64_t> values;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find(',', start);
    values.push_back(std::stoull(text.substr(start, end - start)));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return values;
}
void oracle(ps::CpuNumericProfile profile) {
  std::string operation, shape_text, parameter_text, layout_text;
  unsigned dtype = 0;
  while (std::cin >> operation >> dtype >> shape_text >> parameter_text >>
         layout_text) {
    const auto shape = list(shape_text), parameters = list(parameter_text);
    std::size_t count = 1;
    for (auto extent : shape)
      count *= extent;
    const auto type = static_cast<ps::ElementType>(dtype);
    const auto width = ps::Value::element_size(type);
    auto buffer = take(ps::BufferAllocator{}.allocate(count * width));
    for (std::size_t i = 0; i < count; ++i) {
      std::uint64_t bits = 0;
      std::cin >> std::hex >> bits >> std::dec;
      std::memcpy(buffer.data() + i * width, &bits, width);
    }
    std::vector<std::int64_t> strides(shape.size());
    std::int64_t step = width;
    for (std::size_t j = shape.size(); j; --j) {
      strides[j - 1] = step;
      step *= shape[j - 1];
    }
    auto input =
        take(ps::Value::from_storage({type, shape}, ps::Region::whole(shape),
                                     {0, strides}, std::move(buffer).freeze()));
    const auto layout =
        layout_text == "view"    ? ps::numeric::TransformLayout::View
        : layout_text == "dense" ? ps::numeric::TransformLayout::Dense
                                 : ps::numeric::TransformLayout::Auto;
    std::vector<ps::Value> inputs{input};
    ps::WorkflowNode node;
    auto output_shape = parameters;
    if (operation == "reshape") {
      node = take(ps::numeric::reshape_node(1, ps::WorkflowInputReference{1},
                                            parameters, layout, profile));
    } else if (operation == "transpose") {
      node = take(ps::numeric::transpose_node(1, ps::WorkflowInputReference{1},
                                              parameters, layout, profile));
      for (std::size_t j = 0; j < parameters.size(); ++j)
        output_shape[j] = shape[parameters[j]];
    } else {
      std::vector<std::int64_t> starts(shape.size()), steps(shape.size());
      for (auto& value : starts)
        std::cin >> value;
      for (auto& value : steps)
        std::cin >> value;
      inputs.push_back(array({shape.size()}, starts));
      inputs.push_back(array({shape.size()}, steps));
      node = take(ps::numeric::slice_node(
          1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
          ps::WorkflowInputReference{3}, parameters, layout, profile));
    }
    Fixture fixture(std::move(node), inputs);
    auto result = fixture.run(take(ps::Footprint::all(output_shape)));
    if (!result.ok()) {
      require(
          result.status().code == ps::ErrorCode::InvalidArgument &&
              result.status().message.find("InvalidSlice") != std::string::npos,
          result.status().message.c_str());
      std::cout << "error\n";
      continue;
    }
    auto visited = take(ps::Footprint::all(output_shape))
                       .visit(
                           [&](const auto& coordinate) {
                             std::uint64_t bits = 0;
                             auto read =
                                 rf::read(result.value().results.at("values"),
                                          coordinate, &bits, width);
                             if (read.ok())
                               std::cout << std::hex << bits << ' ' << std::dec;
                             return read;
                           },
                           4096);
    require(visited.ok(), "layout oracle output read");
    std::cout << '\n';
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile must be strict, apple or x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      reshape(profile);
      transpose(profile);
      slice(profile);
      physical_layouts(profile);
      affine_oracle(profile);
      active_layout_budgets(profile);
      boundaries(profile);
      typed_and_lifetime(profile);
      floating_environment(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
