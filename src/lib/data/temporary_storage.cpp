#include "photospider/data/temporary_storage.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ps {
namespace {
constexpr std::uint64_t kBlock = 4096;
Status io_failure() {
  return Status::failure(ErrorCode::OperationFailed,
                         "mandatory temporary storage I/O failed");
}
Status stopped() {
  return Status::failure(ErrorCode::Cancelled, "temporary I/O cancelled");
}
Status stale() {
  return Status::failure(ErrorCode::Stale,
                         "temporary storage is invalid or quarantined");
}
bool seek_file(std::FILE* file, std::uint64_t offset) {
#if defined(_WIN32)
  return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
  return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
bool truncate_file(std::FILE* file, std::uint64_t size) {
  std::clearerr(file);
#if defined(_WIN32)
  return _chsize_s(_fileno(file), size) == 0;
#else
  return ftruncate(fileno(file), static_cast<off_t>(size)) == 0;
#endif
}
ResourceCapacity disk_bytes(std::uint64_t bytes) {
  ResourceCapacity c;
  c[ResourceKind::Disk] = bytes;
  return c;
}
}  // namespace
struct TemporaryStorage::Impl {
  explicit Impl(ResourceBudget value) : budget(std::move(value)) {}
  ResourceBudget budget;
  ResourceLease lease;
  std::mutex mutex;
  std::FILE* file = nullptr;
  std::uint64_t end = 0, allocated = 0, frozen = 0;
  bool sealed = false, broken = false;
  ~Impl() {
    if (file) {
      if (std::fclose(file) != 0)
        lease.quarantine();
      else
        lease.settle_quarantine();
    }
  }
  Result<ResourceLease> io(std::uint64_t bytes) {
    ResourceCapacity c;
    c[ResourceKind::IoSlots] = 1;
    auto permit = budget.reserve(c);
    if (!permit.ok())
      return permit;
    auto charge = budget.consume({1, bytes, 1, 0});
    if (!charge.ok())
      return Result<ResourceLease>(charge);
    return permit;
  }
};
Result<TemporaryStorage> TemporaryStorage::create(ResourceBudget budget) {
  try {
    auto capacity = ResourceCapacity::host(sizeof(Impl), sizeof(Impl));
    capacity[ResourceKind::Files] = 1;
    capacity[ResourceKind::Entries] = 1;
    auto lease = budget.reserve(capacity);
    if (!lease.ok())
      return Result<TemporaryStorage>(lease.status());
    auto impl = std::make_shared<Impl>(std::move(budget));
    impl->lease = lease.take_value();
    impl->file = std::tmpfile();
    if (!impl->file || std::setvbuf(impl->file, nullptr, _IONBF, 0) != 0)
      return Result<TemporaryStorage>(io_failure());
    TemporaryStorage result;
    result.impl_ = std::move(impl);
    return Result<TemporaryStorage>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<TemporaryStorage>(Status::failure(
        ErrorCode::ResourceExhausted, "temporary metadata allocation failed"));
  }
}
std::uint64_t TemporaryStorage::size() const {
  if (!impl_)
    return 0;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->end;
}
Result<std::uint64_t> TemporaryStorage::append_zeroed(
    std::uint64_t bytes, const CancellationToken& cancel) {
  if (!impl_)
    return Result<std::uint64_t>(stale());
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->broken || impl_->sealed)
    return Result<std::uint64_t>(stale());
  if (cancel.cancelled())
    return Result<std::uint64_t>(stopped());
  const auto old_end = impl_->end;
  if (!bytes)
    return Result<std::uint64_t>(old_end);
  if (bytes > static_cast<std::uint64_t>(INT64_MAX) - (kBlock - 1) - old_end)
    return Result<std::uint64_t>(Status::failure(
        ErrorCode::ResourceExhausted, "temporary extent is not addressable"));
  const auto end = old_end + bytes;
  const auto allocated = ((end + kBlock - 1) / kBlock) * kBlock;
  const auto growth = allocated - impl_->allocated;
  auto buffer = impl_->budget.allocator().allocate(std::min(kBlock, bytes));
  if (!buffer.ok())
    return Result<std::uint64_t>(buffer.status());
  auto staging = buffer.take_value();
  auto admitted = impl_->lease.grow(disk_bytes(growth));
  if (!admitted.ok())
    return Result<std::uint64_t>(admitted);
  auto fail = [&](Status failure) {
    if (truncate_file(impl_->file, impl_->allocated)) {
      (void)impl_->lease.shrink(disk_bytes(growth));
    } else {
      impl_->broken = true;
      impl_->lease.quarantine();
    }
    return Result<std::uint64_t>(std::move(failure));
  };
  // Reinitialize the previous padding and the newly reserved extents. Appended
  // bytes never expose stale padding left by an earlier rolled-back attempt.
  for (auto position = old_end; position < allocated;) {
    if (cancel.cancelled())
      return fail(stopped());
    const auto count =
        std::min<std::uint64_t>(staging.size(), allocated - position);
    auto permit = impl_->io(count);
    if (!permit.ok())
      return fail(permit.status());
    if (!seek_file(impl_->file, position) ||
        std::fwrite(staging.data(), 1, static_cast<std::size_t>(count),
                    impl_->file) != count)
      return fail(io_failure());
    position += count;
  }
  impl_->allocated = allocated;
  impl_->end = end;
  return Result<std::uint64_t>(old_end);
}
Status TemporaryStorage::write(std::uint64_t offset, ByteView bytes,
                               const CancellationToken& cancel) {
  if (!impl_)
    return stale();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->broken || impl_->sealed)
    return stale();
  if (cancel.cancelled())
    return stopped();
  if (offset < impl_->frozen || offset > impl_->end ||
      bytes.size() > impl_->end - offset)
    return Status::failure(ErrorCode::InvalidArgument,
                           "write outside mutable temporary range");
  if (bytes.empty())
    return Status::success();
  auto permit = impl_->io(bytes.size());
  if (!permit.ok())
    return permit.status();
  if (!seek_file(impl_->file, offset) ||
      std::fwrite(bytes.data(), 1, bytes.size(), impl_->file) != bytes.size()) {
    // A partial mutable write invalidates subsequent production. Previously
    // frozen ranges remain intact and can still be read.
    impl_->sealed = true;
    return io_failure();
  }
  return Status::success();
}
Result<std::shared_ptr<const CpuStorage>> TemporaryStorage::read(
    std::uint64_t offset, std::uint64_t bytes, std::uint64_t maximum_window,
    const CancellationToken& cancel) const {
  using ReadResult = Result<std::shared_ptr<const CpuStorage>>;
  if (!impl_)
    return ReadResult(stale());
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->broken)
    return ReadResult(stale());
  if (cancel.cancelled())
    return ReadResult(stopped());
  if (!bytes || bytes > maximum_window || offset > impl_->end ||
      bytes > impl_->end - offset)
    return ReadResult(Status::failure(ErrorCode::InvalidArgument,
                                      "invalid temporary read window"));
  auto made = impl_->budget.allocator().allocate(bytes);
  if (!made.ok())
    return ReadResult(made.status());
  auto window = made.take_value();
  auto permit = impl_->io(bytes);
  if (!permit.ok())
    return ReadResult(permit.status());
  if (!seek_file(impl_->file, offset) ||
      std::fread(window.data(), 1, static_cast<std::size_t>(bytes),
                 impl_->file) != bytes)
    return ReadResult(io_failure());
  auto storage = std::move(window).freeze();
  struct WindowOwner {
    ResourceLease metadata;
    std::shared_ptr<Impl> backing;
    std::shared_ptr<const CpuStorage> storage;
  };
  auto metadata = impl_->budget.reserve(
      ResourceCapacity::host(sizeof(WindowOwner), sizeof(WindowOwner)));
  if (!metadata.ok())
    return ReadResult(metadata.status());
  try {
    auto owner = std::make_shared<WindowOwner>(
        WindowOwner{metadata.take_value(), impl_, storage});
    return ReadResult(
        std::shared_ptr<const CpuStorage>(std::move(owner), storage.get()));
  } catch (const std::bad_alloc&) {
    return ReadResult(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Status TemporaryStorage::freeze_prefix(std::uint64_t end) {
  if (!impl_)
    return stale();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->broken || end < impl_->frozen || end > impl_->end)
    return Status::failure(ErrorCode::InvalidArgument,
                           "invalid temporary prefix");
  impl_->frozen = end;
  return Status::success();
}
Status TemporaryStorage::seal() {
  if (!impl_)
    return stale();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->broken)
    return stale();
  impl_->frozen = impl_->end;
  impl_->sealed = true;
  return Status::success();
}
}  // namespace ps
