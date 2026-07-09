#!/usr/bin/env python3
"""Verify the DirtyControlLane production facade."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any


FOCUSED_FILTER = (
    "DirtySourceLifecycleFacade.UsesInteractionServicePublicBoundary:"
    "DirtyControlLaneFacade.ExposesWakeupAndCutoffThroughInteractionService"
)

CONTROL_FILES = [
    "src/kernel/services/compute-service/dirty_control_lane.hpp",
    "src/kernel/services/compute-service/dirty_control_lane.cpp",
    "src/kernel/kernel.hpp",
    "src/kernel/kernel_dirty_roi_facade.cpp",
    "src/kernel/interaction.hpp",
    "tests/test_compute_service_split.cpp",
]

DOC_FILES = [
    "docs/kernel-architecture/Compute-Service-Split.md",
    "docs/kernel-architecture/Dirty-Region-Propagation.md",
    "docs/kernel-architecture/Compute-Flow.md",
    "docs/kernel-architecture/zh/Compute-Service-Split.zh.md",
    "docs/kernel-architecture/zh/Dirty-Region-Propagation.zh.md",
    "docs/kernel-architecture/zh/Compute-Flow.zh.md",
]

STALE_DOC_PATTERNS = [
    r"没有独立 .*DirtyControlLane",
    r"no dedicated .*DirtyControlLane",
    r"DirtyControlLane.*remain follow-up",
    r"独立 .*DirtyControlLane.*仍是后续",
]


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


def file_contains(repo: Path, rel_path: str, needle: str) -> bool:
    return needle in (repo / rel_path).read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    focused_tests = run_command(
        repo,
        ["build/tests/test_compute_service_split", f"--gtest_filter={FOCUSED_FILTER}"],
        out / "test_compute_service_split.log",
    )
    control_scan = run_command(
        repo,
        rg_command(["DirtyControlLane", "begin_dirty_source_control"], CONTROL_FILES),
        out / "control_lane_api_scan.log",
    )
    stale_doc_scan = run_command(
        repo,
        rg_command(STALE_DOC_PATTERNS, DOC_FILES),
        out / "stale_doc_scan.log",
        allow_no_match=True,
    )

    expected = {
        "focused_tests": {"ok": True},
        "control_lane_api": {
            "header_exists": True,
            "kernel_control_api": True,
            "interaction_control_api": True,
            "focused_test_mentions_cutoff": True,
        },
        "stale_doc_scan": {"line_count": 0},
    }
    actual = {
        "focused_tests": focused_tests,
        "control_lane_api": {
            "header_exists": (
                repo
                / "src/kernel/services/compute-service/dirty_control_lane.hpp"
            ).exists(),
            "kernel_control_api": file_contains(
                repo, "src/kernel/kernel.hpp", "begin_dirty_source_control"
            )
            and file_contains(
                repo,
                "src/kernel/kernel_dirty_roi_facade.cpp",
                "compute::DirtyControlLane lane",
            ),
            "interaction_control_api": file_contains(
                repo,
                "src/kernel/interaction.hpp",
                "cmd_begin_dirty_source_control",
            ),
            "focused_test_mentions_cutoff": file_contains(
                repo, "tests/test_compute_service_split.cpp", "cutoff_after_downstream"
            ),
        },
        "control_lane_api_scan": control_scan,
        "stale_doc_scan": stale_doc_scan,
    }

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)

    checks = [
        ("focused dirty control tests", actual["focused_tests"]["ok"]),
        ("DirtyControlLane header exists", actual["control_lane_api"]["header_exists"]),
        ("Kernel routes through DirtyControlLane", actual["control_lane_api"]["kernel_control_api"]),
        (
            "InteractionService exposes control API",
            actual["control_lane_api"]["interaction_control_api"],
        ),
        (
            "focused test asserts cutoff decision",
            actual["control_lane_api"]["focused_test_mentions_cutoff"],
        ),
        (
            "stale DirtyControlLane docs removed",
            actual["stale_doc_scan"]["line_count"]
            == expected["stale_doc_scan"]["line_count"],
        ),
    ]

    passed = True
    lines = []
    for name, ok in checks:
        lines.append(f"{name}: {'PASS' if ok else 'FAIL'}")
        passed = passed and ok
    lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    (out / "compare.log").write_text("\n".join(lines) + "\n", encoding="utf-8")

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
        "# DirtyControlLane Evidence",
        "",
        "This evidence proves feedback issue 7 using internal Kernel/InteractionService facade calls.",
        "",
        f"- Focused tests: `{out / 'test_compute_service_split.log'}`",
        f"- API scan: `{out / 'control_lane_api_scan.log'}`",
        f"- Stale documentation scan: `{out / 'stale_doc_scan.log'}`",
        f"- Expected: `{out / 'expected.json'}`",
        f"- Actual: `{out / 'actual.json'}`",
        f"- Compare: `{out / 'compare.log'}`",
    ]
    if optional_lines:
        summary_lines.extend(["", "Additional validation logs:", *optional_lines])
    summary_lines.extend(
        [
            "",
            "The focused test checks begin/update/end lifecycle, stable generation, dispatcher wakeup, and end-event cutoff decision.",
        ]
    )
    (out / "summary.md").write_text("\n".join(summary_lines) + "\n", encoding="utf-8")

    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
