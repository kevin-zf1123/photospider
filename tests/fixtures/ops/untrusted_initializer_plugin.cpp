/**
 * @file untrusted_initializer_plugin.cpp
 * @brief Emits a sentinel if an unapproved operation DSO is ever initialized.
 */

#include <cstdint>
#include <cstdlib>
#include <fstream>

namespace {

/**
 * @brief Writes the test-selected sentinel during native DSO initialization.
 * @return Nothing.
 * @throws Nothing; all I/O failures are intentionally contained.
 * @note Production trust tests require this function never to execute. The
 * environment value carries no loading authority; it only selects an
 * assertion artifact after a hypothetical loader bypass.
 */
void emit_untrusted_initializer_sentinel() noexcept {
  try {
    const char* const path =
        std::getenv("PS_TEST_UNTRUSTED_PLUGIN_INITIALIZER_SENTINEL");
    if (path != nullptr && *path != '\0') {
      std::ofstream sentinel(path, std::ios::binary | std::ios::trunc);
      sentinel << "untrusted initializer executed\n";
    }
  } catch (...) {
    // A test-only initializer must never disturb the process loader itself.
  }
}

#if defined(_WIN32)
/**
 * @brief Windows loader callback forwarding process attachment to the probe.
 * @param module DSO module handle, unused.
 * @param reason Loader event selector.
 * @param reserved Loader-owned context, unused.
 * @return True so only production trust decides admission.
 * @throws Nothing.
 */
extern "C" int __stdcall DllMain(void* module, std::uint32_t reason,
                                 void* reserved) noexcept {
  static_cast<void>(module);
  static_cast<void>(reserved);
  constexpr std::uint32_t kProcessAttach = 1U;
  if (reason == kProcessAttach) {
    emit_untrusted_initializer_sentinel();
  }
  return 1;
}
#else
/**
 * @brief Forwards POSIX DSO initialization to the sentinel probe.
 * @return Nothing.
 * @throws Nothing; the delegated probe contains every environment/I/O fault.
 * @note The native loader calls this helper before returning from `dlopen`.
 * Trust-gate tests require authorization rejection to prevent this helper and
 * every later registration callback from executing.
 */
__attribute__((constructor)) void untrusted_initializer() noexcept {
  emit_untrusted_initializer_sentinel();
}
#endif

}  // namespace
