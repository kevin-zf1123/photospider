#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/kernel_registry_fixture.hpp"
#include "support/result_image_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
void affine_source_views() {
  for (int mode = 0; mode < 6; ++mode) {
    ResourceBudget source_root;
    ResourceLimits limits;
    limits.capacity[ResourceKind::Payload] = 0;
    ResourceBudget target_root(limits);
    SchemaTemplate source_schema;
    source_schema.id = "test.affine.source";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = {ElementType::UInt8, {2, 3}};
    StridedLayout layout{0, {3, 1}};
    std::vector<std::uint8_t> bytes{0, 1, 2, 3, 4, 5};
    std::vector<std::uint64_t> target_shape{3, 2};
    ResultTensorViewTransform transform;
    transform.source_axes = {{1, 0, 1, 1}, {0, 0, 1, 1}};
    std::vector<std::uint8_t> expected{0, 3, 1, 4, 2, 5};
    if (mode == 1) {
      slot.descriptor.shape = {6};
      layout = {0, {1}};
      target_shape = {3};
      transform.source_axes = {{0, 5, -2, 1}};
      expected = {5, 3, 1};
    } else if (mode == 2) {
      layout = {5, {-3, -1}};
      transform.reshape = true;
      transform.source_axes.clear();
      expected = {5, 4, 3, 2, 1, 0};
    } else if (mode == 3) {
      slot.descriptor.shape = {UINT64_MAX, UINT64_MAX};
      layout = {0, {0, 0}};
      bytes = {9};
      target_shape = {UINT64_MAX, UINT64_MAX};
      transform.reshape = true;
      transform.source_axes.clear();
    } else if (mode == 4) {
      slot.descriptor.shape = {1};
      layout = {0, {INT64_MIN}};
      bytes = {7};
      target_shape = {1, 1};
      transform.source_axes = {{0, 0, -1, 1}};
      expected = {7};
    } else if (mode == 5) {
      slot.descriptor.shape = {1};
      layout = {0, {0}};
      bytes = {4};
      target_shape = {2, 3};
      transform.source_axes = {{-1, 0, 0, 1}};
      expected = {4, 4, 4, 4, 4, 4};
    }
    source_schema.tensors.push_back(slot);
    auto buffer = take(source_root.allocator().allocate(bytes.size()));
    std::memcpy(buffer.data(), bytes.data(), bytes.size());
    auto storage = std::move(buffer).freeze();
    const auto* owner_token = storage.get();
    auto source_builder =
        take(ResultBuilder::start(source_root, source_schema, "affine.source"));
    require(source_builder
                .publish_tensor(0, Region::whole(slot.sample_shape()), layout,
                                storage)
                .ok(),
            "affine view source publishes");
    auto source = take(source_builder.seal());
    auto window = take(source.acquire_tensor(
        take(source.descriptor()), 0, Region::whole(slot.sample_shape())));
    auto target_schema = source_schema;
    target_schema.id = "test.affine.view";
    target_schema.tensors[0].descriptor.shape = target_shape;
    auto target =
        take(ResultBuilder::start(target_root, target_schema, "affine.view"));
    require(target
                .publish_tensor_view(0, Region::whole(target_shape), window,
                                     transform)
                .ok(),
            "affine transform publishes with zero target payload capacity");
    auto result = take(target.seal());
    source_builder = {};
    source = {};
    window = {};
    storage.reset();
    auto retained = take(result.acquire_tensor(take(result.descriptor()), 0,
                                               Region::whole(target_shape)));
    require(retained.storage_owner_token() == owner_token &&
                target_root.statistics().peak[ResourceKind::Payload] == 0 &&
                source_root.statistics().live[ResourceKind::Payload] ==
                    bytes.size(),
            "view retains source root without payload copy or double charge");
    if (mode == 3) {
      auto run = take(retained.row_run({UINT64_MAX - 1, UINT64_MAX - 1}));
      require(
          run.samples == 1 && run.sample_stride_bytes == 0 && *run.data == 9,
          "huge zero-stride reshape avoids overflowing dense element count");
    } else {
      for (std::size_t i = 0; i < expected.size(); ++i) {
        std::vector<std::uint64_t> at(target_shape.size());
        auto position = i;
        for (std::size_t axis = target_shape.size(); axis;) {
          --axis;
          at[axis] = position % target_shape[axis];
          position /= target_shape[axis];
        }
        require(*take(retained.row_run(at)).data == expected[i],
                "affine point/reshape reads exact logical values");
      }
    }
    retained = {};
    result = {};
    target = {};
    require(source_root.statistics().live[ResourceKind::Payload] == 0,
            "last view retirement releases the independently charged source");
  }
}
void affine_view_failures() {
  ResourceBudget root;
  SchemaTemplate schema;
  schema.id = "test.affine.prefix";
  ResultTensorSpec spec;
  spec.key = "samples";
  spec.descriptor = {ElementType::UInt8, {2}};
  schema.tensors.push_back(spec);
  const Region left({{0, 1}}), right({{1, 1}});
  auto make = [&] {
    auto builder = take(ResultBuilder::start(root, schema, "affine.prefix"));
    return builder;
  };
  auto storage =
      take(TensorSpan::create({ElementType::UInt8, {1}}, left, {0, {0}}, {3}))
          .storage();
  auto first = make();
  require(first.publish_tensor(0, left, {0, {0}}, storage).ok(),
          "first affine prefix");
  auto first_ref = first.reference();
  auto facts = take(first_ref.descriptor(false));
  auto input = take(first_ref.acquire_tensor(facts, 0, left));
  ResultTensorViewTransform identity;
  identity.source_axes = {{0, 0, 1, 1}};
  auto second = make();
  require(second.publish_tensor_view(0, left, input, identity).ok(),
          "first affine ownership edge");
  auto second_ref = second.reference();
  auto second_window = take(
      second_ref.acquire_tensor(take(second_ref.descriptor(false)), 0, left));
  ResultTensorViewTransform translated;
  translated.source_axes = {{0, 0, 1, 1, 1}};
  require(first.publish_tensor_view(0, right, second_window, translated).code ==
              ErrorCode::TypeMismatch,
          "indirect affine ownership cycle rejected before publication");
  require(*take(first_ref.acquire_tensor(facts, 0, left))
                  .row_run({0})
                  .value()
                  .data == 3,
          "failed affine cycle leaves previous captured prefix readable");
  auto unauthorized = make();
  ResultTensorViewTransform outside;
  outside.source_axes = {{0, 1, 1, 1}};
  const auto failed = unauthorized.publish_tensor_view(0, left, input, outside);
  require(
      failed.reason == FailureReason::UnauthorizedRead &&
          failed.detail.origin == FailureOrigin::Protocol &&
          failed.detail.scope == FailureScope::Group &&
          unauthorized.publish_tensor_view(0, left, input, identity).reason ==
              failed.reason,
      "unauthorized transform is sticky with complete first cause");
  auto cancelled = make();
  CancellationSource stop;
  stop.cancel();
  require(cancelled.publish_tensor_view(0, left, input, identity, stop.token())
                      .code == ErrorCode::Cancelled &&
              cancelled.seal().status().code == ErrorCode::Cancelled,
          "pre-cancelled affine view publication is sticky");
  auto fragmented = make();
  auto other =
      take(TensorSpan::create({ElementType::UInt8, {1}}, left, {0, {0}}, {4}))
          .storage();
  require(fragmented.publish_tensor(0, left, {0, {0}}, storage).ok() &&
              fragmented.publish_tensor(0, right, {0, {0}, {1}}, other).ok(),
          "unrelated source blocks");
  auto fragmented_ref = take(fragmented.seal());
  auto fragmented_window = take(fragmented_ref.acquire_tensor(
      take(fragmented_ref.descriptor()), 0, Region::whole({2})));
  auto fragmented_unauthorized = make();
  outside.source_axes = {{-1, 2, 0, 1}};
  require(
      fragmented_unauthorized
                  .publish_tensor_view(0, Region::whole({2}), fragmented_window,
                                       outside)
                  .reason == FailureReason::UnauthorizedRead &&
          fragmented_unauthorized.seal().status().reason ==
              FailureReason::UnauthorizedRead,
      "unavailable physical source cannot hide sticky authorization failure");
  auto uniform = make();
  require(uniform.publish_tensor(0, Region::whole({2}), {0, {0}}, storage).ok(),
          "uniform reshape source");
  auto uniform_ref = take(uniform.seal());
  auto uniform_window = take(uniform_ref.acquire_tensor(
      take(uniform_ref.descriptor()), 0, Region::whole({2})));
  for (const auto* source : {&uniform_window, &fragmented_window}) {
    auto three_schema = schema;
    three_schema.tensors[0].descriptor.shape = {3};
    auto three =
        take(ResultBuilder::start(root, three_schema, "reshape.mismatch"));
    ResultTensorViewTransform reshape;
    reshape.reshape = true;
    auto mismatch =
        three.publish_tensor_view(0, Region::whole({3}), *source, reshape);
    require(mismatch.code == ErrorCode::InvalidArgument &&
                mismatch.detail.origin == FailureOrigin::Protocol &&
                mismatch.detail.scope == FailureScope::Group &&
                three.seal().status().code == ErrorCode::InvalidArgument,
            "reshape cardinality failure is sticky for affine and fragmented "
            "sources");
  }
  auto alias = make();
  const auto payload = root.statistics().live[ResourceKind::Payload];
  require(alias
              .publish_tensor_view(0, Region::whole({2}), fragmented_window,
                                   identity)
              .ok(),
          "fragmented identity retains each physical owner");
  auto alias_ref = take(alias.seal());
  auto alias_window = take(alias_ref.acquire_tensor(
      take(alias_ref.descriptor()), 0, Region::whole({2})));
  require(root.statistics().live[ResourceKind::Payload] == payload &&
              take(alias_window.row_run({0})).data ==
                  take(fragmented_window.row_run({0})).data &&
              take(alias_window.row_run({1})).data ==
                  take(fragmented_window.row_run({1})).data,
          "fragmented view publishes no dense copy");
}
void identity_retains_fragmented_input() {
  ResultRef retained;
  {
    auto registry = ps::test::kernel_registry();
    ExecutionContext context(registry);
    auto root = take(context.resource_budget());
    SchemaTemplate schema;
    schema.id = "test.identity.fragments";
    ResultTensorSpec tensor;
    tensor.key = "samples";
    tensor.descriptor = {ElementType::UInt8, {2}};
    schema.tensors.push_back(tensor);
    auto builder =
        take(ResultBuilder::start(root, schema, "identity.fragments"));
    const std::uint8_t expected[] = {3, 241};
    for (std::uint64_t index = 0; index < 2; ++index) {
      auto buffer = take(root.allocator().allocate(1));
      buffer.data()[0] = expected[index];
      require(builder
                  .publish_tensor(0, Region({{index, 1}}), {0, {1}, {index}},
                                  std::move(buffer).freeze())
                  .ok(),
              "separate fragment backing");
    }
    auto source = take(builder.seal());
    WorkflowDocument document;
    WorkflowInputDeclaration input;
    input.id = 1;
    input.name = "input";
    input.result_schema = std::make_shared<SchemaTemplate>(schema);
    document.inputs = {input};
    document.nodes = {{1, "core.identity", {WorkflowInputReference{1}}, {}}};
    document.outputs = {{"out", 1, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    retained =
        take(context.execute(plan, {{{"input", source}}})).results.at("out");
    auto result_window = take(retained.acquire_tensor(
        take(retained.descriptor()), 0, Region::whole({2})));
    auto source_window = take(source.acquire_tensor(take(source.descriptor()),
                                                    0, Region::whole({2})));
    for (std::uint64_t index = 0; index < 2; ++index) {
      auto output = take(result_window.row_run({index}));
      require(*output.data == expected[index] &&
                  output.data == take(source_window.row_run({index})).data,
              "identity retains exact bits from fragmented owners");
    }
  }
  std::uint8_t value = 0;
  require(retained.read_tensor(take(retained.descriptor()), 0, {1}, &value, 1)
                  .ok() &&
              value == 241,
          "fragmented identity survives context and source handles");
}
void view_graph_and_growth() {
  ResourceBudget root;
  auto spec = schema(true);
  spec.tensors[0].batch_axes[0] = spec.tensors[0].batch_axes[1] = 1;
  spec.tensors[0].descriptor.shape = {1, 2};
  auto first = take(ResultBuilder::start(root, spec, "view.graph.first"));
  auto second = take(ResultBuilder::start(root, spec, "view.graph.second"));
  const Region left({{0, 1}, {0, 1}, {0, 1}, {0, 1}});
  const Region right({{0, 1}, {0, 1}, {0, 1}, {1, 1}});
  const float sample = .5F;
  const ByteView bytes(reinterpret_cast<const std::uint8_t*>(&sample), 4);
  require(first.publish_tensor(0, right, bytes).ok() &&
              second.publish_tensor(0, left, bytes).ok(),
          "graph prefixes");
  auto first_ref = first.reference(), second_ref = second.reference();
  auto first_window = take(
      first_ref.acquire_tensor(take(first_ref.descriptor(false)), 0, right));
  auto second_window = take(
      second_ref.acquire_tensor(take(second_ref.descriptor(false)), 0, left));
  require(first.publish_tensor_view(0, left, {&second_window}).ok(),
          "acyclic view edge");
  require(!second.publish_tensor_view(0, right, {&first_window}).ok() &&
              !second.seal().ok(),
          "indirect Result view cycle rejected");
  require(
      !second_ref.acquire_tensor(take(second_ref.descriptor(false)), 0, right)
           .ok(),
      "cycle publishes no coverage");

  // A retained source window does not require the source's unpublished writer
  // lock during another builder's alias proof; callbacks can inspect outputs.
  auto source =
      take(ResultBuilder::start(root, spec, "view.concurrent.source"));
  auto target =
      take(ResultBuilder::start(root, spec, "view.concurrent.target"));
  require(source.publish_tensor(0, left, bytes).ok(),
          "concurrent source prefix");
  auto source_ref = source.reference(), target_ref = target.reference();
  auto retained = take(
      source_ref.acquire_tensor(take(source_ref.descriptor(false)), 0, left));
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, release = false;
  auto writer = std::async(std::launch::async, [&] {
    return source.publish_tensor_kernel(0, right, [&](const auto&) {
      std::unique_lock<std::mutex> lock(mutex);
      entered = true;
      ready.notify_all();
      ready.wait(lock, [&] { return release; });
      lock.unlock();
      return target_ref.descriptor(false).ok()
                 ? Status::success()
                 : Status{ErrorCode::OperationFailed, {}};
    });
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait(lock, [&] { return entered; });
  }
  auto alias = std::async(std::launch::async, [&] {
    return target.publish_tensor_view(0, left, {&retained});
  });
  const auto completes =
      alias.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
    ready.notify_all();
  }
  const auto alias_status = alias.get(), writer_status = writer.get();
  require(completes && alias_status.ok() && writer_status.ok(),
          "view publication avoids source callback lock cycle");

  spec.tensors[0].descriptor.shape = {1, 64};
  auto source64 = take(ResultBuilder::start(root, spec, "growth.source"));
  std::vector<float> pixels(64, .25F);
  require(source64
              .publish_tensor(
                  0, Region::whole(spec.tensors[0].sample_shape()),
                  ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                           256))
              .ok(),
          "growth source");
  source_ref = take(source64.seal());
  ResultGrowthLimits growth;
  growth.maximum_bytes = 4096;
  auto bounded =
      take(ResultBuilder::start(root, spec, "growth.target", growth));
  std::uint64_t successful = 0;
  Status outcome;
  for (std::uint64_t x = 0; x < 64; ++x) {
    const Region cell({{0, 1}, {0, 1}, {0, 1}, {x, 1}});
    auto input =
        take(source_ref.acquire_tensor(take(source_ref.descriptor()), 0, cell));
    outcome = bounded.publish_tensor_view(0, cell, {&input});
    if (!outcome.ok()) {
      break;
    }
    ++successful;
  }
  require(successful > 0 && successful < 64 &&
              outcome.code == ErrorCode::ResourceExhausted,
          "all view metadata shares one producer growth budget");
  target_ref = bounded.reference();
  require(target_ref.acquire_tensor(take(target_ref.descriptor(false)), 0, left)
                  .ok() &&
              !target_ref
                   .acquire_tensor(
                       take(target_ref.descriptor(false)), 0,
                       Region({{0, 1}, {0, 1}, {0, 1}, {successful, 1}}))
                   .ok() &&
              !bounded.seal().ok(),
          "growth failure preserves earlier view and rejects new publication");
}
void large_view() {
  ResourceBudget root;
  auto image_schema = schema(true);
  image_schema.tensors[0].batch_axes[0] =
      image_schema.tensors[0].batch_axes[1] = 1;
  image_schema.tensors[0].descriptor.shape = {1025, 1025};
  auto builder = take(ResultBuilder::start(root, image_schema, "large.source"));
  const auto shape = image_schema.tensors[0].sample_shape();
  require(builder
              .publish_tensor_kernel(
                  0, Region::whole(shape),
                  [](const auto& writers) {
                    for (const auto& writer : writers) {
                      const auto& dims = writer.region().dimensions();
                      for (auto y = dims[2].offset;
                           y < dims[2].offset + dims[2].extent; ++y) {
                        auto run = writer.row_run({0, 0, y, dims[3].offset});
                        if (!run.ok())
                          return run.status();
                        const float value = .25F;
                        for (std::uint64_t i = 0; i < run.value().samples; ++i)
                          std::memcpy(run.value().data + i * 4, &value, 4);
                      }
                    }
                    return Status::success();
                  })
              .ok(),
          "large direct-write publication");
  auto original = take(builder.seal());
  auto window = take(original.acquire_tensor(take(original.descriptor()), 0,
                                             Region::whole(shape)));
  std::vector<ResultViewAxis> axes;
  for (std::int32_t i = 0; i < 4; ++i) {
    axes.push_back({i, 0, 1, 1});
  }
  auto output = take(ResultBuilder::start(root, image_schema, "large.view"));
  const auto before = root.statistics().issued.work;
  ResultTensorViewTransform identity;
  identity.source_axes = std::move(axes);
  require(output.publish_tensor_view(0, Region::whole(shape), window, identity)
              .ok(),
          "view publication above one million samples");
  require(root.statistics().issued.work - before < 100000,
          "fragmented view certification stays below one tenth of logical "
          "sample count");
  auto result = take(output.seal());
  auto alias = take(result.acquire_tensor(take(result.descriptor()), 0,
                                          Region::whole(shape)));
  require(take(alias.row_run({0, 0, 1024, 1024})).data ==
              take(window.row_run({0, 0, 1024, 1024})).data,
          "large view preserves last payload pointer");
}
}  // namespace
int main() {
  try {
    affine_source_views();
    affine_view_failures();
    identity_retains_fragmented_input();
    view_graph_and_growth();
    large_view();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
