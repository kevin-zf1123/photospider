#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "data/content_digest.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
// Synchronous borrowed proof evaluator. Root, work/cancellation callbacks and
// input capabilities must outlive the call; nothing escapes except the owning
// digest string. Every comparison and payload projection uses optional cache
// work, and failures leave the caller's replay state untouched.
class ResultCacheProof final {
 public:
  ResultCacheProof(const ResourceBudget& root, std::uint64_t maximum_window,
                   const std::function<Status(std::uint64_t)>& work,
                   const std::function<CancellationToken()>& cancellation)
      : root_(root),
        maximum_window_(maximum_window),
        work_(work),
        cancellation_(cancellation) {}
  Result<ResourceString> supplied_facts(const ResultObjectInputs&,
                                        const ResultTensorInputs&);
  Result<ResourceString> source_digest(
      const std::vector<ExecutionBinding>&,
      const ResourceVector<SourceObservation>&);

 private:
  Status hash_facts(content_internal::Sha256&, const ResultRef&,
                    const ResultDescriptor&);
  const ResourceBudget& root_;
  std::uint64_t maximum_window_;
  const std::function<Status(std::uint64_t)>& work_;
  const std::function<CancellationToken()>& cancellation_;
};
}  // namespace ps::execution_internal
