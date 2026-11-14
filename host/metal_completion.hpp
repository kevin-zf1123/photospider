#pragma once
#import <Metal/Metal.h>

#include <functional>
#include <memory>
namespace ps::host_internal {
/** @brief Metal owns the wrapper block. Its capture is destroyed in the host
 * after the provider callback and all of its captures have been released. */
void on_metal_completion(id<MTLCommandBuffer> command,
                         std::shared_ptr<const void> lease,
                         std::function<void(id<MTLCommandBuffer>)> callback);
id<MTLBuffer> shared_metal_pages(id<MTLDevice> device, void* pages,
                                 NSUInteger capacity);
}  // namespace ps::host_internal
