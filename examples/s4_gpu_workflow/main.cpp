#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "image_fixture.hpp"  // NOLINT(build/include_subdir)
#include "workflow.hpp"       // NOLINT(build/include_subdir)

namespace {
void explain(const ps::ExecutionPlan& plan) {
  for (const auto& step : plan.physical_steps()) {
    const char* kind = step.kind == ps::PhysicalStepKind::Upload ? "upload"
                       : step.kind == ps::PhysicalStepKind::HostAccess
                           ? "host-access"
                           : "operation";
    std::cout << "plan " << kind
              << " node=" << plan.steps()[step.step_index].node_id
              << " packed_bytes=" << step.packed_bytes
              << " capacity_bound=" << step.allocation_bytes << " region=";
    for (auto d : step.region.dimensions()) {
      std::cout << d.offset << '+' << d.extent << ' ';
    }
    std::cout << '\n';
  }
}
void report(const ps::ExecutionDiagnostics& d) {
  std::cout << " dispatches=" << d.native_dispatch_count
            << " submissions=" << d.native_submission_count
            << " copied_inputs=" << d.transfer_count
            << " input_copy_bytes=" << d.transfer_bytes
            << " host_access=" << d.host_access_count
            << " result_copy_bytes=" << d.result_copy_bytes
            << " upload_hits=" << d.native_upload_hits
            << " cache_hits=" << d.cache_hits
            << " peak_bytes=" << d.peak_live_bytes
            << " shared_peak_bytes=" << d.shared_peak_live_bytes
            << " device_us=" << d.native_compute_us
            << " execute_us=" << d.execute_us
            << " fallback_count=" << d.fallback_reasons.size();
}
}  // namespace
int main(int argc, char** argv) {
  try {
    std::string scenario = "resident-chain", backend = "cpu", module,
                layout = "whole";
    bool cache = true, require_native = false, show_plan = false,
         layout_set = false;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--require-native") {
        require_native = true;
      } else if (arg == "--no-cache") {
        cache = false;
      } else if (arg == "--explain") {
        show_plan = true;
      } else if (arg == "--help") {
        std::cout
            << "--scenario "
               "resident-chain|all-operations|cache-edits|preview-export|"
               "fallback --backend cpu|metal --module PATH\n"
               "resident-chain: --layout whole|tiled|roi --no-cache --explain\n"
               "--require-native returns 77 when Metal is unavailable\n";
        return 0;
      } else {
        s3::require(i + 1 < argc, "missing option value");
        const std::string value = argv[++i];
        if (arg == "--scenario") {
          scenario = value;
        } else if (arg == "--backend") {
          backend = value;
        } else if (arg == "--module") {
          module = value;
        } else if (arg == "--layout") {
          layout = value;
          layout_set = true;
        } else {
          throw std::runtime_error("unknown option: " + arg);
        }
      }
    }
    s3::require(backend == "cpu" || backend == "metal",
                "backend must be cpu or metal");
    s3::require(layout == "whole" || layout == "tiled" || layout == "roi",
                "unknown layout");
    s3::require(
        scenario == "resident-chain" || (!layout_set && cache && !show_plan),
        "layout/no-cache/explain apply to resident-chain");
    auto registry = module.empty() ? ps::make_default_operation_registry()
                                   : std::make_shared<ps::OperationRegistry>();
    if (!module.empty()) {
      auto status = registry->load_plugin(module);
      s3::require(status.ok(), status.message);
      s3::require(registry->freeze().ok(), "registry freeze failed");
    }
    const auto mode = backend == "metal" ? ps::ExecutionMode::MetalFp32
                                         : ps::ExecutionMode::CpuExact;
    bool native = false;
    if (scenario == "cache-edits") {
      native = s4::cache_edits(registry, mode);
    } else if (scenario == "preview-export") {
      native = s4::preview_export(registry, mode);
    } else {
      ps::ExecutionContext execution(registry, s4::config(mode, cache));
      native = execution.gpu_enabled();
      if (scenario == "resident-chain") {
        s3::Scene scene(registry, mode, layout == "whole" ? 128 : 4,
                        layout == "whole" ? 128 : 4);
        auto frozen = scene.freeze(execution, 2);
        if (layout == "roi") {
          frozen = s3::take(frozen.for_region(
              "result", ps::Region({{2, 7}, {3, 9}, {0, 4}})));
        }
        if (show_plan) {
          explain(frozen.plan());
        }
        auto result = s3::take(execution.execute(frozen));
        s4_fixture::Scene oracle;
        oracle.expected = scene.oracle(2);
        s4_fixture::check(oracle, result.values.at("result"));
        if (native) {
          s3::require(result.diagnostics.native_dispatch_count > 0,
                      "no native dispatch in cold chain");
        }
        std::cout << "S4Gpu.ResidentChain backend=" << backend
                  << " native=" << native << " layout=" << layout;
        report(result.diagnostics);
        std::cout << " oracle=passed\n";
        for (const auto& reason : result.diagnostics.fallback_reasons) {
          std::cout << "fallback " << reason << '\n';
        }
      } else if (scenario == "all-operations") {
        const auto dispatches =
            s4_fixture::all_operations(execution, registry, mode);
        std::cout << "S4Gpu.AllOperations operations=8 dispatches="
                  << dispatches << " oracle=passed\n";
      } else if (scenario == "fallback") {
        s4_fixture::numeric_edges(execution, registry);
        ps::ExecutionContext disabled(registry);
        s4_fixture::all_operations(disabled, registry,
                                   ps::ExecutionMode::MetalFp32);
        std::cout
            << "S4Gpu.Fallback disabled_device=passed numeric_edges=passed "
               "circle_coverage=exact oracle=passed\n";
      } else {
        throw std::runtime_error("unknown scenario: " + scenario);
      }
    }
    if (require_native && !native) {
      std::cout << "Metal unavailable: native hardware acceptance skipped\n";
      return 77;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
