#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "photospider/compiler/ir.hpp"

namespace ps {
/** @brief Named trusted frontend over WorkflowDocument source.
 * Lowering reads immutable source and a frozen operation registry, and returns
 * an independent IRProgram. Compiler::analyze binds and validates that program
 * before publication, then attaches snapshot lifetime and registered
 * provenance. Callback/binding failures publish no IR. Binding errors retain
 * their original status, since the source is unvalidated; thrown exceptions
 * become Internal, and allocation failures become ResourceExhausted. Callbacks
 * must be safe for concurrent invocations, preserve source semantics, retain no
 * borrowed arguments, and invoke no runtime operation callbacks. Other source
 * representations require a separate typed input contract.
 */
struct PHOTOSPIDER_API FrontendDefinition final {
  std::string name, version, description;
  std::function<Result<IRProgram>(const WorkflowDocument&,
                                  const OperationRegistry&)>
      lower;
};

/** @brief Registration-order catalog, immutable after freeze.
 * Register and freeze on one thread before sharing. Frozen reads are
 * concurrent; returned definition pointers are valid until mutation or
 * destruction.
 */
class PHOTOSPIDER_API FrontendRegistry final {
 public:
  /** @brief Stable catalog identity; share ownership instead of copying or
   * moving a registry, so published definitions remain valid after freeze.
   */
  FrontendRegistry() = default;
  FrontendRegistry(const FrontendRegistry&) = delete;
  FrontendRegistry& operator=(const FrontendRegistry&) = delete;
  FrontendRegistry(FrontendRegistry&&) = delete;
  FrontendRegistry& operator=(FrontendRegistry&&) = delete;

  /** @brief Register a unique frontend before freeze.
   * InvalidArgument rejects an empty/oversized name or version, absent
   * callback, duplicate name, or frozen registry. Names are limited to 1024
   * bytes and versions to 128 bytes. Allocation failures may throw.
   */
  Status add(FrontendDefinition definition);
  void freeze() noexcept { frozen_ = true; }
  bool frozen() const noexcept { return frozen_; }
  const std::vector<FrontendDefinition>& definitions() const noexcept {
    return definitions_;
  }
  const FrontendDefinition* find(const std::string& name) const noexcept;

 private:
  bool frozen_ = false;
  std::vector<FrontendDefinition> definitions_;
};

/** @brief Shared frozen catalog containing dag@1.
 * Initialization is thread-safe and may throw on allocation failure.
 */
PHOTOSPIDER_API std::shared_ptr<const FrontendRegistry>
make_default_frontend_registry();
}  // namespace ps
