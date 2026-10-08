#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "photospider/data/value.hpp"
namespace ps::execution_internal {
// Pure physical view identity excludes Result semantic facts. Inputs and stop
// callbacks are synchronous borrows; returned Values retain backing storage.
Result<std::uint64_t> region_bytes(const ValueDescriptor&, const Region&);
Result<Value> transfer_value(const Value&, const BufferAllocator&, bool compact,
                             const std::function<ErrorCode()>& stop);
std::string upload_view_key(const Value&);
Result<std::string> upload_content_key(const Value&, const std::string&,
                                       const std::function<ErrorCode()>&);
Result<Value> uploaded_view(const Value&, std::shared_ptr<const CpuStorage>);
}  // namespace ps::execution_internal
