#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef PS_VALUE_IDENTITY_RUNTIME_PATH
#error "PS_VALUE_IDENTITY_RUNTIME_PATH must identify operation_runtime"
#endif

#ifndef PS_VALUE_IDENTITY_DSO_A_PATH
#error "PS_VALUE_IDENTITY_DSO_A_PATH must identify the first Value fixture"
#endif

#ifndef PS_VALUE_IDENTITY_DSO_B_PATH
#error "PS_VALUE_IDENTITY_DSO_B_PATH must identify the second Value fixture"
#endif

namespace {

/**
 * @brief Fixed-width result minted inside one Value-using fixture DSO.
 *
 * @note Both values are opaque diagnostics; the test compares only equality
 *       and never treats either token as persistent identity.
 */
struct MintedIdentities final {
  /** @brief Allocation token returned by the fixture. */
  std::uint64_t allocation = 0U;

  /** @brief Value revision token returned by the fixture. */
  std::uint64_t revision = 0U;
};

/**
 * @brief Exact fixed-width fixture callback resolved from each DSO.
 *
 * @param allocation Output allocation token.
 * @param revision Output Value revision token.
 * @return Fixture status, where zero means success.
 * @throws Nothing across the C boundary.
 */
using MintIdentities = int (*)(std::uint64_t* allocation,
                               std::uint64_t* revision) noexcept;

/**
 * @brief Owns one platform-native dynamic-library reference.
 *
 * @throws std::runtime_error when the library or a required symbol cannot be
 *         loaded.
 * @note Instances are noncopyable so every native reference closes exactly
 *       once. The runtime owner is constructed before fixture owners and
 *       therefore closes after both fixtures.
 */
class NativeLibrary final {
 public:
  /**
   * @brief Opens one exact library path eagerly and locally.
   *
   * @param path Absolute or loader-valid path supplied by CMake.
   * @throws std::runtime_error when the platform loader rejects the path.
   */
  explicit NativeLibrary(const std::string& path) {
#if defined(_WIN32)
    handle_ = LoadLibraryA(path.c_str());
    if (handle_ == nullptr) {
      throw std::runtime_error("Failed to load Value identity DSO: " + path);
    }
#else
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
      const char* detail = dlerror();
      throw std::runtime_error(
          "Failed to load Value identity DSO: " + path + ": " +
          (detail != nullptr ? detail : "unknown loader error"));
    }
#endif
  }

  /** @brief Copy construction is forbidden for native ownership. */
  NativeLibrary(const NativeLibrary&) = delete;

  /** @brief Copy assignment is forbidden for native ownership. */
  NativeLibrary& operator=(const NativeLibrary&) = delete;

  /**
   * @brief Releases this object's native loader reference.
   *
   * @throws Nothing; close failures are intentionally non-actionable in test
   *         teardown.
   */
  ~NativeLibrary() noexcept {
#if defined(_WIN32)
    (void)FreeLibrary(handle_);
#else
    (void)dlclose(handle_);
#endif
  }

  /**
   * @brief Resolves one mandatory C fixture symbol.
   *
   * @tparam Function Exact function-pointer type expected by the test.
   * @param name Exact exported symbol name.
   * @return Typed callable address valid while this library remains loaded.
   * @throws std::runtime_error when the symbol is absent.
   * @note POSIX specifies the `dlsym` conversion for callable symbols; Windows
   *       supplies the corresponding `FARPROC`.
   */
  template <typename Function>
  Function resolve(const char* name) const {
#if defined(_WIN32)
    FARPROC raw = GetProcAddress(handle_, name);
    if (raw == nullptr) {
      throw std::runtime_error(std::string("Missing Value fixture symbol: ") +
                               name);
    }
    return reinterpret_cast<Function>(raw);
#else
    (void)dlerror();
    void* raw = dlsym(handle_, name);
    const char* detail = dlerror();
    if (detail != nullptr || raw == nullptr) {
      throw std::runtime_error(std::string("Missing Value fixture symbol: ") +
                               name);
    }
    return reinterpret_cast<Function>(raw);
#endif
  }

 private:
#if defined(_WIN32)
  /** @brief Owned Windows module reference. */
  HMODULE handle_ = nullptr;
#else
  /** @brief Owned POSIX dynamic-library reference. */
  void* handle_ = nullptr;
#endif
};

/**
 * @brief Invokes one fixture's identity minting seam.
 *
 * @param mint Resolved callback owned by a live NativeLibrary.
 * @return Nonzero allocation/revision pair.
 * @throws Nothing; GoogleTest assertions report callback failure.
 * @note A failed callback leaves zero values so later nonzero assertions also
 *       preserve useful diagnostics.
 */
MintedIdentities mint_from(MintIdentities mint) {
  MintedIdentities result;
  EXPECT_EQ(mint(&result.allocation, &result.revision), 0);
  EXPECT_NE(result.allocation, 0U);
  EXPECT_NE(result.revision, 0U);
  return result;
}

/**
 * @brief Proves two independent Value-using DSOs share one minting authority.
 *
 * @throws std::runtime_error when a required library or symbol cannot load.
 * @note The runtime is explicitly preloaded and remains alive until both
 *       fixture libraries have closed.
 */
TEST(ValueIdentityAcrossDsos, MintingAuthorityIsProcessWide) {
  const NativeLibrary runtime(PS_VALUE_IDENTITY_RUNTIME_PATH);
  const NativeLibrary first_library(PS_VALUE_IDENTITY_DSO_A_PATH);
  const NativeLibrary second_library(PS_VALUE_IDENTITY_DSO_B_PATH);
  constexpr const char* kMintSymbol =
      "photospider_test_mint_runtime_identities";

  const MintedIdentities first =
      mint_from(first_library.resolve<MintIdentities>(kMintSymbol));
  const MintedIdentities second =
      mint_from(second_library.resolve<MintIdentities>(kMintSymbol));

  EXPECT_NE(first.allocation, second.allocation);
  EXPECT_NE(first.revision, second.revision);
}

}  // namespace
