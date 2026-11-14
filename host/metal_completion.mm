#include "host/metal_completion.hpp"

#import <mach/mach.h>
#import <mach/mach_vm.h>

#include <memory>
#include <utility>
namespace ps::host_internal {
namespace {
struct Handler final {
  std::shared_ptr<const void> lease;
  std::function<void(id<MTLCommandBuffer>)> callback;
};
}  // namespace
void on_metal_completion(id<MTLCommandBuffer> command,
                         std::shared_ptr<const void> lease,
                         std::function<void(id<MTLCommandBuffer>)> callback) {
  auto owner =
      std::make_shared<Handler>(Handler{std::move(lease), std::move(callback)});
  [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    owner->callback(completed);
  }];
}
id<MTLBuffer> shared_metal_pages(id<MTLDevice> device, void* pages,
                                 NSUInteger capacity) {
  return [device
      newBufferWithBytesNoCopy:pages
                        length:capacity
                       options:MTLResourceStorageModeShared
                   deallocator:^(void* base, NSUInteger length) {
                     mach_vm_deallocate(
                         mach_task_self(),
                         reinterpret_cast<mach_vm_address_t>(base), length);
                   }];
}
}  // namespace ps::host_internal
