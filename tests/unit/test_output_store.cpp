#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/output_store.hpp"
#include "ipc/unix_socket.hpp"

namespace ps::ipc::internal {
namespace {

/**
 * @brief Returns a deterministic valid opaque identity for one integer.
 * @param value Low-order numeric value.
 * @return Exactly 32 lowercase hexadecimal characters.
 * @throws std::bad_alloc if result storage cannot be allocated.
 */
std::string opaque_id(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text(32, '0');
  std::size_t index = text.size();
  do {
    text[--index] = kHex[value & 0x0fU];
    value >>= 4U;
  } while (value != 0 && index != 0);
  return text;
}

/**
 * @brief Thread-safe deterministic source of unique opaque identities.
 * @throws Nothing for construction.
 */
class SequentialIds {
 public:
  /**
   * @brief Creates a sequence beginning at the requested value.
   * @param initial First integer encoded by `next()`.
   * @throws Nothing.
   */
  explicit SequentialIds(std::uint64_t initial = 1000) : next_(initial) {}

  /**
   * @brief Returns and consumes one deterministic identity.
   * @return Unique 32-lowercase-hex text.
   * @throws std::bad_alloc if result storage cannot be allocated.
   */
  std::string next() {
    return opaque_id(next_.fetch_add(1, std::memory_order_relaxed));
  }

 private:
  /** @brief Next numeric value consumed atomically. */
  std::atomic<std::uint64_t> next_;
};

/**
 * @brief Manually advanced monotonic clock for deterministic TTL tests.
 * @throws Nothing.
 */
class ManualClock {
 public:
  /**
   * @brief Reads the current injected monotonic time.
   * @return Time point built from the atomic nanosecond counter.
   * @throws Nothing.
   */
  OutputStore::TimePoint now() const noexcept {
    return OutputStore::TimePoint(
        std::chrono::nanoseconds(ticks_.load(std::memory_order_acquire)));
  }

  /**
   * @brief Advances time by one nonnegative duration.
   * @param duration Amount added atomically.
   * @return Nothing.
   * @throws Nothing.
   */
  void advance(std::chrono::steady_clock::duration duration) noexcept {
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    ticks_.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }

 private:
  /** @brief Current monotonic nanosecond count. */
  std::atomic<std::int64_t> ticks_{0};
};

/**
 * @brief Reusable deterministic release gate for concurrent test starts.
 * @throws Nothing.
 */
class StartGate {
 public:
  /**
   * @brief Records one ready participant and waits for the release signal.
   * @return Nothing.
   * @throws Nothing.
   */
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++ready_;
    ready_cv_.notify_all();
    release_cv_.wait(lock, [this] { return released_; });
  }

  /**
   * @brief Waits until the requested participant count is ready.
   * @param count Expected ready callers.
   * @return True before the watchdog deadline, false on a hung test.
   * @throws Nothing.
   */
  bool wait_ready(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return ready_cv_.wait_for(lock, std::chrono::seconds(2),
                              [this, count] { return ready_ >= count; });
  }

  /**
   * @brief Releases every arrived participant.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    release_cv_.notify_all();
  }

 private:
  /** @brief Serializes readiness and release state. */
  std::mutex mutex_;

  /** @brief Wakes the coordinator after participant arrival. */
  std::condition_variable ready_cv_;

  /** @brief Wakes participants after coordinated release. */
  std::condition_variable release_cv_;

  /** @brief Number of waiting participants. */
  std::size_t ready_ = 0;

  /** @brief Whether participants may continue. */
  bool released_ = false;
};

/**
 * @brief Same-owner private temporary directory removed after each test.
 * @throws std::runtime_error when creation fails.
 */
class TemporaryDirectory {
 public:
  /**
   * @brief Creates one mode-0700 directory below the platform temp root.
   * @throws std::runtime_error when `mkdtemp` or chmod fails.
   */
  TemporaryDirectory() {
    std::array<char, 128> pattern{};
    const std::string base = (std::filesystem::temp_directory_path() /
                              "photospider-output-store-XXXXXX")
                                 .string();
    if (base.size() + 1 > pattern.size()) {
      throw std::runtime_error("temporary output-store path is too long");
    }
    std::copy(base.begin(), base.end(), pattern.begin());
    char* created = ::mkdtemp(pattern.data());
    if (created == nullptr || ::chmod(created, 0700) != 0) {
      throw std::runtime_error("cannot create private output-store test root");
    }
    path_ = created;
  }

  /**
   * @brief Removes all test-owned paths recursively.
   * @throws Nothing.
   */
  ~TemporaryDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Prevents two owners from deleting the same directory.
   * @throws Nothing because construction is unavailable.
   */
  TemporaryDirectory(const TemporaryDirectory&) = delete;

  /**
   * @brief Prevents replacing one directory cleanup obligation by assignment.
   * @return No value because copying is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  /**
   * @brief Returns the private root path.
   * @return Borrowed path valid for this object's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Test-owned private root. */
  std::filesystem::path path_;
};

/**
 * @brief Temporarily lowers the process soft descriptor limit for EMFILE tests.
 * @throws Nothing; `active()` reports whether the limit was installed.
 * @note Tests create no other threads while this process-global guard is live.
 */
class ScopedFileDescriptorLimit {
 public:
  /**
   * @brief Attempts to cap the soft descriptor limit.
   * @param maximum Desired cap when lower than the original soft limit.
   * @throws Nothing.
   */
  explicit ScopedFileDescriptorLimit(rlim_t maximum) noexcept {
    if (::getrlimit(RLIMIT_NOFILE, &original_) != 0) {
      return;
    }
    struct rlimit limited = original_;
    if (limited.rlim_cur == RLIM_INFINITY || limited.rlim_cur > maximum) {
      limited.rlim_cur = maximum;
    }
    if (::setrlimit(RLIMIT_NOFILE, &limited) == 0) {
      active_ = true;
    }
  }

  /**
   * @brief Restores the original process descriptor limit.
   * @throws Nothing.
   */
  ~ScopedFileDescriptorLimit() noexcept {
    if (active_) {
      (void)::setrlimit(RLIMIT_NOFILE, &original_);
    }
  }

  /**
   * @brief Prevents overlapping restoration ownership.
   * @throws Nothing because construction is unavailable.
   */
  ScopedFileDescriptorLimit(const ScopedFileDescriptorLimit&) = delete;

  /**
   * @brief Prevents replacing one restoration obligation by assignment.
   * @return No value because copying is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedFileDescriptorLimit& operator=(const ScopedFileDescriptorLimit&) =
      delete;

  /**
   * @brief Reports whether the temporary limit is installed.
   * @return True after successful get/set operations.
   * @throws Nothing.
   */
  bool active() const noexcept { return active_; }

 private:
  /** @brief Original soft/hard descriptor limits. */
  struct rlimit original_{};

  /** @brief Whether destruction must restore original_. */
  bool active_ = false;
};

/**
 * @brief Complete socket/lock fixture used to start one OutputStore.
 * @throws std::runtime_error when its lifecycle lock file cannot be opened.
 */
class StoreEnvironment {
 public:
  /**
   * @brief Opens and exclusively locks one lifecycle file in the private root.
   * @throws std::runtime_error when lock creation or nonblocking flock fails.
   */
  StoreEnvironment()
      : socket_path_((root_.path() / "photospiderd.sock").string()),
        lock_(::open((root_.path() / "photospiderd.sock.lock").c_str(),
                     O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
    if (!lock_ || ::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0) {
      throw std::runtime_error(
          "cannot acquire output-store lifecycle test lock");
    }
  }

  /**
   * @brief Returns the synthetic absolute socket path.
   * @return Borrowed path string.
   * @throws Nothing.
   */
  const std::string& socket_path() const noexcept { return socket_path_; }

  /**
   * @brief Returns the held lifecycle-lock descriptor.
   * @return Nonnegative borrowed descriptor.
   * @throws Nothing.
   */
  int lock_fd() const noexcept { return lock_.get(); }

  /**
   * @brief Returns `<socket>.outputs` for direct filesystem assertions.
   * @return Absolute base path.
   * @throws std::bad_alloc if path storage cannot be allocated.
   */
  std::filesystem::path base_path() const {
    return std::filesystem::path(socket_path_ + ".outputs");
  }

  /**
   * @brief Returns one controlled instance path.
   * @param instance_id Valid opaque daemon identity.
   * @return Absolute child path.
   * @throws std::bad_alloc if path storage cannot be allocated.
   */
  std::filesystem::path instance_path(const std::string& instance_id) const {
    return base_path() / ("instance-" + instance_id);
  }

  /**
   * @brief Returns the private root for restart fixture construction.
   * @return Borrowed filesystem path.
   * @throws Nothing.
   */
  const std::filesystem::path& root_path() const noexcept {
    return root_.path();
  }

 private:
  /** @brief Private test-owned filesystem root. */
  TemporaryDirectory root_;

  /** @brief Synthetic daemon socket pathname. */
  std::string socket_path_;

  /** @brief Borrowed-by-store lifecycle lock owner. */
  UniqueFd lock_;
};

/**
 * @brief Creates a CPU image whose source rows may contain padding.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param channels Channels per pixel.
 * @param step Source bytes between rows.
 * @param bytes Complete padded source payload.
 * @return ImageBuffer sharing ownership of the supplied bytes.
 * @throws std::bad_alloc if payload storage cannot be allocated.
 */
ImageBuffer make_u8_image(int width, int height, int channels, std::size_t step,
                          std::vector<std::uint8_t> bytes) {
  auto storage = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
  ImageBuffer image;
  image.width = width;
  image.height = height;
  image.channels = channels;
  image.type = DataType::UINT8;
  image.device = Device::CPU;
  image.step = step;
  image.data = std::shared_ptr<void>(storage, storage->data());
  return image;
}

/**
 * @brief Reads one complete small artifact through no-follow descriptor IO.
 * @param path Absolute artifact path.
 * @return All bytes read to EOF.
 * @throws std::runtime_error for open/read failure.
 */
std::vector<std::uint8_t> read_bytes(const std::string& path) {
  UniqueFd descriptor(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (!descriptor) {
    throw std::runtime_error("cannot open output-store test artifact");
  }
  std::vector<std::uint8_t> bytes;
  std::array<std::uint8_t, 256> buffer{};
  while (true) {
    const ssize_t count =
        ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count > 0) {
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
      continue;
    }
    if (count == 0) {
      return bytes;
    }
    if (errno != EINTR) {
      throw std::runtime_error("cannot read output-store test artifact");
    }
  }
}

/**
 * @brief Creates one exact-mode fixture file with controlled contents.
 * @param path File path below a test-owned directory.
 * @param mode Exact permission bits applied after creation.
 * @param byte One byte written to the file.
 * @return Nothing.
 * @throws std::runtime_error for creation, chmod, or write failure.
 */
void create_file(const std::filesystem::path& path, mode_t mode,
                 std::uint8_t byte = 0x5a) {
  UniqueFd descriptor(
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode));
  if (!descriptor || ::fchmod(descriptor.get(), mode) != 0 ||
      ::write(descriptor.get(), &byte, 1) != 1) {
    throw std::runtime_error("cannot create output-store filesystem fixture");
  }
}

/**
 * @brief Counts non-dot directory entries for no-partial-file assertions.
 * @param path Existing directory path.
 * @return Entry count.
 * @throws std::filesystem::filesystem_error when enumeration fails.
 */
std::size_t directory_entry_count(const std::filesystem::path& path) {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator(path),
                    std::filesystem::directory_iterator()));
}

/**
 * @brief Commits one deterministic active session for store/registry tests.
 * @param sessions Session registry under test.
 * @return Active opaque session id.
 * @throws std::runtime_error if reserve or commit fails.
 */
IpcSessionId activate_output_session(SessionRegistry* sessions) {
  IpcResult<IpcSessionId> reserved = sessions->reserve("output-session");
  if (!reserved.status.ok ||
      !sessions->commit(reserved.value, GraphSessionId{"private-output"}).ok) {
    throw std::runtime_error("cannot activate output-store test session");
  }
  return reserved.value;
}

/**
 * @brief Waits for one compute registry record to become terminal.
 * @param registry Joined compute registry under test.
 * @param compute_id Existing compute identity.
 * @return Complete terminal snapshot or a default snapshot on watchdog expiry.
 * @throws std::bad_alloc if snapshot copying cannot allocate.
 * @note The deadline is a failure watchdog; yielding does not establish order.
 */
ComputeRequestSnapshot wait_for_output_terminal(
    ComputeRequestRegistry* registry, const ComputeRequestId& compute_id) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    IpcResult<ComputeRequestSnapshot> current = registry->status(compute_id);
    if (current.status.ok &&
        (current.value.state == ComputeRequestState::Succeeded ||
         current.value.state == ComputeRequestState::Failed)) {
      return current.value;
    }
    std::this_thread::yield();
  }
  return {};
}

TEST(OutputStore, CanonicalEmptyImagePublishesNoFileOrQuota) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(
      store
          .start(environment.socket_path(), opaque_id(1), environment.lock_fd())
          .ok);

  ComputeOutputPublication publication =
      store.publish(ComputeRequestId{opaque_id(10)}, ImageBuffer{});

  EXPECT_TRUE(publication.status.ok);
  EXPECT_FALSE(publication.output.active());
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
  EXPECT_EQ(directory_entry_count(environment.instance_path(opaque_id(1))), 0U);
}

TEST(OutputStore, WritesExactTightRowsAndPrivateIdentityMetadata) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  const std::string instance_id = opaque_id(2);
  ASSERT_TRUE(
      store.start(environment.socket_path(), instance_id, environment.lock_fd())
          .ok);
  ImageBuffer image = make_u8_image(
      2, 2, 3, 8, {1, 2, 3, 4, 5, 6, 90, 91, 7, 8, 9, 10, 11, 12, 92, 93});

  ComputeOutputPublication publication =
      store.publish(ComputeRequestId{opaque_id(20)}, std::move(image));
  ASSERT_TRUE(publication.status.ok);
  ASSERT_TRUE(publication.output.active());
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);

  EXPECT_EQ(delivery.value.metadata.width, 2);
  EXPECT_EQ(delivery.value.metadata.height, 2);
  EXPECT_EQ(delivery.value.metadata.channels, 3);
  EXPECT_EQ(delivery.value.metadata.data_type, DataType::UINT8);
  EXPECT_EQ(delivery.value.metadata.device, Device::CPU);
  EXPECT_EQ(delivery.value.metadata.row_step, 6U);
  EXPECT_EQ(delivery.value.metadata.byte_size, 12U);
  EXPECT_EQ(read_bytes(delivery.value.metadata.path),
            (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));

  struct stat base{};
  struct stat instance{};
  struct stat artifact{};
  ASSERT_EQ(::lstat(environment.base_path().c_str(), &base), 0);
  ASSERT_EQ(::lstat(environment.instance_path(instance_id).c_str(), &instance),
            0);
  ASSERT_EQ(::lstat(delivery.value.metadata.path.c_str(), &artifact), 0);
  EXPECT_TRUE(S_ISDIR(base.st_mode));
  EXPECT_TRUE(S_ISDIR(instance.st_mode));
  EXPECT_TRUE(S_ISREG(artifact.st_mode));
  EXPECT_EQ(base.st_mode & 07777, 0700);
  EXPECT_EQ(instance.st_mode & 07777, 0700);
  EXPECT_EQ(artifact.st_mode & 07777, 0600);
  EXPECT_EQ(artifact.st_uid, ::geteuid());
  EXPECT_EQ(artifact.st_nlink, 1);
  EXPECT_EQ(static_cast<std::uint64_t>(artifact.st_dev),
            delivery.value.metadata.filesystem_device);
  EXPECT_EQ(static_cast<std::uint64_t>(artifact.st_ino),
            delivery.value.metadata.inode);
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_EQ(store.retained_bytes(), 12U);

  publication.output.reset(delivery.value.delivery_id);
  EXPECT_FALSE(std::filesystem::exists(delivery.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
}

TEST(OutputStore, MalformedImagesBecomeInternalFailureWithoutResidue) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  const std::string instance_id = opaque_id(3);
  ASSERT_TRUE(
      store.start(environment.socket_path(), instance_id, environment.lock_fd())
          .ok);

  std::vector<ImageBuffer> malformed;
  ImageBuffer missing_data;
  missing_data.width = 1;
  missing_data.height = 1;
  missing_data.channels = 1;
  missing_data.step = 1;
  malformed.push_back(missing_data);

  ImageBuffer gpu = make_u8_image(1, 1, 1, 1, {1});
  gpu.device = Device::GPU_METAL;
  malformed.push_back(gpu);

  ImageBuffer short_step = make_u8_image(2, 1, 2, 3, {1, 2, 3, 4});
  malformed.push_back(short_step);

  ImageBuffer invalid_type =
      make_u8_image(1, 1, 1, 8, {1, 2, 3, 4, 5, 6, 7, 8});
  invalid_type.type = static_cast<DataType>(999);
  malformed.push_back(invalid_type);

  ImageBuffer overflow = make_u8_image(1, 1, 1, 1, {1});
  overflow.width = std::numeric_limits<int>::max();
  overflow.height = std::numeric_limits<int>::max();
  overflow.channels = std::numeric_limits<int>::max();
  overflow.type = DataType::FLOAT64;
  overflow.step = std::numeric_limits<std::size_t>::max();
  malformed.push_back(overflow);

  std::uint64_t compute = 30;
  for (ImageBuffer& image : malformed) {
    ComputeOutputPublication publication =
        store.publish(ComputeRequestId{opaque_id(compute++)}, std::move(image));
    EXPECT_FALSE(publication.status.ok);
    EXPECT_EQ(publication.status.domain, OperationErrorDomain::Daemon);
    EXPECT_EQ(publication.status.code, kInternalErrorCode);
    EXPECT_EQ(publication.status.name, "internal_error");
    EXPECT_FALSE(publication.output.active());
  }
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
  EXPECT_EQ(directory_entry_count(environment.instance_path(instance_id)), 0U);
}

TEST(OutputStore, EnforcesPerArtifactAndTotalQuotaTransactionally) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStoreLimits limits;
  limits.artifacts = 4;
  limits.total_bytes = 10;
  limits.artifact_bytes = 8;
  OutputStore store(limits, {}, [&] { return ids.next(); });
  const std::string instance_id = opaque_id(4);
  ASSERT_TRUE(
      store.start(environment.socket_path(), instance_id, environment.lock_fd())
          .ok);

  ComputeOutputPublication too_large =
      store.publish(ComputeRequestId{opaque_id(40)},
                    make_u8_image(9, 1, 1, 9, {1, 2, 3, 4, 5, 6, 7, 8, 9}));
  EXPECT_FALSE(too_large.status.ok);
  EXPECT_EQ(too_large.status.code, kArtifactLimitExceededCode);
  EXPECT_EQ(store.artifact_count(), 0U);

  ComputeOutputPublication first =
      store.publish(ComputeRequestId{opaque_id(41)},
                    make_u8_image(6, 1, 1, 6, {1, 2, 3, 4, 5, 6}));
  ASSERT_TRUE(first.status.ok);
  ComputeOutputPublication total =
      store.publish(ComputeRequestId{opaque_id(42)},
                    make_u8_image(5, 1, 1, 5, {7, 8, 9, 10, 11}));
  EXPECT_FALSE(total.status.ok);
  EXPECT_EQ(total.status.code, kArtifactLimitExceededCode);
  EXPECT_FALSE(total.output.active());
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_EQ(store.retained_bytes(), 6U);
  EXPECT_EQ(directory_entry_count(environment.instance_path(instance_id)), 1U);
  first.output.reset();
}

TEST(OutputStore, OneComputeJobCanPublishOnlyOneStableArtifact) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  const std::string instance_id = opaque_id(20);
  ASSERT_TRUE(
      store.start(environment.socket_path(), instance_id, environment.lock_fd())
          .ok);
  const ComputeRequestId compute_id{opaque_id(200)};
  ComputeOutputPublication first =
      store.publish(compute_id, make_u8_image(1, 1, 1, 1, {1}));
  ASSERT_TRUE(first.status.ok);
  ComputeOutputPublication duplicate =
      store.publish(compute_id, make_u8_image(1, 1, 1, 1, {2}));

  EXPECT_FALSE(duplicate.status.ok);
  EXPECT_EQ(duplicate.status.code, kInternalErrorCode);
  EXPECT_FALSE(duplicate.output.active());
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_EQ(store.retained_bytes(), 1U);
  EXPECT_EQ(directory_entry_count(environment.instance_path(instance_id)), 1U);
  first.output.reset();
  EXPECT_EQ(store.artifact_count(), 0U);
}

TEST(OutputStore, LastArtifactSlotHasOneConcurrentWinner) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStoreLimits limits;
  limits.artifacts = 1;
  limits.total_bytes = 16;
  limits.artifact_bytes = 16;
  OutputStore store(limits, {}, [&] { return ids.next(); });
  ASSERT_TRUE(
      store
          .start(environment.socket_path(), opaque_id(5), environment.lock_fd())
          .ok);
  StartGate gate;
  std::array<OperationStatus, 2> statuses;
  std::array<ComputeOutputOwnership, 2> ownership;
  std::array<std::thread, 2> threads;
  for (std::size_t index = 0; index < threads.size(); ++index) {
    threads[index] = std::thread([&, index] {
      gate.arrive_and_wait();
      ComputeOutputPublication publication =
          store.publish(ComputeRequestId{opaque_id(50 + index)},
                        make_u8_image(4, 1, 1, 4, {1, 2, 3, 4}));
      statuses[index] = publication.status;
      ownership[index] = std::move(publication.output);
    });
  }
  const bool participants_ready = gate.wait_ready(2);
  gate.release();
  for (std::thread& thread : threads) {
    thread.join();
  }
  ASSERT_TRUE(participants_ready);

  const int successes =
      static_cast<int>(statuses[0].ok) + static_cast<int>(statuses[1].ok);
  EXPECT_EQ(successes, 1);
  for (std::size_t index = 0; index < statuses.size(); ++index) {
    if (!statuses[index].ok) {
      EXPECT_EQ(statuses[index].code, kArtifactLimitExceededCode);
    }
  }
  EXPECT_EQ(store.artifact_count(), 1U);
  ownership[0].reset();
  ownership[1].reset();
  EXPECT_EQ(store.artifact_count(), 0U);
}

TEST(OutputStore, RepeatedDeliveryRefreshesOneStableLeaseAtomically) {
  StoreEnvironment environment;
  SequentialIds ids;
  ManualClock clock;
  OutputStoreLimits limits;
  limits.job_ttl = std::chrono::seconds(100);
  limits.delivery_ttl = std::chrono::seconds(10);
  OutputStore store(
      limits, [&] { return clock.now(); }, [&] { return ids.next(); });
  ASSERT_TRUE(
      store
          .start(environment.socket_path(), opaque_id(6), environment.lock_fd())
          .ok);
  ComputeOutputPublication publication = store.publish(
      ComputeRequestId{opaque_id(60)}, make_u8_image(1, 1, 1, 1, {9}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> first =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(first.status.ok);

  clock.advance(std::chrono::seconds(9));
  IpcResult<OutputArtifactDelivery> refreshed =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(refreshed.status.ok);
  EXPECT_EQ(refreshed.value.delivery_id, first.value.delivery_id);
  EXPECT_EQ(refreshed.value.metadata.output_id, first.value.metadata.output_id);

  clock.advance(std::chrono::seconds(2));
  EXPECT_EQ(store.cleanup_expired(), 0U);
  publication.output.reset();
  EXPECT_TRUE(std::filesystem::exists(first.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_EQ(store.retained_bytes(), 1U);

  clock.advance(std::chrono::seconds(7));
  EXPECT_EQ(store.cleanup_expired(), 0U);
  clock.advance(std::chrono::seconds(1));
  EXPECT_EQ(store.cleanup_expired(), 1U);
  EXPECT_FALSE(std::filesystem::exists(first.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 0U);
}

TEST(OutputStore, ExpiredLeaseReactivatesSameIdWhileJobIsOwned) {
  StoreEnvironment environment;
  SequentialIds ids;
  ManualClock clock;
  OutputStoreLimits limits;
  limits.job_ttl = std::chrono::seconds(100);
  limits.delivery_ttl = std::chrono::seconds(5);
  OutputStore store(
      limits, [&] { return clock.now(); }, [&] { return ids.next(); });
  ASSERT_TRUE(
      store
          .start(environment.socket_path(), opaque_id(7), environment.lock_fd())
          .ok);
  ComputeOutputPublication publication = store.publish(
      ComputeRequestId{opaque_id(70)}, make_u8_image(1, 1, 1, 1, {1}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> first =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(first.status.ok);

  clock.advance(std::chrono::seconds(5));
  EXPECT_EQ(store.cleanup_expired(), 1U);
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_TRUE(std::filesystem::exists(first.value.metadata.path));
  IpcResult<OutputArtifactDelivery> reactivated =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(reactivated.status.ok);
  EXPECT_EQ(reactivated.value.delivery_id, first.value.delivery_id);

  publication.output.reset(reactivated.value.delivery_id);
  EXPECT_FALSE(std::filesystem::exists(first.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 0U);
}

TEST(OutputStore, JobReleaseLazilyExpiresLeaseAtExactDeadline) {
  StoreEnvironment environment;
  SequentialIds ids;
  ManualClock clock;
  OutputStoreLimits limits;
  limits.job_ttl = std::chrono::seconds(100);
  limits.delivery_ttl = std::chrono::seconds(5);
  OutputStore store(
      limits, [&] { return clock.now(); }, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(19),
                         environment.lock_fd())
                  .ok);
  ComputeOutputPublication publication = store.publish(
      ComputeRequestId{opaque_id(190)}, make_u8_image(1, 1, 1, 1, {1}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);

  clock.advance(std::chrono::seconds(5));
  publication.output.reset();
  EXPECT_FALSE(std::filesystem::exists(delivery.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
}

TEST(OutputStore, JobTtlPreservesOnlyAnActiveDeliveryLease) {
  StoreEnvironment environment;
  SequentialIds ids;
  ManualClock clock;
  OutputStoreLimits limits;
  limits.job_ttl = std::chrono::seconds(5);
  limits.delivery_ttl = std::chrono::seconds(10);
  OutputStore store(
      limits, [&] { return clock.now(); }, [&] { return ids.next(); });
  ASSERT_TRUE(
      store
          .start(environment.socket_path(), opaque_id(8), environment.lock_fd())
          .ok);
  ComputeOutputPublication publication = store.publish(
      ComputeRequestId{opaque_id(80)}, make_u8_image(1, 1, 1, 1, {1}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);

  clock.advance(std::chrono::seconds(5));
  EXPECT_EQ(store.cleanup_expired(), 0U);
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_TRUE(std::filesystem::exists(delivery.value.metadata.path));
  IpcResult<OutputArtifactDelivery> after_job_ttl =
      store.acquire_delivery(publication.output.reference());
  EXPECT_FALSE(after_job_ttl.status.ok);
  EXPECT_EQ(after_job_ttl.status.code, kArtifactNotFoundCode);

  clock.advance(std::chrono::seconds(5));
  EXPECT_EQ(store.cleanup_expired(), 1U);
  EXPECT_EQ(store.artifact_count(), 0U);
  publication.output.reset();
}

TEST(OutputStore, OrphanLeaseReleaseRequiresOriginalJobAndStableDelivery) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(
      store
          .start(environment.socket_path(), opaque_id(9), environment.lock_fd())
          .ok);
  const ComputeRequestId compute_id{opaque_id(90)};
  ComputeOutputPublication publication =
      store.publish(compute_id, make_u8_image(1, 1, 1, 1, {4}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);
  const std::string path = delivery.value.metadata.path;
  publication.output.reset(std::optional<std::string>(opaque_id(9999)));

  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(store.release_orphaned_delivery(ComputeRequestId{opaque_id(91)},
                                               delivery.value.delivery_id));
  EXPECT_FALSE(store.release_orphaned_delivery(compute_id, opaque_id(9998)));
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_TRUE(
      store.release_orphaned_delivery(compute_id, delivery.value.delivery_id));
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_EQ(store.artifact_count(), 0U);
}

/**
 * @brief Releases one matching lease while its job owner is removed.
 *
 * Both operations start at one deterministic gate. Regardless of lock order,
 * matching lease release must succeed: it either clears the lease before job
 * removal or uses the orphaned-delivery path immediately afterward.
 *
 * @return Nothing; GoogleTest assertions report gate, release, file, or quota
 *         mismatch.
 * @throws std::bad_alloc, std::filesystem::filesystem_error, or
 *         std::system_error if store, artifact, gate, or thread setup fails.
 */
TEST(OutputStore, ConcurrentJobRemovalAndMatchingLeaseReleaseCleanArtifact) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(21),
                         environment.lock_fd())
                  .ok);
  const ComputeRequestId compute_id{opaque_id(210)};
  ComputeOutputPublication publication =
      store.publish(compute_id, make_u8_image(1, 1, 1, 1, {4}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);
  const std::string path = delivery.value.metadata.path;
  ComputeOutputOwnership ownership = std::move(publication.output);

  StartGate gate;
  bool lease_released = false;
  std::thread job_remover([&] {
    gate.arrive_and_wait();
    ownership.reset();
  });
  std::thread lease_releaser([&] {
    gate.arrive_and_wait();
    lease_released =
        store.release_orphaned_delivery(compute_id, delivery.value.delivery_id);
  });
  const bool participants_ready = gate.wait_ready(2);
  gate.release();
  job_remover.join();
  lease_releaser.join();

  ASSERT_TRUE(participants_ready);
  EXPECT_TRUE(lease_released);
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
}

TEST(OutputStore, OpenDescriptorSurvivesLeaseAwareUnlink) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(14),
                         environment.lock_fd())
                  .ok);
  const ComputeRequestId compute_id{opaque_id(140)};
  ComputeOutputPublication publication =
      store.publish(compute_id, make_u8_image(3, 1, 1, 3, {4, 5, 6}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);
  publication.output.reset();
  ASSERT_TRUE(std::filesystem::exists(delivery.value.metadata.path));

  UniqueFd descriptor(::open(delivery.value.metadata.path.c_str(),
                             O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  ASSERT_TRUE(descriptor);
  EXPECT_TRUE(
      store.release_orphaned_delivery(compute_id, delivery.value.delivery_id));
  EXPECT_FALSE(std::filesystem::exists(delivery.value.metadata.path));
  std::array<std::uint8_t, 3> bytes{};
  ASSERT_EQ(::read(descriptor.get(), bytes.data(), bytes.size()), 3);
  EXPECT_EQ(bytes, (std::array<std::uint8_t, 3>{4, 5, 6}));
}

TEST(OutputStore, ConcurrentResultAndJobReleasePreserveOneValidOutcome) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(15),
                         environment.lock_fd())
                  .ok);
  const ComputeRequestId compute_id{opaque_id(150)};
  ComputeOutputPublication publication =
      store.publish(compute_id, make_u8_image(1, 1, 1, 1, {7}));
  ASSERT_TRUE(publication.status.ok);
  const std::string output_id = publication.output.reference();
  ComputeOutputOwnership ownership = std::move(publication.output);
  StartGate gate;
  IpcResult<OutputArtifactDelivery> result;
  std::thread result_thread([&] {
    gate.arrive_and_wait();
    result = store.acquire_delivery(output_id);
  });
  std::thread release_thread([&] {
    gate.arrive_and_wait();
    ownership.reset();
  });
  const bool participants_ready = gate.wait_ready(2);
  gate.release();
  result_thread.join();
  release_thread.join();
  ASSERT_TRUE(participants_ready);

  if (result.status.ok) {
    EXPECT_TRUE(std::filesystem::exists(result.value.metadata.path));
    EXPECT_TRUE(
        store.release_orphaned_delivery(compute_id, result.value.delivery_id));
  } else {
    EXPECT_EQ(result.status.code, kArtifactNotFoundCode);
  }
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
}

TEST(OutputStore, ConcurrentLeaseExpiryAndJobReleaseCleanExactlyOnce) {
  StoreEnvironment environment;
  SequentialIds ids;
  ManualClock clock;
  OutputStoreLimits limits;
  limits.job_ttl = std::chrono::seconds(100);
  limits.delivery_ttl = std::chrono::seconds(5);
  OutputStore store(
      limits, [&] { return clock.now(); }, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(16),
                         environment.lock_fd())
                  .ok);
  ComputeOutputPublication publication = store.publish(
      ComputeRequestId{opaque_id(160)}, make_u8_image(1, 1, 1, 1, {8}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);
  ComputeOutputOwnership ownership = std::move(publication.output);
  clock.advance(std::chrono::seconds(5));
  StartGate gate;
  std::size_t expired = 0;
  std::thread expiry_thread([&] {
    gate.arrive_and_wait();
    expired = store.cleanup_expired();
  });
  std::thread release_thread([&] {
    gate.arrive_and_wait();
    ownership.reset();
  });
  const bool participants_ready = gate.wait_ready(2);
  gate.release();
  expiry_thread.join();
  release_thread.join();
  ASSERT_TRUE(participants_ready);

  EXPECT_LE(expired, 1U);
  EXPECT_FALSE(std::filesystem::exists(delivery.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
}

TEST(OutputStore, ComputeRegistryEvictionDropsJobButPreservesLease) {
  StoreEnvironment environment;
  SequentialIds output_ids(1000);
  SequentialIds session_ids(2000);
  SequentialIds compute_ids(3000);
  OutputStore store({}, {}, [&] { return output_ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(17),
                         environment.lock_fd())
                  .ok);
  SessionRegistry sessions([&] { return session_ids.next(); });
  const IpcSessionId session = activate_output_session(&sessions);
  ComputeRequestRegistryLimits registry_limits;
  registry_limits.terminal = 1;
  ComputeRequestRegistry registry(
      sessions, [](const HostComputeRequest&) { return ok_status(); },
      [](const HostComputeRequest& request) {
        return Result<ImageBuffer>{
            ok_status(),
            make_u8_image(1, 1, 1, 1,
                          {static_cast<std::uint8_t>(request.node.value)})};
      },
      [&](const ComputeRequestId& compute_id, ImageBuffer image) {
        return store.publish(compute_id, std::move(image));
      },
      registry_limits, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  HostComputeRequest first_request;
  first_request.node = NodeId{1};
  IpcResult<ComputeRequestSnapshot> first =
      registry.submit(session, first_request, ComputeResultMode::Image);
  ASSERT_TRUE(first.status.ok);
  ComputeRequestSnapshot first_terminal =
      wait_for_output_terminal(&registry, first.value.compute_id);
  ASSERT_EQ(first_terminal.state, ComputeRequestState::Succeeded);
  ASSERT_TRUE(first_terminal.output_reference.has_value());
  IpcResult<OutputArtifactDelivery> first_delivery =
      store.acquire_delivery(*first_terminal.output_reference);
  ASSERT_TRUE(first_delivery.status.ok);

  HostComputeRequest second_request;
  second_request.node = NodeId{2};
  IpcResult<ComputeRequestSnapshot> second =
      registry.submit(session, second_request, ComputeResultMode::Image);
  ASSERT_TRUE(second.status.ok);
  ComputeRequestSnapshot second_terminal =
      wait_for_output_terminal(&registry, second.value.compute_id);
  ASSERT_EQ(second_terminal.state, ComputeRequestState::Succeeded);
  ASSERT_TRUE(second_terminal.output_reference.has_value());
  IpcResult<OutputArtifactDelivery> second_delivery =
      store.acquire_delivery(*second_terminal.output_reference);
  ASSERT_TRUE(second_delivery.status.ok);
  EXPECT_FALSE(registry.status(first.value.compute_id).status.ok);
  EXPECT_TRUE(std::filesystem::exists(first_delivery.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 2U);

  EXPECT_TRUE(store.release_orphaned_delivery(
      first.value.compute_id, first_delivery.value.delivery_id));
  EXPECT_FALSE(std::filesystem::exists(first_delivery.value.metadata.path));
  EXPECT_TRUE(
      registry
          .release(second.value.compute_id, second_delivery.value.delivery_id)
          .ok);
  EXPECT_FALSE(std::filesystem::exists(second_delivery.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 0U);
  registry.shutdown();
}

TEST(OutputStore, MissingOrReplacedArtifactReturnsOnlyArtifactNotFound) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(10),
                         environment.lock_fd())
                  .ok);
  const ComputeRequestId missing_compute{opaque_id(100)};
  ComputeOutputPublication missing =
      store.publish(missing_compute, make_u8_image(1, 1, 1, 1, {1}));
  ASSERT_TRUE(missing.status.ok);
  IpcResult<OutputArtifactDelivery> missing_delivery =
      store.acquire_delivery(missing.output.reference());
  ASSERT_TRUE(missing_delivery.status.ok);
  ASSERT_EQ(::unlink(missing_delivery.value.metadata.path.c_str()), 0);
  IpcResult<OutputArtifactDelivery> missing_result =
      store.acquire_delivery(missing.output.reference());
  EXPECT_FALSE(missing_result.status.ok);
  EXPECT_EQ(missing_result.status.domain, OperationErrorDomain::Daemon);
  EXPECT_EQ(missing_result.status.code, kArtifactNotFoundCode);
  EXPECT_EQ(missing_result.status.name, "artifact_not_found");
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_EQ(store.retained_bytes(), 1U);
  missing.output.reset(missing_delivery.value.delivery_id);
  EXPECT_EQ(store.artifact_count(), 0U);

  const ComputeRequestId replaced_compute{opaque_id(101)};
  ComputeOutputPublication replaced =
      store.publish(replaced_compute, make_u8_image(1, 1, 1, 1, {2}));
  ASSERT_TRUE(replaced.status.ok);
  IpcResult<OutputArtifactDelivery> replaced_delivery =
      store.acquire_delivery(replaced.output.reference());
  ASSERT_TRUE(replaced_delivery.status.ok);
  const std::filesystem::path victim = environment.root_path() / "victim";
  create_file(victim, 0600, 0x7e);
  ASSERT_EQ(::unlink(replaced_delivery.value.metadata.path.c_str()), 0);
  ASSERT_EQ(
      ::symlink(victim.c_str(), replaced_delivery.value.metadata.path.c_str()),
      0);

  IpcResult<OutputArtifactDelivery> replaced_result =
      store.acquire_delivery(replaced.output.reference());
  EXPECT_FALSE(replaced_result.status.ok);
  EXPECT_EQ(replaced_result.status.code, kArtifactNotFoundCode);
  struct stat replacement{};
  ASSERT_EQ(
      ::lstat(replaced_delivery.value.metadata.path.c_str(), &replacement), 0);
  EXPECT_TRUE(S_ISLNK(replacement.st_mode));
  EXPECT_EQ(read_bytes(victim.string()), (std::vector<std::uint8_t>{0x7e}));
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_EQ(store.retained_bytes(), 1U);
  replaced.output.reset(replaced_delivery.value.delivery_id);
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
}

TEST(OutputStore, DescriptorExhaustionPreservesRecordLeaseAndArtifact) {
  StoreEnvironment environment;
  ScopedFileDescriptorLimit descriptor_limit(64);
  if (!descriptor_limit.active()) {
    GTEST_SKIP() << "cannot install a temporary RLIMIT_NOFILE";
  }
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(18),
                         environment.lock_fd())
                  .ok);
  ComputeOutputPublication publication = store.publish(
      ComputeRequestId{opaque_id(180)}, make_u8_image(1, 1, 1, 1, {6}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);

  std::vector<UniqueFd> fillers;
  fillers.reserve(64);
  while (true) {
    const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
      break;
    }
    fillers.emplace_back(descriptor);
  }
  const int exhaustion_error = errno;
  if (exhaustion_error != EMFILE && exhaustion_error != EBADF) {
    ADD_FAILURE() << "descriptor fill failed with errno " << exhaustion_error;
    fillers.clear();
    publication.output.reset(delivery.value.delivery_id);
    return;
  }
  IpcResult<OutputArtifactDelivery> exhausted =
      store.acquire_delivery(publication.output.reference());
  EXPECT_FALSE(exhausted.status.ok);
  EXPECT_EQ(exhausted.status.code, kInternalErrorCode);
  EXPECT_EQ(store.artifact_count(), 1U);
  EXPECT_EQ(store.retained_bytes(), 1U);
  EXPECT_TRUE(std::filesystem::exists(delivery.value.metadata.path));

  fillers.clear();
  publication.output.reset(delivery.value.delivery_id);
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_FALSE(std::filesystem::exists(delivery.value.metadata.path));
}

TEST(OutputStore, WrongModeAndHardLinkArePreservedDuringLiveCleanup) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(11),
                         environment.lock_fd())
                  .ok);

  ComputeOutputPublication wrong_mode = store.publish(
      ComputeRequestId{opaque_id(110)}, make_u8_image(1, 1, 1, 1, {3}));
  ASSERT_TRUE(wrong_mode.status.ok);
  IpcResult<OutputArtifactDelivery> wrong_mode_delivery =
      store.acquire_delivery(wrong_mode.output.reference());
  ASSERT_TRUE(wrong_mode_delivery.status.ok);
  ASSERT_EQ(::chmod(wrong_mode_delivery.value.metadata.path.c_str(), 0644), 0);
  IpcResult<OutputArtifactDelivery> wrong_mode_result =
      store.acquire_delivery(wrong_mode.output.reference());
  EXPECT_EQ(wrong_mode_result.status.code, kArtifactNotFoundCode);
  EXPECT_TRUE(std::filesystem::exists(wrong_mode_delivery.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 1U);
  wrong_mode.output.reset(wrong_mode_delivery.value.delivery_id);
  EXPECT_EQ(store.artifact_count(), 0U);

  ComputeOutputPublication hard_linked = store.publish(
      ComputeRequestId{opaque_id(111)}, make_u8_image(1, 1, 1, 1, {4}));
  ASSERT_TRUE(hard_linked.status.ok);
  IpcResult<OutputArtifactDelivery> hard_link_delivery =
      store.acquire_delivery(hard_linked.output.reference());
  ASSERT_TRUE(hard_link_delivery.status.ok);
  const std::filesystem::path second_link = environment.root_path() / "link";
  ASSERT_EQ(::link(hard_link_delivery.value.metadata.path.c_str(),
                   second_link.c_str()),
            0);
  IpcResult<OutputArtifactDelivery> hard_link_result =
      store.acquire_delivery(hard_linked.output.reference());
  EXPECT_EQ(hard_link_result.status.code, kArtifactNotFoundCode);
  EXPECT_TRUE(std::filesystem::exists(hard_link_delivery.value.metadata.path));
  EXPECT_TRUE(std::filesystem::exists(second_link));
  EXPECT_EQ(store.artifact_count(), 1U);
  hard_linked.output.reset(hard_link_delivery.value.delivery_id);
  EXPECT_EQ(store.artifact_count(), 0U);
}

TEST(OutputStore, RestartCleanupRemovesOnlyRecognizedSafeStaleEntries) {
  StoreEnvironment environment;
  const std::filesystem::path base = environment.base_path();
  ASSERT_TRUE(std::filesystem::create_directory(base));
  ASSERT_EQ(::chmod(base.c_str(), 0700), 0);
  const std::filesystem::path mixed = base / ("instance-" + opaque_id(200));
  const std::filesystem::path safe_only = base / ("instance-" + opaque_id(201));
  ASSERT_TRUE(std::filesystem::create_directory(mixed));
  ASSERT_TRUE(std::filesystem::create_directory(safe_only));
  ASSERT_EQ(::chmod(mixed.c_str(), 0700), 0);
  ASSERT_EQ(::chmod(safe_only.c_str(), 0700), 0);

  const std::filesystem::path safe_mixed =
      mixed / ("output-" + opaque_id(210) + ".bin");
  const std::filesystem::path wrong_mode =
      mixed / ("output-" + opaque_id(211) + ".bin");
  const std::filesystem::path hard_linked =
      mixed / ("output-" + opaque_id(212) + ".bin");
  const std::filesystem::path hard_link_alias =
      environment.root_path() / "alias";
  const std::filesystem::path unrecognized = mixed / "notes.txt";
  const std::filesystem::path symlink_entry =
      mixed / ("output-" + opaque_id(213) + ".bin");
  const std::filesystem::path fifo_entry =
      mixed / ("output-" + opaque_id(215) + ".bin");
  const std::filesystem::path safe_alone =
      safe_only / ("output-" + opaque_id(214) + ".bin");
  create_file(safe_mixed, 0600);
  create_file(wrong_mode, 0644);
  create_file(hard_linked, 0600);
  ASSERT_EQ(::link(hard_linked.c_str(), hard_link_alias.c_str()), 0);
  create_file(unrecognized, 0600);
  ASSERT_EQ(::symlink(unrecognized.c_str(), symlink_entry.c_str()), 0);
  ASSERT_EQ(::mkfifo(fifo_entry.c_str(), 0600), 0);
  create_file(safe_alone, 0600);

  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(202),
                         environment.lock_fd())
                  .ok);

  EXPECT_FALSE(std::filesystem::exists(safe_mixed));
  EXPECT_TRUE(std::filesystem::exists(wrong_mode));
  EXPECT_TRUE(std::filesystem::exists(hard_linked));
  EXPECT_TRUE(std::filesystem::exists(hard_link_alias));
  EXPECT_TRUE(std::filesystem::exists(unrecognized));
  struct stat symlink_metadata{};
  ASSERT_EQ(::lstat(symlink_entry.c_str(), &symlink_metadata), 0);
  EXPECT_TRUE(S_ISLNK(symlink_metadata.st_mode));
  struct stat fifo_metadata{};
  ASSERT_EQ(::lstat(fifo_entry.c_str(), &fifo_metadata), 0);
  EXPECT_TRUE(S_ISFIFO(fifo_metadata.st_mode));
  EXPECT_FALSE(std::filesystem::exists(safe_alone));
  EXPECT_FALSE(std::filesystem::exists(safe_only));
  EXPECT_TRUE(std::filesystem::exists(mixed));
}

TEST(OutputStore, UnsafeBaseSymlinkAndModeAreRejectedWithoutTraversal) {
  StoreEnvironment environment;
  const std::filesystem::path target = environment.root_path() / "target";
  ASSERT_TRUE(std::filesystem::create_directory(target));
  ASSERT_EQ(::chmod(target.c_str(), 0700), 0);
  const std::filesystem::path marker = target / "marker";
  create_file(marker, 0600);
  ASSERT_EQ(::symlink(target.c_str(), environment.base_path().c_str()), 0);

  SequentialIds ids;
  OutputStore symlink_store({}, {}, [&] { return ids.next(); });
  OperationStatus symlink_status = symlink_store.start(
      environment.socket_path(), opaque_id(220), environment.lock_fd());
  EXPECT_FALSE(symlink_status.ok);
  EXPECT_EQ(symlink_status.code, kInternalErrorCode);
  EXPECT_TRUE(std::filesystem::exists(marker));

  ASSERT_EQ(::unlink(environment.base_path().c_str()), 0);
  ASSERT_TRUE(std::filesystem::create_directory(environment.base_path()));
  ASSERT_EQ(::chmod(environment.base_path().c_str(), 0755), 0);
  const std::filesystem::path unsafe_file = environment.base_path() / "keep";
  create_file(unsafe_file, 0600);
  OutputStore mode_store({}, {}, [&] { return ids.next(); });
  OperationStatus mode_status = mode_store.start(
      environment.socket_path(), opaque_id(221), environment.lock_fd());
  EXPECT_FALSE(mode_status.ok);
  EXPECT_EQ(mode_status.code, kInternalErrorCode);
  EXPECT_TRUE(std::filesystem::exists(unsafe_file));
}

TEST(OutputStore, ShutdownStopsLeasesButAllowsDrainingPublication) {
  StoreEnvironment environment;
  SequentialIds ids;
  OutputStore store({}, {}, [&] { return ids.next(); });
  ASSERT_TRUE(store
                  .start(environment.socket_path(), opaque_id(12),
                         environment.lock_fd())
                  .ok);
  store.stop_leases();

  ComputeOutputPublication publication = store.publish(
      ComputeRequestId{opaque_id(120)}, make_u8_image(1, 1, 1, 1, {8}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> rejected =
      store.acquire_delivery(publication.output.reference());
  EXPECT_FALSE(rejected.status.ok);
  EXPECT_EQ(rejected.status.code, kInternalErrorCode);
  publication.output.reset();
  store.shutdown();
  store.shutdown();
  EXPECT_EQ(store.artifact_count(), 0U);
  EXPECT_EQ(store.retained_bytes(), 0U);
}

TEST(OutputStore, ShutdownWaitsForLeaseUntilManualExpiryWakesIt) {
  StoreEnvironment environment;
  SequentialIds ids;
  ManualClock clock;
  OutputStoreLimits limits;
  limits.job_ttl = std::chrono::seconds(100);
  limits.delivery_ttl = std::chrono::seconds(10);
  OutputStore store(
      limits, [&] { return clock.now(); }, [&] { return ids.next(); });
  const std::string instance_id = opaque_id(13);
  ASSERT_TRUE(
      store.start(environment.socket_path(), instance_id, environment.lock_fd())
          .ok);
  const ComputeRequestId compute_id{opaque_id(130)};
  ComputeOutputPublication publication =
      store.publish(compute_id, make_u8_image(1, 1, 1, 1, {5}));
  ASSERT_TRUE(publication.status.ok);
  IpcResult<OutputArtifactDelivery> delivery =
      store.acquire_delivery(publication.output.reference());
  ASSERT_TRUE(delivery.status.ok);
  publication.output.reset();

  std::promise<void> entered;
  std::future<void> entered_future = entered.get_future();
  std::future<void> stopped = std::async(std::launch::async, [&] {
    entered.set_value();
    store.shutdown();
  });
  const std::future_status entered_status =
      entered_future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(entered_status, std::future_status::ready);
  EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  clock.advance(std::chrono::seconds(10));
  EXPECT_LE(store.cleanup_expired(), 1U);
  std::future_status stopped_status = stopped.wait_for(std::chrono::seconds(2));
  if (stopped_status != std::future_status::ready) {
    (void)store.release_orphaned_delivery(compute_id,
                                          delivery.value.delivery_id);
    clock.advance(std::chrono::seconds(100));
    (void)store.cleanup_expired();
    stopped_status = stopped.wait_for(std::chrono::seconds(2));
  }
  EXPECT_EQ(stopped_status, std::future_status::ready);
  stopped.get();
  EXPECT_FALSE(std::filesystem::exists(delivery.value.metadata.path));
  EXPECT_EQ(store.artifact_count(), 0U);

  ASSERT_TRUE(
      store.start(environment.socket_path(), instance_id, environment.lock_fd())
          .ok);
  store.shutdown();
}

}  // namespace
}  // namespace ps::ipc::internal
