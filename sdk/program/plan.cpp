#include "photospider/compiler/plan.hpp"

#include <memory>

namespace ps {
const ExecutionPlan::Storage& ExecutionPlan::data() const noexcept {
  static const Storage empty;
  return storage_ ? *storage_ : empty;
}
ExecutionPlan::Storage& ExecutionPlan::edit() {
  if (!storage_)
    storage_ = std::make_shared<Storage>();
  else if (storage_.use_count() != 1)
    storage_ = std::make_shared<Storage>(*storage_);
  return *storage_;
}
}  // namespace ps
