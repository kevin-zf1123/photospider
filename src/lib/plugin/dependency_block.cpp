#include "plugin/dependency_block.hpp"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "plugin/operation_identity.hpp"

namespace ps::plugin_internal {
namespace {
std::uint64_t key_cost(const ValueFragments& value) {
  std::uint64_t cost = 1;
  const auto add = [&](std::uint64_t extra) {
    if (extra > UINT64_MAX - cost) {
      cost = UINT64_MAX;
      return false;
    }
    cost += extra;
    return true;
  };
  if (!add(value.descriptor().shape.size() + value.facets().size()))
    return cost;
  for (const auto& facet : value.facets())
    if (!add(facet.key.size()) || !add(facet.payload.size()))
      return cost;
  for (const auto& box : value.coverage().boxes()) {
    auto count = box.element_count();
    const auto scale = 9 + box.rank();
    if (!count.ok() || count.value() > UINT64_MAX / scale)
      return UINT64_MAX;
    if (!add(1 + 2 * box.rank()) || !add(count.value() * scale))
      return cost;
  }
  return cost;
}
Status append(content_internal::Sha256* hash, const ValueFragments& value,
              const DependencyPhase& phase) {
  hash->integer(static_cast<std::uint32_t>(value.descriptor().element_type));
  hash->integer(value.descriptor().shape.size());
  for (const auto extent : value.descriptor().shape)
    hash->integer(extent);
  contract_internal::append_facets(hash, value.facets());
  hash->integer(value.coverage().boxes().size());
  for (const auto& box : value.coverage().boxes())
    for (const auto& dimension : box.dimensions()) {
      hash->integer(dimension.offset);
      hash->integer(dimension.extent);
    }
  const auto width = Value::element_size(value.descriptor().element_type);
  return value.coverage().visit(
      [&](const auto& at) {
        auto status = phase.consume_work(0);
        if (!status.ok())
          return status;
        std::uint8_t bytes[8]{};
        status = value.read(at, bytes, width);
        if (!status.ok())
          return status;
        std::uint64_t bits = 0;
        if (width == 1) {
          bits = bytes[0];
        } else if (width == 4) {
          std::uint32_t word;
          std::memcpy(&word, bytes, 4);
          bits = word;
        } else {
          std::memcpy(&bits, bytes, 8);
        }
        hash->integer(bits);
        return Status::success();
      },
      UINT64_MAX, phase.query.cancellation);
}
bool same_state(const Value& state, const Value& incoming) {
  if (!state.valid() ||
      state.descriptor().shape != incoming.descriptor().shape ||
      state.descriptor().element_type != incoming.descriptor().element_type ||
      !input_internal::same_facets(state.facets(), incoming.facets()))
    return false;
  const auto& a = state.region().dimensions();
  const auto& b = incoming.region().dimensions();
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].offset != b[i].offset || a[i].extent != b[i].extent)
      return false;
  return true;
}
}  // namespace
Result<Value> evaluate_dependency_block(
    const std::string& contract, const DependencyPhase& phase,
    const DependencyBlockServices& services, std::uint32_t kind,
    std::uint64_t begin, std::uint64_t end, std::uint64_t mode,
    const Value& incoming, const std::function<Result<Value>()>& compute) {
  const auto fail = [&](Status status) {
    return Result<Value>(phase.report_failure(std::move(status)));
  };
  auto status = phase.consume_work(1);
  if (!status.ok())
    return fail(status);
  if (phase.query.kind != ObservationKind::Atomic || !incoming.valid() ||
      !compute || begin >= end)
    return fail(
        Status{ErrorCode::InvalidArgument, "invalid pure block request"});
  auto coverage = Footprint::from_regions(incoming.descriptor().shape,
                                          {incoming.region()}, phase.sets);
  if (!coverage.ok())
    return fail(coverage.status());
  auto state = ValueFragments::create(incoming.descriptor(), incoming.facets(),
                                      coverage.value(), {incoming}, phase.sets);
  if (!state.ok())
    return fail(state.status());
  std::string key;
  if (services.consume_work && services.find && services.publish) {
    std::uint64_t cost = key_cost(state.value());
    for (const auto& input : phase.inputs) {
      const auto extra = key_cost(input);
      if (extra > UINT64_MAX - cost) {
        cost = UINT64_MAX;
        break;
      }
      cost += extra;
    }
    if (cost != UINT64_MAX && services.consume_work(cost)) {
      content_internal::Sha256 hash;
      hash.text("photospider.internal-block.v1");
      hash.text(contract);
      hash.integer(kind);
      hash.integer(begin);
      hash.integer(end);
      hash.integer(mode);
      status = append(&hash, state.value(), phase);
      if (!status.ok())
        return fail(status);
      hash.integer(phase.inputs.size());
      for (const auto& input : phase.inputs) {
        status = append(&hash, input, phase);
        if (!status.ok())
          return fail(status);
      }
      key = hash.finish();
      auto cached = services.find(key);
      if (!cached.ok())
        return fail(cached.status());
      if (cached.value().valid()) {
        if (!same_state(cached.value(), incoming))
          return fail(
              Status{ErrorCode::InvalidArgument, "block state mismatch"});
        // A hit borrows an accounted cache owner, then creates this stage's
        // immutable state. No old source/prefix certificate is imported.
        auto copy = MutableValue::allocate(incoming.descriptor(),
                                           incoming.region(), phase.allocator);
        if (!copy.ok())
          return fail(copy.status());
        auto writer = copy.take_value();
        const auto width =
            Value::element_size(incoming.descriptor().element_type);
        std::uint64_t position = 0;
        status = coverage.value().visit(
            [&](const auto& at) {
              auto stopped = phase.consume_work(1);
              if (!stopped.ok())
                return stopped;
              auto address = cached.value().byte_address(at);
              if (!address.ok())
                return address.status();
              std::memcpy(writer.data() + position * width,
                          cached.value().bytes().data() + address.value(),
                          width);
              ++position;
              return Status::success();
            },
            phase.sets.maximum_work, phase.query.cancellation);
        if (!status.ok())
          return fail(status);
        auto result = std::move(writer).publish(incoming.facets());
        return result.ok() ? result : fail(result.status());
      }
    }
  }
  auto computed = compute();
  if (!computed.ok())
    return fail(computed.status());
  if (!same_state(computed.value(), incoming) ||
      !phase.allocator.owns_allocation(*computed.value().storage()))
    return fail(
        Status{ErrorCode::InvalidArgument, "invalid computed block state"});
  status = phase.consume_work(0);
  if (!status.ok())
    return fail(status);
  if (!key.empty()) {
    status = services.publish(key, computed.value());
    if (!status.ok())
      return fail(status);
  }
  return computed;
}
}  // namespace ps::plugin_internal
