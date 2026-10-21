#include "psx/driver.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "common/codec.hpp"
#include "common/environment.hpp"
#include "common/process.hpp"
#include "photospider/compiler/artifact.hpp"
#include "psx/bindings_io.hpp"
#include "psx/options.hpp"
#include "psx/result_export.hpp"

namespace ps::tools::psx {
namespace {
using Clock = std::chrono::steady_clock;
void execute(const Invocation& o,
             const std::shared_ptr<OperationRegistry>& operations, Files* files,
             Diagnostics* diagnostics) {
  diagnostics->stage = "resources";
  ResourceBudget resource_root;
  auto resources = read_resources(o.get("--resources"), files, resource_root);
  diagnostics->stage = "plan-load";
  auto started = Clock::now();
  auto plan = take(CompilerArtifact::import_plan(
      operations, files->read(o.input), resources));
  if (o.has("--requests")) {
    plan = take(plan.select_outputs(read_requests(o.get("--requests"), files)));
  } else if (o.has("--outputs")) {
    std::map<std::string, TileRequest> requests;
    for (const auto& name : output_names(o.get("--outputs"))) {
      auto found = plan.output_requests().find(name);
      if (found == plan.output_requests().end())
        throw ToolError("unknown compiled output: " + name);
      requests.emplace(name, found->second);
    }
    plan = take(plan.select_outputs(requests));
  }
  diagnostics->timing("plan load", started);
  ExecutionContext context(operations, o.context);
  diagnostics->stage = "prepare";
  started = Clock::now();
  auto prepared = take(context.prepare(plan));
  diagnostics->timing("prepare", started);
  diagnostics->stage = "bindings";
  started = Clock::now();
  ExecutionBindings bindings;
  if (!o.has("--check") || o.has("--bindings"))
    bindings = read_bindings(o.get("--bindings"), plan, files,
                             take(context.resource_budget()));
  diagnostics->timing("bindings", started);
  if (o.has("--check"))
    return;
  auto destination = output_path(o);
  files->reserve(destination);
  if (destination == "-" || fs::exists(destination))
    throw ToolError("result directory must be new", 4);
  const auto& options = o.execution;
  CancellationSource cancellation;
  std::atomic<bool> done{false};
  std::thread monitor([&] {
    while (!done.load()) {
      if (interruption_requested())
        cancellation.cancel();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  struct Join {
    std::atomic<bool>& done;
    std::thread& thread;
    ~Join() {
      done = true;
      thread.join();
    }
  } join{done, monitor};
  diagnostics->stage = "execution";
  started = Clock::now();
  auto run = take(context.submit(prepared, std::move(bindings),
                                 cancellation.token(), options));
  auto result = take(run.join());
  diagnostics->timing("execution", started);
  diagnostics->stage = "result-export";
  started = Clock::now();
  export_results(result, destination, *files, cancellation.token(),
                 take(context.resource_budget()));
  diagnostics->timing("result export", started);
}
}  // namespace
int run(int argc, char** argv, Diagnostics* diagnostics) {
  auto o = parse_options(argc, argv, &diagnostics->json);
  configure_diagnostics(o, diagnostics);
  if (o.has("--help")) {
    std::cout << "psx <plan.psxp|-> [options]\n";
    print_common_help();
    std::cout << "psx options:\n"
                 "  --bindings FILE --threads N --parallelism N\n"
                 "  --max-live-bytes N\n";
    return 0;
  }
  if (o.has("--version")) {
    print_version("psx");
    return 0;
  }
  Files files;
  diagnostics->stage = "contracts";
  auto started = std::chrono::steady_clock::now();
  auto registry = load_operations(o.libraries, &files);
  diagnostics->timing("contracts", started);
  if (o.has("--list-operations")) {
    list_operations(*registry);
    return 0;
  }
  execute(o, registry, &files, diagnostics);
  return 0;
}
}  // namespace ps::tools::psx
