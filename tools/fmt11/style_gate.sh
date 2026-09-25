#!/bin/sh
# Exact review-tool gate. Missing/wrong tools are an error, not a skipped pass.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
: "${CLANG_FORMAT:=clang-format}" "${CPPLINT:=cpplint}"
case $("$CLANG_FORMAT" --version) in
  *"version 21."*) ;;
  *) echo 'FMT-11 review gate requires clang-format major 21.' >&2; exit 2 ;;
esac
case $("$CPPLINT" --version 2>&1) in
  *"2.0.2"*) ;;
  *) echo 'FMT-11 review gate requires cpplint 2.0.2.' >&2; exit 2 ;;
esac
# The manifest contains repository-relative paths with no whitespace.
set -- $(cat tools/fmt11/style_scope.txt)
"$CLANG_FORMAT" --dry-run --Werror "$@"
"$CPPLINT" --extensions=c,cpp,hpp --headers=h,hpp "$@"
