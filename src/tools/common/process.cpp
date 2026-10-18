#include "common/process.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ps::tools {
namespace {
std::atomic<std::sig_atomic_t> stopped{0};
static_assert(std::atomic<std::sig_atomic_t>::is_always_lock_free,
              "signal flag must use lock-free atomics");
void on_signal(int) {
  stopped = 1;
}
int stdout_done() {
  std::cout.flush();
  interrupted();
  if (!std::cout)
    throw ToolError("stdout write failed", 4, "PS-IO-WRITE");
  return 0;
}
}  // namespace
bool interruption_requested() noexcept {
  return stopped != 0;
}
void interrupted() {
  if (interruption_requested())
    throw ToolError("interrupted", 130, "PS-Cancelled");
}
void configure_diagnostics(const CommonOptions& o, Diagnostics* diagnostics) {
  diagnostics->json = o.get("--diagnostic-format", "text") == "json";
  diagnostics->source = o.input;
  diagnostics->verbose = o.has("--verbose");
  diagnostics->timings = o.has("--time-report");
  diagnostics->warnings = o.warnings;
#if defined(_WIN32)
  bool tty = _isatty(2) != 0;
#else
  bool tty = isatty(2) != 0;
#endif
  diagnostics->color =
      o.get("--color", "auto") == "always" ||
      (o.get("--color", "auto") == "auto" && tty && !std::getenv("NO_COLOR"));
  const std::set<std::string> groups = {"unused-optimizer"};
  for (auto flag : o.warnings) {
    if (flag == "-Wall" || flag == "-Wextra" || flag == "-Werror" ||
        flag == "-Wno-error")
      continue;
    flag.erase(0, 2);
    if (flag.rfind("no-", 0) == 0)
      flag.erase(0, 3);
    if (flag.rfind("error=", 0) == 0)
      flag.erase(0, 6);
    if (!groups.count(flag))
      throw ToolError("unknown warning group: " + flag, 2);
  }
}
void print_common_help() {
  std::cout << "Common options:\n"
               "  -o PATH --check --load-ops LIB --resources FILE\n"
               "  --outputs NAMES | --requests FILE\n"
               "  --list-operations\n"
               "  --diagnostic-format text|json --color auto|always|never\n"
               "  --time-report --version -h --help\n";
}
void print_version(const char* name) {
  std::cout
      << name << " " << PHOTOSPIDER_CLI_VERSION
      << "\nDAG JSON v1 / WorkflowDocument 7; psir 4; psxp 4; Result ABI 7\n";
}
int run_cli(int argc, char** argv, int (*driver)(int, char**, Diagnostics*)) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  Diagnostics diagnostics;
  stopped = 0;
#if defined(_WIN32)
  auto prior = std::signal(SIGINT, on_signal);
#else
  struct sigaction action{}, prior{};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  // Blocking file operations must return on SIGINT instead of restarting.
  if (sigaction(SIGINT, &action, &prior) != 0) {
    diagnostics.event("error", "PS-SIGNAL", "cannot install SIGINT handler");
    return 3;
  }
#endif
#if defined(SIGPIPE)
  auto prior_pipe = std::signal(SIGPIPE, SIG_IGN);
#endif
  struct Restore {
#if defined(_WIN32)
    void (*handler)(int);
#else
    struct sigaction handler;
#endif
#if defined(SIGPIPE)
    void (*pipe_handler)(int);
#endif
    ~Restore() {
#if defined(_WIN32)
      std::signal(SIGINT, handler);
#else
      sigaction(SIGINT, &handler, nullptr);
#endif
#if defined(SIGPIPE)
      std::signal(SIGPIPE, pipe_handler);
#endif
    }
  } restore{prior
#if defined(SIGPIPE)
            ,
            prior_pipe
#endif
  };
  try {
    const int result = driver(argc, argv, &diagnostics);
    interrupted();
    stdout_done();
    return result;
  } catch (...) {
    return report_exception(diagnostics, std::current_exception());
  }
}
}  // namespace ps::tools
