#pragma once
#include <utility>

#include "photospider/data/tensor.hpp"
namespace ps::data_internal {
class TensorAccess final {
 public:
  static ResourceVector<TensorSpan>& edit(Tensor& tensor) {
    if (!tensor.storage_)
      tensor.storage_ = std::allocate_shared<Tensor::Storage>(
          ResourceAllocator<Tensor::Storage>{});
    else if (tensor.storage_.use_count() != 1)
      tensor.storage_ = std::allocate_shared<Tensor::Storage>(
          ResourceAllocator<Tensor::Storage>{}, *tensor.storage_);
    return tensor.storage_->fragments;
  }
  static void describe(Tensor& tensor, TensorDescriptor descriptor,
                       Footprint coverage) {
    tensor.descriptor_ = std::move(descriptor);
    tensor.authorized_ = std::move(coverage);
  }
  static void commit_coverage(Tensor& tensor, Footprint coverage) {
    tensor.authorized_ = std::move(coverage);
  }
};
}  // namespace ps::data_internal
