/**
 * @file worker_artifact_data_plane.cpp
 * @brief Implements the attempt-scoped Issue #105 worker artifact data plane.
 */
#include "server/worker_artifact_data_plane.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "server/worker_artifact_data_plane_test_access.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/**
 * @brief Clears descriptor ownership before one non-retried close attempt.
 * @param descriptor Non-null exact descriptor owner.
 * @return Nothing after ownership is invalidated.
 * @throws Nothing; close failure, including `EINTR`, is ignored.
 */
void close_once(int* descriptor) noexcept {
  if (descriptor == nullptr) {
    return;
  }
  const int owned = std::exchange(*descriptor, -1);
  if (owned >= 0) {
    static_cast<void>(::close(owned));
  }
}

/**
 * @brief Small move-only descriptor owner for throwing setup paths.
 * @throws Nothing for construction, moves, release, and destruction.
 * @note The owner clears its numeric descriptor before exactly one close
 * attempt, so an interrupted close cannot target later descriptor reuse.
 */
class ScopedDescriptor final {
 public:
  /**
   * @brief Creates one empty descriptor owner.
   * @throws Nothing.
   */
  ScopedDescriptor() noexcept = default;

  /**
   * @brief Takes ownership of one descriptor or invalid sentinel.
   * @param descriptor Exact descriptor to retain.
   * @throws Nothing.
   */
  explicit ScopedDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}

  /**
   * @brief Closes any retained descriptor exactly once at scope exit.
   * @throws Nothing; close errors are ignored without retry.
   */
  ~ScopedDescriptor() noexcept { close_once(&descriptor_); }

  /**
   * @brief Prevents duplicate descriptor ownership.
   * @param other Existing owner that remains unchanged.
   * @throws Nothing because the operation is deleted.
   */
  ScopedDescriptor(const ScopedDescriptor& other) = delete;
  /**
   * @brief Prevents duplicate descriptor assignment.
   * @param other Existing owner that remains unchanged.
   * @return No assignment result because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedDescriptor& operator=(const ScopedDescriptor& other) = delete;

  /**
   * @brief Transfers one exact descriptor owner.
   * @param other Source owner cleared by the move.
   * @throws Nothing.
   */
  ScopedDescriptor(ScopedDescriptor&& other) noexcept
      : descriptor_(other.release()) {}

  /**
   * @brief Replaces ownership from another descriptor owner.
   * @param other Source owner cleared by the move.
   * @return This owner.
   * @throws Nothing.
   */
  ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
    if (this != &other) {
      close_once(&descriptor_);
      descriptor_ = other.release();
    }
    return *this;
  }

  /**
   * @brief Returns the retained descriptor without transfer.
   * @return Exact descriptor or -1.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Transfers descriptor ownership to the caller.
   * @return Prior exact descriptor or -1.
   * @throws Nothing.
   */
  int release() noexcept { return std::exchange(descriptor_, -1); }

 private:
  /** @brief Sole exact descriptor owner. */
  int descriptor_ = -1;
};

/**
 * @brief Owns one private temporary occurrence and its mutable path buffer.
 *
 * Path storage is fully allocated before `mkstemp`. Once creation succeeds,
 * `adopt_created_descriptor()` arms both descriptor and pathname cleanup using
 * only non-throwing scalar/descriptor operations. All later setup exceptions
 * therefore remove the name during stack unwinding.
 *
 * @throws Nothing for construction after argument preparation, moves,
 * descriptor access, explicit creation-descriptor closure, and destruction.
 * @note The temporary name is never placed in protocol metadata. It remains
 * valid only for this owner's lifetime and is unlinked before delegation.
 */
class ScopedTemporaryOccurrence final {
 public:
  /**
   * @brief Retains one preallocated mutable `mkstemp` pathname template.
   * @param mutable_path Null-terminated template ending in six `X` bytes.
   * @throws Nothing after argument construction.
   */
  explicit ScopedTemporaryOccurrence(std::vector<char> mutable_path) noexcept
      : mutable_path_(std::move(mutable_path)) {}

  /**
   * @brief Best-effort removes a retained name and closes its descriptor.
   * @throws Nothing; cleanup failures are ignored during stack unwinding.
   * @note The destructor body unlinks before the descriptor member closes.
   */
  ~ScopedTemporaryOccurrence() noexcept {
    if (owns_name_ && !mutable_path_.empty()) {
      static_cast<void>(::unlink(mutable_path_.data()));
    }
  }

  /**
   * @brief Prevents duplicate occurrence ownership.
   * @param other Existing owner that remains unchanged.
   * @throws Nothing because the operation is deleted.
   */
  ScopedTemporaryOccurrence(const ScopedTemporaryOccurrence& other) = delete;

  /**
   * @brief Prevents duplicate occurrence assignment.
   * @param other Existing owner that remains unchanged.
   * @return No assignment result because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedTemporaryOccurrence& operator=(const ScopedTemporaryOccurrence& other) =
      delete;

  /**
   * @brief Transfers the exact occurrence and all cleanup obligations.
   * @param other Source owner disarmed by the move.
   * @throws Nothing.
   */
  ScopedTemporaryOccurrence(ScopedTemporaryOccurrence&& other) noexcept
      : mutable_path_(std::move(other.mutable_path_)),
        creation_(std::move(other.creation_)),
        owns_name_(std::exchange(other.owns_name_, false)) {}

  /**
   * @brief Prevents replacing a live occurrence owner through assignment.
   * @param other Source owner that remains unchanged.
   * @return No assignment result because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedTemporaryOccurrence& operator=(ScopedTemporaryOccurrence&& other) =
      delete;

  /**
   * @brief Returns mutable storage for the single `mkstemp` call.
   * @return Non-null null-terminated template storage.
   * @throws Nothing.
   * @note The returned pointer is invalidated only when this owner dies or
   * moves; no mutation may resize the retained vector after construction.
   */
  char* mutable_path() noexcept { return mutable_path_.data(); }

  /**
   * @brief Arms cleanup immediately after `mkstemp` succeeds.
   * @param descriptor Exact nonnegative creation descriptor.
   * @return Nothing.
   * @throws Nothing.
   * @note Callers invoke this before any operation that can throw.
   */
  void adopt_created_descriptor(int descriptor) noexcept {
    creation_ = ScopedDescriptor(descriptor);
    owns_name_ = true;
  }

  /**
   * @brief Returns the exact retained name for direction-specific reopen.
   * @return Borrowed null-terminated path valid for this owner lifetime.
   * @throws Nothing.
   */
  const char* path() const noexcept { return mutable_path_.data(); }

  /**
   * @brief Returns the original read/write creation descriptor.
   * @return Exact retained descriptor or -1.
   * @throws Nothing.
   */
  int creation_descriptor() const noexcept { return creation_.get(); }

  /**
   * @brief Unlinks the exact temporary name before any descriptor delegation.
   * @return Nothing after successful unlink and ownership transition.
   * @throws std::system_error when unlink fails.
   */
  void unlink_now() {
    if (owns_name_ && ::unlink(mutable_path_.data()) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "unlink worker artifact data-plane occurrence");
    }
    owns_name_ = false;
  }

  /**
   * @brief Closes the original read/write descriptor after directional reopen.
   * @return Nothing.
   * @throws Nothing; close errors are ignored without retry.
   */
  void close_creation_descriptor() noexcept { creation_ = ScopedDescriptor(); }

 private:
  /** @brief Preallocated mutable pathname, never placed in control metadata. */
  std::vector<char> mutable_path_;
  /** @brief Sole original read/write creation descriptor owner. */
  ScopedDescriptor creation_;
  /** @brief Whether destruction still owes pathname removal. */
  bool owns_name_ = false;
};

/**
 * @brief Builds one null-terminated private `mkstemp` pathname template.
 * @param directory Existing trusted directory selected by the caller.
 * @return Fully allocated mutable template storage.
 * @throws std::invalid_argument when `directory` is empty.
 * @throws std::filesystem::filesystem_error when path conversion fails.
 * @throws std::bad_alloc when template storage cannot be retained.
 * @note Every allocation completes before a filesystem name is created.
 */
std::vector<char> private_temporary_template(
    const std::filesystem::path& directory) {
  if (directory.empty()) {
    throw std::invalid_argument("worker temporary directory is empty");
  }
  const std::filesystem::path template_path =
      directory / "photospider-worker-artifact-XXXXXX";
  const std::string template_text = template_path.string();
  std::vector<char> mutable_template(template_text.begin(),
                                     template_text.end());
  mutable_template.push_back('\0');
  return mutable_template;
}

/**
 * @brief Sets close-on-exec on one manager-owned setup descriptor.
 * @param descriptor Valid descriptor created before fork.
 * @return Nothing after `FD_CLOEXEC` is present.
 * @throws std::system_error when descriptor flag access or mutation fails.
 */
void set_close_on_exec(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFD);
  if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "configure worker artifact descriptor");
  }
}

/**
 * @brief Creates one privately owned mode-0600 temporary regular file.
 * @param directory Existing trusted directory for the temporary occurrence.
 * @param inject_failure_after_creation Whether to throw immediately after
 * `mkstemp` arms its allocation-free cleanup owner.
 * @return Move-only occurrence with its read/write creation descriptor.
 * @throws std::invalid_argument when `directory` is empty.
 * @throws std::filesystem::filesystem_error when its template cannot be built.
 * @throws std::system_error for creation, permission, or flag failure.
 * @throws std::bad_alloc when path-template storage exhausts memory.
 * @note Cleanup is armed without allocation immediately after `mkstemp`; any
 * later allocation or setup exception therefore removes the exact name. The
 * injection is reachable only through the source-private test-access method.
 */
ScopedTemporaryOccurrence create_private_temporary(
    const std::filesystem::path& directory,
    bool inject_failure_after_creation) {
  ScopedTemporaryOccurrence occurrence(private_temporary_template(directory));
  const int descriptor = ::mkstemp(occurrence.mutable_path());
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create worker artifact data-plane occurrence");
  }
  occurrence.adopt_created_descriptor(descriptor);
  if (inject_failure_after_creation) {
    throw std::bad_alloc();
  }
  if (::fchmod(occurrence.creation_descriptor(), S_IRUSR | S_IWUSR) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "protect worker artifact data-plane occurrence");
  }
  set_close_on_exec(occurrence.creation_descriptor());
  return occurrence;
}

/**
 * @brief Creates one private occurrence in the process temporary directory.
 * @return Move-only occurrence with its read/write creation descriptor.
 * @throws Temporary-directory, setup, filesystem, and allocation failures
 * unchanged.
 * @note The directory and complete path buffer are resolved before `mkstemp`.
 */
ScopedTemporaryOccurrence create_private_temporary() {
  return create_private_temporary(std::filesystem::temp_directory_path(),
                                  false);
}

/**
 * @brief Opens one exact direction-scoped descriptor with close-on-exec.
 * @param path Non-null existing manager-owned private temporary path.
 * @param flags `O_RDONLY` or `O_WRONLY`.
 * @return Sole new descriptor owner.
 * @throws std::invalid_argument for unsupported access flags.
 * @throws std::system_error for open or flag configuration failure.
 */
ScopedDescriptor open_directional(const char* path, int flags) {
  if (flags != O_RDONLY && flags != O_WRONLY) {
    throw std::invalid_argument("worker artifact access direction is invalid");
  }
  const int descriptor = ::open(path, flags | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "open worker artifact data-plane occurrence");
  }
  ScopedDescriptor owner(descriptor);
  set_close_on_exec(owner.get());
  return owner;
}

/**
 * @brief Requires a reopened descriptor to name the exact private occurrence.
 * @param creation Original manager-owned `mkstemp` descriptor.
 * @param reopened Direction-scoped descriptor opened through its temporary
 * name before unlink.
 * @return Nothing when device/inode, owner, mode, link count, and regular-file
 * type all match the original private occurrence.
 * @throws std::system_error when either descriptor cannot be inspected.
 * @throws WorkerArtifactDataPlaneError when the path was replaced, linked,
 * repermissioned, or does not name the original private regular file.
 * @note This closes the pathname-reopen race before the name is unlinked and
 * before any descriptor is delegated across `fork`.
 */
void require_same_private_occurrence(int creation, int reopened) {
  struct stat original{};
  struct stat candidate{};
  if (::fstat(creation, &original) != 0 || ::fstat(reopened, &candidate) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "inspect private worker artifact occurrence");
  }
  constexpr mode_t kPermissionMask = S_IRWXU | S_IRWXG | S_IRWXO;
  constexpr mode_t kPrivatePermissions = S_IRUSR | S_IWUSR;
  if (!S_ISREG(original.st_mode) || !S_ISREG(candidate.st_mode) ||
      original.st_dev != candidate.st_dev ||
      original.st_ino != candidate.st_ino || original.st_uid != ::geteuid() ||
      candidate.st_uid != ::geteuid() ||
      (original.st_mode & kPermissionMask) != kPrivatePermissions ||
      (candidate.st_mode & kPermissionMask) != kPrivatePermissions ||
      original.st_nlink != 1 || candidate.st_nlink != 1) {
    throw WorkerArtifactDataPlaneError(
        "worker artifact temporary occurrence identity changed before unlink");
  }
}

/**
 * @brief Writes one complete byte range at fixed file offsets.
 * @param descriptor Valid writable regular-file descriptor.
 * @param bytes Borrowed input, null only when size is zero.
 * @param size Exact byte count.
 * @param initial_offset First file byte to replace.
 * @return Nothing after all bytes are visible in the occurrence.
 * @throws std::invalid_argument for null nonempty input.
 * @throws std::overflow_error when offsets cannot fit `off_t`.
 * @throws std::system_error for a non-interruption `pwrite` failure.
 */
void write_complete_at(int descriptor, const std::byte* bytes, std::size_t size,
                       std::size_t initial_offset = 0U) {
  if (size != 0U && bytes == nullptr) {
    throw std::invalid_argument("worker artifact write input is null");
  }
  if (initial_offset > std::numeric_limits<std::size_t>::max() - size) {
    throw std::overflow_error("worker artifact write range overflowed");
  }
  std::size_t consumed = 0U;
  while (consumed != size) {
    const std::size_t offset = initial_offset + consumed;
    if (offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
      throw std::overflow_error("worker artifact write offset overflowed");
    }
    const std::size_t remaining = size - consumed;
    const std::size_t chunk =
        std::min(remaining,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t written = ::pwrite(descriptor, bytes + consumed, chunk,
                                     static_cast<off_t>(offset));
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::system_error(errno == 0 ? EIO : errno, std::generic_category(),
                              "write worker artifact data-plane occurrence");
    }
    consumed += static_cast<std::size_t>(written);
  }
}

/**
 * @brief Reads one complete exact-sized byte range at fixed file offsets.
 * @param descriptor Valid readable regular-file descriptor.
 * @param size Exact expected payload size.
 * @return Independently owned bytes.
 * @throws std::overflow_error when offsets cannot fit `off_t`.
 * @throws std::system_error for a non-interruption `pread` failure.
 * @throws WorkerArtifactDataPlaneError for premature EOF.
 * @throws std::bad_alloc when bounded allocation exhausts memory.
 */
std::vector<std::byte> read_complete_at(int descriptor, std::size_t size) {
  std::vector<std::byte> bytes(size);
  std::size_t offset = 0U;
  while (offset != size) {
    if (offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
      throw std::overflow_error("worker artifact read offset overflowed");
    }
    const std::size_t remaining = size - offset;
    const std::size_t chunk =
        std::min(remaining,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t received = ::pread(descriptor, bytes.data() + offset, chunk,
                                     static_cast<off_t>(offset));
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "read worker artifact data-plane occurrence");
    }
    if (received == 0) {
      throw WorkerArtifactDataPlaneError(
          "worker artifact data-plane occurrence is truncated");
    }
    offset += static_cast<std::size_t>(received);
  }
  return bytes;
}

/**
 * @brief Reads one exact range into caller-owned storage at fixed offsets.
 * @param descriptor Valid readable regular-file descriptor.
 * @param destination Writable range, null only when `size` is zero.
 * @param size Exact number of bytes to read.
 * @param initial_offset First file byte to consume.
 * @return Nothing after the entire requested range is populated.
 * @throws std::invalid_argument for null nonempty output.
 * @throws std::overflow_error when the requested range cannot fit `off_t`.
 * @throws std::system_error for a non-interruption `pread` failure.
 * @throws WorkerArtifactDataPlaneError for premature EOF.
 * @note The caller owns allocation and must bound `size` before calling.
 */
void read_complete_at_into(int descriptor, std::byte* destination,
                           std::size_t size, std::size_t initial_offset) {
  if (size != 0U && destination == nullptr) {
    throw std::invalid_argument("worker artifact read output is null");
  }
  if (initial_offset > std::numeric_limits<std::size_t>::max() - size) {
    throw std::overflow_error("worker artifact read range overflowed");
  }
  std::size_t consumed = 0U;
  while (consumed != size) {
    const std::size_t offset = initial_offset + consumed;
    if (offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
      throw std::overflow_error("worker artifact read offset overflowed");
    }
    const std::size_t remaining = size - consumed;
    const std::size_t chunk =
        std::min(remaining,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t received = ::pread(descriptor, destination + consumed, chunk,
                                     static_cast<off_t>(offset));
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "read worker artifact data-plane occurrence");
    }
    if (received == 0) {
      throw WorkerArtifactDataPlaneError(
          "worker artifact data-plane occurrence is truncated");
    }
    consumed += static_cast<std::size_t>(received);
  }
}

/**
 * @brief Returns the size of one exact anonymous private occurrence.
 * @param descriptor Valid descriptor for one anonymous occurrence.
 * @return Nonnegative size exactly representable by `std::size_t`.
 * @throws std::system_error when `fstat` fails.
 * @throws WorkerArtifactDataPlaneError for a non-regular, linked,
 * repermissioned, foreign-owned, negative-sized, or unsupported occurrence.
 * @note Every call happens after manager-owned name unlink. Revalidating link,
 * owner, and mode after worker reap prevents worker mutation from promoting a
 * private stage into path authority.
 */
std::size_t private_anonymous_file_size(int descriptor) {
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "inspect worker artifact data-plane occurrence");
  }
  constexpr mode_t kPermissionMask = S_IRWXU | S_IRWXG | S_IRWXO;
  constexpr mode_t kPrivatePermissions = S_IRUSR | S_IWUSR;
  if (!S_ISREG(status.st_mode) || status.st_nlink != 0 ||
      status.st_uid != ::geteuid() ||
      (status.st_mode & kPermissionMask) != kPrivatePermissions ||
      status.st_size < 0) {
    throw WorkerArtifactDataPlaneError(
        "worker artifact data-plane occurrence is not anonymous and private");
  }
  const auto unsigned_size = static_cast<std::uintmax_t>(status.st_size);
  if (unsigned_size > std::numeric_limits<std::size_t>::max()) {
    throw WorkerArtifactDataPlaneError(
        "worker artifact data-plane occurrence size is unsupported");
  }
  return static_cast<std::size_t>(unsigned_size);
}

/**
 * @brief Requires one descriptor to have an exact access direction.
 * @param descriptor Valid descriptor.
 * @param expected_access `O_RDONLY` or `O_WRONLY`.
 * @return Nothing when `F_GETFL` reports exactly the expected direction.
 * @throws std::invalid_argument for an unsupported expected access value.
 * @throws std::system_error when descriptor flags cannot be read.
 * @throws WorkerArtifactDataPlaneError when actual direction differs.
 */
void require_access_direction(int descriptor, int expected_access) {
  if (expected_access != O_RDONLY && expected_access != O_WRONLY) {
    throw std::invalid_argument("worker artifact expected access is invalid");
  }
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "inspect worker artifact descriptor access");
  }
  if ((flags & O_ACCMODE) != expected_access) {
    throw WorkerArtifactDataPlaneError(
        "worker artifact descriptor access direction is invalid");
  }
}

/**
 * @brief Converts one accepted uint64 resource bound to local `size_t`.
 * @param value Positive accepted Job resource bound.
 * @return Exactly represented local size.
 * @throws std::overflow_error when the platform cannot represent the value.
 */
std::size_t resource_size(std::uint64_t value) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error("worker artifact resource bound is unsupported");
  }
  return static_cast<std::size_t>(value);
}

/**
 * @brief Returns the exact output/staging/retention intersection.
 * @param resources Valid immutable Job resource request.
 * @return Positive local tight-byte maximum.
 * @throws std::overflow_error when the platform cannot represent the minimum.
 */
std::size_t output_payload_maximum(const JobResourceRequest& resources) {
  const std::uint64_t maximum =
      std::min({resources.output_bytes, resources.staging_bytes,
                resources.retention_bytes});
  return resource_size(maximum);
}

/**
 * @brief Derives one fixed-width non-authorizing reference from exact fields.
 * @param identity Complete current assignment identity.
 * @param direction Stable `checkpoint-read` or `output-stage` token.
 * @param resource Exact ArtifactId or output-slot value.
 * @return `worker-data-v1.` plus 64 lowercase SHA-256 hexadecimal bytes.
 * @throws Contract, allocation, or hash overflow failures unchanged.
 */
std::string data_plane_reference(const AttemptIdentity& identity,
                                 const std::string& direction,
                                 const std::string& resource) {
  validate_attempt_identity(identity);
  std::string canonical = "worker-data-v1\n";
  canonical.append(direction);
  canonical.push_back('\n');
  canonical.append(identity.tenant_id.value());
  canonical.push_back('\n');
  canonical.append(identity.job_id.value());
  canonical.push_back('\n');
  canonical.append(identity.job_spec_digest.hex());
  canonical.push_back('\n');
  canonical.append(identity.attempt_id.value());
  canonical.push_back('\n');
  canonical.append(identity.worker_instance_id.value());
  canonical.push_back('\n');
  canonical.append(std::to_string(identity.worker_lease_generation.value));
  canonical.push_back('\n');
  canonical.append(resource);
  const JobSpecDigest digest = hash_job_spec_bytes(
      reinterpret_cast<const std::byte*>(canonical.data()), canonical.size());
  std::string reference = "worker-data-v1.";
  reference.append(digest.hex());
  return reference;
}

/**
 * @brief Validates one tight artifact image descriptor without allocation.
 * @param descriptor Candidate descriptor.
 * @return Nothing when dimensions, type, row bytes, and payload agree.
 * @throws std::invalid_argument for invalid shape or checked overflow.
 */
void validate_tight_descriptor(const ArtifactImageDescriptor& descriptor) {
  if (descriptor.width <= 0 || descriptor.height <= 0 ||
      descriptor.channels <= 0) {
    throw std::invalid_argument("worker artifact descriptor is empty");
  }
  const std::size_t width = static_cast<std::size_t>(descriptor.width);
  const std::size_t height = static_cast<std::size_t>(descriptor.height);
  const std::size_t channels = static_cast<std::size_t>(descriptor.channels);
  const std::size_t channel_bytes =
      image_buffer_bytes_per_channel(descriptor.type);
  if (width > std::numeric_limits<std::size_t>::max() / channels ||
      width * channels >
          std::numeric_limits<std::size_t>::max() / channel_bytes) {
    throw std::invalid_argument("worker artifact row size overflowed");
  }
  const std::size_t row_bytes = width * channels * channel_bytes;
  if (row_bytes > std::numeric_limits<std::size_t>::max() / height ||
      descriptor.row_bytes != row_bytes ||
      descriptor.payload_bytes != row_bytes * height) {
    throw std::invalid_argument("worker artifact descriptor is inconsistent");
  }
}

/**
 * @brief Validates every typed field in one durable artifact receipt.
 * @param receipt Candidate metadata-only checkpoint receipt.
 * @return Nothing when provenance identity, typed ids, descriptor, and
 * durability are complete and valid.
 * @throws std::invalid_argument for incomplete or invalid receipt metadata.
 * @throws std::overflow_error for impossible descriptor arithmetic.
 * @note Content bytes and digest agreement are checked separately at the
 * manager and worker data-plane boundaries.
 */
void validate_artifact_receipt_metadata(const OutputCommitReceipt& receipt) {
  validate_attempt_identity(receipt.attempt);
  if (!receipt.output_slot_id.valid() || !receipt.artifact_id.valid() ||
      !receipt.output_commit_id.valid() ||
      receipt.achieved_durability != ArtifactDurability::CrashDurable) {
    throw std::invalid_argument(
        "worker artifact receipt identity or durability is invalid");
  }
  validate_tight_descriptor(receipt.descriptor);
}

/**
 * @brief Builds one exact Assignment metadata contract.
 * @param assignment Valid manager-owned assignment and optional checkpoint.
 * @return Complete deterministic references and output bound.
 * @throws Validation, allocation, and hashing failures unchanged.
 */
WorkerDataPlaneAssignment make_assignment_metadata(
    const JobAssignment& assignment) {
  validate_attempt_identity(assignment.identity);
  if (assignment.spec == nullptr) {
    throw std::invalid_argument("worker data-plane assignment has no JobSpec");
  }
  validate_job_spec(*assignment.spec);
  if (assignment.spec->digest() != assignment.identity.job_spec_digest) {
    throw std::invalid_argument(
        "worker data-plane JobSpec digest does not join identity");
  }
  const bool checkpoint_declared =
      assignment.spec->checkpoint_artifact_id().has_value();
  if (checkpoint_declared != (assignment.checkpoint != nullptr)) {
    throw std::invalid_argument(
        "worker data-plane checkpoint binding is incomplete");
  }

  WorkerDataPlaneAssignment metadata;
  if (assignment.checkpoint != nullptr) {
    const ArtifactRecord& checkpoint = *assignment.checkpoint;
    validate_tight_descriptor(checkpoint.receipt.descriptor);
    if (checkpoint.receipt.attempt.tenant_id != assignment.identity.tenant_id ||
        checkpoint.receipt.artifact_id !=
            *assignment.spec->checkpoint_artifact_id() ||
        checkpoint.receipt.achieved_durability !=
            ArtifactDurability::CrashDurable ||
        checkpoint.receipt.descriptor.payload_bytes !=
            checkpoint.payload.size() ||
        checkpoint.payload.size() >
            resource_size(
                assignment.spec->resource_request().host_memory_bytes) ||
        checkpoint.receipt.content_digest !=
            hash_artifact_content(checkpoint.payload.data(),
                                  checkpoint.payload.size())) {
      throw std::invalid_argument(
          "worker data-plane checkpoint does not match durable authority");
    }
    WorkerCheckpointDataReference reference;
    reference.reference_id =
        data_plane_reference(assignment.identity, "checkpoint-read",
                             checkpoint.receipt.artifact_id.value());
    reference.receipt = checkpoint.receipt;
    metadata.checkpoint = std::move(reference);
  }
  metadata.output.reference_id =
      data_plane_reference(assignment.identity, "output-stage",
                           assignment.spec->output_slot_id().value());
  metadata.output.output_slot_id = assignment.spec->output_slot_id();
  metadata.output.maximum_payload_bytes =
      output_payload_maximum(assignment.spec->resource_request());
  validate_worker_data_plane_assignment(assignment.identity, *assignment.spec,
                                        metadata);
  return metadata;
}

/**
 * @brief Opens an anonymous read-only occurrence containing exact bytes.
 * @param bytes Immutable checkpoint bytes to prepopulate.
 * @return Sole read-only descriptor owner after name unlink.
 * @throws Setup, write, truncation, and cleanup failures unchanged.
 */
ScopedDescriptor create_checkpoint_occurrence(
    const std::vector<std::byte>& bytes) {
  ScopedTemporaryOccurrence occurrence = create_private_temporary();
  write_complete_at(occurrence.creation_descriptor(), bytes.data(),
                    bytes.size());
  if (bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
    throw std::overflow_error("worker checkpoint size exceeds off_t");
  }
  if (::ftruncate(occurrence.creation_descriptor(),
                  static_cast<off_t>(bytes.size())) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "truncate worker checkpoint occurrence");
  }
  ScopedDescriptor reader = open_directional(occurrence.path(), O_RDONLY);
  require_same_private_occurrence(occurrence.creation_descriptor(),
                                  reader.get());
  occurrence.unlink_now();
  occurrence.close_creation_descriptor();
  return reader;
}

/**
 * @brief Opens one empty anonymous output occurrence in opposite directions.
 * @param reader Non-null output receiving manager read descriptor ownership.
 * @param writer Non-null output receiving worker write descriptor ownership.
 * @return Nothing after name unlink and creation-descriptor closure.
 * @throws std::invalid_argument for null outputs.
 * @throws Setup and cleanup failures unchanged.
 */
void create_output_occurrence(ScopedDescriptor* reader,
                              ScopedDescriptor* writer) {
  if (reader == nullptr || writer == nullptr) {
    throw std::invalid_argument("worker output occurrence outputs are null");
  }
  ScopedTemporaryOccurrence occurrence = create_private_temporary();
  *reader = open_directional(occurrence.path(), O_RDONLY);
  *writer = open_directional(occurrence.path(), O_WRONLY);
  require_same_private_occurrence(occurrence.creation_descriptor(),
                                  reader->get());
  require_same_private_occurrence(occurrence.creation_descriptor(),
                                  writer->get());
  occurrence.unlink_now();
  occurrence.close_creation_descriptor();
}

}  // namespace

/** @copydoc
 * WorkerArtifactDataPlaneTestAccess::throw_after_private_temporary_creation */
void WorkerArtifactDataPlaneTestAccess::throw_after_private_temporary_creation(
    const std::filesystem::path& directory) {
  static_cast<void>(create_private_temporary(directory, true));
}

/** @copydoc ps::server::validate_worker_data_plane_assignment */
void validate_worker_data_plane_assignment(
    const AttemptIdentity& identity, const JobSpec& spec,
    const WorkerDataPlaneAssignment& data_plane) {
  validate_attempt_identity(identity);
  validate_job_spec(spec);
  if (spec.digest() != identity.job_spec_digest) {
    throw std::invalid_argument(
        "worker data-plane metadata JobSpec digest is inconsistent");
  }
  const std::string expected_output = data_plane_reference(
      identity, "output-stage", spec.output_slot_id().value());
  const std::size_t expected_maximum =
      output_payload_maximum(spec.resource_request());
  if (data_plane.output.reference_id != expected_output ||
      data_plane.output.reference_id.empty() ||
      data_plane.output.reference_id.size() >
          kMaximumWorkerDataPlaneReferenceBytes ||
      data_plane.output.output_slot_id != spec.output_slot_id() ||
      data_plane.output.maximum_payload_bytes != expected_maximum ||
      expected_maximum == 0U) {
    throw std::invalid_argument("worker output-stage metadata is inconsistent");
  }

  const bool checkpoint_declared = spec.checkpoint_artifact_id().has_value();
  if (checkpoint_declared != data_plane.checkpoint.has_value()) {
    throw std::invalid_argument(
        "worker checkpoint data-plane metadata is incomplete");
  }
  if (!checkpoint_declared) {
    return;
  }
  const WorkerCheckpointDataReference& checkpoint = *data_plane.checkpoint;
  const OutputCommitReceipt& receipt = checkpoint.receipt;
  validate_artifact_receipt_metadata(receipt);
  const std::string expected_reference = data_plane_reference(
      identity, "checkpoint-read", receipt.artifact_id.value());
  if (checkpoint.reference_id != expected_reference ||
      checkpoint.reference_id.empty() ||
      checkpoint.reference_id.size() > kMaximumWorkerDataPlaneReferenceBytes ||
      receipt.attempt.tenant_id != identity.tenant_id ||
      receipt.artifact_id != *spec.checkpoint_artifact_id() ||
      receipt.achieved_durability != ArtifactDurability::CrashDurable ||
      receipt.descriptor.payload_bytes >
          resource_size(spec.resource_request().host_memory_bytes)) {
    throw std::invalid_argument(
        "worker checkpoint data-plane metadata is inconsistent");
  }
}

/** @copydoc ps::server::WorkerArtifactDataPlane::WorkerArtifactDataPlane */
WorkerArtifactDataPlane::WorkerArtifactDataPlane(
    int checkpoint_descriptor, int output_descriptor,
    int output_reader_descriptor, WorkerDataPlaneAssignment metadata) noexcept
    : worker_checkpoint_descriptor_(checkpoint_descriptor),
      worker_output_descriptor_(output_descriptor),
      manager_output_descriptor_(output_reader_descriptor),
      assignment_metadata_(std::move(metadata)) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ps::server::WorkerArtifactDataPlane::create */
WorkerArtifactDataPlane WorkerArtifactDataPlane::create(
    const JobAssignment& assignment) {
  WorkerDataPlaneAssignment metadata = make_assignment_metadata(assignment);
  const std::vector<std::byte> empty;
  ScopedDescriptor checkpoint = create_checkpoint_occurrence(
      assignment.checkpoint == nullptr ? empty
                                       : assignment.checkpoint->payload);
  ScopedDescriptor output_reader;
  ScopedDescriptor output_writer;
  create_output_occurrence(&output_reader, &output_writer);
  return WorkerArtifactDataPlane(checkpoint.release(), output_writer.release(),
                                 output_reader.release(), std::move(metadata));
}

/** @copydoc ps::server::WorkerArtifactDataPlane::~WorkerArtifactDataPlane */
WorkerArtifactDataPlane::~WorkerArtifactDataPlane() noexcept {
  close_worker_descriptors();
  close_once(&manager_output_descriptor_);
}

/** @copydoc ps::server::WorkerArtifactDataPlane::WorkerArtifactDataPlane */
WorkerArtifactDataPlane::WorkerArtifactDataPlane(
    WorkerArtifactDataPlane&& other) noexcept
    : worker_checkpoint_descriptor_(
          std::exchange(other.worker_checkpoint_descriptor_, -1)),
      worker_output_descriptor_(
          std::exchange(other.worker_output_descriptor_, -1)),
      manager_output_descriptor_(
          std::exchange(other.manager_output_descriptor_, -1)),
      assignment_metadata_(std::move(other.assignment_metadata_)) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ps::server::WorkerArtifactDataPlane::operator= */
WorkerArtifactDataPlane& WorkerArtifactDataPlane::operator=(
    WorkerArtifactDataPlane&& other) noexcept {
  if (this != &other) {
    close_worker_descriptors();
    close_once(&manager_output_descriptor_);
    worker_checkpoint_descriptor_ =
        std::exchange(other.worker_checkpoint_descriptor_, -1);
    worker_output_descriptor_ =
        std::exchange(other.worker_output_descriptor_, -1);
    manager_output_descriptor_ =
        std::exchange(other.manager_output_descriptor_, -1);
    assignment_metadata_ = std::move(other.assignment_metadata_);
  }
  return *this;
}

/** @copydoc ps::server::WorkerArtifactDataPlane::close_worker_descriptors */
void WorkerArtifactDataPlane::close_worker_descriptors() noexcept {
  close_once(&worker_checkpoint_descriptor_);
  close_once(&worker_output_descriptor_);
}

/** @copydoc ps::server::WorkerArtifactDataPlane::materialize_report */
JobAttemptReport WorkerArtifactDataPlane::materialize_report(
    JobAttemptReport report,
    const std::optional<WorkerOutputDataReference>& output) const {
  require_access_direction(manager_output_descriptor_, O_RDONLY);
  const std::size_t file_size =
      private_anonymous_file_size(manager_output_descriptor_);
  if (!output.has_value()) {
    if (file_size != 0U) {
      throw WorkerArtifactDataPlaneError(
          "image-free worker report left output-stage bytes");
    }
    return report;
  }
  if (report.image.has_value() ||
      report.outcome != JobAttemptOutcome::Succeeded || !report.settled ||
      report.failure != JobAttemptFailure::None ||
      output->reference_id != assignment_metadata_.output.reference_id ||
      output->output_slot_id != assignment_metadata_.output.output_slot_id) {
    throw WorkerArtifactDataPlaneError(
        "worker output metadata does not join its assigned stage");
  }
  try {
    validate_tight_descriptor(output->descriptor);
  } catch (const std::exception& error) {
    throw WorkerArtifactDataPlaneError(
        std::string("worker output descriptor is invalid: ") + error.what());
  }
  if (output->descriptor.payload_bytes >
          assignment_metadata_.output.maximum_payload_bytes ||
      output->descriptor.payload_bytes != file_size) {
    throw WorkerArtifactDataPlaneError(
        "worker output-stage size exceeds or differs from metadata");
  }
  ImageBuffer image = make_aligned_cpu_image_buffer(
      output->descriptor.width, output->descriptor.height,
      output->descriptor.channels, output->descriptor.type, 64U);
  std::size_t offset = 0U;
  for (int row = 0; row < output->descriptor.height; ++row) {
    read_complete_at_into(manager_output_descriptor_,
                          static_cast<std::byte*>(image.data.get()) +
                              static_cast<std::size_t>(row) * image.step,
                          output->descriptor.row_bytes, offset);
    offset += output->descriptor.row_bytes;
  }
  if (hash_image_artifact_content(image) != output->content_digest) {
    throw WorkerArtifactDataPlaneError(
        "worker output-stage content digest is inconsistent");
  }
  report.image = std::move(image);
  return report;
}

/** @copydoc ps::server::materialize_worker_checkpoint */
std::shared_ptr<const ArtifactRecord> materialize_worker_checkpoint(
    int checkpoint_descriptor, const JobAssignment& assignment,
    const WorkerDataPlaneAssignment& data_plane) {
  if (assignment.checkpoint != nullptr || assignment.spec == nullptr) {
    throw WorkerArtifactDataPlaneError(
        "worker checkpoint materialization state is invalid");
  }
  try {
    validate_worker_data_plane_assignment(assignment.identity, *assignment.spec,
                                          data_plane);
  } catch (const std::exception& error) {
    throw WorkerArtifactDataPlaneError(
        std::string("worker checkpoint metadata is invalid: ") + error.what());
  }
  require_access_direction(checkpoint_descriptor, O_RDONLY);
  const std::size_t file_size =
      private_anonymous_file_size(checkpoint_descriptor);
  if (!data_plane.checkpoint.has_value()) {
    if (file_size != 0U) {
      throw WorkerArtifactDataPlaneError(
          "checkpoint-free assignment received data-plane bytes");
    }
    return nullptr;
  }
  const OutputCommitReceipt& receipt = data_plane.checkpoint->receipt;
  if (file_size != receipt.descriptor.payload_bytes) {
    throw WorkerArtifactDataPlaneError(
        "worker checkpoint file size differs from receipt");
  }
  std::vector<std::byte> payload =
      read_complete_at(checkpoint_descriptor, file_size);
  if (hash_artifact_content(payload.data(), payload.size()) !=
      receipt.content_digest) {
    throw WorkerArtifactDataPlaneError(
        "worker checkpoint content digest is inconsistent");
  }
  ArtifactRecord record;
  record.receipt = receipt;
  record.payload = std::move(payload);
  return std::make_shared<const ArtifactRecord>(std::move(record));
}

/** @copydoc ps::server::stage_worker_output */
std::optional<WorkerOutputDataReference> stage_worker_output(
    int output_descriptor, const JobSpec& spec,
    const WorkerOutputStageReference& output_stage, JobAttemptReport* report) {
  if (report == nullptr) {
    throw std::invalid_argument("worker output report is null");
  }
  validate_attempt_identity(report->identity);
  validate_job_spec(spec);
  const std::string expected_reference = data_plane_reference(
      report->identity, "output-stage", spec.output_slot_id().value());
  if (report->identity.job_spec_digest != spec.digest() ||
      output_stage.reference_id != expected_reference ||
      output_stage.output_slot_id != spec.output_slot_id() ||
      output_stage.maximum_payload_bytes !=
          output_payload_maximum(spec.resource_request())) {
    throw std::invalid_argument("worker output stage does not join assignment");
  }
  require_access_direction(output_descriptor, O_WRONLY);
  static_cast<void>(private_anonymous_file_size(output_descriptor));

  if (!report->image.has_value()) {
    if (::ftruncate(output_descriptor, 0) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "clear image-free worker output stage");
    }
    return std::nullopt;
  }
  if (report->outcome != JobAttemptOutcome::Succeeded || !report->settled ||
      report->failure != JobAttemptFailure::None) {
    throw std::invalid_argument(
        "only a settled successful report may stage an image");
  }
  const ImageBuffer& image = *report->image;
  validate_image_buffer(image);
  if (image.device != Device::CPU || image.width <= 0 || image.height <= 0 ||
      image.channels <= 0 || image.data == nullptr) {
    throw std::invalid_argument("worker output image is not nonempty CPU data");
  }
  const std::size_t row_bytes = image_buffer_row_bytes(image);
  if (row_bytes > std::numeric_limits<std::size_t>::max() /
                      static_cast<std::size_t>(image.height)) {
    throw std::overflow_error("worker output image size overflowed");
  }
  const std::size_t payload_bytes =
      row_bytes * static_cast<std::size_t>(image.height);
  if (payload_bytes > output_stage.maximum_payload_bytes) {
    if (::ftruncate(output_descriptor, 0) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "clear oversized worker output stage");
    }
    report->outcome = JobAttemptOutcome::Failed;
    report->settled = true;
    report->failure = JobAttemptFailure::Compute;
    report->message =
        "worker candidate image exceeds accepted artifact data-plane bounds";
    report->image.reset();
    return std::nullopt;
  }
  if (payload_bytes >
      static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
    throw std::overflow_error("worker output stage size exceeds off_t");
  }
  if (::ftruncate(output_descriptor, 0) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "reset worker output stage");
  }
  std::size_t offset = 0U;
  for (int row = 0; row < image.height; ++row) {
    write_complete_at(output_descriptor, image_buffer_row_data(image, row),
                      row_bytes, offset);
    offset += row_bytes;
  }
  if (::ftruncate(output_descriptor, static_cast<off_t>(payload_bytes)) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "finalize worker output stage size");
  }

  WorkerOutputDataReference output;
  output.reference_id = output_stage.reference_id;
  output.output_slot_id = output_stage.output_slot_id;
  output.descriptor.width = image.width;
  output.descriptor.height = image.height;
  output.descriptor.channels = image.channels;
  output.descriptor.type = image.type;
  output.descriptor.row_bytes = row_bytes;
  output.descriptor.payload_bytes = payload_bytes;
  output.content_digest = hash_image_artifact_content(image);
  report->image.reset();
  return output;
}

}  // namespace ps::server
