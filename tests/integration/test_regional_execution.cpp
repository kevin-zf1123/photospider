#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "image_vertical/image_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
WorkflowDocument document(std::uint64_t height, std::uint64_t width) {
  auto value = s1_fixture::document();
  value.inputs[0].descriptor.shape = {height, width, 4};
  value.inputs[0].region = Region::whole({height, width, 4});
  value.inputs[0].layout = {0, {static_cast<std::int64_t>(width * 16), 16, 4}};
  return value;
}
struct Reads {
  std::atomic<std::uint64_t> calls{0}, bytes{0};
};
std::shared_ptr<RegionalSource> source(const WorkflowDocument& document,
                                       const std::shared_ptr<Reads>& reads,
                                       std::uint64_t shift = 0) {
  auto value = std::make_shared<RegionalSource>();
  value->descriptor = document.inputs[0].descriptor;
  value->facets = document.inputs[0].facets;
  value->read = [reads, shift](const Region& region, std::uint8_t* bytes,
                               std::uint64_t byte_count, const BufferAllocator&,
                               const CancellationToken& token) {
    ++reads->calls;
    reads->bytes += byte_count;
    std::size_t target = 0;
    const auto yd = region.dimensions()[0], xd = region.dimensions()[1];
    for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
      if (token.cancelled())
        return Result<Region>(
            Status::failure(ErrorCode::Cancelled, "source cancelled"));
      for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
        const float pixel[] = {static_cast<float>((x + shift) % 4) / 8,
                               static_cast<float>(y % 4) / 8, 0, .5F};
        std::memcpy(bytes + target, pixel, sizeof(pixel));
        target += sizeof(pixel);
      }
    }
    return Result<Region>(region);
  };
  return value;
}
ExecutionBindings bindings(std::shared_ptr<const RegionalSource> source) {
  return {{{"image", {}, std::move(source)},
           {"gain", s1_fixture::scalar(2)},
           {"opacity", s1_fixture::scalar(.5F)}}};
}
bool check(ValueView value, std::uint64_t shift = 0) {
  const auto yd = value.region().dimensions()[0],
             xd = value.region().dimensions()[1];
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y)
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      const float expected[] = {static_cast<float>((x + shift) % 4) / 8,
                                static_cast<float>(y % 4) / 8, 0, .25F};
      for (std::uint64_t c = 0; c < 4; ++c) {
        auto offset = value.byte_address({y, x, c});
        if (!offset.ok() || std::memcmp(value.bytes().data() + offset.value(),
                                        &expected[c], sizeof(float)))
          return false;
      }
    }
  return true;
}
Status checking(const std::string&, ValueView value) {
  return check(value) ? Status::success()
                      : Status::failure(ErrorCode::OperationFailed,
                                        "pixel oracle mismatch");
}
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto operations = make_default_operation_registry();
  Compiler compiler(operations);
  // The 64 GiB logical image is never allocated by this regional source.
  auto large = document(65536, 65536);
  GraphContext graph(large);
  PlanningOptions options;
  options.tile_height = 2;
  options.tile_width = 3;
  options.output_regions = {{"result", Region({{100, 5}, {200, 7}, {0, 4}})}};
  auto compiled = compiler.compile(graph, options);
  PS_CHECK(compiled.ok());
  auto reads = std::make_shared<Reads>();
  auto input = bindings(source(large, reads));
  ExecutionContext execution(operations, {2, false, 8, 4096});
  std::vector<std::pair<std::uint64_t, std::uint64_t>> order;
  auto stream = execution.execute_stream(
      compiled.value().plan, input,
      [&](const std::string& name, ValueView value) {
        order.emplace_back(value.region().dimensions()[0].offset,
                           value.region().dimensions()[1].offset);
        return checking(name, value);
      });
  PS_CHECK(stream.ok());
  PS_CHECK(stream.value().tile_count == 9 && reads->calls == 9);
  PS_CHECK(stream.value().source_read_bytes == 5 * 7 * 16);
  PS_CHECK(stream.value().peak_live_bytes == 2 * 2 * 3 * 16);
  PS_CHECK(stream.value().planned_peak_bytes == 3 * 2 * 3 * 16);
  PS_CHECK(stream.value().peak_active_tasks <= 2);
  PS_CHECK(stream.value().operation_timings.size() == 2);
  for (const auto& timing : stream.value().operation_timings) {
    PS_CHECK(timing.invocation_count == 9 &&
             timing.computed_elements == 5 * 7 * 4);
  }
  PS_CHECK(order.front() == std::make_pair(UINT64_C(100), UINT64_C(200)));
  PS_CHECK(order.back() == std::make_pair(UINT64_C(104), UINT64_C(206)));
  PS_CHECK(stream.value().result_digest.empty());
  auto collected = execution.execute(compiled.value().plan, input);
  PS_CHECK(collected.ok() &&
           check(ValueView(collected.value().values.at("result"))));
  const auto& output = collected.value().values.at("result");
  PS_CHECK(output.descriptor().shape == large.inputs[0].descriptor.shape);
  PS_CHECK(output.bytes().size() == 5 * 7 * 16);
  PS_CHECK(output.region().dimensions()[0].offset == 100);
  PS_CHECK(collected.value().diagnostics.peak_live_bytes ==
           5 * 7 * 16 + 2 * 2 * 3 * 16);

  ExecutionContext exact(operations,
                         {2, false, 8, stream.value().planned_peak_bytes});
  PS_CHECK(exact.execute_stream(compiled.value().plan, input, checking).ok());
  ExecutionContext short_budget(
      operations, {2, false, 8, stream.value().planned_peak_bytes - 1});
  PS_CHECK(short_budget.execute_stream(compiled.value().plan, input, checking)
               .status()
               .code == ErrorCode::ResourceExhausted);

  // Independent source snapshots reuse the same plan concurrently.
  auto a = std::async(std::launch::async, [&] {
    return execution.execute_stream(compiled.value().plan, input, checking);
  });
  auto b = std::async(std::launch::async, [&] {
    return execution.execute_stream(
        compiled.value().plan, bindings(source(large, reads, 1)),
        [](const std::string&, ValueView value) {
          return check(value, 1) ? Status::success()
                                 : Status::failure(ErrorCode::OperationFailed,
                                                   "mixed snapshot");
        });
  });
  PS_CHECK(a.get().ok() && b.get().ok());

  // A blocked sink prevents admission of the next source read.
  auto slow_reads = std::make_shared<Reads>();
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, released = false;
  auto slow = std::async(std::launch::async, [&] {
    return execution.execute_stream(
        compiled.value().plan, bindings(source(large, slow_reads)),
        [&](const std::string& name, ValueView value) {
          std::unique_lock<std::mutex> lock(mutex);
          if (!entered) {
            entered = true;
            ready.notify_all();
            ready.wait(lock, [&] { return released; });
          }
          return checking(name, value);
        });
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait(lock, [&] { return entered; });
    PS_CHECK(slow_reads->calls == 1);
    released = true;
    ready.notify_all();
  }
  PS_CHECK(slow.get().ok());

  auto wrong = source(large, reads);
  wrong->descriptor.shape[0]++;
  PS_CHECK(
      execution.execute_stream(compiled.value().plan, bindings(wrong), checking)
          .status()
          .code == ErrorCode::TypeMismatch);
  wrong = source(large, reads);
  const auto good_read = wrong->read;
  wrong->read = [good_read](const Region& r, std::uint8_t* p, std::uint64_t n,
                            const BufferAllocator& alloc,
                            const CancellationToken& token) {
    auto result = good_read(r, p, n, alloc, token);
    if (!result.ok())
      return result;
    auto dimensions = r.dimensions();
    dimensions[0].extent = 0;
    return Result<Region>(Region(dimensions));
  };
  PS_CHECK(
      execution.execute_stream(compiled.value().plan, bindings(wrong), checking)
          .status()
          .code == ErrorCode::TypeMismatch);
  wrong = source(large, reads);
  wrong->read = [good_read](const Region& r, std::uint8_t* p, std::uint64_t n,
                            const BufferAllocator& alloc,
                            const CancellationToken& token) {
    auto result = good_read(r, p, n, alloc, token);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(p, &nan, sizeof(nan));
    return result;
  };
  PS_CHECK(
      execution.execute_stream(compiled.value().plan, bindings(wrong), checking)
          .status()
          .code == ErrorCode::InvalidArgument);
  auto failed = execution.execute_stream(
      compiled.value().plan, input, [](const std::string&, ValueView) {
        return Status::failure(ErrorCode::OperationFailed, "sink failed");
      });
  PS_CHECK(failed.status().code == ErrorCode::OperationFailed);
  PS_CHECK(
      execution.execute_stream(compiled.value().plan, input, checking).ok());

  CancellationSource cancellation;
  unsigned int delivered = 0;
  auto cancelled = execution.execute_stream(
      compiled.value().plan, input,
      [&](const std::string&, ValueView) {
        ++delivered;
        cancellation.cancel();
        return Status::success();
      },
      cancellation.token());
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled && delivered == 1);
  auto stale = execution.execute_stream(compiled.value().plan, input,
                                        [&](const std::string&, ValueView) {
                                          graph.replace(large);
                                          return Status::success();
                                        });
  PS_CHECK(stale.status().code == ErrorCode::Stale);
  PS_CHECK(execution.execute_stream(compiled.value().plan, input, {})
               .status()
               .code == ErrorCode::Stale);

  // Unread Value pixels do not participate in per-region numeric validation.
  auto little_doc = s1_fixture::document();
  GraphContext little_graph(little_doc);
  auto little_plan = compiler.compile(little_graph, s1_fixture::demand());
  PS_CHECK(little_plan.ok());
  auto little_input = s1_fixture::bindings();
  auto bytes = little_input.inputs[0].value.copy_bytes();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(bytes.data(), &nan, sizeof(nan));
  auto pixel_value = Value::create(
      little_doc.inputs[0].descriptor, little_doc.inputs[0].region,
      little_doc.inputs[0].layout, bytes, little_doc.inputs[0].facets);
  PS_CHECK(pixel_value.ok());
  little_input.inputs[0].value = pixel_value.take_value();
  auto unread = execution.execute(little_plan.value().plan, little_input);
  PS_CHECK(unread.ok() && s1_fixture::oracle(unread.value()));
  std::memcpy(bytes.data() + 16, &nan, sizeof(nan));
  pixel_value = Value::create(
      little_doc.inputs[0].descriptor, little_doc.inputs[0].region,
      little_doc.inputs[0].layout, bytes, little_doc.inputs[0].facets);
  PS_CHECK(pixel_value.ok());
  little_input.inputs[0].value = pixel_value.take_value();
  PS_CHECK(
      execution.execute(little_plan.value().plan, little_input).status().code ==
      ErrorCode::InvalidArgument);

  auto current = compiler.compile(graph, options);
  PS_CHECK(current.ok());
  auto throwing = source(large, reads);
  throwing->read = [](const Region&, std::uint8_t*, std::uint64_t,
                      const BufferAllocator&,
                      const CancellationToken&) -> Result<Region> {
    throw std::bad_alloc();
  };
  PS_CHECK(
      execution
          .execute_stream(current.value().plan, bindings(throwing), checking)
          .status()
          .code == ErrorCode::ResourceExhausted);
  PS_CHECK(
      execution.execute_stream(current.value().plan, input, checking).ok());
  auto thrown_sink = execution.execute_stream(
      current.value().plan, input, [](const std::string&, ValueView) -> Status {
        throw std::runtime_error("sink exception");
      });
  PS_CHECK(thrown_sink.status().code == ErrorCode::OperationFailed);
  CancellationSource both;
  auto prioritized = execution.execute_stream(
      current.value().plan, input,
      [&](const std::string&, ValueView) {
        graph.replace(large);
        both.cancel();
        return Status::success();
      },
      both.token());
  PS_CHECK(prioritized.status().code == ErrorCode::Cancelled);

  // Three independently allocated Whole results require two live buffers,
  // regardless of chain length. Retired ancestors must not remain cached.
  auto chain_registry = std::make_shared<OperationRegistry>();
  auto allocate_scalar = [](const OperationInvocation& invocation) {
    auto made = MutableValue::allocate(
        {ElementType::Float64, {1}}, Region::whole({1}), invocation.allocator);
    if (!made.ok())
      return Result<Value>(made.status());
    auto writer = made.take_value();
    const double number = 2;
    std::memcpy(writer.data(), &number, sizeof(number));
    return std::move(writer).publish();
  };
  OperationTraits constant_traits, chain_traits;
  chain_traits.input_count = 1;
  chain_traits.input_schema.resize(1);
  chain_traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  PS_CHECK(
      chain_registry
          ->register_operation({"allocate", constant_traits, allocate_scalar})
          .ok());
  PS_CHECK(chain_registry
               ->register_operation({"copy", chain_traits, allocate_scalar})
               .ok());
  PS_CHECK(chain_registry->freeze().ok());
  WorkflowDocument chain_document;
  chain_document.nodes = {{1, "allocate", {}, {}},
                          {2, "copy", {WorkflowNodeOutput{1, "value"}}, {}},
                          {3, "copy", {WorkflowNodeOutput{2, "value"}}, {}}};
  chain_document.outputs = {{"result", 3, "value"}};
  GraphContext chain_graph(chain_document);
  Compiler chain_compiler(chain_registry);
  auto chain_plan = chain_compiler.compile(chain_graph);
  PS_CHECK(chain_plan.ok());
  ExecutionContext chain_execution(chain_registry, {1, false, 4, 16});
  auto chain_result = chain_execution.execute_stream(
      chain_plan.value().plan, {},
      [](const std::string&, ValueView) { return Status::success(); });
  PS_CHECK(chain_result.ok());
  PS_CHECK(chain_result.value().peak_live_bytes == 16);
  PS_CHECK(chain_result.value().planned_peak_bytes == 16);
  ExecutionContext chain_denied(chain_registry, {1, false, 4, 15});
  PS_CHECK(chain_denied
               .execute_stream(chain_plan.value().plan, {},
                               [](const std::string&, ValueView) {
                                 return Status::success();
                               })
               .status()
               .code == ErrorCode::ResourceExhausted);

  // Whole materializations and unrelated side effects execute once per Run.
  auto whole_registry = std::make_shared<OperationRegistry>();
  PS_CHECK(whole_registry->load_plugin(PS_IMAGE_FIXTURE_PATH).ok());
  OperationTraits traits;
  traits.input_count = 1;
  traits.output_element_type = ElementType::Float32;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.input_schema = {{OperationPortKind::RgbaFloat32, 0, 0}};
  traits.output_schema = traits.input_schema[0];
  std::atomic<unsigned int> whole_calls{0}, effects{0};
  PS_CHECK(
      whole_registry
          ->register_operation({"whole", traits,
                                [&](const OperationInvocation& invocation) {
                                  ++whole_calls;
                                  return Result<Value>(invocation.inputs[0]);
                                }})
          .ok());
  OperationTraits effect;
  effect.side_effect_free = false;
  effect.cacheable = false;
  PS_CHECK(whole_registry
               ->register_operation({"effect", effect,
                                     [&](const OperationInvocation&) {
                                       ++effects;
                                       return Result<Value>(
                                           Value::from_float64(1));
                                     }})
               .ok());
  OperationTraits mask_traits;
  mask_traits.output_element_type = ElementType::Float32;
  mask_traits.shape_rule = OperationShapeRule::Fixed;
  mask_traits.fixed_output_shape = {5, 7};
  float computed_mask = -1;
  PS_CHECK(whole_registry
               ->register_operation(
                   {"computed_mask", mask_traits,
                    [&](const OperationInvocation& invocation) {
                      auto made = MutableValue::allocate(
                          {ElementType::Float32, {5, 7}}, Region::whole({5, 7}),
                          invocation.allocator);
                      if (!made.ok())
                        return Result<Value>(made.status());
                      auto writer = made.take_value();
                      for (std::size_t offset = 0; offset < writer.size();
                           offset += 4)
                        std::memcpy(writer.data() + offset, &computed_mask, 4);
                      return std::move(writer).publish();
                    }})
               .ok());
  PS_CHECK(whole_registry->freeze().ok());
  auto small = document(5, 7);
  small.nodes.insert(small.nodes.begin(),
                     {1, "whole", {WorkflowInputReference{1}}, {}});
  small.nodes[1].inputs[0] = WorkflowNodeOutput{1, "value"};
  small.nodes.push_back({99, "effect", {}, {}});
  small.outputs.push_back({"second", 20, "value"});
  GraphContext small_graph(small);
  Compiler whole_compiler(whole_registry);
  PlanningOptions tiling;
  tiling.tile_height = 2;
  tiling.tile_width = 3;
  auto whole_plan = whole_compiler.compile(small_graph, tiling);
  PS_CHECK(whole_plan.ok());
  ExecutionContext whole_execution(whole_registry, {2, false, 8, 4096});
  auto whole_reads = std::make_shared<Reads>();
  auto streamed = whole_execution.execute_stream(
      whole_plan.value().plan, bindings(source(small, whole_reads)), checking);
  PS_CHECK(streamed.ok() && whole_calls == 1 && effects == 1 &&
           whole_reads->calls == 1);
  PS_CHECK(streamed.value().tile_count == 18);
  auto mask_document = document(5, 7);
  mask_document.nodes = {
      {1, "computed_mask", {}, {}},
      {2,
       "image.mask",
       {WorkflowInputReference{1}, WorkflowNodeOutput{1, "value"}},
       {}}};
  mask_document.outputs = {{"result", 2, "value"}};
  GraphContext mask_graph(mask_document);
  auto mask_plan = whole_compiler.compile(mask_graph, tiling);
  PS_CHECK(mask_plan.ok());
  const auto mask_sink = [](const std::string&, ValueView) {
    return Status::success();
  };
  for (float invalid : {-1.F, std::numeric_limits<float>::quiet_NaN()}) {
    computed_mask = invalid;
    auto invalid_result = whole_execution.execute_stream(
        mask_plan.value().plan, bindings(source(mask_document, whole_reads)),
        mask_sink);
    PS_CHECK(invalid_result.status().code == ErrorCode::OperationFailed);
  }
  computed_mask = .5F;
  PS_CHECK(whole_execution
               .execute_stream(mask_plan.value().plan,
                               bindings(source(mask_document, whole_reads)),
                               mask_sink)
               .ok());
  ExecutionContext denied(whole_registry, {2, false, 8, 100});
  PS_CHECK(denied
               .execute_stream(whole_plan.value().plan,
                               bindings(source(small, whole_reads)), checking)
               .status()
               .code == ErrorCode::ResourceExhausted);
  return 0;
}
