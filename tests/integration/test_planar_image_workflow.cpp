#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
ps::Status copy_region(const ps::PlanarOperationInvocation& call) {
  const auto& region = call.output_region;
  const auto y = region.dimensions()[0];
  const auto x = region.dimensions()[1];
  const auto c = region.dimensions()[2];
  for (std::uint64_t channel = c.offset; channel < c.offset + c.extent;
       ++channel)
    for (std::uint64_t row = y.offset; row < y.offset + y.extent; ++row)
      for (std::uint64_t column = x.offset; column < x.offset + x.extent;) {
        const std::vector<std::uint64_t> at{row, column, channel};
        auto source = call.inputs[0].row_run(at);
        if (!source.ok())
          return source.status();
        auto target = call.output.row_run(at);
        if (!target.ok())
          return target.status();
        const auto samples =
            std::min(source.value().samples, target.value().samples);
        std::memcpy(target.value().data, source.value().data, samples * 4);
        column += samples;
      }
  return ps::Status::success();
}
int rectangle_bounds() {
  using namespace ps;  // NOLINT(build/namespaces)
  // Permuted axes, partial final tiles and padded rows.
  const ValueDescriptor descriptor{ElementType::UInt16, {11, 2, 7}};
  const Region roi({{2, 8}, {1, 1}, {1, 6}});
  std::vector<std::uint16_t> values(48);
  for (std::size_t i = 0; i < values.size(); ++i)
    values[i] = static_cast<std::uint16_t>(i * 17 + 3);
  for (auto order : {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
    PlanarImageConfig config;
    config.order = order;
    config.width_axis = 0;
    config.channel_axis = 1;
    config.height_axis = 2;
    config.tile_width = 4;
    config.tile_height = 4;
    config.row_pitch_bytes = order == ImagePlaneOrder::Continuous ? 32 : 0;
    auto invalid_config = config;
    invalid_config.tile_width = 5;
    PS_CHECK(PlanarImage::create(descriptor, invalid_config).status().code ==
             ErrorCode::InvalidArgument);
    invalid_config = config;
    invalid_config.tile_height = 3;
    PS_CHECK(PlanarImage::create(descriptor, invalid_config).status().code ==
             ErrorCode::InvalidArgument);
    auto image = PlanarImage::create(descriptor, config).take_value();
    {
      auto writer = image.begin_write(roi).take_value();
      auto block = writer.rectangle_run({2, 1, 1}).take_value();
      PS_CHECK(block.row.samples == (order == ImagePlaneOrder::Tiled ? 2 : 8));
      PS_CHECK(block.rows == (order == ImagePlaneOrder::Tiled ? 3 : 6));
      PS_CHECK(block.row_stride_bytes ==
               (order == ImagePlaneOrder::Tiled ? 8 : 32));
      PS_CHECK(!writer.rectangle_run({1, 1, 1}).ok());
      // Destruction rolls the unpublished preparation back.
    }
    PS_CHECK(image.valid_samples() == 0);
    PS_CHECK(image
                 .publish(roi,
                          reinterpret_cast<const std::uint8_t*>(values.data()),
                          values.size() * sizeof(std::uint16_t))
                 .ok());
    auto window = image.acquire(roi).take_value();
    for (std::uint64_t x = 2; x < 10; ++x)
      for (std::uint64_t y = 1; y < 7; ++y) {
        auto block = window.rectangle_run({x, 1, y}).take_value();
        PS_CHECK(block.row.samples ==
                 (order == ImagePlaneOrder::Tiled
                      ? std::min<std::uint64_t>(10 - x, 4 - x % 4)
                      : 10 - x));
        PS_CHECK(block.rows == (order == ImagePlaneOrder::Tiled
                                    ? std::min<std::uint64_t>(7 - y, 4 - y % 4)
                                    : 7 - y));
        for (std::uint64_t dy = 0; dy < block.rows; ++dy)
          for (std::uint64_t dx = 0; dx < block.row.samples; ++dx) {
            std::uint16_t actual = 0;
            std::memcpy(&actual,
                        block.row.data + dy * block.row_stride_bytes + dx * 2,
                        2);
            PS_CHECK(actual == values[(x + dx - 2) * 6 + y + dy - 1]);
          }
      }
    PS_CHECK(!window.rectangle_run({10, 1, 1}).ok());
    PS_CHECK(!window.rectangle_run({2, 0, 1}).ok());
    PS_CHECK(!window.rectangle_run({2, 1}).ok());
    auto alias =
        image.channel_view(1, false, Region({{2, 8}, {1, 6}})).take_value();
    auto alias_window = alias.acquire(Region({{2, 8}, {1, 6}})).take_value();
    auto block = alias_window.rectangle_run({2, 1}).take_value();
    PS_CHECK(block.row_stride_bytes ==
             (order == ImagePlaneOrder::Tiled ? 8 : 32));
    std::uint16_t second_row = 0;
    std::memcpy(&second_row, block.row.data + block.row_stride_bytes, 2);
    PS_CHECK(second_row == values[1]);
  }
  Compiler compiler(make_default_operation_registry());
  PlanningOptions invalid_options;
  invalid_options.tile_width = 3;
  PS_CHECK(compiler.plan(OptimizedGraphIR{}, invalid_options).status().code ==
           ErrorCode::InvalidArgument);
  invalid_options.tile_width = 128;
  invalid_options.tile_height = 6;
  PS_CHECK(compiler.plan(OptimizedGraphIR{}, invalid_options).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(!PlanarImageReadWindow{}.rectangle_run({0, 0}).ok());
  PS_CHECK(!PlanarImageWriteWindow{}.rectangle_run({0, 0}).ok());
  return 0;
}
int workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  constexpr std::uint64_t height = 130, width = 200, channels = 4;
  const ValueDescriptor descriptor{ElementType::Float32,
                                   {height, width, channels}};
  PlanarImageConfig config;
  config.groups = {{"color", 0, 3}, {"alpha", 3, 1}};
  auto source = PlanarImage::create(descriptor, config).take_value();
  const Region roi({{127, 3}, {127, 3}, {0, channels}});
  std::vector<float> samples(3 * 3 * channels);
  for (std::size_t index = 0; index < samples.size(); ++index)
    samples[index] = static_cast<float>(index + 1);
  PS_CHECK(source
               .publish(roi,
                        reinterpret_cast<const std::uint8_t*>(samples.data()),
                        samples.size() * sizeof(float))
               .ok());
  const auto page = source.page_size();
  const auto align = [page](std::uint64_t size) {
    return (size + page - 1) / page * page;
  };
  const auto independent_offset = [&](std::uint64_t y, std::uint64_t x,
                                      std::uint64_t channel) {
    std::uint64_t plane_span = 0, target = 0;
    for (std::uint64_t ty = 0; ty < 2; ++ty)
      for (std::uint64_t tx = 0; tx < 2; ++tx) {
        if (ty == y / 128 && tx == x / 128)
          target = plane_span;
        const auto valid_h = std::min<std::uint64_t>(128, height - ty * 128);
        plane_span = align(plane_span + valid_h * 128 * sizeof(float));
      }
    return channel * plane_span + target + y % 128 * 128 * sizeof(float) +
           x % 128 * sizeof(float);
  };
  for (std::uint64_t channel = 0; channel < channels; ++channel)
    for (std::uint64_t y = 127; y < 130; ++y)
      for (std::uint64_t x = 127; x < 130; ++x)
        PS_CHECK(source.byte_offset({y, x, channel}).value() ==
                 independent_offset(y, x, channel));
  PS_CHECK(source.reserved_bytes() >=
           independent_offset(129, 199, channels - 1) + sizeof(float));
  PS_CHECK(source.valid_samples() == samples.size());
  if (page == 16384) {
    PS_CHECK(source.reserved_bytes() == 655360);
    PS_CHECK(source.byte_offset({129, 199, 0}).value() == 148252);
  }
  auto window = source.acquire(roi).take_value();
  PS_CHECK(window.row_run({127, 127, 0}).value().samples == 1);
  const Region extra({{0, 1}, {0, 1}, {0, 1}});
  float extra_value = 73;
  PS_CHECK(source
               .publish(extra,
                        reinterpret_cast<const std::uint8_t*>(&extra_value),
                        sizeof(float))
               .ok());
  PS_CHECK(window.row_run({127, 127, 0}).ok());

  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "test.planar_copy";
  definition.traits.planar_storage_capable = true;
  definition.traits.input_count = 1;
  definition.traits.input_schema.resize(1);
  definition.traits.outputs[0].output_element_type = ElementType::Float32;
  definition.traits.outputs[0].shape_rule =
      OperationShapeRule::PreserveFirstInput;
  definition.traits.outputs[0].region_rule = OperationRegionRule::Elementwise;
  definition.traits.outputs[0].output_semantic_rule =
      OperationSemanticRule::PreserveInput;
  definition.traits.outputs[0].planar_layout = PlanarImageLayout{
      ImagePlaneOrder::Tiled, 0, 1, 2, 0, {{"color", 0, 3}, {"alpha", 3, 1}}};
  definition.planar_callback = copy_region;
  PS_CHECK(registry->register_operation(std::move(definition)).ok());
  PS_CHECK(registry->freeze().ok());

  WorkflowDocument document;
  document.inputs = {{1,
                      "image",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {},
                      {},
                      PlanarImageLayout{ImagePlaneOrder::Tiled,
                                        0,
                                        1,
                                        2,
                                        0,
                                        {{"color", 0, 3}, {"alpha", 3, 1}}}}};
  document.nodes = {
      {1, "test.planar_copy", {WorkflowInputReference{1}}, {}},
      {2, "test.planar_copy", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"result", 2, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  PlanningOptions options;
  options.tile_height = 128;
  options.tile_width = 128;
  options.output_regions = {{"result", roi}};
  auto compiled = compiler.compile(graph, options);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry, {1, false, 8, 2 * 1024 * 1024});
  ExecutionBindings bindings;
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(source);
  bindings.inputs.push_back(std::move(binding));
  auto run = execution.execute(compiled.value().plan, bindings);
  PS_CHECK(run.ok());
  PS_CHECK(run.value().values.empty());
  PS_CHECK(run.value().diagnostics.operation_timings.size() == 2);
  PS_CHECK(run.value().diagnostics.selected_backends.size() == 2);
  PS_CHECK(run.value().diagnostics.source_read_count == 1);
  PS_CHECK(run.value().diagnostics.source_read_bytes == samples.size() * 4);
  PS_CHECK(run.value().diagnostics.result_copy_bytes == 0);
  PS_CHECK(run.value().diagnostics.tile_count == 4);
  PS_CHECK(run.value().diagnostics.peak_live_bytes > 0);
  PS_CHECK(!run.value().diagnostics.plan_digest.empty());
  auto result = run.value().images.at("result");
  PS_CHECK(result.config().order == ImagePlaneOrder::Tiled);
  PS_CHECK(result.valid_samples() == samples.size());
  std::vector<float> output(samples.size());
  PS_CHECK(result
               .read(roi, reinterpret_cast<std::uint8_t*>(output.data()),
                     output.size() * sizeof(float))
               .ok());
  PS_CHECK(output == samples);
  PS_CHECK(result
               .read(extra, reinterpret_cast<std::uint8_t*>(output.data()),
                     sizeof(float))
               .code == ErrorCode::NotFound);

  PlanningOptions component_options = options;
  const Region green({{127, 3}, {127, 3}, {1, 1}});
  component_options.output_regions = {{"result", green}};
  auto component_plan = compiler.compile(graph, component_options);
  PS_CHECK(component_plan.ok());
  auto component_run = execution.execute(component_plan.value().plan, bindings);
  PS_CHECK(component_run.ok());
  const auto& component = component_run.value().images.at("result");
  PS_CHECK(component.valid_samples() == 9);
  std::vector<float> green_samples(9);
  PS_CHECK(component
               .read(green,
                     reinterpret_cast<std::uint8_t*>(green_samples.data()),
                     green_samples.size() * sizeof(float))
               .ok());
  for (std::size_t i = 0; i < green_samples.size(); ++i)
    PS_CHECK(green_samples[i] == samples[i * channels + 1]);
  const Region alpha({{127, 3}, {127, 3}, {3, 1}});
  PS_CHECK(component.acquire(alpha).status().code == ErrorCode::NotFound);

  PlanarImageConfig wrong_config = config;
  wrong_config.tile_height = 64;
  wrong_config.tile_width = 64;
  auto wrong_geometry = PlanarImage::create(descriptor, wrong_config);
  PS_CHECK(wrong_geometry.ok());
  bindings.inputs[0].image =
      std::make_shared<const PlanarImage>(wrong_geometry.take_value());
  PS_CHECK(execution.execute(compiled.value().plan, bindings).status().code ==
           ErrorCode::TypeMismatch);

  // Continuous planar rows may have explicit row-end padding.
  PlanarImageConfig continuous;
  continuous.order = ImagePlaneOrder::Continuous;
  continuous.row_pitch_bytes = 16;
  continuous.tile_height = 64;
  continuous.tile_width = 64;
  auto flat = PlanarImage::create({ElementType::UInt8, {2, 3, 2}}, continuous)
                  .take_value();
  const Region flat_region = Region::whole({2, 3, 2});
  const std::vector<std::uint8_t> flat_bytes{1, 2, 3, 4,  5,  6,
                                             7, 8, 9, 10, 11, 12};
  PS_CHECK(
      flat.publish(flat_region, flat_bytes.data(), flat_bytes.size()).ok());
  PS_CHECK(flat.byte_offset({1, 2, 0}).value() == 18);
  PS_CHECK(flat.byte_offset({1, 2, 1}).value() == align(2 * 16) + 18);
  std::vector<std::uint8_t> flat_read(flat_bytes.size());
  PS_CHECK(flat.read(flat_region, flat_read.data(), flat_read.size()).ok());
  PS_CHECK(flat_read == flat_bytes);

  PlanarImageConfig chw_config;
  chw_config.height_axis = 1;
  chw_config.width_axis = 2;
  chw_config.channel_axis = 0;
  chw_config.tile_height = 2;
  chw_config.tile_width = 2;
  chw_config.groups = {{"color", 0, 2}};
  auto chw = PlanarImage::create({ElementType::UInt8, {2, 2, 3}}, chw_config)
                 .take_value();
  const auto chw_region = Region::whole({2, 2, 3});
  const std::vector<std::uint8_t> chw_bytes{0, 1, 2, 3, 4,  5,
                                            6, 7, 8, 9, 10, 11};
  PS_CHECK(chw.publish(chw_region, chw_bytes.data(), chw_bytes.size()).ok());
  PS_CHECK(chw.byte_offset({1, 1, 2}).value() == 3 * page + 2);
  std::vector<std::uint8_t> chw_read(chw_bytes.size());
  PS_CHECK(chw.read(chw_region, chw_read.data(), chw_read.size()).ok());
  PS_CHECK(chw_read == chw_bytes);

  PlanarImageConfig gray_config;
  gray_config.order = ImagePlaneOrder::Continuous;
  gray_config.channel_axis = std::nullopt;
  gray_config.row_pitch_bytes = 4;
  auto gray = PlanarImage::create({ElementType::UInt8, {2, 3}}, gray_config)
                  .take_value();
  const auto gray_region = Region::whole({2, 3});
  const std::vector<std::uint8_t> gray_bytes{1, 2, 3, 4, 5, 6};
  PS_CHECK(
      gray.publish(gray_region, gray_bytes.data(), gray_bytes.size()).ok());
  PS_CHECK(gray.byte_offset({1, 2}).value() == 6);
  std::vector<std::uint8_t> gray_read(gray_bytes.size());
  PS_CHECK(gray.read(gray_region, gray_read.data(), gray_read.size()).ok());
  PS_CHECK(gray_read == gray_bytes);

  PlanarImageConfig tight_tiles;
  tight_tiles.tile_height = 64;
  tight_tiles.tile_width = 64;
  tight_tiles.channel_axis = 2;
  auto small =
      PlanarImage::create({ElementType::UInt8, {64, 128, 1}}, tight_tiles)
          .take_value();
  PS_CHECK(small.byte_offset({0, 64, 0}).value() == align(4096));

  const Region one({{0, 1}, {0, 1}, {0, 1}});
  std::uint8_t value = 17;
  PlanarImageConfig denied_config;
  denied_config.maximum_backed_bytes = page - 1;
  auto denied =
      PlanarImage::create({ElementType::UInt8, {2, 2, 1}}, denied_config)
          .take_value();
  PS_CHECK(denied.publish(one, &value, 1).code == ErrorCode::ResourceExhausted);
  PS_CHECK(denied.backed_bytes() == 0 && denied.valid_samples() == 0);
  auto shared_budget = std::make_shared<PlanarPageBudget>(page + 4096);
  PlanarImageConfig shared_config;
  shared_config.aggregate_budget = shared_budget;
  PlanarImage retained;
  {
    auto first =
        PlanarImage::create({ElementType::UInt8, {2, 2, 1}}, shared_config)
            .take_value();
    PS_CHECK(first.publish(one, &value, 1).ok());
    retained = first;
    auto second =
        PlanarImage::create({ElementType::UInt8, {2, 2, 1}}, shared_config)
            .take_value();
    PS_CHECK(second.publish(one, &value, 1).code ==
             ErrorCode::ResourceExhausted);
    PS_CHECK(second.backed_bytes() == 0 && second.valid_samples() == 0);
  }
  PS_CHECK(shared_budget->live_bytes() > page);
  retained = {};
  PS_CHECK(shared_budget->live_bytes() == 0);

  auto aborted =
      PlanarImage::create({ElementType::UInt8, {2, 2, 1}}, shared_config)
          .take_value();
  {
    auto pending = aborted.begin_write(one).take_value();
    auto run = pending.row_run({0, 0, 0}).take_value();
    *run.data = 42;
    CancellationSource read_stop;
    auto blocked = std::async(std::launch::async, [&] {
      return aborted.acquire(one, read_stop.token()).status().code;
    });
    read_stop.cancel();
    PS_CHECK(blocked.wait_for(std::chrono::seconds(1)) ==
             std::future_status::ready);
    PS_CHECK(blocked.get() == ErrorCode::Cancelled);
  }
  PS_CHECK(aborted.valid_samples() == 0 && aborted.backed_bytes() == 0);
  PS_CHECK(shared_budget->live_bytes() == aborted.metadata_bytes());

  PlanarImageConfig row_limited;
  row_limited.maximum_metadata_rows = 1;
  row_limited.maximum_metadata_intervals = 2;
  auto same_row =
      PlanarImage::create({ElementType::UInt8, {1, 4, 1}}, row_limited)
          .take_value();
  PS_CHECK(same_row.publish(one, &value, 1).ok());
  const Region third({{0, 1}, {2, 1}, {0, 1}});
  PS_CHECK(same_row.publish(third, &value, 1).ok());
  const Region fourth({{0, 1}, {3, 1}, {0, 1}});
  PS_CHECK(same_row.publish(fourth, &value, 1).ok());
  const Region gap({{0, 1}, {1, 1}, {0, 1}});
  PS_CHECK(same_row.publish(gap, &value, 1).ok());
  auto interval_limited =
      PlanarImage::create({ElementType::UInt8, {1, 6, 1}}, row_limited)
          .take_value();
  PS_CHECK(interval_limited.publish(one, &value, 1).ok());
  PS_CHECK(interval_limited.publish(third, &value, 1).ok());
  const Region fifth({{0, 1}, {4, 1}, {0, 1}});
  PS_CHECK(interval_limited.publish(fifth, &value, 1).code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(interval_limited.valid_samples() == 2);

  PlanarImageConfig oversized_groups;
  oversized_groups.groups = {{std::string(1024, 'x'), 0, 1}};
  PS_CHECK(
      PlanarImage::create({ElementType::UInt8, {1, 1, 1}}, oversized_groups)
          .status()
          .code == ErrorCode::InvalidArgument);

  // A result rebound to the same context already owns a root-budget lease.
  OperationDefinition small_definition;
  small_definition.key = "test.small_planar_copy";
  small_definition.traits.planar_storage_capable = true;
  small_definition.traits.input_count = 1;
  small_definition.traits.input_schema.resize(1);
  small_definition.traits.outputs[0].output_element_type = ElementType::Float32;
  small_definition.traits.outputs[0].shape_rule =
      OperationShapeRule::PreserveFirstInput;
  small_definition.traits.outputs[0].region_rule =
      OperationRegionRule::Elementwise;
  small_definition.traits.outputs[0].planar_layout =
      PlanarImageLayout{ImagePlaneOrder::Tiled, 0, 1, 2, 0, {}};
  small_definition.planar_callback = copy_region;
  auto small_registry = std::make_shared<OperationRegistry>();
  PS_CHECK(
      small_registry->register_operation(std::move(small_definition)).ok());
  PS_CHECK(small_registry->freeze().ok());
  const ValueDescriptor small_descriptor{ElementType::Float32, {1, 1, 1}};
  PlanarImageConfig small_config;
  auto small_source =
      PlanarImage::create(small_descriptor, small_config).take_value();
  const auto small_region = Region::whole(small_descriptor.shape);
  const float small_sample = 19;
  PS_CHECK(small_source
               .publish(small_region,
                        reinterpret_cast<const std::uint8_t*>(&small_sample),
                        sizeof(small_sample))
               .ok());
  WorkflowDocument small_document;
  small_document.inputs = {
      {1,
       "image",
       small_descriptor,
       small_region,
       {},
       {},
       PlanarImageLayout{ImagePlaneOrder::Tiled, 0, 1, 2, 0, {}}}};
  small_document.nodes = {
      {1, "test.small_planar_copy", {WorkflowInputReference{1}}, {}}};
  small_document.outputs = {{"result", 1, "value"}};
  GraphContext small_graph(small_document);
  auto small_plan = Compiler(small_registry).compile(small_graph).take_value();
  ExecutionContext small_context(small_registry,
                                 {1, false, 8, 2 * page + 8192});
  ExecutionBindings small_bindings;
  small_bindings.inputs.push_back(
      {"image", {}, {}, {}, std::make_shared<const PlanarImage>(small_source)});
  auto first_run = small_context.execute(small_plan.plan, small_bindings);
  PS_CHECK(first_run.ok());
  small_bindings.inputs[0].image = std::make_shared<const PlanarImage>(
      first_run.value().images.at("result"));
  auto second_run = small_context.execute(small_plan.plan, small_bindings);
  PS_CHECK(second_run.ok());
  float observed = 0;
  PS_CHECK(second_run.value()
               .images.at("result")
               .read(small_region, reinterpret_cast<std::uint8_t*>(&observed),
                     sizeof(observed))
               .ok());
  PS_CHECK(observed == small_sample);
  auto packed_legacy =
      Value::create(small_descriptor, small_region, {0, {4, 4, 4}},
                    std::vector<std::uint8_t>(sizeof(float)))
          .take_value();
  auto direct_legacy_binding = small_bindings;
  direct_legacy_binding.inputs[0].image.reset();
  direct_legacy_binding.inputs[0].value = packed_legacy;
  PS_CHECK(small_context.execute(small_plan.plan, direct_legacy_binding)
               .status()
               .code == ErrorCode::TypeMismatch);

  auto concurrent_registry = std::make_shared<OperationRegistry>();
  OperationDefinition concurrent_definition;
  concurrent_definition.key = "test.concurrent_planar_copy";
  concurrent_definition.traits =
      small_registry->find_traits("test.small_planar_copy").take_value();
  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  unsigned entered = 0;
  bool release = false;
  concurrent_definition.planar_callback =
      [&](const PlanarOperationInvocation& call) {
        {
          std::unique_lock<std::mutex> lock(gate_mutex);
          ++entered;
          gate_changed.notify_all();
          gate_changed.wait(lock, [&] { return release; });
        }
        return copy_region(call);
      };
  PS_CHECK(
      concurrent_registry->register_operation(std::move(concurrent_definition))
          .ok());
  PS_CHECK(concurrent_registry->freeze().ok());
  small_document.nodes[0].operation = "test.concurrent_planar_copy";
  GraphContext concurrent_graph(small_document);
  auto concurrent_plan =
      Compiler(concurrent_registry).compile(concurrent_graph).take_value();
  ExecutionContext concurrent_context(concurrent_registry,
                                      {2, false, 8, 3 * page + 8192});
  small_bindings.inputs[0].image =
      std::make_shared<const PlanarImage>(small_source);
  auto first_future = std::async(std::launch::async, [&] {
    return concurrent_context.execute(concurrent_plan.plan, small_bindings);
  });
  auto second_future = std::async(std::launch::async, [&] {
    return concurrent_context.execute(concurrent_plan.plan, small_bindings);
  });
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate_changed.wait_for(lock, std::chrono::seconds(2),
                          [&] { return entered == 2; });
    release = true;
  }
  gate_changed.notify_all();
  auto concurrent_first = first_future.get();
  auto concurrent_second = second_future.get();
  PS_CHECK(entered == 2 && concurrent_first.ok() && concurrent_second.ok());

  const ValueDescriptor growing_descriptor{ElementType::Float32, {1, 2, 1}};
  auto growing_source =
      PlanarImage::create(growing_descriptor, small_config).take_value();
  const Region first_pixel({{0, 1}, {0, 1}, {0, 1}});
  const Region second_pixel({{0, 1}, {1, 1}, {0, 1}});
  PS_CHECK(growing_source
               .publish(first_pixel,
                        reinterpret_cast<const std::uint8_t*>(&small_sample),
                        sizeof(small_sample))
               .ok());
  small_document.inputs[0].descriptor = growing_descriptor;
  small_document.inputs[0].region = Region::whole(growing_descriptor.shape);
  GraphContext growing_graph(small_document);
  PlanningOptions growing_options;
  growing_options.output_regions = {{"result", first_pixel}};
  auto growing_plan = Compiler(concurrent_registry)
                          .compile(growing_graph, growing_options)
                          .take_value();
  ExecutionContext growing_context(concurrent_registry,
                                   {1, false, 8, 3 * page + 8192});
  ExecutionBindings growing_bindings;
  growing_bindings.inputs.push_back(
      {"image",
       {},
       {},
       {},
       std::make_shared<const PlanarImage>(growing_source)});
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    entered = 0;
    release = false;
  }
  auto growing_future = std::async(std::launch::async, [&] {
    return growing_context.execute(growing_plan.plan, growing_bindings);
  });
  bool growing_entered = false;
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    growing_entered = gate_changed.wait_for(lock, std::chrono::seconds(2),
                                            [&] { return entered == 1; });
  }
  const auto blocked_publication = growing_source.publish(
      second_pixel, reinterpret_cast<const std::uint8_t*>(&small_sample),
      sizeof(small_sample));
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  auto grown = growing_future.get();
  PS_CHECK(growing_entered && blocked_publication.code == ErrorCode::Stale &&
           grown.ok());
  PS_CHECK(growing_source
               .publish(second_pixel,
                        reinterpret_cast<const std::uint8_t*>(&small_sample),
                        sizeof(small_sample))
               .ok());

  ExecutionContext cycling_context(small_registry,
                                   {1, false, 8, 2 * page + 8192});
  for (unsigned cycle = 0; cycle < 64; ++cycle) {
    auto fresh_source =
        PlanarImage::create(small_descriptor, small_config).take_value();
    PS_CHECK(fresh_source
                 .publish(small_region,
                          reinterpret_cast<const std::uint8_t*>(&small_sample),
                          sizeof(small_sample))
                 .ok());
    ExecutionBindings fresh_binding;
    fresh_binding.inputs.push_back(
        {"image",
         {},
         {},
         {},
         std::make_shared<const PlanarImage>(fresh_source)});
    PS_CHECK(cycling_context.execute(small_plan.plan, fresh_binding).ok());
  }

  auto legacy_layer = make_layer_operation(LayerOperation::Opacity);
  PS_CHECK(legacy_layer.ok());
  OperationRegistry layer_registry;
  PS_CHECK(layer_registry.register_operation(legacy_layer.take_value()).code ==
           ErrorCode::InvalidArgument);
  auto layer_layout = layer_schema(LayerRepresentation::Layer).take_value();
  PS_CHECK(layer_layout.validate().code == ErrorCode::TypeMismatch);
  PS_CHECK(ResultBuilder::start(ResourceBudget{}, layer_layout, "legacy")
               .status()
               .code == ErrorCode::TypeMismatch);

  auto failure_registry = std::make_shared<OperationRegistry>();
  const auto failure_traits =
      small_registry->find_traits("test.small_planar_copy").take_value();
  CancellationSource returned_cancel;
  OperationDefinition returned_failure;
  returned_failure.key = "test.planar_cancel_return";
  returned_failure.traits = failure_traits;
  returned_failure.planar_callback = [&](const PlanarOperationInvocation&) {
    returned_cancel.cancel();
    return Status::failure(ErrorCode::OperationFailed, "fixture failure");
  };
  PS_CHECK(
      failure_registry->register_operation(std::move(returned_failure)).ok());
  CancellationSource thrown_cancel;
  OperationDefinition thrown_failure;
  thrown_failure.key = "test.planar_cancel_throw";
  thrown_failure.traits = failure_traits;
  thrown_failure.planar_callback = [&](const PlanarOperationInvocation&) {
    thrown_cancel.cancel();
    throw std::runtime_error("fixture throw");
    return Status::success();
  };
  PS_CHECK(
      failure_registry->register_operation(std::move(thrown_failure)).ok());
  OperationDefinition stale_failure;
  stale_failure.key = "test.planar_stale_failure";
  stale_failure.traits = failure_traits;
  std::shared_ptr<GraphContext> changed_graph;
  stale_failure.planar_callback = [&](const PlanarOperationInvocation&) {
    changed_graph->replace(small_document);
    return Status::failure(ErrorCode::OperationFailed, "fixture stale");
  };
  PS_CHECK(failure_registry->register_operation(std::move(stale_failure)).ok());
  PS_CHECK(failure_registry->freeze().ok());
  auto failure_document = small_document;
  failure_document.inputs[0].descriptor = small_descriptor;
  failure_document.inputs[0].region = small_region;
  failure_document.nodes[0].operation = "test.planar_cancel_return";
  GraphContext returned_graph(failure_document);
  auto returned_plan =
      Compiler(failure_registry).compile(returned_graph).take_value();
  ExecutionContext failure_context(failure_registry);
  small_bindings.inputs[0].image =
      std::make_shared<const PlanarImage>(small_source);
  PS_CHECK(
      failure_context
          .execute(returned_plan.plan, small_bindings, returned_cancel.token())
          .status()
          .code == ErrorCode::Cancelled);
  failure_document.nodes[0].operation = "test.planar_cancel_throw";
  GraphContext thrown_graph(failure_document);
  auto thrown_plan =
      Compiler(failure_registry).compile(thrown_graph).take_value();
  PS_CHECK(failure_context
               .execute(thrown_plan.plan, small_bindings, thrown_cancel.token())
               .status()
               .code == ErrorCode::Cancelled);
  failure_document.nodes[0].operation = "test.planar_stale_failure";
  changed_graph = std::make_shared<GraphContext>(failure_document);
  auto stale_plan =
      Compiler(failure_registry).compile(*changed_graph).take_value();
  PS_CHECK(
      failure_context.execute(stale_plan.plan, small_bindings).status().code ==
      ErrorCode::Stale);

  OperationRegistry invalid_planar_registry;
  OperationDefinition unsupported_workspace;
  unsupported_workspace.key = "test.planar_workspace";
  unsupported_workspace.traits = failure_traits;
  unsupported_workspace.traits.workspace_bytes = 1;
  unsupported_workspace.planar_callback = copy_region;
  PS_CHECK(invalid_planar_registry
               .register_operation(std::move(unsupported_workspace))
               .code == ErrorCode::InvalidArgument);
  OperationDefinition unsupported_multiplier;
  unsupported_multiplier.key = "test.planar_workspace_multiplier";
  unsupported_multiplier.traits = failure_traits;
  unsupported_multiplier.traits.workspace_input_multiplier = 1;
  unsupported_multiplier.planar_callback = copy_region;
  PS_CHECK(invalid_planar_registry
               .register_operation(std::move(unsupported_multiplier))
               .code == ErrorCode::InvalidArgument);
  OperationDefinition unsupported_port;
  unsupported_port.key = "test.planar_old_image_port";
  unsupported_port.traits = failure_traits;
  unsupported_port.traits.input_schema[0].kind = OperationPortKind::RgbaFloat32;
  unsupported_port.planar_callback = copy_region;
  PS_CHECK(
      invalid_planar_registry.register_operation(std::move(unsupported_port))
          .code == ErrorCode::InvalidArgument);
  OperationDefinition unsupported_typed;
  unsupported_typed.key = "test.planar_typed_port";
  unsupported_typed.traits = failure_traits;
  unsupported_typed.traits.input_schema[0].kind = OperationPortKind::Typed;
  unsupported_typed.planar_callback = copy_region;
  PS_CHECK(
      invalid_planar_registry.register_operation(std::move(unsupported_typed))
          .code == ErrorCode::InvalidArgument);

  auto base_operations = make_default_operation_registry();
  auto inferred_registry = std::make_shared<OperationRegistry>();
  unsigned legacy_callbacks = 0;
  OperationDefinition inferred_mask;
  inferred_mask.key = "test.inferred_mask";
  inferred_mask.traits =
      base_operations->find_traits("mask.threshold").take_value();
  inferred_mask.callback = [&](const OperationInvocation& call) {
    ++legacy_callbacks;
    return Result<Value>(call.inputs[0]);
  };
  PS_CHECK(
      inferred_registry->register_operation(std::move(inferred_mask)).ok());
  OperationDefinition inferred_image;
  inferred_image.key = "test.inferred_image";
  inferred_image.traits.input_count = 1;
  inferred_image.traits.input_schema.resize(1);
  auto& inferred_output = inferred_image.traits.outputs[0];
  inferred_output.shape_rule = OperationShapeRule::PreserveFirstInput;
  inferred_output.output_dtype_rule = OperationDtypeRule::Input;
  inferred_output.output_semantic_rule = OperationSemanticRule::Parameter;
  inferred_output.output_semantic_parameter = "semantic";
  inferred_image.traits.parameter_schema = {
      {"semantic", OperationParameterType::String, true}};
  inferred_image.callback = [&](const OperationInvocation& call) {
    ++legacy_callbacks;
    return Result<Value>(call.inputs[0]);
  };
  PS_CHECK(
      inferred_registry->register_operation(std::move(inferred_image)).ok());
  PS_CHECK(inferred_registry->freeze().ok());
  SemanticDescriptor scalar_description;
  scalar_description.kind = SemanticKind::ScalarField;
  scalar_description.unit = "dimensionless";
  scalar_description.channels = {{"value", "value", "dimensionless"}};
  auto scalar_field =
      Value::create({ElementType::Float32, {1, 1}}, Region::whole({1, 1}),
                    {0, {4, 4}}, std::vector<std::uint8_t>(4),
                    {encode_semantic(scalar_description).take_value()})
          .take_value();
  const std::vector<Value> mask_input{scalar_field};
  const std::vector<Region> mask_demand{scalar_field.region()};
  const std::map<std::string, ParameterValue> threshold{{"threshold", 0.0}};
  PS_CHECK(
      inferred_registry
          ->invoke("test.inferred_mask", {mask_input, mask_demand, threshold})
          .status()
          .code == ErrorCode::TypeMismatch);
  auto generic_field =
      Value::create({ElementType::Float32, {1, 1, 3}}, Region::whole({1, 1, 3}),
                    {0, {12, 12, 4}}, std::vector<std::uint8_t>(12))
          .take_value();
  auto rgb_description = rgba_semantics();
  rgb_description.channels.pop_back();
  rgb_description.association = "none";
  const std::vector<Value> color_inputs{generic_field};
  const std::vector<Region> color_demands{generic_field.region()};
  const std::map<std::string, ParameterValue> image_parameter{
      {"semantic", semantic_parameter(rgb_description).take_value()}};
  PS_CHECK(inferred_registry
               ->invoke("test.inferred_image",
                        {color_inputs, color_demands, image_parameter})
               .status()
               .code == ErrorCode::TypeMismatch);
  PS_CHECK(legacy_callbacks == 0);

  // Physical conversion and storage preserve arbitrary raw numeric bits.
  for (const auto type : {ElementType::UInt8, ElementType::Int64,
                          ElementType::Float32, ElementType::Float64}) {
    const auto scalar_width = Value::element_size(type);
    const ValueDescriptor raw{type, {2, 3, 2}};
    const auto whole = Region::whole(raw.shape);
    std::vector<std::uint8_t> bits(12 * scalar_width);
    for (std::size_t i = 0; i < bits.size(); ++i)
      bits[i] = static_cast<std::uint8_t>((i * 37 + 11) & 255);
    if (type == ElementType::Float32) {
      const std::uint32_t patterns[] = {0x7FC12345U, 0x80000000U, 0x00000000U,
                                        0x7F800000U};
      std::memcpy(bits.data(), patterns, sizeof(patterns));
    }
    if (type == ElementType::Float64) {
      const std::uint64_t patterns[] = {
          0x7FF8000000001234ULL, 0x8000000000000000ULL, 0x0000000000000000ULL,
          0x7FF0000000000000ULL};
      std::memcpy(bits.data(), patterns, sizeof(patterns));
    }
    auto interleaved =
        Value::create(raw, whole,
                      {0,
                       {3 * 2 * static_cast<std::int64_t>(scalar_width),
                        2 * static_cast<std::int64_t>(scalar_width),
                        static_cast<std::int64_t>(scalar_width)}},
                      bits)
            .take_value();
    PlanarImageConfig raw_config;
    raw_config.tile_height = 2;
    raw_config.tile_width = 2;
    auto converted = PlanarImage::import_value(interleaved, raw_config);
    PS_CHECK(converted.ok());
    std::vector<std::uint8_t> roundtrip(bits.size());
    PS_CHECK(
        converted.value().read(whole, roundtrip.data(), roundtrip.size()).ok());
    PS_CHECK(roundtrip == bits);
    auto raw_window = converted.value().acquire(whole).take_value();
    auto run = raw_window.row_run({0, 0, 0}).take_value();
    PS_CHECK(run.samples == 2 && run.bytes == 2 * scalar_width);
    PS_CHECK(std::memcmp(run.data, bits.data(), scalar_width) == 0);
    PS_CHECK(converted.value().byte_offset({0, 0, 1}).value() >=
             converted.value().page_size());
  }
  return 0;
}
int page_run_rollback() {
  using namespace ps;  // NOLINT(build/namespaces)
  PlanarImageConfig config;
  config.channel_axis.reset();
  config.order = ImagePlaneOrder::Continuous;
  auto probe = PlanarImage::create({ElementType::UInt8, {1, 1}}, config);
  PS_CHECK(probe.ok());
  const auto width = probe.value().page_size() * 1027;
  auto created = PlanarImage::create({ElementType::UInt8, {2, width}}, config);
  PS_CHECK(created.ok());
  auto image = created.take_value();
  const std::uint8_t first = 123, second = 211;
  PS_CHECK(image.publish(Region({{0, 1}, {0, 1}}), &first, 1).ok());
  PS_CHECK(image.publish(Region({{1, 1}, {0, 1}}), &second, 1).ok());
  const auto before = image.resident_bytes();
  const Region requested({{0, 2}, {1, width - 1}});
  {
    auto prepared = image.begin_write(requested);
    PS_CHECK(prepared.ok());
    auto window = prepared.take_value();
    auto last = window.row_run({1, width - 1});
    PS_CHECK(last.ok());
    last.value().data[0] = 99;
    // Two old backed pages split fresh runs; each new run crosses the
    // 1024-page batching limit. Destruction rolls back only fresh backing.
  }
  PS_CHECK(image.resident_bytes() == before);
  PS_CHECK(image.backed_bytes() == 2 * image.page_size());
  std::uint8_t observed = 0;
  PS_CHECK(image.read(Region({{0, 1}, {0, 1}}), &observed, 1).ok());
  PS_CHECK(observed == first);
  PS_CHECK(image.read(Region({{1, 1}, {0, 1}}), &observed, 1).ok());
  PS_CHECK(observed == second);
  PS_CHECK(!image.acquire(Region({{1, 1}, {width - 1, 1}})).ok());
  CancellationSource stop;
  stop.cancel();
  PS_CHECK(image.begin_write(requested, stop.token()).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(image.resident_bytes() == before);
  return 0;
}
}  // namespace

int main() {
  if (page_run_rollback())
    return 1;
  if (rectangle_bounds())
    return 1;
  return workflow();
}
