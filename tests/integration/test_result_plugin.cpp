#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/unified_result_workflow/minimal_ops.hpp"
#include "photospider/photospider.hpp"
namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}
WorkflowDocument document(const std::string& key, std::int64_t mode = 0) {
  WorkflowDocument doc;
  auto schema =
      std::make_shared<SchemaTemplate>(unified_example::image_schema());
  WorkflowInputDeclaration a;
  a.id = 1;
  a.name = "image";
  a.result_schema = schema;
  WorkflowInputDeclaration c;
  c.id = 2;
  c.name = "control";
  c.descriptor = {ElementType::Int64, {1}};
  c.region = Region::whole({1});
  c.layout.byte_strides = {8};
  doc.inputs = {a, c};
  doc.nodes = {{1,
                key,
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                {{"mode", mode}}}};
  doc.outputs = {{"out", 1, "image"}};
  return doc;
}
void run() {
  for (const auto* path : {PS_BAD_RESULT_1, PS_BAD_RESULT_2, PS_BAD_RESULT_3,
                           PS_BAD_RESULT_4, PS_BAD_RESULT_5, PS_BAD_RESULT_6}) {
    OperationRegistry registry;
    require(!registry.load_plugin(path).ok(), "bad C Result table accepted");
    require(!registry.find_traits("fixture.result.copy").ok(),
            "partial C registration escaped");
  }
  auto registry = std::make_shared<OperationRegistry>();
  auto loaded = registry->load_plugin(PS_RESULT_FIXTURE);
  require(loaded.ok(), loaded.message);
  require(registry->freeze().ok(), "freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.cpu_workers = 2;
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  ExecutionBinding a;
  a.name = "image";
  a.result = unified_example::input_image(root);
  auto writer = MutableValue::allocate({ElementType::Int64, {1}},
                                       Region::whole({1}), BufferAllocator{})
                    .take_value();
  const std::int64_t shift = 1;
  std::memcpy(writer.data(), &shift, 8);
  ExecutionBinding c;
  c.name = "control";
  c.value = std::move(writer).publish().take_value();
  const auto q = Footprint::from_regions(
                     {2, 2, 2, 4}, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}})})
                     .take_value();
  for (const auto& key : {"fixture.result.copy", "fixture.result.whole",
                          "fixture.result.tiles"}) {
    GraphContext graph(document(key));
    auto compiled = Compiler(registry).compile(graph);
    require(compiled.ok(), compiled.status().message);
    auto frozen = context.freeze(compiled.value().plan, {{a, c}}).take_value();
    auto output = context.execute_fragments(frozen, {{"out", q}});
    require(output.ok(), output.status().message);
    auto result = output.value().results.at("out");
    float pixel = 0;
    require(
        result
            .read_image(result.descriptor().value(), 0, {1, 0, 1, 2}, &pixel, 4)
            .ok(),
        "read C Result sample");
    require(pixel == (std::string(key) == "fixture.result.copy" ? 1013 : 1014),
            "C host/parallel/tile image result");
    auto dirty = output.value().dependencies.potential_dirty(
        "control", Footprint::all({1}).value());
    require(dirty.ok() && dirty.value().at("out") == q, "C Control witness");
    if (std::string(key) == "fixture.result.tiles")
      require(output.value().diagnostics.cpu_stage_count > 0 &&
                  output.value().diagnostics.cpu_tile_callback_count > 0,
              "C tile diagnostics");
    auto empty_image = context.execute_fragments(
        frozen, {{"out", Footprint::none(q.shape()).value()}});
    require(empty_image.ok(), empty_image.status().message);
    auto empty_observations =
        empty_image.value().dependencies.source_observations();
    require(empty_observations.ok(), empty_observations.status().message);
    if (std::string(key) == "fixture.result.copy")
      require(!empty_observations.value().empty(),
              "Empty image Control obligation");
    auto warm = context.execute_fragments(frozen, {{"out", q}});
    require(warm.ok(), warm.status().message);
    auto observations = warm.value().dependencies.source_observations();
    require(observations.ok(), observations.status().message);
    require(warm.value().results.at("out").object_id() == result.object_id(),
            "C completed Result reuse");
  }
  for (std::int64_t mode : {1, 2, 3, 5, 7, 8}) {
    GraphContext graph(document("fixture.result.copy", mode));
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    auto output = context.execute(plan, {{a, c}});
    require(!output.ok() && output.status().code ==
                                (mode == 3 ? ErrorCode::Cancelled
                                           : ErrorCode::InvalidArgument),
            "C sticky failure/cancel");
  }
  for (auto mode : {4, 9}) {
    GraphContext malformed(document("fixture.result.copy", mode));
    auto compiled = Compiler(registry).compile(malformed);
    require(
        !compiled.ok() && compiled.status().code == ErrorCode::InvalidArgument,
        "unaligned metadata sink");
  }
  for (auto mode : {6, 10, 11}) {
    GraphContext stale(document("fixture.result.whole", mode));
    auto compiled = Compiler(registry).compile(stale).take_value();
    auto output = context.execute(compiled.plan, {{a, c}});
    require(!output.ok() && output.status().code == ErrorCode::InvalidArgument,
            "stale start CPU service lease");
  }
  {
    SemanticDescriptor semantic;
    semantic.kind = SemanticKind::Lut;
    semantic.channels = {{"table", "value", "dimensionless"}};
    semantic.unit = "dimensionless";
    semantic.sample_step = 1;
    semantic.sample_axis_unit = "index";
    auto facet = encode_semantic(semantic).take_value();
    float table[] = {0, 1, 2, 3};
    auto value =
        Value::create({ElementType::Float32, {4}}, Region::whole({4}), {0, {4}},
                      std::vector<uint8_t>(
                          reinterpret_cast<uint8_t*>(table),
                          reinterpret_cast<uint8_t*>(table) + sizeof(table)),
                      {facet})
            .take_value();
    WorkflowDocument typed;
    WorkflowInputDeclaration lut;
    lut.id = 1;
    lut.name = "lut";
    lut.descriptor = {ElementType::Float32, {4}};
    lut.region = Region::whole({4});
    lut.layout.byte_strides = {4};
    lut.facets = {facet};
    WorkflowInputDeclaration factor;
    factor.id = 2;
    factor.name = "factor";
    factor.descriptor = {ElementType::Float32, {1}};
    factor.region = Region::whole({1});
    factor.layout.byte_strides = {4};
    typed.inputs = {lut, factor};
    typed.nodes = {{1,
                    "fixture.result.typed",
                    {WorkflowInputReference{1}, WorkflowInputReference{2}},
                    {}}};
    typed.outputs = {{"out", 1, "number"}};
    GraphContext graph(typed);
    auto plan = Compiler(registry).compile(graph);
    require(plan.ok(), plan.status().message);
    for (float number : {.5F, 2.0F, std::numeric_limits<float>::quiet_NaN()}) {
      auto writer = MutableValue::allocate(factor.descriptor, factor.region,
                                           BufferAllocator{})
                        .take_value();
      std::memcpy(writer.data(), &number, 4);
      ExecutionBinding input;
      input.name = "lut";
      input.value = value;
      ExecutionBinding scalar;
      scalar.name = "factor";
      scalar.value = std::move(writer).publish().take_value();
      auto output = context.execute(plan.value().plan, {{input, scalar}});
      if (number == .5F) {
        require(output.ok(), output.status().message);
        float result = 0;
        std::memcpy(&result, output.value().values.at("out").bytes().data(), 4);
        require(
            result == 3 && !output.value().values.at("out").facets().empty(),
            "C Typed LUT/Scalar input and Typed output");
      } else {
        require(!output.ok(), "C scalar bounds/nonfinite validation");
      }
    }
  }
  {
    auto projected = document("fixture.result.mixed", 12);
    projected.outputs = {{"out", 1, "number"}};
    GraphContext graph(projected);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    auto escaped = context.execute(plan, {{a, c}});
    require(
        !escaped.ok() && escaped.status().code == ErrorCode::InvalidArgument,
        "Unknown descriptor does not waive selected output projection");
  }
  auto mixed_doc = document("fixture.result.mixed");
  mixed_doc.nodes[0].parameters.clear();
  mixed_doc.outputs = {{"image", 1, "image"},
                       {"count", 1, "number"},
                       {"lut", 1, "lut"},
                       {"metadata", 1, "metadata"}};
  GraphContext mixed_graph(mixed_doc);
  auto mixed_plan = Compiler(registry).compile(mixed_graph);
  require(mixed_plan.ok(), mixed_plan.status().message);
  auto mixed = context.execute(mixed_plan.value().plan, {{a, c}});
  require(mixed.ok(), "C mixed: " + std::string(mixed.status().message));
  require(mixed.value().results.size() == 2 && mixed.value().values.size() == 2,
          "C same-node mixed named outputs");
  require(
      mixed.value().results.at("metadata").descriptor().value().rows(0) == 1,
      "C mixed runtime Result field");
  for (bool multiple : {false, true}) {
    auto unsupported = unified_example::image_schema();
    if (multiple) {
      auto other = unsupported.images[0];
      other.key = "depth";
      unsupported.images.push_back(other);
    } else {
      unsupported.images.clear();
      unsupported.fields = {{"rows", ElementType::UInt8, {}, {}}};
    }
    auto doc = document("fixture.result.copy");
    doc.inputs[0].result_schema = std::make_shared<SchemaTemplate>(unsupported);
    GraphContext graph(doc);
    auto refused = Compiler(registry).compile(graph);
    require(
        !refused.ok() && refused.status().code == ErrorCode::InvalidArgument,
        "C fixture rejects unsupported slot count before dereference");
  }
  // The same C key resolves another bounded N/L/H/W schema.
  auto dynamic_schema = unified_example::image_schema();
  dynamic_schema.images[0].frames = 1;
  dynamic_schema.images[0].layers = 3;
  dynamic_schema.images[0].descriptor.shape = {3, 5};
  auto builder =
      ResultBuilder::start(root, dynamic_schema, "dynamic").take_value();
  require(builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
              .ok(),
          "dynamic basis");
  float pixels[45];
  for (unsigned i = 0; i < 45; ++i)
    pixels[i] = i;
  require(
      builder
          .publish_image(
              0, Region::whole({1, 3, 3, 5}),
              ByteView(reinterpret_cast<uint8_t*>(pixels), sizeof(pixels)),
              ResultRelation::cartesian(root, 45, {0, 1, 0, 0}).take_value(),
              {true, true, true, true})
          .ok(),
      "dynamic image");
  ExecutionBinding dynamic = a;
  dynamic.result = builder.seal().take_value();
  auto dynamic_doc = document("fixture.result.copy");
  dynamic_doc.inputs[0].result_schema =
      std::make_shared<SchemaTemplate>(dynamic_schema);
  GraphContext dynamic_graph(dynamic_doc);
  auto dynamic_plan = Compiler(registry).compile(dynamic_graph);
  require(dynamic_plan.ok(), dynamic_plan.status().message);
  auto dynamic_result =
      context.execute(dynamic_plan.value().plan, {{dynamic, c}});
  require(dynamic_result.ok(), dynamic_result.status().message);
  float pixel = 0;
  auto object = dynamic_result.value().results.at("out");
  require(
      object.read_image(object.descriptor().value(), 0, {0, 2, 2, 4}, &pixel, 4)
              .ok() &&
          pixel == 40,
      "C resolved query and new image dimensions");
  // Prefix producer and complete/prefix consumers use mandatory field I/O.
  auto control = [&](int64_t count) {
    auto value = MutableValue::allocate({ElementType::Int64, {1}},
                                        Region::whole({1}), BufferAllocator{})
                     .take_value();
    std::memcpy(value.data(), &count, 8);
    ExecutionBinding binding = c;
    binding.value = std::move(value).publish().take_value();
    return binding;
  };
  WorkflowDocument rows;
  rows.inputs = {document("fixture.result.copy").inputs[1]};
  rows.nodes = {
      {1, "fixture.result.rows", {WorkflowInputReference{2}}, {}},
      {2, "fixture.result.rows_sum", {WorkflowNodeOutput{1, "rows"}}, {}},
      {3, "fixture.result.rows_first", {WorkflowNodeOutput{1, "rows"}}, {}}};
  rows.outputs = {{"rows", 1, "rows"},
                  {"sum", 2, "number"},
                  {"first", 3, "number"}};
  GraphContext rows_graph(rows);
  auto rows_plan = Compiler(registry).compile(rows_graph).take_value().plan;
  ResultRef three, none;
  for (int64_t count : {3, 0}) {
    auto result = context.execute(rows_plan, {{control(count)}});
    require(result.ok(), "C rows: " + std::string(result.status().message));
    require(result.value().results.at("rows").descriptor().value().rows(0) ==
                static_cast<uint64_t>(count),
            "C RuntimeCount zero/nonzero");
    float sum = 0;
    std::memcpy(&sum, result.value().values.at("sum").bytes().data(), 4);
    require(sum == (count ? 6 : 0), "C field consumer I/O");
    if (count)
      three = result.value().results.at("rows");
    else
      none = result.value().results.at("rows");
  }
  auto row_schema = std::make_shared<SchemaTemplate>(
      registry->find_traits("fixture.result.rows")
          .value()
          .outputs[0]
          .result_schema.value());
  WorkflowDocument bound_rows;
  WorkflowInputDeclaration source;
  source.id = 1;
  source.name = "rows";
  source.result_schema = row_schema;
  bound_rows.inputs = {source};
  bound_rows.nodes = {
      {1, "fixture.result.rows_sum", {WorkflowInputReference{1}}, {}}};
  bound_rows.outputs = {{"sum", 1, "number"}};
  GraphContext bound_graph(bound_rows);
  auto bound_plan = Compiler(registry).compile(bound_graph).take_value().plan;
  ExecutionBinding bound;
  bound.name = "rows";
  bound.result = three;
  auto demand = context.open_demand(bound_plan, {{bound}}).take_value();
  auto one = Footprint::all({1}).take_value();
  require(demand.request({{"sum", one}}).ok(), "field demand");
  bound.result = none;
  auto changed = demand.replace_bindings({{bound}});
  require(changed.ok(), changed.status().message);
  auto zero = demand.request({{"sum", one}});
  require(zero.ok(), zero.status().message);
  bound.result = three;
  changed = demand.replace_bindings({{bound}});
  require(changed.ok(), changed.status().message);
  auto again = demand.request({{"sum", one}});
  require(again.ok(), again.status().message);
  WorkflowDocument scalar;
  scalar.nodes = {{1, "fixture.result.scalar", {}, {}}};
  scalar.outputs = {{"out", 1, "number"}};
  GraphContext graph(scalar);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto frozen = context.freeze(plan).take_value();
  const auto sparse =
      Footprint::from_regions({128}, {Region({{1, 1}}), Region({{30, 1}})})
          .take_value();
  auto result = context.execute_fragments(frozen, {{"out", sparse}});
  require(result.ok(), result.status().message);
  float value = 0;
  require(
      result.value().values.at("out").read({30}, &value, 4).ok() && value == 30,
      "C sparse scalar output");
  std::vector<Region> boxes;
  for (uint64_t i = 0; i < 64; ++i)
    boxes.emplace_back(std::vector<RegionDimension>{{i * 2, 1}});
  auto many = Footprint::from_regions({128}, boxes).take_value();
  ExecutionOptions fragment_options;
  fragment_options.maximum_dependency_work = 8 * 1048576;
  auto fragments =
      context.execute_fragments(frozen, {{"out", many}}, {}, fragment_options);
  require(fragments.ok(),
          "C many fragments: " + std::string(fragments.status().message));
  require(fragments.value().values.at("out").read({126}, &value, 4).ok() &&
              value == 126,
          "C >32 balanced relation fragments");
  auto empty = context.execute_fragments(
      frozen, {{"out", Footprint::none({128}).value()}});
  require(empty.ok(), empty.status().message);
  require(empty.value().values.at("out").coverage().empty(),
          "nonzero Value extent Empty Q");
}
struct ScalarImage {
  unsigned stage = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!stage++) {
      ResultProgramNeed need;
      need.values = {{0, Footprint::all({1}).take_value(), 1}};
      return Result<ResultProgramPoll>(std::move(need));
    }
    float number = 0;
    auto read = phase.read(0, {0}, &number, 4);
    if (!read.ok())
      return Result<ResultProgramPoll>(read);
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key)
            .take_value();
    auto status = builder.bind_descriptor_relation(
        ResultRelation::cartesian(phase.resources, 1, {0, 8, 0, 0})
            .take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    status = builder.publish_image(
        0, Region::whole({1, 1, 1, 1}),
        ByteView(reinterpret_cast<const uint8_t*>(&number), 4),
        ResultRelation::cartesian(phase.resources, 1, {0, 1, 0, 1})
            .take_value(),
        {true, true, true, true});
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    return Result<ResultProgramPoll>(
        ResultPublication{builder.seal().take_value(), true});
  }
};
void native_ancestors(OperationRegistry* registry) {
  OperationDefinition numeric;
  numeric.key = "fixture.native.scalar";
  numeric.traits.input_count = 1;
  numeric.traits.input_schema.resize(1);
  numeric.traits.supports_cpu = false;
  numeric.traits.supports_gpu = true;
  numeric.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  numeric.traits.outputs[0].output_element_type = ElementType::Float32;
  numeric.traits.outputs[0].shape_rule = OperationShapeRule::Fixed;
  numeric.traits.outputs[0].fixed_output_shape = {1};
  numeric.callback = [](const OperationInvocation& call) -> Result<Value> {
    if (!call.gpu || call.gpu->backend != PS_GPU_BACKEND_METAL_V11)
      return Result<Value>(Status{ErrorCode::BackendUnavailable, {}});
    auto output = MutableValue::allocate({ElementType::Float32, {1}},
                                         Region::whole({1}), call.allocator)
                      .take_value();
    uint64_t input = 0, destination = 0;
    const auto* gpu = call.gpu;
    if (gpu->buffer(gpu->context, call.inputs[0].bytes().data(), 4, 0,
                    &input) ||
        gpu->buffer(gpu->context, output.data(), 4, 1, &destination))
      return Result<Value>(Status{ErrorCode::OperationFailed, {}});
    ps_gpu_buffer_binding_v11 bindings[] = {
        {sizeof(ps_gpu_buffer_binding_v11), 0, input, 0, 4, 0},
        {sizeof(ps_gpu_buffer_binding_v11), 1, destination, 0, 4, 1}};
    const char source[] =
        "#include <metal_stdlib>\nusing namespace metal;\nkernel void "
        "half_value(device const float* a [[buffer(0)]],device float* b "
        "[[buffer(1)]],uint i [[thread_position_in_grid]]){b[i]=a[i]*.5f;}";
    ps_gpu_dispatch_v11 command{};
    command.struct_size = sizeof(command);
    command.source = source;
    command.source_size = sizeof(source) - 1;
    command.entry = "half_value";
    command.entry_size = 10;
    command.buffers = bindings;
    command.buffer_count = 2;
    command.grid[0] = command.grid[1] = command.grid[2] = 1;
    if (gpu->execute(gpu->context, &command, 1) ||
        gpu->release(gpu->context, input) ||
        gpu->release(gpu->context, destination))
      return Result<Value>(Status{ErrorCode::OperationFailed, {}});
    return std::move(output).publish();
  };
  require(registry->register_operation(std::move(numeric)).ok(),
          "native numeric ancestor registration");
  for (bool broadcast : {false, true}) {
    OperationDefinition source;
    source.key =
        broadcast ? "fixture.broadcast.scalar" : "fixture.affine.scalar";
    source.traits.outputs[0].output_element_type = ElementType::Float32;
    source.traits.outputs[0].shape_rule = OperationShapeRule::Fixed;
    source.traits.outputs[0].fixed_output_shape = {broadcast ? 1000U : 1U};
    source.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    source.traits.workspace_bytes = 65536;
    source.callback =
        [broadcast](const OperationInvocation& call) -> Result<Value> {
      const ValueDescriptor backing{ElementType::Float32,
                                    {broadcast ? 1U : 1000U}};
      auto allocated = MutableValue::allocate(
          backing, Region::whole(backing.shape), call.allocator);
      if (!allocated.ok())
        return Result<Value>(allocated.status());
      auto writer = allocated.take_value();
      float eight = 8;
      std::memcpy(writer.data(), &eight, 4);
      auto dense = std::move(writer).publish().take_value();
      ValueDescriptor shape{ElementType::Float32, {broadcast ? 1000U : 1U}};
      return Value::from_storage(shape, Region::whole(shape.shape),
                                 {0, {broadcast ? 0 : 4}}, dense.storage());
    };
    require(registry->register_operation(std::move(source)).ok(),
            "native affine/broadcast source registration");
  }
  OperationDefinition output;
  output.key = "fixture.scalar.image";
  output.traits = unified_example::traits(1, sizeof(ScalarImage));
  auto schema = unified_example::image_schema();
  schema.images[0].frames = schema.images[0].layers = 1;
  schema.images[0].descriptor.shape = {1, 1};
  unified_example::result_output(&output.traits.outputs[0], schema);
  output.start_result = [](const ResultProgramQuery&,
                           const BufferAllocator& allocator) {
    return ResultContinuation::make<ScalarImage>(allocator);
  };
  require(registry->register_operation(std::move(output)).ok(),
          "image from scalar registration");
}
int native_gpu() {
  auto registry = std::make_shared<OperationRegistry>();
  auto loaded = registry->load_plugin(PS_RESULT_FIXTURE);
  require(loaded.ok(), loaded.message);
  native_ancestors(registry.get());
  require(registry->freeze().ok(), "native registry");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.gpu_enabled = true;
  ExecutionContext context(registry, config);
  if (!context.gpu_enabled()) {
    std::cout << "native Result GPU device unavailable\n";
    return 77;
  }
  WorkflowDocument doc;
  doc.nodes = {{1, "fixture.result.native", {}, {}}};
  doc.outputs = {{"out", 1, "image"}};
  GraphContext graph(doc);
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, options);
  require(compiled.ok(), compiled.status().message);
  auto output = context.execute(compiled.value().plan);
  require(output.ok(), output.status().message);
  const auto& result = output.value().results.at("out");
  float number = 0;
  require(result.read_image(result.descriptor().value(), 0, {0, 0, 0, 0},
                            &number, 4)
                  .ok() &&
              number == 4,
          "native Result GPU readback");
  require(output.value().diagnostics.native_dispatch_count == 1 &&
              output.value().diagnostics.native_submission_count == 1,
          "actual native Result GPU dispatch");
  for (int64_t mode : {1, 2}) {
    doc.nodes[0].parameters = {{"mode", mode}};
    GraphContext invalid(doc);
    auto plan = Compiler(registry).compile(invalid, options).take_value().plan;
    auto failed = context.execute(plan);
    require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument,
            "native service preserves sticky host InvalidArgument");
  }
  WorkflowDocument chain;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "number";
  input.descriptor = {ElementType::Float32, {1}};
  input.region = Region::whole({1});
  input.layout.byte_strides = {4};
  chain.inputs = {input};
  chain.nodes = {
      {1, "fixture.native.scalar", {WorkflowInputReference{1}}, {}},
      {2, "fixture.scalar.image", {WorkflowNodeOutput{1, "value"}}, {}}};
  chain.outputs = {{"out", 2, "value"}};
  GraphContext chain_graph(chain);
  auto chain_plan =
      Compiler(registry).compile(chain_graph, options).take_value().plan;
  auto value =
      MutableValue::allocate(input.descriptor, input.region, BufferAllocator{})
          .take_value();
  float eight = 8;
  std::memcpy(value.data(), &eight, 4);
  ExecutionBinding binding;
  binding.name = "number";
  binding.value = std::move(value).publish().take_value();
  auto transformed = context.execute(chain_plan, {{binding}});
  require(transformed.ok(), transformed.status().message);
  auto image = transformed.value().results.at("out");
  number = 0;
  require(
      image.read_image(image.descriptor().value(), 0, {0, 0, 0, 0}, &number, 4)
              .ok() &&
          number == 4,
      "native numeric ancestor to Result image");
  require(transformed.value().diagnostics.native_dispatch_count == 1 &&
              transformed.value().diagnostics.transfer_count == 1 &&
              transformed.value().diagnostics.selected_backends.at({1, 0}) ==
                  Backend::Gpu,
          "unified native ancestor lane and input transfer");
  for (bool broadcast : {false, true}) {
    WorkflowDocument view_document;
    view_document.nodes = {
        {1,
         broadcast ? "fixture.broadcast.scalar" : "fixture.affine.scalar",
         {},
         {}},
        {2, "fixture.native.scalar", {WorkflowNodeOutput{1, "value"}}, {}},
        {3, "fixture.scalar.image", {WorkflowNodeOutput{2, "value"}}, {}}};
    view_document.outputs = {{"out", 3, "value"}};
    GraphContext view_graph(view_document);
    auto view_plan =
        Compiler(registry).compile(view_graph, options).take_value().plan;
    auto copied = context.execute(view_plan);
    require(copied.ok(), copied.status().message);
    require(
        copied.value().diagnostics.transfer_bytes == (broadcast ? 4000U : 4U),
        "native transfer counts packed bytes for affine/broadcast views");
    if (broadcast) {
      auto small_config = config;
      small_config.managed_resources->maximum_work = 500;
      ExecutionContext limited(registry, small_config);
      auto refused = limited.execute(view_plan);
      require(!refused.ok() &&
                  refused.status().code == ErrorCode::ResourceExhausted,
              "broadcast native copy refuses insufficient work");
      require(limited.resource_budget().value().statistics().issued.work <= 500,
              "native work cap remains bounded");
    }
  }
  std::cout << "native Result GPU dispatch=1 readback=4; numeric ancestor "
               "dispatch=1 transfer=1\n";
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--gpu-only")
      return native_gpu();
    run();
    std::cout << "C11 unified Result fixture passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
