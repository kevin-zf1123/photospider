#pragma once

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "execution/memory_budget.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
/** @brief Disposable finite Float32 regions with exclusive directory ownership.
 * @note The sole writer is asynchronous; only explicit flush/destruction waits.
 * All retained Values keep their original controlled allocation leases.
 */
class DiskCache final {
 public:
  DiskCache(DiskCacheConfig config, std::string implementation,
            std::shared_ptr<MemoryBudget> budget, std::uint64_t queue_bytes)
      : config_(std::move(config)),
        implementation_(std::move(implementation)),
        budget_(std::move(budget)),
        queue_limit_(queue_bytes) {
    if (config_.directory.empty() || config_.maximum_bytes == 0 ||
        config_.maximum_entries == 0 || config_.maximum_entries > 1000000 ||
        config_.maximum_queued_writes == 0 ||
        config_.maximum_queued_writes > 1024)
      throw std::invalid_argument("invalid disk cache bounds");
    std::filesystem::create_directories(config_.directory);
    root_ = std::filesystem::canonical(config_.directory);
    try {
#if defined(_WIN32)
      lock_ =
          CreateFileW((root_ / ".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                      0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (lock_ == INVALID_HANDLE_VALUE)
        throw std::invalid_argument("disk cache directory already owned");
#else
      lock_ = ::open((root_ / ".lock").c_str(), O_CREAT | O_RDWR, 0600);
      if (lock_ < 0 || ::flock(lock_, LOCK_EX | LOCK_NB) != 0)
        throw std::invalid_argument("disk cache directory already owned");
#endif
      for (const auto& file : std::filesystem::directory_iterator(root_)) {
        if (!std::filesystem::is_regular_file(file.symlink_status()))
          continue;
        const auto name = file.path().filename().string();
        if (name.size() == 68 && valid_key(name.substr(0, 64)) &&
            name.substr(64) == ".tmp") {
          std::filesystem::remove(file.path());
          continue;
        }
        if (name.size() != 72 || !valid_key(name.substr(0, 64)) ||
            name.substr(64) != ".pscache")
          continue;
        const auto size = file.file_size();
        if (size > config_.maximum_bytes) {
          std::filesystem::remove(file.path());
          continue;
        }
        while (!entries_.empty() &&
               (entries_.size() >= config_.maximum_entries ||
                bytes_ > config_.maximum_bytes - size))
          evict_locked();
        entries_[name.substr(0, 64)] = {size, ++clock_};
        bytes_ += size;
      }
      writer_ = std::thread([this] { worker(); });
    } catch (...) {
      unlock_directory();
      throw;
    }
  }
  ~DiskCache() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
    }
    changed_.notify_all();
    if (writer_.joinable())
      writer_.join();
    unlock_directory();
  }
  DiskCacheStatistics statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = stats_;
    result.retained_bytes = bytes_ + reserved_;
    result.entries = entries_.size();
    result.queued_writes = queue_.size() + (active_ ? 1 : 0);
    return result;
  }
  void flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return queue_.empty() && !active_; });
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    queue_.clear();
    queued_keys_.clear();
    queued_bytes_ = 0;
    while (!entries_.empty())
      evict_locked();
    changed_.notify_all();
  }
  void drop_pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    queued_keys_.clear();
    queued_bytes_ = 0;
    changed_.notify_all();
  }
  std::string disk_key(const std::string& key) const {
    if (key.empty() || implementation_.empty())
      return {};
    content_internal::Sha256 hash;
    hash.text("photospider.disk-result.v1");
    hash.text(implementation_);
    hash.text(key);
    return hash.finish();
  }
  /** @brief Reads only an exact expected representation; malformed data misses.
   * @note File lengths never select allocation sizes; expected plan metadata
   * does.
   */
  Value get(const std::string& logical, const ValueDescriptor& descriptor,
            const Region& region, OperationPortKind kind) noexcept {
    try {
      if (!supported(descriptor, kind))
        return {};
      const auto key = disk_key(logical);
      if (key.empty())
        return {};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!entries_.count(key)) {
          ++stats_.misses;
          return {};
        }
      }
      const auto path = root_ / (key + ".pscache");
      const auto prefix = header(key, descriptor, region, kind);
      const auto count = byte_count(region);
      if (!std::filesystem::is_regular_file(
              std::filesystem::symlink_status(path)) ||
          count > config_.maximum_bytes ||
          std::filesystem::file_size(path) != prefix.size() + 64 + count) {
        invalidate(key);
        return {};
      }
      std::ifstream file(path, std::ios::binary);
      std::string actual(prefix.size(), '\0'), checksum(64, '\0');
      file.read(actual.data(), static_cast<std::streamsize>(actual.size()));
      file.read(checksum.data(), 64);
      if (!file || actual != prefix || !valid_key(checksum)) {
        invalidate(key);
        return {};
      }
      auto reserved = budget_->reserve(count);
      if (!reserved.ok())
        return {};
      auto reservation = reserved.take_value();
      auto made =
          MutableValue::allocate(descriptor, region, reservation->allocator());
      if (!made.ok())
        return {};
      auto value = made.take_value();
      content_internal::Sha256 hash;
      hash.bytes(prefix.data(), prefix.size());
      std::array<std::uint8_t, 4096> buffer{};
      std::uint64_t offset = 0;
      while (offset < count) {
        const auto n = std::min<std::uint64_t>(buffer.size(), count - offset);
        file.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(n));
        if (!file) {
          invalidate(key);
          return {};
        }
        hash.bytes(buffer.data(), n);
        for (std::uint64_t i = 0; i < n; i += 4) {
          std::uint32_t bits = 0;
          for (unsigned b = 0; b < 4; ++b)
            bits |= static_cast<std::uint32_t>(buffer[i + b]) << (b * 8);
          std::memcpy(value.data() + offset + i, &bits, 4);
        }
        offset += n;
      }
      if (hash.finish() != checksum) {
        invalidate(key);
        return {};
      }
      auto result = std::move(value).publish(
          kind == OperationPortKind::Float32Mask
              ? std::vector<ValueFacet>{}
              : std::vector<ValueFacet>{input_internal::image_facet()});
      if (!result.ok()) {
        invalidate(key);
        return {};
      }
      auto published = result.take_value();
      auto valid = input_internal::validate_port_value(
          {kind, 0, 0}, published, ErrorCode::InvalidArgument, {});
      if (!valid.ok()) {
        invalidate(key);
        return {};
      }
      reservation->seal();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.hits;
        auto found = entries_.find(key);
        if (found != entries_.end())
          found->second.age = ++clock_;
      }
      return published;
    } catch (...) {
      return {};
    }
  }
  /** @brief Optional queue admission never throws or waits for file writes. */
  void put(const std::string& logical, const Value& value,
           OperationPortKind kind) noexcept {
    try {
      if (!supported(value.descriptor(), kind))
        return;
      auto key = disk_key(logical);
      if (key.empty())
        return;
      const auto capacity = value.storage()->capacity();
      std::lock_guard<std::mutex> lock(mutex_);
      if (closing_ || entries_.count(key) || queued_keys_.count(key))
        return;
      if (queue_.size() >= config_.maximum_queued_writes ||
          capacity > queue_limit_ || queued_bytes_ > queue_limit_ - capacity) {
        ++stats_.dropped_writes;
        return;
      }
      queued_keys_.insert(key);
      try {
        queue_.push_back({key, value, kind, epoch_});
      } catch (...) {
        queued_keys_.erase(key);
        throw;
      }
      queued_bytes_ += capacity;
      changed_.notify_all();
    } catch (...) {
    }
  }

 private:
  struct Entry {
    std::uint64_t bytes = 0, age = 0;
  };
  struct Pending {
    std::string key;
    Value value;
    OperationPortKind kind;
    std::uint64_t epoch;
  };
  static bool valid_key(const std::string& key) {
    return key.size() == 64 && std::all_of(key.begin(), key.end(), [](char c) {
             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
  }
  static bool supported(const ValueDescriptor& descriptor,
                        OperationPortKind kind) {
    return descriptor.element_type == ElementType::Float32 &&
           ((kind == OperationPortKind::Float32Mask &&
             descriptor.shape.size() == 2) ||
            (kind == OperationPortKind::LinearPremultipliedRgbaFloat32 &&
             descriptor.shape.size() == 3 && descriptor.shape[2] == 4));
  }
  static std::uint64_t byte_count(const Region& region) {
    std::uint64_t bytes = 4;
    for (auto d : region.dimensions()) {
      if (d.extent == 0 || d.extent > UINT64_MAX / bytes)
        throw std::overflow_error("disk region size overflow");
      bytes *= d.extent;
    }
    return bytes;
  }
  static std::string header(const std::string& key,
                            const ValueDescriptor& descriptor,
                            const Region& region, OperationPortKind kind) {
    std::string result = "PSCACHE1";
    const auto integer = [&](std::uint64_t value) {
      for (unsigned i = 0; i < 8; ++i)
        result.push_back(static_cast<char>(value >> (8 * i)));
    };
    integer(1);
    integer(static_cast<std::uint32_t>(kind));
    integer(descriptor.shape.size());
    for (auto n : descriptor.shape)
      integer(n);
    for (auto d : region.dimensions()) {
      integer(d.offset);
      integer(d.extent);
    }
    integer(byte_count(region));
    result += key;
    return result;
  }
  /** @brief Converts packed host Float32 samples to canonical little endian. */
  template <class Sink>
  static void payload(const Value& value, Sink sink) {
    std::array<std::uint8_t, 4096> buffer{};
    std::size_t used = 0;
    const auto& r = value.region().dimensions();
    const std::uint64_t channels = r.size() == 3 ? 4 : 1;
    for (std::uint64_t y = r[0].offset; y < r[0].offset + r[0].extent; ++y)
      for (std::uint64_t x = r[1].offset; x < r[1].offset + r[1].extent; ++x)
        for (std::uint64_t c = 0; c < channels; ++c) {
          std::vector<std::uint64_t> coord{y, x};
          if (channels == 4)
            coord.push_back(c);
          std::uint32_t bits;
          std::memcpy(&bits,
                      value.bytes().data() + value.byte_address(coord).value(),
                      4);
          for (unsigned i = 0; i < 4; ++i)
            buffer[used++] = static_cast<std::uint8_t>(bits >> (8 * i));
          if (used == buffer.size()) {
            sink(buffer.data(), used);
            used = 0;
          }
        }
    if (used)
      sink(buffer.data(), used);
  }
  void invalidate(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.invalid_entries;
    std::error_code ec;
    std::filesystem::remove(root_ / (key + ".pscache"), ec);
    auto found = entries_.find(key);
    if (found != entries_.end()) {
      bytes_ -= found->second.bytes;
      entries_.erase(found);
    }
  }
  void evict_locked() {
    auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                   [](const auto& a, const auto& b) {
                                     return a.second.age < b.second.age;
                                   });
    std::error_code ec;
    std::filesystem::remove(root_ / (oldest->first + ".pscache"), ec);
    if (ec)
      throw std::runtime_error("cannot evict disk entry");
    bytes_ -= oldest->second.bytes;
    entries_.erase(oldest);
  }
  void write(const Pending& pending) {
    const auto prefix = header(pending.key, pending.value.descriptor(),
                               pending.value.region(), pending.kind);
    const auto bytes = byte_count(pending.value.region());
    if (bytes > config_.maximum_bytes ||
        prefix.size() + 64 > config_.maximum_bytes - bytes)
      return;
    const auto total = bytes + prefix.size() + 64;
    const auto temporary = root_ / (pending.key + ".tmp");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending.epoch != epoch_)
        return;
      while (!entries_.empty() && (entries_.size() >= config_.maximum_entries ||
                                   bytes_ > config_.maximum_bytes - total))
        evict_locked();
      reserved_ = total;
    }
    content_internal::Sha256 hash;
    hash.bytes(prefix.data(), prefix.size());
    payload(pending.value,
            [&](const auto* data, std::size_t n) { hash.bytes(data, n); });
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    const auto checksum = hash.finish();
    file.write(checksum.data(), 64);
    payload(pending.value, [&](const auto* data, std::size_t n) {
      file.write(reinterpret_cast<const char*>(data),
                 static_cast<std::streamsize>(n));
    });
    file.close();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file || pending.epoch != epoch_) {
      std::filesystem::remove(temporary);
      if (!file)
        ++stats_.write_failures;
      return;
    }
    std::filesystem::rename(temporary, root_ / (pending.key + ".pscache"));
    auto inserted = entries_.emplace(pending.key, Entry{total, ++clock_});
    if (inserted.second)
      bytes_ += total;
    reserved_ = 0;
  }
  void worker() noexcept {
    for (;;) {
      std::optional<Pending> pending;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closing_ || !queue_.empty(); });
        if (queue_.empty())
          return;
        pending = std::move(queue_.front());
        queued_bytes_ -= pending->value.storage()->capacity();
        queue_.pop_front();
        active_ = true;
      }
      try {
        write(*pending);
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.write_failures;
        std::error_code ec;
        std::filesystem::remove(root_ / (pending->key + ".tmp"), ec);
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        queued_keys_.erase(pending->key);
        pending.reset();
        active_ = false;
        reserved_ = 0;
      }
      changed_.notify_all();
    }
  }
  void unlock_directory() noexcept {
#if defined(_WIN32)
    if (lock_ != INVALID_HANDLE_VALUE) {
      CloseHandle(lock_);
      lock_ = INVALID_HANDLE_VALUE;
    }
#else
    if (lock_ >= 0) {
      ::close(lock_);
      lock_ = -1;
    }
#endif
  }
  DiskCacheConfig config_;
  std::string implementation_;
  std::shared_ptr<MemoryBudget> budget_;
  std::uint64_t queue_limit_;
  std::filesystem::path root_;
#if defined(_WIN32)
  HANDLE lock_ = INVALID_HANDLE_VALUE;
#else
  int lock_ = -1;
#endif
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::thread writer_;
  bool closing_ = false, active_ = false;
  std::uint64_t bytes_ = 0, reserved_ = 0, queued_bytes_ = 0, clock_ = 0,
                epoch_ = 0;
  std::map<std::string, Entry> entries_;
  std::deque<Pending> queue_;
  std::set<std::string> queued_keys_;
  DiskCacheStatistics stats_;
};
}  // namespace ps::execution_internal
