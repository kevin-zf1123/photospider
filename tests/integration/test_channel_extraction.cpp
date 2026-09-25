#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)

int workflow() {
  auto registry = make_default_operation_registry();
  PS_CHECK(registry->find_traits("channel.extract_index_strict").ok());
  PS_CHECK(registry->find_traits("channel.extract_named_strict").ok());
  const ValueDescriptor source{ElementType::UInt8, {2, 2, 4}};
  TensorDescription description;
  description.channel_axis = 2;
  description.channels = {{"B", "blue", "relative"},
                          {"A", "coverage", "dimensionless"},
                          {"R", "red", "relative"},
                          {"G", "green", "relative"}};
  description.model = "rgb";
  description.transfer = "linear";
  auto facet = encode_tensor_description(description);
  PS_CHECK(facet.ok());
  const std::vector<std::uint8_t> bytes{30, 25, 10, 20, 31, 50, 11, 21,
                                        32, 75, 12, 22, 33, 99, 13, 23};
  auto value = Value::create(source, Region::whole(source.shape),
                             {0, {8, 4, 1}}, bytes, {facet.value()});
  PS_CHECK(value.ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "source",
                      source,
                      Region::whole(source.shape),
                      {0, {8, 4, 1}},
                      {facet.value()}}};
  document.nodes = {{1,
                     "channel.extract_index_strict",
                     {WorkflowInputReference{1}},
                     {{"axis", std::int64_t{2}},
                      {"index", std::int64_t{1}},
                      {"keepdims", false},
                      {"layout", std::string("materialize")},
                      {"metadata_mode", std::string("respect")}}}};
  document.outputs = {{"alpha", 1, "values"}};
  PlanningOptions options;
  options.output_regions = {{"alpha", Region({{1, 1}, {0, 2}})}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto plan = compiler.compile(graph, options);
  PS_CHECK(plan.ok());
  ExecutionBindings bindings;
  bindings.inputs = {{"source", value.take_value()}};
  ExecutionContext execution(registry);
  auto run = execution.execute(plan.value().plan, bindings);
  if (!run.ok())
    std::cerr << "run failure: " << static_cast<int>(run.status().code) << " "
              << run.status().message << '\n';
  PS_CHECK(run.ok());
  const auto& result = run.value().values.at("alpha");
  PS_CHECK(result.descriptor().shape == std::vector<std::uint64_t>({2, 2}));
  PS_CHECK(result.region().dimensions()[0].offset == 1);
  PS_CHECK(result.bytes().size() == 2);
  PS_CHECK(result.bytes().data()[0] == 75);
  PS_CHECK(result.bytes().data()[1] == 99);
  auto output_description = decode_tensor_description(result.facets()[0]);
  PS_CHECK(output_description.ok());
  PS_CHECK(!output_description.value().channel_axis);
  PS_CHECK(output_description.value().component->name == "A");

  document.nodes[0].operation = "channel.extract_named_strict";
  document.nodes[0].parameters.erase("index");
  document.nodes[0].parameters["match"] = std::string("role");
  document.nodes[0].parameters["selector"] = std::string("red");
  document.nodes[0].parameters["layout"] = std::string("view");
  GraphContext named_graph(document);
  auto named_plan = compiler.compile(named_graph, options);
  PS_CHECK(named_plan.ok());
  auto named = execution.execute(named_plan.value().plan, bindings);
  PS_CHECK(named.ok());
  const auto& red = named.value().values.at("alpha");
  PS_CHECK(red.byte_address({1, 0}).ok());
  PS_CHECK(red.bytes().data()[red.byte_address({1, 0}).value()] == 12);
  PS_CHECK(red.bytes().data()[red.byte_address({1, 1}).value()] == 13);

  WorkflowDocument split_document;
  split_document.inputs = document.inputs;
  OperationMetadata source_metadata;
  source_metadata.descriptor = source;
  source_metadata.facets = {facet.value()};
  auto handles = format::split_channels(
      split_document, WorkflowInputReference{1}, source_metadata);
  PS_CHECK(handles.ok() && handles.value().size() == 4);
  PS_CHECK(handles.value()[2].name == "c2");
  PS_CHECK(split_document.nodes.size() == 4);
  split_document.outputs = {{"red", handles.value()[2].output.source_node,
                             handles.value()[2].output.source_port}};
  GraphContext split_graph(split_document);
  options.output_regions = {{"red", Region({{1, 1}, {0, 2}})}};
  auto split_plan = compiler.compile(split_graph, options);
  PS_CHECK(split_plan.ok());
  auto split_run = execution.execute(split_plan.value().plan, bindings);
  PS_CHECK(split_run.ok());
  const auto& split_red = split_run.value().values.at("red");
  PS_CHECK(split_red.bytes().data()[split_red.byte_address({1, 0}).value()] ==
           12);
  PS_CHECK(split_red.bytes().data()[split_red.byte_address({1, 1}).value()] ==
           13);
  PS_CHECK(!split_run.value().diagnostics.operation_timings.empty());
  for (const auto& timing : split_run.value().diagnostics.operation_timings)
    PS_CHECK(timing.output.node_id == handles.value()[2].output.source_node);
  return 0;
}
int resource_lifetime() {
  auto bytes = numeric_fixture::fixture();
  ResourceBudget budget;
  auto imported =
      IccProfile::import(ByteView(bytes.data(), bytes.size()), budget);
  PS_CHECK(imported.ok());
  auto profile = imported.take_value();
  const auto identity = profile.identity();
  auto owners = ResourceBindings::create({profile}, budget);
  PS_CHECK(owners.ok());
  auto resource_bindings = owners.take_value();
  TensorDescription description;
  description.channel_axis = 2;
  description.channels = {{"C", "cyan", "relative"},
                          {"M", "magenta", "relative"},
                          {"Y", "yellow", "relative"},
                          {"K", "black", "relative"}};
  description.model = "cmyk";
  description.white = std::array<double, 2>{.3127, .3290};
  description.primaries_xy =
      std::array<double, 6>{.64, .33, .30, .60, .15, .06};
  description.profile = identity;
  auto facet = encode_tensor_description(description);
  PS_CHECK(facet.ok());
  auto decoded = decode_tensor_description(facet.value());
  PS_CHECK(decoded.ok());
  PS_CHECK(decoded.value().white == description.white);
  PS_CHECK(decoded.value().primaries_xy == description.primaries_xy);
  PS_CHECK(decoded.value().profile == description.profile);
  const ValueDescriptor descriptor{ElementType::UInt8, {1, 1, 4}};
  PS_CHECK(Value::create(descriptor, Region::whole(descriptor.shape),
                         {0, {4, 4, 1}}, {1, 2, 3, 4}, {facet.value()})
               .status()
               .code == ErrorCode::InvalidArgument);
  Value surviving;
  {
    auto registry = make_default_operation_registry();
    auto source = Value::create(descriptor, Region::whole(descriptor.shape),
                                {0, {4, 4, 1}}, {1, 2, 3, 4}, {facet.value()},
                                resource_bindings);
    PS_CHECK(source.ok());
    WorkflowDocument document;
    document.inputs = {{1,
                        "source",
                        descriptor,
                        Region::whole(descriptor.shape),
                        {0, {4, 4, 1}},
                        {facet.value()}}};
    document.nodes = {{1,
                       "channel.extract_index_strict",
                       {WorkflowInputReference{1}},
                       {{"index", std::int64_t{3}},
                        {"keepdims", false},
                        {"layout", std::string("materialize")},
                        {"metadata_mode", std::string("respect")}}}};
    document.outputs = {{"black", 1, "values"}};
    GraphContext graph(document);
    Compiler compiler(registry);
    auto compiled = compiler.compile(graph, {}, resource_bindings);
    PS_CHECK(compiled.ok());
    ExecutionContext execution(registry);
    auto run = execution.execute(compiled.value().plan,
                                 {{{"source", source.take_value()}}});
    PS_CHECK(run.ok());
    surviving = run.value().values.at("black");
    document.inputs[0].facets.clear();
    document.nodes[0].parameters["metadata_mode"] = std::string("override");
    document.nodes[0].parameters["metadata_override"] =
        tensor_description_parameter(description).take_value();
    document.nodes[0].parameters["layout"] = std::string("view");
    auto plain = Value::create(descriptor, Region::whole(descriptor.shape),
                               {0, {4, 4, 1}}, {1, 2, 3, 4})
                     .take_value();
    GraphContext override_graph(document);
    auto override_plan =
        compiler.compile(override_graph, {}, resource_bindings);
    PS_CHECK(override_plan.ok());
    auto override_run =
        execution.execute(override_plan.value().plan, {{{"source", plain}}});
    PS_CHECK(override_run.ok());
    PS_CHECK(override_run.value()
                 .values.at("black")
                 .resources()
                 .icc_profile(identity)
                 .ok());
    document.inputs[0].layout = {};
    document.inputs[0].planar_layout = PlanarImageLayout{};
    auto image = PlanarImage::import_value(plain, {}).take_value();
    ExecutionBindings image_bindings;
    ExecutionBinding image_binding;
    image_binding.name = "source";
    image_binding.image = std::make_shared<const PlanarImage>(image);
    image_bindings.inputs.push_back(image_binding);
    GraphContext image_graph(document);
    auto image_plan = compiler.compile(image_graph, {}, resource_bindings);
    PS_CHECK(image_plan.ok());
    auto image_run = execution.execute(image_plan.value().plan, image_bindings);
    PS_CHECK(image_run.ok());
    PS_CHECK(image_run.value()
                 .images.at("black")
                 .resources()
                 .icc_profile(identity)
                 .ok());
    auto spatial_description = description;
    spatial_description.channel_axis = 0;
    spatial_description.channels = {{"row", "", ""}};
    document.nodes[0].parameters["metadata_override"] =
        tensor_description_parameter(spatial_description).take_value();
    document.nodes[0].parameters["index"] = std::int64_t{0};
    document.nodes[0].parameters["layout"] = std::string("auto");
    GraphContext spatial_graph(document);
    auto spatial_plan = compiler.compile(spatial_graph, {}, resource_bindings);
    PS_CHECK(spatial_plan.ok());
    auto spatial_run =
        execution.execute(spatial_plan.value().plan, image_bindings);
    PS_CHECK(spatial_run.ok());
    PS_CHECK(spatial_run.value()
                 .values.at("black")
                 .resources()
                 .icc_profile(identity)
                 .ok());
  }
  resource_bindings = ResourceBindings{};
  profile = {};
  PS_CHECK(surviving.resources().icc_profile(identity).ok());
  PS_CHECK(surviving.bytes().data()[0] == 4);
  return 0;
}
int arbitrary_axis_oracle() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContext execution(registry);
  const std::vector<ElementType> dtypes{
      ElementType::UInt8,  ElementType::UInt16, ElementType::Int8,
      ElementType::Int16,  ElementType::Int64,  ElementType::Float32,
      ElementType::Float64};
  const std::vector<std::pair<std::vector<std::uint64_t>, std::uint32_t>> cases{
      {{3, 2, 4}, 0},
      {{3, 2, 4}, 1},
      {{3, 2, 4}, 2},
      {{2, 1, 2, 1, 2, 1, 2, 2}, 6},
      {{4}, 0}};
  for (auto dtype : dtypes)
    for (const auto& fixture : cases)
      for (bool keepdims : {false, true}) {
        const auto& shape = fixture.first;
        const auto axis = fixture.second;
        if (shape.size() == 1 && !keepdims)
          continue;
        const auto width = Value::element_size(dtype);
        std::uint64_t count = 1;
        for (auto extent : shape)
          count *= extent;
        std::vector<std::uint8_t> bytes(count * width);
        for (std::size_t i = 0; i < bytes.size(); ++i)
          bytes[i] = static_cast<std::uint8_t>(17 + i * 37);
        if (dtype == ElementType::Float32) {
          const std::uint32_t payloads[]{0x80000000U, 0x7fc00011U, 0x7f800000U,
                                         0xff800000U};
          std::memcpy(bytes.data(), payloads, sizeof(payloads));
        } else if (dtype == ElementType::Float64) {
          const std::uint64_t payloads[]{
              0x8000000000000000ULL, 0x7ff8000000000011ULL,
              0x7ff0000000000000ULL, 0xfff0000000000000ULL};
          std::memcpy(bytes.data(), payloads, sizeof(payloads));
        }
        std::vector<std::int64_t> strides(shape.size());
        std::uint64_t stride = width;
        for (std::size_t i = shape.size(); i-- > 0;) {
          strides[i] = static_cast<std::int64_t>(stride);
          stride *= shape[i];
        }
        auto input = Value::create({dtype, shape}, Region::whole(shape),
                                   {0, strides}, bytes);
        PS_CHECK(input.ok());
        WorkflowDocument document;
        document.inputs = {{1,
                            "source",
                            {dtype, shape},
                            Region::whole(shape),
                            {0, strides},
                            {},
                            {}}};
        const auto selected = shape[axis] - 1;
        document.nodes = {{1,
                           "channel.extract_index_strict",
                           {WorkflowInputReference{1}},
                           {{"axis", static_cast<std::int64_t>(axis)},
                            {"index", static_cast<std::int64_t>(selected)},
                            {"keepdims", keepdims},
                            {"layout", std::string("materialize")},
                            {"metadata_mode", std::string("raw")}}}};
        document.outputs = {{"selected", 1, "values"}};
        std::vector<std::uint64_t> out_shape = shape;
        if (keepdims)
          out_shape[axis] = 1;
        else
          out_shape.erase(out_shape.begin() + axis);
        std::vector<RegionDimension> dims;
        for (auto extent : out_shape)
          dims.push_back({extent > 1 ? 1ULL : 0ULL, 1});
        const Region roi(dims);
        PlanningOptions options;
        options.output_regions = {{"selected", roi}};
        GraphContext graph(document);
        auto compiled = compiler.compile(graph, options);
        PS_CHECK(compiled.ok());
        auto run = execution.execute(compiled.value().plan,
                                     {{{"source", input.take_value()}}});
        PS_CHECK(run.ok());
        const auto& result = run.value().values.at("selected");
        PS_CHECK(result.descriptor().element_type == dtype);
        PS_CHECK(result.descriptor().shape == out_shape);
        std::vector<std::uint64_t> source_coordinate;
        std::vector<std::uint64_t> output_coordinate;
        for (auto dim : dims)
          output_coordinate.push_back(dim.offset);
        source_coordinate.resize(shape.size());
        for (std::size_t i = 0; i < shape.size(); ++i)
          source_coordinate[i] =
              i == axis ? selected
                        : output_coordinate[i < axis || keepdims ? i : i - 1];
        std::uint64_t linear = 0;
        for (std::size_t i = 0; i < shape.size(); ++i)
          linear = linear * shape[i] + source_coordinate[i];
        const auto address = result.byte_address(output_coordinate);
        PS_CHECK(address.ok());
        PS_CHECK(std::memcmp(result.bytes().data() + address.value(),
                             bytes.data() + linear * width, width) == 0);
      }
  return 0;
}
int preflight_errors() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  const ValueDescriptor descriptor{ElementType::UInt8, {2, 3}};
  TensorDescription incorrect;
  incorrect.channel_axis = 1;
  incorrect.channels = {{"A", "a", ""}, {"B", "b", ""}};
  auto facet = encode_tensor_description(incorrect);
  PS_CHECK(facet.ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "source",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {0, {3, 1}},
                      {facet.value()}}};
  document.nodes = {{1,
                     "channel.extract_index_strict",
                     {WorkflowInputReference{1}},
                     {{"axis", std::int64_t{1}},
                      {"index", std::int64_t{0}},
                      {"keepdims", false},
                      {"layout", std::string("materialize")},
                      {"metadata_mode", std::string("respect")}}}};
  document.outputs = {{"selected", 1, "values"}};
  GraphContext graph(document);
  auto invalid_table = compiler.compile(graph);
  PS_CHECK(invalid_table.status().code == ErrorCode::TypeMismatch);
  PS_CHECK(invalid_table.status().reason == FailureReason::None);
  document.nodes[0].operation = "channel.extract_named_strict";
  document.nodes[0].parameters.erase("index");
  document.nodes[0].parameters["match"] = std::string("name");
  document.nodes[0].parameters["selector"] = std::string("A");
  GraphContext named_graph(document);
  auto invalid_named = compiler.compile(named_graph);
  PS_CHECK(invalid_named.status().code == ErrorCode::TypeMismatch);

  WorkflowDocument producer;
  producer.inputs = {{1,
                      "source",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {0, {3, 1}},
                      {},
                      {}}};
  producer.nodes = {{99, "core.identity", {WorkflowInputReference{1}}, {}}};
  OperationMetadata false_metadata;
  false_metadata.descriptor = {ElementType::Float64, {9, 3}};
  format::ChannelExtractOptions options;
  options.axis = 1;
  options.metadata_mode = "raw";
  auto handles = format::split_channels(
      producer, WorkflowNodeOutput{99, "value"}, false_metadata, options);
  PS_CHECK(handles.ok());
  producer.outputs = {
      {"selected", handles.value()[0].output.source_node, "values"}};
  GraphContext mismatched_graph(producer);
  auto mismatched = compiler.compile(mismatched_graph);
  PS_CHECK(mismatched.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(mismatched.status().reason == FailureReason::InvalidDomain);
  return 0;
}
int planar_alias() {
  PlanarImageConfig config;
  config.tile_height = 2;
  config.tile_width = 2;
  const ValueDescriptor descriptor{ElementType::UInt8, {2, 3, 2}};
  auto created = PlanarImage::create(descriptor, config);
  PS_CHECK(created.ok());
  auto source = created.take_value();
  const Region selected({{0, 2}, {0, 3}, {1, 1}});
  const std::vector<std::uint8_t> samples{1, 2, 3, 4, 5, 6};
  PS_CHECK(source.publish(selected, samples.data(), samples.size()).ok());
  auto writer = source.begin_write(Region({{0, 1}, {0, 1}, {0, 1}}));
  PS_CHECK(writer.ok());
  CancellationSource cancellation;
  auto future = std::async(std::launch::async, [&] {
    return source.channel_view(1, false, Region::whole({2, 3}), {}, {},
                               cancellation.token());
  });
  cancellation.cancel();
  const auto ready = future.wait_for(std::chrono::seconds(2));
  writer = Result<PlanarImageWriteWindow>(
      Status{ErrorCode::Cancelled, "release test writer"});
  PS_CHECK(ready == std::future_status::ready);
  PS_CHECK(future.get().status().code == ErrorCode::Cancelled);
  auto alias = source.channel_view(1, false, Region::whole({2, 3}));
  PS_CHECK(alias.ok());
  auto view = alias.take_value();
  PS_CHECK(view.descriptor().shape == std::vector<std::uint64_t>({2, 3}));
  PS_CHECK(view.valid_samples() == 6);
  PS_CHECK(view.owner_token() == source.owner_token());
  PS_CHECK(view.reserved_bytes() == source.reserved_bytes());
  PS_CHECK(view.begin_write(Region::whole({2, 3})).status().code ==
           ErrorCode::InvalidArgument);
  source = {};
  std::vector<std::uint8_t> observed(6);
  PS_CHECK(
      view.read(Region::whole({2, 3}), observed.data(), observed.size()).ok());
  PS_CHECK(observed == samples);
  auto kept = view.channel_view(0, true, Region::whole({2, 3, 1}));
  PS_CHECK(kept.status().code == ErrorCode::InvalidArgument);
  return 0;
}
int planar_workflow() {
  auto registry = make_default_operation_registry();
  const ValueDescriptor descriptor{ElementType::UInt8, {130, 130, 3}};
  PlanarImageConfig config;
  auto created = PlanarImage::create(descriptor, config);
  PS_CHECK(created.ok());
  auto source = created.take_value();
  const Region input_region({{127, 3}, {127, 3}, {1, 1}});
  const std::vector<std::uint8_t> bytes{1, 2, 3, 4, 5, 6, 7, 8, 9};
  PS_CHECK(source.publish(input_region, bytes.data(), bytes.size()).ok());
  WorkflowDocument document;
  document.inputs = {
      {1,
       "image",
       descriptor,
       Region::whole(descriptor.shape),
       {},
       {},
       PlanarImageLayout{ImagePlaneOrder::Tiled, 0, 1, 2, 0, {}}}};
  document.nodes = {{1,
                     "channel.extract_index_strict",
                     {WorkflowInputReference{1}},
                     {{"axis", std::int64_t{2}},
                      {"index", std::int64_t{1}},
                      {"keepdims", false},
                      {"layout", std::string("view")},
                      {"metadata_mode", std::string("raw")}}}};
  document.outputs = {{"selected", 1, "values"}};
  PlanningOptions options;
  options.output_regions = {{"selected", Region({{127, 3}, {127, 3}})}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph, options);
  if (!compiled.ok())
    std::cerr << "planar compile: " << compiled.status().message << '\n';
  PS_CHECK(compiled.ok());
  ExecutionBindings bindings;
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(source);
  bindings.inputs.push_back(std::move(binding));
  ExecutionContext execution(registry);
  auto run = execution.execute(compiled.value().plan, bindings);
  if (!run.ok())
    std::cerr << "planar execute: " << run.status().message << '\n';
  PS_CHECK(run.ok());
  const auto& selected = run.value().images.at("selected");
  PS_CHECK(selected.owner_token() == source.owner_token());
  std::vector<std::uint8_t> observed(bytes.size());
  PS_CHECK(
      selected
          .read(Region({{127, 3}, {127, 3}}), observed.data(), observed.size())
          .ok());
  PS_CHECK(observed == bytes);
  PS_CHECK(selected.valid_samples() == 9);
  const Region extra({{0, 1}, {0, 1}, {1, 1}});
  const std::uint8_t extra_sample = 99;
  PS_CHECK(source.publish(extra, &extra_sample, 1).code == ErrorCode::Stale);
  PS_CHECK(selected.valid_samples() == 9);
  std::uint8_t denied = 0;
  PS_CHECK(selected.read(Region({{0, 1}, {0, 1}}), &denied, 1).code ==
           ErrorCode::NotFound);
  document.nodes[0].parameters["layout"] = std::string("materialize");
  GraphContext copied_graph(document);
  auto copied_plan = compiler.compile(copied_graph, options);
  PS_CHECK(copied_plan.ok());
  auto copied = execution.execute(copied_plan.value().plan, bindings);
  if (!copied.ok())
    std::cerr << "planar copy: " << copied.status().message << '\n';
  PS_CHECK(copied.ok());
  const auto& materialized = copied.value().images.at("selected");
  PS_CHECK(materialized.owner_token() != source.owner_token());
  observed.assign(bytes.size(), 0);
  PS_CHECK(
      materialized
          .read(Region({{127, 3}, {127, 3}}), observed.data(), observed.size())
          .ok());
  PS_CHECK(observed == bytes);
  document.nodes[0].parameters["keepdims"] = true;
  document.nodes[0].parameters["layout"] = std::string("view");
  options.output_regions = {{"selected", Region({{127, 3}, {127, 3}, {0, 1}})}};
  GraphContext kept_graph(document);
  auto kept_plan = compiler.compile(kept_graph, options);
  PS_CHECK(kept_plan.ok());
  auto kept_run = execution.execute(kept_plan.value().plan, bindings);
  PS_CHECK(kept_run.ok());
  const auto& kept = kept_run.value().images.at("selected");
  PS_CHECK(kept.descriptor().shape ==
           std::vector<std::uint64_t>({130, 130, 1}));
  observed.assign(bytes.size(), 0);
  PS_CHECK(kept.read(Region({{127, 3}, {127, 3}, {0, 1}}), observed.data(),
                     observed.size())
               .ok());
  PS_CHECK(observed == bytes);

  WorkflowDocument split;
  split.inputs = document.inputs;
  OperationMetadata metadata;
  metadata.descriptor = descriptor;
  metadata.planar_layout = *split.inputs[0].planar_layout;
  format::ChannelExtractOptions split_options;
  split_options.axis = 2;
  auto handles = format::split_channels(split, WorkflowInputReference{1},
                                        metadata, split_options);
  PS_CHECK(handles.ok() && handles.value().size() == 3);
  split.outputs = {
      {"selected", handles.value()[1].output.source_node, "values"}};
  options.output_regions = {{"selected", Region({{127, 3}, {127, 3}})}};
  GraphContext split_graph(split);
  auto split_plan = compiler.compile(split_graph, options);
  PS_CHECK(split_plan.ok());
  auto split_run = execution.execute(split_plan.value().plan, bindings);
  PS_CHECK(split_run.ok());
  observed.assign(bytes.size(), 0);
  PS_CHECK(
      split_run.value()
          .images.at("selected")
          .read(Region({{127, 3}, {127, 3}}), observed.data(), observed.size())
          .ok());
  PS_CHECK(observed == bytes);
  return 0;
}
int planar_variants() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContext execution(registry);
  TensorDescription description;
  description.channel_axis = 2;
  description.channels = {{"B", "blue", "relative"},
                          {"R", "red", "relative"},
                          {"G", "green", "relative"}};
  auto facet = encode_tensor_description(description);
  PS_CHECK(facet.ok());
  const ValueDescriptor described{ElementType::UInt8, {2, 2, 3}};
  auto created = PlanarImage::create(described, {}, {facet.value()});
  PS_CHECK(created.ok());
  auto source = created.take_value();
  const Region green({{0, 2}, {0, 2}, {2, 1}});
  const std::vector<std::uint8_t> green_bytes{10, 11, 12, 13};
  PS_CHECK(source.publish(green, green_bytes.data(), green_bytes.size()).ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "image",
                      described,
                      Region::whole(described.shape),
                      {},
                      {facet.value()},
                      PlanarImageLayout{}}};
  document.nodes = {{1,
                     "channel.extract_named_strict",
                     {WorkflowInputReference{1}},
                     {{"keepdims", false},
                      {"layout", std::string("view")},
                      {"match", std::string("role")},
                      {"metadata_mode", std::string("respect")},
                      {"selector", std::string("green")}}}};
  document.outputs = {{"green", 1, "values"}};
  GraphContext graph(document);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionBindings bindings;
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(source);
  bindings.inputs.push_back(std::move(binding));
  auto run = execution.execute(compiled.value().plan, bindings);
  PS_CHECK(run.ok());
  const auto& result = run.value().images.at("green");
  PS_CHECK(result.owner_token() == source.owner_token());
  std::vector<std::uint8_t> observed(4);
  PS_CHECK(result.read(Region::whole({2, 2}), observed.data(), observed.size())
               .ok());
  PS_CHECK(observed == green_bytes);
  auto projected = decode_tensor_description(result.facets()[0]);
  PS_CHECK(projected.ok() && projected.value().component->role == "green");

  PlanarImageConfig chw_config;
  chw_config.height_axis = 1;
  chw_config.width_axis = 2;
  chw_config.channel_axis = 0;
  const ValueDescriptor chw_descriptor{ElementType::UInt8, {3, 130, 130}};
  auto chw_created = PlanarImage::create(chw_descriptor, chw_config);
  PS_CHECK(chw_created.ok());
  auto chw_source = chw_created.take_value();
  const Region chw_region({{1, 1}, {127, 3}, {127, 3}});
  const std::vector<std::uint8_t> chw_bytes{1, 2, 3, 4, 5, 6, 7, 8, 9};
  PS_CHECK(
      chw_source.publish(chw_region, chw_bytes.data(), chw_bytes.size()).ok());
  WorkflowDocument chw_document;
  chw_document.inputs = {
      {1,
       "image",
       chw_descriptor,
       Region::whole(chw_descriptor.shape),
       {},
       {},
       PlanarImageLayout{ImagePlaneOrder::Tiled, 1, 2, 0, 0, {}}}};
  chw_document.nodes = {{1,
                         "channel.extract_index_strict",
                         {WorkflowInputReference{1}},
                         {{"axis", std::int64_t{0}},
                          {"index", std::int64_t{1}},
                          {"keepdims", false},
                          {"layout", std::string("materialize")},
                          {"metadata_mode", std::string("raw")}}}};
  chw_document.outputs = {{"selected", 1, "values"}};
  PlanningOptions options;
  options.output_regions = {{"selected", Region({{127, 3}, {127, 3}})}};
  GraphContext chw_graph(chw_document);
  auto chw_plan = compiler.compile(chw_graph, options);
  PS_CHECK(chw_plan.ok());
  bindings.inputs[0].image = std::make_shared<const PlanarImage>(chw_source);
  auto chw_run = execution.execute(chw_plan.value().plan, bindings);
  PS_CHECK(chw_run.ok());
  observed.assign(chw_bytes.size(), 0);
  PS_CHECK(
      chw_run.value()
          .images.at("selected")
          .read(Region({{127, 3}, {127, 3}}), observed.data(), observed.size())
          .ok());
  PS_CHECK(observed == chw_bytes);
  return 0;
}
int strided_producer() {
  auto registry = make_default_operation_registry(false);
  const ValueDescriptor descriptor{ElementType::UInt8, {2, 3}};
  auto base = Value::create(descriptor, Region::whole(descriptor.shape),
                            {0, {3, 1}}, {0, 1, 2, 3, 4, 5});
  PS_CHECK(base.ok());
  auto reversed =
      Value::from_storage(descriptor, Region::whole(descriptor.shape),
                          {2, {3, -1}}, base.value().storage());
  PS_CHECK(reversed.ok());
  auto broadcast =
      Value::from_storage(descriptor, Region::whole(descriptor.shape),
                          {0, {0, 1}}, base.value().storage());
  PS_CHECK(broadcast.ok());
  const auto register_source = [&](const std::string& name, Value value) {
    OperationDefinition definition;
    definition.key = name;
    auto& output = definition.traits.outputs[0];
    output.output_element_type = ElementType::UInt8;
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {2, 3};
    output.region_rule = OperationRegionRule::Whole;
    definition.callback = [held =
                               std::move(value)](const OperationInvocation&) {
      return Result<Value>(held);
    };
    return registry->register_operation(std::move(definition));
  };
  PS_CHECK(
      register_source("test.channel_reversed", reversed.take_value()).ok());
  PS_CHECK(
      register_source("test.channel_broadcast", broadcast.take_value()).ok());
  PS_CHECK(registry->freeze().ok());
  Compiler compiler(registry);
  ExecutionContext execution(registry);
  for (const auto& case_data :
       {std::make_pair("test.channel_reversed",
                       std::vector<std::uint8_t>{1, 4}),
        std::make_pair("test.channel_broadcast",
                       std::vector<std::uint8_t>{1, 1})}) {
    WorkflowDocument document;
    document.nodes = {{1, case_data.first, {}, {}},
                      {2,
                       "channel.extract_index_strict",
                       {WorkflowNodeOutput{1, "value"}},
                       {{"axis", std::int64_t{1}},
                        {"index", std::int64_t{1}},
                        {"keepdims", false},
                        {"layout", std::string("view")},
                        {"metadata_mode", std::string("raw")}}}};
    document.outputs = {{"result", 2, "values"}};
    GraphContext graph(document);
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    auto run = execution.execute(compiled.value().plan);
    PS_CHECK(run.ok());
    const auto& result = run.value().values.at("result");
    PS_CHECK(result.bytes().data()[result.byte_address({0}).value()] ==
             case_data.second[0]);
    PS_CHECK(result.bytes().data()[result.byte_address({1}).value()] ==
             case_data.second[1]);
  }
  return 0;
}
int disjoint_planar_roots() {
  auto registry = make_default_operation_registry();
  const ValueDescriptor descriptor{ElementType::UInt8, {1, 3, 2}};
  auto created = PlanarImage::create(descriptor, {});
  PS_CHECK(created.ok());
  auto source = created.take_value();
  const std::uint8_t left = 17, right = 29;
  PS_CHECK(source.publish(Region({{0, 1}, {0, 1}, {1, 1}}), &left, 1).ok());
  PS_CHECK(source.publish(Region({{0, 1}, {2, 1}, {1, 1}}), &right, 1).ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "image",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {},
                      {},
                      PlanarImageLayout{}}};
  document.nodes = {{1,
                     "channel.extract_index_strict",
                     {WorkflowInputReference{1}},
                     {{"axis", std::int64_t{2}},
                      {"index", std::int64_t{1}},
                      {"keepdims", false},
                      {"layout", std::string("view")},
                      {"metadata_mode", std::string("raw")}}}};
  document.outputs = {{"left", 1, "values"}, {"right", 1, "values"}};
  PlanningOptions options;
  options.output_regions = {{"left", Region({{0, 1}, {0, 1}})},
                            {"right", Region({{0, 1}, {2, 1}})}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto plan = compiler.compile(graph, options);
  PS_CHECK(plan.ok());
  ExecutionBindings bindings;
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(source);
  bindings.inputs.push_back(std::move(binding));
  ExecutionContext execution(registry);
  auto run = execution.execute(plan.value().plan, bindings);
  PS_CHECK(run.ok());
  std::uint8_t observed = 0;
  PS_CHECK(run.value()
               .images.at("left")
               .read(Region({{0, 1}, {0, 1}}), &observed, 1)
               .ok());
  PS_CHECK(observed == left);
  PS_CHECK(run.value()
               .images.at("right")
               .read(Region({{0, 1}, {2, 1}}), &observed, 1)
               .ok());
  PS_CHECK(observed == right);
  PS_CHECK(run.value().images.at("left").valid_samples() == 1);
  PS_CHECK(run.value().images.at("right").valid_samples() == 1);
  return 0;
}
int planar_spatial_axis() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContext execution(registry);
  const ValueDescriptor descriptor{ElementType::UInt8, {2, 3, 4}};
  auto source = PlanarImage::create(descriptor, {}).take_value();
  const std::vector<std::uint8_t> bytes{100, 101, 102, 103, 104, 105,
                                        106, 107, 108, 109, 110, 111};
  PS_CHECK(
      source
          .publish(Region({{1, 1}, {0, 3}, {0, 4}}), bytes.data(), bytes.size())
          .ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "image",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {},
                      {},
                      PlanarImageLayout{}}};
  document.nodes = {{1,
                     "channel.extract_index_strict",
                     {WorkflowInputReference{1}},
                     {{"axis", std::int64_t{0}},
                      {"index", std::int64_t{1}},
                      {"keepdims", false},
                      {"layout", std::string("auto")},
                      {"metadata_mode", std::string("raw")}}}};
  document.outputs = {{"selected", 1, "values"}};
  ExecutionBindings bindings;
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(source);
  bindings.inputs.push_back(binding);
  for (bool keep : {false, true}) {
    document.nodes[0].parameters["keepdims"] = keep;
    for (const auto* layout : {"auto", "materialize"}) {
      document.nodes[0].parameters["layout"] = std::string(layout);
      PlanningOptions options;
      options.output_regions = {
          {"selected",
           keep ? Region({{0, 1}, {1, 2}, {1, 2}}) : Region({{1, 2}, {1, 2}})}};
      GraphContext graph(document);
      auto compiled = compiler.compile(graph, options);
      if (!compiled.ok())
        std::cerr << compiled.status().message << '\n';
      PS_CHECK(compiled.ok());
      auto run = execution.execute(compiled.value().plan, bindings);
      if (!run.ok())
        std::cerr << run.status().message << '\n';
      PS_CHECK(run.ok());
      std::vector<std::uint8_t> observed(4);
      if (keep) {
        PS_CHECK(run.value()
                     .images.at("selected")
                     .read(options.output_regions.at("selected"),
                           observed.data(), observed.size())
                     .ok());
      } else {
        const auto& value = run.value().values.at("selected");
        observed.assign(value.bytes().begin(), value.bytes().end());
      }
      PS_CHECK(observed == std::vector<std::uint8_t>({105, 106, 109, 110}));
    }
    document.nodes[0].parameters["layout"] = std::string("view");
    GraphContext rejected(document);
    auto view_plan = compiler.compile(rejected);
    PS_CHECK(view_plan.ok());
    PS_CHECK(
        execution.execute(view_plan.value().plan, bindings).status().code ==
        ErrorCode::InvalidArgument);
  }
  document.nodes[0].parameters["keepdims"] = false;
  document.nodes[0].parameters["layout"] = std::string("auto");
  document.nodes.push_back({2,
                            "channel.extract_index_strict",
                            {WorkflowNodeOutput{1, "values"}},
                            {{"axis", std::int64_t{1}},
                             {"index", std::int64_t{1}},
                             {"keepdims", false},
                             {"layout", std::string("materialize")},
                             {"metadata_mode", std::string("raw")}}});
  document.outputs = {{"selected", 2, "values"}};
  PlanningOptions options;
  options.output_regions = {{"selected", Region({{1, 2}})}};
  GraphContext graph(document);
  auto compiled = compiler.compile(graph, options);
  PS_CHECK(compiled.ok());
  auto run = execution.execute(compiled.value().plan, bindings);
  if (!run.ok())
    std::cerr << "chained: " << run.status().message << '\n';
  PS_CHECK(run.ok());
  const auto& value = run.value().values.at("selected");
  PS_CHECK(value.bytes().size() == 2);
  PS_CHECK(value.bytes()[0] == 105 && value.bytes()[1] == 109);
  return 0;
}
int planar_view_budget() {
  auto registry = make_default_operation_registry();
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(registry, config);
  const auto budget = execution.resource_budget().take_value();
  const auto before = budget.statistics().live[ResourceKind::Payload];
  PlanarImage retained;
  {
    const ValueDescriptor descriptor{ElementType::UInt8, {1, 1, 2}};
    auto source = PlanarImage::create(descriptor, {}).take_value();
    const std::uint8_t sample = 23;
    PS_CHECK(source.publish(Region({{0, 1}, {0, 1}, {1, 1}}), &sample, 1).ok());
    WorkflowDocument document;
    document.inputs = {{1,
                        "image",
                        descriptor,
                        Region::whole(descriptor.shape),
                        {},
                        {},
                        PlanarImageLayout{}}};
    document.nodes = {{1,
                       "channel.extract_index_strict",
                       {WorkflowInputReference{1}},
                       {{"axis", std::int64_t{2}},
                        {"index", std::int64_t{1}},
                        {"keepdims", true},
                        {"layout", std::string("view")},
                        {"metadata_mode", std::string("raw")}}}};
    document.outputs = {{"selected", 1, "values"}};
    GraphContext graph(document);
    Compiler compiler(registry);
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    ExecutionBindings bindings;
    ExecutionBinding binding;
    binding.name = "image";
    binding.image = std::make_shared<const PlanarImage>(source);
    bindings.inputs.push_back(binding);
    ExecutionContextConfig limited_config;
    limited_config.maximum_live_bytes = 1;
    ExecutionContext limited(registry, limited_config);
    PS_CHECK(limited.execute(compiled.value().plan, bindings).status().code ==
             ErrorCode::ResourceExhausted);
    auto run = execution.execute(compiled.value().plan, bindings);
    PS_CHECK(run.ok());
    auto nested = run.value()
                      .images.at("selected")
                      .channel_view(0, false, Region::whole({1, 1}));
    PS_CHECK(nested.ok());
    retained = nested.take_value();
    PS_CHECK(budget.statistics().live[ResourceKind::Payload] > before);
  }
  PS_CHECK(budget.statistics().live[ResourceKind::Payload] > before);
  std::uint8_t observed = 0;
  PS_CHECK(retained.read(Region::whole({1, 1}), &observed, 1).ok());
  PS_CHECK(observed == 23);
  retained = {};
  PS_CHECK(budget.statistics().live[ResourceKind::Payload] == before);
  return 0;
}
int mixed_input_budget() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  const ValueDescriptor descriptor{ElementType::UInt8, {4096}};
  auto value = Value::create(descriptor, Region::whole(descriptor.shape),
                             {0, {1}}, std::vector<std::uint8_t>(4096))
                   .take_value();
  WorkflowDocument document;
  document.inputs = {
      {1, "tensor", descriptor, Region::whole(descriptor.shape), {0, {1}}, {}}};
  document.nodes = {{1, "core.identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"tensor", 1, "value"}};
  PlanningOptions options;
  options.output_regions = {{"tensor", Region({{0, 1}})}};
  ExecutionBindings bindings;
  bindings.inputs = {{"tensor", value}};
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Referenced] = 1024;
  ExecutionContext execution(registry, config);
  for (bool mixed : {false, true}) {
    if (mixed) {
      const ValueDescriptor image_descriptor{ElementType::UInt8, {1, 1, 1}};
      auto image = PlanarImage::create(image_descriptor, {}).take_value();
      const std::uint8_t sample = 1;
      PS_CHECK(image.publish(Region::whole(image_descriptor.shape), &sample, 1)
                   .ok());
      document.inputs.push_back({2,
                                 "image",
                                 image_descriptor,
                                 Region::whole(image_descriptor.shape),
                                 {},
                                 {},
                                 PlanarImageLayout{}});
      document.nodes.push_back({2,
                                "channel.extract_index_strict",
                                {WorkflowInputReference{2}},
                                {{"axis", std::int64_t{2}},
                                 {"index", std::int64_t{0}},
                                 {"keepdims", false},
                                 {"layout", std::string("view")},
                                 {"metadata_mode", std::string("raw")}}});
      document.outputs.push_back({"image", 2, "values"});
      ExecutionBinding binding;
      binding.name = "image";
      binding.image = std::make_shared<const PlanarImage>(image);
      bindings.inputs.push_back(binding);
    }
    GraphContext graph(document);
    auto compiled = compiler.compile(graph, options);
    PS_CHECK(compiled.ok());
    PS_CHECK(execution.execute(compiled.value().plan, bindings).status().code ==
             ErrorCode::ResourceExhausted);
  }
  return 0;
}
int physical_width_slice() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContext execution(registry);
  const ValueDescriptor descriptor{ElementType::UInt16, {5, 3, 2}};
  for (const auto order :
       {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
    PlanarImageConfig config;
    config.order = order;
    config.width_axis = 0;
    config.channel_axis = 1;
    config.height_axis = 2;
    if (order == ImagePlaneOrder::Continuous)
      config.row_pitch_bytes = 32;
    auto source = PlanarImage::create(descriptor, config).take_value();
    std::vector<std::uint16_t> samples;
    for (std::uint16_t x = 0; x < 5; ++x)
      for (std::uint16_t c = 0; c < 3; ++c)
        for (std::uint16_t y = 0; y < 2; ++y)
          samples.push_back(100 * x + 10 * c + y);
    PS_CHECK(source
                 .publish(Region::whole(descriptor.shape),
                          reinterpret_cast<const std::uint8_t*>(samples.data()),
                          samples.size() * sizeof(std::uint16_t))
                 .ok());
    WorkflowDocument document;
    document.inputs = {
        {1,
         "image",
         descriptor,
         Region::whole(descriptor.shape),
         {},
         {},
         PlanarImageLayout{order, 2, 0, 1, config.row_pitch_bytes, {}}}};
    document.nodes = {{1,
                       "channel.extract_index_strict",
                       {WorkflowInputReference{1}},
                       {{"axis", std::int64_t{0}},
                        {"index", std::int64_t{3}},
                        {"keepdims", true},
                        {"layout", std::string("materialize")},
                        {"metadata_mode", std::string("raw")}}}};
    document.outputs = {{"width", 1, "values"}};
    GraphContext graph(document);
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    ExecutionBinding binding;
    binding.name = "image";
    binding.image = std::make_shared<const PlanarImage>(source);
    ExecutionBindings bindings;
    bindings.inputs.push_back(binding);
    auto run = execution.execute(compiled.value().plan, bindings);
    PS_CHECK(run.ok());
    std::vector<std::uint16_t> observed(6);
    PS_CHECK(run.value()
                 .images.at("width")
                 .read(Region::whole({1, 3, 2}),
                       reinterpret_cast<std::uint8_t*>(observed.data()),
                       observed.size() * sizeof(std::uint16_t))
                 .ok());
    PS_CHECK(observed ==
             std::vector<std::uint16_t>({300, 301, 310, 311, 320, 321}));
  }
  return 0;
}
}  // namespace

int main() {
  if (workflow())
    return 1;
  if (resource_lifetime())
    return 1;
  if (arbitrary_axis_oracle())
    return 1;
  if (preflight_errors())
    return 1;
  if (planar_alias())
    return 1;
  if (planar_workflow())
    return 1;
  if (planar_variants())
    return 1;
  if (strided_producer())
    return 1;
  if (disjoint_planar_roots() || planar_spatial_axis())
    return 1;
  if (planar_view_budget())
    return 1;
  if (mixed_input_budget())
    return 1;
  return physical_width_slice();
}
