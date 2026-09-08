#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/data/value.hpp"
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
  auto live = std::make_shared<std::uint64_t>(0);
  Value retained;
  {
    BufferAllocator allocator([live](std::uint64_t bytes) {
      if (bytes > 32 - *live)
        return Result<std::shared_ptr<void>>(
            Status::failure(ErrorCode::ResourceExhausted, "test budget"));
      return Result<std::shared_ptr<void>>(
          std::make_shared<Lease>(live, bytes));
    });
    const Region roi({{7, 2}, {11, 3}});
    auto made = MutableValue::allocate({ElementType::UInt8, {1000, 1000}}, roi,
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
    auto reverse = Value::from_storage(retained.descriptor(), roi,
                                       StridedLayout{5, {-3, -1}, {7, 11}},
                                       retained.storage());
    PS_CHECK(reverse.ok());
    PS_CHECK(reverse.value().byte_address({8, 13}).value() == 0);
    PS_CHECK(!Value::from_storage(retained.descriptor(), roi,
                                  StridedLayout{0, {-3, -1}, {7, 11}},
                                  retained.storage())
                  .ok());
    auto full = allocator.allocate(26);
    PS_CHECK(full.ok() && *live == 32);
    PS_CHECK(!allocator.allocate(1).ok());
    PS_CHECK(!allocator.allocate(UINT64_MAX).ok());
  }
  PS_CHECK(*live == 6 && retained.bytes()[5] == 6);
  Value copy = retained;
  retained = Value();
  PS_CHECK(*live == 6);
  copy = Value();
  PS_CHECK(*live == 0);
  return 0;
}
