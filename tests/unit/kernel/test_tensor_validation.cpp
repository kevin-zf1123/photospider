#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "plugin/validation_dispatch.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
int typed_validation_work_batches() {
  ResourceBudget root;
  auto schema = multi_result::schema(ElementType::Float32, {1, 513});
  schema.tensors[0].batch_axes = {1, 1};
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].descriptor.shape = {1, 513};
  schema.tensors[0].layout.channel_axis.reset();
  const auto shape = schema.tensors[0].sample_shape();
  const auto samples = take(Footprint::all(shape));
  const auto make = [&](bool invalid) {
    std::vector<float> values(513, .5F);
    if (invalid)
      values[256] = 2.F;
    auto builder = take(
        ResultBuilder::start(root, schema, "validation.input", {}, {}, 1024));
    check(builder.publish_tensor(
        0, Region::whole(shape),
        ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                 values.size() * sizeof(float))));
    return take(builder.seal());
  };
  const auto valid = make(false), invalid = make(true);
  std::vector<std::uint64_t> charges;
  const auto validate = [&](const ResultRef& input,
                            const CancellationToken& token,
                            const std::function<Status(std::uint64_t)>& consume,
                            const std::function<ErrorCode()>& stop = {}) {
    OperationRegistry registry;
    ValidationDefinition validator;
    validator.identity = {"fixture.range", 1, "range/1"};
    validator.prepare = [](const auto&) {
      return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
    };
    validator.validate = [&](const void*, const ValidationContext& c) {
      auto window = c.acquire_tensor(0, Region::whole(shape));
      if (!window.ok())
        return window.status();
      for (std::uint64_t i = 0; i < 513; ++i) {
        if (!(i % 256)) {
          auto status = consume(std::min<std::uint64_t>(256, 513 - i));
          if (!status.ok())
            return status;
        }
        if (token.cancelled())
          return Status{ErrorCode::Cancelled, {}};
        if (stop && stop() != ErrorCode::Ok)
          return Status{stop(), {}};
        auto row = window.value().row_run({0, 0, 0, i});
        if (!row.ok())
          return row.status();
        float value;
        std::memcpy(&value, row.value().data, 4);
        if (value < 0 || value > 1)
          return Status{ErrorCode::TypeMismatch, "range"};
      }
      return Status::success();
    };
    check(registry.register_validator(validator));
    OperationDefinition op;
    op.key = "fixture.range";
    op.traits.outputs = {multi_result::output("value", schema)};
    op.traits.outputs[0].output_schema.validators = {validator.identity};
    op.start_result = [](const auto&,
                         const auto&) -> Result<ResultContinuation> {
      return Result<ResultContinuation>(Status{ErrorCode::OperationFailed, {}});
    };
    check(registry.register_operation(std::move(op)));
    check(registry.freeze());
    auto prepared = take(registry.prepare_operation("fixture.range", {}, {}));
    return plugin_internal::validate_result_contract(
        *prepared, true, 0, input, take(input.descriptor()), nullptr, root,
        token, [](auto) { return Status::success(); }, 4096,
        ErrorCode::TypeMismatch);
  };
  auto status = validate(valid, {}, [&](auto n) {
    charges.push_back(n);
    return Status::success();
  });
  PS_REQUIRE_OK(status);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256, 1}));
  charges.clear();
  status = validate(invalid, {}, [&](auto n) {
    charges.push_back(n);
    return charges.size() == 2 ? Status{ErrorCode::ResourceExhausted, {}}
                               : Status::success();
  });
  PS_CHECK(status.code == ErrorCode::ResourceExhausted);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256}));
  charges.clear();
  status = validate(invalid, {}, [&](auto n) {
    charges.push_back(n);
    return Status::success();
  });
  PS_CHECK(status.code == ErrorCode::TypeMismatch);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256}));
  charges.clear();
  CancellationSource cancellation;
  status = validate(invalid, cancellation.token(), [&](auto n) {
    charges.push_back(n);
    if (charges.size() == 2)
      cancellation.cancel();
    return Status::success();
  });
  PS_CHECK(status.code == ErrorCode::Cancelled);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256}));
  charges.clear();
  status = validate(
      valid, {},
      [&](auto n) {
        charges.push_back(n);
        return Status::success();
      },
      [&] { return charges.size() == 3 ? ErrorCode::Stale : ErrorCode::Ok; });
  PS_CHECK(status.code == ErrorCode::Stale);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256, 1}));
  return 0;
}
int incremental_authorization() {
  ResourceBudget root;
  auto schema = multi_result::schema(ElementType::Float64, {2});
  schema.publication = PublishPolicy::StablePrefix;
  schema.fields = {
      {"rows", ElementType::Float64, {ResultExtentKind::RuntimeCount}, {}}};
  auto builder = take(ResultBuilder::start(root, schema, "incremental"));
  double value = 3;
  check(
      builder.append(0, 1, {reinterpret_cast<const std::uint8_t*>(&value), 8}));
  check(builder.publish(0, 1));
  check(builder.publish_tensor(
      0, Region({{0, 1}}), {reinterpret_cast<const std::uint8_t*>(&value), 8}));
  auto result = builder.reference();
  auto previous = take(result.descriptor(false));
  check(
      builder.append(0, 1, {reinterpret_cast<const std::uint8_t*>(&value), 8}));
  check(builder.publish(0, 2));
  check(builder.publish_tensor(
      0, Region({{1, 1}}), {reinterpret_cast<const std::uint8_t*>(&value), 8}));
  bool violate = false;
  unsigned seen = 0;
  OperationRegistry registry;
  ValidationDefinition validator;
  validator.identity = {"fixture.delta", 1, "delta/1"};
  validator.prepare = [](const auto&) {
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  validator.validate = [&](const void*, const ValidationContext& c) {
    ++seen;
    auto range = c.field_range(0);
    if (!range.ok() ||
        range.value() != std::make_pair(UINT64_C(1), UINT64_C(1)))
      return Status{ErrorCode::Internal, "field delta"};
    if (!c.tensor_coverage(0) ||
        c.tensor_coverage(0)->element_count().value() != 1 ||
        !c.tensor_coverage(0)->contains({1}))
      return Status{ErrorCode::Internal, "tensor delta"};
    auto field = c.read_field(0, 1, 1);
    if (!field.ok())
      return field.status();
    auto tensor = c.acquire_tensor(0, Region({{1, 1}}));
    if (!tensor.ok())
      return tensor.status();
    if (violate)
      (void)c.read_field(0, 0, 1);
    return Status::success();
  };
  check(registry.register_validator(validator));
  OperationDefinition op;
  op.key = "fixture.delta";
  op.traits.outputs = {multi_result::output("value", schema)};
  op.traits.outputs[0].output_schema.validators = {validator.identity};
  op.start_result = [](const auto&, const auto&) -> Result<ResultContinuation> {
    return Result<ResultContinuation>(Status{ErrorCode::OperationFailed, {}});
  };
  check(registry.register_operation(op));
  check(registry.freeze());
  auto prepared = take(registry.prepare_operation(op.key, {}, {}));
  auto validate = [&] {
    return plugin_internal::validate_result_contract(
        *prepared, true, 0, result, take(result.descriptor(false)), &previous,
        root, {}, [](auto) { return Status::success(); }, 4096,
        ErrorCode::TypeMismatch);
  };
  PS_REQUIRE_OK(validate());
  PS_CHECK(seen == 1);
  violate = true;
  auto rejected = validate();
  PS_CHECK(!rejected.ok() &&
           rejected.reason == FailureReason::UnauthorizedRead);
  return 0;
}
}  // namespace
int main() {
  return typed_validation_work_batches() || incremental_authorization();
}
