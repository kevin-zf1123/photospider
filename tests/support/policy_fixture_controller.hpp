#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "fixtures/policy_plugins/policy_fixture_control.h"

namespace ps::test {

/**
 * @brief Owns one native handle used to control the repository policy fixture.
 *
 * Tests use this helper only to select fixture behavior and observe callback
 * counts. Production loading still happens independently through
 * `PolicyRegistry`, so the helper never substitutes for the loader under test.
 *
 * @throws std::runtime_error when the native DSO or a control symbol cannot be
 * opened.
 * @note The controller must outlive every installed fixture hook. Instances
 * are intentionally noncopyable because each owns one native loader reference.
 */
class PolicyFixtureController final {
 public:
  /**
   * @brief Opens the fixture DSO and resolves every test-control export.
   * @param path Native path to the built repository fixture.
   * @throws std::runtime_error when opening or symbol resolution fails.
   */
  explicit PolicyFixtureController(const std::string& path) {
#if defined(_WIN32)
    handle_ = LoadLibraryA(path.c_str());
    if (handle_ == nullptr) {
      throw std::runtime_error("Failed to open policy fixture: " + path);
    }
#else
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
      const char* detail = dlerror();
      throw std::runtime_error(
          "Failed to open policy fixture: " + path + ": " +
          (detail != nullptr ? detail : "unknown loader error"));
    }
#endif
    try {
      reset_ = resolve<decltype(reset_)>("ps_policy_fixture_reset");
      set_api_mode_ =
          resolve<decltype(set_api_mode_)>("ps_policy_fixture_set_api_mode");
      set_metadata_mode_ = resolve<decltype(set_metadata_mode_)>(
          "ps_policy_fixture_set_metadata_mode");
      set_create_mode_ = resolve<decltype(set_create_mode_)>(
          "ps_policy_fixture_set_create_mode");
      set_select_mode_ = resolve<decltype(set_select_mode_)>(
          "ps_policy_fixture_set_select_mode");
      set_destroy_mode_ = resolve<decltype(set_destroy_mode_)>(
          "ps_policy_fixture_set_destroy_mode");
      set_hook_ = resolve<decltype(set_hook_)>("ps_policy_fixture_set_hook");
      create_count_ =
          resolve<decltype(create_count_)>("ps_policy_fixture_create_count");
      select_count_ =
          resolve<decltype(select_count_)>("ps_policy_fixture_select_count");
      destroy_count_ =
          resolve<decltype(destroy_count_)>("ps_policy_fixture_destroy_count");
    } catch (...) {
      close();
      throw;
    }
  }

  /**
   * @brief Releases the controller's native loader reference.
   * @throws Nothing; platform close status is intentionally ignored.
   */
  ~PolicyFixtureController() noexcept { close(); }

  /** @brief Prevents duplicate native-handle ownership. */
  PolicyFixtureController(const PolicyFixtureController&) = delete;

  /** @brief Prevents assigning duplicate native-handle ownership. */
  PolicyFixtureController& operator=(const PolicyFixtureController&) = delete;

  /**
   * @brief Restores every fixture mode, hook, and counter to defaults.
   * @return Nothing.
   * @throws Nothing.
   */
  void reset() const noexcept { reset_(); }

  /**
   * @brief Selects the fixture API-table behavior.
   * @param mode Exact fixture API mode.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_api_mode(ps_policy_fixture_api_mode mode) const noexcept {
    set_api_mode_(mode);
  }

  /**
   * @brief Selects the fixture metadata behavior.
   * @param mode Exact fixture metadata mode.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_metadata_mode(ps_policy_fixture_metadata_mode mode) const noexcept {
    set_metadata_mode_(mode);
  }

  /**
   * @brief Selects the fixture context-create behavior.
   * @param mode Exact fixture create mode.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_create_mode(ps_policy_fixture_create_mode mode) const noexcept {
    set_create_mode_(mode);
  }

  /**
   * @brief Selects the fixture decision behavior.
   * @param mode Exact fixture decision mode.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_select_mode(ps_policy_fixture_select_mode mode) const noexcept {
    set_select_mode_(mode);
  }

  /**
   * @brief Selects the fixture context-destroy behavior.
   * @param mode Exact fixture destroy mode.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_destroy_mode(ps_policy_fixture_destroy_mode mode) const noexcept {
    set_destroy_mode_(mode);
  }

  /**
   * @brief Installs one test-owned hook for controlled callback entry.
   * @param hook Hook function, or null to clear it.
   * @param context Opaque test-owned context passed unchanged.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_hook(ps_policy_fixture_hook_v1 hook, void* context) const noexcept {
    set_hook_(hook, context);
  }

  /**
   * @brief Returns fixture create-callback entries since reset.
   * @return Exact create callback count.
   * @throws Nothing.
   */
  std::uint32_t create_count() const noexcept { return create_count_(); }

  /**
   * @brief Returns fixture select-callback entries since reset.
   * @return Exact select callback count.
   * @throws Nothing.
   */
  std::uint32_t select_count() const noexcept { return select_count_(); }

  /**
   * @brief Returns fixture destroy-callback entries since reset.
   * @return Exact destroy callback count.
   * @throws Nothing.
   */
  std::uint32_t destroy_count() const noexcept { return destroy_count_(); }

 private:
#if defined(_WIN32)
  /** @brief Platform-native DSO handle type. */
  using NativeHandle = HMODULE;
#else
  /** @brief Platform-native DSO handle type. */
  using NativeHandle = void*;
#endif

  /**
   * @brief Resolves one mandatory fixture-control symbol.
   * @tparam Function Exact function-pointer type.
   * @param name Exact exported C symbol.
   * @return Typed callable address while `handle_` remains open.
   * @throws std::runtime_error when the symbol is absent.
   */
  template <typename Function>
  Function resolve(const char* name) const {
#if defined(_WIN32)
    FARPROC raw = GetProcAddress(handle_, name);
    if (raw == nullptr) {
      throw std::runtime_error(std::string("Missing policy fixture symbol: ") +
                               name);
    }
    return reinterpret_cast<Function>(raw);
#else
    (void)dlerror();
    void* raw = dlsym(handle_, name);
    const char* detail = dlerror();
    if (detail != nullptr || raw == nullptr) {
      throw std::runtime_error(std::string("Missing policy fixture symbol: ") +
                               name);
    }
    return reinterpret_cast<Function>(raw);
#endif
  }

  /**
   * @brief Releases the native handle if open.
   * @return Nothing.
   * @throws Nothing.
   */
  void close() noexcept {
    if (handle_ == nullptr) {
      return;
    }
#if defined(_WIN32)
    (void)FreeLibrary(handle_);
#else
    (void)dlclose(handle_);
#endif
    handle_ = nullptr;
  }

  /** @brief Owned native fixture handle. */
  NativeHandle handle_ = nullptr;

  /** @brief Resolved fixture reset callback. */
  decltype(&ps_policy_fixture_reset) reset_ = nullptr;

  /** @brief Resolved API-mode setter. */
  decltype(&ps_policy_fixture_set_api_mode) set_api_mode_ = nullptr;

  /** @brief Resolved metadata-mode setter. */
  decltype(&ps_policy_fixture_set_metadata_mode) set_metadata_mode_ = nullptr;

  /** @brief Resolved create-mode setter. */
  decltype(&ps_policy_fixture_set_create_mode) set_create_mode_ = nullptr;

  /** @brief Resolved select-mode setter. */
  decltype(&ps_policy_fixture_set_select_mode) set_select_mode_ = nullptr;

  /** @brief Resolved destroy-mode setter. */
  decltype(&ps_policy_fixture_set_destroy_mode) set_destroy_mode_ = nullptr;

  /** @brief Resolved test-hook setter. */
  decltype(&ps_policy_fixture_set_hook) set_hook_ = nullptr;

  /** @brief Resolved create counter. */
  decltype(&ps_policy_fixture_create_count) create_count_ = nullptr;

  /** @brief Resolved select counter. */
  decltype(&ps_policy_fixture_select_count) select_count_ = nullptr;

  /** @brief Resolved destroy counter. */
  decltype(&ps_policy_fixture_destroy_count) destroy_count_ = nullptr;
};

}  // namespace ps::test
