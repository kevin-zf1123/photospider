#!/usr/bin/env python3
"""Shared CMake producer metadata helpers for build-smoke drivers."""

from __future__ import annotations

import platform
from pathlib import Path


#: @brief Module-owned lookup set for exact CMake false cache serializations.
#: @note Values are compared only after whitespace trimming and uppercasing.
#:   The set lives for the importing process and excludes suffixed
#:   ``-NOTFOUND`` values, which the propagation policy handles separately.
_CMAKE_FALSE_VALUES = {
    "",
    "0",
    "FALSE",
    "IGNORE",
    "N",
    "NO",
    "NOTFOUND",
    "OFF",
}


def cmake_cache_value(build: Path, key: str) -> str:
    """@brief Read one exact value from a producer CMake cache.

    @param build Producer build directory that may contain CMakeCache.txt.
    @param key Cache variable name before the CMake type separator.
    @return Serialized value after ``=`` or an empty string when the cache or
      key is absent.
    @throws OSError If an existing cache file cannot be read.
    @note The helper is read-only, ignores comments and malformed assignments,
      and returns the final matching assignment to mirror CMake's effective
      cache value.
    """

    cache_path = build / "CMakeCache.txt"
    if not cache_path.is_file():
        return ""
    prefix = f"{key}:"
    value = ""
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line.startswith(prefix) or "=" not in line:
            continue
        value = line.split("=", 1)[1]
    return value


def producer_osx_architecture_arguments(
    build: Path, *, system_name: str | None = None
) -> tuple[str, ...]:
    """@brief Derive child configure argv from producer macOS architecture.

    @param build Configured producer build whose cache owns the architecture.
    @param system_name Optional host platform name. ``None`` queries
      ``platform.system()``; an explicit value supports deterministic tests.
    @return An empty tuple when propagation is inapplicable, otherwise one
      ``-DCMAKE_OSX_ARCHITECTURES=<value>`` argv element.
    @throws OSError If an existing producer cache cannot be read.
    @note Propagation is Darwin-only and rejects CMake false/NOTFOUND values.
      A semicolon-separated multi-architecture list remains one argv element;
      callers pass it through ``subprocess`` without a shell. Linux and Windows
      never receive this macOS-specific cache option.
    """

    active_system = platform.system() if system_name is None else system_name
    if active_system != "Darwin":
        return ()
    architectures = cmake_cache_value(
        build, "CMAKE_OSX_ARCHITECTURES"
    ).strip()
    normalized = architectures.upper()
    if (
        normalized in _CMAKE_FALSE_VALUES
        or normalized.endswith("-NOTFOUND")
    ):
        return ()
    return (f"-DCMAKE_OSX_ARCHITECTURES={architectures}",)
