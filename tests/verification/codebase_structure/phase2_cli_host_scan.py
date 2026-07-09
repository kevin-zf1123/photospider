#!/usr/bin/env python3
"""Verify the phase-2 graph_cli-to-Host migration boundary."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


SCAN_ROOTS = (
    "cli/graph_cli.cpp",
    "include/cli",
    "src/cli",
    "include/benchmark",
    "src/benchmark",
)
TEXT_EXTENSIONS = (".cpp", ".hpp", ".h", ".hh", ".hxx", ".cxx", ".cc")
FORBIDDEN_PATTERNS = (
    ("InteractionService", re.compile(r"\bInteractionService\b")),
    ("Kernel::", re.compile(r"\bKernel::")),
    ("svc.kernel()", re.compile(r"\bsvc\s*\.\s*kernel\s*\(")),
    ("GraphModel", re.compile(r"\bGraphModel\b")),
    ("GraphRuntime", re.compile(r"\bGraphRuntime\b")),
    ("kernel include", re.compile(r"^\s*#\s*include\s*[<\"]kernel/")),
    (
        "graph_model.hpp include",
        re.compile(r"^\s*#\s*include\s*[<\"]graph_model\.hpp[>\"]"),
    ),
    ("legacy cmd_* call", re.compile(r"\bcmd_[A-Za-z0-9_]*\b")),
)


def strip_comments(text: str) -> str:
    def replace_block(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    without_blocks = re.sub(r"/\*.*?\*/", replace_block, text, flags=re.DOTALL)
    return "\n".join(line.split("//", 1)[0] for line in without_blocks.splitlines())


def rel(repo: Path, path: Path) -> str:
    try:
        return path.relative_to(repo).as_posix()
    except ValueError:
        return path.as_posix()


def iter_scan_files(repo: Path) -> list[Path]:
    files: list[Path] = []
    for root in SCAN_ROOTS:
        path = repo / root
        if path.is_file():
            files.append(path)
        elif path.is_dir():
            files.extend(
                child
                for child in path.rglob("*")
                if child.is_file() and child.suffix in TEXT_EXTENSIONS
            )
    return sorted(files)


def inspect_cli_boundary(repo: Path) -> dict[str, Any]:
    violations = []
    files = iter_scan_files(repo)
    for path in files:
        code = strip_comments(path.read_text(encoding="utf-8"))
        for line_number, line in enumerate(code.splitlines(), start=1):
            for label, pattern in FORBIDDEN_PATTERNS:
                if pattern.search(line):
                    violations.append(
                        {
                            "file": rel(repo, path),
                            "line": line_number,
                            "pattern": label,
                            "text": line.strip(),
                        }
                    )
    return {
        "scan_roots": list(SCAN_ROOTS),
        "scanned_files": [rel(repo, path) for path in files],
        "violation_count": len(violations),
        "violations": violations,
    }


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def make_compare(actual: dict[str, Any], expected: dict[str, Any]) -> tuple[bool, str]:
    checks = {
        "cli boundary files scanned": bool(actual["scanned_files"])
        == expected["has_scanned_files"],
        "cli boundary avoids kernel implementation references": actual[
            "violation_count"
        ]
        == expected["violation_count"],
    }
    passed = all(checks.values())
    lines = ["phase2_cli_host_scan"]
    lines.extend(
        f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()
    )
    lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    return passed, "\n".join(lines) + "\n"


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    return "\n".join(
        [
            "# codebase-refactor phase-2 CLI Host evidence",
            "",
            "## Test objective",
            "",
            "Verify graph_cli, CLI common, and benchmark frontend helpers use",
            "`ps::Host`/public Host snapshot values instead of directly",
            "including or naming kernel implementation boundaries.",
            "",
            "## Evidence files",
            "",
            f"- `expected.json`: `{out / 'expected.json'}`",
            f"- `actual.json`: `{out / 'actual.json'}`",
            f"- `compare.log`: `{out / 'compare.log'}`",
            "",
            "## Result",
            "",
            f"- Scan roots: {', '.join(actual['scan_roots'])}",
            f"- Files scanned: {len(actual['scanned_files'])}",
            f"- Forbidden reference count: {actual['violation_count']}",
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "A passing result proves the CLI entrypoint and CLI common source",
            "boundary no longer mention InteractionService, Kernel request",
            "types, GraphModel, GraphRuntime, direct kernel includes, or",
            "`graph_model.hpp`, and no longer call legacy `cmd_*` interaction",
            "facade helpers. It does not forbid kernel implementation files",
            "outside the CLI boundary from using those types internally.",
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

    actual = inspect_cli_boundary(repo)
    expected = {"has_scanned_files": True, "violation_count": 0}
    passed, compare = make_compare(actual, expected)

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)
    (out / "compare.log").write_text(compare, encoding="utf-8")
    (out / "summary.md").write_text(
        make_summary(out, actual, passed) + "\n", encoding="utf-8"
    )
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
