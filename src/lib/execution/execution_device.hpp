#pragma once
#include <memory>

#include "execution/native_gpu.hpp"
namespace ps::execution_internal {
class ThreadPool;
bool gpu_lane_available(ThreadPool*,
                        const std::shared_ptr<gpu_internal::Device>&);
std::shared_ptr<gpu_internal::Device> execution_device(
    bool, const std::shared_ptr<ResourceBudget>&);
}  // namespace ps::execution_internal
