#include "plugin/operation_runtime_router.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ps::plugin_host {
namespace {

/** @brief Ordered key form for one operation-ABI runtime identity. */
using RuntimeKey = std::array<std::uint64_t, 2U>;

/** @brief Immutable copied route state retained independently by each call. */
struct RuntimeRoute final {
  /** @brief Signed supervised executor shared with in-flight calls. */
  std::shared_ptr<execution::PluginInvocationExecutor> executor;
  /** @brief Fresh caller/worker/invocation identity source. */
  SupervisedOperationIdentityFactory identity_factory;
};

/** @brief Protects the process-local package-to-route snapshot. */
std::mutex runtime_routes_mutex;
/** @brief Visible routes keyed only by copied 128-bit package identity. */
std::map<RuntimeKey, RuntimeRoute> runtime_routes;

/**
 * @brief Converts one ABI identity into the ordered route key.
 * @param identity Copied opaque words.
 * @return Exact two-word key.
 * @throws Nothing.
 */
RuntimeKey runtime_key(ps_operation_identity_v1 identity) noexcept {
  return RuntimeKey{identity.word0, identity.word1};
}

/**
 * @brief Converts one ABI identity into canonical package-id bytes.
 * @param identity Nonzero copied opaque words.
 * @return Sixteen big-endian bytes used by signed trust manifests.
 * @throws Nothing.
 */
PluginPackageId package_id(ps_operation_identity_v1 identity) noexcept {
  PluginPackageId result{};
  const std::array<std::uint64_t, 2U> words{identity.word0, identity.word1};
  for (std::size_t word = 0U; word < words.size(); ++word) {
    for (std::size_t byte = 0U; byte < 8U; ++byte) {
      result[word * 8U + byte] = static_cast<std::uint8_t>(
          (words[word] >> ((7U - byte) * 8U)) & 0xffU);
    }
  }
  return result;
}

/**
 * @brief Copies signed package facts into one wire invocation identity.
 * @param package Valid signed package identity.
 * @param identity Nonnull destination identity tuple.
 * @return Nothing after replacing package bytes and generation.
 * @throws std::invalid_argument for a null destination.
 */
void bind_signed_package(const PluginPackageIdentity& package,
                         execution::IsolatedCpuInvocationIdentity* identity) {
  if (identity == nullptr) {
    throw std::invalid_argument(
        "supervised operation invocation identity is null");
  }
  for (std::size_t index = 0U; index < package.package_id.size(); ++index) {
    identity->plugin_package_id.bytes[index] =
        static_cast<std::byte>(package.package_id[index]);
  }
  identity->plugin_generation = package.generation;
}

}  // namespace

/** @copydoc install_supervised_operation_runtime_route */
void install_supervised_operation_runtime_route(
    ps_operation_identity_v1 runtime_package_identity,
    std::shared_ptr<execution::PluginInvocationExecutor> executor,
    SupervisedOperationIdentityFactory identity_factory) {
  if ((runtime_package_identity.word0 == 0U &&
       runtime_package_identity.word1 == 0U) ||
      !executor || !identity_factory) {
    throw std::invalid_argument(
        "supervised operation runtime route is incomplete");
  }
  if (executor->package_identity().package_id !=
      package_id(runtime_package_identity)) {
    throw std::invalid_argument(
        "supervised operation runtime route package identity mismatches trust");
  }
  std::lock_guard<std::mutex> lock(runtime_routes_mutex);
  runtime_routes.insert_or_assign(
      runtime_key(runtime_package_identity),
      RuntimeRoute{std::move(executor), std::move(identity_factory)});
}

/** @copydoc remove_supervised_operation_runtime_route */
bool remove_supervised_operation_runtime_route(
    ps_operation_identity_v1 runtime_package_identity) noexcept {
  try {
    std::lock_guard<std::mutex> lock(runtime_routes_mutex);
    return runtime_routes.erase(runtime_key(runtime_package_identity)) != 0U;
  } catch (...) {
    return false;
  }
}

/** @copydoc invoke_supervised_operation_runtime */
execution::IsolatedCpuHostInvocationResult invoke_supervised_operation_runtime(
    ps_operation_identity_v1 runtime_package_identity,
    execution::IsolatedCpuHostInvocation invocation) {
  RuntimeRoute route;
  {
    std::lock_guard<std::mutex> lock(runtime_routes_mutex);
    const auto found =
        runtime_routes.find(runtime_key(runtime_package_identity));
    if (found == runtime_routes.end()) {
      throw std::invalid_argument(
          "supervised operation runtime package route is unavailable");
    }
    route = found->second;
  }
  invocation.identity = route.identity_factory();
  const PluginPackageIdentity signed_package =
      route.executor->package_identity();
  if (signed_package.package_id != package_id(runtime_package_identity) ||
      signed_package.generation == 0U) {
    throw std::invalid_argument(
        "supervised operation runtime signed package identity is stale");
  }
  bind_signed_package(signed_package, &invocation.identity);
  return route.executor->invoke(invocation);
}

}  // namespace ps::plugin_host
