#!/usr/bin/env python3
"""Verify public legacy facades are removed and internal facades stay closed."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any, Optional


PUBLIC_LEGACY_HEADERS = (
    "include/kernel/kernel.hpp",
    "include/kernel/interaction.hpp",
)
INTERNAL_SCAN_HEADERS = (
    "src/kernel/kernel.hpp",
    "src/kernel/interaction.hpp",
)
SCAN_CLASSES = ("Kernel", "InteractionService")
RAW_INTERNAL_TYPES = (
    "Kernel",
    "GraphRuntime",
    "GraphStateExecutor",
    "GraphModel",
)
RAW_INTERNAL_TYPE_RE = r"(?:ps::)?(?:" + "|".join(RAW_INTERNAL_TYPES) + r")"
RAW_INTERNAL_RETURN_RE = (
    rf"(?:const\s+)?{RAW_INTERNAL_TYPE_RE}(?:\s+const)?"
)
FORBIDDEN_PATTERNS = (
    (
        "direct raw internal return",
        re.compile(
            rf"\b{RAW_INTERNAL_RETURN_RE}\s*(?:[&*]\s*)+\w+\s*\(",
            re.MULTILINE,
        ),
    ),
    (
        "trailing raw internal return",
        re.compile(
            rf"\bauto\s+\w+\s*\([^;{{}}]*\)\s*(?:const\s*)?"
            rf"(?:noexcept\s*)?->\s*{RAW_INTERNAL_RETURN_RE}\s*(?:[&*]\s*)+",
            re.DOTALL,
        ),
    ),
    (
        "Kernel::post()",
        re.compile(r"\bpost\s*\("),
    ),
)
TEMPLATE_POST_RE = re.compile(
    r"template\s*<[^>]*>\s*(?:\n|.){0,400}?\bpost\s*\(",
    re.MULTILINE,
)


def strip_comments(text: str) -> str:
    """Return source text with C and C++ comments removed."""

    def replace_block(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    without_blocks = re.sub(r"/\*.*?\*/", replace_block, text, flags=re.DOTALL)
    return "\n".join(
        line.split("//", 1)[0] for line in without_blocks.splitlines()
    )


def rel(repo: Path, path: Path) -> str:
    """Return a repository-relative path for report output."""

    return path.relative_to(repo).as_posix()


def line_number_for_offset(text: str, offset: int) -> int:
    """Convert a string offset to a one-based line number."""

    return text.count("\n", 0, offset) + 1


def find_class_body(code: str, class_name: str) -> Optional[tuple[str, int]]:
    """Return one class body and its offset within the full source text."""

    class_match = re.search(rf"\bclass\s+{re.escape(class_name)}\b", code)
    if not class_match:
        return None

    brace = code.find("{", class_match.end())
    semicolon = code.find(";", class_match.end())
    if brace < 0 or (semicolon >= 0 and semicolon < brace):
        return None

    depth = 0
    for index in range(brace, len(code)):
        char = code[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return code[brace + 1 : index], brace + 1
    return None


def public_class_chunks(body: str, body_offset: int) -> list[tuple[str, int]]:
    """Return source ranges for explicit public sections of a C++ class body."""

    chunks: list[tuple[str, int]] = []
    access = "private"
    last = 0
    for match in re.finditer(r"\b(public|private|protected)\s*:", body):
        if access == "public":
            chunks.append((body[last : match.start()], body_offset + last))
        access = match.group(1)
        last = match.end()
    if access == "public":
        chunks.append((body[last:], body_offset + last))
    return chunks


def scan_header(repo: Path, path: Path) -> list[dict[str, Any]]:
    """Scan one internal facade header for forbidden escape-hatch signatures."""

    raw = path.read_text(encoding="utf-8")
    code = strip_comments(raw)
    violations: list[dict[str, Any]] = []
    header = rel(repo, path)

    for class_name in SCAN_CLASSES:
        class_body = find_class_body(code, class_name)
        if class_body is None:
            continue
        for chunk, chunk_offset in public_class_chunks(*class_body):
            for label, pattern in FORBIDDEN_PATTERNS:
                for match in pattern.finditer(chunk):
                    violations.append(
                        {
                            "file": header,
                            "line": line_number_for_offset(
                                code, chunk_offset + match.start()
                            ),
                            "class": class_name,
                            "pattern": label,
                            "text": " ".join(match.group(0).split()),
                        }
                    )

            for match in TEMPLATE_POST_RE.finditer(chunk):
                violations.append(
                    {
                        "file": header,
                        "line": line_number_for_offset(
                            code, chunk_offset + match.start()
                        ),
                        "class": class_name,
                        "pattern": "templated graph-state post()",
                        "text": " ".join(match.group(0).split()),
                    }
                )

    return violations


def detector_selftest() -> dict[str, Any]:
    """Exercise every detector against synthetic positive and negative input."""

    synthetic = """
    class InteractionService {
     public:
      Kernel& kernel();
      const ps::Kernel* kernel() const;
      Kernel const& raw_kernel_east();
      auto raw_kernel() -> Kernel&;
      auto raw_kernel_east() -> Kernel const&;
     private:
      Kernel& private_kernel();
    };
    class Kernel {
     public:
      GraphRuntime& runtime(const std::string& name);
      const ps::GraphRuntime*
      runtime(const std::string& name) const;
      GraphRuntime& graph_runtime(const std::string& name);
      GraphRuntime const& graph_runtime_east(const std::string& name);
      GraphStateExecutor& graph_state(const std::string& name);
      GraphModel* mutable_model(const std::string& name);
      auto runtime(const std::string& name) -> GraphRuntime&;
      auto runtime_east(const std::string& name) -> GraphRuntime const&;
      template <typename Fn>
      auto
      post(const std::string& name, Fn&& fn);
     private:
      GraphRuntime& private_runtime(const std::string& name);
    };
    // Kernel& kernel();
    """
    clean = strip_comments(synthetic)
    synthetic_violations: list[str] = []
    for class_name in SCAN_CLASSES:
        class_body = find_class_body(clean, class_name)
        if class_body is None:
            continue
        for chunk, _ in public_class_chunks(*class_body):
            for label, pattern in FORBIDDEN_PATTERNS:
                synthetic_violations.extend(
                    label for _ in pattern.finditer(chunk)
                )
            synthetic_violations.extend(
                "templated graph-state post()"
                for _ in TEMPLATE_POST_RE.finditer(chunk)
            )
    return {
        "detects_direct_raw_internal_returns": synthetic_violations.count(
            "direct raw internal return"
        )
        == 9,
        "detects_trailing_raw_internal_returns": synthetic_violations.count(
            "trailing raw internal return"
        )
        == 4,
        "detects_graph_state_post": "Kernel::post()" in synthetic_violations,
        "detects_templated_post": TEMPLATE_POST_RE.search(clean) is not None,
        "detects_templated_post_in_public_section": (
            "templated graph-state post()" in synthetic_violations
        ),
        "ignores_private_accessors": len(synthetic_violations) == 15,
        "ignores_commented_kernel_accessor": clean.count("kernel(") == 4,
    }


def write_json(path: Path, payload: dict[str, Any]) -> None:
    """Write stable JSON for CTest and evidence consumers."""

    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def make_compare(actual: dict[str, Any], expected: dict[str, Any]) -> tuple[bool, str]:
    """Build a compact PASS/FAIL comparison for the scan."""

    selftest = actual["detector_selftest"]
    checks = {
        "legacy public facade headers removed": not actual[
            "public_legacy_headers_present"
        ],
        "internal facade headers scanned": actual["scanned_header_count"]
        == expected["scanned_header_count"],
        "no escape hatch signatures": actual["violation_count"]
        == expected["violation_count"],
        "detector finds direct raw internal returns": selftest[
            "detects_direct_raw_internal_returns"
        ],
        "detector finds trailing raw internal returns": selftest[
            "detects_trailing_raw_internal_returns"
        ],
        "detector finds Kernel::post()": selftest["detects_graph_state_post"],
        "detector finds templated post()": selftest["detects_templated_post"],
        "detector finds public-section templated post()": selftest[
            "detects_templated_post_in_public_section"
        ],
        "detector ignores private accessors": selftest[
            "ignores_private_accessors"
        ],
        "detector ignores commented accessor": selftest[
            "ignores_commented_kernel_accessor"
        ],
    }
    passed = all(checks.values())
    lines = ["public_escape_hatch_scan"]
    lines.extend(
        f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()
    )
    lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    return passed, "\n".join(lines) + "\n"


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    """Create a reader-oriented summary for scan artifacts."""

    return "\n".join(
        [
            "# codebase-refactor phase-3 public escape hatch scan",
            "",
            "## Test objective",
            "",
            "Verify legacy Kernel and InteractionService facade headers no",
            "longer remain under `include/kernel/`, and their internal",
            "`src/kernel/` replacements do not offer raw GraphRuntime,",
            "GraphStateExecutor, GraphModel, or templated graph-state",
            "submission escape hatches.",
            "",
            "## Evidence files",
            "",
            f"- `expected.json`: `{out / 'expected.json'}`",
            f"- `actual.json`: `{out / 'actual.json'}`",
            f"- `compare.log`: `{out / 'compare.log'}`",
            "",
            "## Result",
            "",
            f"- Removed public legacy headers: {actual['public_legacy_headers_absent']}",
            f"- Internal headers scanned: {actual['scanned_headers']}",
            f"- Forbidden signature count: {actual['violation_count']}",
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "A passing result proves the legacy Kernel/InteractionService",
            "facade headers are absent from the public include tree and that",
            "raw internal GraphRuntime, GraphStateExecutor, GraphModel, and",
            "templated graph-state `post()` escape hatches are absent from the",
            "remaining internal facade headers.",
            "Internal tests may still include the `tests/support` helper",
            "explicitly; that helper is intentionally outside this public header",
            "scan.",
        ]
    )


def main() -> int:
    """Run the public escape-hatch scan."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    scanned_headers: list[str] = []
    violations: list[dict[str, Any]] = []
    public_legacy_headers_present = [
        header for header in PUBLIC_LEGACY_HEADERS if (repo / header).exists()
    ]
    for header in INTERNAL_SCAN_HEADERS:
        path = repo / header
        if path.is_file():
            scanned_headers.append(header)
            violations.extend(scan_header(repo, path))

    actual = {
        "public_legacy_headers": list(PUBLIC_LEGACY_HEADERS),
        "public_legacy_headers_present": public_legacy_headers_present,
        "public_legacy_headers_absent": not public_legacy_headers_present,
        "scan_headers": list(INTERNAL_SCAN_HEADERS),
        "scanned_headers": scanned_headers,
        "scanned_header_count": len(scanned_headers),
        "violation_count": len(violations),
        "violations": violations,
        "detector_selftest": detector_selftest(),
    }
    expected = {
        "public_legacy_headers_present": [],
        "scanned_header_count": len(INTERNAL_SCAN_HEADERS),
        "violation_count": 0,
    }
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
