/**
 * @file plugin_trust.hpp
 * @brief Declares fail-closed native plugin content trust and file capability.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace ps {

/** @brief Closed executable role bound by the signed plugin manifest. */
enum class PluginArtifactKind : std::uint8_t {
  /** @brief In-process operation ABI dynamic library. */
  Operation = 0U,

  /** @brief In-process policy ABI dynamic library. */
  Policy = 1U,

  /** @brief Freshly execed isolated CPU runtime package. */
  IsolatedRuntime = 2U,
};

/** @brief Exact 128-bit package identity retained from one signed entry. */
using PluginPackageId = std::array<std::uint8_t, 16U>;

/** @brief Exact SHA-256 content identity retained from one signed entry. */
using PluginContentDigest = std::array<std::uint8_t, 32U>;

/**
 * @brief Signed package identity and monotonic package generation.
 * @throws Nothing for aggregate value operations.
 */
struct PluginPackageIdentity final {
  /** @brief Nonzero exact package identifier. */
  PluginPackageId package_id{};

  /** @brief Positive package generation. */
  std::uint64_t generation = 0U;
};

/**
 * @brief Compares complete package identity and generation.
 * @param lhs First identity.
 * @param rhs Second identity.
 * @return True only for exact package and generation equality.
 * @throws Nothing.
 */
bool operator==(const PluginPackageIdentity& lhs,
                const PluginPackageIdentity& rhs) noexcept;

/**
 * @brief Reports any package identity or generation difference.
 * @param lhs First identity.
 * @param rhs Second identity.
 * @return True when any field differs.
 * @throws Nothing.
 */
bool operator!=(const PluginPackageIdentity& lhs,
                const PluginPackageIdentity& rhs) noexcept;

/** @brief Stable fail-closed plugin trust failure category. */
enum class PluginTrustErrorCode : std::uint8_t {
  /** @brief Operator startup trust configuration is absent or malformed. */
  ConfigurationInvalid = 0U,

  /** @brief Manifest structure or canonical encoding is invalid. */
  ManifestInvalid = 1U,

  /** @brief Public key or detached Ed25519 signature is invalid. */
  SignatureInvalid = 2U,

  /** @brief Candidate or copied snapshot is not one valid bounded artifact. */
  ArtifactInvalid = 3U,

  /** @brief No signed entry approves the candidate's kind and content. */
  ArtifactNotApproved = 4U,

  /** @brief Signed runtime package identity differs from the invocation. */
  PackageMismatch = 5U,

  /** @brief The platform cannot map or exec the authorized exact object. */
  ExactObjectUnsupported = 6U,
};

/**
 * @brief Host-owned native plugin trust rejection.
 * @throws std::bad_alloc when retaining the diagnostic fails.
 * @note This exception contains no key, signature, file, or loading authority.
 */
class PluginTrustError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one typed trust rejection.
   * @param code Closed rejection category.
   * @param message Host-owned diagnostic without secret key material.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  PluginTrustError(PluginTrustErrorCode code, std::string message);

  /**
   * @brief Returns the closed rejection category.
   * @return Code supplied at construction.
   * @throws Nothing.
   */
  PluginTrustErrorCode code() const noexcept { return code_; }

 private:
  /** @brief Stable failure category. */
  PluginTrustErrorCode code_;
};

/**
 * @brief Operator-owned file locations for one signed process trust policy.
 * @throws Nothing for moves; path copying can allocate.
 * @note Paths are configuration only. Candidate or IPC data cannot alter them.
 */
struct PluginTrustConfiguration final {
  /** @brief Canonical LF-terminated manifest path. */
  std::filesystem::path manifest_path;

  /** @brief Lowercase-hex detached Ed25519 signature path. */
  std::filesystem::path signature_path;

  /** @brief PEM Ed25519 public-key path. */
  std::filesystem::path public_key_path;
};

/**
 * @brief Move-only retained capability for one authorized exact file object.
 *
 * On Linux the object owns an immutable sealed-memfd snapshot whose digest was
 * confirmed after copying. DSO loading uses `native_load_path()` while the
 * descriptor remains live, and every successful DSO consumer moves the
 * capability into the shared native-library lease returned by
 * `make_authorized_native_library_lease()`. Isolated exec uses
 * `native_descriptor()` directly. Original path spelling is retained only for
 * diagnostics and never re-authorizes or executes replacement bytes.
 * Unsupported platforms cannot construct this authority through
 * `PluginTrustPolicy`.
 *
 * @note Move and destruction must not race on the same object. Independent
 * capabilities own independent descriptors and require no shared lock.
 */
class AuthorizedPluginFile final {
 public:
  /**
   * @brief Transfers one retained exact-file capability.
   * @param other Capability whose descriptor and signed facts are transferred.
   * @throws Nothing.
   * @note `other` becomes inactive; its diagnostic role remains unspecified
   * and must not be used as authority.
   */
  AuthorizedPluginFile(AuthorizedPluginFile&& other) noexcept;

  /**
   * @brief Replaces this capability after closing prior exact-file ownership.
   * @param other Capability whose descriptor and signed facts are transferred.
   * @return Reference to this capability after transfer.
   * @throws Nothing; descriptor close failure is intentionally ignored.
   * @note Self-move is a no-op. Otherwise current ownership is retired before
   * `other` becomes inactive, so exactly one object retains each descriptor.
   */
  AuthorizedPluginFile& operator=(AuthorizedPluginFile&& other) noexcept;

  /**
   * @brief Closes the retained descriptor after native use retires.
   * @throws Nothing; descriptor close failure is intentionally ignored.
   * @note A DSO consumer must move this capability into the same shared lease
   * that owns its native handle. That lease closes the native library before
   * destroying this capability, preventing reuse of a `/proc/self/fd/N` name
   * while the corresponding mapping remains resident.
   */
  ~AuthorizedPluginFile() noexcept;

  /**
   * @brief Prevents duplicating exact-file authorization.
   * @param other Source capability that cannot be copied.
   * @throws Nothing because this overload is deleted.
   */
  AuthorizedPluginFile(const AuthorizedPluginFile& other) = delete;

  /**
   * @brief Prevents copy-assigning exact-file authorization.
   * @param other Source capability that cannot be copied.
   * @return No value because this overload is deleted.
   * @throws Nothing because this overload is deleted.
   */
  AuthorizedPluginFile& operator=(const AuthorizedPluginFile& other) = delete;

  /**
   * @brief Reports whether an opened exact object remains retained.
   * @return True until movement or destruction.
   * @throws Nothing.
   */
  bool active() const noexcept;

  /**
   * @brief Returns the authorized closed artifact role.
   * @return Role copied from the matching signed entry.
   * @throws Nothing.
   */
  PluginArtifactKind kind() const noexcept { return kind_; }

  /**
   * @brief Returns the signed package identity.
   * @return Exact matched manifest identity and generation.
   * @throws Nothing.
   */
  PluginPackageIdentity package_identity() const noexcept { return package_; }

  /**
   * @brief Returns the verified SHA-256 content identity.
   * @return Exact candidate digest matched to the manifest.
   * @throws Nothing.
   */
  PluginContentDigest content_digest() const noexcept { return digest_; }

  /**
   * @brief Returns the absolute original candidate spelling for diagnostics.
   * @return Borrowed path retained by this capability.
   * @throws Nothing.
   */
  const std::filesystem::path& original_path() const noexcept {
    return original_path_;
  }

  /**
   * @brief Returns the platform path that maps the retained exact DSO object.
   * @return `/proc/self/fd/N` for the retained Linux sealed memfd.
   * @throws std::bad_alloc if string construction cannot allocate.
   * @throws PluginTrustError if exact-object mapping is unsupported.
   * @note The returned value is valid only while this capability stays active;
   * its descriptor-number spelling is not an identity after descriptor close.
   */
  std::string native_load_path() const;

  /**
   * @brief Returns the retained immutable POSIX snapshot descriptor.
   * @return Descriptor greater than two, or -1 on non-POSIX platforms.
   * @throws Nothing.
   */
  int native_descriptor() const noexcept;

 private:
  friend class PluginTrustPolicy;

  /**
   * @brief Adopts one already verified platform-native exact file owner.
   * @param original_path Absolute diagnostic path.
   * @param kind Exact signed artifact role.
   * @param package Exact signed package identity.
   * @param digest Exact verified content digest.
   * @param native_owner Immutable private Linux snapshot descriptor.
   * @throws std::bad_alloc when path ownership cannot allocate.
   * @note Called only after Linux sealing and post-copy digest confirmation;
   * this constructor performs no further authorization or filesystem access.
   */
  AuthorizedPluginFile(std::filesystem::path original_path,
                       PluginArtifactKind kind, PluginPackageIdentity package,
                       PluginContentDigest digest, std::intptr_t native_owner);

  /**
   * @brief Closes current native ownership and clears active signed facts.
   * @return Nothing.
   * @throws Nothing; descriptor close failure is intentionally ignored.
   * @note Package, digest, path, and descriptor state become inactive. `kind_`
   * remains only as non-authoritative diagnostic state on moved-from objects.
   */
  void reset() noexcept;

  /** @brief Absolute original spelling retained only for diagnostics. */
  std::filesystem::path original_path_;

  /** @brief Closed signed role. */
  PluginArtifactKind kind_ = PluginArtifactKind::Operation;

  /** @brief Signed package and generation. */
  PluginPackageIdentity package_;

  /** @brief Verified artifact digest. */
  PluginContentDigest digest_;

  /** @brief Immutable private POSIX snapshot descriptor. */
  std::intptr_t native_owner_ = -1;
};

/**
 * @brief Non-throwing platform callback that closes one native DSO handle.
 * @param native_handle Nonnull handle returned by the platform loader.
 * @return Nothing after the close attempt.
 * @throws Nothing; platform close failures are intentionally ignored.
 */
using NativeLibraryCloseFunction = void(void* native_handle) noexcept;

/**
 * @brief Couples one authorized exact object to its native DSO handle.
 *
 * The returned aliasing owner exposes `native_handle` through `get()` while a
 * private shared control block owns both the handle and `authorized_file`.
 * Every callback, transaction, record, or binding that can use native code
 * must share this exact owner.
 *
 * @param native_handle Nonnull handle returned by `dlopen` or `LoadLibrary`.
 * @param authorized_file Active capability used to map that exact handle.
 * @param close_native_library Non-throwing platform handle closer retained by
 * the shared state.
 * @return Shared alias whose final release closes the native handle and only
 * then destroys `authorized_file` and closes its snapshot descriptor.
 * @throws std::invalid_argument if `native_handle` is null or
 * `authorized_file` is inactive. A nonnull handle is still closed.
 * @throws std::bad_alloc if the shared control block cannot allocate; the
 * handle is closed before the capability is destroyed on this path as well.
 * @note Construction transfers capability ownership. Copies are thread-safe as
 * ordinary `shared_ptr` owners, and final release may close the DSO on any
 * releasing thread. `close_native_library` must have static lifetime. Callers
 * must still obey platform and consumer synchronization rules for symbol use
 * and final native unload.
 */
std::shared_ptr<void> make_authorized_native_library_lease(
    void* native_handle, AuthorizedPluginFile authorized_file,
    NativeLibraryCloseFunction& close_native_library);

/**
 * @brief Immutable verified native plugin trust manifest.
 *
 * Loading validates bounded canonical bytes, Ed25519 signature, closed kinds,
 * sorted unique identities, unique content-role mappings, package ids,
 * generations, and digests. On Linux, candidate authorization opens and hashes
 * a stable regular file, matches it, copies it into a private immutable
 * snapshot, and confirms the signed digest on that snapshot before returning
 * authority.
 * Darwin, Windows, and other platforms fail closed before candidate path
 * access because no supported unprivileged immutable exact-object primitive is
 * available there.
 */
class PluginTrustPolicy final {
 public:
  /**
   * @brief Loads and verifies one operator-controlled signed policy.
   * @param configuration Exact manifest, signature, and public-key paths.
   * @return Immutable shareable verified policy.
   * @throws PluginTrustError for any configuration/signature/manifest fault.
   * @throws std::bad_alloc when bounded retained state cannot allocate.
   */
  static PluginTrustPolicy load(const PluginTrustConfiguration& configuration);

  /**
   * @brief Opens, hashes, and authorizes one exact candidate object.
   * @param candidate Candidate path whose final component must not be symlink.
   * @param kind Required signed role.
   * @param expected_package Required runtime package identity, or no package
   * constraint for operation/policy loading.
   * @return Move-only capability retaining a private immutable snapshot.
   * @throws PluginTrustError before native code execution for every rejection.
   * @throws std::bad_alloc when bounded path or diagnostic state cannot grow.
   * @throws std::filesystem::filesystem_error when Linux absolute-path
   * normalization cannot inspect the process filesystem context.
   * @note An isolated-runtime constructor may omit `expected_package` to retain
   * the signed package identity for later invocation comparison. Operation and
   * policy roles reject a supplied package constraint so role checks cannot be
   * confused. Linux supports sealed descriptor snapshots for all roles.
   * Darwin, Windows, and other platforms fail closed with
   * `ExactObjectUnsupported` before candidate path access for every role.
   */
  AuthorizedPluginFile authorize(const std::filesystem::path& candidate,
                                 PluginArtifactKind kind,
                                 std::optional<PluginPackageIdentity>
                                     expected_package = std::nullopt) const;

  /**
   * @brief Returns the canonical diagnostic trust-root id.
   * @return Borrowed immutable id from the verified manifest.
   * @throws Nothing.
   */
  const std::string& trust_root_id() const noexcept;

 private:
  /** @brief Immutable verified implementation state. */
  struct Impl;

  /** @brief Adopts one verified immutable implementation. */
  explicit PluginTrustPolicy(
      std::shared_ptr<const Impl> implementation) noexcept;

  /** @brief Shared immutable policy state. */
  std::shared_ptr<const Impl> implementation_;
};

/**
 * @brief Returns the process-immutable environment-selected trust policy.
 * @return Verified policy from the first authorization attempt.
 * @throws PluginTrustError with the cached first failure for process lifetime.
 * @throws std::bad_alloc by rethrowing the cached first allocation failure from
 * environment/path retention or `PluginTrustPolicy::load` unchanged.
 * @throws Any other exception emitted by the first `PluginTrustPolicy::load`
 * unchanged on every call through the cached `std::exception_ptr`.
 * @note Reads exactly `PHOTOSPIDER_PLUGIN_TRUST_MANIFEST`,
 * `PHOTOSPIDER_PLUGIN_TRUST_SIGNATURE`, and
 * `PHOTOSPIDER_PLUGIN_TRUST_PUBLIC_KEY` once. Later environment changes have no
 * effect, including after a fail-closed first result.
 */
const PluginTrustPolicy& process_plugin_trust_policy();

/**
 * @brief Authorizes one candidate through the immutable process trust policy.
 * @param candidate Candidate native artifact path.
 * @param kind Required signed role.
 * @param expected_package Required exact runtime package identity when used.
 * @return Move-only retained exact-file capability.
 * @throws PluginTrustError from immutable policy initialization or admission.
 * @throws std::bad_alloc from cached policy initialization or candidate
 * authorization unchanged.
 * @throws std::filesystem::filesystem_error from Linux candidate path
 * normalization unchanged.
 * @throws Any other cached initialization exception from
 * `process_plugin_trust_policy()` unchanged.
 */
AuthorizedPluginFile authorize_process_plugin(
    const std::filesystem::path& candidate, PluginArtifactKind kind,
    std::optional<PluginPackageIdentity> expected_package = std::nullopt);

}  // namespace ps
