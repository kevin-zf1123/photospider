#!/usr/bin/env python3
"""Collect focused evidence for scheduler ready-task dispatch boundaries."""

import argparse
import json
import pathlib
import subprocess
import sys


def run_command(cmd, cwd, log_path):
    completed = subprocess.run(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    log_path.write_text(completed.stdout, encoding="utf-8")
    return {
        "command": cmd,
        "returncode": completed.returncode,
        "log": str(log_path),
        "passed": completed.returncode == 0,
    }


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        return ""
    brace = source.find("{", start)
    if brace < 0:
        return ""
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    return ""


def scan_scheduler_lifecycle(repo: pathlib.Path, out_dir: pathlib.Path):
    kernel_cpp = (repo / "src/kernel/kernel.cpp").read_text(encoding="utf-8")
    graph_runtime_mm = (repo / "src/kernel/graph_runtime.mm").read_text(
        encoding="utf-8"
    )
    gpu_scheduler_cpp = (
        repo / "src/kernel/scheduler/gpu_pipeline_scheduler.cpp"
    ).read_text(encoding="utf-8")

    setup_body = function_body(kernel_cpp, "void Kernel::setup_schedulers_for_runtime")
    start_body = function_body(graph_runtime_mm, "void GraphRuntime::start()")
    stop_body = function_body(graph_runtime_mm, "void GraphRuntime::stop()")
    set_body = function_body(
        graph_runtime_mm, "void GraphRuntime::set_scheduler("
    )
    gpu_attach_body = function_body(
        gpu_scheduler_cpp, "void GpuPipelineScheduler::attach"
    )
    gpu_dispatch_body = function_body(
        gpu_scheduler_cpp, "bool GpuPipelineScheduler::can_dispatch_hp_to_gpu"
    )

    attach_pos = set_body.find("attach(this)")
    start_pos = set_body.find("start()", attach_pos)

    checks = {
        "kernel_setup_registers_hp_scheduler": (
            "runtime.set_scheduler(ComputeIntent::GlobalHighPrecision" in setup_body
        ),
        "kernel_setup_registers_rt_scheduler": (
            "runtime.set_scheduler(ComputeIntent::RealTimeUpdate" in setup_body
        ),
        "kernel_setup_does_not_prestart_hp_scheduler": (
            "hp_scheduler->start()" not in setup_body
        ),
        "kernel_setup_does_not_prestart_rt_scheduler": (
            "rt_scheduler->start()" not in setup_body
        ),
        "runtime_start_starts_registered_schedulers": (
            "scheduler->start()" in start_body
        ),
        "runtime_stop_shuts_down_registered_schedulers": (
            "scheduler->shutdown()" in stop_body
        ),
        "runtime_set_scheduler_starts_after_attach": (
            attach_pos >= 0 and start_pos > attach_pos
        ),
        "gpu_attach_backfills_gpu_workers_when_already_running": (
            "start_gpu_workers_if_available()" in gpu_attach_body
        ),
        "gpu_dispatch_requires_started_gpu_workers": (
            "num_gpu_workers_ > 0" in gpu_dispatch_body
        ),
    }
    log_lines = [
        f"{name}={'PASS' if passed else 'FAIL'}"
        for name, passed in sorted(checks.items())
    ]
    log_path = out_dir / "scheduler_lifecycle_scan.log"
    log_path.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
    return {
        "checks": checks,
        "log": str(log_path),
        "passed": all(checks.values()),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    out_dir = pathlib.Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    source_first = run_command(
        [
            str(repo / "build/tests/test_scheduler"),
            "--gtest_filter=SchedulerDirtyReadyTasks.SourceFirstOrderOnSerialAndCpuSchedulers",
        ],
        repo,
        out_dir / "source_first.log",
    )
    gpu_policy = run_command(
        [
            str(repo / "build/tests/test_gpu_pipeline_scheduler"),
            "--gtest_filter=GpuPipelineSchedulerTest.HPPrefersGPU:"
            "GpuPipelineSchedulerTest.RTPriorityOverHP:"
            "GpuPipelineSchedulerTest.NewEpochCancelsStale:"
            "GpuPipelineSchedulerTest.ExceptionPropagation",
        ],
        repo,
        out_dir / "gpu_policy.log",
    )
    lifecycle_tests = run_command(
        [
            str(repo / "build/tests/test_milestone2"),
            "--gtest_filter=GraphRuntimeSchedulerTest.StartStartsAttachedSchedulers:"
            "GraphRuntimeSchedulerTest.SetSchedulerOnRunningRuntimeStartsAfterAttach",
        ],
        repo,
        out_dir / "scheduler_lifecycle_tests.log",
    )
    gpu_integration = run_command(
        [
            "ctest",
            "--output-on-failure",
            "--test-dir",
            "build",
            "-R",
            r"^GpuPipelineIntegrationTest\.(SchedulerWithRuntime|DualSchedulerConcurrentExecution)$",
            "--timeout",
            "15",
        ],
        repo,
        out_dir / "gpu_pipeline_integration_ctest.log",
    )
    boundary_scan = run_command(
        [
            "rg",
            "-n",
            "ComputeTaskGraph|DirtyRegionSnapshot|DirtyUpdateWorkSet|"
            "dependency_counter|dirty_source|DirtyRegionPlanner",
            "include/kernel/scheduler",
            "src/kernel/scheduler",
        ],
        repo,
        out_dir / "scheduler_boundary_scan.log",
    )
    lifecycle_scan = scan_scheduler_lifecycle(repo, out_dir)

    actual = {
        "source_first": source_first,
        "gpu_policy": gpu_policy,
        "scheduler_lifecycle_tests": lifecycle_tests,
        "gpu_pipeline_integration": gpu_integration,
        "scheduler_boundary_scan": boundary_scan,
        "boundary_scan_has_no_matches": boundary_scan["returncode"] == 1,
        "scheduler_lifecycle_scan": lifecycle_scan,
    }
    expected = {
        "source_first_passed": True,
        "gpu_policy_passed": True,
        "scheduler_lifecycle_tests_passed": True,
        "gpu_pipeline_integration_passed": True,
        "boundary_scan_has_no_matches": True,
        "scheduler_lifecycle_scan_passed": True,
    }
    passed = (
        source_first["passed"]
        and gpu_policy["passed"]
        and lifecycle_tests["passed"]
        and gpu_integration["passed"]
        and boundary_scan["returncode"] == 1
        and lifecycle_scan["passed"]
    )

    (out_dir / "expected.json").write_text(
        json.dumps(expected, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (out_dir / "actual.json").write_text(
        json.dumps(actual, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    compare = [
        f"source_first_passed={source_first['passed']}",
        f"gpu_policy_passed={gpu_policy['passed']}",
        f"scheduler_lifecycle_tests_passed={lifecycle_tests['passed']}",
        f"gpu_pipeline_integration_passed={gpu_integration['passed']}",
        f"boundary_scan_has_no_matches={boundary_scan['returncode'] == 1}",
        f"scheduler_lifecycle_scan_passed={lifecycle_scan['passed']}",
        f"overall={'PASS' if passed else 'FAIL'}",
    ]
    (out_dir / "compare.log").write_text("\n".join(compare) + "\n",
                                          encoding="utf-8")
    print("\n".join(compare))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
