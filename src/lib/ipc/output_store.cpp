#include "ipc/output_store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

#include "ipc/codec.hpp"

namespace ps::ipc::internal {
namespace {

/** @brief Prefix for one daemon-instance output directory. */
constexpr char kInstancePrefix[] = "instance-";

/** @brief Prefix for every controlled artifact or publication-stage file. */
constexpr char kOutputPrefix[] = "output-";

/** @brief Suffix for every controlled artifact or publication-stage file. */
constexpr char kOutputSuffix[] = ".bin";

/**
 * @brief Exact checked layout of one nonempty CPU image.
 * @throws Nothing.
 */
struct ImageLayout {
  /** @brief Tight bytes copied from every source row. */
  std::size_t row_bytes = 0;

  /** @brief Total tight bytes written to the artifact. */
  std::size_t total_bytes = 0;
};

/**
 * @brief Tri-state result of live ancestry or artifact identity validation.
 * @throws Nothing.
 */
enum class ArtifactValidation {
  /** @brief Every path and descriptor still matches the retained identity. */
  Valid,

  /** @brief A path is absent, replaced, unsafe, or identity-mismatched. */
  MissingOrMismatch,

  /** @brief A transient/resource/system error prevented a trustworthy check. */
  InternalError,
};

/**
 * @brief Minimal move-only POSIX descriptor owner.
 * @throws Nothing.
 */
class ScopedFd {
 public:
  /**
   * @brief Creates an empty descriptor owner.
   * @throws Nothing.
   */
  ScopedFd() noexcept = default;

  /**
   * @brief Takes ownership of one descriptor.
   * @param fd Descriptor value, or -1.
   * @throws Nothing.
   */
  explicit ScopedFd(int fd) noexcept : fd_(fd) {}

  /**
   * @brief Closes the owned descriptor when present.
   * @throws Nothing; close failures are ignored during ownership cleanup.
   */
  ~ScopedFd() noexcept { reset(); }

  /**
   * @brief Prevents descriptor duplication by copy.
   * @param other Owner that cannot be copied.
   * @throws Nothing because this operation is unavailable.
   */
  ScopedFd(const ScopedFd&) = delete;

  /**
   * @brief Prevents descriptor duplication by copy assignment.
   * @param other Owner that cannot be copied.
   * @return No value because copying is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ScopedFd& operator=(const ScopedFd&) = delete;

  /**
   * @brief Transfers descriptor ownership.
   * @param other Source made empty.
   * @throws Nothing.
   */
  ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}

  /**
   * @brief Closes current ownership and transfers another descriptor.
   * @param other Source made empty.
   * @return This descriptor owner.
   * @throws Nothing; close failures are ignored during replacement.
   */
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  /**
   * @brief Returns the borrowed descriptor value.
   * @return Descriptor or -1.
   * @throws Nothing.
   */
  int get() const noexcept { return fd_; }

  /**
   * @brief Reports whether a descriptor is owned.
   * @return True for a nonnegative descriptor.
   * @throws Nothing.
   */
  explicit operator bool() const noexcept { return fd_ >= 0; }

  /**
   * @brief Releases ownership without closing.
   * @return Previous descriptor or -1.
   * @throws Nothing.
   * @note The caller becomes responsible for the returned descriptor.
   */
  int release() noexcept {
    const int released = fd_;
    fd_ = -1;
    return released;
  }

  /**
   * @brief Closes current ownership and optionally adopts a replacement.
   * @param fd Replacement descriptor or -1.
   * @return Nothing.
   * @throws Nothing; close failures are ignored after retrying interruption.
   * @note Ownership transfers to this object only after the former descriptor
   *       has been closed or its close attempt has terminated.
   */
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      while (::close(fd_) != 0 && errno == EINTR) {
      }
    }
    fd_ = fd;
  }

 private:
  /** @brief Owned descriptor, or -1. */
  int fd_ = -1;
};

/**
 * @brief Retries an interrupted lstat operation.
 * @param path Path whose final component must not be followed.
 * @param metadata Receives one successful observation.
 * @return Zero on success, otherwise -1 with errno preserved.
 * @throws Nothing.
 */
int retry_lstat(const char* path, struct stat* metadata) noexcept {
  int result = -1;
  do {
    result = ::lstat(path, metadata);
  } while (result != 0 && errno == EINTR);
  return result;
}

/**
 * @brief Retries an interrupted fstat operation.
 * @param fd Borrowed open descriptor.
 * @param metadata Receives one successful observation.
 * @return Zero on success, otherwise -1 with errno preserved.
 * @throws Nothing.
 */
int retry_fstat(int fd, struct stat* metadata) noexcept {
  int result = -1;
  do {
    result = ::fstat(fd, metadata);
  } while (result != 0 && errno == EINTR);
  return result;
}

/**
 * @brief Retries an interrupted no-follow fstatat operation.
 * @param directory_fd Borrowed parent directory descriptor.
 * @param name Relative basename.
 * @param metadata Receives one successful observation.
 * @return Zero on success, otherwise -1 with errno preserved.
 * @throws Nothing.
 */
int retry_fstatat(int directory_fd, const char* name,
                  struct stat* metadata) noexcept {
  int result = -1;
  do {
    result = ::fstatat(directory_fd, name, metadata, AT_SYMLINK_NOFOLLOW);
  } while (result != 0 && errno == EINTR);
  return result;
}

/**
 * @brief Opens an untrusted artifact path without blocking or following links.
 * @param directory_fd Borrowed instance directory descriptor.
 * @param name Controlled relative basename.
 * @return Descriptor or -1 with errno preserved after retrying EINTR.
 * @throws Nothing.
 */
int retry_open_artifact(int directory_fd, const char* name) noexcept {
  int descriptor = -1;
  do {
    descriptor = ::openat(directory_fd, name,
                          O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  } while (descriptor < 0 && errno == EINTR);
  return descriptor;
}

/**
 * @brief Classifies pathname errors that prove absence or unsafe replacement.
 * @param error Captured errno from one path operation.
 * @return True for absence, non-directory ancestry, or rejected symlink.
 * @throws Nothing.
 */
bool path_mismatch_error(int error) noexcept {
  return error == ENOENT || error == ENOTDIR || error == ELOOP;
}

/**
 * @brief Classifies final artifact-entry absence or symlink replacement.
 * @param error Captured errno from an instance-relative artifact operation.
 * @return True for an absent or rejected symlink basename.
 * @throws Nothing.
 * @note `ENOTDIR` is excluded because a controlled basename has no separator;
 *       it therefore indicates an unexpected instance-fd failure.
 */
bool artifact_mismatch_error(int error) noexcept {
  return error == ENOENT || error == ELOOP;
}

/**
 * @brief Builds a daemon internal output-store failure.
 * @param message Owned diagnostic.
 * @return Stable daemon `internal_error` status.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 */
OperationStatus internal_status(std::string message) {
  return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                        "internal_error", std::move(message));
}

/**
 * @brief Builds the transactional output quota failure.
 * @return Stable daemon `artifact_limit_exceeded` status.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 */
OperationStatus artifact_limit_status() {
  return failure_status(OperationErrorDomain::Daemon,
                        kArtifactLimitExceededCode, "artifact_limit_exceeded",
                        "output artifact quota is exhausted");
}

/**
 * @brief Builds the exact result-time output lookup failure.
 * @return Stable daemon `artifact_not_found` status.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 */
OperationStatus artifact_not_found_status() {
  return failure_status(OperationErrorDomain::Daemon, kArtifactNotFoundCode,
                        "artifact_not_found",
                        "compute output artifact was not found");
}

/**
 * @brief Adds the current errno description to one operation label.
 * @param operation Human-readable failing operation.
 * @return Owned diagnostic containing the errno text.
 * @throws std::bad_alloc if result storage cannot be allocated.
 */
std::string errno_message(const std::string& operation) {
  return operation + ": " + std::strerror(errno);
}

/**
 * @brief Reports whether all permission and special bits exactly match.
 * @param metadata Filesystem metadata to inspect.
 * @param expected Expected low permission bits.
 * @return True only for the exact requested mode.
 * @throws Nothing.
 */
bool exact_mode(const struct stat& metadata, mode_t expected) noexcept {
  constexpr mode_t kComparedBits =
      S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
  return (metadata.st_mode & kComparedBits) == expected;
}

/**
 * @brief Reports whether two observations identify the same filesystem entry.
 * @param left First metadata observation.
 * @param right Second metadata observation.
 * @return True for equal device and inode.
 * @throws Nothing.
 */
bool same_identity(const struct stat& left, const struct stat& right) noexcept {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

/**
 * @brief Validates one private directory observation.
 * @param metadata Metadata obtained without following symlinks.
 * @param expected_device Required filesystem device, or zero for any device.
 * @return True for a same-owner exact-0700 real directory.
 * @throws Nothing.
 */
bool safe_directory(const struct stat& metadata,
                    dev_t expected_device = 0) noexcept {
  return S_ISDIR(metadata.st_mode) && metadata.st_uid == ::geteuid() &&
         exact_mode(metadata, 0700) &&
         (expected_device == 0 || metadata.st_dev == expected_device);
}

/**
 * @brief Validates the already-protected socket parent directory.
 * @param metadata Metadata obtained from both path and descriptor.
 * @return True for a same-owner real directory with no group/other access.
 * @throws Nothing.
 */
bool safe_socket_parent(const struct stat& metadata) noexcept {
  return S_ISDIR(metadata.st_mode) && metadata.st_uid == ::geteuid() &&
         (metadata.st_mode & 0077) == 0;
}

/**
 * @brief Validates one private artifact observation.
 * @param metadata Metadata obtained without following symlinks.
 * @param expected_device Filesystem device of the owning instance directory.
 * @return True for a same-owner exact-0600 one-link regular file.
 * @throws Nothing.
 */
bool safe_artifact(const struct stat& metadata,
                   dev_t expected_device) noexcept {
  return S_ISREG(metadata.st_mode) && metadata.st_uid == ::geteuid() &&
         exact_mode(metadata, 0600) && metadata.st_nlink == 1 &&
         metadata.st_dev == expected_device && metadata.st_size >= 0;
}

/**
 * @brief Reports whether text is a recognized instance directory name.
 * @param name Candidate basename.
 * @return True only for `instance-<32-lowercase-hex>`.
 * @throws std::bad_alloc only if substring storage cannot be allocated.
 */
bool recognized_instance_name(const std::string& name) {
  const std::string prefix(kInstancePrefix);
  return name.size() == prefix.size() + 32 &&
         name.compare(0, prefix.size(), prefix) == 0 &&
         valid_opaque_id(name.substr(prefix.size()));
}

/**
 * @brief Reports whether text is a controlled artifact basename.
 * @param name Candidate basename.
 * @return True only for `output-<32-lowercase-hex>.bin`.
 * @throws std::bad_alloc only if substring storage cannot be allocated.
 */
bool recognized_output_name(const std::string& name) {
  const std::string prefix(kOutputPrefix);
  const std::string suffix(kOutputSuffix);
  if (name.size() != prefix.size() + 32 + suffix.size() ||
      name.compare(0, prefix.size(), prefix) != 0 ||
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  return valid_opaque_id(name.substr(prefix.size(), 32));
}

/**
 * @brief Creates a controlled artifact basename from an opaque identity.
 * @param opaque_id Valid 32-lowercase-hex identity.
 * @return `output-<opaque_id>.bin`.
 * @throws std::bad_alloc if result storage cannot be allocated.
 */
std::string output_filename(const std::string& opaque_id) {
  return std::string(kOutputPrefix) + opaque_id + kOutputSuffix;
}

/**
 * @brief Duplicates a descriptor with close-on-exec enabled.
 * @param fd Borrowed source descriptor.
 * @return Owned duplicate or an empty owner.
 * @throws Nothing.
 */
ScopedFd duplicate_cloexec(int fd) noexcept {
#if defined(F_DUPFD_CLOEXEC)
  return ScopedFd(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
#else
  const int duplicate = ::dup(fd);
  if (duplicate >= 0) {
    (void)::fcntl(duplicate, F_SETFD, FD_CLOEXEC);
  }
  return ScopedFd(duplicate);
#endif
}

/**
 * @brief Lists current basenames through one borrowed directory descriptor.
 * @param directory_fd Open real directory descriptor.
 * @return Owned entry names excluding dot entries.
 * @throws std::runtime_error if duplication or enumeration fails.
 * @throws std::bad_alloc if result storage cannot be allocated.
 */
std::vector<std::string> list_directory(int directory_fd) {
  ScopedFd duplicate = duplicate_cloexec(directory_fd);
  if (!duplicate) {
    throw std::runtime_error(
        errno_message("cannot duplicate output directory"));
  }
  DIR* stream = ::fdopendir(duplicate.release());
  if (stream == nullptr) {
    throw std::runtime_error(
        errno_message("cannot enumerate output directory"));
  }
  std::unique_ptr<DIR, decltype(&::closedir)> owner(stream, &::closedir);
  std::vector<std::string> names;
  errno = 0;
  while (dirent* entry = ::readdir(stream)) {
    const std::string name(entry->d_name);
    if (name != "." && name != "..") {
      names.push_back(name);
    }
    errno = 0;
  }
  if (errno != 0) {
    throw std::runtime_error(errno_message("cannot read output directory"));
  }
  return names;
}

/**
 * @brief Checks whether an ImageBuffer type is one of the six public values.
 * @param type Candidate enum value.
 * @return True for a defined DataType enumerator.
 * @throws Nothing.
 */
bool valid_data_type(DataType type) noexcept {
  switch (type) {
    case DataType::UINT8:
    case DataType::INT8:
    case DataType::UINT16:
    case DataType::INT16:
    case DataType::FLOAT32:
    case DataType::FLOAT64:
      return true;
  }
  return false;
}

/**
 * @brief Reports whether an image is the canonical empty CPU descriptor.
 * @param image Image returned by Host.
 * @return True only when dimensions, step, payload, context, and device are
 *         empty/default-safe.
 * @throws Nothing.
 */
bool canonical_empty_image(const ImageBuffer& image) noexcept {
  return image.width == 0 && image.height == 0 && image.channels == 0 &&
         image.step == 0 && image.data == nullptr && image.context == nullptr &&
         image.device == Device::CPU && valid_data_type(image.type);
}

/**
 * @brief Validates and computes the exact tight-row image layout.
 * @param image Nonempty Host image candidate.
 * @return Checked row and total byte counts.
 * @throws std::invalid_argument for malformed geometry, type, device, payload,
 *         or step.
 * @throws std::overflow_error when multiplication exceeds size_t.
 */
ImageLayout checked_image_layout(const ImageBuffer& image) {
  if (image.width <= 0 || image.height <= 0 || image.channels <= 0 ||
      image.data == nullptr || image.device != Device::CPU ||
      !valid_data_type(image.type)) {
    throw std::invalid_argument(
        "compute image is not a valid nonempty CPU image");
  }
  const std::size_t width = static_cast<std::size_t>(image.width);
  const std::size_t height = static_cast<std::size_t>(image.height);
  const std::size_t channels = static_cast<std::size_t>(image.channels);
  const std::size_t channel_bytes = image_buffer_bytes_per_channel(image.type);
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (width > maximum / channels ||
      width * channels > maximum / channel_bytes) {
    throw std::overflow_error("compute image row byte count overflowed");
  }
  const std::size_t row_bytes = width * channels * channel_bytes;
  if (row_bytes == 0 || image.step < row_bytes) {
    throw std::invalid_argument(
        "compute image row step is smaller than tight rows");
  }
  if (height > maximum / row_bytes) {
    throw std::overflow_error("compute image artifact byte count overflowed");
  }
  if (height > 1 && image.step > (maximum - row_bytes) / (height - 1)) {
    throw std::overflow_error("compute image source row offset overflowed");
  }
  return {row_bytes, height * row_bytes};
}

/**
 * @brief Writes every requested byte while retrying interrupted calls.
 * @param fd Open writable descriptor.
 * @param data Borrowed byte range.
 * @param size Exact byte count.
 * @return Nothing.
 * @throws std::runtime_error on zero progress or a non-interrupted error.
 * @note The helper neither closes nor seeks `fd`; callers retain ownership and
 *       serialize writes to the artifact.
 */
void write_all(int fd, const std::uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const std::size_t remaining = size - written;
    const std::size_t request =
        std::min(remaining, static_cast<std::size_t>(SSIZE_MAX));
    const ssize_t result = ::write(fd, data + written, request);
    if (result > 0) {
      written += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error(errno_message("cannot write output artifact"));
  }
}

/**
 * @brief Atomically renames a complete stage entry without replacement.
 * @param directory_fd Directory containing both basenames.
 * @param source Existing controlled stage basename.
 * @param destination Absent controlled published basename.
 * @return True on atomic publication; false with errno preserved on failure.
 * @throws Nothing.
 */
bool rename_noreplace(int directory_fd, const std::string& source,
                      const std::string& destination) noexcept {
#if defined(__APPLE__)
  return ::renameatx_np(directory_fd, source.c_str(), directory_fd,
                        destination.c_str(), RENAME_EXCL) == 0;
#elif defined(__linux__)
  return ::syscall(SYS_renameat2, directory_fd, source.c_str(), directory_fd,
                   destination.c_str(), RENAME_NOREPLACE) == 0;
#else
  if (::linkat(directory_fd, source.c_str(), directory_fd, destination.c_str(),
               0) != 0) {
    return false;
  }
  if (::unlinkat(directory_fd, source.c_str(), 0) == 0) {
    return true;
  }
  const int failure = errno;
  (void)::unlinkat(directory_fd, destination.c_str(), 0);
  errno = failure;
  return false;
#endif
}

/**
 * @brief Adds a positive duration while saturating at TimePoint::max().
 * @param now Current monotonic time.
 * @param duration Positive configured TTL.
 * @return Saturated future deadline.
 * @throws Nothing.
 */
OutputStore::TimePoint saturating_deadline(
    OutputStore::TimePoint now,
    std::chrono::steady_clock::duration duration) noexcept {
  const OutputStore::TimePoint maximum = OutputStore::TimePoint::max();
  if (maximum - now < duration) {
    return maximum;
  }
  return now + duration;
}

}  // namespace

/**
 * @brief POSIX implementation of OutputStore's secure lifecycle and quotas.
 * @throws Whatever explicitly documented public construction/start paths throw.
 */
class OutputStore::Impl {
 public:
  /**
   * @brief Complete retained artifact record.
   * @throws std::bad_alloc when owned strings or metadata are constructed.
   */
  struct Record {
    /** @brief Original accepted image-job identity. */
    std::string compute_id;

    /** @brief Revalidated private delivery metadata without pixel bytes. */
    OutputArtifactMetadata metadata;

    /** @brief Controlled basename used for dirfd validation and cleanup. */
    std::string filename;

    /** @brief Stable sole delivery identity generated at publication. */
    std::string delivery_id;

    /** @brief Whether terminal-job ownership still protects this record. */
    bool job_owned = true;

    /** @brief Defensive store-side job ownership expiry. */
    TimePoint job_expiry{};

    /** @brief Whether one result/open lease currently protects this record. */
    bool lease_active = false;

    /** @brief Refreshed lease deadline when lease_active is true. */
    TimePoint lease_expiry{};
  };

  /**
   * @brief Internal publication outcome before cleanup ownership construction.
   * @throws std::bad_alloc when status or output identity is copied.
   */
  struct RawPublication {
    /** @brief Exact nested publication status. */
    OperationStatus status;

    /** @brief Published output identity, empty for empty/failure outcomes. */
    std::string output_id;
  };

 private:
  /**
   * @brief Transaction-local startup paths, descriptors, and stable identity.
   * @throws Nothing after construction.
   * @note Assembly helpers may allocate owned paths. Descriptors remain owned
   *       here until commit; rollback removes only entries this candidate
   *       created.
   */
  struct StartCandidate {
    /** @brief Absolute protected socket-parent path. */
    std::string parent_path;

    /** @brief Socket-specific output-base basename. */
    std::string base_name;

    /** @brief Absolute socket-specific output-base path. */
    std::string base_path;

    /** @brief Controlled current-instance basename. */
    std::string instance_name;

    /** @brief Candidate socket-parent descriptor. */
    ScopedFd parent_fd;

    /** @brief Candidate output-base descriptor. */
    ScopedFd base_fd;

    /** @brief Candidate current-instance descriptor. */
    ScopedFd instance_fd;

    /** @brief Stable socket-parent descriptor identity. */
    struct stat parent_identity{};

    /** @brief Stable output-base descriptor identity. */
    struct stat base_identity{};

    /** @brief Stable current-instance descriptor identity. */
    struct stat instance_identity{};

    /** @brief Whether this transaction created the output base. */
    bool base_created = false;

    /** @brief Whether this transaction created the instance directory. */
    bool instance_created = false;
  };

  /**
   * @brief Transaction-local artifact publication ownership and rollback state.
   * @throws Nothing after construction.
   * @note Caller holds `mutex_`; assembly helpers may allocate identities,
   *       names, or a record. The open stage descriptor remains owned until
   *       this candidate is destroyed after commit or rollback.
   */
  struct PublicationCandidate {
    /** @brief New opaque artifact identity. */
    std::string output_id;

    /** @brief Stable delivery identity paired with the artifact. */
    std::string delivery_id;

    /** @brief Controlled final artifact basename. */
    std::string final_name;

    /** @brief Controlled exclusive publication-stage basename. */
    std::string stage_name;

    /** @brief Complete record transferred into `records_` only at commit. */
    std::unique_ptr<Record> record;

    /** @brief Open descriptor retaining the created inode identity. */
    ScopedFd artifact_fd;

    /** @brief Latest verified metadata for the created inode. */
    struct stat owned_identity{};

    /** @brief Whether the controlled stage inode exists. */
    bool stage_created = false;

    /** @brief Whether the stage name was atomically changed to final name. */
    bool renamed = false;

    /** @brief Whether the compute lookup was inserted and needs rollback. */
    bool compute_inserted = false;

    /** @brief Whether the delivery lookup was inserted and needs rollback. */
    bool delivery_inserted = false;
  };

 public:
  /**
   * @brief Stores validated constructor dependencies.
   * @param limits Positive quota and TTL policy.
   * @param clock Nonempty monotonic clock.
   * @param id_generator Nonempty opaque-id source.
   * @throws std::invalid_argument for invalid policy/callbacks.
   * @throws std::bad_alloc when callback storage cannot be allocated.
   */
  Impl(OutputStoreLimits limits, Clock clock, IdGenerator id_generator)
      : limits_(limits),
        clock_(std::move(clock)),
        id_generator_(std::move(id_generator)) {
    if (limits_.artifacts == 0 || limits_.total_bytes == 0 ||
        limits_.artifact_bytes == 0 ||
        limits_.artifact_bytes > limits_.total_bytes ||
        limits_.job_ttl <= std::chrono::steady_clock::duration::zero() ||
        limits_.delivery_ttl <= std::chrono::steady_clock::duration::zero()) {
      throw std::invalid_argument(
          "output store quotas and TTLs must be positive");
    }
    if (!clock_ || !id_generator_) {
      throw std::invalid_argument("output store callbacks must be nonempty");
    }
  }

  /**
   * @brief Starts directory ownership under an already-held lifecycle lock.
   * @param socket_path Absolute bound socket path.
   * @param server_instance_id Valid daemon instance id.
   * @param lifecycle_lock_fd Borrowed live lifecycle-lock descriptor.
   * @return Nothing.
   * @throws std::invalid_argument for malformed arguments.
   * @throws std::runtime_error for unsafe or unavailable filesystem state.
   * @throws std::bad_alloc if path/enumeration storage cannot be allocated.
   * @note The store mutex serializes the full filesystem transaction. The
   *       lifecycle descriptor remains borrowed; newly opened directory
   *       descriptors transfer to the store only after complete validation.
   */
  void start(const std::string& socket_path,
             const std::string& server_instance_id, int lifecycle_lock_fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    validate_start_locked(socket_path, server_instance_id, lifecycle_lock_fd);
    StartCandidate candidate =
        make_start_candidate(socket_path, server_instance_id);
    try {
      open_start_parent_locked(&candidate);
      open_start_base_locked(&candidate);
      cleanup_stale_instances(candidate.base_fd.get(), candidate.base_identity);
      open_start_instance_locked(&candidate);
      commit_start_locked(&candidate);
    } catch (...) {
      rollback_start_locked(&candidate);
      throw;
    }
  }

  /**
   * @brief Removes only safe recognized stale contents below a validated base.
   * @param base_fd Borrowed same-owner real base directory descriptor.
   * @param base_metadata Stable base identity.
   * @return Nothing.
   * @throws std::runtime_error if enumeration itself fails.
   * @throws std::bad_alloc if entry storage cannot be allocated.
   * @note Unsafe/unrecognized entries are left untouched and simply prevent
   *       their containing instance directory from being removed as empty.
   *       The caller holds `mutex_` and retains descriptor ownership.
   */
  void cleanup_stale_instances(int base_fd, const struct stat& base_metadata) {
    for (const std::string& instance_name : list_directory(base_fd)) {
      if (!recognized_instance_name(instance_name)) {
        continue;
      }
      struct stat instance_path{};
      if (::fstatat(base_fd, instance_name.c_str(), &instance_path,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
          !safe_directory(instance_path, base_metadata.st_dev)) {
        continue;
      }
      ScopedFd instance_fd(
          ::openat(base_fd, instance_name.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      struct stat instance_descriptor{};
      if (!instance_fd ||
          ::fstat(instance_fd.get(), &instance_descriptor) != 0 ||
          !safe_directory(instance_descriptor, base_metadata.st_dev) ||
          !same_identity(instance_path, instance_descriptor)) {
        continue;
      }
      for (const std::string& entry_name : list_directory(instance_fd.get())) {
        if (!recognized_output_name(entry_name)) {
          continue;
        }
        struct stat first{};
        struct stat current{};
        if (::fstatat(instance_fd.get(), entry_name.c_str(), &first,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !safe_artifact(first, instance_descriptor.st_dev)) {
          continue;
        }
        ScopedFd artifact_fd(
            ::openat(instance_fd.get(), entry_name.c_str(),
                     O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        struct stat descriptor_metadata{};
        if (!artifact_fd ||
            ::fstat(artifact_fd.get(), &descriptor_metadata) != 0 ||
            !safe_artifact(descriptor_metadata, instance_descriptor.st_dev) ||
            !same_identity(first, descriptor_metadata) ||
            ::fstatat(instance_fd.get(), entry_name.c_str(), &current,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !safe_artifact(current, instance_descriptor.st_dev) ||
            !same_identity(descriptor_metadata, current) ||
            descriptor_metadata.st_size != current.st_size) {
          continue;
        }
        (void)::unlinkat(instance_fd.get(), entry_name.c_str(), 0);
      }
      struct stat final_path{};
      struct stat final_descriptor{};
      if (::fstat(instance_fd.get(), &final_descriptor) == 0 &&
          ::fstatat(base_fd, instance_name.c_str(), &final_path,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
          safe_directory(final_descriptor, base_metadata.st_dev) &&
          safe_directory(final_path, base_metadata.st_dev) &&
          same_identity(instance_descriptor, final_descriptor) &&
          same_identity(final_descriptor, final_path)) {
        (void)::unlinkat(base_fd, instance_name.c_str(), AT_REMOVEDIR);
      }
      instance_fd.reset();
    }
  }

  /**
   * @brief Publishes an empty or validated nonempty image transactionally.
   * @param compute_id Accepted image-job identity.
   * @param image Exact Host image result.
   * @return Nested publication status plus output id on nonempty success.
   * @throws std::invalid_argument for a malformed compute id or image layout.
   * @throws std::overflow_error when image layout arithmetic overflows.
   * @throws std::runtime_error for lifecycle, duplicate-job, or filesystem
   *         failures.
   * @throws std::bad_alloc for allocation failure before/after file creation;
   *         the file, record, and quota are rolled back before propagation.
   * @throws Whatever the injected clock or id generator throws.
   * @note Image bytes remain borrowed for this call. `mutex_` serializes
   *       admission, identity generation, file publication, record commit, and
   *       rollback so quota and ownership become visible atomically.
   */
  RawPublication publish_raw(const ComputeRequestId& compute_id,
                             const ImageBuffer& image) {
    if (!valid_opaque_id(compute_id.value)) {
      throw std::invalid_argument(
          "output publication requires a valid compute id");
    }
    if (canonical_empty_image(image)) {
      return {ok_status(), {}};
    }
    const ImageLayout layout = checked_image_layout(image);
    if (layout.total_bytes > limits_.artifact_bytes) {
      return {artifact_limit_status(), {}};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!admit_publication_locked(compute_id, layout)) {
      return {artifact_limit_status(), {}};
    }

    PublicationCandidate candidate =
        make_publication_candidate_locked(compute_id, image, layout);
    RawPublication successful{ok_status(), candidate.output_id};
    try {
      create_and_write_stage_locked(image, layout, &candidate);
      publish_stage_locked(layout, &candidate);
      commit_publication_locked(compute_id, layout, &candidate);
      return successful;
    } catch (...) {
      rollback_publication_locked(compute_id, &candidate);
      throw;
    }
  }

  /**
   * @brief Acquires one stable refreshed delivery under the store lock.
   * @param output_id Private terminal-job output reference.
   * @return Revalidated delivery or exact lookup/lifecycle failure.
   * @throws std::bad_alloc if copied output storage cannot be allocated.
   */
  IpcResult<OutputArtifactDelivery> acquire_delivery(
      const std::string& output_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !leases_open_) {
      return {internal_status("output store is not accepting delivery leases"),
              {}};
    }
    TimePoint now;
    try {
      now = clock_();
    } catch (const std::exception& error) {
      return {internal_status(error.what()), {}};
    } catch (...) {
      return {internal_status("output-store clock failed"), {}};
    }
    (void)expire_locked(now);
    const auto found = records_.find(output_id);
    if (found == records_.end() || !found->second->job_owned) {
      return {artifact_not_found_status(), {}};
    }
    const ArtifactValidation validation =
        validate_record_locked(*found->second);
    if (validation == ArtifactValidation::InternalError) {
      return {internal_status("cannot validate compute output artifact"), {}};
    }
    if (validation == ArtifactValidation::MissingOrMismatch) {
      found->second->job_owned = false;
      if (!found->second->lease_active) {
        erase_record_locked(found);
      }
      return {artifact_not_found_status(), {}};
    }

    OutputArtifactDelivery delivery;
    delivery.metadata = found->second->metadata;
    delivery.delivery_id = found->second->delivery_id;
    TimePoint refresh_time;
    try {
      refresh_time = clock_();
    } catch (const std::exception& error) {
      return {internal_status(error.what()), {}};
    } catch (...) {
      return {internal_status("output-store clock failed"), {}};
    }
    found->second->lease_expiry =
        saturating_deadline(refresh_time, limits_.delivery_ttl);
    found->second->lease_active = true;
    return {ok_status(), std::move(delivery)};
  }

  /**
   * @brief Releases one job owner and optional matching lease atomically.
   * @param output_id Store output identity.
   * @param delivery_id Optional matching stable lease identity.
   * @return True when a retained record existed.
   * @throws Nothing.
   */
  bool release_job(const std::string& output_id,
                   const std::optional<std::string>& delivery_id) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      expire_now_best_effort_locked();
      const auto found = records_.find(output_id);
      if (found == records_.end()) {
        return false;
      }
      Record& record = *found->second;
      record.job_owned = false;
      if (delivery_id.has_value() && record.lease_active &&
          *delivery_id == record.delivery_id) {
        record.lease_active = false;
      }
      if (!record.lease_active) {
        erase_record_locked(found);
      }
      cv_.notify_all();
      return true;
    } catch (...) {
      return false;
    }
  }

  /**
   * @brief Releases an active orphaned lease only for its original job pair.
   * @param compute_id Original image-job identity.
   * @param delivery_id Stable delivery identity.
   * @return True only when an active matching lease was released.
   * @throws Nothing.
   */
  bool release_orphaned_delivery(const ComputeRequestId& compute_id,
                                 const std::string& delivery_id) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      expire_now_best_effort_locked();
      const auto delivery = delivery_to_output_.find(delivery_id);
      if (delivery == delivery_to_output_.end()) {
        return false;
      }
      const auto found = records_.find(delivery->second);
      if (found == records_.end() || !found->second->lease_active ||
          found->second->compute_id != compute_id.value) {
        return false;
      }
      found->second->lease_active = false;
      if (!found->second->job_owned) {
        erase_record_locked(found);
      }
      cv_.notify_all();
      return true;
    } catch (...) {
      return false;
    }
  }

  /**
   * @brief Expires store-side job owners and delivery leases at one timestamp.
   * @return Number of active delivery leases removed.
   * @throws Nothing.
   */
  std::size_t cleanup_expired() noexcept {
    try {
      const TimePoint now = clock_();
      std::lock_guard<std::mutex> lock(mutex_);
      const std::size_t expired = expire_locked(now);
      cv_.notify_all();
      return expired;
    } catch (...) {
      return 0;
    }
  }

  /**
   * @brief Stops new lease creation while preserving drained publication.
   * @return Nothing.
   * @throws Nothing.
   * @note Acquires `mutex_`, changes no artifact ownership, and wakes any
   *       shutdown waiter so it can re-evaluate lease state.
   */
  void stop_leases() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    leases_open_ = false;
    cv_.notify_all();
  }

  /**
   * @brief Performs idempotent lease-aware descriptor and directory shutdown.
   * @return Nothing.
   * @throws Nothing.
   * @note One caller owns shutdown under `mutex_`; concurrent callers wait for
   *       that owner. Job ownership is removed before active leases drain, and
   *       directory descriptors close exactly once at the final commit point.
   */
  void shutdown() noexcept {
    try {
      std::unique_lock<std::mutex> lock(mutex_);
      if (shutdown_in_progress_) {
        shutdown_cv_.wait(lock, [this] { return !shutdown_in_progress_; });
        return;
      }
      if (!running_) {
        return;
      }
      begin_shutdown_locked();
      drain_shutdown_records_locked(&lock);
      remove_instance_directory_locked();
      finish_shutdown_locked();
      lock.unlock();
      shutdown_cv_.notify_all();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      fail_shutdown_locked();
      shutdown_cv_.notify_all();
    }
  }

  /**
   * @brief Returns the current retained-record count.
   * @return Quota-accounted artifacts.
   * @throws Nothing.
   */
  std::size_t artifact_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
  }

  /**
   * @brief Returns the current retained tight-row byte total.
   * @return Quota-accounted bytes.
   * @throws Nothing.
   */
  std::size_t retained_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return retained_bytes_;
  }

 private:
  /**
   * @brief Validates lifecycle, lock-file identity, and empty retained state.
   * @param socket_path Candidate absolute socket path.
   * @param server_instance_id Candidate opaque daemon instance id.
   * @param lifecycle_lock_fd Borrowed lifecycle-lock descriptor.
   * @return Nothing.
   * @throws std::invalid_argument for malformed input or an unsafe lock file.
   * @throws std::runtime_error for active lifecycle or retained state.
   * @note Caller holds `mutex_`; the descriptor remains borrowed and no store
   *       filesystem path is changed by validation.
   */
  void validate_start_locked(const std::string& socket_path,
                             const std::string& server_instance_id,
                             int lifecycle_lock_fd) const {
    if (running_ || shutdown_in_progress_) {
      throw std::runtime_error(
          "output store is already running or shutting down");
    }
    if (socket_path.empty() || socket_path.front() != '/' ||
        !valid_opaque_id(server_instance_id) || lifecycle_lock_fd < 0 ||
        ::fcntl(lifecycle_lock_fd, F_GETFD) < 0) {
      throw std::invalid_argument(
          "output store requires absolute socket, valid instance, and lock fd");
    }
    struct stat lifecycle_lock_metadata{};
    if (retry_fstat(lifecycle_lock_fd, &lifecycle_lock_metadata) != 0 ||
        !S_ISREG(lifecycle_lock_metadata.st_mode) ||
        lifecycle_lock_metadata.st_uid != ::geteuid() ||
        !exact_mode(lifecycle_lock_metadata, 0600) ||
        lifecycle_lock_metadata.st_nlink != 1) {
      throw std::invalid_argument(
          "output store lifecycle lock is not a stable private file");
    }
    if (!records_.empty() || !compute_to_output_.empty() ||
        !delivery_to_output_.empty() || retained_bytes_ != 0) {
      throw std::runtime_error("output store restart found retained state");
    }
  }

  /**
   * @brief Builds controlled startup paths before any filesystem mutation.
   * @param socket_path Validated absolute socket path.
   * @param server_instance_id Validated daemon instance id.
   * @return Candidate containing owned paths and empty descriptor owners.
   * @throws std::invalid_argument when the socket has no basename.
   * @throws std::bad_alloc when path storage cannot be allocated.
   * @note Caller holds `mutex_`; the returned candidate owns all later
   *       transaction-local descriptors until commit.
   */
  StartCandidate make_start_candidate(
      const std::string& socket_path,
      const std::string& server_instance_id) const {
    const std::size_t separator = socket_path.find_last_of('/');
    if (separator == std::string::npos || separator + 1 >= socket_path.size()) {
      throw std::invalid_argument("output store socket path has no basename");
    }
    StartCandidate candidate;
    candidate.parent_path =
        separator == 0 ? "/" : socket_path.substr(0, separator);
    candidate.base_name = socket_path.substr(separator + 1) + ".outputs";
    candidate.base_path =
        candidate.parent_path == "/"
            ? candidate.parent_path + candidate.base_name
            : candidate.parent_path + "/" + candidate.base_name;
    candidate.instance_name = std::string(kInstancePrefix) + server_instance_id;
    return candidate;
  }

  /**
   * @brief Opens and validates the protected socket-parent directory.
   * @param candidate Startup transaction receiving descriptor and identity.
   * @return Nothing.
   * @throws std::runtime_error when the path and descriptor are not the same
   *         protected real directory.
   * @note Caller holds `mutex_`; `candidate` retains sole ownership of the new
   *       descriptor and no filesystem entry is created.
   */
  void open_start_parent_locked(StartCandidate* candidate) const {
    candidate->parent_fd.reset(
        ::open(candidate->parent_path.c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat parent_path_metadata{};
    if (!candidate->parent_fd ||
        ::lstat(candidate->parent_path.c_str(), &parent_path_metadata) != 0 ||
        ::fstat(candidate->parent_fd.get(), &candidate->parent_identity) != 0 ||
        !safe_socket_parent(parent_path_metadata) ||
        !safe_socket_parent(candidate->parent_identity) ||
        !same_identity(parent_path_metadata, candidate->parent_identity)) {
      throw std::runtime_error(
          "output-store socket parent is not a protected real directory");
    }
  }

  /**
   * @brief Opens or creates and then identity-validates the private output
   * base.
   * @param candidate Startup transaction with a validated parent descriptor.
   * @return Nothing.
   * @throws std::runtime_error for inspection, creation, protection, open, or
   *         identity failures.
   * @throws std::bad_alloc if an errno diagnostic cannot be allocated.
   * @note Caller holds `mutex_`; a newly created base is recorded for exact
   *       rollback, while its descriptor remains owned by `candidate`.
   */
  void open_start_base_locked(StartCandidate* candidate) const {
    struct stat path_metadata{};
    if (::fstatat(candidate->parent_fd.get(), candidate->base_name.c_str(),
                  &path_metadata, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT) {
        throw std::runtime_error(
            errno_message("cannot inspect output-store base"));
      }
      if (::mkdirat(candidate->parent_fd.get(), candidate->base_name.c_str(),
                    0700) != 0) {
        throw std::runtime_error(
            errno_message("cannot create output-store base"));
      }
      candidate->base_created = true;
    }
    candidate->base_fd.reset(
        ::openat(candidate->parent_fd.get(), candidate->base_name.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!candidate->base_fd) {
      throw std::runtime_error(errno_message("cannot open output-store base"));
    }
    if (candidate->base_created &&
        ::fchmod(candidate->base_fd.get(), 0700) != 0) {
      throw std::runtime_error(
          errno_message("cannot protect output-store base"));
    }
    struct stat base_path_metadata{};
    if (::fstat(candidate->base_fd.get(), &candidate->base_identity) != 0 ||
        ::fstatat(candidate->parent_fd.get(), candidate->base_name.c_str(),
                  &base_path_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        !safe_directory(candidate->base_identity) ||
        !safe_directory(base_path_metadata) ||
        !same_identity(candidate->base_identity, base_path_metadata)) {
      throw std::runtime_error(
          "output-store base is not a same-owner exact-0700 real directory");
    }
  }

  /**
   * @brief Creates, protects, and identity-validates the current instance.
   * @param candidate Startup transaction with a validated base descriptor.
   * @return Nothing.
   * @throws std::runtime_error for creation, protection, open, or identity
   *         failures.
   * @throws std::bad_alloc if an errno diagnostic cannot be allocated.
   * @note Caller holds `mutex_`; the created directory and descriptor remain
   *       candidate-owned until commit or exact rollback.
   */
  void open_start_instance_locked(StartCandidate* candidate) const {
    if (::mkdirat(candidate->base_fd.get(), candidate->instance_name.c_str(),
                  0700) != 0) {
      throw std::runtime_error(
          errno_message("cannot create output-store instance"));
    }
    candidate->instance_created = true;
    candidate->instance_fd.reset(
        ::openat(candidate->base_fd.get(), candidate->instance_name.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!candidate->instance_fd ||
        ::fchmod(candidate->instance_fd.get(), 0700) != 0) {
      throw std::runtime_error(
          errno_message("cannot open/protect output-store instance"));
    }
    struct stat instance_path_metadata{};
    if (::fstatat(candidate->base_fd.get(), candidate->instance_name.c_str(),
                  &instance_path_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstat(candidate->instance_fd.get(), &candidate->instance_identity) !=
            0 ||
        !safe_directory(instance_path_metadata,
                        candidate->base_identity.st_dev) ||
        !safe_directory(candidate->instance_identity,
                        candidate->base_identity.st_dev) ||
        !same_identity(instance_path_metadata, candidate->instance_identity)) {
      throw std::runtime_error(
          "output-store instance identity verification failed");
    }
  }

  /**
   * @brief Commits validated startup paths, identities, and descriptor owners.
   * @param candidate Complete startup transaction to transfer into the store.
   * @return Nothing.
   * @throws std::bad_alloc if persistent path assignment cannot allocate.
   * @note Caller holds `mutex_`. Path assignment completes before descriptors
   *       move; `running_` is the final publication commit point.
   */
  void commit_start_locked(StartCandidate* candidate) {
    parent_path_ = candidate->parent_path;
    base_name_ = candidate->base_name;
    base_path_ = candidate->base_path;
    instance_name_ = candidate->instance_name;
    instance_path_ = candidate->base_path + "/" + candidate->instance_name;
    parent_identity_ = candidate->parent_identity;
    base_identity_ = candidate->base_identity;
    instance_identity_ = candidate->instance_identity;
    parent_fd_ = std::move(candidate->parent_fd);
    base_fd_ = std::move(candidate->base_fd);
    instance_fd_ = std::move(candidate->instance_fd);
    publications_open_ = true;
    leases_open_ = true;
    running_ = true;
  }

  /**
   * @brief Removes only this failed startup transaction's validated creations.
   * @param candidate Failed candidate whose descriptors and flags are consumed.
   * @return Nothing.
   * @throws Nothing; cleanup is best-effort and preserves every mismatch.
   * @note Caller holds `mutex_`. Instance cleanup precedes descriptor close and
   *       base cleanup; persistent store ownership is never modified.
   */
  void rollback_start_locked(StartCandidate* candidate) const noexcept {
    if (candidate->instance_created) {
      struct stat instance_path_metadata{};
      struct stat instance_fd_metadata{};
      if (candidate->instance_fd &&
          ::fstat(candidate->instance_fd.get(), &instance_fd_metadata) == 0 &&
          ::fstatat(candidate->base_fd.get(), candidate->instance_name.c_str(),
                    &instance_path_metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
          safe_directory(instance_path_metadata) &&
          safe_directory(instance_fd_metadata) &&
          same_identity(instance_path_metadata, instance_fd_metadata)) {
        (void)::unlinkat(candidate->base_fd.get(),
                         candidate->instance_name.c_str(), AT_REMOVEDIR);
      }
    }
    candidate->instance_fd.reset();
    if (candidate->base_created) {
      struct stat base_path_metadata{};
      struct stat base_fd_metadata{};
      if (candidate->base_fd &&
          ::fstat(candidate->base_fd.get(), &base_fd_metadata) == 0 &&
          ::fstatat(candidate->parent_fd.get(), candidate->base_name.c_str(),
                    &base_path_metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
          safe_directory(base_path_metadata) &&
          safe_directory(base_fd_metadata) &&
          same_identity(base_path_metadata, base_fd_metadata)) {
        (void)::unlinkat(candidate->parent_fd.get(),
                         candidate->base_name.c_str(), AT_REMOVEDIR);
      }
    }
    candidate->base_fd.reset();
  }

  /**
   * @brief Applies lazy expiry and checks publication lifecycle and quota.
   * @param compute_id Valid image-job identity being admitted.
   * @param layout Validated nonempty tight-row image layout.
   * @return True when identity and count/byte quota permit publication.
   * @throws std::runtime_error when publication is closed or already exists.
   * @throws Whatever the injected clock throws.
   * @note Caller holds `mutex_`; expiry and admission are one serialized
   *       observation and no file is created by this helper.
   */
  bool admit_publication_locked(const ComputeRequestId& compute_id,
                                const ImageLayout& layout) {
    if (!running_ || !publications_open_ || !instance_fd_) {
      throw std::runtime_error("output store is not accepting publication");
    }
    const TimePoint cleanup_time = clock_();
    (void)expire_locked(cleanup_time);
    if (compute_to_output_.count(compute_id.value) != 0) {
      throw std::runtime_error(
          "output store already published this compute job");
    }
    return records_.size() < limits_.artifacts &&
           retained_bytes_ <= limits_.total_bytes - layout.total_bytes;
  }

  /**
   * @brief Reserves opaque names and constructs the uncommitted artifact
   * record.
   * @param compute_id Accepted image-job identity.
   * @param image Validated nonempty Host image.
   * @param layout Checked tight-row byte layout.
   * @return Candidate holding names, metadata, and rollback flags.
   * @throws std::runtime_error for invalid entropy or collision exhaustion.
   * @throws std::bad_alloc when ids, names, paths, or record storage allocate.
   * @throws Whatever the injected id generator throws.
   * @note Caller holds `mutex_`; ids are checked against retained maps and the
   *       instance directory, but no file or map entry is created yet.
   */
  PublicationCandidate make_publication_candidate_locked(
      const ComputeRequestId& compute_id, const ImageBuffer& image,
      const ImageLayout& layout) {
    PublicationCandidate candidate;
    candidate.output_id = generate_unique_id_locked({}, {});
    candidate.delivery_id = generate_unique_id_locked(candidate.output_id, {});
    const std::string stage_id =
        generate_unique_id_locked(candidate.output_id, candidate.delivery_id);
    candidate.final_name = output_filename(candidate.output_id);
    candidate.stage_name = output_filename(stage_id);
    const std::string absolute_path =
        instance_path_ + "/" + candidate.final_name;

    candidate.record = std::make_unique<Record>();
    candidate.record->compute_id = compute_id.value;
    candidate.record->metadata.output_id = candidate.output_id;
    candidate.record->metadata.path = absolute_path;
    candidate.record->metadata.width = image.width;
    candidate.record->metadata.height = image.height;
    candidate.record->metadata.channels = image.channels;
    candidate.record->metadata.data_type = image.type;
    candidate.record->metadata.device = Device::CPU;
    candidate.record->metadata.row_step = layout.row_bytes;
    candidate.record->metadata.byte_size = layout.total_bytes;
    candidate.record->filename = candidate.final_name;
    candidate.record->delivery_id = candidate.delivery_id;
    return candidate;
  }

  /**
   * @brief Creates, protects, fills, syncs, and size-validates one stage file.
   * @param image Borrowed validated Host image source.
   * @param layout Exact tight-row source and artifact sizes.
   * @param candidate Publication transaction receiving fd and inode identity.
   * @return Nothing.
   * @throws std::runtime_error for filesystem, identity, write, or size errors.
   * @throws std::bad_alloc if an errno diagnostic cannot be allocated.
   * @note Caller holds `mutex_`; the candidate owns the descriptor and records
   *       stage creation immediately so later failure performs exact rollback.
   */
  void create_and_write_stage_locked(const ImageBuffer& image,
                                     const ImageLayout& layout,
                                     PublicationCandidate* candidate) const {
    candidate->artifact_fd.reset(
        ::openat(instance_fd_.get(), candidate->stage_name.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!candidate->artifact_fd) {
      throw std::runtime_error(
          errno_message("cannot create output artifact stage"));
    }
    candidate->stage_created = true;
    if (::fstat(candidate->artifact_fd.get(), &candidate->owned_identity) !=
            0 ||
        !S_ISREG(candidate->owned_identity.st_mode) ||
        candidate->owned_identity.st_uid != ::geteuid() ||
        candidate->owned_identity.st_nlink != 1 ||
        candidate->owned_identity.st_dev != instance_identity_.st_dev) {
      throw std::runtime_error(
          "output artifact stage ownership verification failed");
    }
    if (::fchmod(candidate->artifact_fd.get(), 0600) != 0 ||
        ::fstat(candidate->artifact_fd.get(), &candidate->owned_identity) !=
            0 ||
        !safe_artifact(candidate->owned_identity, instance_identity_.st_dev)) {
      throw std::runtime_error(
          "output artifact stage identity verification failed");
    }

    const auto* source = static_cast<const std::uint8_t*>(image.data.get());
    for (int row = 0; row < image.height; ++row) {
      const std::size_t offset = static_cast<std::size_t>(row) * image.step;
      write_all(candidate->artifact_fd.get(), source + offset,
                layout.row_bytes);
    }
    if (::fsync(candidate->artifact_fd.get()) != 0 ||
        ::fstat(candidate->artifact_fd.get(), &candidate->owned_identity) !=
            0 ||
        !safe_artifact(candidate->owned_identity, instance_identity_.st_dev) ||
        static_cast<std::uintmax_t>(candidate->owned_identity.st_size) !=
            static_cast<std::uintmax_t>(layout.total_bytes)) {
      throw std::runtime_error(
          "output artifact write/size verification failed");
    }
  }

  /**
   * @brief Atomically publishes and revalidates a completely written artifact.
   * @param layout Exact expected artifact size.
   * @param candidate Stage transaction receiving final identity metadata.
   * @return Nothing.
   * @throws std::runtime_error for rename, ancestry, or identity failure.
   * @throws std::bad_alloc if an errno diagnostic cannot be allocated.
   * @throws Whatever the injected clock throws.
   * @note Caller holds `mutex_` and the candidate retains its open inode fd;
   *       `renamed` changes before post-rename validation so rollback targets
   *       the correct controlled name.
   */
  void publish_stage_locked(const ImageLayout& layout,
                            PublicationCandidate* candidate) {
    if (!rename_noreplace(instance_fd_.get(), candidate->stage_name,
                          candidate->final_name)) {
      throw std::runtime_error(
          errno_message("cannot atomically publish output artifact"));
    }
    candidate->renamed = true;
    struct stat published_path{};
    struct stat published_fd{};
    if (!ancestry_matches_locked() ||
        ::fstatat(instance_fd_.get(), candidate->final_name.c_str(),
                  &published_path, AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstat(candidate->artifact_fd.get(), &published_fd) != 0 ||
        !safe_artifact(published_path, instance_identity_.st_dev) ||
        !safe_artifact(published_fd, instance_identity_.st_dev) ||
        !same_identity(published_path, published_fd) ||
        !same_identity(published_fd, candidate->owned_identity) ||
        static_cast<std::uintmax_t>(published_path.st_size) !=
            static_cast<std::uintmax_t>(layout.total_bytes)) {
      throw std::runtime_error(
          "published output artifact identity verification failed");
    }

    const TimePoint publication_time = clock_();
    candidate->record->job_expiry =
        saturating_deadline(publication_time, limits_.job_ttl);
    candidate->record->metadata.filesystem_device =
        static_cast<std::uint64_t>(published_path.st_dev);
    candidate->record->metadata.inode =
        static_cast<std::uint64_t>(published_path.st_ino);
  }

  /**
   * @brief Commits delivery, compute, record, then retained-byte accounting.
   * @param compute_id Original accepted image-job identity.
   * @param layout Exact retained artifact byte count.
   * @param candidate Published transaction whose record ownership transfers.
   * @return Nothing.
   * @throws std::runtime_error if any supposedly unique map identity collides.
   * @throws std::bad_alloc if ordered-map insertion allocates.
   * @note Caller holds `mutex_`; rollback flags change immediately after each
   *       successful lookup insertion. Byte accounting is the final commit and
   *       cannot throw.
   */
  void commit_publication_locked(const ComputeRequestId& compute_id,
                                 const ImageLayout& layout,
                                 PublicationCandidate* candidate) {
    const auto delivery_result = delivery_to_output_.emplace(
        candidate->delivery_id, candidate->output_id);
    if (!delivery_result.second) {
      throw std::runtime_error("output delivery identity reservation failed");
    }
    candidate->delivery_inserted = true;
    const auto compute_result =
        compute_to_output_.emplace(compute_id.value, candidate->output_id);
    if (!compute_result.second) {
      throw std::runtime_error("output compute identity reservation failed");
    }
    candidate->compute_inserted = true;
    const auto record_result =
        records_.emplace(candidate->output_id, std::move(candidate->record));
    if (!record_result.second) {
      throw std::runtime_error("output record identity reservation failed");
    }
    retained_bytes_ += layout.total_bytes;
  }

  /**
   * @brief Reverses partial lookup insertion and removes the created inode.
   * @param compute_id Original accepted image-job identity.
   * @param candidate Failed publication transaction with precise commit flags.
   * @return Nothing.
   * @throws Nothing; every rollback action is conservative and best-effort.
   * @note Caller holds `mutex_`; delivery removal precedes compute removal and
   *       identity-checked unlink, matching forward commit ownership order.
   */
  void rollback_publication_locked(const ComputeRequestId& compute_id,
                                   PublicationCandidate* candidate) noexcept {
    if (candidate->delivery_inserted) {
      delivery_to_output_.erase(candidate->delivery_id);
    }
    if (candidate->compute_inserted) {
      compute_to_output_.erase(compute_id.value);
    }
    if (candidate->stage_created) {
      const std::string& cleanup_name =
          candidate->renamed ? candidate->final_name : candidate->stage_name;
      unlink_created_locked(cleanup_name, candidate->owned_identity);
    }
  }

  /**
   * @brief Closes admissions and removes every retained job-owner flag.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds `mutex_`; the ownership marker is set before admissions
   *       close, and delivery leases continue to own artifacts.
   */
  void begin_shutdown_locked() noexcept {
    shutdown_in_progress_ = true;
    publications_open_ = false;
    leases_open_ = false;
    for (auto& entry : records_) {
      entry.second->job_owned = false;
    }
  }

  /**
   * @brief Expires or waits out leases and removes every unowned record.
   * @param lock Owning `mutex_` lock used by the condition-variable wait.
   * @return Nothing.
   * @throws std::system_error if the condition-variable wait fails.
   * @note The injected clock is contained: a throwing clock advances cleanup
   *       to `TimePoint::max()`. Waits release `mutex_`, allowing explicit
   *       lease release to reach the shutdown owner.
   */
  void drain_shutdown_records_locked(std::unique_lock<std::mutex>* lock) {
    while (!records_.empty()) {
      TimePoint now;
      try {
        now = clock_();
      } catch (...) {
        now = TimePoint::max();
      }
      (void)expire_locked(now);
      if (records_.empty()) {
        break;
      }
      TimePoint earliest = TimePoint::max();
      for (const auto& entry : records_) {
        if (entry.second->lease_active) {
          earliest = std::min(earliest, entry.second->lease_expiry);
        }
      }
      if (earliest == TimePoint::max()) {
        while (!records_.empty()) {
          erase_record_locked(records_.begin());
        }
        break;
      }
      const auto remaining = earliest > now
                                 ? earliest - now
                                 : std::chrono::steady_clock::duration::zero();
      if (remaining == std::chrono::steady_clock::duration::zero()) {
        continue;
      }
      cv_.wait_for(*lock, remaining);
    }
  }

  /**
   * @brief Removes the current instance directory only under stable identity.
   * @return Nothing.
   * @throws Nothing; mismatch or system error conservatively preserves it.
   * @note Caller holds `mutex_` and all ancestry descriptors. The immediate
   *       path/fd validation precedes the best-effort directory unlink.
   */
  void remove_instance_directory_locked() const noexcept {
    if (!ancestry_matches_locked()) {
      return;
    }
    struct stat final_instance_path{};
    struct stat final_instance_fd{};
    if (::fstat(instance_fd_.get(), &final_instance_fd) == 0 &&
        ::fstatat(base_fd_.get(), instance_name_.c_str(), &final_instance_path,
                  AT_SYMLINK_NOFOLLOW) == 0 &&
        safe_directory(final_instance_path, base_identity_.st_dev) &&
        safe_directory(final_instance_fd, base_identity_.st_dev) &&
        same_identity(final_instance_path, instance_identity_) &&
        same_identity(final_instance_fd, instance_identity_)) {
      (void)::unlinkat(base_fd_.get(), instance_name_.c_str(), AT_REMOVEDIR);
    }
  }

  /**
   * @brief Commits successful shutdown by clearing descriptors and identities.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds `mutex_`; descriptors close before path/identity state
   *       clears, and `shutdown_in_progress_` is the final state transition.
   */
  void finish_shutdown_locked() noexcept {
    instance_fd_.reset();
    base_fd_.reset();
    parent_fd_.reset();
    parent_path_.clear();
    base_name_.clear();
    base_path_.clear();
    instance_name_.clear();
    instance_path_.clear();
    parent_identity_ = {};
    base_identity_ = {};
    instance_identity_ = {};
    running_ = false;
    shutdown_in_progress_ = false;
  }

  /**
   * @brief Applies the original conservative nonthrowing shutdown-failure
   * state.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds `mutex_`. Admissions and running state close, directory
   *       descriptors release exactly once, and diagnostic path/identity data
   *       remains untouched for post-failure object destruction parity.
   */
  void fail_shutdown_locked() noexcept {
    publications_open_ = false;
    leases_open_ = false;
    running_ = false;
    shutdown_in_progress_ = false;
    instance_fd_.reset();
    base_fd_.reset();
    parent_fd_.reset();
  }

  /**
   * @brief Generates a collision-free id not already present on disk.
   * @param excluded_one First candidate forbidden for cross-type uniqueness.
   * @param excluded_two Second candidate forbidden for cross-type uniqueness.
   * @return Valid opaque id.
   * @throws std::runtime_error for invalid entropy or collision exhaustion.
   * @throws std::bad_alloc if candidate/path storage cannot be allocated.
   * @note Caller holds mutex_ and the instance directory descriptor.
   */
  std::string generate_unique_id_locked(const std::string& excluded_one,
                                        const std::string& excluded_two) {
    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
      std::string candidate = id_generator_();
      if (!valid_opaque_id(candidate)) {
        throw std::runtime_error(
            "output-store id generator returned invalid data");
      }
      if (candidate == excluded_one || candidate == excluded_two ||
          records_.count(candidate) != 0 ||
          delivery_to_output_.count(candidate) != 0) {
        continue;
      }
      struct stat occupied{};
      const std::string filename = output_filename(candidate);
      if (::fstatat(instance_fd_.get(), filename.c_str(), &occupied,
                    AT_SYMLINK_NOFOLLOW) == 0) {
        continue;
      }
      if (errno == ENOENT) {
        return candidate;
      }
      throw std::runtime_error(
          errno_message("cannot reserve output-store identity"));
    }
    throw std::runtime_error("output-store opaque id collision limit reached");
  }

  /**
   * @brief Revalidates parent, base, and instance paths against held fds.
   * @return Valid, definite absence/mismatch, or an inconclusive system error.
   * @throws Nothing.
   * @note Caller holds mutex_. Unexpected syscall errors must not be collapsed
   *       into `artifact_not_found` or cause a retained lease to be removed.
   */
  ArtifactValidation validate_ancestry_locked() const noexcept {
    if (!parent_fd_ || !base_fd_ || !instance_fd_ || parent_path_.empty() ||
        base_name_.empty() || base_path_.empty() || instance_path_.empty()) {
      return ArtifactValidation::InternalError;
    }
    struct stat parent_path{};
    struct stat parent_fd{};
    struct stat base_path{};
    struct stat base_fd{};
    struct stat instance_path{};
    struct stat instance_fd{};
    if (retry_lstat(parent_path_.c_str(), &parent_path) != 0) {
      return path_mismatch_error(errno) ? ArtifactValidation::MissingOrMismatch
                                        : ArtifactValidation::InternalError;
    }
    if (retry_fstat(parent_fd_.get(), &parent_fd) != 0) {
      return ArtifactValidation::InternalError;
    }
    if (retry_fstatat(parent_fd_.get(), base_name_.c_str(), &base_path) != 0) {
      return path_mismatch_error(errno) ? ArtifactValidation::MissingOrMismatch
                                        : ArtifactValidation::InternalError;
    }
    if (retry_fstat(base_fd_.get(), &base_fd) != 0) {
      return ArtifactValidation::InternalError;
    }
    if (retry_fstatat(base_fd_.get(), instance_name_.c_str(), &instance_path) !=
        0) {
      return path_mismatch_error(errno) ? ArtifactValidation::MissingOrMismatch
                                        : ArtifactValidation::InternalError;
    }
    if (retry_fstat(instance_fd_.get(), &instance_fd) != 0) {
      return ArtifactValidation::InternalError;
    }
    if (!safe_socket_parent(parent_path) || !safe_socket_parent(parent_fd) ||
        !safe_directory(base_path) || !safe_directory(base_fd) ||
        !safe_directory(instance_path, base_identity_.st_dev) ||
        !safe_directory(instance_fd, base_identity_.st_dev) ||
        !same_identity(parent_path, parent_identity_) ||
        !same_identity(parent_fd, parent_identity_) ||
        !same_identity(base_path, base_identity_) ||
        !same_identity(base_fd, base_identity_) ||
        !same_identity(instance_path, instance_identity_) ||
        !same_identity(instance_fd, instance_identity_)) {
      return ArtifactValidation::MissingOrMismatch;
    }
    return ArtifactValidation::Valid;
  }

  /**
   * @brief Reports whether live ancestry validation fully succeeds.
   * @return True only for a complete valid observation.
   * @throws Nothing.
   * @note Cleanup conservatively leaves paths untouched for both mismatch and
   *       inconclusive system errors.
   */
  bool ancestry_matches_locked() const noexcept {
    return validate_ancestry_locked() == ArtifactValidation::Valid;
  }

  /**
   * @brief Revalidates one live output path, descriptor, and stored identity.
   * @param record Retained record to inspect.
   * @return Valid, definite absence/mismatch, or an inconclusive system error.
   * @throws Nothing.
   * @note Caller holds mutex_.
   */
  ArtifactValidation validate_record_locked(
      const Record& record) const noexcept {
    const ArtifactValidation ancestry = validate_ancestry_locked();
    if (ancestry != ArtifactValidation::Valid) {
      return ancestry;
    }
    struct stat path_metadata{};
    if (retry_fstatat(instance_fd_.get(), record.filename.c_str(),
                      &path_metadata) != 0) {
      return artifact_mismatch_error(errno)
                 ? ArtifactValidation::MissingOrMismatch
                 : ArtifactValidation::InternalError;
    }
    if (!safe_artifact(path_metadata, instance_identity_.st_dev) ||
        static_cast<std::uint64_t>(path_metadata.st_dev) !=
            record.metadata.filesystem_device ||
        static_cast<std::uint64_t>(path_metadata.st_ino) !=
            record.metadata.inode ||
        static_cast<std::uintmax_t>(path_metadata.st_size) !=
            static_cast<std::uintmax_t>(record.metadata.byte_size)) {
      return ArtifactValidation::MissingOrMismatch;
    }
    ScopedFd descriptor(
        retry_open_artifact(instance_fd_.get(), record.filename.c_str()));
    if (!descriptor) {
      return artifact_mismatch_error(errno)
                 ? ArtifactValidation::MissingOrMismatch
                 : ArtifactValidation::InternalError;
    }
    struct stat descriptor_metadata{};
    struct stat current_path{};
    if (retry_fstat(descriptor.get(), &descriptor_metadata) != 0) {
      return ArtifactValidation::InternalError;
    }
    if (retry_fstatat(instance_fd_.get(), record.filename.c_str(),
                      &current_path) != 0) {
      return artifact_mismatch_error(errno)
                 ? ArtifactValidation::MissingOrMismatch
                 : ArtifactValidation::InternalError;
    }
    if (!safe_artifact(descriptor_metadata, instance_identity_.st_dev) ||
        !safe_artifact(current_path, instance_identity_.st_dev) ||
        !same_identity(path_metadata, descriptor_metadata) ||
        !same_identity(descriptor_metadata, current_path) ||
        path_metadata.st_size != descriptor_metadata.st_size ||
        descriptor_metadata.st_size != current_path.st_size) {
      return ArtifactValidation::MissingOrMismatch;
    }
    return ArtifactValidation::Valid;
  }

  /**
   * @brief Unlinks one path only while it matches a known open-file identity.
   * @param filename Controlled basename.
   * @param expected Expected device/inode/type/mode/link identity.
   * @param expected_bytes Expected file byte size.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds mutex_. A final fstatat immediately precedes unlinkat;
   *       unsafe replacements are preserved.
   */
  void unlink_matching_locked(const std::string& filename,
                              const struct stat& expected,
                              std::size_t expected_bytes) const noexcept {
    if (!ancestry_matches_locked() ||
        !safe_artifact(expected, instance_identity_.st_dev)) {
      return;
    }
    struct stat first{};
    struct stat descriptor_metadata{};
    struct stat current{};
    if (::fstatat(instance_fd_.get(), filename.c_str(), &first,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !safe_artifact(first, instance_identity_.st_dev) ||
        !same_identity(first, expected) ||
        static_cast<std::uintmax_t>(first.st_size) !=
            static_cast<std::uintmax_t>(expected_bytes)) {
      return;
    }
    ScopedFd descriptor(
        ::openat(instance_fd_.get(), filename.c_str(),
                 O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!descriptor || ::fstat(descriptor.get(), &descriptor_metadata) != 0 ||
        !safe_artifact(descriptor_metadata, instance_identity_.st_dev) ||
        !same_identity(first, descriptor_metadata) ||
        ::fstatat(instance_fd_.get(), filename.c_str(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !safe_artifact(current, instance_identity_.st_dev) ||
        !same_identity(descriptor_metadata, current) ||
        descriptor_metadata.st_size != current.st_size) {
      return;
    }
    while (::unlinkat(instance_fd_.get(), filename.c_str(), 0) != 0 &&
           errno == EINTR) {
    }
  }

  /**
   * @brief Removes a just-created publication entry by its open-fd identity.
   * @param filename Controlled stage or final basename.
   * @param expected Metadata captured from the still-open created descriptor.
   * @return Nothing.
   * @throws Nothing.
   * @note Unlike retained cleanup, rollback permits an incomplete size or mode
   *       because it owns an unpublished O_EXCL-created inode. It still checks
   *       ancestry, real regular type, owner, device, inode, and one link.
   */
  void unlink_created_locked(const std::string& filename,
                             const struct stat& expected) const noexcept {
    if (!ancestry_matches_locked() || !S_ISREG(expected.st_mode) ||
        expected.st_uid != ::geteuid() || expected.st_nlink != 1 ||
        expected.st_dev != instance_identity_.st_dev) {
      return;
    }
    struct stat current{};
    if (::fstatat(instance_fd_.get(), filename.c_str(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(current.st_mode) || current.st_uid != ::geteuid() ||
        current.st_nlink != 1 || !same_identity(current, expected)) {
      return;
    }
    while (::unlinkat(instance_fd_.get(), filename.c_str(), 0) != 0 &&
           errno == EINTR) {
    }
  }

  /**
   * @brief Identity-cleans and erases one retained record and its quota.
   * @param iterator Existing output map iterator.
   * @return Iterator following the erased record.
   * @throws Nothing.
   * @note Caller holds mutex_. Quota is released even when an unsafe replaced
   *       path must be preserved.
   */
  std::map<std::string, std::unique_ptr<Record>>::iterator erase_record_locked(
      std::map<std::string, std::unique_ptr<Record>>::iterator
          iterator) noexcept {
    Record& record = *iterator->second;
    struct stat expected{};
    expected.st_mode = S_IFREG | 0600;
    expected.st_uid = ::geteuid();
    expected.st_nlink = 1;
    expected.st_dev = static_cast<dev_t>(record.metadata.filesystem_device);
    expected.st_ino = static_cast<ino_t>(record.metadata.inode);
    expected.st_size = static_cast<off_t>(record.metadata.byte_size);
    unlink_matching_locked(record.filename, expected,
                           record.metadata.byte_size);
    compute_to_output_.erase(record.compute_id);
    delivery_to_output_.erase(record.delivery_id);
    if (retained_bytes_ >= record.metadata.byte_size) {
      retained_bytes_ -= record.metadata.byte_size;
    } else {
      retained_bytes_ = 0;
    }
    return records_.erase(iterator);
  }

  /**
   * @brief Applies job-owner and delivery deadlines and removes unowned files.
   * @param now One monotonic timestamp shared by the complete cleanup pass.
   * @return Number of active delivery leases expired.
   * @throws Nothing.
   * @note Caller holds mutex_.
   */
  std::size_t expire_locked(TimePoint now) noexcept {
    std::size_t expired_leases = 0;
    auto iterator = records_.begin();
    while (iterator != records_.end()) {
      Record& record = *iterator->second;
      if (record.job_owned && now >= record.job_expiry) {
        record.job_owned = false;
      }
      if (record.lease_active && now >= record.lease_expiry) {
        record.lease_active = false;
        ++expired_leases;
      }
      if (!record.job_owned && !record.lease_active) {
        iterator = erase_record_locked(iterator);
      } else {
        ++iterator;
      }
    }
    return expired_leases;
  }

  /**
   * @brief Applies current injected deadlines without failing release cleanup.
   * @return Nothing.
   * @throws Nothing; a throwing clock leaves existing flags unchanged.
   * @note Caller holds mutex_. Release still removes explicit ownership even
   *       when the optional lazy-expiry pass cannot obtain a timestamp.
   */
  void expire_now_best_effort_locked() noexcept {
    try {
      (void)expire_locked(clock_());
    } catch (...) {
    }
  }

  /** @brief Immutable quota and TTL policy. */
  OutputStoreLimits limits_;

  /** @brief Injectable monotonic time source. */
  Clock clock_;

  /** @brief Injectable opaque candidate source. */
  IdGenerator id_generator_;

  /** @brief Serializes descriptors, records, leases, and lifecycle. */
  mutable std::mutex mutex_;

  /** @brief Wakes shutdown after explicit release or clock-driven cleanup. */
  std::condition_variable cv_;

  /** @brief Wakes concurrent callers after the sole shutdown completes. */
  std::condition_variable shutdown_cv_;

  /** @brief Records indexed by stable output id. */
  std::map<std::string, std::unique_ptr<Record>> records_;

  /** @brief Original compute id to its sole retained output id. */
  std::map<std::string, std::string> compute_to_output_;

  /** @brief Stable delivery id to output id lookup, active or inactive. */
  std::map<std::string, std::string> delivery_to_output_;

  /** @brief Current quota-accounted tight-row bytes. */
  std::size_t retained_bytes_ = 0;

  /** @brief Validated protected socket-parent descriptor. */
  ScopedFd parent_fd_;

  /** @brief Validated socket-specific base descriptor. */
  ScopedFd base_fd_;

  /** @brief Validated current-instance directory descriptor. */
  ScopedFd instance_fd_;

  /** @brief Absolute protected socket-parent path. */
  std::string parent_path_;

  /** @brief Controlled base basename relative to the socket parent. */
  std::string base_name_;

  /** @brief Absolute `<socket>.outputs` path. */
  std::string base_path_;

  /** @brief Controlled basename of the current instance directory. */
  std::string instance_name_;

  /** @brief Absolute current-instance path advertised in metadata ancestry. */
  std::string instance_path_;

  /** @brief Stable parent device/inode/type/owner/mode observation. */
  struct stat parent_identity_{};

  /** @brief Stable base device/inode/type/owner/mode observation. */
  struct stat base_identity_{};

  /** @brief Stable instance device/inode/type/owner/mode observation. */
  struct stat instance_identity_{};

  /** @brief Whether descriptors and record lifecycle are active. */
  bool running_ = false;

  /** @brief Whether drained image jobs may still publish artifacts. */
  bool publications_open_ = false;

  /** @brief Whether result operations may create or refresh leases. */
  bool leases_open_ = false;

  /** @brief Whether one lifecycle caller currently owns shutdown. */
  bool shutdown_in_progress_ = false;
};

/** @copydoc OutputStore::OutputStore */
OutputStore::OutputStore(OutputStoreLimits limits, Clock clock,
                         IdGenerator id_generator) {
  if (!clock) {
    clock = [] { return std::chrono::steady_clock::now(); };
  }
  if (!id_generator) {
    id_generator = generate_opaque_id;
  }
  impl_ =
      std::make_unique<Impl>(limits, std::move(clock), std::move(id_generator));
}

/** @copydoc OutputStore::~OutputStore */
OutputStore::~OutputStore() noexcept {
  shutdown();
}

/** @copydoc OutputStore::start */
OperationStatus OutputStore::start(const std::string& socket_path,
                                   const std::string& server_instance_id,
                                   int lifecycle_lock_fd) {
  try {
    impl_->start(socket_path, server_instance_id, lifecycle_lock_fd);
    return ok_status();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return internal_status(error.what());
  } catch (...) {
    return internal_status("unexpected output-store startup failure");
  }
}

/** @copydoc OutputStore::stop_leases */
void OutputStore::stop_leases() noexcept {
  impl_->stop_leases();
}

/** @copydoc OutputStore::publish */
ComputeOutputPublication OutputStore::publish(
    const ComputeRequestId& compute_id, ImageBuffer image) {
  try {
    Impl::RawPublication raw = impl_->publish_raw(compute_id, image);
    if (!raw.status.ok || raw.output_id.empty()) {
      return {std::move(raw.status), {}};
    }
    try {
      const std::string output_id = raw.output_id;
      ComputeOutputOwnership ownership(
          output_id,
          [implementation = impl_.get(),
           output_id](const std::optional<std::string>& delivery_id) noexcept {
            (void)implementation->release_job(output_id, delivery_id);
          });
      return {std::move(raw.status), std::move(ownership)};
    } catch (...) {
      (void)impl_->release_job(raw.output_id, std::nullopt);
      throw;
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    return {internal_status(error.what()), {}};
  } catch (...) {
    return {internal_status("unexpected output artifact publication failure"),
            {}};
  }
}

/** @copydoc OutputStore::acquire_delivery */
IpcResult<OutputArtifactDelivery> OutputStore::acquire_delivery(
    const std::string& output_id) {
  return impl_->acquire_delivery(output_id);
}

/** @copydoc OutputStore::release_job */
bool OutputStore::release_job(
    const std::string& output_id,
    const std::optional<std::string>& delivery_id) noexcept {
  return impl_->release_job(output_id, delivery_id);
}

/** @copydoc OutputStore::release_orphaned_delivery */
bool OutputStore::release_orphaned_delivery(
    const ComputeRequestId& compute_id,
    const std::string& delivery_id) noexcept {
  return impl_->release_orphaned_delivery(compute_id, delivery_id);
}

/** @copydoc OutputStore::cleanup_expired */
std::size_t OutputStore::cleanup_expired() noexcept {
  return impl_->cleanup_expired();
}

/** @copydoc OutputStore::shutdown */
void OutputStore::shutdown() noexcept {
  if (impl_) {
    impl_->shutdown();
  }
}

/** @copydoc OutputStore::artifact_count */
std::size_t OutputStore::artifact_count() const noexcept {
  return impl_->artifact_count();
}

/** @copydoc OutputStore::retained_bytes */
std::size_t OutputStore::retained_bytes() const noexcept {
  return impl_->retained_bytes();
}

}  // namespace ps::ipc::internal
