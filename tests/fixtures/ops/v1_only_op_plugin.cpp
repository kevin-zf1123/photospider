#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define PHOTOSPIDER_V1_FIXTURE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define PHOTOSPIDER_V1_FIXTURE_EXPORT __attribute__((visibility("default")))
#else
#define PHOTOSPIDER_V1_FIXTURE_EXPORT
#endif

namespace {

/** @brief Environment variable naming the unexpected-v1 invocation marker. */
constexpr const char* kInvocationMarkerEnvironment = "PS_V1_ONLY_PLUGIN_MARKER";

}  // namespace

/**
 * @brief Marks any accidental invocation of the unsupported v1 entry point.
 * @param ignored Registrar-shaped pointer ignored by this negative fixture.
 * @return Nothing.
 * @throws Nothing; file errors are deliberately ignored by the marker fixture.
 * @note This DSO intentionally exports no v2 symbol and includes no Photospider
 * header. The v2-only loader must reject it during symbol resolution and never
 * call this function.
 */
extern "C" PHOTOSPIDER_V1_FIXTURE_EXPORT void register_photospider_ops_v1(
    void* ignored) noexcept {
  (void)ignored;
  if (const char* path = std::getenv(kInvocationMarkerEnvironment)) {
    if (std::FILE* marker = std::fopen(path, "ab")) {
      (void)std::fputs("v1_called\n", marker);
      (void)std::fclose(marker);
    }
  }
}

#undef PHOTOSPIDER_V1_FIXTURE_EXPORT
