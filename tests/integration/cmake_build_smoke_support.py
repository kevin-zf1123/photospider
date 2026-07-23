#!/usr/bin/env python3
"""Shared CMake metadata and destructive-path helpers.

The helpers are imported by repository-owned build-smoke drivers.
"""

from __future__ import annotations

import os
import platform
import shutil
import stat
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

#: @brief Logical Darwin temporary root accepted only as a system-owned alias.
#: @note The exact spelling is part of the narrow macOS compatibility contract.
_DARWIN_SYSTEM_TMP_ALIAS = Path("/tmp")
#: @brief Physical Darwin temporary root required for the trusted alias.
#: @note No other target, alias, or user-selected path receives special trust.
_DARWIN_PHYSICAL_TMP = Path("/private/tmp")


def _is_trusted_darwin_tmp_alias(
    *,
    system_name: str,
    alias_mode: int,
    alias_uid: int,
    resolved_alias: Path,
    physical_mode: int,
    physical_uid: int,
) -> bool:
    """@brief Evaluate immutable facts for the one trusted Darwin tmp alias.

    @param system_name Exact host platform name.
    @param alias_mode Resulting ``st_mode`` from ``lstat("/tmp")``.
    @param alias_uid Resulting ``st_uid`` from ``lstat("/tmp")``.
    @param resolved_alias Strict canonical result for ``/tmp``.
    @param physical_mode Resulting ``st_mode`` from
      ``lstat("/private/tmp")``.
    @param physical_uid Resulting ``st_uid`` from
      ``lstat("/private/tmp")``.
    @return True only for Darwin's root-owned ``/tmp`` symlink resolving
      exactly to a root-owned physical ``/private/tmp`` directory.
    @throws Nothing; the predicate reads only caller-supplied scalar facts.
    @note Keeping this predicate pure permits deterministic cross-platform
      regression coverage without creating or replacing a system path.
    """

    return (
        system_name == "Darwin"
        and stat.S_ISLNK(alias_mode)
        and alias_uid == 0
        and resolved_alias == _DARWIN_PHYSICAL_TMP
        and stat.S_ISDIR(physical_mode)
        and not stat.S_ISLNK(physical_mode)
        and physical_uid == 0
    )


def _trusted_system_tmp_mapping(
    *, system_name: str | None = None
) -> tuple[Path, Path] | None:
    """@brief Inspect and return the one trusted system temporary alias.

    @param system_name Optional platform override. ``None`` queries
      ``platform.system()``; tests may supply a non-Darwin value to prove the
      ordinary-platform path.
    @return ``(/tmp, /private/tmp)`` only when every trusted Darwin fact
      matches, otherwise ``None``.
    @throws OSError If Darwin system paths cannot be inspected or strictly
      resolved.
    @throws RuntimeError If strict canonical resolution encounters a symlink
      loop.
    @note Linux and other platforms return before inspecting either path.
      A Darwin near miss receives no fallback trust and is handled by the
      ordinary component-by-component symlink rejection.
    """

    active_system = platform.system() if system_name is None else system_name
    if active_system != "Darwin":
        return None

    alias_metadata = _DARWIN_SYSTEM_TMP_ALIAS.lstat()
    resolved_alias = _DARWIN_SYSTEM_TMP_ALIAS.resolve(strict=True)
    physical_metadata = _DARWIN_PHYSICAL_TMP.lstat()
    if not _is_trusted_darwin_tmp_alias(
        system_name=active_system,
        alias_mode=alias_metadata.st_mode,
        alias_uid=alias_metadata.st_uid,
        resolved_alias=resolved_alias,
        physical_mode=physical_metadata.st_mode,
        physical_uid=physical_metadata.st_uid,
    ):
        return None
    return (_DARWIN_SYSTEM_TMP_ALIAS, _DARWIN_PHYSICAL_TMP)


def _rewrite_trusted_system_alias_prefix(
    absolute_work: Path,
    mapping: tuple[Path, Path] | None,
) -> Path:
    """@brief Replace a trusted logical-root prefix with its physical root.

    @param absolute_work Absolute caller spelling already checked for ``..``.
    @param mapping Optional trusted logical/physical root pair.
    @return The original spelling when it is outside the mapping, otherwise
      the same suffix rooted below the physical path.
    @throws RuntimeError If an injected or internally produced mapping is not
      absolute, names a filesystem root, or permits lexical escape.
    @note This function does not discover trust. Production mappings come only
      from ``_trusted_system_tmp_mapping``; direct injection is reserved for
      deterministic tests.
    """

    if mapping is None:
        return absolute_work
    logical_root, physical_root = mapping
    if (
        not logical_root.is_absolute()
        or not physical_root.is_absolute()
        or logical_root.parent == logical_root
        or physical_root.parent == physical_root
    ):
        raise RuntimeError("trusted system temporary mapping is invalid")
    try:
        relative_work = absolute_work.relative_to(logical_root)
    except ValueError:
        return absolute_work
    if os.pardir in relative_work.parts:
        raise RuntimeError("trusted system temporary mapping escaped its root")
    rewritten_work = physical_root / relative_work
    if (
        rewritten_work != physical_root
        and physical_root not in rewritten_work.parents
    ):
        raise RuntimeError("trusted system temporary mapping escaped its root")
    return rewritten_work


def resolve_work_directory(work: Path, repo: Path) -> Path:
    """@brief Validate a destructive nested-build directory and normalize only
    Darwin's trusted system temporary alias.

    @param work Caller-selected absolute directory whose prior tree may be
      removed.
    @param repo Photospider repository root that must remain untouched.
    @return Absolute work spelling after every guard passes. A trusted Darwin
      ``/tmp`` prefix is returned under physical ``/private/tmp``; no other
      symlink target replaces caller spelling.
    @throws OSError If the repository, trusted system alias, or an existing
      work component cannot be inspected.
    @throws RuntimeError If canonical resolution encounters a symlink loop or
      the trusted mapping violates its fixed subtree boundary.
    @throws ValueError If work is empty/relative, contains parent traversal,
      names a trusted system temporary root, resolves to the repository, a
      repository ancestor, or a filesystem root, or contains any untrusted
      existing symlink component including its leaf.
    @note Canonical resolution is used only for protected-location comparison.
      Root-to-leaf ``lstat`` remains the final validation step. The returned
      path is suitable only for caller-owned transient content whose
      components cannot be concurrently replaced by an untrusted actor.
    """

    if not work.is_absolute():
        raise ValueError(
            f"refusing non-absolute destructive work path: {work}"
        )
    absolute_work = work
    if os.pardir in absolute_work.parts:
        raise ValueError(
            "refusing parent traversal in destructive work path: "
            f"{absolute_work}"
        )

    trusted_mapping = _trusted_system_tmp_mapping()
    normalized_work = _rewrite_trusted_system_alias_prefix(
        absolute_work, trusted_mapping
    )
    if trusted_mapping is not None:
        logical_root, physical_root = trusted_mapping
        if absolute_work in (logical_root, physical_root):
            raise ValueError(
                "refusing to remove trusted system temporary root: "
                f"{absolute_work}"
            )

    resolved_repo = repo.resolve(strict=True)
    comparison_work = normalized_work.resolve()
    if comparison_work.parent == comparison_work:
        raise ValueError(
            f"refusing to remove filesystem root: {comparison_work}"
        )
    if (
        comparison_work == resolved_repo
        or comparison_work in resolved_repo.parents
    ):
        raise ValueError(
            "refusing to remove repository or ancestor as work path: "
            f"{comparison_work}"
        )

    components = (*reversed(normalized_work.parents), normalized_work)
    for component in components:
        try:
            metadata = component.lstat()
        except FileNotFoundError:
            break
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError(
                "refusing symlink component in destructive work path: "
                f"{component}"
            )
    return normalized_work


def remove_work_tree(work: Path, repo: Path) -> Path:
    """@brief Remove one validated nested-build tree without hiding failure.

    @param work Caller-selected absolute directory whose previous contents
      must vanish.
    @param repo Photospider repository root protected from recursive removal.
    @return Validated physical work directory, absent when this function
      returns.
    @throws OSError If path inspection, resolution, or recursive removal fails.
    @throws ValueError If the work path violates any destructive-path guard.
    @throws RuntimeError If canonical resolution cannot complete, the trusted
      mapping escapes, or recursive removal returns while the tree remains.
    @note Validation is repeated immediately before ``shutil.rmtree`` to narrow
      the check/delete replacement window. That sequence is not an atomic
      cross-platform filesystem transaction, so callers must exclusively own
      the transient subtree. The trusted root alias is removed from the path
      before deletion; every remaining symlink component and symlink leaf is
      rejected. The helper never creates the returned directory.
    """

    validated_work = resolve_work_directory(work, repo)
    try:
        validated_work.lstat()
    except FileNotFoundError:
        return validated_work

    validated_work = resolve_work_directory(validated_work, repo)
    shutil.rmtree(validated_work)
    try:
        validated_work.lstat()
    except FileNotFoundError:
        return validated_work
    else:
        raise RuntimeError(
            f"nested build directory still exists: {validated_work}"
        )


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
