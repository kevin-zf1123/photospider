#!/usr/bin/env python3
"""Prepare an independent stock OCIO build; change CMake integration only."""
import argparse
from pathlib import Path
import re
import shutil


def remove_installs(text):
    # Disable packaging of private dependency targets, without touching engines.
    matches = list(re.finditer(r"(?mi)^[ \t]*install\s*\(", text))
    for match in reversed(matches):
        depth, end, quote = 1, match.end(), False
        while depth:
            char = text[end]
            if char == '"' and text[end - 1] != "\\":
                quote = not quote
            elif not quote:
                if char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
            end += 1
        text = text[:match.start()] + text[end:]
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or source in output.parents or output in source.parents:
        parser.error("source and output must be separate directories")
    if not (source / "include/OpenColorIO/OpenColorIO.h").is_file():
        parser.error("source must be the pinned OpenColorIO 2.5.2 extraction")
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(source, output)
    for path in output.rglob("CMakeLists.txt"):
        path.write_text(remove_installs(path.read_text()))
    path = output / "CMakeLists.txt"
    text = path.read_text()
    assert text.count("include(FindExtPackages)") == 1
    path.write_text(text.replace("include(FindExtPackages)", ""))
    path = output / "share/cmake/utils/CppVersion.cmake"
    text = path.read_text()
    target = 'set_property(CACHE CMAKE_CXX_STANDARD PROPERTY STRINGS "' + chr(36) + '{SUPPORTED_CXX_STANDARDS}")'
    assert text.count(target) == 1
    path.write_text(text.replace(target, ""))
    count = 0
    for path in source.rglob("*"):
        if path.is_file() and path.suffix in {".c", ".cpp", ".h", ".hpp", ".inl"}:
            if path.read_bytes() != (output / path.relative_to(source)).read_bytes():
                raise RuntimeError("Reference engine source changed: " + str(path))
            count += 1
    print(f"Independent stock OCIO: {count} C/C++ source/header files unchanged")


if __name__ == "__main__":
    main()
