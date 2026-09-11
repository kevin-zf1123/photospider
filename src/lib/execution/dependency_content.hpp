#pragma once

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "data/content_digest.hpp"
#include "photospider/execution/execution.hpp"
#include "plugin/operation_identity.hpp"

namespace ps::execution_internal {
/** @brief Same canonical region framing as InputSnapshot, without copying it.
 * @note No extra validation samples are read; IEEE/integer payload bits enter
 * the digest at their actual width and in logical row-major order.
 */
inline Result<std::string> dependency_value_identity(
    const Value& value, const Region& region, const CancellationToken& cancel) {
  content_internal::Sha256 hash;
  hash.text("photospider.input-region.v2");
  hash.integer(static_cast<std::uint32_t>(value.descriptor().element_type));
  hash.integer(value.descriptor().shape.size());
  for (const auto n : value.descriptor().shape)
    hash.integer(n);
  for (const auto& d : region.dimensions()) {
    hash.integer(d.offset);
    hash.integer(d.extent);
  }
  contract_internal::append_facets(&hash, value.facets());
  const auto width = Value::element_size(value.descriptor().element_type);
  auto count = region.element_count();
  if (!count.ok())
    return Result<std::string>(count.status());
  std::vector<std::uint64_t> at(region.rank());
  for (std::uint64_t i = 0; i < count.value(); ++i) {
    if (cancel.cancelled())
      return Result<std::string>(Status{ErrorCode::Cancelled, {}});
    auto position = i;
    for (std::size_t axis = at.size(); axis; --axis) {
      const auto d = region.dimensions()[axis - 1];
      at[axis - 1] = d.offset + position % d.extent;
      position /= d.extent;
    }
    auto address = value.byte_address(at);
    if (!address.ok())
      return Result<std::string>(address.status());
    const auto* source = value.bytes().data() + address.value();
    std::uint64_t bits = 0;
    if (width == 1) {
      bits = *source;
    } else if (width == 4) {
      std::uint32_t word;
      std::memcpy(&word, source, 4);
      bits = word;
    } else {
      std::memcpy(&bits, source, 8);
    }
    hash.integer(bits);
  }
  return Result<std::string>(hash.finish());
}
/** @brief Verifies the complete old source witness against immutable bindings.
 * @note Work is charged before scans; caller-owned hints never bypass bytes.
 * Custom RegionalSource inputs have no identity and cannot enter this cache.
 */
inline Result<std::string> dependency_content_identity(
    const std::vector<ExecutionBinding>& bindings,
    const std::map<std::string, Footprint>& support, std::uint64_t* work,
    const CancellationToken& cancel) {
  if (cancel.cancelled())
    return Result<std::string>(Status{ErrorCode::Cancelled, {}});
  const auto charge = [&](std::uint64_t count) {
    if (count > *work) {
      *work = 0;
      return false;
    }
    *work -= count;
    return true;
  };
  if (!charge(bindings.size()))
    return Result<std::string>(Status{ErrorCode::ResourceExhausted, {}});
  std::map<std::string, const ExecutionBinding*> inputs;
  for (const auto& input : bindings) {
    if (!charge(input.name.size()))
      return Result<std::string>(Status{ErrorCode::ResourceExhausted, {}});
    inputs.emplace(input.name, &input);
  }
  content_internal::Sha256 hash;
  hash.text("photospider.dependency-witness.v1");
  hash.integer(support.size());
  for (const auto& source : support) {
    const auto found = inputs.find(source.first);
    if (found == inputs.end())
      return Result<std::string>(Status{ErrorCode::InvalidArgument, {}});
    const auto& input = *found->second;
    if (!input.snapshot && !input.value.valid())
      return Result<std::string>(Status{ErrorCode::InvalidArgument, {}});
    if (!charge(source.first.size() + 1))
      return Result<std::string>(Status{ErrorCode::ResourceExhausted, {}});
    hash.text(source.first);
    hash.integer(source.second.boxes().size());
    for (const auto& box : source.second.boxes()) {
      auto count = box.element_count();
      if (!count.ok() || !charge(1) || !charge(count.value()) ||
          !charge(2 + 3 * box.rank()))
        return Result<std::string>(Status{ErrorCode::ResourceExhausted, {}});
      const auto& facets =
          input.snapshot ? input.snapshot->facets() : input.value.facets();
      if (!charge(facets.size()))
        return Result<std::string>(Status{ErrorCode::ResourceExhausted, {}});
      for (const auto& facet : facets)
        if (!charge(facet.key.size()) || !charge(facet.payload.size()))
          return Result<std::string>(Status{ErrorCode::ResourceExhausted, {}});
      auto digest =
          input.snapshot
              ? input.snapshot->content_identity(box, {count.value(), cancel})
              : dependency_value_identity(input.value, box, cancel);
      if (!digest.ok())
        return digest;
      hash.text(digest.value());
    }
  }
  return Result<std::string>(hash.finish());
}
}  // namespace ps::execution_internal
