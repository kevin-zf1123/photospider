// FILE: cli/graph_cli.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <opencv2/core/ocl.hpp>

#include "cli/print_cli_help.hpp"
#include "cli_config.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

namespace {

/** @brief Process exit code reserved for unrecoverable memory exhaustion. */
constexpr int kResourceExhaustionExitCode = 3;

/**
 * @brief Checks whether an argument vector requests CLI help.
 *
 * @param argc Number of entries in `argv`.
 * @param argv Argument vector whose pointed-to strings remain valid.
 * @return True when `-h` or `--help` is present.
 * @throws Nothing.
 * @note The scan uses C-string comparison and performs no allocation, allowing
 * the process to avoid constructing the embedded Host on the help fast path.
 */
bool arguments_request_help(int argc, char** argv) noexcept {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-h") == 0 ||
        std::strcmp(argv[i], "--help") == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

/**
 * @brief Configures process runtime policy and executes the graph CLI.
 *
 * @param argc Number of command-line arguments.
 * @param argv Mutable argument vector supplied by the operating system.
 * @return The CLI result, or exit code 3 when resource exhaustion crosses the
 * reusable run boundary.
 * @throws Nothing under the documented CLI/Host exception contract.
 * @note The help fast path avoids Host/plugin construction. `std::bad_alloc`
 * is handled here, separately from recoverable run-boundary translation, with
 * an allocation-free diagnostic write.
 */
int main(int argc, char** argv) {
  try {
    try {
      // Hard-disable OpenCL runtime at process start to avoid spurious driver
      // errors.
      setenv("OPENCV_OPENCL_DEVICE", "disabled", 1);
      setenv("OPENCV_OPENCL_RUNTIME", "disabled", 1);
      cv::ocl::setUseOpenCL(false);

      // Avoid initializing plugins/Metal for the help-only process path.
      if (arguments_request_help(argc, argv)) {
        print_cli_help();
        return 0;
      }
      auto host = ps::create_embedded_host();
      return run_graph_cli(argc, argv, *host);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& e) {
      std::fputs("Fatal: graph_cli startup/runtime error: ", stderr);
      std::fputs(e.what(), stderr);
      std::fputc('\n', stderr);
      return 2;
    }
  } catch (const std::bad_alloc&) {
    std::fputs("Fatal: graph_cli resource exhaustion.\n", stderr);
    return kResourceExhaustionExitCode;
  }
}
