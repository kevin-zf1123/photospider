/**
 * @file durable_server_state.cpp
 * @brief Implements Issue #99 durable Job and image-artifact authority.
 */
#include "server/durable_server_state.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace ps::server {
namespace {

/** @brief Fixed control-plane namespace below the trusted state root. */
constexpr char kControlDirectory[] = "control";
/** @brief Fixed durable Job-record namespace below `control`. */
constexpr char kJobsDirectory[] = "jobs";
/** @brief Fixed artifact namespace below the trusted state root. */
constexpr char kArtifactsDirectory[] = "artifacts";
/** @brief Fixed tight image-payload leaf within one artifact directory. */
constexpr char kPayloadName[] = "payload.bin";
/** @brief Fixed authoritative manifest-last artifact publication leaf. */
constexpr char kManifestName[] = "manifest";
/** @brief Fixed synchronized manifest staging leaf before publication. */
constexpr char kPrivateManifestName[] = ".manifest.private";
/** @brief Fixed suffix identifying authoritative durable Job records. */
constexpr char kJobRecordSuffix[] = ".record";
/** @brief Exact canonical version prefix for artifact manifests. */
constexpr char kArtifactManifestVersion[] = "photospider-artifact-manifest-v1";
/** @brief Exact canonical version prefix for durable Job records. */
constexpr char kJobRecordVersion[] = "photospider-job-record-v1";

/**
 * @brief Minimal unique owner for one POSIX descriptor.
 * @throws Nothing for lifecycle operations; an unexpected close failure in
 * destruction terminates to avoid silently losing durability authority.
 */
class ScopedDescriptor final {
 public:
  /**
   * @brief Takes ownership of one descriptor or an invalid sentinel.
   * @param descriptor Open descriptor to own, or `-1` for no ownership.
   * @throws Nothing.
   */
  explicit ScopedDescriptor(int descriptor = -1) noexcept
      : descriptor_(descriptor) {}

  /**
   * @brief Closes the owned descriptor, terminating on an unexpected failure.
   * @throws Nothing; close failure terminates because durability authority
   * cannot be reported from destruction.
   */
  ~ScopedDescriptor() noexcept {
    if (descriptor_ >= 0 && ::close(descriptor_) != 0) {
      std::terminate();
    }
  }

  /**
   * @brief Prevents duplicate descriptor ownership.
   * @param other Owner that cannot be copied.
   */
  ScopedDescriptor(const ScopedDescriptor& other) = delete;
  /**
   * @brief Prevents duplicate descriptor assignment.
   * @param other Owner that cannot be copied.
   * @return No assignment result because the operation is deleted.
   */
  ScopedDescriptor& operator=(const ScopedDescriptor& other) = delete;

  /**
   * @brief Returns the borrowed descriptor without transferring ownership.
   * @return Open descriptor or `-1` when this owner is empty.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Releases ownership without closing the descriptor.
   * @return Previously owned descriptor, or `-1` when already empty.
   * @throws Nothing.
   */
  int release() noexcept {
    const int result = descriptor_;
    descriptor_ = -1;
    return result;
  }

 private:
  /** @brief Owned descriptor or `-1`. */
  int descriptor_ = -1;
};

/**
 * @brief Throws `std::system_error` for the current `errno`.
 * @param operation Stable failed-operation description.
 * @throws std::system_error always.
 */
[[noreturn]] void throw_errno(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

/**
 * @brief Synchronizes one file/directory for crash durability.
 * @param descriptor Valid open descriptor.
 * @param kind Stable object-kind diagnostic.
 * @return Nothing after the barrier succeeds.
 * @throws DurableCapabilityError when the platform rejects the primitive.
 * @throws std::system_error for other failures.
 */
void synchronize_descriptor(int descriptor, const char* kind) {
  if (::fsync(descriptor) == 0) {
    return;
  }
  if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {
    throw DurableCapabilityError(std::string(kind) +
                                 " crash-durability barrier is unsupported");
  }
  throw_errno("fsync durable server state");
}

/**
 * @brief Returns a regular-file stat or fails closed.
 * @param descriptor Open candidate file descriptor.
 * @param operation Stable diagnostic operation.
 * @return Exact stat value.
 * @throws DurableCorruptionError when the descriptor is not regular.
 * @throws std::system_error when `fstat` fails.
 */
struct stat regular_file_stat(int descriptor, const char* operation) {
  struct stat value{};
  if (::fstat(descriptor, &value) != 0) {
    throw_errno(operation);
  }
  if (!S_ISREG(value.st_mode) || value.st_size < 0) {
    throw DurableCorruptionError("durable state leaf is not a regular file");
  }
  return value;
}

/**
 * @brief Returns a directory stat or fails closed.
 * @param descriptor Open candidate directory descriptor.
 * @param operation Stable diagnostic operation.
 * @return Exact stat value.
 * @throws DurableCorruptionError when the descriptor is not a directory.
 * @throws std::system_error when `fstat` fails.
 */
struct stat directory_stat(int descriptor, const char* operation) {
  struct stat value{};
  if (::fstat(descriptor, &value) != 0) {
    throw_errno(operation);
  }
  if (!S_ISDIR(value.st_mode)) {
    throw DurableCorruptionError("durable state node is not a directory");
  }
  return value;
}

/**
 * @brief Writes every byte to one open private file.
 * @param descriptor Writable file descriptor.
 * @param bytes Borrowed bytes, null only when size is zero.
 * @param size Exact byte count.
 * @return Nothing after complete write.
 * @throws std::system_error for write failure.
 * @throws DurableStateError when write makes no progress.
 */
void write_all(int descriptor, const std::byte* bytes, std::size_t size) {
  while (size != 0U) {
    const std::size_t chunk = std::min(
        size, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t written = ::write(descriptor, bytes, chunk);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("write durable server state");
    }
    if (written == 0) {
      throw DurableStateError("durable state write made no progress");
    }
    bytes += written;
    size -= static_cast<std::size_t>(written);
  }
}

/**
 * @brief Opens and reads one exact no-follow regular file below a directory.
 * @param parent_descriptor Held trusted parent directory.
 * @param name Single opaque/fixed leaf name.
 * @return Exact file bytes.
 * @throws DurableCorruptionError for invalid type/size/drift.
 * @throws std::system_error/allocation failures unchanged.
 */
std::vector<std::byte> read_file_at(int parent_descriptor,
                                    const std::string& name) {
  const int raw = ::openat(parent_descriptor, name.c_str(),
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (raw < 0) {
    if (errno == ELOOP || errno == ENOTDIR) {
      throw DurableCorruptionError(
          "durable state leaf traverses a non-file entry");
    }
    throw_errno("open durable state leaf");
  }
  ScopedDescriptor descriptor(raw);
  const struct stat value =
      regular_file_stat(descriptor.get(), "fstat durable state leaf");
  if (static_cast<std::uintmax_t>(value.st_size) >
      std::numeric_limits<std::size_t>::max()) {
    throw DurableCorruptionError("durable state leaf is too large");
  }
  std::vector<std::byte> result(static_cast<std::size_t>(value.st_size));
  std::size_t offset = 0U;
  while (offset < result.size()) {
    const ssize_t count = ::read(descriptor.get(), result.data() + offset,
                                 result.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("read durable state leaf");
    }
    if (count == 0) {
      throw DurableCorruptionError("durable state leaf shortened during read");
    }
    offset += static_cast<std::size_t>(count);
  }
  const struct stat after =
      regular_file_stat(descriptor.get(), "refstat durable state leaf");
  if (after.st_dev != value.st_dev || after.st_ino != value.st_ino ||
      after.st_size != value.st_size) {
    throw DurableCorruptionError("durable state leaf identity drifted");
  }
  return result;
}

/**
 * @brief Converts exact byte storage to a copied string.
 * @param bytes Exact byte vector.
 * @return String with identical byte sequence.
 * @throws std::bad_alloc when allocation fails.
 */
std::string bytes_to_string(const std::vector<std::byte>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/**
 * @brief Lists exact entry names under a held directory.
 * @param descriptor Held directory capability.
 * @return Sorted names excluding dot entries.
 * @throws std::system_error/allocation failures unchanged.
 */
std::vector<std::string> list_directory(int descriptor) {
  const int duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
  if (duplicate < 0) {
    throw_errno("duplicate durable state directory");
  }
  DIR* stream = ::fdopendir(duplicate);
  if (stream == nullptr) {
    const int saved = errno;
    static_cast<void>(::close(duplicate));
    errno = saved;
    throw_errno("open durable state directory stream");
  }
  std::unique_ptr<DIR, int (*)(DIR*)> owner(stream, ::closedir);
  std::vector<std::string> result;
  for (;;) {
    errno = 0;
    const struct dirent* entry = ::readdir(owner.get());
    if (entry == nullptr) {
      if (errno != 0) {
        throw_errno("read durable state directory");
      }
      break;
    }
    const std::string_view name(entry->d_name);
    if (name != "." && name != "..") {
      result.emplace_back(name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

/**
 * @brief Opens or creates one fixed child directory without following links.
 * @param parent Held trusted parent directory.
 * @param name Fixed child name.
 * @return Newly owned directory descriptor.
 * @throws DurableCorruptionError for non-directory/symlink state.
 * @throws std::system_error for creation/open failures.
 */
int open_or_create_directory(int parent, const char* name) {
  if (::mkdirat(parent, name, S_IRWXU) != 0 && errno != EEXIST) {
    throw_errno("create durable state directory");
  }
  const int descriptor =
      ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ELOOP || errno == ENOTDIR) {
      throw DurableCorruptionError(
          "durable state namespace is not a no-follow directory");
    }
    throw_errno("open durable state directory");
  }
  try {
    static_cast<void>(
        directory_stat(descriptor, "fstat durable state directory"));
  } catch (...) {
    static_cast<void>(::close(descriptor));
    throw;
  }
  return descriptor;
}

/**
 * @brief Revalidates that the configured path still names the held root fd.
 * @param root Canonical retained root path.
 * @param descriptor Held root descriptor.
 * @param device Constructor-time device identity.
 * @param inode Constructor-time inode identity.
 * @return Nothing while the binding remains exact.
 * @throws DurableCorruptionError for disappearance/replacement/symlink drift.
 */
void verify_root_binding(const std::filesystem::path& root, int descriptor,
                         std::uint64_t device, std::uint64_t inode) {
  struct stat held{};
  struct stat named{};
  if (::fstat(descriptor, &held) != 0 || ::lstat(root.c_str(), &named) != 0 ||
      !S_ISDIR(held.st_mode) || !S_ISDIR(named.st_mode) ||
      static_cast<std::uint64_t>(held.st_dev) != device ||
      static_cast<std::uint64_t>(held.st_ino) != inode ||
      held.st_dev != named.st_dev || held.st_ino != named.st_ino) {
    throw DurableCorruptionError(
        "durable state root path no longer names the held directory");
  }
}

/**
 * @brief Appends one decimal-length-framed field.
 * @param field Exact borrowed field bytes.
 * @param output Non-null destination.
 * @return Nothing.
 * @throws std::invalid_argument for null output.
 * @throws std::bad_alloc when growth fails.
 */
void append_frame(std::string_view field, std::string* output) {
  if (output == nullptr) {
    throw std::invalid_argument("durable canonical output is null");
  }
  output->append(std::to_string(field.size()));
  output->push_back(':');
  output->append(field.data(), field.size());
}

/**
 * @brief Strict reader for decimal-length-framed canonical storage.
 * @throws DurableCorruptionError for malformed/noncanonical input.
 * @note Returned views borrow the constructor input for this reader's lifetime.
 */
class FrameReader final {
 public:
  /**
   * @brief Begins reading after one exact required version prefix.
   * @param input Complete canonical storage borrowed for this reader's
   * lifetime.
   * @param version Exact required leading version token.
   * @throws DurableCorruptionError when the version prefix differs.
   */
  FrameReader(std::string_view input, std::string_view version)
      : input_(input), offset_(version.size()) {
    if (input_.substr(0U, version.size()) != version) {
      throw DurableCorruptionError("durable canonical version is invalid");
    }
  }

  /**
   * @brief Reads the next exact canonical frame.
   * @return Borrowed frame content.
   * @throws DurableCorruptionError for invalid length syntax or bounds.
   */
  std::string_view next() {
    if (offset_ >= input_.size()) {
      throw DurableCorruptionError("durable canonical frame is missing");
    }
    const std::size_t digits_begin = offset_;
    std::size_t length = 0U;
    while (offset_ < input_.size() && input_[offset_] != ':') {
      const char character = input_[offset_];
      if (character < '0' || character > '9' ||
          (offset_ == digits_begin + 1U && input_[digits_begin] == '0')) {
        throw DurableCorruptionError("durable frame length is noncanonical");
      }
      const std::size_t digit = static_cast<std::size_t>(character - '0');
      if (length > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
        throw DurableCorruptionError("durable frame length overflowed");
      }
      length = length * 10U + digit;
      ++offset_;
    }
    if (offset_ == digits_begin || offset_ >= input_.size()) {
      throw DurableCorruptionError("durable frame length delimiter is missing");
    }
    ++offset_;
    if (length > input_.size() - offset_) {
      throw DurableCorruptionError("durable frame exceeds retained bytes");
    }
    const std::string_view result = input_.substr(offset_, length);
    offset_ += length;
    return result;
  }

  /**
   * @brief Requires exact end of canonical storage.
   * @return Nothing.
   * @throws DurableCorruptionError when trailing bytes remain.
   */
  void finish() const {
    if (offset_ != input_.size()) {
      throw DurableCorruptionError(
          "durable canonical storage has trailing bytes");
    }
  }

 private:
  /** @brief Complete borrowed canonical input. */
  std::string_view input_;
  /** @brief Next unread byte position. */
  std::size_t offset_ = 0U;
};

/**
 * @brief Parses one canonical unsigned integer.
 * @tparam Integer Unsigned destination type.
 * @param text Exact decimal spelling.
 * @return Parsed value.
 * @throws DurableCorruptionError for empty, leading-zero, invalid, or overflow.
 */
template <typename Integer>
Integer parse_unsigned(std::string_view text) {
  static_assert(std::is_unsigned<Integer>::value,
                "durable integer parser requires unsigned type");
  if (text.empty() || (text.size() > 1U && text.front() == '0')) {
    throw DurableCorruptionError("durable unsigned integer is noncanonical");
  }
  Integer value = 0U;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      throw DurableCorruptionError("durable unsigned integer is invalid");
    }
    const Integer digit = static_cast<Integer>(character - '0');
    if (value > (std::numeric_limits<Integer>::max() - digit) / 10U) {
      throw DurableCorruptionError("durable unsigned integer overflowed");
    }
    value = static_cast<Integer>(value * 10U + digit);
  }
  return value;
}

/**
 * @brief Parses one canonical nonnegative `int`.
 * @param text Exact decimal spelling.
 * @return Representable nonnegative integer.
 * @throws DurableCorruptionError for invalid/overflowing input.
 */
int parse_nonnegative_int(std::string_view text) {
  const std::uint64_t value = parse_unsigned<std::uint64_t>(text);
  if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    throw DurableCorruptionError("durable int field overflowed");
  }
  return static_cast<int>(value);
}

/**
 * @brief Parses one canonical boolean token.
 * @param text Exact `0` or `1`.
 * @return Parsed truth value.
 * @throws DurableCorruptionError for any other token.
 */
bool parse_bool(std::string_view text) {
  if (text == "0") {
    return false;
  }
  if (text == "1") {
    return true;
  }
  throw DurableCorruptionError("durable boolean field is invalid");
}

/**
 * @brief Parses exact lowercase SHA-256 hexadecimal.
 * @tparam Digest Strong digest domain.
 * @param text Exactly 64 lowercase hexadecimal characters.
 * @return Typed digest.
 * @throws DurableCorruptionError for invalid spelling.
 */
template <typename Digest>
Digest parse_digest(std::string_view text) {
  if (text.size() != 64U) {
    throw DurableCorruptionError("durable SHA-256 length is invalid");
  }
  Digest digest;
  for (std::size_t index = 0U; index < digest.bytes.size(); ++index) {
    const auto nibble = [](char character) -> std::uint8_t {
      if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
      }
      if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
      }
      throw DurableCorruptionError("durable SHA-256 spelling is invalid");
    };
    digest.bytes[index] = static_cast<std::byte>(
        (nibble(text[index * 2U]) << 4U) | nibble(text[index * 2U + 1U]));
  }
  return digest;
}

/**
 * @brief Validates one closed Job-state representation.
 * @param value Numeric persisted representation.
 * @return Typed state.
 * @throws DurableCorruptionError for unknown values.
 */
JobState parse_job_state(std::uint8_t value) {
  switch (static_cast<JobState>(value)) {
    case JobState::Queued:
    case JobState::Running:
    case JobState::Cancelling:
    case JobState::Succeeded:
    case JobState::Failed:
    case JobState::Cancelled:
      return static_cast<JobState>(value);
  }
  throw DurableCorruptionError("durable Job state is invalid");
}

/**
 * @brief Validates one closed attempt-outcome representation.
 * @param value Numeric persisted representation.
 * @return Typed outcome.
 * @throws DurableCorruptionError for unknown values.
 */
JobAttemptOutcome parse_attempt_outcome(std::uint8_t value) {
  switch (static_cast<JobAttemptOutcome>(value)) {
    case JobAttemptOutcome::None:
    case JobAttemptOutcome::Succeeded:
    case JobAttemptOutcome::Failed:
    case JobAttemptOutcome::Cancelled:
      return static_cast<JobAttemptOutcome>(value);
  }
  throw DurableCorruptionError("durable attempt outcome is invalid");
}

/**
 * @brief Validates one closed attempt-failure representation.
 * @param value Numeric persisted representation.
 * @return Typed failure.
 * @throws DurableCorruptionError for unknown values.
 */
JobAttemptFailure parse_attempt_failure(std::uint8_t value) {
  switch (static_cast<JobAttemptFailure>(value)) {
    case JobAttemptFailure::None:
    case JobAttemptFailure::InvalidAssignment:
    case JobAttemptFailure::GraphResolution:
    case JobAttemptFailure::HostSetup:
    case JobAttemptFailure::GraphLoad:
    case JobAttemptFailure::Compute:
    case JobAttemptFailure::Settlement:
    case JobAttemptFailure::CancellationObserved:
    case JobAttemptFailure::Unexpected:
    case JobAttemptFailure::ReportRejected:
    case JobAttemptFailure::ArtifactCommit:
    case JobAttemptFailure::RecoveryInterrupted:
      return static_cast<JobAttemptFailure>(value);
  }
  throw DurableCorruptionError("durable attempt failure is invalid");
}

/**
 * @brief Converts exact canonical bytes into one non-owning byte view.
 * @param text Borrowed bytes.
 * @return Pointer suitable for hashing/writing, null only when empty.
 * @throws Nothing.
 */
const std::byte* as_bytes(std::string_view text) noexcept {
  return reinterpret_cast<const std::byte*>(text.data());
}

/**
 * @brief Serializes one identity-complete artifact receipt manifest.
 * @param receipt Valid receipt.
 * @return Exact canonical manifest bytes.
 * @throws std::bad_alloc when construction fails.
 */
std::string serialize_artifact_manifest(const OutputCommitReceipt& receipt) {
  std::string output{kArtifactManifestVersion};
  append_frame(receipt.attempt.tenant_id.value(), &output);
  append_frame(receipt.attempt.job_id.value(), &output);
  append_frame(receipt.attempt.job_spec_digest.hex(), &output);
  append_frame(receipt.attempt.attempt_id.value(), &output);
  append_frame(receipt.attempt.worker_instance_id.value(), &output);
  append_frame(std::to_string(receipt.attempt.worker_lease_generation.value),
               &output);
  append_frame(receipt.output_slot_id.value(), &output);
  append_frame(receipt.artifact_id.value(), &output);
  append_frame(receipt.output_commit_id.value(), &output);
  append_frame(std::to_string(receipt.descriptor.width), &output);
  append_frame(std::to_string(receipt.descriptor.height), &output);
  append_frame(std::to_string(receipt.descriptor.channels), &output);
  append_frame(
      std::to_string(static_cast<std::uint32_t>(receipt.descriptor.type)),
      &output);
  append_frame(std::to_string(receipt.descriptor.row_bytes), &output);
  append_frame(std::to_string(receipt.descriptor.payload_bytes), &output);
  append_frame(receipt.content_digest.hex(), &output);
  append_frame("crash-durable", &output);
  return output;
}

/**
 * @brief Parses one strict artifact manifest.
 * @param bytes Exact manifest bytes.
 * @return Identity-complete typed receipt.
 * @throws DurableCorruptionError for malformed fields or identity drift.
 * @throws std::bad_alloc when retained value construction exhausts memory.
 */
OutputCommitReceipt parse_artifact_manifest(std::string_view bytes) {
  try {
    FrameReader reader(bytes, kArtifactManifestVersion);
    OutputCommitReceipt receipt;
    receipt.attempt.tenant_id = TenantId(std::string(reader.next()));
    receipt.attempt.job_id = JobId(std::string(reader.next()));
    receipt.attempt.job_spec_digest =
        parse_digest<JobSpecDigest>(reader.next());
    receipt.attempt.attempt_id = JobAttemptId(std::string(reader.next()));
    receipt.attempt.worker_instance_id =
        WorkerInstanceId(std::string(reader.next()));
    receipt.attempt.worker_lease_generation.value =
        parse_unsigned<std::uint64_t>(reader.next());
    receipt.output_slot_id = OutputSlotId(std::string(reader.next()));
    receipt.artifact_id = ArtifactId(std::string(reader.next()));
    receipt.output_commit_id = OutputCommitId(std::string(reader.next()));
    receipt.descriptor.width = parse_nonnegative_int(reader.next());
    receipt.descriptor.height = parse_nonnegative_int(reader.next());
    receipt.descriptor.channels = parse_nonnegative_int(reader.next());
    const std::uint32_t type = parse_unsigned<std::uint32_t>(reader.next());
    if (type > static_cast<std::uint32_t>(DataType::FLOAT64)) {
      throw DurableCorruptionError("artifact manifest data type is invalid");
    }
    receipt.descriptor.type = static_cast<DataType>(type);
    receipt.descriptor.row_bytes = parse_unsigned<std::size_t>(reader.next());
    receipt.descriptor.payload_bytes =
        parse_unsigned<std::size_t>(reader.next());
    receipt.content_digest = parse_digest<ArtifactContentDigest>(reader.next());
    if (reader.next() != "crash-durable") {
      throw DurableCorruptionError("artifact durability token is invalid");
    }
    reader.finish();
    receipt.achieved_durability = ArtifactDurability::CrashDurable;
    validate_attempt_identity(receipt.attempt);
    if (!receipt.output_slot_id.valid() || !receipt.artifact_id.valid() ||
        !receipt.output_commit_id.valid() || receipt.descriptor.width <= 0 ||
        receipt.descriptor.height <= 0 || receipt.descriptor.channels <= 0 ||
        receipt.descriptor.row_bytes == 0U ||
        receipt.descriptor.payload_bytes == 0U) {
      throw DurableCorruptionError("artifact manifest has an invalid field");
    }
    const std::size_t channel_bytes =
        image_buffer_bytes_per_channel(receipt.descriptor.type);
    const std::uint64_t width =
        static_cast<std::uint64_t>(receipt.descriptor.width);
    const std::uint64_t channels =
        static_cast<std::uint64_t>(receipt.descriptor.channels);
    if (width > std::numeric_limits<std::uint64_t>::max() / channels ||
        width * channels >
            std::numeric_limits<std::uint64_t>::max() / channel_bytes) {
      throw DurableCorruptionError("artifact manifest row size overflowed");
    }
    const std::uint64_t expected_row = width * channels * channel_bytes;
    if (expected_row != receipt.descriptor.row_bytes ||
        receipt.descriptor.row_bytes >
            std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(receipt.descriptor.height) ||
        receipt.descriptor.row_bytes *
                static_cast<std::size_t>(receipt.descriptor.height) !=
            receipt.descriptor.payload_bytes) {
      throw DurableCorruptionError(
          "artifact manifest descriptor is inconsistent");
    }
    return receipt;
  } catch (const DurableCorruptionError&) {
    throw;
  } catch (const std::invalid_argument& error) {
    throw DurableCorruptionError(std::string("invalid artifact manifest: ") +
                                 error.what());
  } catch (const std::overflow_error& error) {
    throw DurableCorruptionError(std::string("invalid artifact manifest: ") +
                                 error.what());
  }
}

/**
 * @brief Opens one child artifact directory without following links.
 * @param artifacts_descriptor Held artifacts namespace.
 * @param artifact_id Valid opaque identity used as one leaf name.
 * @return Newly owned directory descriptor.
 * @throws std::system_error for open failure.
 * @throws DurableCorruptionError for type mismatch.
 */
int open_artifact_directory(int artifacts_descriptor,
                            const ArtifactId& artifact_id) {
  const int descriptor =
      ::openat(artifacts_descriptor, artifact_id.value().c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ELOOP || errno == ENOTDIR) {
      throw DurableCorruptionError(
          "durable artifact entry is not a no-follow directory");
    }
    throw_errno("open durable artifact directory");
  }
  try {
    static_cast<void>(directory_stat(descriptor, "fstat artifact directory"));
  } catch (...) {
    static_cast<void>(::close(descriptor));
    throw;
  }
  return descriptor;
}

/**
 * @brief Loads, verifies, and barriers one committed artifact directory.
 * @param artifacts_descriptor Held artifacts namespace.
 * @param tenant_id Exact configured tenant owner.
 * @param artifact_id Directory identity expected in the manifest.
 * @param root_descriptor Held durability root for final barrier.
 * @return Immutable verified artifact record.
 * @throws DurableCorruptionError/system/allocation failures unchanged.
 */
std::shared_ptr<const ArtifactRecord> load_artifact(
    int artifacts_descriptor, const TenantId& tenant_id,
    const ArtifactId& artifact_id, int root_descriptor) {
  ScopedDescriptor directory(
      open_artifact_directory(artifacts_descriptor, artifact_id));
  const std::vector<std::string> names = list_directory(directory.get());
  bool has_payload = false;
  bool has_manifest = false;
  bool has_private_manifest = false;
  for (const std::string& name : names) {
    if (name == kPayloadName) {
      has_payload = true;
    } else if (name == kManifestName) {
      has_manifest = true;
    } else if (name == kPrivateManifestName) {
      has_private_manifest = true;
    } else {
      throw DurableCorruptionError(
          "artifact directory contains an ambiguous retained entry");
    }
  }
  if (!has_payload || !has_manifest) {
    throw DurableCorruptionError("committed artifact is incomplete");
  }
  const std::string manifest =
      bytes_to_string(read_file_at(directory.get(), kManifestName));
  const OutputCommitReceipt receipt = parse_artifact_manifest(manifest);
  if (receipt.attempt.tenant_id != tenant_id ||
      receipt.artifact_id != artifact_id) {
    throw DurableCorruptionError(
        "artifact manifest identity does not match root");
  }
  std::vector<std::byte> payload = read_file_at(directory.get(), kPayloadName);
  if (payload.size() != receipt.descriptor.payload_bytes ||
      hash_artifact_content(payload.data(), payload.size()) !=
          receipt.content_digest) {
    throw DurableCorruptionError(
        "artifact payload length or digest mismatches");
  }
  if (has_private_manifest) {
    const std::vector<std::byte> private_manifest =
        read_file_at(directory.get(), kPrivateManifestName);
    if (bytes_to_string(private_manifest) != manifest) {
      throw DurableCorruptionError(
          "published artifact retains a conflicting private manifest");
    }
    if (::unlinkat(directory.get(), kPrivateManifestName, 0) != 0) {
      throw_errno("remove reconciled private artifact manifest");
    }
  }
  synchronize_descriptor(directory.get(), "artifact directory");
  synchronize_descriptor(artifacts_descriptor, "artifacts directory");
  synchronize_descriptor(root_descriptor, "durability root directory");
  auto record = std::make_shared<ArtifactRecord>();
  record->receipt = receipt;
  record->payload = std::move(payload);
  return record;
}

/**
 * @brief Removes one unambiguous pre-manifest artifact directory.
 * @param artifacts_descriptor Held artifacts namespace.
 * @param artifact_id Exact private directory identity.
 * @param root_descriptor Held durability root.
 * @return Nothing after cleanup and parent barriers.
 * @throws DurableCorruptionError for unknown/manifest-present entries.
 * @throws std::system_error/durability failures unchanged.
 */
void remove_incomplete_artifact(int artifacts_descriptor,
                                const ArtifactId& artifact_id,
                                int root_descriptor) {
  ScopedDescriptor directory(
      open_artifact_directory(artifacts_descriptor, artifact_id));
  const std::vector<std::string> names = list_directory(directory.get());
  for (const std::string& name : names) {
    if (name == kManifestName) {
      throw DurableCorruptionError(
          "refusing to remove an authoritative artifact manifest");
    }
    if (name != kPayloadName && name != kPrivateManifestName) {
      throw DurableCorruptionError(
          "incomplete artifact contains ambiguous retained state");
    }
    struct stat value{};
    if (::fstatat(directory.get(), name.c_str(), &value, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        !S_ISREG(value.st_mode)) {
      throw DurableCorruptionError(
          "incomplete artifact leaf cannot be removed safely");
    }
    if (::unlinkat(directory.get(), name.c_str(), 0) != 0) {
      throw_errno("remove incomplete artifact leaf");
    }
  }
  synchronize_descriptor(directory.get(), "incomplete artifact directory");
  const int raw_directory = directory.release();
  if (::close(raw_directory) != 0) {
    throw_errno("close incomplete artifact directory");
  }
  if (::unlinkat(artifacts_descriptor, artifact_id.value().c_str(),
                 AT_REMOVEDIR) != 0) {
    throw_errno("remove incomplete artifact directory");
  }
  synchronize_descriptor(artifacts_descriptor, "artifacts directory");
  synchronize_descriptor(root_descriptor, "durability root directory");
}

/**
 * @brief Checks whether one no-follow child exists.
 * @param parent Held parent directory.
 * @param name Child leaf name.
 * @return True when a non-symlink entry exists; false only for ENOENT.
 * @throws std::system_error for other observation failures.
 */
bool child_exists(int parent, const std::string& name) {
  struct stat value{};
  if (::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0) {
    return true;
  }
  if (errno == ENOENT) {
    return false;
  }
  throw_errno("observe durable child entry");
}

/**
 * @brief Validates stable request/record identity and exact candidate content.
 * @param request Current retry request.
 * @param candidate Newly computed candidate record.
 * @param existing Original committed occurrence.
 * @return Nothing for one idempotent retry.
 * @throws DurableConflictError for any stable-identity/content mismatch.
 */
void validate_idempotent_retry(const DurableArtifactCommitRequest& request,
                               const ArtifactRecord& candidate,
                               const ArtifactRecord& existing) {
  const OutputCommitReceipt& current = candidate.receipt;
  const OutputCommitReceipt& retained = existing.receipt;
  if (retained.attempt.tenant_id != request.attempt.tenant_id ||
      retained.attempt.job_id != request.attempt.job_id ||
      retained.attempt.job_spec_digest != request.attempt.job_spec_digest ||
      retained.output_slot_id != request.output_slot_id ||
      retained.artifact_id != request.artifact_id ||
      retained.output_commit_id != request.output_commit_id ||
      !(retained.descriptor == current.descriptor) ||
      retained.content_digest != current.content_digest ||
      existing.payload != candidate.payload ||
      retained.achieved_durability != ArtifactDurability::CrashDurable) {
    throw DurableConflictError(
        "stable output commit identity conflicts with retained artifact");
  }
}

/**
 * @brief Appends all canonical JobSpec fields to one durable record.
 * @param spec Valid immutable JobSpec.
 * @param output Non-null record output.
 * @return Nothing after complete framing.
 * @throws Validation/allocation failures unchanged.
 */
void append_job_spec(const JobSpec& spec, std::string* output) {
  validate_job_spec(spec);
  append_frame(spec.canonical_bytes(), output);
  append_frame(spec.graph_artifact_id().value(), output);
  append_frame(std::to_string(spec.target_node()), output);
  append_frame(spec.output_slot_id().value(), output);
  append_frame("embedded-cpu-v1", output);
  append_frame("crash-durable", output);
  const JobResourceRequest& request = spec.resource_request();
  append_frame(std::to_string(request.cpu_slots), output);
  append_frame(std::to_string(request.host_memory_bytes), output);
  append_frame(std::to_string(request.output_bytes), output);
  append_frame(std::to_string(request.staging_bytes), output);
  append_frame(std::to_string(request.retention_bytes), output);
  append_frame(std::to_string(request.devices.size()), output);
  for (const DeviceResourceRequest& device : request.devices) {
    append_frame(device.device_id, output);
    append_frame(std::to_string(device.bytes), output);
  }
  append_frame(spec.checkpoint_artifact_id().has_value() ? "1" : "0", output);
  append_frame(spec.checkpoint_artifact_id().has_value()
                   ? spec.checkpoint_artifact_id()->value()
                   : std::string_view{},
               output);
}

/**
 * @brief Reads and reconstructs one exact canonical JobSpec.
 * @param reader Non-null record frame reader positioned at spec fields.
 * @return Shared immutable validated JobSpec.
 * @throws DurableCorruptionError/validation/allocation failures unchanged.
 */
std::shared_ptr<const JobSpec> read_job_spec(FrameReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("durable JobSpec reader is null");
  }
  const std::string canonical(reader->next());
  GraphArtifactId graph(std::string(reader->next()));
  const int target = parse_nonnegative_int(reader->next());
  OutputSlotId slot(std::string(reader->next()));
  if (reader->next() != "embedded-cpu-v1" ||
      reader->next() != "crash-durable") {
    throw DurableCorruptionError("durable JobSpec profile is invalid");
  }
  JobResourceRequest request;
  request.cpu_slots = parse_unsigned<std::uint32_t>(reader->next());
  request.host_memory_bytes = parse_unsigned<std::uint64_t>(reader->next());
  request.output_bytes = parse_unsigned<std::uint64_t>(reader->next());
  request.staging_bytes = parse_unsigned<std::uint64_t>(reader->next());
  request.retention_bytes = parse_unsigned<std::uint64_t>(reader->next());
  const std::size_t devices = parse_unsigned<std::size_t>(reader->next());
  if (devices > kMaximumOpaqueIdentityBytes) {
    throw DurableCorruptionError("durable JobSpec device count is excessive");
  }
  request.devices.reserve(devices);
  for (std::size_t index = 0U; index < devices; ++index) {
    DeviceResourceRequest device;
    device.device_id = std::string(reader->next());
    device.bytes = parse_unsigned<std::uint64_t>(reader->next());
    request.devices.push_back(std::move(device));
  }
  const bool has_checkpoint = parse_bool(reader->next());
  const std::string checkpoint(reader->next());
  if (has_checkpoint != !checkpoint.empty()) {
    throw DurableCorruptionError(
        "durable JobSpec checkpoint framing conflicts");
  }
  std::optional<ArtifactId> checkpoint_id;
  if (has_checkpoint) {
    checkpoint_id = ArtifactId(checkpoint);
  }
  auto spec = std::make_shared<const JobSpec>(
      std::move(graph), target, std::move(slot), std::move(request),
      std::move(checkpoint_id), JobExecutionProfile::EmbeddedCpuV1,
      ArtifactDurability::CrashDurable);
  if (spec->canonical_bytes() != canonical) {
    throw DurableCorruptionError("durable JobSpec canonical bytes drifted");
  }
  return spec;
}

/**
 * @brief Reports whether one failure is a valid worker-owned failed fact.
 * @param failure Candidate closed failure representation.
 * @return True only for a failure accepted from `JobAttemptWorker`.
 * @throws Nothing.
 */
bool is_persistable_worker_failure(JobAttemptFailure failure) noexcept {
  switch (failure) {
    case JobAttemptFailure::InvalidAssignment:
    case JobAttemptFailure::GraphResolution:
    case JobAttemptFailure::HostSetup:
    case JobAttemptFailure::GraphLoad:
    case JobAttemptFailure::Compute:
    case JobAttemptFailure::Settlement:
    case JobAttemptFailure::Unexpected:
      return true;
    case JobAttemptFailure::None:
    case JobAttemptFailure::CancellationObserved:
    case JobAttemptFailure::ReportRejected:
    case JobAttemptFailure::ArtifactCommit:
    case JobAttemptFailure::RecoveryInterrupted:
      return false;
  }
  return false;
}

/**
 * @brief Validates one embedded durable receipt descriptor and quota bounds.
 * @param receipt Candidate identity-joined durable receipt.
 * @param spec Exact accepted JobSpec that owns its output transaction.
 * @return Nothing after descriptor shape and payload bounds are consistent.
 * @throws std::invalid_argument for invalid attempt/descriptor/quota facts.
 */
void validate_durable_receipt(const OutputCommitReceipt& receipt,
                              const JobSpec& spec) {
  validate_attempt_identity(receipt.attempt);
  const ArtifactImageDescriptor& descriptor = receipt.descriptor;
  if (descriptor.width <= 0 || descriptor.height <= 0 ||
      descriptor.channels <= 0 || descriptor.row_bytes == 0U ||
      descriptor.payload_bytes == 0U) {
    throw std::invalid_argument("durable Job receipt descriptor is empty");
  }
  const std::uint64_t width = static_cast<std::uint64_t>(descriptor.width);
  const std::uint64_t channels =
      static_cast<std::uint64_t>(descriptor.channels);
  const std::uint64_t channel_bytes =
      image_buffer_bytes_per_channel(descriptor.type);
  if (width > std::numeric_limits<std::uint64_t>::max() / channels ||
      width * channels >
          std::numeric_limits<std::uint64_t>::max() / channel_bytes) {
    throw std::invalid_argument("durable Job receipt row size overflowed");
  }
  const std::uint64_t expected_row = width * channels * channel_bytes;
  if (expected_row != descriptor.row_bytes ||
      descriptor.row_bytes > std::numeric_limits<std::size_t>::max() /
                                 static_cast<std::size_t>(descriptor.height) ||
      descriptor.row_bytes * static_cast<std::size_t>(descriptor.height) !=
          descriptor.payload_bytes) {
    throw std::invalid_argument(
        "durable Job receipt descriptor is inconsistent");
  }
  const JobResourceRequest& request = spec.resource_request();
  if (descriptor.payload_bytes > request.output_bytes ||
      descriptor.payload_bytes > request.staging_bytes ||
      descriptor.payload_bytes > request.retention_bytes) {
    throw std::invalid_argument(
        "durable Job receipt exceeds accepted resource bounds");
  }
}

/**
 * @brief Validates persisted state/outcome/settlement/failure relationships.
 * @param record Candidate identity-complete durable Job record.
 * @return Nothing after the closed lifecycle representation is coherent.
 * @throws std::invalid_argument for an impossible persisted combination.
 */
void validate_durable_job_semantics(const DurableJobRecord& record) {
  switch (record.state) {
    case JobState::Queued:
    case JobState::Running:
      if (record.cancellation_requested || record.attempt_settled ||
          record.attempt_outcome != JobAttemptOutcome::None ||
          record.failure != JobAttemptFailure::None ||
          record.output_receipt.has_value()) {
        throw std::invalid_argument(
            "durable active Job record has terminal facts");
      }
      return;
    case JobState::Cancelling:
      if (!record.cancellation_requested || record.attempt_settled ||
          record.attempt_outcome != JobAttemptOutcome::None ||
          record.failure != JobAttemptFailure::None ||
          record.output_receipt.has_value()) {
        throw std::invalid_argument(
            "durable cancelling Job record is inconsistent");
      }
      return;
    case JobState::Succeeded:
      if (record.cancellation_requested || !record.attempt_settled ||
          record.attempt_outcome != JobAttemptOutcome::Succeeded ||
          record.failure != JobAttemptFailure::None ||
          !record.output_receipt.has_value()) {
        throw std::invalid_argument(
            "durable successful Job record is inconsistent");
      }
      return;
    case JobState::Cancelled:
      if (!record.cancellation_requested || !record.attempt_settled ||
          (record.attempt_outcome != JobAttemptOutcome::Succeeded &&
           record.attempt_outcome != JobAttemptOutcome::Cancelled) ||
          record.failure != JobAttemptFailure::CancellationObserved ||
          record.output_receipt.has_value()) {
        throw std::invalid_argument(
            "durable cancelled Job record is inconsistent");
      }
      return;
    case JobState::Failed:
      break;
  }
  if (record.output_receipt.has_value() ||
      record.failure == JobAttemptFailure::None ||
      record.failure == JobAttemptFailure::CancellationObserved) {
    throw std::invalid_argument("durable failed Job record is inconsistent");
  }
  if (is_persistable_worker_failure(record.failure)) {
    if (record.attempt_outcome != JobAttemptOutcome::Failed) {
      throw std::invalid_argument(
          "durable worker failure has no failed attempt outcome");
    }
    return;
  }
  if (record.failure == JobAttemptFailure::ReportRejected) {
    if (record.attempt_settled ||
        record.attempt_outcome != JobAttemptOutcome::None) {
      throw std::invalid_argument(
          "durable report rejection trusts attempt facts");
    }
    return;
  }
  if (record.failure == JobAttemptFailure::RecoveryInterrupted) {
    if (!record.attempt_settled ||
        record.attempt_outcome != JobAttemptOutcome::Failed) {
      throw std::invalid_argument("durable recovery failure is not settled");
    }
    return;
  }
  if (record.failure == JobAttemptFailure::ArtifactCommit) {
    const bool terminal_outcome =
        record.attempt_outcome == JobAttemptOutcome::Succeeded ||
        record.attempt_outcome == JobAttemptOutcome::Cancelled;
    if ((terminal_outcome && !record.attempt_settled) ||
        (record.attempt_outcome == JobAttemptOutcome::None &&
         record.attempt_settled)) {
      throw std::invalid_argument(
          "durable publication failure contradicts settlement");
    }
    return;
  }
  throw std::invalid_argument("durable failed Job category is invalid");
}

/**
 * @brief Validates a complete durable Job record before persistence/recovery.
 * @param record Candidate record.
 * @param configured_tenant Exact root tenant.
 * @return Nothing after all stable joins are valid.
 * @throws std::invalid_argument for inconsistent authority facts.
 */
void validate_durable_job_record(const DurableJobRecord& record,
                                 const TenantId& configured_tenant) {
  if (record.tenant_id != configured_tenant || !record.job_id.valid() ||
      record.spec == nullptr || !record.output_artifact_id.valid() ||
      !record.output_commit_id.valid()) {
    throw std::invalid_argument("durable Job record identity is incomplete");
  }
  validate_job_spec(*record.spec);
  validate_attempt_identity(record.assignment);
  if (record.assignment.tenant_id != record.tenant_id ||
      record.assignment.job_id != record.job_id ||
      record.assignment.job_spec_digest != record.spec->digest()) {
    throw std::invalid_argument("durable Job assignment join is invalid");
  }
  try {
    static_cast<void>(parse_job_state(static_cast<std::uint8_t>(record.state)));
    static_cast<void>(parse_attempt_outcome(
        static_cast<std::uint8_t>(record.attempt_outcome)));
    static_cast<void>(
        parse_attempt_failure(static_cast<std::uint8_t>(record.failure)));
  } catch (const DurableCorruptionError& error) {
    throw std::invalid_argument(error.what());
  }
  if (record.output_receipt.has_value()) {
    const OutputCommitReceipt& receipt = *record.output_receipt;
    if (receipt.attempt.tenant_id != record.tenant_id ||
        receipt.attempt.job_id != record.job_id ||
        receipt.attempt.job_spec_digest != record.spec->digest() ||
        receipt.output_slot_id != record.spec->output_slot_id() ||
        receipt.artifact_id != record.output_artifact_id ||
        receipt.output_commit_id != record.output_commit_id ||
        receipt.achieved_durability != ArtifactDurability::CrashDurable) {
      throw std::invalid_argument("durable Job receipt join is invalid");
    }
    validate_durable_receipt(receipt, *record.spec);
  }
  validate_durable_job_semantics(record);
}

/**
 * @brief Serializes one complete durable Job record.
 * @param record Valid record.
 * @return Exact canonical record bytes.
 * @throws Validation/allocation failures unchanged.
 */
std::string serialize_job_record(const DurableJobRecord& record) {
  std::string output{kJobRecordVersion};
  append_frame(record.tenant_id.value(), &output);
  append_frame(record.job_id.value(), &output);
  append_job_spec(*record.spec, &output);
  append_frame(record.assignment.attempt_id.value(), &output);
  append_frame(record.assignment.worker_instance_id.value(), &output);
  append_frame(std::to_string(record.assignment.worker_lease_generation.value),
               &output);
  append_frame(record.output_artifact_id.value(), &output);
  append_frame(record.output_commit_id.value(), &output);
  append_frame(std::to_string(static_cast<std::uint8_t>(record.state)),
               &output);
  append_frame(record.cancellation_requested ? "1" : "0", &output);
  append_frame(record.attempt_settled ? "1" : "0", &output);
  append_frame(
      std::to_string(static_cast<std::uint8_t>(record.attempt_outcome)),
      &output);
  append_frame(std::to_string(static_cast<std::uint8_t>(record.failure)),
               &output);
  append_frame(record.message, &output);
  append_frame(record.output_receipt.has_value()
                   ? serialize_artifact_manifest(*record.output_receipt)
                   : std::string{},
               &output);
  return output;
}

/**
 * @brief Parses one durable Job record and joins any persisted receipt.
 * @param bytes Exact record bytes.
 * @param configured_tenant Exact root tenant.
 * @return Complete validated durable Job record.
 * @throws DurableCorruptionError for malformed fields or semantic drift.
 * @throws std::bad_alloc when retained value construction exhausts memory.
 */
DurableJobRecord parse_job_record(std::string_view bytes,
                                  const TenantId& configured_tenant) {
  try {
    FrameReader reader(bytes, kJobRecordVersion);
    DurableJobRecord record;
    record.tenant_id = TenantId(std::string(reader.next()));
    record.job_id = JobId(std::string(reader.next()));
    record.spec = read_job_spec(&reader);
    record.assignment.tenant_id = record.tenant_id;
    record.assignment.job_id = record.job_id;
    record.assignment.job_spec_digest = record.spec->digest();
    record.assignment.attempt_id = JobAttemptId(std::string(reader.next()));
    record.assignment.worker_instance_id =
        WorkerInstanceId(std::string(reader.next()));
    record.assignment.worker_lease_generation.value =
        parse_unsigned<std::uint64_t>(reader.next());
    record.output_artifact_id = ArtifactId(std::string(reader.next()));
    record.output_commit_id = OutputCommitId(std::string(reader.next()));
    record.state = parse_job_state(parse_unsigned<std::uint8_t>(reader.next()));
    record.cancellation_requested = parse_bool(reader.next());
    record.attempt_settled = parse_bool(reader.next());
    record.attempt_outcome =
        parse_attempt_outcome(parse_unsigned<std::uint8_t>(reader.next()));
    record.failure =
        parse_attempt_failure(parse_unsigned<std::uint8_t>(reader.next()));
    record.message = std::string(reader.next());
    const std::string receipt_manifest(reader.next());
    reader.finish();
    if (!receipt_manifest.empty()) {
      record.output_receipt = parse_artifact_manifest(receipt_manifest);
    }
    validate_durable_job_record(record, configured_tenant);
    return record;
  } catch (const DurableCorruptionError&) {
    throw;
  } catch (const std::invalid_argument& error) {
    throw DurableCorruptionError(std::string("invalid durable Job record: ") +
                                 error.what());
  } catch (const std::overflow_error& error) {
    throw DurableCorruptionError(std::string("invalid durable Job record: ") +
                                 error.what());
  }
}

/**
 * @brief Writes and synchronizes one private no-replace file.
 * @param parent Held parent directory.
 * @param name Unique private leaf name.
 * @param bytes Exact bytes.
 * @return Nothing after file barrier and close.
 * @throws std::system_error/durability failures unchanged.
 */
void write_private_file(int parent, const std::string& name,
                        std::string_view bytes) {
  const int raw = ::openat(parent, name.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                           S_IRUSR | S_IWUSR);
  if (raw < 0) {
    throw_errno("create private durable state file");
  }
  ScopedDescriptor descriptor(raw);
  write_all(descriptor.get(), as_bytes(bytes), bytes.size());
  synchronize_descriptor(descriptor.get(), "durable state file");
  const struct stat value =
      regular_file_stat(descriptor.get(), "fstat private durable state file");
  if (static_cast<std::uintmax_t>(value.st_size) != bytes.size()) {
    throw DurableCorruptionError("private durable state file length drifted");
  }
}

/**
 * @brief Returns the fixed Job record filename for one valid identity.
 * @param job_id Valid durable Job identity.
 * @return Opaque identity plus fixed suffix.
 * @throws std::bad_alloc when construction fails.
 */
std::string job_record_name(const JobId& job_id) {
  return job_id.value() + kJobRecordSuffix;
}

/**
 * @brief Removes one known regular leaf if present.
 * @param parent Held directory.
 * @param name Exact leaf name.
 * @return True when removed; false when absent.
 * @throws DurableCorruptionError for non-regular retained state.
 * @throws std::system_error for observation/removal failures.
 */
bool remove_regular_leaf_if_present(int parent, const std::string& name) {
  struct stat value{};
  if (::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return false;
    }
    throw_errno("observe durable regular leaf");
  }
  if (!S_ISREG(value.st_mode)) {
    throw DurableCorruptionError("durable leaf cleanup target is not regular");
  }
  if (::unlinkat(parent, name.c_str(), 0) != 0) {
    throw_errno("remove durable regular leaf");
  }
  return true;
}

}  // namespace

/** @copydoc ps::server::DurableServerState::DurableServerState */
DurableServerState::DurableServerState(std::filesystem::path root,
                                       TenantId tenant_id,
                                       DurableServerStateOptions options)
    : tenant_id_(std::move(tenant_id)), options_(std::move(options)) {
  if (!tenant_id_.valid() || root.empty()) {
    throw std::invalid_argument("durable state root or tenant is invalid");
  }
  std::filesystem::create_directories(root);
  root_ = std::filesystem::canonical(std::move(root));
  const int raw_root =
      ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (raw_root < 0) {
    throw_errno("open durable state root");
  }
  ScopedDescriptor root_owner(raw_root);
  const struct stat root_stat =
      directory_stat(root_owner.get(), "fstat durable state root");
  if (::flock(root_owner.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOTSUP ||
        errno == EOPNOTSUPP) {
      throw DurableCapabilityError(
          "exclusive durable state root ownership is unavailable");
    }
    throw_errno("lock durable state root");
  }
  ScopedDescriptor control(
      open_or_create_directory(root_owner.get(), kControlDirectory));
  ScopedDescriptor jobs(
      open_or_create_directory(control.get(), kJobsDirectory));
  ScopedDescriptor artifacts(
      open_or_create_directory(root_owner.get(), kArtifactsDirectory));
  synchronize_descriptor(jobs.get(), "jobs directory");
  synchronize_descriptor(control.get(), "control directory");
  synchronize_descriptor(artifacts.get(), "artifacts directory");
  synchronize_descriptor(root_owner.get(), "durability root directory");

  root_device_ = static_cast<std::uint64_t>(root_stat.st_dev);
  root_inode_ = static_cast<std::uint64_t>(root_stat.st_ino);
  root_descriptor_ = root_owner.release();
  control_descriptor_ = control.release();
  jobs_descriptor_ = jobs.release();
  artifacts_descriptor_ = artifacts.release();

  try {
    verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
    for (const std::string& name : list_directory(artifacts_descriptor_)) {
      ArtifactId artifact_id;
      try {
        artifact_id = ArtifactId(name);
      } catch (const std::invalid_argument&) {
        throw DurableCorruptionError(
            "artifact namespace contains an invalid directory identity");
      }
      ScopedDescriptor directory(
          open_artifact_directory(artifacts_descriptor_, artifact_id));
      const bool committed = child_exists(directory.get(), kManifestName);
      const int raw_directory = directory.release();
      if (::close(raw_directory) != 0) {
        throw_errno("close observed artifact directory");
      }
      if (!committed) {
        remove_incomplete_artifact(artifacts_descriptor_, artifact_id,
                                   root_descriptor_);
        continue;
      }
      auto record = load_artifact(artifacts_descriptor_, tenant_id_,
                                  artifact_id, root_descriptor_);
      if (!artifacts_.emplace(artifact_id.value(), record).second ||
          !commits_.emplace(record->receipt.output_commit_id.value(), record)
               .second) {
        throw DurableCorruptionError(
            "durable artifact or commit identity is duplicated");
      }
    }

    for (const std::string& name : list_directory(jobs_descriptor_)) {
      const bool private_record = name.rfind(".job-", 0U) == 0U &&
                                  name.size() >= 4U &&
                                  name.substr(name.size() - 4U) == ".tmp";
      if (private_record) {
        static_cast<void>(
            remove_regular_leaf_if_present(jobs_descriptor_, name));
        continue;
      }
      if (name.size() <= std::strlen(kJobRecordSuffix) ||
          name.substr(name.size() - std::strlen(kJobRecordSuffix)) !=
              kJobRecordSuffix) {
        throw DurableCorruptionError(
            "durable Job namespace contains an ambiguous entry");
      }
      const std::string job_text =
          name.substr(0U, name.size() - std::strlen(kJobRecordSuffix));
      JobId job_id;
      try {
        job_id = JobId(job_text);
      } catch (const std::invalid_argument&) {
        throw DurableCorruptionError(
            "durable Job filename identity is invalid");
      }
      DurableJobRecord record = parse_job_record(
          bytes_to_string(read_file_at(jobs_descriptor_, name)), tenant_id_);
      if (record.job_id != job_id ||
          !jobs_.emplace(job_id.value(), std::move(record)).second) {
        throw DurableCorruptionError(
            "durable Job filename/content identity conflicts");
      }
    }
    synchronize_descriptor(jobs_descriptor_, "jobs directory");
    synchronize_descriptor(control_descriptor_, "control directory");
    synchronize_descriptor(root_descriptor_, "durability root directory");
  } catch (...) {
    const std::array<int*, 4U> descriptors{
        &artifacts_descriptor_, &jobs_descriptor_, &control_descriptor_,
        &root_descriptor_};
    for (int* descriptor : descriptors) {
      if (*descriptor >= 0) {
        static_cast<void>(::close(*descriptor));
        *descriptor = -1;
      }
    }
    throw;
  }
}

/** @copydoc ps::server::DurableServerState::~DurableServerState */
DurableServerState::~DurableServerState() noexcept {
  const std::array<int*, 4U> descriptors{
      &artifacts_descriptor_, &jobs_descriptor_, &control_descriptor_,
      &root_descriptor_};
  for (int* descriptor : descriptors) {
    if (*descriptor >= 0 && ::close(*descriptor) != 0) {
      std::terminate();
    }
    *descriptor = -1;
  }
}

/** @copydoc ps::server::DurableServerState::recovered_artifacts */
std::vector<std::shared_ptr<const ArtifactRecord>>
DurableServerState::recovered_artifacts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<const ArtifactRecord>> result;
  result.reserve(artifacts_.size());
  for (const auto& entry : artifacts_) {
    result.push_back(entry.second);
  }
  return result;
}

/** @copydoc ps::server::DurableServerState::recovered_jobs */
std::vector<DurableJobRecord> DurableServerState::recovered_jobs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<DurableJobRecord> result;
  result.reserve(jobs_.size());
  for (const auto& entry : jobs_) {
    result.push_back(entry.second);
  }
  return result;
}

/** @copydoc ps::server::DurableServerState::commit_artifact */
OutputCommitReceipt DurableServerState::commit_artifact(
    const DurableArtifactCommitRequest& request, const ImageBuffer& image) {
  validate_attempt_identity(request.attempt);
  if (!request.output_slot_id.valid() || !request.artifact_id.valid() ||
      !request.output_commit_id.valid()) {
    throw std::invalid_argument("durable artifact commit identity is invalid");
  }
  validate_job_resource_request(request.reserved_resources);
  validate_image_buffer(image);
  if (image.width <= 0 || image.height <= 0 || image.channels <= 0 ||
      image.device != Device::CPU || image.data == nullptr) {
    throw std::invalid_argument(
        "durable artifact commit requires a nonempty CPU image");
  }
  const std::size_t row_bytes = image_buffer_row_bytes(image);
  if (row_bytes > std::numeric_limits<std::size_t>::max() /
                      static_cast<std::size_t>(image.height)) {
    throw std::overflow_error("durable artifact payload size overflowed");
  }
  const std::size_t payload_bytes =
      row_bytes * static_cast<std::size_t>(image.height);
  if (payload_bytes == 0U ||
      payload_bytes > request.reserved_resources.output_bytes ||
      payload_bytes > request.reserved_resources.staging_bytes ||
      payload_bytes > request.reserved_resources.retention_bytes) {
    throw std::invalid_argument(
        "durable artifact payload exceeds reserved output/staging/retention");
  }
  ArtifactRecord candidate;
  candidate.receipt.attempt = request.attempt;
  candidate.receipt.output_slot_id = request.output_slot_id;
  candidate.receipt.artifact_id = request.artifact_id;
  candidate.receipt.output_commit_id = request.output_commit_id;
  candidate.receipt.descriptor =
      ArtifactImageDescriptor{image.width, image.height, image.channels,
                              image.type,  row_bytes,    payload_bytes};
  candidate.receipt.achieved_durability = ArtifactDurability::CrashDurable;
  candidate.payload.resize(payload_bytes);
  for (int row = 0; row < image.height; ++row) {
    std::memcpy(
        candidate.payload.data() + static_cast<std::size_t>(row) * row_bytes,
        image_buffer_row_data(image, row), row_bytes);
  }
  candidate.receipt.content_digest =
      hash_artifact_content(candidate.payload.data(), candidate.payload.size());
  const std::string manifest = serialize_artifact_manifest(candidate.receipt);

  std::lock_guard<std::mutex> lock(mutex_);
  verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
  const auto committed = commits_.find(request.output_commit_id.value());
  if (committed != commits_.end()) {
    validate_idempotent_retry(request, candidate, *committed->second);
    return committed->second->receipt;
  }

  if (child_exists(artifacts_descriptor_, request.artifact_id.value())) {
    ScopedDescriptor existing(
        open_artifact_directory(artifacts_descriptor_, request.artifact_id));
    const bool has_manifest = child_exists(existing.get(), kManifestName);
    const int raw_existing = existing.release();
    if (::close(raw_existing) != 0) {
      throw_errno("close existing artifact directory");
    }
    if (!has_manifest) {
      remove_incomplete_artifact(artifacts_descriptor_, request.artifact_id,
                                 root_descriptor_);
    } else {
      auto record = load_artifact(artifacts_descriptor_, tenant_id_,
                                  request.artifact_id, root_descriptor_);
      validate_idempotent_retry(request, candidate, *record);
      if (commits_.find(record->receipt.output_commit_id.value()) !=
              commits_.end() ||
          artifacts_.find(record->receipt.artifact_id.value()) !=
              artifacts_.end()) {
        throw DurableConflictError(
            "reconciled artifact collides with loaded durable identity");
      }
      artifacts_.emplace(record->receipt.artifact_id.value(), record);
      commits_.emplace(record->receipt.output_commit_id.value(), record);
      return record->receipt;
    }
  }

  if (::mkdirat(artifacts_descriptor_, request.artifact_id.value().c_str(),
                S_IRWXU) != 0) {
    if (errno == EEXIST) {
      throw DurableConflictError("artifact identity appeared during commit");
    }
    throw_errno("create private artifact directory");
  }
  bool manifest_published = false;
  try {
    ScopedDescriptor directory(
        open_artifact_directory(artifacts_descriptor_, request.artifact_id));
    const int payload_raw =
        ::openat(directory.get(), kPayloadName,
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                 S_IRUSR | S_IWUSR);
    if (payload_raw < 0) {
      throw_errno("create durable artifact payload");
    }
    {
      ScopedDescriptor payload(payload_raw);
      write_all(payload.get(), candidate.payload.data(),
                candidate.payload.size());
      synchronize_descriptor(payload.get(), "artifact payload file");
      const struct stat value =
          regular_file_stat(payload.get(), "fstat durable artifact payload");
      if (static_cast<std::uintmax_t>(value.st_size) !=
          candidate.payload.size()) {
        throw DurableCorruptionError("durable artifact payload length drifted");
      }
    }
    const std::vector<std::byte> reopened =
        read_file_at(directory.get(), kPayloadName);
    if (reopened != candidate.payload ||
        hash_artifact_content(reopened.data(), reopened.size()) !=
            candidate.receipt.content_digest) {
      throw DurableCorruptionError(
          "durable artifact payload revalidation failed");
    }
    if (options_.artifact_commit_observer) {
      options_.artifact_commit_observer(
          DurableArtifactCommitStage::PayloadSynchronized);
    }
    write_private_file(directory.get(), kPrivateManifestName, manifest);
    if (::linkat(directory.get(), kPrivateManifestName, directory.get(),
                 kManifestName, 0) != 0) {
      if (errno == ENOTSUP || errno == EOPNOTSUPP || errno == EPERM) {
        throw DurableCapabilityError(
            "atomic manifest link-no-replace is unsupported");
      }
      throw_errno("publish durable artifact manifest");
    }
    manifest_published = true;
    if (::unlinkat(directory.get(), kPrivateManifestName, 0) != 0) {
      throw_errno("remove private durable artifact manifest");
    }
    if (bytes_to_string(read_file_at(directory.get(), kManifestName)) !=
        manifest) {
      throw DurableCorruptionError(
          "published durable artifact manifest drifted");
    }
    if (options_.artifact_commit_observer) {
      options_.artifact_commit_observer(
          DurableArtifactCommitStage::ManifestPublished);
    }
    synchronize_descriptor(directory.get(), "artifact directory");
    synchronize_descriptor(artifacts_descriptor_, "artifacts directory");
    synchronize_descriptor(root_descriptor_, "durability root directory");
    if (options_.artifact_commit_observer) {
      options_.artifact_commit_observer(
          DurableArtifactCommitStage::DirectoryBarriersCompleted);
    }
  } catch (...) {
    if (!manifest_published) {
      remove_incomplete_artifact(artifacts_descriptor_, request.artifact_id,
                                 root_descriptor_);
    }
    throw;
  }
  auto record = std::make_shared<ArtifactRecord>(std::move(candidate));
  artifacts_.emplace(request.artifact_id.value(), record);
  commits_.emplace(request.output_commit_id.value(), record);
  return record->receipt;
}

/** @copydoc ps::server::DurableServerState::find_artifact */
std::shared_ptr<const ArtifactRecord> DurableServerState::find_artifact(
    const ArtifactId& artifact_id) const {
  if (!artifact_id.valid()) {
    throw std::invalid_argument("durable artifact lookup identity is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = artifacts_.find(artifact_id.value());
  if (found != artifacts_.end()) {
    return found->second;
  }
  if (!child_exists(artifacts_descriptor_, artifact_id.value())) {
    return nullptr;
  }
  auto record = load_artifact(artifacts_descriptor_, tenant_id_, artifact_id,
                              root_descriptor_);
  const auto commit = commits_.find(record->receipt.output_commit_id.value());
  if (commit != commits_.end() &&
      commit->second->receipt.artifact_id != artifact_id) {
    throw DurableCorruptionError("durable commit identity is duplicated");
  }
  artifacts_.emplace(artifact_id.value(), record);
  commits_.emplace(record->receipt.output_commit_id.value(), record);
  return record;
}

/** @copydoc ps::server::DurableServerState::find_commit */
std::shared_ptr<const ArtifactRecord> DurableServerState::find_commit(
    const OutputCommitId& output_commit_id) const {
  if (!output_commit_id.valid()) {
    throw std::invalid_argument("durable commit lookup identity is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = commits_.find(output_commit_id.value());
  return found == commits_.end() ? nullptr : found->second;
}

/** @copydoc ps::server::DurableServerState::erase_artifact */
std::uint64_t DurableServerState::erase_artifact(
    const ArtifactId& artifact_id) {
  if (!artifact_id.valid()) {
    throw std::invalid_argument("durable artifact erase identity is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
  if (!child_exists(artifacts_descriptor_, artifact_id.value())) {
    artifacts_.erase(artifact_id.value());
    return 0U;
  }
  auto record = load_artifact(artifacts_descriptor_, tenant_id_, artifact_id,
                              root_descriptor_);
  ScopedDescriptor directory(
      open_artifact_directory(artifacts_descriptor_, artifact_id));
  if (::unlinkat(directory.get(), kManifestName, 0) != 0) {
    throw_errno("remove authoritative artifact manifest");
  }
  synchronize_descriptor(directory.get(), "artifact directory");
  synchronize_descriptor(artifacts_descriptor_, "artifacts directory");
  synchronize_descriptor(root_descriptor_, "durability root directory");
  static_cast<void>(
      remove_regular_leaf_if_present(directory.get(), kPayloadName));
  static_cast<void>(
      remove_regular_leaf_if_present(directory.get(), kPrivateManifestName));
  synchronize_descriptor(directory.get(), "artifact directory");
  const int raw_directory = directory.release();
  if (::close(raw_directory) != 0) {
    throw_errno("close erased artifact directory");
  }
  if (::unlinkat(artifacts_descriptor_, artifact_id.value().c_str(),
                 AT_REMOVEDIR) != 0) {
    throw_errno("remove erased artifact directory");
  }
  synchronize_descriptor(artifacts_descriptor_, "artifacts directory");
  synchronize_descriptor(root_descriptor_, "durability root directory");
  artifacts_.erase(artifact_id.value());
  commits_.erase(record->receipt.output_commit_id.value());
  return static_cast<std::uint64_t>(record->receipt.descriptor.payload_bytes);
}

/** @copydoc ps::server::DurableServerState::persist_job */
void DurableServerState::persist_job(const DurableJobRecord& record) {
  validate_durable_job_record(record, tenant_id_);
  const std::string bytes = serialize_job_record(record);
  std::lock_guard<std::mutex> lock(mutex_);
  verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
  if (next_private_record_sequence_ == 0U) {
    throw std::overflow_error("private durable Job record sequence exhausted");
  }
  const std::string private_name =
      ".job-" + record.job_id.value() + "-" +
      std::to_string(next_private_record_sequence_) + ".tmp";
  ++next_private_record_sequence_;
  bool private_exists = false;
  try {
    write_private_file(jobs_descriptor_, private_name, bytes);
    private_exists = true;
    const std::string final_name = job_record_name(record.job_id);
    if (::renameat(jobs_descriptor_, private_name.c_str(), jobs_descriptor_,
                   final_name.c_str()) != 0) {
      throw_errno("publish durable Job record");
    }
    private_exists = false;
    synchronize_descriptor(jobs_descriptor_, "jobs directory");
    synchronize_descriptor(control_descriptor_, "control directory");
    synchronize_descriptor(root_descriptor_, "durability root directory");
  } catch (...) {
    if (private_exists) {
      static_cast<void>(
          remove_regular_leaf_if_present(jobs_descriptor_, private_name));
    }
    throw;
  }
  jobs_[record.job_id.value()] = record;
}

/** @copydoc ps::server::DurableServerState::erase_job */
bool DurableServerState::erase_job(const JobId& job_id) {
  if (!job_id.valid()) {
    throw std::invalid_argument("durable Job erase identity is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
  const bool removed =
      remove_regular_leaf_if_present(jobs_descriptor_, job_record_name(job_id));
  if (removed) {
    synchronize_descriptor(jobs_descriptor_, "jobs directory");
    synchronize_descriptor(control_descriptor_, "control directory");
    synchronize_descriptor(root_descriptor_, "durability root directory");
  }
  jobs_.erase(job_id.value());
  return removed;
}

}  // namespace ps::server
