#!/usr/bin/env python3
"""Verify GraphRuntime no longer owns the legacy worker queue."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any


LEGACY_PATTERNS = [
    r"\.post\(",
    r"->post\(",
    r"GraphRuntime::post",
    r"submit_initial_tasks",
    r"submit_ready_task",
    r"wait_for_completion",
    r"dec_graph_tasks_to_complete",
    r"inc_graph_tasks_to_complete",
    r"struct TaskGraph",
    r"using Task =",
    r"ScheduledTask",
    r"TaskPriority",
    r"workers_\b",
    r"local_task_queues_\b",
    r"high_priority_queue_\b",
    r"normal_priority_queue_\b",
    r"ready_task_count_\b",
    r"sleeping_thread_count_\b",
    r"tasks_to_complete_\b",
    r"first_exception_\b",
    r"has_exception_\b",
]

DOC_PATTERNS = [
    r"support queue",
    r"support queues",
    r"worker state",
    r"worker 状态",
    r"GraphRuntime.*worker queue",
    r"GraphRuntime.*内部队列",
    r"Runtime and scheduler queues",
    r"运行时和调度器队列",
]

LEGACY_SCAN_FILES = [
    "include/kernel/graph_runtime.hpp",
    "src/kernel/graph_runtime.mm",
    "include/kernel/kernel.hpp",
    "src/kernel/kernel.cpp",
    "tests/test_scheduler.cpp",
    "tests/split_compute_service_trace.cpp",
]

DOC_SCAN_PATHS = [
    "docs/kernel-architecture",
    "docs/adr",
]

SCHEDULER_FILTER = (
    "SchedulerDirtyReadyTasks.SourceFirstOrderOnSerialAndCpuSchedulers:"
    "Scheduler.DirtyRegionTiledComputation:"
    "Scheduler.DirtyRegionProductionTraceCoversStaleGenerationAndExceptionRethrow"
)

COMPUTE_SPLIT_FILTER = (
    "IntentUpdateCoordinatorSplit.ValidatesRtDirtyRoiAndCoordinatesDualPathWithoutParallel:"
    "GlobalHighPrecisionDirtyUpdate.UsesDirtyPlanningForGlobalHpDirtyRoi:"
    "DirtySourceLifecycleFacade.UsesInteractionServicePublicBoundary"
)


def run_command(
    repo: Path, command: list[str], log_path: Path, allow_no_match: bool = False
) -> dict[str, Any]:
    proc = subprocess.run(
        command,
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    log_path.write_text(proc.stdout, encoding="utf-8")
    ok = proc.returncode == 0 or (allow_no_match and proc.returncode == 1)
    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    return {
        "command": command,
        "exit_code": proc.returncode,
        "ok": ok,
        "line_count": len(lines),
        "log": str(log_path),
    }


def rg_command(patterns: list[str], paths: list[str]) -> list[str]:
    command = ["rg", "-n"]
    for pattern in patterns:
      command.extend(["-e", pattern])
    command.extend(paths)
    return command


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    legacy_scan = run_command(
        repo,
        rg_command(LEGACY_PATTERNS, LEGACY_SCAN_FILES),
        out / "legacy_boundary_scan.log",
        allow_no_match=True,
    )
    docs_scan = run_command(
        repo,
        rg_command(DOC_PATTERNS, DOC_SCAN_PATHS),
        out / "docs_legacy_statement_scan.log",
        allow_no_match=True,
    )
    scheduler_test = run_command(
        repo,
        ["build/tests/test_scheduler", f"--gtest_filter={SCHEDULER_FILTER}"],
        out / "test_scheduler.log",
    )
    compute_split_test = run_command(
        repo,
        [
            "build/tests/test_compute_service_split",
            f"--gtest_filter={COMPUTE_SPLIT_FILTER}",
        ],
        out / "test_compute_service_split.log",
    )

    graph_state_header = repo / "include/kernel/graph_state_executor.hpp"
    graph_runtime_header = (
        repo / "include/kernel/graph_runtime.hpp"
    ).read_text(encoding="utf-8")
    kernel_header = (repo / "include/kernel/kernel.hpp").read_text(
        encoding="utf-8"
    )

    expected = {
        "legacy_boundary_scan": {"line_count": 0},
        "docs_legacy_statement_scan": {"line_count": 0},
        "graph_state_executor": {
            "header_exists": True,
            "runtime_owns_executor": True,
            "kernel_post_routes_to_executor": True,
        },
        "focused_tests": {
            "test_scheduler_ok": True,
            "test_compute_service_split_ok": True,
        },
    }

    actual = {
        "legacy_boundary_scan": legacy_scan,
        "docs_legacy_statement_scan": docs_scan,
        "graph_state_executor": {
            "header_exists": graph_state_header.exists(),
            "runtime_owns_executor": "GraphStateExecutor graph_state_;"
            in graph_runtime_header,
            "kernel_post_routes_to_executor": "graph_state().submit"
            in kernel_header,
        },
        "focused_tests": {
            "test_scheduler_ok": scheduler_test["ok"],
            "test_compute_service_split_ok": compute_split_test["ok"],
        },
    }

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)

    checks = [
        (
            "legacy boundary scan",
            actual["legacy_boundary_scan"]["line_count"]
            == expected["legacy_boundary_scan"]["line_count"],
        ),
        (
            "docs legacy statement scan",
            actual["docs_legacy_statement_scan"]["line_count"]
            == expected["docs_legacy_statement_scan"]["line_count"],
        ),
        (
            "GraphStateExecutor header exists",
            actual["graph_state_executor"]["header_exists"],
        ),
        (
            "GraphRuntime owns GraphStateExecutor",
            actual["graph_state_executor"]["runtime_owns_executor"],
        ),
        (
            "Kernel::post routes to GraphStateExecutor",
            actual["graph_state_executor"]["kernel_post_routes_to_executor"],
        ),
        ("test_scheduler focused tests", actual["focused_tests"]["test_scheduler_ok"]),
        (
            "test_compute_service_split focused tests",
            actual["focused_tests"]["test_compute_service_split_ok"],
        ),
    ]

    compare_lines = []
    passed = True
    for name, ok in checks:
        compare_lines.append(f"{name}: {'PASS' if ok else 'FAIL'}")
        passed = passed and ok
    compare_lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    (out / "compare.log").write_text("\n".join(compare_lines) + "\n", encoding="utf-8")

    optional_logs = [
        "build.log",
        "build_full.log",
        "ctest_full.log",
        "py_compile.log",
        "git_diff_check.log",
        "git_personal_diff_check.log",
    ]
    optional_lines = [
        f"- {name}: `{out / name}`" for name in optional_logs if (out / name).exists()
    ]
    summary_lines = [
        "# GraphRuntime Worker Queue Removal Evidence",
        "",
        "This evidence proves feedback issue 5 using runtime-derived logs.",
        "",
        f"- Legacy API scan: `{out / 'legacy_boundary_scan.log'}`",
        f"- Documentation scan: `{out / 'docs_legacy_statement_scan.log'}`",
        f"- Focused scheduler tests: `{out / 'test_scheduler.log'}`",
        f"- Focused compute split tests: `{out / 'test_compute_service_split.log'}`",
        f"- Expected: `{out / 'expected.json'}`",
        f"- Actual: `{out / 'actual.json'}`",
        f"- Compare: `{out / 'compare.log'}`",
    ]
    if optional_lines:
        summary_lines.extend(["", "Additional validation logs:", *optional_lines])
    summary_lines.extend(
        [
            "",
            "The legacy scan is scoped to GraphRuntime, Kernel, and the tests that previously used GraphRuntime::post.",
            "Scheduler-owned `SchedulerTaskRuntime` APIs remain intentionally out of scope.",
        ]
    )
    summary = "\n".join(summary_lines)
    (out / "summary.md").write_text(summary + "\n", encoding="utf-8")

    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
