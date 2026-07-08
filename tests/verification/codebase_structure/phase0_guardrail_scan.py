#!/usr/bin/env python3
"""Verify phase-0 codebase-structure public header guardrails."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path
from typing import Any


PUBLIC_HEADER_GLOBS = ("*.h", "*.hpp", "*.hh", "*.hxx")
FORBIDDEN_INCLUDE_PREFIXES = (
    "src/",
    "kernel/services/",
)
IMPLEMENTATION_ONLY_TYPES = (
    "GraphModel",
    "GraphRuntime",
    "GraphStateExecutor",
    "ComputeService",
    "DirtyControlLane",
    "ComputePlan",
    "FullTaskGraph",
    "CpuWorkStealingScheduler",
    "GpuPipelineScheduler",
    "SerialDebugScheduler",
    "GraphCacheService",
    "GraphTraversalService",
    "GraphIoService",
    "GraphInspectService",
    "RoiPropagationService",
)
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]")
TYPE_RE_TEMPLATE = r"\b{}\b"
DOC_PATHS = (
    "docs/codebase-structure/Codebase-Structure-Direction.md",
    "docs/codebase-structure/zh/Codebase-Structure-Direction.zh.md",
)


def run_git(repo: Path, args: list[str]) -> tuple[int, str]:
    proc = subprocess.run(
        ["git", *args],
        cwd=repo,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    return proc.returncode, proc.stdout


def run_personal_git(repo: Path, args: list[str]) -> tuple[int, str]:
    proc = subprocess.run(
        ["git", "--git-dir=.git-personal", "--work-tree=.", *args],
        cwd=repo,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    return proc.returncode, proc.stdout


def git_tracks(repo: Path, path: str) -> bool:
    code, output = run_git(repo, ["ls-files", "--", path])
    return code == 0 and path in output.splitlines()


def personal_tracks(repo: Path, path: str) -> bool | None:
    if not (repo / ".git-personal").exists():
        return None
    code, output = run_personal_git(repo, ["ls-files", "--", path])
    return code == 0 and path in output.splitlines()


def public_headers(repo: Path) -> list[Path]:
    public_root = repo / "include" / "photospider"
    headers: list[Path] = []
    if not public_root.exists():
        return headers
    for pattern in PUBLIC_HEADER_GLOBS:
        headers.extend(public_root.rglob(pattern))
    return sorted(path for path in headers if path.is_file())


def relative_to_repo(repo: Path, path: Path) -> str:
    return path.relative_to(repo).as_posix()


def normalize_include(include: str) -> str:
    return include.replace("\\", "/")


def include_violations(header: str, text: str) -> list[dict[str, Any]]:
    violations: list[dict[str, Any]] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        include = normalize_include(match.group(1))
        for prefix in FORBIDDEN_INCLUDE_PREFIXES:
            if include == prefix.rstrip("/") or include.startswith(prefix):
                violations.append(
                    {
                        "file": header,
                        "line": line_number,
                        "include": include,
                        "reason": f"forbidden include prefix `{prefix}`",
                    }
                )
        if "/src/" in include or include.startswith("../src/"):
            violations.append(
                {
                    "file": header,
                    "line": line_number,
                    "include": include,
                    "reason": "forbidden implementation source include",
                }
            )
    return violations


def implementation_type_occurrences(
    header: str, text: str
) -> list[dict[str, Any]]:
    occurrences: list[dict[str, Any]] = []
    patterns = [
        (name, re.compile(TYPE_RE_TEMPLATE.format(re.escape(name))))
        for name in IMPLEMENTATION_ONLY_TYPES
    ]
    in_block_comment = False
    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line
        if in_block_comment:
            if "*/" in stripped:
                stripped = stripped.split("*/", 1)[1]
                in_block_comment = False
            else:
                continue
        while "/*" in stripped:
            before, after = stripped.split("/*", 1)
            if "*/" in after:
                stripped = before + after.split("*/", 1)[1]
            else:
                stripped = before
                in_block_comment = True
                break
        stripped = stripped.split("//", 1)[0]
        if not stripped.strip():
            continue
        for name, pattern in patterns:
            if pattern.search(stripped):
                occurrences.append(
                    {
                        "file": header,
                        "line": line_number,
                        "type": name,
                        "text": line.strip(),
                    }
                )
    return occurrences


def scan_headers(repo: Path) -> dict[str, Any]:
    headers = public_headers(repo)
    include_hits: list[dict[str, Any]] = []
    type_hits: list[dict[str, Any]] = []
    header_rows: list[str] = []
    for path in headers:
        header = relative_to_repo(repo, path)
        header_rows.append(header)
        text = path.read_text(encoding="utf-8")
        include_hits.extend(include_violations(header, text))
        type_hits.extend(implementation_type_occurrences(header, text))
    return {
        "headers": header_rows,
        "header_count": len(header_rows),
        "include_violations": include_hits,
        "implementation_type_occurrences": type_hits,
    }


def detector_selftest() -> dict[str, Any]:
    include_sample = "\n".join(
        [
            '#include "src/private.hpp"',
            '#include <kernel/services/compute_service.hpp>',
            "namespace ps { class Public; }",
        ]
    )
    type_sample = "namespace ps { GraphRuntime* runtime; }"
    include_hits = include_violations("synthetic/include.hpp", include_sample)
    type_hits = implementation_type_occurrences(
        "synthetic/type.hpp", type_sample
    )
    return {
        "detects_src_include": any(
            hit["include"] == "src/private.hpp" for hit in include_hits
        ),
        "detects_kernel_services_include": any(
            hit["include"] == "kernel/services/compute_service.hpp"
            for hit in include_hits
        ),
        "detects_implementation_type": any(
            hit["type"] == "GraphRuntime" for hit in type_hits
        ),
        "sample_include_violations": include_hits,
        "sample_type_occurrences": type_hits,
    }


def document_ownership(repo: Path) -> dict[str, Any]:
    docs = []
    for path in DOC_PATHS:
        personal = personal_tracks(repo, path)
        docs.append(
            {
                "path": path,
                "exists": (repo / path).exists(),
                "tracked_in_primary": git_tracks(repo, path),
                "tracked_in_personal_overlay": personal,
                "not_tracked_in_personal_overlay_when_present": personal is not True,
            }
        )
    return {"docs": docs}


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    header_count = actual["public_headers"]["header_count"]
    docs = actual["document_ownership"]["docs"]
    return "\n".join(
        [
            "# codebase-refactor phase-0 guardrail evidence",
            "",
            "## Test objective",
            "",
            "Verify the phase-0 public header boundary guardrails for issue #27.",
            "The check covers public-header inventory, forbidden include detection,",
            "implementation-only type detection, detector negative samples, and",
            "codebase-structure document ownership.",
            "",
            "## Evidence files",
            "",
            f"- `expected.json`: `{out / 'expected.json'}`",
            f"- `actual.json`: `{out / 'actual.json'}`",
            f"- `compare.log`: `{out / 'compare.log'}`",
            "",
            "## Result",
            "",
            f"- Public headers scanned: {header_count}",
            f"- English design doc tracked in primary repo: {docs[0]['tracked_in_primary']}",
            f"- Chinese mirror tracked in primary repo: {docs[1]['tracked_in_primary']}",
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "The scan proves the installable `include/photospider/` header set is",
            "non-empty, contains no forbidden `src/` or `kernel/services/...`",
            "dependencies, and does not name implementation-only graph/runtime/compute",
            "types. The detector self-test proves the same scanner reports the three",
            "violation classes required by issue #27.",
            "",
            "The document ownership checks prove the English design document and its",
            "Chinese mirror are tracked by the primary repository, not only by the",
            "local personal overlay.",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    actual = {
        "public_headers": scan_headers(repo),
        "detector_selftest": detector_selftest(),
        "document_ownership": document_ownership(repo),
    }
    expected = {
        "public_headers": {
            "header_count_minimum": 1,
            "include_violations": [],
            "implementation_type_occurrences": [],
        },
        "detector_selftest": {
            "detects_src_include": True,
            "detects_kernel_services_include": True,
            "detects_implementation_type": True,
        },
        "document_ownership": {
            "all_docs_exist": True,
            "all_docs_tracked_in_primary": True,
            "no_docs_tracked_in_personal_overlay_when_present": True,
        },
    }

    checks = [
        (
            "public_headers.header_count",
            actual["public_headers"]["header_count"]
            >= expected["public_headers"]["header_count_minimum"],
        ),
        (
            "public_headers.include_violations",
            actual["public_headers"]["include_violations"]
            == expected["public_headers"]["include_violations"],
        ),
        (
            "public_headers.implementation_type_occurrences",
            actual["public_headers"]["implementation_type_occurrences"]
            == expected["public_headers"]["implementation_type_occurrences"],
        ),
        (
            "detector_selftest.detects_src_include",
            actual["detector_selftest"]["detects_src_include"]
            == expected["detector_selftest"]["detects_src_include"],
        ),
        (
            "detector_selftest.detects_kernel_services_include",
            actual["detector_selftest"]["detects_kernel_services_include"]
            == expected["detector_selftest"]["detects_kernel_services_include"],
        ),
        (
            "detector_selftest.detects_implementation_type",
            actual["detector_selftest"]["detects_implementation_type"]
            == expected["detector_selftest"]["detects_implementation_type"],
        ),
        (
            "document_ownership.all_docs_exist",
            all(row["exists"] for row in actual["document_ownership"]["docs"])
            == expected["document_ownership"]["all_docs_exist"],
        ),
        (
            "document_ownership.all_docs_tracked_in_primary",
            all(
                row["tracked_in_primary"]
                for row in actual["document_ownership"]["docs"]
            )
            == expected["document_ownership"]["all_docs_tracked_in_primary"],
        ),
        (
            "document_ownership.no_docs_tracked_in_personal_overlay_when_present",
            all(
                row["not_tracked_in_personal_overlay_when_present"]
                for row in actual["document_ownership"]["docs"]
            )
            == expected["document_ownership"][
                "no_docs_tracked_in_personal_overlay_when_present"
            ],
        ),
    ]

    passed = all(ok for _, ok in checks)
    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)
    compare_lines = [f"{name}: {'PASS' if ok else 'FAIL'}" for name, ok in checks]
    compare_lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    (out / "compare.log").write_text(
        "\n".join(compare_lines) + "\n", encoding="utf-8"
    )
    (out / "summary.md").write_text(
        make_summary(out, actual, passed) + "\n", encoding="utf-8"
    )
    print(f"overall={'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
