#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/value_fragments.hpp"
#include "photospider/plugin/operation_types.hpp"

namespace ps {
/** @brief Compiler-owned immutable input/output metadata for a staged result.
 * Prepared before execution; it contains no runtime allocation or callback.
 */
struct ResultProgramMetadata final {
  std::vector<OperationMetadata> inputs;
  OperationMetadata output;
};
/** @brief Borrowed static query for one structured-protocol continuation.
 * value_outputs is absent for ResultRef outputs, including empty collections.
 * page_bytes is a physical work-window choice, excluded from semantic_key.
 */
struct ResultProgramQuery final {
  ResultProgramQuery(
      const ResultProgramMetadata& metadata,
      const std::map<std::string, ParameterValue>& static_parameters)
      : inputs(metadata.inputs),
        output(metadata.output),
        parameters(static_parameters) {}
  const std::vector<OperationMetadata>& inputs;
  const OperationMetadata& output;
  const std::map<std::string, ParameterValue>& parameters;
  std::optional<Footprint> value_outputs;
  std::string_view semantic_key;
  std::uint32_t output_index = 0;
  std::uint64_t page_bytes = 4096;
  CancellationToken cancellation;
};
struct ResultValueNeed final {
  std::uint32_t input = 0;
  Footprint samples;
};
/** @brief Requests complete associated data or a monotone minimum field prefix.
 * Complete requests wait for seal. A prefix request returns a sealed shorter
 * collection when discovery finished; the consumer checks actual count.
 */
struct ResultObjectNeed final {
  std::uint32_t input = 0, field = 0;
  bool complete = true;
  std::uint64_t minimum_rows = 0;
};
struct ResultCreateTemporary final {};
struct ResultReadTemporary final {
  TemporaryStorage storage;
  std::uint64_t offset = 0, bytes = 0;
};
struct ResultWriteTemporary final {
  TemporaryStorage storage;
  std::uint64_t offset = 0;
  std::shared_ptr<const CpuStorage> bytes;
};
struct ResultExtendTemporary final {
  TemporaryStorage storage;
  std::uint64_t bytes = 0;
};
/** @brief Closed mandatory I/O actions, executed after a callback yields. */
// NOLINTBEGIN(whitespace/indent_namespace)
using ResultIoRequest =
    std::variant<ResultReadPlan, ResultWritePlan, ResultCreateTemporary,
                 ResultReadTemporary, ResultWriteTemporary,
                 ResultExtendTemporary>;
/** @brief Read owner, new temporary owner, append offset or write success. */
using ResultIoReply =
    std::variant<std::shared_ptr<const CpuStorage>, TemporaryStorage,
                 std::uint64_t, std::monostate>;
// NOLINTEND
/** @brief At most 64 bounded requests per stage; empty Need is invalid. */
struct ResultProgramNeed final {
  ResourceVector<ResultValueNeed> values;
  ResourceVector<ResultObjectNeed> results;
  ResourceVector<ResultIoRequest> io;
};
/** @brief New certified prefix or complete object, published by one producer.
 */
struct ResultPublication final {
  ResultRef result;
  bool complete = false;
};
/** @brief Complete ordinary Value fragments with an explicit support guarantee.
 * The witness is in flattened output sample coordinates. Conservative support
 * never enters the old Exact-only DependencyCertificate path.
 */
struct ResultValuePublication final {
  ValueFragments value;
  ResultRelation relation;
};
// NOLINTBEGIN(whitespace/indent_namespace)
using ResultProgramPoll =
    std::variant<ResultProgramNeed, ResultPublication, ResultValuePublication>;
using ResultValueInputs =
    std::map<std::uint32_t, ValueFragments, std::less<std::uint32_t>,
             ResourceAllocator<std::pair<const std::uint32_t, ValueFragments>>>;
using ResultObjectInputs =
    std::map<std::uint32_t, ResultRef, std::less<std::uint32_t>,
             ResourceAllocator<std::pair<const std::uint32_t, ResultRef>>>;
// NOLINTEND
/** @brief Ready inputs/windows borrowed only until this poll returns.
 * Root resource operations are explicit admission; mandatory I/O is returned as
 * Need actions. Calling TemporaryStorage I/O inside poll is a sticky protocol
 * failure. read() and consume_work() failures cannot be ignored into success.
 */
struct PHOTOSPIDER_API ResultProgramPhase final {
  const ResultProgramQuery& query;
  const ResultValueInputs& values;
  const ResultObjectInputs& results;
  const ResourceVector<ResultIoReply>& io;
  const BufferAllocator& allocator;
  const ResourceBudget& resources;
  std::function<Status(std::uint64_t)> consume_work;
  std::shared_ptr<std::atomic<ErrorCode>> failure;
  Status read(std::uint32_t input, const std::vector<std::uint64_t>& coordinate,
              void* destination, std::size_t bytes) const;
};
/** @brief Move-only host-owned structured continuation; destructor runs once.
 */
class PHOTOSPIDER_API ResultContinuation final {
 public:
  ResultContinuation() = default;
  ~ResultContinuation() noexcept;
  ResultContinuation(ResultContinuation&&) noexcept;
  ResultContinuation& operator=(ResultContinuation&&) noexcept;
  ResultContinuation(const ResultContinuation&) = delete;
  ResultContinuation& operator=(const ResultContinuation&) = delete;
  template <class State, class... Args>
  static Result<ResultContinuation> make(const BufferAllocator& allocator,
                                         Args&&... args) {
    static_assert(alignof(State) <= alignof(std::max_align_t),
                  "overaligned result state");
    static_assert(std::is_nothrow_destructible<State>::value,
                  "result state destructor must not throw");
    auto memory = allocator.allocate(sizeof(State));
    if (!memory.ok())
      return Result<ResultContinuation>(memory.status());
    ResultContinuation continuation;
    continuation.storage_ = memory.take_value();
    new (continuation.storage_.data()) State(std::forward<Args>(args)...);
    continuation.destroy_ = [](void* state) noexcept {
      static_cast<State*>(state)->~State();
    };
    continuation.poll_ = [](void* state, const ResultProgramPhase& phase) {
      return static_cast<State*>(state)->poll(phase);
    };
    return Result<ResultContinuation>(std::move(continuation));
  }
  bool valid() const noexcept { return poll_ != nullptr; }
  /** @brief Executes one finite stage; the host provides the exception fence.
   */
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase);

 private:
  friend class OperationRegistry;
  void reset() noexcept;
  std::shared_ptr<const void> definition_;
  MutableBuffer storage_;
  using Destroy = void (*)(void*) noexcept;  // NOLINT(readability/casting)
  Destroy destroy_ = nullptr;
  Result<ResultProgramPoll> (*poll_)(void*,
                                     const ResultProgramPhase&) = nullptr;
};
using ResultProgramStart = std::function<Result<ResultContinuation>(
    const ResultProgramQuery&, const BufferAllocator&)>;
}  // namespace ps
