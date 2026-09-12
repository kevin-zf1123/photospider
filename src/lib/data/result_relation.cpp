#include "photospider/data/result_relation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace ps {
namespace {
bool valid_support(ResultSupport support) {
  return support.roles && !(support.roles & ~15U) && support.input < 1024 &&
         support.count <= UINT64_MAX - support.first;
}
bool valid_guarantee(DependencyGuarantee guarantee) {
  return guarantee == DependencyGuarantee::Exact ||
         guarantee == DependencyGuarantee::Conservative ||
         guarantee == DependencyGuarantee::Unknown;
}
Status invalid_relation() {
  return Status{ErrorCode::InvalidArgument, "invalid result support relation"};
}
DependencyGuarantee combine_guarantee(DependencyGuarantee a,
                                      DependencyGuarantee b) {
  return static_cast<DependencyGuarantee>(
      std::max(static_cast<unsigned>(a), static_cast<unsigned>(b)));
}
}  // namespace
struct ResultRelation::Impl {
  enum class Kind { Cartesian, Identity, Rows, Union, Compose, Unknown };
  explicit Impl(ResourceBudget value) : budget(std::move(value)) {}
  ResourceBudget budget;
  ResourceLease lease;
  Kind kind = Kind::Unknown;
  DependencyGuarantee guarantee = DependencyGuarantee::Unknown;
  std::uint64_t outputs = 0, row_count = 0;
  std::uint32_t depth = 1, children_count = 0;
  ResultSupport support;
  TemporaryStorage rows;
  std::array<std::shared_ptr<const Impl>, 16> children;
  static Result<std::shared_ptr<Impl>> make(ResourceBudget budget,
                                            std::uint64_t outputs) {
    auto capacity = ResourceCapacity::host(sizeof(Impl), sizeof(Impl));
    capacity[ResourceKind::Entries] = 1;
    auto lease = budget.reserve(capacity);
    if (!lease.ok())
      return Result<std::shared_ptr<Impl>>(lease.status());
    try {
      auto impl = std::make_shared<Impl>(std::move(budget));
      impl->lease = lease.take_value();
      impl->outputs = outputs;
      return Result<std::shared_ptr<Impl>>(std::move(impl));
    } catch (const std::bad_alloc&) {
      return Result<std::shared_ptr<Impl>>(
          Status{ErrorCode::ResourceExhausted, {}});
    }
  }
  Status walk(std::uint64_t output, std::uint64_t* remaining,
              const std::function<Status(ResultSupport)>& visitor) const {
    if (output >= outputs)
      return invalid_relation();
    auto tick = [&]() {
      if (!*remaining)
        return Status{ErrorCode::ResourceExhausted,
                      "result relation work exhausted"};
      auto status = budget.consume({1});
      if (status.ok())
        --*remaining;
      return status;
    };
    auto status = tick();
    if (!status.ok())
      return status;
    switch (kind) {
      case Kind::Unknown:
        return Status{ErrorCode::NotFound, "Unresolved result support"};
      case Kind::Cartesian:
        return support.count ? visitor(support) : Status::success();
      case Kind::Identity:
        return visitor({support.input, support.roles, output, 1});
      case Kind::Rows:
        for (std::uint64_t i = 0; i < row_count; ++i) {
          status = tick();
          if (!status.ok())
            return status;
          auto page =
              rows.read(i * sizeof(ResultRelationRow),
                        sizeof(ResultRelationRow), sizeof(ResultRelationRow));
          if (!page.ok())
            return page.status();
          ResultRelationRow row;
          std::memcpy(&row, page.value()->bytes().data(), sizeof(row));
          if (row.output == output) {
            status = visitor(row.support);
            if (!status.ok())
              return status;
          }
        }
        return Status::success();
      case Kind::Union:
        for (std::uint32_t i = 0; i < children_count; ++i) {
          status = children[i]->walk(output, remaining, visitor);
          if (!status.ok())
            return status;
        }
        return Status::success();
      case Kind::Compose:
        return children[0]->walk(output, remaining, [&](ResultSupport middle) {
          for (std::uint64_t i = 0; i < middle.count; ++i) {
            auto walked = children[1]->walk(middle.first + i, remaining,
                                            [&](ResultSupport leaf) {
                                              leaf.roles |= middle.roles;
                                              return visitor(leaf);
                                            });
            if (!walked.ok())
              return walked;
          }
          return Status::success();
        });
    }
    return invalid_relation();
  }
};
bool ResultRelation::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->budget.same_owner(budget);
}
DependencyGuarantee ResultRelation::guarantee() const noexcept {
  return impl_ ? impl_->guarantee : DependencyGuarantee::Unknown;
}
std::uint64_t ResultRelation::coverage() const noexcept {
  return impl_ ? impl_->outputs : 0;
}
Result<ResultRelation> ResultRelation::cartesian(
    ResourceBudget budget, std::uint64_t outputs, ResultSupport support,
    DependencyGuarantee guarantee) {
  if (!valid_support(support) || !valid_guarantee(guarantee))
    return Result<ResultRelation>(invalid_relation());
  auto made = Impl::make(std::move(budget), outputs);
  if (!made.ok())
    return Result<ResultRelation>(made.status());
  auto impl = made.take_value();
  impl->kind = Impl::Kind::Cartesian;
  impl->guarantee = guarantee;
  impl->support = support;
  ResultRelation relation;
  relation.impl_ = std::move(impl);
  return Result<ResultRelation>(std::move(relation));
}
Result<ResultRelation> ResultRelation::identity(ResourceBudget budget,
                                                std::uint64_t count,
                                                std::uint32_t input,
                                                std::uint32_t roles) {
  auto made = cartesian(std::move(budget), count, {input, roles, 0, count});
  if (!made.ok())
    return made;
  auto result = made.take_value();
  // The object has not escaped this constructor; publish only after freezing.
  std::const_pointer_cast<Impl>(result.impl_)->kind = Impl::Kind::Identity;
  return Result<ResultRelation>(std::move(result));
}
Result<ResultRelation> ResultRelation::unknown(ResourceBudget budget,
                                               std::uint64_t outputs) {
  auto made = cartesian(std::move(budget), outputs, {0, 1, 0, 0},
                        DependencyGuarantee::Unknown);
  if (!made.ok())
    return made;
  auto result = made.take_value();
  std::const_pointer_cast<Impl>(result.impl_)->kind = Impl::Kind::Unknown;
  return Result<ResultRelation>(std::move(result));
}
Result<ResultRelation> ResultRelation::rows(
    ResourceBudget budget, std::uint64_t outputs, std::uint64_t count,
    const std::function<Result<ResultRelationRow>(std::uint64_t)>& reader,
    DependencyGuarantee guarantee) {
  if (!reader || !valid_guarantee(guarantee) ||
      count > UINT64_MAX / sizeof(ResultRelationRow))
    return Result<ResultRelation>(invalid_relation());
  auto made = Impl::make(budget, outputs);
  if (!made.ok())
    return Result<ResultRelation>(made.status());
  auto impl = made.take_value();
  auto file = TemporaryStorage::create(budget);
  if (!file.ok())
    return Result<ResultRelation>(file.status());
  impl->rows = file.take_value();
  auto allocated = impl->rows.append_zeroed(count * sizeof(ResultRelationRow));
  if (!allocated.ok())
    return Result<ResultRelation>(allocated.status());
  for (std::uint64_t i = 0; i < count; ++i) {
    auto status = budget.consume({1});
    if (!status.ok())
      return Result<ResultRelation>(status);
    auto row = reader(i);
    if (!row.ok())
      return Result<ResultRelation>(row.status());
    if (row.value().output >= outputs || !valid_support(row.value().support))
      return Result<ResultRelation>(invalid_relation());
    status = impl->rows.write(
        i * sizeof(ResultRelationRow),
        ByteView(reinterpret_cast<const std::uint8_t*>(&row.value()),
                 sizeof(ResultRelationRow)));
    if (!status.ok())
      return Result<ResultRelation>(status);
  }
  auto sealed = impl->rows.seal();
  if (!sealed.ok())
    return Result<ResultRelation>(sealed);
  impl->kind = Impl::Kind::Rows;
  impl->guarantee = guarantee;
  impl->row_count = count;
  ResultRelation relation;
  relation.impl_ = std::move(impl);
  return Result<ResultRelation>(std::move(relation));
}
Result<ResultRelation> ResultRelation::unite(
    ResourceBudget budget, const std::vector<ResultRelation>& inputs) {
  if (inputs.empty() || inputs.size() > 16 || !inputs[0].owned_by(budget))
    return Result<ResultRelation>(invalid_relation());
  auto made = Impl::make(std::move(budget), inputs[0].coverage());
  if (!made.ok())
    return Result<ResultRelation>(made.status());
  auto impl = made.take_value();
  impl->kind = Impl::Kind::Union;
  impl->guarantee = DependencyGuarantee::Exact;
  for (const auto& input : inputs) {
    if (!input.owned_by(impl->budget) || input.coverage() != impl->outputs ||
        input.impl_->depth >= 32)
      return Result<ResultRelation>(invalid_relation());
    impl->children[impl->children_count++] = input.impl_;
    impl->depth = std::max(impl->depth, input.impl_->depth + 1);
    impl->guarantee = combine_guarantee(impl->guarantee, input.guarantee());
  }
  ResultRelation relation;
  relation.impl_ = std::move(impl);
  return Result<ResultRelation>(std::move(relation));
}
Result<ResultRelation> ResultRelation::compose(ResourceBudget budget,
                                               ResultRelation first,
                                               ResultRelation second,
                                               std::uint64_t maximum_work) {
  if (!first.owned_by(budget) || !second.owned_by(budget) ||
      first.impl_->depth >= 32 || second.impl_->depth >= 32)
    return Result<ResultRelation>(invalid_relation());
  if (first.guarantee() != DependencyGuarantee::Unknown) {
    for (std::uint64_t row = 0; row < first.coverage(); ++row) {
      auto checked =
          first.impl_->walk(row, &maximum_work, [&](ResultSupport middle) {
            return middle.input == 0 && middle.first <= second.coverage() &&
                           middle.count <= second.coverage() - middle.first
                       ? Status::success()
                       : invalid_relation();
          });
      if (!checked.ok())
        return Result<ResultRelation>(checked);
    }
  }
  auto made = Impl::make(std::move(budget), first.coverage());
  if (!made.ok())
    return Result<ResultRelation>(made.status());
  auto impl = made.take_value();
  impl->kind = Impl::Kind::Compose;
  impl->guarantee = combine_guarantee(first.guarantee(), second.guarantee());
  impl->depth = std::max(first.impl_->depth, second.impl_->depth) + 1;
  impl->children[0] = std::move(first.impl_);
  impl->children[1] = std::move(second.impl_);
  impl->children_count = 2;
  ResultRelation relation;
  relation.impl_ = std::move(impl);
  return Result<ResultRelation>(std::move(relation));
}
Status ResultRelation::visit(
    std::uint64_t output, std::uint64_t maximum_work,
    const std::function<Status(ResultSupport)>& visitor) const {
  if (!impl_ || !visitor || output >= coverage())
    return invalid_relation();
  if (guarantee() == DependencyGuarantee::Unknown)
    return Status{ErrorCode::NotFound, "Unresolved result support"};
  return impl_->walk(output, &maximum_work, visitor);
}
Result<std::optional<bool>> ResultRelation::intersects(
    std::uint64_t output, const std::vector<ResultSupport>& changed,
    std::uint64_t maximum_work) const {
  if (!impl_ || output >= coverage() || changed.size() > 1024 ||
      std::any_of(changed.begin(), changed.end(),
                  [](auto s) { return !valid_support(s); }))
    return Result<std::optional<bool>>(invalid_relation());
  if (guarantee() == DependencyGuarantee::Unknown)
    return Result<std::optional<bool>>(std::optional<bool>{});
  bool dirty = false;
  auto status = impl_->walk(output, &maximum_work, [&](ResultSupport support) {
    for (const auto& edit : changed) {
      if (!maximum_work)
        return Status{ErrorCode::ResourceExhausted,
                      "result relation work exhausted"};
      auto charged = impl_->budget.consume({1});
      if (!charged.ok())
        return charged;
      --maximum_work;
      dirty |= support.input == edit.input && (support.roles & edit.roles) &&
               support.first < edit.first + edit.count &&
               edit.first < support.first + support.count;
    }
    return Status::success();
  });
  return status.ok() ? Result<std::optional<bool>>(std::optional<bool>{dirty})
                     : Result<std::optional<bool>>(status);
}
}  // namespace ps
