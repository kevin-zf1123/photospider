#include "common/domain/exact_numeric.hpp"

namespace ps::data_internal::format_numeric {
thread_local const core_internal::WorkConsumer* exact_work_charge = nullptr;
thread_local const CancellationToken* exact_work_cancellation = nullptr;
}  // namespace ps::data_internal::format_numeric
