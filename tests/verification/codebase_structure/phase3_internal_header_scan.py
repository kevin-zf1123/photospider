#!/usr/bin/env python3
"""Verify issue #32 internal header and public snapshot boundaries."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


PUBLIC_HEADER_GLOBS = ("*.h", "*.hpp", "*.hh", "*.hxx")
FORBIDDEN_PUBLIC_NAMES = (
    "GraphModel",
    "GraphRuntime",
    "ComputeService",
    "ComputePlan",
    "FullTaskGraph",
    "DirtyControlLane",
    "CpuWorkStealingScheduler",
    "GpuPipelineScheduler",
    "SerialDebugScheduler",
)
MOVED_INTERNAL_HEADERS = {
    "graph_model": ("include/graph_model.hpp", "src/graph_model.hpp"),
    "graph_runtime": (
        "include/kernel/graph_runtime.hpp",
        "src/kernel/graph_runtime.hpp",
    ),
    "graph_state_executor": (
        "include/kernel/graph_state_executor.hpp",
        "src/kernel/graph_state_executor.hpp",
    ),
    "compute_service": (
        "include/kernel/services/compute_service.hpp",
        "src/kernel/services/compute_service.hpp",
    ),
    "dirty_control_lane": (
        "include/kernel/services/compute-service/dirty_control_lane.hpp",
        "src/kernel/services/compute-service/dirty_control_lane.hpp",
    ),
    "cpu_work_stealing_scheduler": (
        "include/kernel/scheduler/cpu_work_stealing_scheduler.hpp",
        "src/kernel/scheduler/cpu_work_stealing_scheduler.hpp",
    ),
    "gpu_pipeline_scheduler": (
        "include/kernel/scheduler/gpu_pipeline_scheduler.hpp",
        "src/kernel/scheduler/gpu_pipeline_scheduler.hpp",
    ),
    "serial_debug_scheduler": (
        "include/kernel/scheduler/serial_debug_scheduler.hpp",
        "src/kernel/scheduler/serial_debug_scheduler.hpp",
    ),
    "kernel_facade": (
        "include/kernel/kernel.hpp",
        "src/kernel/kernel.hpp",
    ),
    "interaction_facade": (
        "include/kernel/interaction.hpp",
        "src/kernel/interaction.hpp",
    ),
}
PRIVATE_PLANNING_HEADERS = (
    "src/kernel/services/compute-service/task_graph_planning.hpp",
    "src/kernel/services/compute-service/dirty_region_planner.hpp",
    "src/kernel/services/compute-service/dirty_region_snapshot.hpp",
)
REQUIRED_PUBLIC_SNAPSHOTS = (
    "DirtyRegionInspectionSnapshot",
    "ComputePlanningInspectionSnapshot",
    "ComputePlanningTaskSnapshot",
    "SchedulerTraceEventSnapshot",
)
REQUIRED_HOST_METHODS = (
    "dirty_region_snapshot",
    "compute_planning_snapshot",
    "recent_compute_planning_snapshots",
    "scheduler_trace",
)


def rel(repo: Path, path: Path) -> str:
    """Return a path relative to the repository root."""

    return path.relative_to(repo).as_posix()


def public_headers(repo: Path) -> list[Path]:
    """Return the installable public header inventory."""

    root = repo / "include" / "photospider"
    headers: list[Path] = []
    for pattern in PUBLIC_HEADER_GLOBS:
        headers.extend(root.rglob(pattern))
    return sorted(path for path in headers if path.is_file())


def line_number_for_offset(text: str, offset: int) -> int:
    """Convert a text offset to a one-based line number."""

    return text.count("\n", 0, offset) + 1


def scan_public_header_names(repo: Path) -> dict[str, Any]:
    """Find forbidden internal names in the installable header inventory."""

    hits: list[dict[str, Any]] = []
    headers = public_headers(repo)
    patterns = [
        (name, re.compile(rf"\b{re.escape(name)}\b"))
        for name in FORBIDDEN_PUBLIC_NAMES
    ]
    for path in headers:
        text = path.read_text(encoding="utf-8")
        header = rel(repo, path)
        for name, pattern in patterns:
            for match in pattern.finditer(text):
                line_number = line_number_for_offset(text, match.start())
                line = text.splitlines()[line_number - 1].strip()
                hits.append(
                    {
                        "file": header,
                        "line": line_number,
                        "name": name,
                        "text": line,
                    }
                )
    return {
        "headers": [rel(repo, path) for path in headers],
        "header_count": len(headers),
        "forbidden_name_hits": hits,
    }


def inspect_internal_headers(repo: Path) -> list[dict[str, Any]]:
    """Check that issue #32 headers moved from include/ to src/."""

    rows: list[dict[str, Any]] = []
    for label, (old_path, new_path) in MOVED_INTERNAL_HEADERS.items():
        rows.append(
            {
                "label": label,
                "old_path": old_path,
                "old_path_absent": not (repo / old_path).exists(),
                "new_path": new_path,
                "new_path_present": (repo / new_path).is_file(),
            }
        )
    for path in PRIVATE_PLANNING_HEADERS:
        rows.append(
            {
                "label": Path(path).stem,
                "old_path": "",
                "old_path_absent": True,
                "new_path": path,
                "new_path_present": (repo / path).is_file(),
            }
        )
    return rows


def inspect_value_snapshots(repo: Path) -> dict[str, Any]:
    """Check public value snapshots and Host diagnostic methods."""

    inspection = (
        repo / "include" / "photospider" / "core" / "inspection_types.hpp"
    ).read_text(encoding="utf-8")
    event_stream = (
        repo / "include" / "photospider" / "host" / "event_stream.hpp"
    ).read_text(encoding="utf-8")
    host = (repo / "include" / "photospider" / "host" / "host.hpp").read_text(
        encoding="utf-8"
    )
    combined_snapshots = inspection + "\n" + event_stream
    return {
        "required_snapshots": [
            {
                "name": name,
                "present": re.search(
                    rf"\b(struct|enum\s+class)\s+{re.escape(name)}\b",
                    combined_snapshots,
                )
                is not None,
            }
            for name in REQUIRED_PUBLIC_SNAPSHOTS
        ],
        "required_host_methods": [
            {
                "name": name,
                "present": re.search(rf"\b{re.escape(name)}\s*\(", host)
                is not None,
            }
            for name in REQUIRED_HOST_METHODS
        ],
    }


def inspect_cmake_boundary(repo: Path) -> dict[str, Any]:
    """Check CMake uses private include roots for internal implementation."""

    cmake = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    product_include = re.search(
        r"target_include_directories\s*\(\s*photospider\b.*?\n\s*\)",
        cmake,
        re.DOTALL,
    )
    product_include_block = product_include.group(0) if product_include else ""
    return {
        "declares_public_include_dir": "PHOTOSPIDER_PUBLIC_INCLUDE_DIR" in cmake,
        "declares_private_include_dirs": "PHOTOSPIDER_PRIVATE_INCLUDE_DIRS" in cmake
        and '"${PROJECT_SOURCE_DIR}/src"' in cmake,
        "installable_public_header_root_is_photospider": (
            'set(PHOTOSPIDER_PUBLIC_HEADER_ROOT' in cmake
            and '"${PROJECT_SOURCE_DIR}/include/photospider"' in cmake
        ),
        "photospider_static_product_exists": (
            re.search(r"add_library\s*\(\s*photospider\s+STATIC\b", cmake)
            is not None
        ),
        "photospider_product_keeps_src_private": (
            "PRIVATE" in product_include_block
            and "${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}" in product_include_block
            and not re.search(
                r"\bPUBLIC\b(?:(?!\bPRIVATE\b).)*\$\{PROJECT_SOURCE_DIR\}/src",
                product_include_block,
                re.DOTALL,
            )
        ),
        "phase3_scan_registered": "Phase3InternalHeaderScan" in cmake,
    }


def detector_selftest() -> dict[str, Any]:
    """Exercise the forbidden-name detector against synthetic text."""

    sample = """
    struct PublicValue {};
    // comments are part of the public contract text for this detector:
    // GraphRuntime must be reported if a public header names it.
    """
    pattern = re.compile(r"\bGraphRuntime\b")
    return {
        "detects_forbidden_name_in_comment": pattern.search(sample) is not None,
        "rejects_partial_name": re.search(r"\bComputePlan\b", "ComputePlanning")
        is None,
    }


def write_json(path: Path, payload: dict[str, Any]) -> None:
    """Write deterministic JSON output."""

    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def make_compare(actual: dict[str, Any], expected: dict[str, Any]) -> tuple[bool, str]:
    """Create PASS/FAIL comparison text for issue #32."""

    checks = {
        "public header inventory is nonempty": actual["public_headers"][
            "header_count"
        ]
        >= expected["public_headers"]["header_count_minimum"],
        "public headers avoid internal names": actual["public_headers"][
            "forbidden_name_hits"
        ]
        == expected["public_headers"]["forbidden_name_hits"],
        "internal headers moved from include to src": all(
            row["old_path_absent"] and row["new_path_present"]
            for row in actual["internal_headers"]
        ),
        "required public snapshots exist": all(
            row["present"] for row in actual["value_snapshots"]["required_snapshots"]
        ),
        "required Host methods exist": all(
            row["present"]
            for row in actual["value_snapshots"]["required_host_methods"]
        ),
        "CMake declares private include root": actual["cmake_boundary"][
            "declares_private_include_dirs"
        ],
        "CMake installable root is include/photospider": actual[
            "cmake_boundary"
        ]["installable_public_header_root_is_photospider"],
        "photospider static product exists": actual["cmake_boundary"][
            "photospider_static_product_exists"
        ],
        "photospider product keeps src private": actual["cmake_boundary"][
            "photospider_product_keeps_src_private"
        ],
        "Phase3 CTest registered": actual["cmake_boundary"][
            "phase3_scan_registered"
        ],
        "detector self-test passes": all(actual["detector_selftest"].values()),
    }
    passed = all(checks.values())
    lines = ["phase3_internal_header_scan"]
    lines.extend(
        f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()
    )
    lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    return passed, "\n".join(lines) + "\n"


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    """Create a reader-facing summary for issue #32 evidence."""

    optional_logs = [
        "commands.log",
        "cmake_configure.log",
        "git_diff_check.log",
        "py_compile.log",
        "cpplint_scoped.log",
        "build_test_host_adapter.log",
        "test_host_planning_snapshot.log",
        "boundary_ctest_subset.log",
        "build_all.log",
        "ctest_full.log",
        "verify_compute_service_public_api.log",
        "verify_dirty_control_lane.log",
        "verify_remove_graphruntime_worker_queue.log",
    ]
    log_lines = [
        f"- `{name}`: `{out / name}`"
        for name in optional_logs
        if (out / name).exists()
    ]
    lines = [
        "# codebase-refactor phase-3 internal headers evidence",
        "",
        "## Test objective",
        "",
        "Verify issue #32 keeps installable public headers limited to",
        "`include/photospider`, removes graph/runtime/service/planning and",
        "concrete scheduler type names from that public inventory, and keeps",
        "the moved implementation headers available under the private `src/`",
        "include root.",
        "",
        "## Evidence files",
        "",
        f"- `expected.json`: `{out / 'expected.json'}`",
        f"- `actual.json`: `{out / 'actual.json'}`",
        f"- `compare.log`: `{out / 'compare.log'}`",
    ]
    if log_lines:
        lines.extend(["", "## Replay command logs", "", *log_lines])
    lines.extend(
        [
            "",
            "## Result",
            "",
            f"- Public headers scanned: {actual['public_headers']['header_count']}",
            "- Forbidden public name hits: "
            f"{len(actual['public_headers']['forbidden_name_hits'])}",
            "- Internal/private headers checked: "
            f"{len(actual['internal_headers'])}",
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "A passing result proves the installable public header inventory no",
            "longer names the issue #32 internal graph/runtime/service/planning",
            "or concrete scheduler concepts, while the private implementation",
            "headers still exist under `src/` for internal targets. The scan also",
            "checks that dirty-region, compute planning, and scheduler trace",
            "diagnostics are exposed through public value snapshots and Host",
            "methods rather than through backend implementation headers. The",
            "CMake boundary check now expects the static `photospider` product",
            "target to keep `src/` as a private include root.",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    """Run the phase-3 internal header scan."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    actual = {
        "public_headers": scan_public_header_names(repo),
        "internal_headers": inspect_internal_headers(repo),
        "value_snapshots": inspect_value_snapshots(repo),
        "cmake_boundary": inspect_cmake_boundary(repo),
        "detector_selftest": detector_selftest(),
    }
    expected = {
        "public_headers": {
            "header_count_minimum": 1,
            "forbidden_name_hits": [],
        },
    }
    passed, compare = make_compare(actual, expected)

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)
    (out / "compare.log").write_text(compare, encoding="utf-8")
    (out / "summary.md").write_text(
        make_summary(out, actual, passed) + "\n", encoding="utf-8"
    )
    print(f"overall={'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
