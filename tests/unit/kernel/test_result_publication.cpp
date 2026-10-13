#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
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
int field_prefix() {
  ResourceBudget root;
  SchemaTemplate schema;
  schema.id = "test.publication.fields";
  schema.publication = PublishPolicy::StablePrefix;
  schema.fields.push_back(
      {"rows", ElementType::Float64, {ResultExtentKind::RuntimeCount, 0}, {}});
  auto builder = take(ResultBuilder::start(root, schema, "prefix"));
  const double first = 7, second = 11;
  check(builder.append(0, 1, bytes(first)));
  check(builder.publish(0, 1));
  auto captured = take(builder.reference().capture());
  const auto facts = take(captured.descriptor(false));
  check(builder.append(0, 1, bytes(second)));
  check(builder.publish(0, 2));
  PS_CHECK(take(captured.descriptor(false)).rows(0) == 1);
  PS_CHECK(!captured.prepare_read(facts, 0, 0, 2).ok());
  PS_CHECK(builder.publish(0, 3).code == ErrorCode::TypeMismatch);
  PS_CHECK(builder.seal().status().code == ErrorCode::TypeMismatch);
  builder = {};
  auto storage = take(take(captured.prepare_read(facts, 0, 0, 1)).load(8));
  double value = 0;
  std::memcpy(&value, storage->bytes().data(), sizeof(value));
  PS_CHECK(value == first);

  schema.publication = PublishPolicy::CompleteBundle;
  schema.fields[0].rows = {ResultExtentKind::Fixed, 2};
  auto incomplete = take(ResultBuilder::start(root, schema, "fixed"));
  check(incomplete.append(0, 1, bytes(first)));
  check(incomplete.publish(0, 1));
  PS_CHECK(!incomplete.reference().descriptor(false).ok());
  PS_CHECK(incomplete.seal().status().code == ErrorCode::TypeMismatch);
  return 0;
}
int tensor_transaction() {
  for (bool spatial : {false, true}) {
    for (unsigned mode = 0; mode < 4; ++mode) {
      ResourceBudget root;
      auto schema = multi_result::schema(ElementType::Float64, {1, 4});
      schema.publication = PublishPolicy::StablePrefix;
      schema.tensors[0].layout.spatial = spatial;
      schema.tensors[0].layout.channel_axis.reset();
      auto builder =
          take(ResultBuilder::start(root, schema, "transaction", {}, {}, 2));
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
    reverse.source_axes = {{0, 3, -1, 1}};
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
  ResultViewAxis axis{0, UINT64_MAX, -1, 1, UINT64_MAX};
  PS_CHECK(take(axis.source_coordinate(UINT64_MAX)) == UINT64_MAX);
  PS_CHECK(axis.source_coordinate(UINT64_MAX - 1).status().code ==
           ErrorCode::InvalidArgument);
  axis = {0, UINT64_MAX, INT64_MIN, 1, 0};
  PS_CHECK(take(axis.source_coordinate(1)) == UINT64_MAX / 2);
  PS_CHECK(!axis.source_coordinate(2).ok());

  schema.publication = PublishPolicy::StablePrefix;
  auto first = take(ResultBuilder::start(root, schema, "cycle.first"));
  const double value = 13;
  const Region left({{0, 1}}), right({{1, 1}});
  check(first.publish_tensor(0, left, bytes(value)));
  auto first_ref = first.reference();
  auto first_facts = take(first_ref.descriptor(false));
  auto window = take(first_ref.acquire_tensor(first_facts, 0, left));
  ResultTensorViewTransform identity;
  identity.source_axes = {{0, 0, 1, 1}};
  auto second = take(ResultBuilder::start(root, schema, "cycle.second"));
  check(second.publish_tensor_view(0, left, window, identity));
  auto second_ref = second.reference();
  auto back = take(
      second_ref.acquire_tensor(take(second_ref.descriptor(false)), 0, left));
  ResultTensorViewTransform translated;
  translated.source_axes = {{0, 0, 1, 1, 1}};
  PS_CHECK(first.publish_tensor_view(0, right, back, translated).code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(take(first_ref.descriptor(false)).revision() ==
           first_facts.revision());
  double retained = 0;
  check(first_ref.read_tensor(first_facts, 0, {0}, &retained, 8));
  PS_CHECK(retained == value);
  auto unauthorized = take(ResultBuilder::start(root, schema, "unauthorized"));
  translated.source_axes = {{-1, 1, 0, 1}};
  PS_CHECK(
      unauthorized.publish_tensor_view(0, left, window, translated).reason ==
      FailureReason::UnauthorizedRead);
  return 0;
}
}  // namespace
int main() {
  try {
    return field_prefix() || tensor_transaction() || affine_views();
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
