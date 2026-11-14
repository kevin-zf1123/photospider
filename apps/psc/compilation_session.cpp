#include "psc/compilation_session.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "common/codec.hpp"
#include "common/process.hpp"
#include "photospider/compiler/artifact.hpp"
#include "psc/dag_io.hpp"

namespace ps::tools::psc {
namespace {
using Clock = std::chrono::steady_clock;
PlanningOptions planning(const Invocation& o, Files* files) {
  PlanningOptions result = o.planning;
  if (o.has("--requests")) {
    result.output_requests = read_requests(o.get("--requests"), files);
    result.output_names.emplace();
    for (const auto& entry : result.output_requests)
      result.output_names->push_back(entry.first);
  }
  return result;
}
}  // namespace
int compile(const Invocation& o, const Compiler& compiler, Files* files,
            Diagnostics* diagnostics) {
  auto last = terminal(o);
  auto path = o.has("--check") ? std::string{} : output_path(o, last);
  auto started = Clock::now();
  diagnostics->stage = "read";
  auto input = files->read(o.input);
  auto type = detect(input);
  if (!o.input_type.empty() && type != o.input_type)
    throw ToolError("input content conflicts with -x", 2);
  diagnostics->timing("read", started);
  diagnostics->stage = "resources";
  ResourceBudget root;
  auto resources = read_resources(o.get("--resources"), files, root);
  PlanningOptions options;
  if (!o.has("--check") && o.emit == EmitKind::Plan)
    options = planning(o, files);
  if (!o.has("--check"))
    files->reserve(path);
  for (const auto& snapshot : o.snapshots)
    files->reserve(snapshot.second);
  for (std::size_t i = last; i < o.optimizers.size(); ++i)
    diagnostics->warning("unused-optimizer",
                         "optimizer stage " + std::to_string(i + 1) +
                             " is after the requested endpoint");
  std::unique_ptr<GraphContext> graph;
  diagnostics->stage = type == "dag" ? "frontend" : "validation";
  started = Clock::now();
  ValidatedIR current;
  if (type == "dag") {
    graph = std::make_unique<GraphContext>(read_dag(input));
    current = take(compiler.analyze(graph->snapshot(), resources));
  } else {
    current = take(CompilerArtifact::import_ir(compiler, input, resources));
  }
  diagnostics->revision = 0;
  diagnostics->timing(diagnostics->stage, started);
  if (o.has("--check"))
    return 0;
  CompiledWorkflow session;
  session.ir_versions.push_back({0, current});
  struct Save {
    std::size_t revision;
    std::string path;
    std::future<std::uint64_t> completion;
  };
  std::vector<Save> saves;
  std::map<std::size_t, std::size_t> expected, completed;
  std::vector<std::pair<std::size_t, std::string>> published;
  for (const auto& snapshot : o.snapshots)
    ++expected[snapshot.first];
  if (o.emit != EmitKind::Plan)
    ++expected[last];
  auto release = [&](std::size_t index) {
    if (o.releases.count(index) && completed[index] == expected[index]) {
      session.ir_versions[index].ir.reset();
      if (diagnostics->verbose)
        diagnostics->event("remark", "PS-IR-RELEASE",
                           "released retained IR" + std::to_string(index));
    }
  };
  auto finish = [&](Save& save) {
    auto us = save.completion.get();
    struct RestoreDiagnostic {
      Diagnostics* destination;
      Diagnostics prior;
      ~RestoreDiagnostic() { *destination = std::move(prior); }
    } restore_diagnostic{diagnostics, *diagnostics};
    diagnostics->stage = "snapshot";
    diagnostics->revision = save.revision;
    diagnostics->source = save.path;
    published.emplace_back(save.revision, save.path);
    ++completed[save.revision];
    release(save.revision);
    if (diagnostics->timings)
      diagnostics->event("remark", "PS-TIME",
                         "save IR" + std::to_string(save.revision) +
                             " wall_us=" + std::to_string(us));
  };
  struct Failure {
    std::exception_ptr error;
    int priority;
    std::string stage, source;
    std::optional<std::size_t> revision;
  };
  std::vector<Failure> failures;
  auto remember = [&](std::exception_ptr error, const std::string& stage,
                      std::optional<std::size_t> revision,
                      const std::string& source) {
    int code = 3;
    try {
      std::rethrow_exception(error);
    } catch (const ToolError& e) {
      code = e.exit_code;
    } catch (const fs::filesystem_error&) {
      code = 4;
    } catch (const std::bad_alloc&) {
      code = 1;
    } catch (...) {
    }
    int priority = code == 130 ? 5
                   : code == 3 ? 4
                   : code == 4 ? 3
                   : code == 2 ? 2
                               : 1;
    failures.push_back({error, priority, stage, source, revision});
  };
  auto collect = [&](Save& save) {
    try {
      finish(save);
    } catch (...) {
      remember(std::current_exception(), "snapshot", save.revision, save.path);
    }
  };
  auto schedule = [&](std::size_t index) {
    for (const auto& snapshot : o.snapshots)
      if (snapshot.first == index) {
        std::size_t active = 0;
        for (auto& save : saves)
          if (save.completion.valid())
            ++active;
        if (active >= 2) {
          for (auto& save : saves)
            if (save.completion.valid()) {
              collect(save);
              break;
            }
        }
        auto ir = current;
        saves.push_back(
            {index, snapshot.second,
             std::async(
                 std::launch::async, [ir, target = snapshot.second, files] {
                   auto start = Clock::now();
                   auto text = take(CompilerArtifact::export_ir(ir));
                   files->write(target, text);
                   return static_cast<std::uint64_t>(
                       std::chrono::duration_cast<std::chrono::microseconds>(
                           Clock::now() - start)
                           .count());
                 })});
      }
  };

  try {
    schedule(0);
    for (std::size_t i = 0; i < last; ++i) {
      interrupted();
      diagnostics->stage = "optimizer:" + o.optimizers[i].name;
      diagnostics->revision = i + 1;
      started = Clock::now();
      if (diagnostics->verbose)
        diagnostics->event("remark", "PS-STAGE",
                           "begin optimizer stage " + std::to_string(i + 1));
      current = take(compiler.optimize(current, o.optimizers[i]));
      session.ir_versions.push_back({i + 1, current});
      diagnostics->timing(diagnostics->stage, started);
      schedule(i + 1);
      for (auto& save : saves)
        if (save.completion.valid() &&
            save.completion.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready)
          collect(save);
    }
    if (o.emit == EmitKind::Plan) {
      diagnostics->stage = "planner";
      started = Clock::now();
      session.plan = take(compiler.plan(current, options));
      diagnostics->timing("planner", started);
    }
  } catch (...) {
    remember(std::current_exception(), diagnostics->stage,
             diagnostics->revision, diagnostics->source);
  }
  // Drain all readers before releasing session/context/registry ownership.
  for (auto& save : saves)
    if (save.completion.valid()) {
      collect(save);
    }
  if (failures.empty()) {
    diagnostics->stage = "artifact";
    diagnostics->revision = last;
    diagnostics->source = path;
    started = Clock::now();
    try {
      interrupted();
      auto text = o.emit == EmitKind::Plan
                      ? take(CompilerArtifact::export_plan(*session.plan))
                      : take(CompilerArtifact::export_ir(current));
      files->write(path, text);
      if (o.emit != EmitKind::Plan) {
        ++completed[last];
        release(last);
      }
      diagnostics->timing("main output", started);
    } catch (...) {
      remember(std::current_exception(), diagnostics->stage,
               diagnostics->revision, diagnostics->source);
    }
  }
  if (!failures.empty()) {
    std::sort(failures.begin(), failures.end(),
              [](const Failure& a, const Failure& b) {
                if (a.priority != b.priority)
                  return a.priority > b.priority;
                return std::tie(a.revision, a.stage, a.source) <
                       std::tie(b.revision, b.stage, b.source);
              });
    int result = 0;
    std::size_t reported = 0;
    auto limit = o.has("--error-limit")
                     ? unsigned_number(o.get("--error-limit"), "--error-limit")
                     : 20;
    for (const auto& failure : failures) {
      if (limit && reported >= limit)
        break;
      diagnostics->stage = failure.stage;
      diagnostics->revision = failure.revision;
      diagnostics->source = failure.source;
      auto code = report_exception(*diagnostics, failure.error);
      if (!reported++)
        result = code;
    }
    for (const auto& saved : published) {
      diagnostics->stage = "snapshot";
      diagnostics->revision = saved.first;
      diagnostics->source = saved.second;
      diagnostics->event("note", "PS-SNAPSHOT-SAVED",
                         "snapshot remains published");
    }
    return result;
  }
  return 0;
}
}  // namespace ps::tools::psc
