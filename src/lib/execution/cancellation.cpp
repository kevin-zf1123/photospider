#include "photospider/execution/cancellation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "execution/cancellation_poll.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps {
struct CancellationToken::State {
  explicit State(const ResourceBudget* budget = nullptr)
      : sources(budget
                    ? ResourceAllocator<std::shared_ptr<const State>>(*budget)
                    : ResourceAllocator<std::shared_ptr<const State>>{}) {}
  std::atomic<bool> flag{false};
  std::vector<std::shared_ptr<const State>,
              ResourceAllocator<std::shared_ptr<const State>>>
      sources;
};
execution_internal::CancellationPoll
execution_internal::CancellationPoll::borrow(
    const CancellationToken& token) noexcept {
  CancellationPoll result;
  if (token.state_) {
    result.flags[result.size++] = &token.state_->flag;
    for (const auto& source : token.state_->sources)
      result.flags[result.size++] = &source->flag;
  }
  return result;
}
bool CancellationToken::cancelled() const noexcept {
  if (!state_)
    return false;
  if (state_->flag.load(std::memory_order_acquire))
    return true;
  for (const auto& source : state_->sources)
    if (source->flag.load(std::memory_order_acquire))
      return true;
  return false;
}
Result<CancellationToken> CancellationToken::combine_impl(
    const CancellationToken* tokens, std::size_t count,
    const ResourceBudget* budget) {
  if (count > 64)
    return Result<CancellationToken>(Status{ErrorCode::ResourceExhausted, {}});
  std::array<std::shared_ptr<const State>, 64> flags;
  std::size_t used = 0;
  const auto add = [&](const std::shared_ptr<const State>& source) {
    if (std::find(flags.begin(), flags.begin() + used, source) !=
        flags.begin() + used)
      return true;
    if (used == flags.size())
      return false;
    flags[used++] = source;
    return true;
  };
  for (std::size_t i = 0; i < count; ++i) {
    const auto& token = tokens[i];
    if (!token.state_)
      continue;
    if (token.state_->sources.empty()) {
      if (!add(token.state_))
        return Result<CancellationToken>(
            Status{ErrorCode::ResourceExhausted, {}});
    } else {
      for (const auto& source : token.state_->sources)
        if (!add(source))
          return Result<CancellationToken>(
              Status{ErrorCode::ResourceExhausted, {}});
    }
  }
  if (!used)
    return Result<CancellationToken>(CancellationToken{});
  if (used == 1)
    return Result<CancellationToken>(CancellationToken(flags[0]));
  auto state = budget ? std::allocate_shared<State>(
                            ResourceAllocator<State>(*budget), budget)
                      : std::make_shared<State>();
  state->sources.assign(flags.begin(), flags.begin() + used);
  return Result<CancellationToken>(CancellationToken(std::move(state)));
}
Result<CancellationToken> CancellationToken::combine(
    const std::vector<CancellationToken>& tokens) {
  return combine_impl(tokens.data(), tokens.size(),
                      resource_internal::metadata_budget());
}
Result<CancellationToken> CancellationToken::combine(
    std::initializer_list<CancellationToken> tokens) {
  return combine_impl(tokens.begin(), tokens.size(),
                      resource_internal::metadata_budget());
}
Result<CancellationToken> CancellationToken::combine(
    const std::vector<CancellationToken>& tokens,
    const ResourceBudget& budget) {
  try {
    return combine_impl(tokens.data(), tokens.size(), &budget);
  } catch (const std::bad_alloc&) {
    return Result<CancellationToken>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<CancellationToken> CancellationToken::combine(
    std::initializer_list<CancellationToken> tokens,
    const ResourceBudget& budget) {
  try {
    return combine_impl(tokens.begin(), tokens.size(), &budget);
  } catch (const std::bad_alloc&) {
    return Result<CancellationToken>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
CancellationSource::CancellationSource() {
  auto* budget = resource_internal::metadata_budget();
  state_ =
      budget ? std::allocate_shared<CancellationToken::State>(
                   ResourceAllocator<CancellationToken::State>(*budget), budget)
             : std::make_shared<CancellationToken::State>();
}
// NOLINTBEGIN(whitespace/indent_namespace)
CancellationSource::CancellationSource(const ResourceBudget& budget)
    : state_(std::allocate_shared<CancellationToken::State>(
          ResourceAllocator<CancellationToken::State>(budget), &budget)) {}
// NOLINTEND
bool CancellationSource::cancel() noexcept {
  bool expected = false;
  return state_->flag.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel);
}
}  // namespace ps
