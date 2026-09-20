#include "photospider/numeric/layouts.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

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

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
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
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto name = "input" + std::to_string(i);
      const auto& value = inputs[i];
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
    }
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
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
    config.maximum_live_bytes = capacity;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    auto frozen = execution.freeze(plan.value().plan, bindings);
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    ps::ExecutionOptions options;
    options.dependencies.maximum_work = work;
    return execution.execute_fragments(frozen.value(), {{"values", demand}}, {},
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
        require(result.values.at("values").read({i, j}, &value, 8).ok() &&
                    value == 2 * i + j,
                "reshape logical sequence");
      }
    std::uint64_t viewed = 0, copied = 0;
    for (const auto& timing : result.diagnostics.operation_timings) {
      viewed += timing.numeric.view_elements;
      copied += timing.numeric.copied_elements;
    }
    require(layout == ps::numeric::TransformLayout::Dense
                ? copied == 6 && viewed == 0
                : viewed == 6 && copied == 0,
            "actual reshape representation counters");
  }
  // Direct bindings are dense. A public transpose node creates the physically
  // strided intermediate whose per-rectangle viewability reshape must inspect.
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
  auto regional = take(unavailable.run(one_row));
  require(regional.values.at("values").fragments().size() == 1,
          "a smaller request can be one affine view");
  unavailable.document.nodes[1].parameters["layout"] = std::string("auto");
  auto fallback = take(unavailable.run(take(ps::Footprint::all({3, 2}))));
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = 0; j < 2; ++j) {
      std::int64_t value = -1;
      require(fallback.values.at("values").read({i, j}, &value, 8).ok() &&
                  value == 2 * i + j,
              "auto packs the non-affine rectangle");
    }
  std::cout << "reshape: [2,3]->[3,2] raw logical order; per-rectangle "
               "view/auto/dense and counters passed\n";
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
        require(result.values.at("values").read({k, i, j}, &value, 8).ok() &&
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
    require(result.values.at("values").read({i}, &value, 8).ok() &&
                value == 4 - 2 * static_cast<std::int64_t>(i),
            "reverse stride slice values");
  }
  const auto support = take(result.dependencies.source_support());
  const auto source_points = take(ps::Footprint::from_regions(
      {6}, {ps::Region({{0, 1}}), ps::Region({{2, 1}}), ps::Region({{4, 1}})}));
  require(support.at("input0") == source_points,
          "nonunit slice never authorizes gaps");
  require(take(result.dependencies.potential_dirty(
                   "input0", take(ps::Footprint::from_regions(
                                 {6}, {ps::Region({{1, 1}})}))))
              .at("values")
              .empty(),
          "unread source gap stays clean");
  require(take(result.dependencies.potential_dirty(
                   "input0", take(ps::Footprint::from_regions(
                                 {6}, {ps::Region({{2, 1}})}))))
                  .at("values") ==
              take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})})),
          "slice inverse dirty selects one output");
  auto one = take(fixture.run(
      take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}))));
  require(
      one.values.at("values").fragments()[0].layout().byte_strides[0] == -16,
      "partial singleton ROI preserves used global slice stride");
  fixture.bindings.inputs[2].value = array({1}, {1});
  auto invalid = fixture.run(
      take(ps::Footprint::from_regions({3}, {ps::Region({{0, 1}})})));
  require(
      invalid.status().code == ps::ErrorCode::InvalidArgument &&
          invalid.status().message.find("InvalidSlice") != std::string::npos,
      "partial request still validates the full slice domain");
  fixture.document.nodes[0].parameters["counts"] = std::string("1");
  unsigned step_reads = 0;
  auto failing = std::make_shared<ps::RegionalSource>();
  failing->descriptor = fixture.bindings.inputs[2].value.descriptor();
  failing->read = [&](const auto&, auto*, auto, const auto&, const auto&) {
    ++step_reads;
    return ps::Result<ps::Region>(
        ps::Status{ps::ErrorCode::OperationFailed, "ignored singleton step"});
  };
  fixture.bindings.inputs[2].value = {};
  fixture.bindings.inputs[2].source = failing;
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  auto singleton = take(execution.execute(plan.plan, fixture.bindings));
  std::int64_t value = 0;
  const auto& single_value = singleton.values.at("values");
  std::memcpy(
      &value,
      single_value.bytes().data() + take(single_value.byte_address({0})), 8);
  require(step_reads == 0 && value == 4,
          "singleton count ignores failing step producer");
  std::cout << "slice: [4,2,0], exact support/dirty, full-domain validation "
               "and ignored singleton step passed\n";
}
void physical_layouts(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
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
      const std::vector<ps::Value> inputs{source};
      const std::vector<ps::Region> demands{source.region()};
      ps::OperationInvocation call(inputs, demands, node.parameters);
      auto result = take(registry->invoke(node.operation, call));
      for (std::uint64_t j = 0; j < 3; ++j)
        for (std::uint64_t i = 0; i < 2; ++i) {
          std::int64_t value = -1;
          std::memcpy(&value,
                      result.bytes().data() + take(result.byte_address({j, i})),
                      8);
          require(value == raw[(zero ? 1 : 1 - i) * 3 + j],
                  "unaligned negative/zero stride transpose");
        }
      require(layout == ps::numeric::TransformLayout::Dense ||
                  result.storage() == owner,
              "public invoke retains actual source owner for a view");
    }
  }
  for (bool shared : {true, false}) {
    for (auto layout : {ps::numeric::TransformLayout::View,
                        ps::numeric::TransformLayout::Auto,
                        ps::numeric::TransformLayout::Dense}) {
      auto node = take(ps::numeric::reshape_node(
          1, ps::WorkflowInputReference{1}, {4}, layout, profile));
      ps::DependencyRequest request;
      request.inputs = {{{ps::ElementType::Int64, {2, 2}}, {}}};
      request.parameters = node.parameters;
      request.outputs = take(ps::Footprint::all({4}));
      request.snapshot_identity = "layout-fragments";
      auto session = take(registry->start_dependency(node.operation, request));
      require(session->poll().ok(), "fragmented reshape initial need");
      std::vector<ps::Value> pieces;
      for (std::uint64_t row = 0; row < 2; ++row) {
        auto storage = shared ? owner : array({2}, {2, 3}).storage();
        pieces.push_back(take(ps::Value::from_storage(
            request.inputs[0].descriptor, ps::Region({{row, 1}, {0, 2}}),
            {shared ? 1 + 16 * row : 0, {0, 8}, {row, 0}}, storage)));
      }
      auto supplied = take(
          ps::ValueFragments::create(request.inputs[0].descriptor, {},
                                     take(ps::Footprint::all({2, 2})), pieces));
      require(supplied.fragments().size() == 2,
              "same-owner fixture must preserve two distinct address maps");
      require(session->supply({supplied}, request.snapshot_identity).ok(),
              "fragmented reshape supply");
      auto done = session->poll();
      if (!shared && layout == ps::numeric::TransformLayout::View) {
        require(
            done.status().message.find("ViewUnavailable") != std::string::npos,
            "one requested rectangle cannot borrow multiple owners");
      } else {
        auto answer = take(std::move(done));
        const auto& result = std::get<ps::DependencyResult>(answer).value;
        require(result.fragments().size() == 1,
                "one requested rectangle produces one view or dense copy");
        for (std::uint64_t i = 0; i < 4; ++i) {
          std::int64_t value = -1;
          require(
              result.read({i}, &value, 8).ok() &&
                  value == static_cast<std::int64_t>(shared ? i : 2 + i % 2),
              "multi-fragment logical order");
        }
        require(!shared || layout == ps::numeric::TransformLayout::Dense ||
                    result.fragments()[0].storage() == owner,
                "joint affine proof preserves same owner");
      }
    }
  }
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
  auto stopped = fixture.run(take(ps::Footprint::all({3, 2})), 262144, 1);
  require(stopped.status().code == ps::ErrorCode::ResourceExhausted &&
              stopped.status().reason == ps::FailureReason::WorkLimit,
          "layout work exhaustion is not an auto fallback");
  auto registry = fixture.registry;
  ps::DependencyRequest request;
  request.inputs = {{fixture.bindings.inputs[0].value.descriptor(), {}}};
  request.parameters = reshape.parameters;
  request.outputs = take(ps::Footprint::none({3, 2}));
  request.snapshot_identity = "layout-boundaries";
  auto empty = take(registry->start_dependency(reshape.operation, request));
  require(std::holds_alternative<ps::DependencyResult>(take(empty->poll())) &&
              empty->poll_count() == 0,
          "Empty does not start arithmetic or source reads");
  request.outputs = take(ps::Footprint::all({3, 2}));
  ps::CancellationSource cancellation;
  request.cancellation = cancellation.token();
  auto cancelled = take(registry->start_dependency(reshape.operation, request));
  cancellation.cancel();
  require(cancelled->poll().status().code == ps::ErrorCode::Cancelled,
          "cancel before layout poll");
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
  require(view.values.at("values").read({63, 63}, &last, 8).ok() && last == 7 &&
              take(view.values.at("values").retained_bytes()) == 8,
          "view admission never reserves the 32768-byte dense output");
  large.document.nodes[1].parameters["layout"] = std::string("dense");
  auto dense = large.run(take(ps::Footprint::all({64, 64})), 4096);
  require(dense.status().code == ps::ErrorCode::ResourceExhausted,
          "dense output admits its actual payload capacity");
  std::cout << "schema, Empty, WorkLimit, cancellation and low-budget "
               "public constant/transpose view passed\n";
}
void typed_and_lifetime(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
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
    ps::DependencyRequest request;
    request.inputs = {{{ps::ElementType::Float32, {1, 1, 4}}, {facet}}};
    request.parameters = node.parameters;
    request.snapshot_identity = "layout-typed";
    request.outputs =
        kind == 0
            ? take(ps::Footprint::from_regions({4}, {ps::Region({{0, 1}})}))
        : kind == 1 ? take(ps::Footprint::from_regions(
                          {4, 1, 1}, {ps::Region({{0, 1}, {0, 1}, {0, 1}})}))
                    : take(ps::Footprint::all({1, 1, 1}));
    if (kind == 2) {
      request.inputs.push_back({{ps::ElementType::Int64, {3}}, {}});
      request.inputs.push_back({{ps::ElementType::Int64, {3}}, {}});
    }
    auto session = take(registry->start_dependency(node.operation, request));
    require(session->poll().ok(), "typed layout initial need");
    if (kind == 2) {
      auto controls = array({3}, {0, 0, 0});
      auto none = take(
          ps::ValueFragments::create(request.inputs[0].descriptor, {facet},
                                     take(ps::Footprint::none({1, 1, 4})), {}));
      auto starts = take(ps::ValueFragments::create(
          controls.descriptor(), {}, take(ps::Footprint::all({3})),
          {controls}));
      auto unused = take(ps::ValueFragments::create(
          controls.descriptor(), {}, take(ps::Footprint::none({3})), {}));
      require(session->supply({none, starts, unused}, request.snapshot_identity)
                  .ok(),
              "typed singleton slice control stage ignores every step");
      require(session->poll().ok(), "typed slice source stage");
    }
    bool data = false, validation = false;
    for (const auto& need : take(session->pending_reads())) {
      if (need.port != 0)
        continue;
      if (need.roles == 1)
        data = take(need.samples.element_count()) == 1;
      if (need.roles == 4)
        validation = take(need.samples.element_count()) == 4;
    }
    require(
        data && validation,
        "each layout has one data sample and complete typed-pixel validation");
  }
  Fixture fixture(take(ps::numeric::reshape_node(
                      1, ps::WorkflowInputReference{1}, {3, 2},
                      ps::numeric::TransformLayout::View, profile)),
                  {array({2, 3}, {0, 1, 2, 3, 4, 5})});
  ps::Value held;
  std::optional<ps::ResourceBudget> root;
  {
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    root = take(execution.resource_budget());
    auto frozen = take(execution.freeze(plan.plan, fixture.bindings));
    auto result = take(execution.execute_fragments(
        frozen, {{"values", take(ps::Footprint::all({3, 2}))}}));
    held = result.values.at("values").fragments()[0];
  }
  require(root->statistics().live[ps::ResourceKind::Metadata] > 0,
          "ordinary returned Value retains publication metadata after context");
  std::int64_t value = -1;
  std::memcpy(&value, held.bytes().data() + take(held.byte_address({2, 1})), 8);
  require(value == 5, "returned view survives result/context destruction");
  held = {};
  require(root->statistics().live[ps::ResourceKind::Metadata] == 0 &&
              root->statistics().live[ps::ResourceKind::Payload] == 0,
          "final publication owner releases all admitted metadata and payload");
  std::cout << "all layout typed-validation closures and escaped Value "
               "publication lifetime passed\n";
}
void floating_environment(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
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
        std::vector<ps::Region> demands{source.region()};
        if (kind == 2) {
          inputs.push_back(starts);
          inputs.push_back(steps);
          demands.push_back(starts.region());
          demands.push_back(steps.region());
        }
        ps::OperationInvocation call(inputs, demands, node.parameters);
        require(feclearexcept(FE_ALL_EXCEPT) == 0 &&
                    feraiseexcept(FE_DIVBYZERO) == 0,
                "set prior floating flag");
        auto value = take(registry->invoke(node.operation, call));
        require(
            fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "layout preserves caller rounding and never evaluates sNaN");
        for (std::uint64_t j = 0; j < 4; ++j) {
          const auto coordinate = kind == 0
                                      ? std::vector<std::uint64_t>{j / 2, j % 2}
                                      : std::vector<std::uint64_t>{j};
          require(
              std::memcmp(
                  value.bytes().data() + take(value.byte_address(coordinate)),
                  &raw[j], 8) == 0,
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
                                 result.value().values.at("values").read(
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
