#include <cstdio>
#include <iostream>
#include <string>

#include "common/diagnostics.hpp"
#include "common/file_io.hpp"
#include "common/json.hpp"

namespace ps::tools {
namespace {
std::string error_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok:
      return "Ok";
    case ErrorCode::InvalidArgument:
      return "InvalidArgument";
    case ErrorCode::NotFound:
      return "NotFound";
    case ErrorCode::Cycle:
      return "Cycle";
    case ErrorCode::TypeMismatch:
      return "TypeMismatch";
    case ErrorCode::ResourceExhausted:
      return "ResourceExhausted";
    case ErrorCode::Cancelled:
      return "Cancelled";
    case ErrorCode::Stale:
      return "Stale";
    case ErrorCode::BackendUnavailable:
      return "BackendUnavailable";
    case ErrorCode::OperationFailed:
      return "OperationFailed";
    case ErrorCode::Internal:
      return "Internal";
  }
  return "Internal";
}
}  // namespace
void check(Status status) {
  if (status.ok())
    return;
  const int code = status.code == ErrorCode::Internal          ? 3
                   : status.code == ErrorCode::Cancelled       ? 130
                   : status.detail.origin == FailureOrigin::Io ? 4
                                                               : 1;
  ToolError error(
      status.message.empty() ? error_name(status.code) : status.message, code,
      "PS-" + error_name(status.code));
  error.status = error_name(status.code);
  error.reason = status.reason;
  error.node = status.detail.node_id;
  error.input = status.detail.input_id;
  throw error;
}
void Diagnostics::event(const std::string& severity, const std::string& id,
                        const std::string& message, const std::string& code,
                        std::uint64_t node, std::uint64_t input,
                        FailureReason reason) const try {
  if (json) {
    JsonWriter w(JsonWriter::InvalidUtf8::Replace);
    auto* root = w.object();
    w.put(root, "version", w.integer(std::uint64_t{1}));
    w.put(root, "severity", w.str(severity));
    w.put(root, "id", w.str(id));
    w.put(root, "stage", w.str(stage));
    w.put(root, "message", w.str(message));
    if (!source.empty())
      w.put(root, "source", w.str(source));
    if (!code.empty())
      w.put(root, "status_code", w.str(code));
    if (reason != FailureReason::None)
      w.put(root, "failure_reason",
            w.integer(static_cast<std::uint64_t>(reason)));
    if (revision)
      w.put(root, "ir_revision",
            w.integer(static_cast<std::uint64_t>(*revision)));
    if (node)
      w.put(root, "node_id", w.integer(node));
    if (input)
      w.put(root, "input_id", w.integer(input));
    std::cerr << w.finish(root);
  } else {
    if (color)
      std::cerr << (severity == "error" ? "\033[31m" : "\033[36m");
    if (!source.empty())
      std::cerr << source << ": ";
    std::cerr << stage;
    if (revision)
      std::cerr << " IR" << *revision;
    std::cerr << ": " << severity << " [" << id << "]: " << message;
    if (node)
      std::cerr << " (node " << node << ')';
    if (input)
      std::cerr << " (input " << input << ')';
    if (reason != FailureReason::None)
      std::cerr << " (reason " << static_cast<unsigned>(reason) << ')';
    if (color)
      std::cerr << "\033[0m";
    std::cerr << '\n';
  }
} catch (...) {
  // Reporting an allocation/encoding failure must not terminate the driver.
  std::fputs(
      "{\"version\":1,\"severity\":\"fatal\",\"id\":\"PS-DIAGNOSTIC-FAILURE\","
      "\"stage\":\"driver\",\"message\":\"diagnostic unavailable\"}\n",
      stderr);
}
void Diagnostics::warning(const std::string& group,
                          const std::string& message) const {
  bool enabled = true, error = false;
  std::optional<bool> specific_enabled, specific_error;
  for (const auto& flag : warnings) {
    if (flag == "-Wall" || flag == "-Wextra") {
      enabled = true;
    } else if (flag == "-Werror") {
      error = true;
    } else if (flag == "-Wno-error") {
      error = false;
    } else if (flag == "-W" + group) {
      specific_enabled = true;
    } else if (flag == "-Wno-" + group) {
      specific_enabled = false;
    } else if (flag == "-Werror=" + group) {
      specific_enabled = true;
      specific_error = true;
    } else if (flag == "-Wno-error=" + group) {
      specific_error = false;
    }
  }
  if (!specific_enabled.value_or(enabled))
    return;
  if (specific_error.value_or(error))
    throw ToolError(message, 1, "PS-W-" + group);
  event("warning", "PS-W-" + group, message);
}
void Diagnostics::timing(const std::string& name,
                         std::chrono::steady_clock::time_point start) const {
  if (!timings)
    return;
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start)
                .count();
  event("remark", "PS-TIME", name + " wall_us=" + std::to_string(us));
}
int report_exception(const Diagnostics& diagnostics, std::exception_ptr error) {
  try {
    std::rethrow_exception(error);
  } catch (const ToolError& e) {
    diagnostics.event("error", e.id, e.what(), e.status, e.node, e.input,
                      e.reason);
    return e.exit_code;
  } catch (const fs::filesystem_error& e) {
    diagnostics.event("error", "PS-IO", e.what());
    return 4;
  } catch (const std::bad_alloc&) {
    diagnostics.event("error", "PS-ResourceExhausted", "allocation failed",
                      "ResourceExhausted");
    return 1;
  } catch (const std::exception& e) {
    diagnostics.event("fatal", "PS-INTERNAL", e.what(), "Internal");
    return 3;
  } catch (...) {
    diagnostics.event("fatal", "PS-INTERNAL", "unknown internal failure",
                      "Internal");
    return 3;
  }
}
}  // namespace ps::tools
