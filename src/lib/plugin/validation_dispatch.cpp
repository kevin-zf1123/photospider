#include "plugin/validation_dispatch.hpp"

#include <utility>
#include <vector>

#include "plugin/operation_exception.hpp"

namespace ps::plugin_internal {
namespace {
Status denied() {
  return {ErrorCode::InvalidArgument,
          "validator read outside granted coverage",
          FailureReason::UnauthorizedRead,
          {FailureOrigin::Protocol, FailureScope::Group}};
}
Status domain(Status status, ErrorCode code) {
  if (!status.ok() &&
      (status.code == ErrorCode::InvalidArgument ||
       status.code == ErrorCode::OperationFailed) &&
      (status.detail.origin == FailureOrigin::Unspecified ||
       status.detail.origin == FailureOrigin::Domain)) {
    status.code = code;
    status.detail.origin = FailureOrigin::Domain;
  }
  return status;
}
Result<bool> covers(const Footprint& granted, const Region& region,
                    const CancellationToken& cancellation,
                    const std::function<Status(std::uint64_t)>& consume) {
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = consume;
  auto requested = Footprint::from_regions(granted.shape(), {region}, limits);
  if (!requested.ok())
    return Result<bool>(requested.status());
  auto outside = requested.value().subtract(granted, limits);
  return outside.ok() ? Result<bool>(outside.value().empty())
                      : Result<bool>(outside.status());
}
}  // namespace
Status validate_tensor_contract(
    const PreparedOperation& prepared, std::uint32_t port,
    const SchemaTemplate& schema, std::uint32_t slot,
    const ResultTensorInput& input, const Footprint& granted,
    const ResourceBudget& resources, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume,
    ErrorCode domain_failure) try {
  if (!prepared.has_validators(false, port) || granted.empty())
    return Status::success();
  Status service_failure;
  const auto observe = [&](Status status) {
    if (!status.ok() && service_failure.ok())
      service_failure = status;
    return status;
  };
  ValidationContext context(schema, resources, cancellation);
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = consume;
  auto all = Footprint::all(granted.shape(), limits);
  if (!all.ok())
    return all.status();
  context.complete = schema.fields.empty() && schema.tensors.size() == 1 &&
                     granted == all.value();
  context.tensor_slot = slot;
  context.samples = &granted;
  context.consume = [&](auto n) { return observe(consume(n)); };
  context.tensor_coverage = [&](auto index) {
    return index == slot ? &granted : nullptr;
  };
  context.field_range = [&](std::uint32_t) {
    return Result<std::pair<std::uint64_t, std::uint64_t>>(observe(denied()));
  };
  context.read_field = [&](std::uint32_t, std::uint64_t, std::uint64_t) {
    return Result<std::shared_ptr<const CpuStorage>>(observe(denied()));
  };
  context.acquire_tensor = [&](std::uint32_t index, const Region& region) {
    using Answer = Result<ResultTensorReadWindow>;
    if (index != slot)
      return Answer(observe(denied()));
    auto included = covers(granted, region, cancellation, context.consume);
    if (!included.ok())
      return Answer(observe(included.status()));
    if (!included.value())
      return Answer(observe(denied()));
    auto window = input.acquire(region, cancellation);
    if (!window.ok())
      observe(window.status());
    return window;
  };
  auto status = prepared.validate_port(false, port, context);
  return service_failure.ok() ? domain(status, domain_failure)
                              : service_failure;
} catch (...) {
  return current_operation_exception();
}
Status validate_result_contract(
    const PreparedOperation& prepared, bool output, std::uint32_t port,
    const ResultRef& result, const ResultDescriptor& descriptor,
    const ResultDescriptor* previous, const ResourceBudget& resources,
    const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume,
    std::uint64_t maximum_window, ErrorCode domain_failure) try {
  if (!prepared.has_validators(output, port))
    return Status::success();
  const auto& schema = result.schema();
  ResourceVector<Footprint> coverage{ResourceAllocator<Footprint>(resources)};
  coverage.reserve(schema.tensors.size());
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = consume;
  for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
    auto samples = descriptor.tensor_coverage(slot);
    if (previous) {
      auto difference =
          samples.subtract(previous->tensor_coverage(slot), limits);
      if (!difference.ok())
        return difference.status();
      samples = difference.take_value();
    }
    coverage.push_back(std::move(samples));
  }
  Status service_failure;
  const auto observe = [&](Status status) {
    if (!status.ok() && service_failure.ok())
      service_failure = status;
    return status;
  };
  ValidationContext context(schema, resources, cancellation);
  context.complete = descriptor.sealed();
  context.consume = [&](auto n) { return observe(consume(n)); };
  context.tensor_coverage = [&](std::uint32_t slot) -> const Footprint* {
    return slot < coverage.size() ? &coverage[slot] : nullptr;
  };
  context.field_range = [&](std::uint32_t field) {
    using Answer = Result<std::pair<std::uint64_t, std::uint64_t>>;
    if (field >= schema.fields.size())
      return Answer(observe(denied()));
    const auto begin = previous ? previous->rows(field) : 0;
    return Answer({begin, descriptor.rows(field) - begin});
  };
  context.read_field = [&](std::uint32_t field, std::uint64_t first,
                           std::uint64_t count) {
    using Answer = Result<std::shared_ptr<const CpuStorage>>;
    auto range = context.field_range(field);
    if (!range.ok())
      return Answer(range.status());
    const auto start = range.value().first, size = range.value().second;
    if (first < start || first - start > size || count > size - (first - start))
      return Answer(observe(denied()));
    auto read = result.prepare_read(descriptor, field, first, count);
    if (!read.ok())
      return Answer(observe(read.status()));
    auto loaded = read.value().load(maximum_window, cancellation);
    if (!loaded.ok())
      observe(loaded.status());
    return loaded;
  };
  context.acquire_tensor = [&](std::uint32_t slot, const Region& region) {
    using Answer = Result<ResultTensorReadWindow>;
    if (slot >= coverage.size())
      return Answer(observe(denied()));
    auto included =
        covers(coverage[slot], region, cancellation, context.consume);
    if (!included.ok())
      return Answer(observe(included.status()));
    if (!included.value())
      return Answer(observe(denied()));
    auto window = result.acquire_tensor(descriptor, slot, region, cancellation);
    if (!window.ok())
      observe(window.status());
    return window;
  };
  auto status = prepared.validate_port(output, port, context);
  return service_failure.ok() ? domain(status, domain_failure)
                              : service_failure;
} catch (...) {
  return current_operation_exception();
}
}  // namespace ps::plugin_internal
