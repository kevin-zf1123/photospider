/**
 * @file b1_output_store.cpp
 * @brief Implements process-I/O-backed crash-durable B1 artifact commits.
 */
#include "benchmark/b1_output_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "core/value_image_adapter.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {
namespace {

/** @brief Exact committed payload leaf. */
constexpr char kPayloadName[] = "output.rgba32le";

/** @brief Exact manifest leaf published last. */
constexpr char kManifestName[] = "manifest.txt";

/** @brief Private manifest leaf removed after no-replace publication. */
constexpr char kPrivateManifestName[] = ".manifest.private";

/**
 * @brief Typed internal failure for an unavailable durability primitive.
 * @throws Nothing beyond standard runtime-error string allocation.
 */
class B1DurabilityError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one typed durability failure.
   * @param message Complete diagnostic.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  explicit B1DurabilityError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Typed internal failure for reopened byte or identity mismatch.
 * @throws Nothing beyond standard runtime-error string allocation.
 */
class B1RevalidationError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one typed revalidation failure.
   * @param message Complete diagnostic.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  explicit B1RevalidationError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Cross-task immutable/mutable state retained through I/O settlement.
 * @throws Nothing for default construction except owned string allocation.
 * @note Caller reads task-written fields only after completion wait returns.
 */
struct B1CommitTaskState final {
  /** @brief Exact occurrence slot. */
  std::filesystem::path slot;
  /** @brief Candidate descriptor retained through payload completion. */
  ImageBuffer image;
  /** @brief Canonical manifest constructed after payload settlement. */
  std::string manifest;
  /** @brief Verified payload digest written by the payload task. */
  B1Sha256Digest payload_digest;
  /** @brief Verified manifest digest written by the commit task. */
  B1Sha256Digest manifest_digest;
  /** @brief Stable published manifest filesystem identity. */
  std::string published_identity;
};

#if !defined(_WIN32)

/**
 * @brief Throws a system error retaining the current `errno`.
 * @param operation Exact failed operation.
 * @return Nothing.
 * @throws std::system_error always.
 */
[[noreturn]] void throw_errno(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

/**
 * @brief Writes a complete borrowed byte range to one descriptor.
 * @param descriptor Open writable descriptor.
 * @param data First byte, null only for zero size.
 * @param size Exact byte count.
 * @return Nothing after all bytes are written.
 * @throws std::system_error for an unrecoverable write failure.
 */
void write_all(int descriptor, const std::byte* data, std::size_t size) {
  while (size != 0U) {
    const ssize_t written =
        ::write(descriptor, data,
                std::min(size, static_cast<std::size_t>(
                                   std::numeric_limits<ssize_t>::max())));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("write B1 artifact");
    }
    if (written == 0) {
      throw B1RevalidationError("B1 artifact write made no progress.");
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
}

/**
 * @brief Closes one descriptor and preserves a prior exception when requested.
 * @param descriptor Open descriptor, or negative no-op sentinel.
 * @return Nothing after successful close.
 * @throws std::system_error when close fails.
 */
void close_checked(int descriptor) {
  if (descriptor >= 0 && ::close(descriptor) != 0) {
    throw_errno("close B1 artifact");
  }
}

/**
 * @brief Owns one POSIX descriptor through exception-safe checked close.
 * @throws Nothing for construction; destruction is fail-stop on close failure.
 */
class ScopedFileDescriptor final {
 public:
  /**
   * @brief Adopts one open descriptor.
   * @param descriptor Nonnegative descriptor.
   * @throws std::invalid_argument for a negative descriptor.
   */
  explicit ScopedFileDescriptor(int descriptor) : descriptor_(descriptor) {
    if (descriptor_ < 0) {
      throw std::invalid_argument(
          "B1 descriptor owner received a negative fd.");
    }
  }

  /**
   * @brief Closes an unreleased descriptor.
   * @throws Nothing; close failure terminates to avoid a false durability fact.
   */
  ~ScopedFileDescriptor() noexcept {
    if (descriptor_ >= 0 && ::close(descriptor_) != 0) {
      std::terminate();
    }
  }

  /** @brief Descriptor ownership cannot be copied. */
  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;

  /** @brief Descriptor ownership cannot be assigned. */
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  /**
   * @brief Returns the borrowed descriptor.
   * @return Nonnegative owned descriptor.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Performs checked close and relinquishes ownership.
   * @return Nothing.
   * @throws std::system_error when close fails.
   */
  void close() {
    const int descriptor = descriptor_;
    descriptor_ = -1;
    close_checked(descriptor);
  }

 private:
  /** @brief Owned descriptor or negative released sentinel. */
  int descriptor_ = -1;
};

/**
 * @brief Synchronizes one open regular file for crash durability.
 * @param descriptor Open writable file descriptor.
 * @return Nothing after the platform barrier succeeds.
 * @throws B1DurabilityError when the barrier is unsupported.
 * @throws std::system_error for other synchronization failures.
 */
void synchronize_file(int descriptor) {
  if (::fsync(descriptor) == 0) {
    return;
  }
  if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {
    throw B1DurabilityError("B1 file synchronization is unsupported.");
  }
  throw_errno("fsync B1 file");
}

/**
 * @brief Synchronizes one directory namespace update.
 * @param directory Existing directory.
 * @return Nothing after the directory barrier succeeds.
 * @throws B1DurabilityError when directory barriers are unsupported.
 * @throws std::system_error for open or other synchronization failures.
 */
void synchronize_directory(const std::filesystem::path& directory) {
#if defined(O_DIRECTORY)
  const int flags = O_RDONLY | O_DIRECTORY;
#else
  const int flags = O_RDONLY;
#endif
  const int descriptor = ::open(directory.c_str(), flags);
  if (descriptor < 0) {
    throw_errno("open B1 directory for synchronization");
  }
  ScopedFileDescriptor owner(descriptor);
  if (::fsync(owner.get()) != 0) {
    if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {
      throw B1DurabilityError("B1 directory synchronization is unsupported.");
    }
    throw_errno("fsync B1 directory");
  }
  owner.close();
}

/**
 * @brief Detects whether native scalar storage is little endian.
 * @return True when a uint16 one begins with byte one.
 * @throws Nothing.
 */
bool native_little_endian() noexcept {
  const std::uint16_t one = 1U;
  return *reinterpret_cast<const std::uint8_t*>(&one) == 1U;
}

/**
 * @brief Validates the exact frozen candidate descriptor.
 * @param image Candidate image.
 * @return Nothing for exact CPU FP32 RGBA B1 state.
 * @throws std::invalid_argument for descriptor drift.
 */
void validate_b1_candidate_image(const ImageBuffer& image) {
  validate_image_buffer(image);
  if (image.width != static_cast<int>(kB1ImageEdge) ||
      image.height != static_cast<int>(kB1ImageEdge) ||
      image.channels != static_cast<int>(kB1ChannelCount) ||
      image.type != DataType::FLOAT32 || image.device != Device::CPU ||
      image_buffer_row_bytes(image) != kB1PayloadRowBytes) {
    throw std::invalid_argument("B1 candidate image descriptor drifted.");
  }
}

/**
 * @brief Streams exact tight little-endian B1 payload bytes to one fresh file.
 * @param state Complete task state retaining candidate and slot.
 * @return Nothing after write, fsync, reopen, length, and digest validation.
 * @throws File, descriptor, digest, and durability failures unchanged.
 */
void write_and_validate_payload(B1CommitTaskState* state) {
  const std::filesystem::path path = state->slot / kPayloadName;
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw_errno("create B1 payload no-replace");
  }
  ScopedFileDescriptor owner(descriptor);
  B1Sha256 write_hash;
  std::vector<std::byte> converted;
  if (!native_little_endian()) {
    converted.resize(static_cast<std::size_t>(kB1PayloadRowBytes));
  }
  for (int row_index = 0; row_index < state->image.height; ++row_index) {
    const std::byte* row = image_buffer_row_data(state->image, row_index);
    if (native_little_endian()) {
      write_all(owner.get(), row, static_cast<std::size_t>(kB1PayloadRowBytes));
      write_hash.update(row, static_cast<std::size_t>(kB1PayloadRowBytes));
      continue;
    }
    for (std::size_t offset = 0U; offset < converted.size(); offset += 4U) {
      converted[offset] = row[offset + 3U];
      converted[offset + 1U] = row[offset + 2U];
      converted[offset + 2U] = row[offset + 1U];
      converted[offset + 3U] = row[offset];
    }
    write_all(owner.get(), converted.data(), converted.size());
    write_hash.update(converted.data(), converted.size());
  }
  synchronize_file(owner.get());
  owner.close();
  const B1Sha256Digest first_digest = write_hash.finish();

  const int read_descriptor = ::open(path.c_str(), O_RDONLY);
  if (read_descriptor < 0) {
    throw_errno("reopen B1 payload");
  }
  ScopedFileDescriptor read_owner(read_descriptor);
  B1Sha256 read_hash;
  std::array<std::byte, 65536U> buffer{};
  std::uint64_t length = 0U;
  for (;;) {
    const ssize_t count =
        ::read(read_owner.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("read B1 payload for revalidation");
    }
    if (count == 0) {
      break;
    }
    if (length > std::numeric_limits<std::uint64_t>::max() -
                     static_cast<std::uint64_t>(count)) {
      throw B1RevalidationError("B1 payload length overflowed.");
    }
    length += static_cast<std::uint64_t>(count);
    read_hash.update(buffer.data(), static_cast<std::size_t>(count));
  }
  read_owner.close();
  const B1Sha256Digest second_digest = read_hash.finish();
  if (length != kB1PayloadBytes || first_digest != second_digest) {
    throw B1RevalidationError(
        "B1 payload length or digest revalidation failed.");
  }
  state->payload_digest = second_digest;
}

/**
 * @brief Reads one exact file and validates its canonical bytes.
 * @param path Existing file.
 * @param expected Exact expected bytes.
 * @return SHA-256 of the validated bytes.
 * @throws File or revalidation failures unchanged.
 */
B1Sha256Digest validate_exact_file(const std::filesystem::path& path,
                                   std::string_view expected) {
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    throw_errno("open B1 manifest for revalidation");
  }
  ScopedFileDescriptor owner(descriptor);
  std::string observed(expected.size(), '\0');
  std::size_t offset = 0U;
  while (offset < observed.size()) {
    const ssize_t count =
        ::read(owner.get(), observed.data() + offset, observed.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("read B1 manifest for revalidation");
    }
    if (count == 0) {
      throw B1RevalidationError("B1 manifest was truncated.");
    }
    offset += static_cast<std::size_t>(count);
  }
  std::byte extra{};
  ssize_t trailing = 0;
  do {
    trailing = ::read(owner.get(), &extra, 1U);
  } while (trailing < 0 && errno == EINTR);
  if (trailing < 0) {
    throw_errno("read B1 manifest trailing byte");
  }
  owner.close();
  if (trailing != 0 || observed != expected) {
    throw B1RevalidationError("B1 manifest bytes drifted.");
  }
  return b1_sha256(observed);
}

/**
 * @brief Writes, publishes no-replace, synchronizes, and validates manifest.
 * @param state Complete task state with verified payload digest/manifest.
 * @param root Selected canonical root.
 * @return Nothing after leaf-to-root barriers and identity capture.
 * @throws File, durability, and revalidation failures unchanged.
 */
void commit_and_validate_manifest(B1CommitTaskState* state,
                                  const std::filesystem::path& root) {
  const std::filesystem::path private_path = state->slot / kPrivateManifestName;
  const std::filesystem::path published_path = state->slot / kManifestName;
  const int descriptor = ::open(private_path.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw_errno("create private B1 manifest no-replace");
  }
  ScopedFileDescriptor owner(descriptor);
  write_all(owner.get(),
            reinterpret_cast<const std::byte*>(state->manifest.data()),
            state->manifest.size());
  synchronize_file(owner.get());
  owner.close();

  if (::link(private_path.c_str(), published_path.c_str()) != 0) {
    if (errno == ENOTSUP || errno == EOPNOTSUPP || errno == EPERM) {
      throw B1DurabilityError("B1 atomic link-no-replace is unsupported.");
    }
    throw_errno("publish B1 manifest link-no-replace");
  }
  if (::unlink(private_path.c_str()) != 0) {
    throw_errno("unlink private B1 manifest after publication");
  }

  state->manifest_digest = validate_exact_file(published_path, state->manifest);
  struct stat identity{};
  if (::stat(published_path.c_str(), &identity) != 0) {
    throw_errno("stat published B1 manifest");
  }
  std::ostringstream identity_text;
  identity_text << "dev=" << static_cast<std::uint64_t>(identity.st_dev)
                << ";ino=" << static_cast<std::uint64_t>(identity.st_ino);
  state->published_identity = identity_text.str();

  synchronize_directory(state->slot);
  synchronize_directory(root);
  const B1Sha256Digest after_barrier =
      validate_exact_file(published_path, state->manifest);
  if (after_barrier != state->manifest_digest) {
    throw B1RevalidationError(
        "B1 manifest digest changed after namespace barriers.");
  }
}

/**
 * @brief Returns a readable diagnostic from one callback exception.
 * @param failure Original exception identity.
 * @return Exception text or stable unknown marker.
 * @throws std::bad_alloc when diagnostic allocation fails.
 */
std::string exception_diagnostic(const std::exception_ptr& failure) {
  if (failure == nullptr) {
    return "accepted B1 I/O task failed without exception identity";
  }
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "accepted B1 I/O task threw a non-standard exception";
  }
}

/**
 * @brief Classifies an accepted callback failure without losing its diagnostic.
 * @param failure Original callback exception.
 * @return Durability, revalidation, or generic task-failure status.
 * @throws Nothing.
 */
B1OutputCommitStatus classify_task_failure(
    const std::exception_ptr& failure) noexcept {
  try {
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  } catch (const B1DurabilityError&) {
    return B1OutputCommitStatus::DurabilityUnsupported;
  } catch (const B1RevalidationError&) {
    return B1OutputCommitStatus::RevalidationFailed;
  } catch (...) {
    return B1OutputCommitStatus::TaskFailed;
  }
  return B1OutputCommitStatus::TaskFailed;
}

/**
 * @brief Best-effort removes one exact failed occurrence slot.
 * @param slot Fully resolved store-owned child path.
 * @return Nothing.
 * @throws Nothing; cleanup failure cannot upgrade the typed commit result.
 */
void cleanup_failed_slot(const std::filesystem::path& slot) noexcept {
  std::error_code error;
  std::filesystem::remove_all(slot, error);
}

#endif

}  // namespace

B1OutputStore::B1OutputStore(std::filesystem::path root,
                             execution::ComputeIoExecutor& executor,
                             B1OutputStoreOptions options)
    : root_(std::filesystem::canonical(std::move(root))),
      executor_(executor),
      options_(options) {
  if (!std::filesystem::is_directory(root_)) {
    throw std::invalid_argument("B1 output root is not an existing directory.");
  }
#if !defined(_WIN32)
  struct stat identity{};
  if (::stat(root_.c_str(), &identity) != 0) {
    throw_errno("stat selected B1 output root");
  }
  root_device_ = static_cast<std::uint64_t>(identity.st_dev);
  root_inode_ = static_cast<std::uint64_t>(identity.st_ino);
#endif
}

B1OutputCommitResult B1OutputStore::commit(const B1JobInstance& job,
                                           const ImageBuffer& image) {
  B1OutputCommitResult result;
  try {
    validate_b1_job_instance(job);
    if (options_.requested_durability != B1OutputDurability::CrashDurable) {
      result.status = B1OutputCommitStatus::InvalidRequest;
      result.diagnostic = "B1 requires requested crash-durable output.";
      return result;
    }
    if (!options_.crash_durability_supported) {
      result.status = B1OutputCommitStatus::DurabilityUnsupported;
      result.diagnostic = "Selected B1 backend cannot prove crash durability.";
      return result;
    }
  } catch (const std::exception& error) {
    result.status = B1OutputCommitStatus::InvalidRequest;
    result.diagnostic = error.what();
    return result;
  }
  try {
    validate_b1_candidate_image(image);
  } catch (const std::exception& error) {
    result.status = B1OutputCommitStatus::InvalidImage;
    result.diagnostic = error.what();
    return result;
  }

#if defined(_WIN32)
  result.status = B1OutputCommitStatus::DurabilityUnsupported;
  result.diagnostic =
      "B1 crash-durable no-replace/barrier backend is unsupported on Windows.";
  return result;
#else

  std::error_code filesystem_error;
  const std::filesystem::path current_root =
      std::filesystem::canonical(root_, filesystem_error);
  struct stat current_root_identity{};
  if (filesystem_error || current_root != root_ ||
      !std::filesystem::is_directory(root_, filesystem_error) ||
      filesystem_error || ::stat(root_.c_str(), &current_root_identity) != 0 ||
      static_cast<std::uint64_t>(current_root_identity.st_dev) !=
          root_device_ ||
      static_cast<std::uint64_t>(current_root_identity.st_ino) != root_inode_) {
    result.status = B1OutputCommitStatus::RootUnavailable;
    result.diagnostic = "Selected B1 output root is unavailable or moved.";
    return result;
  }

  B1Sha256 commit_hash;
  commit_hash.update("execution-profile-output-commit-id-v1\n");
  commit_hash.update(encode_b1_job_instance(job));
  const std::string commit_id = b1_digest_hex(commit_hash.finish());
  const std::filesystem::path rooted_slot = "occurrence-" + commit_id;
  const std::filesystem::path slot = root_ / rooted_slot;
  if (!std::filesystem::create_directory(slot, filesystem_error)) {
    result.status = filesystem_error ? B1OutputCommitStatus::RootUnavailable
                                     : B1OutputCommitStatus::SlotExists;
    result.diagnostic =
        filesystem_error ? "Cannot create the B1 occurrence slot: " +
                               filesystem_error.message()
                         : "The immutable B1 occurrence slot already exists.";
    return result;
  }

  auto state = std::make_shared<B1CommitTaskState>();
  state->slot = slot;
  state->image = image;
  result.io_observations.push_back(
      B1ComputeIoObservation{B1IoObservationPoint::Initial, std::nullopt, 0U,
                             std::nullopt, std::nullopt, executor_.snapshot()});

  /**
   * @brief Retains the required quiescent row boundary after terminal cleanup.
   * @return Nothing after appending the authority-free executor snapshot.
   * @throws std::bad_alloc when evidence storage cannot grow.
   * @note Every path after `Initial` calls this exactly once before returning.
   */
  const auto append_final_observation = [&]() {
    result.io_observations.push_back(B1ComputeIoObservation{
        B1IoObservationPoint::Final, std::nullopt, 0U, std::nullopt,
        std::nullopt, executor_.snapshot()});
  };

  const auto run_task = [&](const B1IoTaskIdentity& identity,
                            std::uint64_t planned_bytes,
                            const execution::ComputeIoExecutor::Task& task)
      -> std::optional<execution::ComputeIoTaskResult> {
    for (std::size_t attempt_number = 1U;
         attempt_number <= kB1CapacityAdmissionAttemptLimit; ++attempt_number) {
      const std::shared_ptr<const void> lifetime = state;
      const execution::ComputeIoSubmission submission = executor_.try_submit(
          planned_bytes, lifetime, [task]() { return task; });
      const execution::ComputeIoExecutorSnapshot snapshot =
          executor_.snapshot();
      result.io_observations.push_back(B1ComputeIoObservation{
          submission.accepted() ? B1IoObservationPoint::AcceptedAdmission
                                : B1IoObservationPoint::OfferRejected,
          identity, planned_bytes, submission.admission_status(), std::nullopt,
          snapshot});
      if (submission.accepted()) {
        const execution::ComputeIoTaskResult completion =
            submission.completion().wait();
        result.io_observations.push_back(
            B1ComputeIoObservation{B1IoObservationPoint::Settlement, identity,
                                   planned_bytes, submission.admission_status(),
                                   completion.status(), executor_.snapshot()});
        return completion;
      }
      if (submission.admission_status() !=
              execution::ComputeIoAdmissionStatus::TaskLimit &&
          submission.admission_status() !=
              execution::ComputeIoAdmissionStatus::PlannedByteLimit) {
        return std::nullopt;
      }
      if (planned_bytes > snapshot.planned_bytes_limit) {
        return std::nullopt;
      }
      if (options_.capacity_rejection_observer != nullptr) {
        options_.capacity_rejection_observer(
            options_.capacity_rejection_observer_context, identity,
            attempt_number);
      }
      if (attempt_number == kB1CapacityAdmissionAttemptLimit) {
        return std::nullopt;
      }
      std::this_thread::yield();
    }
    return std::nullopt;
  };

  const B1IoTaskIdentity payload_identity{job, B1IoStage::PayloadStage, 0U};
  const auto payload = run_task(payload_identity, kB1PayloadBytes, [state]() {
    write_and_validate_payload(state.get());
  });
  if (!payload.has_value()) {
    result.status = B1OutputCommitStatus::AdmissionFailed;
    result.diagnostic = "B1 payload admission failed after at most " +
                        std::to_string(kB1CapacityAdmissionAttemptLimit) +
                        " deterministic attempts.";
    cleanup_failed_slot(slot);
    append_final_observation();
    return result;
  }
  if (payload->status() != execution::ComputeIoCompletionStatus::Succeeded) {
    result.status = classify_task_failure(payload->failure());
    result.diagnostic = exception_diagnostic(payload->failure());
    cleanup_failed_slot(slot);
    append_final_observation();
    return result;
  }

  state->manifest = b1_artifact_manifest(job.job_index, state->payload_digest);
  const B1IoTaskIdentity manifest_identity{job, B1IoStage::ManifestCommit, 0U};
  const auto manifest =
      run_task(manifest_identity, b1_manifest_length(job.job_index),
               [state, root = root_]() {
                 commit_and_validate_manifest(state.get(), root);
               });
  if (!manifest.has_value()) {
    result.status = B1OutputCommitStatus::AdmissionFailed;
    result.diagnostic = "B1 manifest admission failed after at most " +
                        std::to_string(kB1CapacityAdmissionAttemptLimit) +
                        " deterministic attempts.";
    cleanup_failed_slot(slot);
    append_final_observation();
    return result;
  }
  if (manifest->status() != execution::ComputeIoCompletionStatus::Succeeded) {
    result.status = classify_task_failure(manifest->failure());
    result.diagnostic = exception_diagnostic(manifest->failure());
    cleanup_failed_slot(slot);
    append_final_observation();
    return result;
  }

  append_final_observation();
  try {
    const Value candidate =
        value_image_adapter::snapshot_cpu_image_value(image);
    const ContentDigestResult logical = compute_content_digest(candidate);
    if (logical.state != ContentDigestState::Available ||
        !logical.digest.has_value()) {
      throw B1RevalidationError("B1 candidate logical digest is unavailable: " +
                                logical.diagnostic);
    }
    result.receipt =
        B1OutputCommitReceipt{commit_id,
                              root_,
                              rooted_slot,
                              job,
                              "dense-tensor-hwc-fp32-rgba-2048x2048",
                              *logical.digest,
                              1U,
                              kPayloadName,
                              kManifestName,
                              kB1PayloadBytes,
                              b1_manifest_length(job.job_index),
                              state->payload_digest,
                              state->manifest_digest,
                              B1OutputDurability::CrashDurable,
                              B1OutputDurability::CrashDurable,
                              state->published_identity};
    result.status = B1OutputCommitStatus::Succeeded;
    return result;
  } catch (const std::exception& error) {
    result.status = B1OutputCommitStatus::RevalidationFailed;
    result.diagnostic = error.what();
    cleanup_failed_slot(slot);
    return result;
  }
#endif
}

}  // namespace ps::benchmark
