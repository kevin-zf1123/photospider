#!/usr/bin/env python3
import argparse
import json
import os
import re
from pathlib import Path


CACHE_NAME = "cached" + "_output"
FORBIDDEN = re.compile(
    rf"(?<![A-Za-z0-9_:]){re.escape(CACHE_NAME)}(?![A-Za-z0-9_])|"
    rf"Node::{re.escape(CACHE_NAME)}(?![A-Za-z0-9_])"
)

EXCLUDED_DIRS = {
    ".git",
    ".git-personal",
    "build",
    "cache",
    "extern",
    "out",
}

TEXT_SUFFIXES = {
    ".cpp",
    ".hpp",
    ".h",
    ".mm",
    ".md",
    ".txt",
    ".py",
    ".yaml",
    ".yml",
    ".json",
    ".sh",
    ".cmake",
}


def is_excluded(path: Path) -> bool:
    parts = set(path.parts)
    if parts & EXCLUDED_DIRS:
        return True
    return path.parts[:2] == ("tests", "results")


def is_text_candidate(path: Path) -> bool:
    return path.name in {"CMakeLists.txt", "Dockerfile.ci"} or path.suffix in TEXT_SUFFIXES


def scan(root: Path) -> tuple[list[dict], list[dict]]:
    violations: list[dict] = []
    outdated_notes: list[dict] = []
    for dirpath, dirnames, filenames in os.walk(root):
      current = Path(dirpath).relative_to(root)
      dirnames[:] = [
          d for d in dirnames if not is_excluded(current / d)
      ]
      for filename in filenames:
        rel = current / filename
        if is_excluded(rel) or not is_text_candidate(rel):
          continue
        full_path = root / rel
        try:
          text = full_path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
          try:
            text = full_path.read_text(encoding="utf-8", errors="ignore")
          except OSError:
            continue
        except OSError:
          continue
        for line_no, line in enumerate(text.splitlines(), 1):
          if not FORBIDDEN.search(line):
            continue
          entry = {"file": str(rel), "line": line_no, "text": line.strip()}
          if rel.parts[:2] == ("docs", "outdated"):
            outdated_notes.append(entry)
          else:
            violations.append(entry)
    return violations, outdated_notes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--json", dest="json_path")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    violations, outdated_notes = scan(root)
    result = {
        "root": str(root),
        "violations": violations,
        "outdated_notes": outdated_notes,
        "pass": not violations,
    }
    if args.json_path:
        Path(args.json_path).write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if not violations else 1


if __name__ == "__main__":
    raise SystemExit(main())
