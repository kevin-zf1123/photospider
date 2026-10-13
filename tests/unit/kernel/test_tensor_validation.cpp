#include <cstdint>
#include <functional>
#include <vector>

#include "data/value_validation.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
int typed_validation_work_batches() {
  ResourceBudget root;
  const auto facet = take(encode_semantic(coverage_semantics()));
  auto schema = multi_result::schema(ElementType::Float32, {1, 513});
  schema.tensors[0].batch_axes = {1, 1};
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].descriptor.shape = {1, 513};
  schema.tensors[0].layout.channel_axis.reset();
  schema.tensors[0].facets = {facet};
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
    return input_internal::validate_tensor_samples(
        input, take(input.descriptor()), 0, samples, root,
        ErrorCode::TypeMismatch, token, stop, consume);
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
}  // namespace
int main() {
  return typed_validation_work_batches();
}
