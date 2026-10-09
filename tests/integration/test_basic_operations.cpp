#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "foundations_workflow/workflow.hpp"
#include "numeric_workflow/result_fixture.hpp"
#include "support/execution_sync_fixture.hpp"

namespace {
using namespace foundations;  // NOLINT(build/namespaces)
using ps::ErrorCode;
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok()) {
    throw std::runtime_error(
        "basic status code=" +
        std::to_string(static_cast<int>(result.status().code)) +
        " reason=" + std::to_string(static_cast<int>(result.status().reason)) +
        " " + result.status().message);
  }
  return result.take_value();
}

ps::Result<ps::Value> finite(const std::string& key,
                             const std::vector<ps::Value>& inputs,
                             ps::Region region = {},
                             Parameters parameters = {}) {
  // Values construct private numeric fixtures and hold the packed oracle.
  // The operation itself receives Result bindings and publishes a Result.
  auto registry = ps::make_default_operation_registry();
  ps::WorkflowDocument doc;
  numeric_result_fixture::declare_sources(&doc, inputs);
  std::vector<ps::WorkflowInput> refs;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    refs.push_back(ps::WorkflowInputReference{i + 1});
  }
  doc.nodes = {{1, key, refs, std::move(parameters)}};
  doc.outputs = {{"result", 1, "value"}};
  ps::GraphContext graph(doc);
  auto compiled = ps::Compiler(registry).compile(graph);
  if (!compiled.ok()) {
    return ps::Result<ps::Value>(compiled.status());
  }
  ps::ExecutionContextConfig config;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  auto root = take(execution.resource_budget());
  const auto& tensor =
      compiled.value().plan.steps()[0].output_result_schema->tensors[0];
  if (region.empty()) {
    region = ps::Region::whole(tensor.sample_shape());
  }
  auto query =
      take(ps::Footprint::from_regions(tensor.sample_shape(), {region}));
  auto frozen = execution.freeze(
      compiled.value().plan,
      numeric_result_fixture::bind_sources(root, inputs, &doc));
  if (!frozen.ok()) {
    return ps::Result<ps::Value>(frozen.status());
  }
  auto result =
      execution.execute_fragments(frozen.value(), {{"result", query}});
  if (!result.ok()) {
    auto status = result.status();
    status.message = key + ": " + status.message;
    return ps::Result<ps::Value>(std::move(status));
  }
  const auto& output = result.value().results.at("result");
  auto packed = take(ps::MutableValue::allocate(
      {tensor.descriptor.element_type, tensor.sample_shape()}, region,
      ps::BufferAllocator{}));
  const auto width = ps::Value::element_size(tensor.descriptor.element_type);
  std::uint64_t index = 0;
  auto status = query.visit(
      [&](const auto& at) {
        return numeric_result_fixture::read(
            output, at, packed.data() + index++ * width, width);
      },
      UINT64_MAX);
  if (!status.ok()) {
    return ps::Result<ps::Value>(status);
  }
  return std::move(packed).publish();
}
ps::Result<ps::Value> operation(const std::string& key,
                                const std::vector<ps::Value>& inputs,
                                Parameters parameters = {}) {
  return finite(key, inputs, {}, std::move(parameters));
}
ps::Value output(ps::Result<ps::Value> value) {
  return take(std::move(value));
}
void rejected(const ps::Result<ps::Value>& value, ErrorCode expected) {
  require(!value.ok() && value.status().code == expected,
          "unexpected basic Result failure: " + value.status().message +
              " code=" + std::to_string(static_cast<int>(value.status().code)));
}
Parameters domain(double a = 0, double b = 1) {
  return {{"domain_min", a},
          {"domain_max", b},
          {"out_of_domain", std::string("reject")}};
}
Parameters samples(std::int64_t n = 5) {
  auto p = domain();
  p["count"] = n;
  return p;
}
Parameters levels(double gamma = 1) {
  return {{"black", 0.},
          {"white", 1.},
          {"gamma", gamma},
          {"out_min", 0.},
          {"out_max", 1.}};
}
void curves() {
  const auto controls = array<float>({0, 0, .5, .25, 1, 1}, {3, 2});
  exact<float>(output(operation("curve.sample_linear", {controls}, samples())),
               {0, .125, .25, .625, 1});
  exact<float>(
      output(operation("curve.sample_monotone", {controls}, samples())),
      {0, .078125, .25, .546875, 1});
  const auto flat = array<double>({0, 2, .3, 2, .6, -1, 1, -1}, {4, 2});
  const auto values =
      output(operation("curve.sample_monotone", {flat}, samples(101)));
  double last = 2;
  for (unsigned i = 0; i <= 100; ++i) {
    double value;
    std::memcpy(&value, values.bytes().data() + 8 * i, 8);
    require(value <= last && value >= -1, "PCHIP shape oracle");
    if (i <= 30) {
      require(value == 2, "PCHIP first plateau");
    }
    if (i >= 60) {
      require(value == -1, "PCHIP last plateau");
    }
    last = value;
  }
  auto p = samples(3);
  for (const auto* key : {"curve.sample_linear", "curve.sample_monotone"}) {
    exact<double>(
        output(operation(key, {array<double>({0, -1, 1, 1}, {2, 2})}, p)),
        {-1, 0, 1});
    rejected(operation(key, {array<float>({0, 0, 0, 1}, {2, 2})}, p),
             ErrorCode::OperationFailed);
    rejected(operation(key, {array<float>({0, 0}, {1, 2})}, p),
             ErrorCode::TypeMismatch);
    rejected(operation(key, {array<float>({0, 0, 1, INFINITY}, {2, 2})}, p),
             ErrorCode::OperationFailed);
    auto bad = p;
    bad["count"] = std::int64_t{1};
    rejected(operation(key, {controls}, bad), ErrorCode::InvalidArgument);
    bad = p;
    bad["domain_min"] = -1.;
    rejected(operation(key, {controls}, bad), ErrorCode::OperationFailed);
    bad["out_of_domain"] = std::string("clip");
    exact<float>(output(operation(key, {controls}, bad)), {0, 0, 1});
  }
  const auto q = array<float>({0, .25, .5, 1}, {2, 2});
  const auto table = array<float>({0, .25, 1});
  exact<float>(output(operation("field.apply_lut_1d", {q, table}, domain())),
               {0, .125, .25, 1});
  rejected(
      operation("field.apply_lut_1d", {q, array<double>({0, 1})}, domain()),
      ErrorCode::TypeMismatch);
  rejected(
      operation("field.apply_lut_1d", {q, array<float>({0, NAN})}, domain()),
      ErrorCode::OperationFailed);
  rejected(operation("field.apply_lut_1d", {q, table}, domain(1, 0)),
           ErrorCode::InvalidArgument);
  auto clip = domain();
  clip["out_of_domain"] = std::string("clip");
  exact<float>(output(operation("field.apply_lut_1d",
                                {array<float>({-1, 2}, {1, 2}), table}, clip)),
               {0, 1});
  const double max = std::numeric_limits<double>::max();
  exact<double>(
      output(operation("field.apply_lut_1d",
                       {array<double>({0}, {1, 1}), array<double>({-max, max})},
                       domain(-max, max))),
      {0});
  exact<float>(
      output(operation("field.apply_lut_1d",
                       {array<float>({0}, {1, 1}),
                        array<float>({-std::numeric_limits<float>::max(),
                                      std::numeric_limits<float>::max()})},
                       domain(-1, 1))),
      {0});
  exact<float>(
      output(operation("field.apply_lut_1d",
                       {array<float>({0}, {1, 1}), array<float>({1e30F, 0})},
                       domain(-1, 0x1p-52))),
      {222044608266240.F});
  exact<float>(output(operation("field.apply_lut_1d",
                                {array<float>({0}, {1, 1}),
                                 array<float>({0x1p100F, -0x1p48F})},
                                domain(-1, 0x1p-52))),
               {0});
  exact<float>(output(operation("field.apply_lut_1d",
                                {array<float>({2}, {1, 1}),
                                 array<float>({0x1p101F, -3 * 0x1p100F})},
                                domain(0, 5))),
               {0});
  for (const auto* key : {"curve.sample_linear", "curve.sample_monotone"}) {
    auto p = domain(0, 5);
    p["count"] = std::int64_t{6};
    const auto result = output(operation(
        key, {array<float>({0, 0x1p101F, 5, -3 * 0x1p100F}, {2, 2})}, p));
    float middle;
    std::memcpy(&middle, result.bytes().data() + 8, 4);
    require(middle == 0, "integer-domain weighted cancellation");
    exact<float>(
        output(operation(key, {array<float>({0, 0.F, .5F, -0.F, 1, 1}, {3, 2})},
                         samples(3))),
        {0.F, -0.F, 1});
  }
  for (double amplitude : {2., 6.}) {
    const double tiny = amplitude * std::numeric_limits<double>::denorm_min();
    exact<double>(
        output(operation(
            "field.apply_lut_1d",
            {array<double>({.5}, {1, 1}), array<double>({tiny, 0})}, domain())),
        {tiny / 2});
    for (const auto* key : {"curve.sample_linear", "curve.sample_monotone"}) {
      exact<double>(
          output(operation(key, {array<double>({0, tiny, 1, 0}, {2, 2})},
                           samples(3))),
          {tiny, tiny / 2, 0});
    }
    auto p = levels();
    p["out_min"] = 0.;
    p["out_max"] = tiny;
    exact<double>(
        output(operation("grade.levels", {array<double>({.5}, {1, 1})}, p)),
        {tiny / 2});
  }
}
void fields() {
  const auto input = array<float>({0, .25, .5, 1}, {2, 2});
  exact<float>(output(operation("grade.levels", {input}, levels(2))),
               {0, .5, std::sqrt(.5F), 1});
  auto bad = levels();
  bad["gamma"] = 0.;
  rejected(operation("grade.levels", {input}, bad), ErrorCode::InvalidArgument);
  bad = levels();
  bad["white"] = 0.;
  rejected(operation("grade.levels", {input}, bad), ErrorCode::InvalidArgument);
  exact<float>(output(operation("field.smoothstep", {input},
                                {{"edge0", 0.}, {"edge1", 1.}})),
               {0, .15625F, .5F, 1});
  rejected(
      operation("field.smoothstep", {input}, {{"edge0", 1.}, {"edge1", 0.}}),
      ErrorCode::InvalidArgument);
  for (const auto* key : {"numeric.minimum", "numeric.maximum"}) {
    const bool minimum = std::string(key) == "numeric.minimum";
    exact<double>(take(finite(key, {array<double>({-0., 1, -3}),
                                    array<double>({0., 2, -4})})),
                  {minimum ? -0. : 0., minimum ? 1. : 2., minimum ? -4. : -3.});
    require(
        finite(key, {array<double>({1}), array<float>({1})}).status().code ==
            ErrorCode::TypeMismatch,
        "finite dtype mismatch");
  }
  exact<double>(take(finite("numeric.abs", {array<double>({-0., -3, 4})})),
                {0., 3, 4});
  require(finite("numeric.abs", {array<float>({NAN})}).status().code ==
              ErrorCode::OperationFailed,
          "finite nonfinite input");
  auto cancellation_levels = levels();
  cancellation_levels["white"] = 5.;
  cancellation_levels["out_min"] = -3 * 0x1p100;
  cancellation_levels["out_max"] = 0x1p101;
  exact<float>(output(operation("grade.levels", {array<float>({3}, {1, 1})},
                                cancellation_levels)),
               {0});
}
void histograms() {
  Parameters hist{{"bins", std::int64_t{4}},
                  {"range_min", 0.},
                  {"range_max", 1.}};
  const auto values = array<double>({-1, 0, .25, .5, .75, 1, 2}, {1, 7});
  exact<std::int64_t>(output(operation("analysis.histogram", {values}, hist)),
                      {1, 1, 1, 2});
  hist.erase("bins");
  exact<std::int64_t>(
      output(operation("analysis.histogram_out_of_range", {values}, hist)),
      {1, 1});
  rejected(operation("analysis.histogram_out_of_range",
                     {array<float>({INFINITY}, {1, 1})}, hist),
           ErrorCode::OperationFailed);
  std::vector<std::int64_t> boundary_counts(100, 0);
  boundary_counts[29] = 2;
  boundary_counts[30] = 2;
  boundary_counts[58] = 1;
  exact<std::int64_t>(
      output(operation("analysis.histogram",
                       {array<double>({29, 58, std::nextafter(30., 0.), 30.,
                                       std::nextafter(30., 100.)},
                                      {1, 5})},
                       {{"bins", std::int64_t{100}},
                        {"range_min", 0.},
                        {"range_max", 100.}})),
      boundary_counts);
}
void regional() {
  std::vector<float> data(35);
  for (unsigned i = 0; i < data.size(); ++i) {
    data[i] = static_cast<float>(i % 11) - 5;
  }
  auto input = array(data, {5, 7});
  for (const auto* key : {"grade.levels", "numeric.abs"}) {
    Parameters p;
    if (std::string(key) == "grade.levels") {
      p = levels();
    }
    const auto whole = take(finite(key, {input}, {}, p));
    ps::PlanningOptions options;
    options.tile_height = 1;
    options.tile_width = 2;
    options.output_regions["result"] = ps::Region({{1, 3}, {2, 4}});
    const auto roi =
        take(finite(key, {input}, options.output_regions["result"], p));
    std::vector<float> expected;
    for (unsigned y = 1; y < 4; ++y) {
      for (unsigned x = 2; x < 6; ++x) {
        float number;
        std::memcpy(&number, whole.bytes().data() + (y * 7 + x) * 4, 4);
        expected.push_back(number);
      }
    }
    exact<float>(roi, expected);
  }
}

void views_and_failures() {
  auto registry = ps::make_default_operation_registry();
  std::vector<std::uint8_t> bytes(13);
  const float data[] = {1, -2, 3};
  std::memcpy(bytes.data() + 1, data, 12);
  auto view = take(ps::Value::create({ps::ElementType::Float32, {1, 3}},
                                     ps::Region::whole({1, 3}),
                                     {1, {0, -4}, {0, 2}}, bytes));
  exact<float>(take(finite("numeric.abs", {view})), {3, 2, 1});
  const float value = .5;
  std::memcpy(bytes.data() + 1, &value, 4);
  // A partial view retains full descriptor coordinates and a nonzero origin.
  auto partial = take(ps::Value::create({ps::ElementType::Float32, {9, 9}},
                                        ps::Region({{4, 1}, {5, 3}}),
                                        {1, {0, 4}, {4, 5}}, bytes));
  exact<float>(take(finite("numeric.abs", {partial}, partial.region())),
               {.5, 2, 3});
  const auto controls = array<double>({0, 0, .5, .25, 1, 1}, {3, 2});
  ps::WorkflowDocument doc;
  numeric_result_fixture::declare_sources(&doc, {controls});
  doc.nodes = {
      {1, "curve.sample_monotone", {ps::WorkflowInputReference{1}}, samples()}};
  doc.outputs = {{"result", 1, "value"}};
  ps::GraphContext graph(doc);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext execution(registry, {1, false, 8, 4096, 2048});
  auto sources = take(execution.resource_budget());
  auto first = numeric_result_fixture::bind_sources(sources, {controls}, &doc);
  const auto changed = array<double>({0, 1, .5, 1, 1, 1}, {3, 2});
  auto second = numeric_result_fixture::bind_sources(sources, {changed}, &doc);
  auto execute = [&](ps::ExecutionContext& context,
                     const ps::ExecutionBindings& bindings,
                     ps::CancellationToken token = {}) {
    return context.execute(compiled.plan, bindings, token);
  };
  auto check_result = [&](const ps::ExecutionResult& result,
                          const std::vector<double>& expected) {
    const auto& object = result.results.at("result");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      double number = 0;
      require(numeric_result_fixture::read(object, {i}, &number, 8).ok() &&
                  std::memcmp(&number, &expected[i], 8) == 0,
              "rebound PCHIP oracle");
    }
  };
  check_result(take(execute(execution, first)), {0, .078125, .25, .546875, 1});
  check_result(take(execute(execution, second)), std::vector<double>(5, 1));
  for (auto bytes_limit : {UINT64_C(64), UINT64_C(144)}) {
    ps::ExecutionContext limited(registry, {1, false, 8, bytes_limit});
    auto root = take(limited.resource_budget());
    auto limited_inputs =
        numeric_result_fixture::bind_sources(root, {controls}, &doc);
    const auto baseline = root.statistics().live[ps::ResourceKind::Payload];
    auto failed = execute(limited, limited_inputs);
    require(!failed.ok() &&
                failed.status().code == ErrorCode::ResourceExhausted &&
                root.statistics().live[ps::ResourceKind::Payload] == baseline,
            "failed PCHIP output/scratch must release payload");
  }
  ps::CancellationSource stopped;
  stopped.cancel();
  require(execute(execution, first, stopped.token()).status().code ==
              ErrorCode::Cancelled,
          "public Result execution cancellation");
  const int rounding = std::fegetround();
  require(std::fesetround(FE_DOWNWARD) == 0, "set rounding fixture");
  auto rounded =
      operation("grade.levels", {array<double>({.25}, {1, 1})}, levels(2));
  const bool restored = std::fegetround() == FE_DOWNWARD;
  std::fesetround(rounding);
  exact<double>(output(std::move(rounded)), {.5});
  require(restored, "numeric environment must be restored");
}
void result_contracts() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = make_default_operation_registry();
  ExecutionContext context(registry, {1, false, 8, 1048576});
  auto root = take(context.resource_budget());
  auto packed = array<float>({-1, 0, .25F, .5F, 1, 2}, {2, 3});
  auto schema = numeric_result_fixture::source_schema(packed);
  schema.tensors[0].atomic_trailing_axes = 1;
  auto input = numeric_result_fixture::source(root, packed, &schema);
  auto prepare = [&](const std::string& key,
                     const std::vector<ResultRef>& inputs, Parameters params) {
    WorkflowDocument doc;
    WorkflowNode node;
    node.id = 7;
    node.operation = key;
    node.parameters = std::move(params);
    ExecutionBindings bindings;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration declaration;
      declaration.id = 11 + i;
      declaration.name = "input" + std::to_string(i);
      declaration.result_schema =
          std::make_shared<SchemaTemplate>(inputs[i].schema());
      doc.inputs.push_back(declaration);
      node.inputs.push_back(WorkflowInputReference{declaration.id});
      bindings.inputs.push_back({declaration.name, inputs[i]});
    }
    doc.nodes = {node};
    doc.outputs = {{"out", 7, "value"}};
    auto graph = std::make_shared<GraphContext>(doc);
    auto compiled = take(Compiler(registry).compile(*graph));
    return std::make_pair(graph, take(context.freeze(compiled.plan, bindings)));
  };
  auto smoothing =
      prepare("field.smoothstep", {input}, {{"edge0", 0.}, {"edge1", 1.}});
  auto point = take(Footprint::from_regions(
      {1, 1, 2, 3}, {Region({{0, 1}, {0, 1}, {1, 1}, {1, 1}})}));
  auto result =
      take(context.execute_fragments(smoothing.second, {{"out", point}}));
  const auto& output = result.results.at("out");
  require(output.schema().id == "photospider.image" &&
              output.schema().tensors[0].key == "pixels" &&
              output.schema().tensors[0].batch_axes ==
                  ResourceVector<std::uint64_t>({1, 1}),
          "smoothstep publishes canonical frame/layer coverage Result");
  float sample = 0;
  require(
      output.read_tensor(take(output.descriptor()), 0, {0, 0, 1, 1}, &sample, 4)
              .ok() &&
          sample == 1,
      "smoothstep sparse sample");
  const auto change =
      take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {0, 1}})}));
  auto data_dirty = take(result.dependencies.potential_dirty(
      "input0", change, 1, {}, ResultSupportTarget::Tensor, 0));
  auto validation_dirty = take(result.dependencies.potential_dirty(
      "input0", change, 4, {}, ResultSupportTarget::Tensor, 0));
  require(data_dirty.at("out").empty() && validation_dirty.at("out") == point,
          "basic Data Q and atomic Validation closure map separate footprints");
  const auto row =
      take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {0, 3}})}));
  require(take(result.dependencies.source_support()).at("input0") == row,
          "basic support contains only requested validation row");
  auto small_identity = prepare("core.identity", {input}, {});
  auto digested = take(context.execute(small_identity.second));
  require(!digested.diagnostics.result_digest.empty(),
          "small Result output receives a complete content digest");
  for (const auto limit : {UINT64_C(0), UINT64_C(5)}) {
    ExecutionOptions options;
    options.maximum_result_digest_samples = limit;
    auto omitted = take(context.execute(small_identity.second, {}, options));
    float unchanged = 0;
    const auto& tensor = omitted.results.at("out");
    require(omitted.diagnostics.result_digest.empty() &&
                tensor
                    .read_tensor(take(tensor.descriptor()), 0, {1, 1},
                                 &unchanged, 4)
                    .ok() &&
                unchanged == 1,
            "disabled or insufficient digest quota preserves Result execution");
  }
  auto empty = take(context.execute_fragments(
      smoothing.second, {{"out", take(Footprint::none({1, 1, 2, 3}))}}));
  require(
      take(empty.results.at("out").descriptor()).tensor_coverage(0).empty() &&
          take(empty.dependencies.source_support()).empty(),
      "Empty smoothstep performs no input observations");
  auto controls = numeric_result_fixture::source(
      root, array<double>({0, 0, .5, NAN, 1, 1}, {3, 2}));
  auto table = numeric_result_fixture::source(root, array<float>({0, NAN, 1}));
  for (const auto* key :
       {"curve.sample_linear", "curve.sample_monotone", "field.apply_lut_1d",
        "grade.levels", "analysis.histogram",
        "analysis.histogram_out_of_range"}) {
    std::vector<ResultRef> inputs{input};
    Parameters params;
    if (std::string(key).find("curve.") == 0) {
      inputs = {controls};
      params = samples();
    } else if (std::string(key) == "field.apply_lut_1d") {
      inputs.push_back(table);
      params = domain();
    } else if (std::string(key) == "grade.levels") {
      params = levels();
    } else {
      params = {{"range_min", 0.}, {"range_max", 1.}};
      if (std::string(key) == "analysis.histogram") {
        params["bins"] = INT64_C(4);
      }
    }
    auto frozen = prepare(key, inputs, params);
    const auto shape = frozen.second.plan()
                           .steps()[0]
                           .output_result_schema->tensors[0]
                           .sample_shape();
    auto no_data = take(context.execute_fragments(
        frozen.second, {{"out", take(Footprint::none(shape))}}));
    require(take(no_data.results.at("out").descriptor())
                    .tensor_coverage(0)
                    .empty() &&
                take(no_data.dependencies.source_support()).empty(),
            "Empty basic Whole/Dependency output skips payload");
    ExecutionOptions exhausted;
    exhausted.maximum_dependency_work = 1;
    auto limited = context.execute_fragments(
        frozen.second, {{"out", take(Footprint::all(shape))}}, {}, exhausted);
    require(
        !limited.ok() && limited.status().code == ErrorCode::ResourceExhausted,
        "basic execution work quota is enforced");
  }
  auto bad_schema = schema;
  bad_schema.fields.push_back({"extra", ElementType::Float64, {}, {}});
  OperationMetadata metadata;
  metadata.result_schema = std::make_shared<SchemaTemplate>(bad_schema);
  auto bad = registry->resolve_traits("grade.levels", {metadata}, levels());
  require(!bad.ok() && bad.status().code == ErrorCode::TypeMismatch,
          "basic Result rejects scalar fields");
  bad_schema = schema;
  bad_schema.tensors[0].batch_axes = {2, 1};
  metadata.result_schema = std::make_shared<SchemaTemplate>(bad_schema);
  bad = registry->resolve_traits("grade.levels", {metadata}, levels());
  require(!bad.ok() && bad.status().code == ErrorCode::TypeMismatch,
          "basic field rejects multiple frames");
  auto broadcast =
      numeric_result_fixture::source_schema(array<float>({.5F}, {1, 1}));
  broadcast.tensors[0].descriptor.shape = {1000000, 1000000};
  auto storage = take(root.allocator().allocate(4));
  const float number = .5F;
  std::memcpy(storage.data(), &number, 4);
  auto builder = take(ResultBuilder::start(root, broadcast, "basic.broadcast"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "broadcast descriptor");
  require(builder
              .publish_tensor(0, Region::whole({1000000, 1000000}), {0, {0, 0}},
                              std::move(storage).freeze(),
                              take(ResultRelation::cartesian(
                                  root, UINT64_C(1000000000000), {})),
                              {true, true, true, true})
              .ok(),
          "broadcast source");
  auto huge = take(builder.seal());
  auto preserving = prepare("core.identity", {huge}, {});
  auto alias = take(context.execute(preserving.second));
  require(alias.results.at("out").schema().tensors[0].sample_shape() ==
                  std::vector<std::uint64_t>({1000000, 1000000}) &&
              alias.diagnostics.result_digest.empty(),
          "optional digest skips huge logical views without failing execution");

  auto counting =
      prepare("analysis.histogram", {huge},
              {{"range_min", 0.}, {"range_max", 1.}, {"bins", INT64_C(4)}});
  const auto baseline_payload = root.statistics().live[ResourceKind::Payload];
  ps::test::PhaseWorkGate gate("analysis.histogram", 1024);
  CancellationSource stop;
  std::future<ps::Result<ps::ExecutionResult>> active;
  ps::test::OnExit cleanup([&] {
    stop.cancel();
    gate.release();
    if (active.valid())
      active.wait();
  });
  active = std::async(std::launch::async, [&] {
    return context.execute(counting.second, stop.token());
  });
  const bool progressed = gate.wait();
  stop.cancel();
  gate.release();
  auto cancelled = active.get();
  require(progressed && gate.cancelled_in_service() && !cancelled.ok() &&
              cancelled.status().code == ErrorCode::Cancelled &&
              root.statistics().live[ResourceKind::Payload] == baseline_payload,
          "active histogram cancellation releases unpublished counters");
}

}  // namespace
int main() {
  try {
    curves();
    fields();
    histograms();
    regional();
    views_and_failures();
    result_contracts();
    std::cout << "basic operator public oracles passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
