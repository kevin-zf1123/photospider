#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "photospider/data/tensor_span.hpp"
namespace ps::execution_internal {
// Pure physical view identity excludes Result semantic facts. Inputs and stop
// callbacks are synchronous borrows; returned Values retain backing storage.
Result<std::uint64_t> region_bytes(const TensorDescriptor&, const Region&);
Result<TensorSpan> transfer_value(const TensorSpan&, const BufferAllocator&,
                                  bool compact,
                                  const std::function<ErrorCode()>& stop);
std::string upload_view_key(const TensorSpan&);
Result<std::string> upload_content_key(const TensorSpan&, const std::string&,
                                       const std::function<ErrorCode()>&);
Result<TensorSpan> uploaded_view(const TensorSpan&,
                                 std::shared_ptr<const CpuStorage>);
}  // namespace ps::execution_internal
