#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
ByteView bytes(const double& value) {
  return {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)};
}
int tensor_transaction() {
  for (bool spatial : {false, true}) {
    for (unsigned mode = 0; mode < 4; ++mode) {
      ResourceBudget root;
      auto schema = multi_result::schema(ElementType::Float64, {1, 4});

      schema.tensors[0].layout.spatial = spatial;
      schema.tensors[0].layout.channel_axis.reset();
      auto builder =
          take(ResultBuilder::start(root, schema, "transaction", {}, 2));
      const double first = 7, next = 11;
      const Region left({{0, 1}, {0, 1}}), right({{0, 1}, {2, 1}});
      check(builder.publish_tensor(0, left, bytes(first)));
      auto reference = builder.reference();
      const auto before = take(reference.descriptor(false));
      CancellationSource cancellation;
      unsigned calls = 0;
      auto status = builder.publish_tensor_kernel(
          0, right,
          [&](const auto& windows) {
            ++calls;
            double prior = 0;
            check(reference.read_tensor(before, 0, {0, 0}, &prior, 8));
            if (prior != first ||
                reference.acquire_tensor(before, 0, right).ok())
              return Status{ErrorCode::OperationFailed, "prefix isolation"};
            auto run = take(windows[0].row_run({0, 2}));
            std::memcpy(run.data, &next, sizeof(next));
            if (mode == 1)
              return Status{ErrorCode::OperationFailed, "write rejected"};
            if (mode == 2)
              cancellation.cancel();
            if (mode == 3)
              static_cast<void>(builder.seal());
            return Status::success();
          },
          cancellation.token());
      PS_CHECK(calls == 1);
      auto after = take(reference.descriptor(false));
      PS_CHECK(reference.acquire_tensor(after, 0, left).ok());
      PS_CHECK(!reference.acquire_tensor(before, 0, right).ok());
      if (mode) {
        const auto expected = mode == 1   ? ErrorCode::OperationFailed
                              : mode == 2 ? ErrorCode::Cancelled
                                          : ErrorCode::InvalidArgument;
        PS_CHECK(status.code == expected);
        PS_CHECK(after.revision() == before.revision());
        PS_CHECK(!reference.acquire_tensor(after, 0, right).ok());
        PS_CHECK(builder.seal().status().code == expected);
      } else {
        check(status);
        // Data publication may seal sparse coverage. The executor separately
        // requires complete computation-unit coverage from operation callbacks.
        auto sealed = take(builder.seal());
        double value = 0;
        check(sealed.read_tensor(take(sealed.descriptor()), 0, {0, 2}, &value,
                                 8));
        PS_CHECK(value == next);
        PS_CHECK(!sealed
                      .acquire_tensor(take(sealed.descriptor()), 0,
                                      Region::whole({1, 4}))
                      .ok());
      }
    }
  }
  return 0;
}
int irregular_partition() {
  ResourceBudget root;
  auto schema = multi_result::schema(ElementType::Float64, {2, 2});
  auto builder = take(ResultBuilder::start(root, schema, "partition"));
  const double left[] = {3, 3};
  const double upper = 5, lower = 7;
  check(builder.publish_tensor(
      0, Region({{0, 2}, {0, 1}}),
      ByteView(reinterpret_cast<const std::uint8_t*>(left), sizeof(left))));
  check(builder.publish_tensor(0, Region({{0, 1}, {1, 1}}), bytes(upper)));
  check(builder.publish_tensor(0, Region({{1, 1}, {1, 1}}), bytes(lower)));
  auto result = take(builder.seal());
  auto window = take(result.acquire_tensor(take(result.descriptor()), 0,
                                           Region::whole({2, 2})));
  for (unsigned y = 0; y < 2; ++y)
    for (unsigned x = 0; x < 2; ++x) {
      double value = 0;
      std::memcpy(&value, take(window.row_run({y, x})).data, sizeof(value));
      PS_CHECK(value == (x ? (y ? lower : upper) : 3));
    }
  return 0;
}
int incremental_owners() {
  ResourceLimits limits;
  limits.maximum_work = 400000;
  ResourceBudget root(limits);
  constexpr std::uint64_t count = 2048;
  auto schema = multi_result::schema(ElementType::Float64, {count});
  auto builder = take(ResultBuilder::start(root, schema, "incremental", {}, 1));
  std::vector<TensorSpan> spans;
  for (std::uint64_t i = 0; i < count; ++i) {
    auto buffer = take(root.allocator().allocate(sizeof(double)));
    const double value = i;
    std::memcpy(buffer.data(), &value, sizeof(value));
    auto storage = std::move(buffer).freeze();
    spans.push_back(take(
        TensorSpan::from_storage({ElementType::Float64, {count}},
                                 Region({{i, 1}}), {0, {8}, {i}}, storage)));
    check(builder.publish_tensor(0, Region({{i, 1}}), {0, {8}, {i}}, storage));
  }
  auto result = take(builder.seal());
  {
    ResourceAllocationScope scope(root);
    FootprintLimits work;
    work.consume_work = [&](auto count) { return root.consume({count}); };
    auto fragmented =
        take(Tensor::create({ElementType::Float64, {count}},
                            take(Footprint::all({count})), spans, work));
    PS_CHECK(take(fragmented.retained_bytes(work)) == count * sizeof(double));
    FootprintLimits bounded;
    bounded.maximum_work = 1;
    PS_CHECK(fragmented.retained_bytes(bounded).status().reason ==
             FailureReason::WorkLimit);
    CancellationSource cancellation;
    cancellation.cancel();
    bounded.cancellation = cancellation.token();
    PS_CHECK(fragmented.retained_bytes(bounded).status().code ==
             ErrorCode::Cancelled);
  }
  double last = 0;
  check(
      result.read_tensor(take(result.descriptor()), 0, {count - 1}, &last, 8));
  PS_CHECK(last == count - 1);
  PS_CHECK(root.statistics().live[ResourceKind::Payload] ==
           count * sizeof(double));
  // One physical owner reused across members consumes the growth allowance
  // once.
  auto shared = take(root.allocator().allocate(4096));
  const double value = 17;
  std::memcpy(shared.data(), &value, sizeof(value));
  auto storage = std::move(shared).freeze();
  auto pair = multi_result::schema();
  pair.tensors.push_back(pair.tensors[0]);
  pair.tensors[1].key = "other";
  auto aliases = take(ResultBuilder::start(root, pair, "aliases", {4608}));
  for (unsigned slot = 0; slot < 2; ++slot)
    check(aliases.publish_tensor(slot, Region::whole({1}), {0, {8}}, storage));
  auto aliased = take(aliases.seal());
  for (unsigned slot = 0; slot < 2; ++slot) {
    check(aliased.read_tensor(take(aliased.descriptor()), slot, {0}, &last, 8));
    PS_CHECK(last == value);
  }
  return 0;
}
int affine_views() {
  ResourceBudget root;
  auto schema = multi_result::schema(ElementType::Float64, {4});
  ResultRef view;
  {
    const double values[] = {2, 3, 5, 7};
    auto source = take(ResultBuilder::start(root, schema, "source"));
    check(source.publish_tensor(
        0, Region::whole({4}),
        ByteView(reinterpret_cast<const std::uint8_t*>(values),
                 sizeof(values))));
    auto input = take(source.seal());
    auto window = take(
        input.acquire_tensor(take(input.descriptor()), 0, Region::whole({4})));
    ResultTensorViewTransform reverse;
    reverse.source_axes = {{0, 3, -1}};
    auto builder = take(ResultBuilder::start(root, schema, "reverse"));
    check(builder.publish_tensor_view(0, Region::whole({4}), window, reverse));
    view = take(builder.seal());
    auto output = take(
        view.acquire_tensor(take(view.descriptor()), 0, Region::whole({4})));
    PS_CHECK(take(output.row_run({0})).data == take(window.row_run({3})).data);
    PS_CHECK(take(output.row_run({0})).sample_stride_bytes == -8);
  }
  for (unsigned i = 0; i < 4; ++i) {
    const double expected[] = {7, 5, 3, 2};
    double value = 0;
    check(view.read_tensor(take(view.descriptor()), 0, {i}, &value, 8));
    PS_CHECK(value == expected[i]);
  }
  ResultViewAxis axis{0, UINT64_MAX, -1, UINT64_MAX};
  PS_CHECK(take(axis.source_coordinate(UINT64_MAX)) == UINT64_MAX);
  PS_CHECK(axis.source_coordinate(UINT64_MAX - 1).status().code ==
           ErrorCode::InvalidArgument);
  axis = {0, UINT64_MAX, INT64_MIN, 0};
  PS_CHECK(take(axis.source_coordinate(1)) == UINT64_MAX / 2);
  PS_CHECK(!axis.source_coordinate(2).ok());

  auto first = take(ResultBuilder::start(root, schema, "cycle.first"));
  const double value = 13;
  const Region left({{0, 1}}), right({{1, 1}});
  check(first.publish_tensor(0, left, bytes(value)));
  auto first_ref = first.reference();
  auto first_facts = take(first_ref.descriptor(false));
  auto window = take(first_ref.acquire_tensor(first_facts, 0, left));
  ResultTensorViewTransform identity;
  identity.source_axes = {{0, 0, 1}};
  auto second = take(ResultBuilder::start(root, schema, "cycle.second"));
  check(second.publish_tensor_view(0, left, window, identity));
  auto second_ref = second.reference();
  auto back = take(
      second_ref.acquire_tensor(take(second_ref.descriptor(false)), 0, left));
  ResultTensorViewTransform translated;
  translated.source_axes = {{0, 0, 1, 1}};
  PS_CHECK(first.publish_tensor_view(0, right, back, translated).code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(take(first_ref.descriptor(false)).revision() ==
           first_facts.revision());
  double retained = 0;
  check(first_ref.read_tensor(first_facts, 0, {0}, &retained, 8));
  PS_CHECK(retained == value);
  auto unauthorized = take(ResultBuilder::start(root, schema, "unauthorized"));
  translated.source_axes = {{-1, 1, 0}};
  PS_CHECK(
      unauthorized.publish_tensor_view(0, left, window, translated).reason ==
      FailureReason::UnauthorizedRead);
  return 0;
}
}  // namespace
int main() {
  try {
    return tensor_transaction() || affine_views() || irregular_partition() ||
           incremental_owners();
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
