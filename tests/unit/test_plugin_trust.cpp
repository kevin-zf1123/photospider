#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#include <fcntl.h>
#endif

#include "photospider/policy/policy_plugin_api.h"
#include "plugin/plugin_manager.hpp"  // NOLINT(build/include_subdir)
#include "plugin/plugin_trust.hpp"

#ifndef PS_TEST_PLUGIN_TRUST_PRIVATE_KEY
#error "PS_TEST_PLUGIN_TRUST_PRIVATE_KEY must name the test signing key"
#endif
#ifndef PS_TEST_SIGNED_POLICY_PLUGIN
#error "PS_TEST_SIGNED_POLICY_PLUGIN must name the signed policy fixture"
#endif

namespace ps {
namespace {

static_assert(!std::is_copy_constructible<AuthorizedPluginFile>::value,
              "authorized plugin file capabilities must not be copied");
static_assert(std::is_nothrow_move_constructible<AuthorizedPluginFile>::value,
              "authorized file movement must retain exact-object authority");

/**
 * @brief Returns the configured valid signed test trust bundle.
 * @return Complete source-private file configuration.
 * @throws Nothing.
 */
PluginTrustConfiguration valid_configuration() {
  return PluginTrustConfiguration{PS_TEST_PLUGIN_TRUST_MANIFEST,
                                  PS_TEST_PLUGIN_TRUST_SIGNATURE,
                                  PS_TEST_PLUGIN_TRUST_PUBLIC_KEY};
}

/** @brief Unique OpenSSL BIO owner used by manifest-signing tests. */
using UniqueTestBio = std::unique_ptr<BIO, decltype(&BIO_free)>;

/** @brief Unique OpenSSL key owner used by manifest-signing tests. */
using UniqueTestEvpKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

/** @brief Unique OpenSSL signing-context owner used by manifest tests. */
using TestEvpContext = std::unique_ptr<
    EVP_MD_CTX,                   // NOLINT(whitespace/indent_namespace)
    decltype(&EVP_MD_CTX_free)>;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Signs exact canonical manifest bytes with the repository test key.
 * @param manifest Exact bytes to sign without transformation.
 * @return Lowercase hexadecimal Ed25519 signature with one trailing LF.
 * @throws std::runtime_error when key loading or single-shot signing fails.
 * @throws std::bad_alloc when retained signature storage cannot allocate.
 * @note This helper is test-only and never changes production trust state.
 */
std::string sign_test_manifest(std::string_view manifest) {
  UniqueTestBio bio(BIO_new_file(PS_TEST_PLUGIN_TRUST_PRIVATE_KEY, "r"),
                    &BIO_free);
  if (!bio) {
    throw std::runtime_error("cannot open plugin trust test private key");
  }
  UniqueTestEvpKey key(
      PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
      &EVP_PKEY_free);
  if (!key) {
    throw std::runtime_error("cannot parse plugin trust test private key");
  }
  TestEvpContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr,
                                     key.get()) != 1) {
    throw std::runtime_error("cannot initialize test manifest signing");
  }
  std::size_t signature_size = 0U;
  const auto* bytes = reinterpret_cast<const unsigned char*>(manifest.data());
  if (EVP_DigestSign(context.get(), nullptr, &signature_size, bytes,
                     manifest.size()) != 1 ||
      signature_size != 64U) {
    throw std::runtime_error("cannot size Ed25519 test signature");
  }
  std::vector<unsigned char> signature(signature_size);
  if (EVP_DigestSign(context.get(), signature.data(), &signature_size, bytes,
                     manifest.size()) != 1 ||
      signature_size != signature.size()) {
    throw std::runtime_error("cannot create Ed25519 test signature");
  }
  constexpr char kLowerHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(signature.size() * 2U + 1U);
  for (unsigned char byte : signature) {
    result.push_back(kLowerHex[byte >> 4U]);
    result.push_back(kLowerHex[byte & 0x0fU]);
  }
  result.push_back('\n');
  return result;
}

/**
 * @brief Hashes all bytes from one test artifact with SHA-256.
 * @param path Exact source path to read once.
 * @return SHA-256 content identity for the observed bytes.
 * @throws std::runtime_error when the file or digest operation fails.
 * @throws std::bad_alloc when file storage cannot allocate.
 */
PluginContentDigest hash_test_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("cannot open plugin trust mutation source");
  }
  const std::streampos end = stream.tellg();
  if (end < 0) {
    throw std::runtime_error("cannot size plugin trust mutation source");
  }
  std::vector<unsigned char> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
  }
  if (!stream) {
    throw std::runtime_error("cannot read plugin trust mutation source");
  }
  PluginContentDigest digest{};
  unsigned int digest_size = 0U;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digest_size,
                 EVP_sha256(), nullptr) != 1 ||
      digest_size != digest.size()) {
    throw std::runtime_error("cannot hash plugin trust mutation source");
  }
  return digest;
}

#if defined(__APPLE__) || defined(__linux__)
/**
 * @brief Replaces all bytes of one already-open writable test artifact.
 * @param descriptor Writable descriptor opened before authorization.
 * @return Nothing after truncation, replacement, and durable flush.
 * @throws std::system_error when any exact replacement operation fails.
 */
void overwrite_open_test_artifact(int descriptor) {
  if (::ftruncate(descriptor, 0) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "truncate authorized test artifact");
  }
  constexpr std::string_view kReplacement = "not a native plugin\n";
  std::size_t offset = 0U;
  while (offset < kReplacement.size()) {
    const ssize_t written =
        ::pwrite(descriptor, kReplacement.data() + offset,
                 kReplacement.size() - offset, static_cast<off_t>(offset));
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::system_error(errno == 0 ? EIO : errno, std::generic_category(),
                              "replace authorized test artifact");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "flush authorized test artifact replacement");
  }
}

/**
 * @brief Owns one test-loaded policy DSO and exposes its ABI behavior.
 * @throws std::runtime_error when native loading or symbol lookup fails.
 * @note The retained authorized capability must outlive this loader handle.
 */
class ScopedPolicyLibrary final {
 public:
  /**
   * @brief Loads one authorized policy descriptor path.
   * @param path Descriptor-backed native load path.
   * @throws std::runtime_error when the native loader rejects the object.
   */
  explicit ScopedPolicyLibrary(const std::string& path) {
    handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
      const char* detail = ::dlerror();
      throw std::runtime_error(std::string("cannot load authorized policy: ") +
                               (detail != nullptr ? detail : "unknown error"));
    }
  }

  /** @brief Releases the native loader reference without throwing. */
  ~ScopedPolicyLibrary() noexcept {
    if (handle_ != nullptr) {
      static_cast<void>(::dlclose(handle_));
    }
  }

  /** @brief Prevents duplicate native loader ownership. */
  ScopedPolicyLibrary(const ScopedPolicyLibrary&) = delete;
  /** @brief Prevents duplicate native loader assignment. */
  ScopedPolicyLibrary& operator=(const ScopedPolicyLibrary&) = delete;

  /**
   * @brief Resolves and invokes the public policy ABI exports.
   * @return True only when the fixture returns a complete compatible table.
   * @throws std::runtime_error when either mandatory export is absent.
   */
  bool exposes_compatible_policy_behavior() const {
    using AbiVersionFunction = decltype(&ps_policy_plugin_get_abi_version);
    using ApiFunction = decltype(&ps_policy_plugin_get_api_v1);
    AbiVersionFunction abi_version =
        resolve<AbiVersionFunction>("ps_policy_plugin_get_abi_version");
    ApiFunction get_api = resolve<ApiFunction>("ps_policy_plugin_get_api_v1");
    ps_policy_plugin_api_v1 api{};
    return abi_version() == PS_POLICY_PLUGIN_ABI_VERSION &&
           get_api(&api) == PS_POLICY_STATUS_OK &&
           api.struct_size == sizeof(api) &&
           api.struct_kind == PS_POLICY_STRUCT_PLUGIN_API &&
           api.abi_version == PS_POLICY_PLUGIN_ABI_VERSION &&
           api.type_count > 0U && api.get_metadata != nullptr &&
           api.create != nullptr && api.select != nullptr &&
           api.destroy != nullptr;
  }

 private:
  /**
   * @brief Resolves one mandatory export from the retained native handle.
   * @tparam Function Exact public function-pointer type.
   * @param name Exact C export name.
   * @return Typed callable export address.
   * @throws std::runtime_error when native symbol resolution fails.
   */
  template <typename Function>
  Function resolve(const char* name) const {
    static_cast<void>(::dlerror());
    void* raw = ::dlsym(handle_, name);
    const char* detail = ::dlerror();
    if (raw == nullptr || detail != nullptr) {
      throw std::runtime_error(std::string("cannot resolve policy export: ") +
                               name);
    }
    return reinterpret_cast<Function>(raw);
  }

  /** @brief Sole owned native loader handle. */
  void* handle_ = nullptr;
};
#endif

/**
 * @brief Selects or clears the untrusted-initializer sentinel environment.
 * @param path Nonempty sentinel path, or empty to clear the variable.
 * @return Nothing after the platform environment is updated.
 * @throws std::runtime_error when the environment cannot be changed.
 */
void set_initializer_sentinel_environment(const std::filesystem::path& path) {
#if defined(_WIN32)
  const int result = _putenv_s("PS_TEST_UNTRUSTED_PLUGIN_INITIALIZER_SENTINEL",
                               path.string().c_str());
#else
  const int result =
      path.empty() ? ::unsetenv("PS_TEST_UNTRUSTED_PLUGIN_INITIALIZER_SENTINEL")
                   : ::setenv("PS_TEST_UNTRUSTED_PLUGIN_INITIALIZER_SENTINEL",
                              path.c_str(), 1);
#endif
  if (result != 0) {
    throw std::runtime_error(
        "cannot configure untrusted initializer sentinel environment");
  }
}

/**
 * @brief Proves a valid signed operation becomes a private snapshot capability.
 * @throws Nothing when signature, kind, digest, and descriptor path agree.
 */
TEST(PluginTrustPolicy, AuthorizesSignedOperationAsPrivateSnapshot) {
  const PluginTrustPolicy policy =
      PluginTrustPolicy::load(valid_configuration());
  AuthorizedPluginFile authorized = policy.authorize(
      PS_TEST_SIGNED_OPERATION_PLUGIN, PluginArtifactKind::Operation);

  EXPECT_TRUE(authorized.active());
  EXPECT_EQ(authorized.kind(), PluginArtifactKind::Operation);
  EXPECT_EQ(authorized.original_path(),
            std::filesystem::absolute(PS_TEST_SIGNED_OPERATION_PLUGIN));
  EXPECT_FALSE(authorized.native_load_path().empty());
  EXPECT_GT(authorized.native_descriptor(), 2);
}

/**
 * @brief Proves bad signatures and signed entries of the wrong kind fail
 * closed.
 * @throws Nothing when typed trust errors are returned before native mapping.
 */
TEST(PluginTrustPolicy, RejectsBadSignatureAndWrongKind) {
  PluginTrustConfiguration bad = valid_configuration();
  bad.signature_path = PS_TEST_PLUGIN_TRUST_BAD_SIGNATURE;
  try {
    static_cast<void>(PluginTrustPolicy::load(bad));
    FAIL() << "a changed detached signature must reject the manifest";
  } catch (const PluginTrustError& error) {
    EXPECT_EQ(error.code(), PluginTrustErrorCode::SignatureInvalid);
  }

  const PluginTrustPolicy policy =
      PluginTrustPolicy::load(valid_configuration());
  try {
    static_cast<void>(policy.authorize(PS_TEST_WRONG_KIND_PLUGIN,
                                       PluginArtifactKind::Policy));
    FAIL() << "an operation trust entry must not authorize policy code";
  } catch (const PluginTrustError& error) {
    EXPECT_EQ(error.code(), PluginTrustErrorCode::ArtifactNotApproved);
  }
}

/**
 * @brief Rejects an absent manifest entry and runtime package mismatch.
 * @throws Nothing when both typed trust decisions fail before native use.
 */
TEST(PluginTrustPolicy, RejectsAbsentEntryAndRuntimePackageMismatch) {
  const PluginTrustPolicy policy =
      PluginTrustPolicy::load(valid_configuration());
  try {
    static_cast<void>(policy.authorize(PS_TEST_UNTRUSTED_INITIALIZER_PLUGIN,
                                       PluginArtifactKind::Operation));
    FAIL() << "an absent digest must not receive native loading authority";
  } catch (const PluginTrustError& error) {
    EXPECT_EQ(error.code(), PluginTrustErrorCode::ArtifactNotApproved);
  }

  PluginPackageIdentity wrong_package;
  wrong_package.package_id.fill(0xA5U);
  wrong_package.generation = 5U;
  try {
    static_cast<void>(policy.authorize(PS_TEST_SIGNED_RUNTIME_PLUGIN,
                                       PluginArtifactKind::IsolatedRuntime,
                                       wrong_package));
    FAIL() << "a signed runtime must still reject a different package key";
  } catch (const PluginTrustError& error) {
#if defined(__APPLE__)
    EXPECT_EQ(error.code(), PluginTrustErrorCode::ExactObjectUnsupported);
#else
    EXPECT_EQ(error.code(), PluginTrustErrorCode::PackageMismatch);
#endif
  }
}

/**
 * @brief Rejects one content-role identity mapped to two package generations.
 * @throws Filesystem and test-only Ed25519 signing failures unchanged.
 */
TEST(PluginTrustPolicy, RejectsDuplicateContentRoleAcrossSignedGenerations) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      "photospider-plugin-trust-duplicate-content-role-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const std::filesystem::path manifest = root / "manifest.txt";
  const std::filesystem::path signature = root / "signature.hex";
  const std::string digest(64U, 'a');
  const std::string manifest_text =
      "photospider-plugin-trust-manifest-v1\n"
      "trust-root duplicate-content-role-test\n"
      "operation 11111111111111111111111111111111 1 " +
      digest + "\n" + "operation 22222222222222222222222222222222 2 " + digest +
      "\n";
  {
    std::ofstream output(manifest, std::ios::binary);
    output << manifest_text;
  }
  {
    std::ofstream output(signature, std::ios::binary);
    output << sign_test_manifest(manifest_text);
  }

  try {
    static_cast<void>(PluginTrustPolicy::load(PluginTrustConfiguration{
        manifest, signature, PS_TEST_PLUGIN_TRUST_PUBLIC_KEY}));
    FAIL() << "one content-role identity mapped to two generations";
  } catch (const PluginTrustError& error) {
    EXPECT_EQ(error.code(), PluginTrustErrorCode::ManifestInvalid);
  }
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves an absent operation entry cannot run a native initializer.
 * @throws Filesystem, environment, and loader setup failures unchanged.
 */
TEST(PluginTrustPolicy, RejectsBeforeUntrustedOperationInitializerExecutes) {
  const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     "photospider-untrusted-initializer-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const std::filesystem::path sentinel = root / "initializer-ran";
  set_initializer_sentinel_environment(sentinel);

  PluginManager& manager = PluginManager::process_instance();
  static_cast<void>(manager.unload_all_plugins());
  const std::size_t handles_before = manager.loaded_plugin_count();
  PluginLoadResult result;
  try {
    result = manager.load_from_dirs_report(
        {std::filesystem::path(PS_TEST_UNTRUSTED_INITIALIZER_PLUGIN)
             .parent_path()
             .string()});
    set_initializer_sentinel_environment({});
  } catch (...) {
    set_initializer_sentinel_environment({});
    throw;
  }

  EXPECT_EQ(result.attempted, 1);
  EXPECT_EQ(result.loaded, 0);
  ASSERT_EQ(result.errors.size(), 1U);
  EXPECT_FALSE(std::filesystem::exists(sentinel));
  EXPECT_EQ(manager.loaded_plugin_count(), handles_before);
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves preopened source mutation cannot alter authorized policy code.
 * @throws Filesystem, hashing, and native policy loading failures unchanged.
 */
TEST(PluginTrustPolicy, RetainsPrivatePolicySnapshotAfterSourceMutation) {
#if defined(__APPLE__) || defined(__linux__)
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      "photospider-plugin-trust-source-mutation-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const std::filesystem::path candidate = root / "candidate-plugin";
  std::filesystem::copy_file(PS_TEST_SIGNED_POLICY_PLUGIN, candidate);
  const int writable_descriptor = ::open(candidate.c_str(), O_RDWR);
  ASSERT_GE(writable_descriptor, 0);

  const PluginTrustPolicy policy =
      PluginTrustPolicy::load(valid_configuration());
  AuthorizedPluginFile authorized =
      policy.authorize(candidate, PluginArtifactKind::Policy);
  overwrite_open_test_artifact(writable_descriptor);
  ASSERT_EQ(::close(writable_descriptor), 0);

  EXPECT_NE(hash_test_file(candidate), authorized.content_digest());
  EXPECT_TRUE(authorized.active());
  ScopedPolicyLibrary loaded(authorized.native_load_path());
  EXPECT_TRUE(loaded.exposes_compatible_policy_behavior());
  std::filesystem::remove_all(root);
#else
  GTEST_SKIP() << "descriptor-backed DSO snapshots are POSIX-only";
#endif
}

/**
 * @brief Proves a final symlink is rejected before digest-based admission.
 * @throws std::filesystem::filesystem_error only if test cleanup itself fails.
 */
TEST(PluginTrustPolicy, RejectsFinalSymlink) {
  const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     "photospider-plugin-trust-symlink-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const std::filesystem::path link = root / "candidate";
  std::filesystem::create_symlink(PS_TEST_SIGNED_OPERATION_PLUGIN, link);

  const PluginTrustPolicy policy =
      PluginTrustPolicy::load(valid_configuration());
  EXPECT_THROW(policy.authorize(link, PluginArtifactKind::Operation),
               PluginTrustError);
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace ps
