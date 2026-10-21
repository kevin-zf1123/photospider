#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "photospider/data/result.hpp"

namespace ps::data_internal {
// A globally unique identity can be reserved before a Result's data exists.
// Only this move-only token can construct its one canonical Result Impl.
// Dropped or failed reservations are never reused. The token owns no payload.
class ResultIdentity final {
 public:
  ResultIdentity() = default;
  ResultIdentity(const ResultIdentity&) = delete;
  ResultIdentity& operator=(const ResultIdentity&) = delete;
  ResultIdentity(ResultIdentity&& other) noexcept
      : id_(std::exchange(other.id_, 0)) {}
  ResultIdentity& operator=(ResultIdentity&& other) noexcept {
    if (this != &other)
      id_ = std::exchange(other.id_, 0);
    return *this;
  }
  static Result<ResultIdentity> reserve();
  std::uint64_t id() const noexcept { return id_; }
  Result<ResultBuilder> materialize(ResourceBudget budget,
                                    const SchemaTemplate& schema,
                                    std::string_view scope,
                                    ResultGrowthLimits limits = {},
                                    std::uint64_t tile_extent = 128) &&;

 private:
  friend class ps::ResultBuilder;
  explicit ResultIdentity(std::uint64_t id) : id_(id) {}
  std::uint64_t id_ = 0;
};
}  // namespace ps::data_internal
