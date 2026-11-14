#include <stdexcept>
#include <utility>

#include "sdk/execution/provider.hpp"
namespace ps {
namespace {
ExecutorRegistry default_registry() {
  ExecutorRegistry r;
  auto s = r.add_physical();
  if (!s.ok())
    throw std::logic_error(s.message);
  r.freeze();
  return r;
}
}  // namespace
Status ExecutorRegistry::add_physical() {
  if (frozen_)
    return {ErrorCode::InvalidArgument, "executor catalog frozen"};
  if (!providers_.count("physical"))
    return add_builtin(executor_sdk::physical_api());
  return {};
}
ExecutionContext::ExecutionContext(std::shared_ptr<OperationRegistry> ops,
                                   ExecutionContextConfig cfg)
    : ExecutionContext(std::move(ops), cfg, default_registry(), "physical") {}
}  // namespace ps
