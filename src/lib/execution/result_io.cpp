#include "execution/result_io.hpp"

#include <utility>

#include "execution/result_protocol.hpp"

namespace ps::execution_internal {
Result<ResultIoReply> execute_result_io(
    const ResultIoRequest& request, const ResourceBudget& root,
    std::uint64_t maximum_window,
    const std::function<Status(std::uint64_t)>& consume,
    const std::function<CancellationToken()>& cancellation) {
  using Answer = Result<ResultIoReply>;
  auto charged = consume(1);
  if (!charged.ok())
    return Answer(charged);
  if (const auto* read = std::get_if<ResultReadPlan>(&request)) {
    auto ready = read->load(maximum_window, cancellation());
    return ready.ok() ? Answer(ResultIoReply{ready.take_value()})
                      : Answer(ready.status());
  }
  if (const auto* write = std::get_if<ResultWritePlan>(&request)) {
    auto applied = write->apply(cancellation());
    return applied.ok() ? Answer(ResultIoReply{std::monostate{}})
                        : Answer(applied);
  }
  if (std::holds_alternative<ResultCreateTemporary>(request)) {
    auto file = TemporaryStorage::create(root);
    return file.ok() ? Answer(ResultIoReply{file.take_value()})
                     : Answer(file.status());
  }
  if (const auto* read = std::get_if<ResultReadTemporary>(&request)) {
    auto ready = read->storage.read(read->offset, read->bytes, maximum_window,
                                    cancellation());
    return ready.ok() ? Answer(ResultIoReply{ready.take_value()})
                      : Answer(ready.status());
  }
  if (const auto* write = std::get_if<ResultWriteTemporary>(&request)) {
    if (!write->bytes)
      return Answer(protocol_failure("missing temporary write payload"));
    auto storage = write->storage;
    auto retained = root.reference(write->bytes);
    if (!retained.ok())
      return Answer(retained.status());
    auto applied =
        storage.write(write->offset, retained.value()->bytes(), cancellation());
    return applied.ok() ? Answer(ResultIoReply{std::monostate{}})
                        : Answer(applied);
  }
  const auto& extend = std::get<ResultExtendTemporary>(request);
  auto storage = extend.storage;
  auto applied = storage.append_zeroed(extend.bytes, cancellation());
  return applied.ok() ? Answer(ResultIoReply{applied.value()})
                      : Answer(applied.status());
}
}  // namespace ps::execution_internal
