/**
 * @file plugin_trust.cpp
 * @brief Implements signed native plugin trust and retained exact-file
 * admission.
 */

#include "plugin/plugin_trust.hpp"  // NOLINT(build/include_subdir)

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif
#endif

namespace ps {
namespace {

/** @brief Maximum accepted canonical manifest bytes. */
constexpr std::size_t kMaximumManifestBytes = 4U * 1024U * 1024U;
/** @brief Exact lowercase-hex detached Ed25519 signature length. */
constexpr std::size_t kSignatureHexBytes = 128U;
/** @brief Maximum accepted public-key PEM bytes. */
constexpr std::size_t kMaximumPublicKeyBytes = 64U * 1024U;
/**
 * @brief Complete immutable manifest entry sorted by signed identity.
 * @throws Nothing for moves; path-free scalar/vector values allocate nowhere.
 */
struct TrustEntry final {
  /** @brief Closed role. */
  PluginArtifactKind kind = PluginArtifactKind::Operation;
  /** @brief Nonzero package and positive generation. */
  PluginPackageIdentity package;
  /** @brief Approved content digest. */
  PluginContentDigest digest;
};

/**
 * @brief Closes a PEM-owned EVP key through one unique pointer.
 * @param key OpenSSL key pointer, possibly null.
 * @return Nothing.
 * @throws Nothing.
 */
void free_evp_key(EVP_PKEY* key) noexcept {
  EVP_PKEY_free(key);
}

/**
 * @brief Frees an OpenSSL memory BIO through one unique pointer.
 * @param bio OpenSSL BIO pointer, possibly null.
 * @return Nothing.
 * @throws Nothing.
 */
void free_bio(BIO* bio) noexcept {
  BIO_free(bio);
}

/**
 * @brief Frees an OpenSSL digest context through one unique pointer.
 * @param context OpenSSL context pointer, possibly null.
 * @return Nothing.
 * @throws Nothing.
 */
void free_digest_context(EVP_MD_CTX* context) noexcept {
  EVP_MD_CTX_free(context);
}

using UniqueEvpKey = std::unique_ptr<EVP_PKEY, decltype(&free_evp_key)>;
using UniqueBio = std::unique_ptr<BIO, decltype(&free_bio)>;
using UniqueDigestContext =
    std::unique_ptr<EVP_MD_CTX, decltype(&free_digest_context)>;  // NOLINT

/**
 * @brief Returns whether one byte is lowercase hexadecimal ASCII.
 * @param value Candidate byte.
 * @return True for `0-9` or `a-f`.
 * @throws Nothing.
 */
bool is_lower_hex(char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

/**
 * @brief Converts one lowercase hexadecimal nibble.
 * @param value Previously validated lowercase hexadecimal byte.
 * @return Numeric nibble value.
 * @throws Nothing.
 */
std::uint8_t hex_nibble(char value) noexcept {
  return static_cast<std::uint8_t>(value <= '9' ? value - '0'
                                                : value - 'a' + 10);
}

/**
 * @brief Decodes an exact lowercase hexadecimal field.
 * @tparam Size Required decoded byte count.
 * @param value Candidate exact-length field.
 * @param field_name Diagnostic field name.
 * @return Decoded fixed-size byte array.
 * @throws PluginTrustError for wrong length or noncanonical bytes.
 */
template <std::size_t Size>
std::array<std::uint8_t, Size> decode_lower_hex(std::string_view value,
                                                const char* field_name) {
  if (value.size() != Size * 2U ||
      !std::all_of(value.begin(), value.end(), is_lower_hex)) {
    throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                           std::string("Plugin trust ") + field_name +
                               " is not exact lowercase hexadecimal text.");
  }
  std::array<std::uint8_t, Size> decoded{};
  for (std::size_t index = 0U; index < Size; ++index) {
    decoded[index] =
        static_cast<std::uint8_t>((hex_nibble(value[index * 2U]) << 4U) |
                                  hex_nibble(value[index * 2U + 1U]));
  }
  return decoded;
}

/**
 * @brief Reads a non-symlink regular configuration file with one hard bound.
 * @param path Operator-selected path.
 * @param maximum_bytes Inclusive content bound.
 * @param label Diagnostic file role.
 * @return Complete retained bytes.
 * @throws PluginTrustError for path, type, size, read, or replacement faults.
 * @throws std::bad_alloc when bounded retained storage cannot allocate.
 */
std::vector<std::byte> read_bounded_configuration_file(
    const std::filesystem::path& path, std::size_t maximum_bytes,
    const char* label) {
#ifdef _WIN32
  const HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw PluginTrustError(PluginTrustErrorCode::ConfigurationInvalid,
                           std::string("Cannot open plugin trust ") + label);
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information) ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    CloseHandle(handle);
    throw PluginTrustError(PluginTrustErrorCode::ConfigurationInvalid,
                           std::string("Plugin trust ") + label +
                               " is not a non-reparse regular file.");
  }
  const std::uint64_t size =
      (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
      information.nFileSizeLow;
  if (size > maximum_bytes) {
    CloseHandle(handle);
    throw PluginTrustError(
        PluginTrustErrorCode::ConfigurationInvalid,
        std::string("Plugin trust ") + label + " exceeds its size bound.");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  DWORD read = 0U;
  if (size != 0U && (!ReadFile(handle, bytes.data(), static_cast<DWORD>(size),
                               &read, nullptr) ||
                     read != size)) {
    CloseHandle(handle);
    throw PluginTrustError(PluginTrustErrorCode::ConfigurationInvalid,
                           std::string("Cannot read plugin trust ") + label);
  }
  CloseHandle(handle);
  return bytes;
#else
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0) {
    throw PluginTrustError(PluginTrustErrorCode::ConfigurationInvalid,
                           std::string("Cannot open plugin trust ") + label +
                               ": " + std::strerror(errno));
  }
  struct stat before{};
  if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < 0 ||
      static_cast<std::uint64_t>(before.st_size) > maximum_bytes) {
    ::close(descriptor);
    throw PluginTrustError(PluginTrustErrorCode::ConfigurationInvalid,
                           std::string("Plugin trust ") + label +
                               " is not a bounded regular file.");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(before.st_size));
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t count =
        ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      ::close(descriptor);
      throw PluginTrustError(
          PluginTrustErrorCode::ConfigurationInvalid,
          std::string("Cannot completely read plugin trust ") + label);
    }
    offset += static_cast<std::size_t>(count);
  }
  struct stat after{};
  const bool stable =
      ::fstat(descriptor, &after) == 0 && before.st_dev == after.st_dev &&
      before.st_ino == after.st_ino && before.st_size == after.st_size &&
      before.st_mtime == after.st_mtime;
  ::close(descriptor);
  if (!stable) {
    throw PluginTrustError(
        PluginTrustErrorCode::ConfigurationInvalid,
        std::string("Plugin trust ") + label + " changed while it was read.");
  }
  return bytes;
#endif
}

/**
 * @brief Parses one exact canonical positive decimal generation.
 * @param value Candidate decimal field.
 * @return Positive `uint64_t` generation.
 * @throws PluginTrustError for empty, leading-zero, nondecimal, zero, or
 * overflow.
 */
std::uint64_t parse_generation(std::string_view value) {
  if (value.empty() || value.front() == '0') {
    throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                           "Plugin trust generation is not canonical.");
  }
  std::uint64_t generation = 0U;
  for (char byte : value) {
    if (byte < '0' || byte > '9') {
      throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                             "Plugin trust generation is not decimal.");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(byte - '0');
    if (generation >
        (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                             "Plugin trust generation overflows uint64.");
    }
    generation = generation * 10U + digit;
  }
  if (generation == 0U) {
    throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                           "Plugin trust generation is zero.");
  }
  return generation;
}

/**
 * @brief Converts exact kind text to the closed role.
 * @param value Canonical manifest token.
 * @return Matching role.
 * @throws PluginTrustError for unsupported text.
 */
PluginArtifactKind parse_kind(std::string_view value) {
  if (value == "operation") {
    return PluginArtifactKind::Operation;
  }
  if (value == "policy") {
    return PluginArtifactKind::Policy;
  }
  if (value == "isolated-runtime") {
    return PluginArtifactKind::IsolatedRuntime;
  }
  throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                         "Plugin trust manifest contains an unknown kind.");
}

/**
 * @brief Parses exact manifest bytes into uniquely mapped ordered entries.
 * @param bytes Verified-by-caller candidate manifest bytes.
 * @param trust_root Receives canonical trust-root id.
 * @return Strictly sorted entries unique by identity and content-role key.
 * @throws PluginTrustError for every noncanonical structural fact.
 * @throws std::bad_alloc when bounded retained state cannot allocate.
 */
std::vector<TrustEntry> parse_manifest(const std::vector<std::byte>& bytes,
                                       std::string* trust_root) {
  if (trust_root == nullptr || bytes.empty() ||
      static_cast<char>(bytes.back()) != '\n') {
    throw PluginTrustError(
        PluginTrustErrorCode::ManifestInvalid,
        "Plugin trust manifest is empty or not LF terminated.");
  }
  std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (text.find('\0') != std::string::npos ||
      text.find('\r') != std::string::npos ||
      !std::all_of(text.begin(), text.end(), [](unsigned char byte) {
        return byte == '\n' || (byte >= 0x20U && byte <= 0x7eU);
      })) {
    throw PluginTrustError(
        PluginTrustErrorCode::ManifestInvalid,
        "Plugin trust manifest is not canonical ASCII/LF text.");
  }
  std::istringstream stream(text);
  std::string line;
  if (!std::getline(stream, line) ||
      line != "photospider-plugin-trust-manifest-v1") {
    throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                           "Plugin trust manifest version header is invalid.");
  }
  if (!std::getline(stream, line) || line.rfind("trust-root ", 0U) != 0U ||
      line.size() <= std::string("trust-root ").size()) {
    throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                           "Plugin trust root id is absent or malformed.");
  }
  *trust_root = line.substr(std::string("trust-root ").size());
  if (!std::all_of(trust_root->begin(), trust_root->end(), [](char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
               byte == '.' || byte == '_' || byte == '-';
      })) {
    throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                           "Plugin trust root id is not canonical.");
  }

  std::vector<TrustEntry> entries;
  std::string previous_line;
  std::set<std::tuple<PluginArtifactKind, PluginPackageId, std::uint64_t>>
      identities;
  std::set<std::pair<PluginArtifactKind, PluginContentDigest>> content_roles;
  while (std::getline(stream, line)) {
    if (line.empty() || (!previous_line.empty() && line <= previous_line)) {
      throw PluginTrustError(
          PluginTrustErrorCode::ManifestInvalid,
          "Plugin trust entries are empty, duplicate, or unsorted.");
    }
    std::istringstream row(line);
    std::string kind_value;
    std::string package_value;
    std::string generation_value;
    std::string digest_value;
    std::string trailing;
    if (!(row >> kind_value >> package_value >> generation_value >>
          digest_value) ||
        (row >> trailing)) {
      throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                             "Plugin trust entry does not have four fields.");
    }
    TrustEntry entry;
    entry.kind = parse_kind(kind_value);
    entry.package.package_id =
        decode_lower_hex<16U>(package_value, "package id");
    if (std::all_of(entry.package.package_id.begin(),
                    entry.package.package_id.end(),
                    [](std::uint8_t byte) { return byte == 0U; })) {
      throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                             "Plugin trust package id is zero.");
    }
    entry.package.generation = parse_generation(generation_value);
    entry.digest = decode_lower_hex<32U>(digest_value, "content digest");
    const auto identity = std::make_tuple(entry.kind, entry.package.package_id,
                                          entry.package.generation);
    if (!identities.insert(identity).second) {
      throw PluginTrustError(
          PluginTrustErrorCode::ManifestInvalid,
          "Plugin trust manifest contains a duplicate signed identity.");
    }
    if (!content_roles.insert(std::make_pair(entry.kind, entry.digest))
             .second) {
      throw PluginTrustError(
          PluginTrustErrorCode::ManifestInvalid,
          "Plugin trust manifest contains a duplicate content-role mapping.");
    }
    entries.push_back(entry);
    previous_line = line;
  }
  if (entries.empty()) {
    throw PluginTrustError(PluginTrustErrorCode::ManifestInvalid,
                           "Plugin trust manifest contains no entries.");
  }
  return entries;
}

/**
 * @brief Verifies the exact manifest bytes against one Ed25519 key/signature.
 * @param manifest Complete canonical signed bytes.
 * @param signature_hex Exact lowercase signature file bytes with optional LF.
 * @param public_key_pem Complete bounded PEM bytes.
 * @return Nothing after successful verification.
 * @throws PluginTrustError for key, algorithm, signature, or EVP failure.
 */
void verify_manifest_signature(const std::vector<std::byte>& manifest,
                               std::vector<std::byte> signature_hex,
                               const std::vector<std::byte>& public_key_pem) {
  if (!signature_hex.empty() &&
      static_cast<char>(signature_hex.back()) == '\n') {
    signature_hex.pop_back();
  }
  const std::string signature_text(
      reinterpret_cast<const char*>(signature_hex.data()),
      signature_hex.size());
  std::array<std::uint8_t, 64U> signature{};
  try {
    signature = decode_lower_hex<64U>(signature_text, "signature");
  } catch (const PluginTrustError&) {
    throw PluginTrustError(PluginTrustErrorCode::SignatureInvalid,
                           "Plugin trust signature is not canonical hex.");
  }
  UniqueBio bio(BIO_new_mem_buf(public_key_pem.data(),
                                static_cast<int>(public_key_pem.size())),
                &free_bio);
  UniqueEvpKey key(
      bio ? PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr,
      &free_evp_key);
  if (!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519) {
    throw PluginTrustError(PluginTrustErrorCode::SignatureInvalid,
                           "Plugin trust public key is not Ed25519.");
  }
  UniqueDigestContext context(EVP_MD_CTX_new(), &free_digest_context);
  if (!context ||
      EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr,
                           key.get()) != 1 ||
      EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                       reinterpret_cast<const unsigned char*>(manifest.data()),
                       manifest.size()) != 1) {
    throw PluginTrustError(PluginTrustErrorCode::SignatureInvalid,
                           "Plugin trust Ed25519 signature is invalid.");
  }
}

#if defined(__linux__)
/** @brief Maximum accepted Linux native plugin artifact bytes. */
constexpr std::uint64_t kMaximumPluginArtifactBytes = 1ULL << 30U;

/**
 * @brief Unique owner for one POSIX artifact descriptor.
 * @throws Nothing for moves, reset, and destruction.
 */
class UniquePosixFd final {
 public:
  /**
   * @brief Takes ownership of one descriptor or invalid sentinel.
   * @param descriptor Descriptor closed at reset/destruction.
   * @throws Nothing.
   */
  explicit UniquePosixFd(int descriptor = -1) noexcept
      : descriptor_(descriptor) {}

  /** @brief Closes the retained descriptor once without throwing. */
  ~UniquePosixFd() noexcept { reset(); }

  /** @brief Prevents duplicate descriptor ownership. */
  UniquePosixFd(const UniquePosixFd&) = delete;
  /** @brief Prevents duplicate descriptor assignment. */
  UniquePosixFd& operator=(const UniquePosixFd&) = delete;

  /**
   * @brief Transfers one descriptor owner.
   * @param other Source owner cleared after transfer.
   * @throws Nothing.
   */
  UniquePosixFd(UniquePosixFd&& other) noexcept
      : descriptor_(other.release()) {}

  /**
   * @brief Replaces current ownership with one transferred descriptor.
   * @param other Source owner cleared after transfer.
   * @return This owner after closing any prior descriptor.
   * @throws Nothing.
   */
  UniquePosixFd& operator=(UniquePosixFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  /**
   * @brief Returns the retained descriptor without transferring it.
   * @return Descriptor or invalid sentinel.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Transfers the descriptor to the caller.
   * @return Previously retained descriptor or invalid sentinel.
   * @throws Nothing.
   */
  int release() noexcept { return std::exchange(descriptor_, -1); }

  /**
   * @brief Closes current ownership and optionally adopts a replacement.
   * @param replacement Descriptor to retain after reset.
   * @return Nothing.
   * @throws Nothing; close errors do not restore authority.
   */
  void reset(int replacement = -1) noexcept {
    const int retired = std::exchange(descriptor_, replacement);
    if (retired >= 0) {
      static_cast<void>(::close(retired));
    }
  }

 private:
  /** @brief Sole owned descriptor. */
  int descriptor_ = -1;
};

/**
 * @brief Moves a private snapshot descriptor above the standard streams.
 * @param descriptor Nonnull unique descriptor owner.
 * @return Nothing after retaining a descriptor greater than two.
 * @throws PluginTrustError when the owner is null/invalid or duplication fails.
 * @note Standard-stream slots are never returned as executable/loader
 * capabilities because child setup assigns fixed meanings to low descriptors.
 */
void normalize_private_snapshot_descriptor(UniquePosixFd* descriptor) {
  if (descriptor == nullptr || descriptor->get() < 0) {
    throw PluginTrustError(PluginTrustErrorCode::ExactObjectUnsupported,
                           "Private plugin snapshot descriptor is invalid.");
  }
  if (descriptor->get() > STDERR_FILENO) {
    return;
  }
  int normalized = -1;
#ifdef F_DUPFD_CLOEXEC
  normalized = ::fcntl(descriptor->get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
  normalized = ::fcntl(descriptor->get(), F_DUPFD, STDERR_FILENO + 1);
  if (normalized >= 0 && ::fcntl(normalized, F_SETFD, FD_CLOEXEC) != 0) {
    const int duplicate_error = errno;
    static_cast<void>(::close(normalized));
    normalized = -1;
    errno = duplicate_error;
  }
#endif
  if (normalized < 0) {
    throw PluginTrustError(
        PluginTrustErrorCode::ExactObjectUnsupported,
        "Cannot normalize private plugin snapshot descriptor.");
  }
  descriptor->reset(normalized);
}

/**
 * @brief Hashes one retained candidate descriptor from offset zero.
 * @param descriptor Open regular-file descriptor.
 * @return Exact SHA-256 digest.
 * @throws PluginTrustError for seek, read, EVP, or stability failure.
 */
PluginContentDigest hash_candidate_descriptor(int descriptor) {
  if (::lseek(descriptor, 0, SEEK_SET) < 0) {
    throw PluginTrustError(
        PluginTrustErrorCode::ArtifactInvalid,
        "Plugin artifact descriptor cannot seek for hashing.");
  }
  UniqueDigestContext context(EVP_MD_CTX_new(), &free_digest_context);
  if (!context ||
      EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    throw PluginTrustError(PluginTrustErrorCode::ArtifactInvalid,
                           "Cannot initialize plugin SHA-256 hashing.");
  }
  std::array<unsigned char, 64U * 1024U> buffer{};
  while (true) {
    const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 ||
        (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(),
                                       static_cast<std::size_t>(count)) != 1)) {
      throw PluginTrustError(PluginTrustErrorCode::ArtifactInvalid,
                             "Cannot hash complete plugin artifact bytes.");
    }
    if (count == 0) {
      break;
    }
  }
  PluginContentDigest digest{};
  unsigned int digest_size = 0U;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 ||
      digest_size != digest.size()) {
    throw PluginTrustError(PluginTrustErrorCode::ArtifactInvalid,
                           "Plugin SHA-256 digest finalization failed.");
  }
  return digest;
}

/**
 * @brief Copies an exact bounded source extent into one private descriptor.
 * @param source_descriptor Stable candidate descriptor.
 * @param destination_descriptor Newly created private snapshot descriptor.
 * @param source_size Exact positive byte count to copy.
 * @return Nothing after writing every source byte exactly once.
 * @throws PluginTrustError with `ArtifactInvalid` for incomplete source reads
 * or `ExactObjectUnsupported` for destination write failures.
 * @note The destination must be empty and exclusively Host-owned. The caller
 * confirms the digest on the immutable/reopened destination afterward.
 */
void copy_artifact_bytes(int source_descriptor, int destination_descriptor,
                         std::uint64_t source_size) {
  std::array<unsigned char, 64U * 1024U> buffer{};
  std::uint64_t offset = 0U;
  while (offset != source_size) {
    const std::size_t requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), source_size - offset));
    ssize_t count = -1;
    do {
      count = ::pread(source_descriptor, buffer.data(), requested,
                      static_cast<off_t>(offset));
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
      throw PluginTrustError(
          PluginTrustErrorCode::ArtifactInvalid,
          "Cannot copy complete plugin artifact source bytes.");
    }
    std::size_t written_offset = 0U;
    while (written_offset != static_cast<std::size_t>(count)) {
      ssize_t written = -1;
      do {
        written =
            ::write(destination_descriptor, buffer.data() + written_offset,
                    static_cast<std::size_t>(count) - written_offset);
      } while (written < 0 && errno == EINTR);
      if (written <= 0) {
        throw PluginTrustError(
            PluginTrustErrorCode::ExactObjectUnsupported,
            "Cannot persist complete private plugin snapshot bytes.");
      }
      written_offset += static_cast<std::size_t>(written);
    }
    offset += static_cast<std::uint64_t>(count);
  }
}

/**
 * @brief Confirms one immutable private snapshot preserves approved bytes.
 * @param descriptor Retained snapshot descriptor.
 * @param expected_size Exact copied source size.
 * @param expected_digest Manifest-approved SHA-256 digest.
 * @return Nothing when type, size, and digest all match.
 * @throws PluginTrustError for type, size, or digest mismatch.
 * @note This check runs only after irreversible Linux memfd sealing, so the
 * returned authorization never relies on the mutable candidate descriptor.
 */
void confirm_private_snapshot(int descriptor, std::uint64_t expected_size,
                              const PluginContentDigest& expected_digest) {
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) != expected_size) {
    throw PluginTrustError(
        PluginTrustErrorCode::ExactObjectUnsupported,
        "Private plugin snapshot type or size is not stable.");
  }
  if (hash_candidate_descriptor(descriptor) != expected_digest) {
    throw PluginTrustError(
        PluginTrustErrorCode::ArtifactInvalid,
        "Private plugin snapshot does not preserve approved content.");
  }
}

#if defined(__linux__)
/** @brief Linux `fcntl` command that atomically adds memfd seals. */
#if defined(F_ADD_SEALS)
constexpr int kLinuxAddSealsCommand = F_ADD_SEALS;
#else
constexpr int kLinuxAddSealsCommand = 1033;
#endif

/** @brief Linux `fcntl` command that reports all applied memfd seals. */
#if defined(F_GET_SEALS)
constexpr int kLinuxGetSealsCommand = F_GET_SEALS;
#else
constexpr int kLinuxGetSealsCommand = 1034;
#endif

/** @brief Linux seal that permanently prevents adding further seals. */
#if defined(F_SEAL_SEAL)
constexpr int kLinuxSealSeal = F_SEAL_SEAL;
#else
constexpr int kLinuxSealSeal = 0x0001;
#endif

/** @brief Linux seal that prevents shrinking the snapshot. */
#if defined(F_SEAL_SHRINK)
constexpr int kLinuxSealShrink = F_SEAL_SHRINK;
#else
constexpr int kLinuxSealShrink = 0x0002;
#endif

/** @brief Linux seal that prevents growing the snapshot. */
#if defined(F_SEAL_GROW)
constexpr int kLinuxSealGrow = F_SEAL_GROW;
#else
constexpr int kLinuxSealGrow = 0x0004;
#endif

/** @brief Linux seal that prevents every future write to the snapshot. */
#if defined(F_SEAL_WRITE)
constexpr int kLinuxSealWrite = F_SEAL_WRITE;
#else
constexpr int kLinuxSealWrite = 0x0008;
#endif

/**
 * @brief Copies approved bytes into one sealed anonymous Linux memfd.
 * @param source_descriptor Stable verified candidate descriptor.
 * @param source_size Exact positive bounded source size.
 * @param expected_digest Manifest-approved SHA-256 digest.
 * @return Unique owner for the sealed descriptor after digest confirmation.
 * @throws PluginTrustError for memfd, copy, permission, seal, or digest faults.
 * @note The exact seals are write, grow, shrink, and seal. They are applied
 * before the confirmation hash and remain irreversible for capability life.
 */
UniquePosixFd create_linux_sealed_snapshot(
    int source_descriptor, std::uint64_t source_size,
    const PluginContentDigest& expected_digest) {
#if defined(SYS_memfd_create)
  int descriptor = static_cast<int>(
      ::syscall(SYS_memfd_create, "photospider-plugin-snapshot",
                static_cast<unsigned int>(MFD_ALLOW_SEALING | MFD_CLOEXEC)));
  if (descriptor < 0) {
    throw PluginTrustError(
        PluginTrustErrorCode::ExactObjectUnsupported,
        std::string("Cannot create sealed Linux plugin snapshot: ") +
            std::strerror(errno));
  }
  UniquePosixFd snapshot(descriptor);
  normalize_private_snapshot_descriptor(&snapshot);
  descriptor = snapshot.get();
  copy_artifact_bytes(source_descriptor, descriptor, source_size);
  if (::fchmod(descriptor, S_IRUSR | S_IXUSR) != 0) {
    throw PluginTrustError(
        PluginTrustErrorCode::ExactObjectUnsupported,
        "Cannot set sealed Linux plugin snapshot permissions.");
  }
  constexpr int kRequiredSeals =
      kLinuxSealWrite | kLinuxSealGrow | kLinuxSealShrink | kLinuxSealSeal;
  if (::fcntl(descriptor, kLinuxAddSealsCommand, kRequiredSeals) != 0) {
    throw PluginTrustError(
        PluginTrustErrorCode::ExactObjectUnsupported,
        "Cannot apply immutable Linux plugin snapshot seals.");
  }
  const int applied_seals = ::fcntl(descriptor, kLinuxGetSealsCommand);
  if (applied_seals < 0 || (applied_seals & kRequiredSeals) != kRequiredSeals) {
    throw PluginTrustError(
        PluginTrustErrorCode::ExactObjectUnsupported,
        "Cannot confirm immutable Linux plugin snapshot seals.");
  }
  confirm_private_snapshot(descriptor, source_size, expected_digest);
  return snapshot;
#else
  static_cast<void>(source_descriptor);
  static_cast<void>(source_size);
  static_cast<void>(expected_digest);
  throw PluginTrustError(PluginTrustErrorCode::ExactObjectUnsupported,
                         "Linux memfd creation is unavailable.");
#endif
}
#endif

#endif

}  // namespace

/** @brief Immutable verified trust policy state. */
struct PluginTrustPolicy::Impl final {
  /** @brief Canonical operator diagnostic root id. */
  std::string trust_root_id;
  /** @brief Strictly sorted identity- and content-role-unique entries. */
  std::vector<TrustEntry> entries;
};

/** @copydoc operator==(const PluginPackageIdentity&, const
 * PluginPackageIdentity&) */
bool operator==(const PluginPackageIdentity& lhs,
                const PluginPackageIdentity& rhs) noexcept {
  return lhs.package_id == rhs.package_id && lhs.generation == rhs.generation;
}

/** @copydoc operator!=(const PluginPackageIdentity&, const
 * PluginPackageIdentity&) */
bool operator!=(const PluginPackageIdentity& lhs,
                const PluginPackageIdentity& rhs) noexcept {
  return !(lhs == rhs);
}

/** @copydoc PluginTrustError::PluginTrustError */
PluginTrustError::PluginTrustError(PluginTrustErrorCode code,
                                   std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

/** @copydoc AuthorizedPluginFile::AuthorizedPluginFile */
AuthorizedPluginFile::AuthorizedPluginFile(std::filesystem::path original_path,
                                           PluginArtifactKind kind,
                                           PluginPackageIdentity package,
                                           PluginContentDigest digest,
                                           std::intptr_t native_owner)
    : original_path_(std::move(original_path)),
      kind_(kind),
      package_(package),
      digest_(digest),
      native_owner_(native_owner) {}  // NOLINT

/** @copydoc AuthorizedPluginFile::AuthorizedPluginFile */
AuthorizedPluginFile::AuthorizedPluginFile(
    AuthorizedPluginFile&& other) noexcept
    : original_path_(std::move(other.original_path_)),
      kind_(other.kind_),
      package_(other.package_),
      digest_(other.digest_),
      native_owner_(other.native_owner_) {
  other.package_ = {};
  other.digest_ = {};
  other.native_owner_ = -1;
}

/** @copydoc AuthorizedPluginFile::operator= */
AuthorizedPluginFile& AuthorizedPluginFile::operator=(
    AuthorizedPluginFile&& other) noexcept {
  if (this != &other) {
    reset();
    original_path_ = std::move(other.original_path_);
    kind_ = other.kind_;
    package_ = other.package_;
    digest_ = other.digest_;
    native_owner_ = other.native_owner_;
    other.package_ = {};
    other.digest_ = {};
    other.native_owner_ = -1;
  }
  return *this;
}

/** @copydoc AuthorizedPluginFile::~AuthorizedPluginFile */
AuthorizedPluginFile::~AuthorizedPluginFile() noexcept {
  reset();
}

/** @copydoc AuthorizedPluginFile::active */
bool AuthorizedPluginFile::active() const noexcept {
  return native_owner_ != -1;
}

/** @copydoc AuthorizedPluginFile::native_load_path */
std::string AuthorizedPluginFile::native_load_path() const {
  if (!active()) {
    throw PluginTrustError(PluginTrustErrorCode::ArtifactInvalid,
                           "Authorized plugin file is inactive.");
  }
#if defined(__linux__)
  return "/proc/self/fd/" + std::to_string(native_owner_);
#else
  throw PluginTrustError(PluginTrustErrorCode::ExactObjectUnsupported,
                         "Exact-object DSO loading is unsupported here.");
#endif
}

/** @copydoc AuthorizedPluginFile::native_descriptor */
int AuthorizedPluginFile::native_descriptor() const noexcept {
#ifdef _WIN32
  return -1;
#else
  return active() ? static_cast<int>(native_owner_) : -1;
#endif
}

/** @copydoc AuthorizedPluginFile::reset */
void AuthorizedPluginFile::reset() noexcept {
  if (!active()) {
    return;
  }
#ifdef _WIN32
  CloseHandle(reinterpret_cast<HANDLE>(native_owner_));
#else
  ::close(static_cast<int>(native_owner_));
#endif
  native_owner_ = -1;
  package_ = {};
  digest_ = {};
  original_path_.clear();
}

/** @copydoc PluginTrustPolicy::PluginTrustPolicy */
PluginTrustPolicy::PluginTrustPolicy(
    std::shared_ptr<const Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

/** @copydoc PluginTrustPolicy::load */
PluginTrustPolicy PluginTrustPolicy::load(
    const PluginTrustConfiguration& configuration) {
  if (configuration.manifest_path.empty() ||
      configuration.signature_path.empty() ||
      configuration.public_key_path.empty()) {
    throw PluginTrustError(PluginTrustErrorCode::ConfigurationInvalid,
                           "Plugin trust configuration paths are incomplete.");
  }
  const std::vector<std::byte> manifest = read_bounded_configuration_file(
      configuration.manifest_path, kMaximumManifestBytes, "manifest");
  const std::vector<std::byte> signature = read_bounded_configuration_file(
      configuration.signature_path, kSignatureHexBytes + 1U, "signature");
  const std::vector<std::byte> public_key = read_bounded_configuration_file(
      configuration.public_key_path, kMaximumPublicKeyBytes, "public key");
  verify_manifest_signature(manifest, signature, public_key);
  auto implementation = std::make_shared<Impl>();
  implementation->entries =
      parse_manifest(manifest, &implementation->trust_root_id);
  return PluginTrustPolicy(std::move(implementation));
}

/** @copydoc PluginTrustPolicy::authorize */
AuthorizedPluginFile PluginTrustPolicy::authorize(
    const std::filesystem::path& candidate, PluginArtifactKind kind,
    std::optional<PluginPackageIdentity> expected_package) const {
  if (!implementation_) {
    throw PluginTrustError(PluginTrustErrorCode::ConfigurationInvalid,
                           "Plugin trust policy is inactive.");
  }
#if defined(__APPLE__)
  static_cast<void>(candidate);
  static_cast<void>(kind);
  static_cast<void>(expected_package);
  throw PluginTrustError(
      PluginTrustErrorCode::ExactObjectUnsupported,
      "Darwin has no unprivileged immutable exact-object boundary for native "
      "plugin loading.");
#elif !defined(__linux__)
  static_cast<void>(candidate);
  static_cast<void>(kind);
  static_cast<void>(expected_package);
  throw PluginTrustError(
      PluginTrustErrorCode::ExactObjectUnsupported,
      "Private immutable plugin snapshots are unsupported on this platform.");
#else
  if (kind != PluginArtifactKind::IsolatedRuntime &&
      expected_package.has_value()) {
    throw PluginTrustError(
        PluginTrustErrorCode::ConfigurationInvalid,
        "Expected package identity is valid only for isolated-runtime trust.");
  }
  const std::filesystem::path absolute = std::filesystem::absolute(candidate);
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  UniquePosixFd source(::open(absolute.c_str(), flags));
  if (source.get() < 0) {
    throw PluginTrustError(
        PluginTrustErrorCode::ArtifactInvalid,
        std::string("Cannot open plugin artifact: ") + std::strerror(errno));
  }
  struct stat before{};
  if (::fstat(source.get(), &before) != 0 || !S_ISREG(before.st_mode)) {
    throw PluginTrustError(PluginTrustErrorCode::ArtifactInvalid,
                           "Plugin artifact is not a regular file.");
  }
  if (before.st_size <= 0 || static_cast<std::uint64_t>(before.st_size) >
                                 kMaximumPluginArtifactBytes) {
    throw PluginTrustError(PluginTrustErrorCode::ArtifactInvalid,
                           "Plugin artifact size is outside its bound.");
  }
  const PluginContentDigest digest = hash_candidate_descriptor(source.get());
  struct stat after{};
  if (::fstat(source.get(), &after) != 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_mtime != after.st_mtime) {
    throw PluginTrustError(PluginTrustErrorCode::ArtifactInvalid,
                           "Plugin artifact changed while it was hashed.");
  }
  const auto matching = std::find_if(
      implementation_->entries.begin(), implementation_->entries.end(),
      [kind, &digest](const TrustEntry& entry) {
        return entry.kind == kind && entry.digest == digest;
      });
  if (matching == implementation_->entries.end()) {
    throw PluginTrustError(
        PluginTrustErrorCode::ArtifactNotApproved,
        "Plugin artifact content and role are not approved.");
  }
  if (expected_package.has_value() && matching->package != *expected_package) {
    throw PluginTrustError(PluginTrustErrorCode::PackageMismatch,
                           "Plugin runtime package identity is not approved.");
  }
  UniquePosixFd snapshot = create_linux_sealed_snapshot(
      source.get(), static_cast<std::uint64_t>(before.st_size), digest);
  source.reset();
  return AuthorizedPluginFile(absolute, kind, matching->package,
                              matching->digest, snapshot.release());
#endif
}

/** @copydoc PluginTrustPolicy::trust_root_id */
const std::string& PluginTrustPolicy::trust_root_id() const noexcept {
  return implementation_->trust_root_id;
}

/** @copydoc process_plugin_trust_policy */
const PluginTrustPolicy& process_plugin_trust_policy() {
  /**
   * @brief Caches either the first verified policy or its first failure.
   * @throws Nothing from construction because failures are retained.
   * @note This prevents C++ function-local-static retry semantics from turning
   * an initial configuration failure into later process reconfiguration.
   */
  struct ProcessTrustState final {
    /** @brief Reads environment once and retains success or failure. */
    ProcessTrustState() noexcept {
      try {
        const char* manifest = std::getenv("PHOTOSPIDER_PLUGIN_TRUST_MANIFEST");
        const char* signature =
            std::getenv("PHOTOSPIDER_PLUGIN_TRUST_SIGNATURE");
        const char* public_key =
            std::getenv("PHOTOSPIDER_PLUGIN_TRUST_PUBLIC_KEY");
        if (manifest == nullptr || signature == nullptr ||
            public_key == nullptr || *manifest == '\0' || *signature == '\0' ||
            *public_key == '\0') {
          throw PluginTrustError(
              PluginTrustErrorCode::ConfigurationInvalid,
              "Plugin trust environment configuration is incomplete.");
        }
        policy.emplace(PluginTrustPolicy::load(
            PluginTrustConfiguration{manifest, signature, public_key}));
      } catch (...) {
        failure = std::current_exception();
      }
    }

    /** @brief First verified policy, or no value after failure. */
    std::optional<PluginTrustPolicy> policy;
    /** @brief First initialization failure retained for exact rethrow. */
    std::exception_ptr failure;
  };

  static const ProcessTrustState state;
  if (state.failure) {
    std::rethrow_exception(state.failure);
  }
  return *state.policy;
}

/** @copydoc authorize_process_plugin */
AuthorizedPluginFile authorize_process_plugin(
    const std::filesystem::path& candidate, PluginArtifactKind kind,
    std::optional<PluginPackageIdentity> expected_package) {
  return process_plugin_trust_policy().authorize(candidate, kind,
                                                 expected_package);
}

}  // namespace ps
