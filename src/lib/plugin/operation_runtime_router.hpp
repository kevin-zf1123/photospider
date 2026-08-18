#pragma once

#include <functional>
#include <memory>

#include "execution/device/plugin_runtime_supervisor.hpp"  // NOLINT(build/include_subdir)
#include "photospider/plugin/operation_plugin_api.h"

namespace ps::plugin_host {

/**
 * @brief Mints caller/worker/invocation comparison identity for one fresh
 * supervised operation call.
 * @return Complete identity; runtime package bytes/generation are replaced by
 * the router's signed executor facts before protocol validation.
 * @throws Any Host identity-source failure unchanged.
 * @note The factory must mint a fresh invocation id and current tenant, Job,
 * attempt, worker, and worker-lease facts. It returns no process authority.
 */
using SupervisedOperationIdentityFactory =
    std::function<execution::IsolatedCpuInvocationIdentity()>;

/**
 * @brief Installs or atomically replaces one signed supervised runtime route.
 * @param runtime_package_identity Nonzero operation-ABI runtime package key.
 * @param executor Nonnull already trust-authorized supervised executor.
 * @param identity_factory Nonempty fresh invocation identity factory.
 * @return Nothing after the new immutable route becomes visible.
 * @throws std::invalid_argument when the key, executor, factory, or signed
 * package identity is missing or inconsistent.
 * @throws std::bad_alloc when process route storage cannot allocate.
 * @note The route stores no path, descriptor, mapping, PID, DSO context, or
 * callback pointer in wire data. Replacement does not disturb in-flight calls
 * that already copied the previous shared executor/factory.
 */
void install_supervised_operation_runtime_route(
    ps_operation_identity_v1 runtime_package_identity,
    std::shared_ptr<execution::PluginInvocationExecutor> executor,
    SupervisedOperationIdentityFactory identity_factory);

/**
 * @brief Removes one supervised runtime route from future visibility.
 * @param runtime_package_identity Exact nonzero operation-ABI package key.
 * @return True when one visible route was removed.
 * @throws Nothing.
 * @note In-flight calls retain their copied shared executor. This function is
 * a composition/test teardown seam and never terminates an owned child.
 */
bool remove_supervised_operation_runtime_route(
    ps_operation_identity_v1 runtime_package_identity) noexcept;

/**
 * @brief Invokes the exact signed route selected by one operation descriptor.
 * @param runtime_package_identity Copied nonzero operation-ABI package key.
 * @param invocation Complete pointer-free Host request except identity tuple.
 * @return Fully validated supervised result with fresh Host Values on success.
 * @throws std::invalid_argument when no exact route is visible or the identity
 * factory returns package facts inconsistent with the signed executor.
 * @throws All identity-factory and supervised invocation failures unchanged.
 * @note There is no direct/in-process fallback. The signed executor package
 * id/generation overwrite non-authoritative factory package fields before use.
 */
execution::IsolatedCpuHostInvocationResult invoke_supervised_operation_runtime(
    ps_operation_identity_v1 runtime_package_identity,
    execution::IsolatedCpuHostInvocation invocation);

}  // namespace ps::plugin_host
