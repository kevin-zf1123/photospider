#pragma once
#include <cstring>

#include "photospider/plugin/operation_extensions.hpp"
#include "photospider/plugin/result_program.hpp"
namespace diagnostic_fixture {
inline ps::ExtensionIdentity identity() {
  return {"fixture.work", 1, "counters/1"};
}
inline ps::DiagnosticDefinition definition() {
  ps::DiagnosticDefinition type;
  type.identity = identity();
  type.payload_bytes = 24;
  type.validate = [](ps::ByteView bytes) {
    if (bytes.size() != 24)
      return ps::Status{ps::ErrorCode::InvalidArgument, "counter width"};
    std::uint64_t n[3];
    std::memcpy(n, bytes.data(), 24);
    return n[1] <= n[0] && n[2] <= n[0] - n[1]
               ? ps::Status::success()
               : ps::Status{ps::ErrorCode::InvalidArgument,
                            "inconsistent work counts"};
  };
  type.merge = [](ps::ByteView before, ps::ByteView report,
                  ps::DiagnosticDestination out) {
    std::uint64_t a[3], b[3];
    std::memcpy(a, before.data(), 24);
    std::memcpy(b, report.data(), 24);
    for (unsigned i = 0; i < 3; ++i) {
      if (b[i] > UINT64_MAX - a[i])
        return ps::Status{ps::ErrorCode::ResourceExhausted, "count overflow"};
      a[i] += b[i];
    }
    std::memcpy(out.data, a, 24);
    return ps::Status::success();
  };
  return type;
}
inline ps::Status report(const ps::ResultProgramPhase& phase,
                         std::uint64_t count) {
  const std::uint64_t values[3]{count, 0, 0};
  return phase.report_diagnostic(
      0, {reinterpret_cast<const std::uint8_t*>(values), 24});
}
inline std::uint64_t count(const ps::DiagnosticAccumulator& report,
                           unsigned index = 0) {
  auto bytes = report.find(identity());
  if (!bytes.ok())
    throw std::runtime_error("missing work report");
  std::uint64_t values[3];
  std::memcpy(values, bytes.value().data(), 24);
  return values[index];
}
}  // namespace diagnostic_fixture
