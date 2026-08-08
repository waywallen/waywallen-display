#!/usr/bin/env bash
# Format maintained C, C++, and Rust sources.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

CHECK=0
if [[ "${1:-}" == "--check" ]]; then
    CHECK=1
elif [[ $# -gt 0 ]]; then
    echo "usage: scripts/format.sh [--check]" >&2
    exit 2
fi

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format is required" >&2
    exit 1
fi

if ! command -v cargo >/dev/null 2>&1; then
    echo "cargo is required" >&2
    exit 1
fi

cxx_files=()
while IFS= read -r file; do
    case "$file" in
        glibc-2.17.h | include/waywallen_display_protocol_types.h | src/generated/*)
            continue
            ;;
    esac
    cxx_files+=("$file")
done < <(git ls-files '*.c' '*.h' '*.cpp' '*.hpp' '*.cc' '*.hh')

if [[ ${#cxx_files[@]} -gt 0 ]]; then
    if [[ $CHECK -eq 1 ]]; then
        clang-format --dry-run --Werror "${cxx_files[@]}"
    else
        clang-format -i "${cxx_files[@]}"
    fi
fi

if [[ $CHECK -eq 1 ]]; then
    cargo fmt --all -- --check
else
    cargo fmt --all
fi
