#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/data/tensor_span.hpp"
#include "support/test_support.hpp"

namespace {
struct Lease {
  std::shared_ptr<std::uint64_t> live;
  std::uint64_t bytes;
  Lease(std::shared_ptr<std::uint64_t> count, std::uint64_t size)
      : live(std::move(count)), bytes(size) {
    *live += bytes;
  }
  ~Lease() { *live -= bytes; }
};
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  {
    unsigned parent_failures = 0, child_failures = 0;
    Status parent_failure, child_failure;
    auto parent =
        BufferAllocator().limited_requested(8, [&](const Status& status) {
          parent_failure = status;
          ++parent_failures;
        });
    auto child = parent.limited(6, [&](const Status& status) {
      child_failure = status;
      ++child_failures;
    });
    auto first = child.allocate(6).take_value();
    PS_CHECK(parent.owns_allocation(first) && child.owns_allocation(first));
    const auto rejected = child.allocate(1);
    PS_CHECK(!rejected.ok());
    PS_CHECK(parent_failures == 1 && child_failures == 1);
    for (const auto& failure : {parent_failure, child_failure}) {
      PS_CHECK(failure.code == rejected.status().code &&
               failure.reason == FailureReason::CapacityLimit &&
               failure.reason == rejected.status().reason &&
               failure.message == rejected.status().message &&
               !failure.message.empty());
    }
    auto second = parent.allocate(2).take_value();
    PS_CHECK(!parent.allocate(1).ok());
    auto retained = std::move(first).freeze();
    auto alias = retained;
    retained.reset();
    PS_CHECK(!child.allocate(1).ok());
    alias.reset();
    PS_CHECK(child.allocate(6).ok());
    PS_CHECK(!parent.allocate(UINT64_MAX).ok());
    second = MutableBuffer();
    PS_CHECK(parent.allocate(8).ok());
    PS_CHECK(!parent.limited_requested(0).allocate(1).ok());
    auto narrow = parent.limited_requested(3);
    auto copied = narrow;
    auto shared = copied.allocate(3).take_value();
    PS_CHECK(!narrow.allocate(1).ok());
    PS_CHECK(parent.owns_allocation(shared));
    PS_CHECK(!child.owns_allocation(shared));
  }
  auto live = std::make_shared<std::uint64_t>(0);
  TensorSpan retained;
  {
    BufferAllocator allocator([live](std::uint64_t bytes) {
      if (bytes > 32 - *live)
        return Result<std::shared_ptr<void>>(
            Status::failure(ErrorCode::ResourceExhausted, "test budget"));
      return Result<std::shared_ptr<void>>(
          std::make_shared<Lease>(live, bytes));
    });
    const Region roi({{7, 2}, {11, 3}});
    auto made = TensorWriter::allocate({ElementType::UInt8, {1000, 1000}}, roi,
                                       allocator);
    PS_CHECK(made.ok());
    auto writable = made.take_value();
    PS_CHECK(writable.size() == 6 && *live == 6);
    for (std::uint8_t i = 0; i < 6; ++i)
      writable.data()[i] = i + 1;
    auto published = std::move(writable).publish();
    PS_CHECK(published.ok() && writable.data() == nullptr);
    retained = published.value();
    PS_CHECK(retained.storage()->capacity() == 6);
    PS_CHECK(retained.byte_address({8, 13}).value() == 5);
    PS_CHECK(!retained.byte_address({6, 11}).ok());
    auto sub = retained.view(Region({{8, 1}, {12, 2}}));
    PS_CHECK(sub.ok() && sub.value().storage() == retained.storage());
    PS_CHECK(sub.value().bytes()[sub.value().byte_address({8, 12}).value()] ==
             5);
    PS_CHECK(!retained.view(Region({{8, 2}, {11, 1}})).ok());
    auto reverse = TensorSpan::from_storage(retained.descriptor(), roi,
                                            StridedLayout{5, {-3, -1}, {7, 11}},
                                            retained.storage());
    PS_CHECK(reverse.ok());
    PS_CHECK(reverse.value().byte_address({8, 13}).value() == 0);
    PS_CHECK(!TensorSpan::from_storage(retained.descriptor(), roi,
                                       StridedLayout{0, {-3, -1}, {7, 11}},
                                       retained.storage())
                  .ok());
    auto full = allocator.allocate(26);
    PS_CHECK(full.ok() && *live == 32);
    PS_CHECK(!allocator.allocate(1).ok());
    PS_CHECK(!allocator.allocate(UINT64_MAX).ok());
  }
  PS_CHECK(*live == 6 && retained.bytes()[5] == 6);
  TensorSpan copy = retained;
  retained = TensorSpan();
  PS_CHECK(*live == 6);
  copy = TensorSpan();
  PS_CHECK(*live == 0);
  return 0;
}
