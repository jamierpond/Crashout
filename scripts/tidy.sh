#!/usr/bin/env bash
# Runs clang-tidy (the repo's .clang-tidy, a copy of eacp's) over this repo's
# own sources and exits 1 if it reports anything.
#
#   scripts/tidy.sh <build-dir>
#
# <build-dir> must already be configured (and built, so generated headers
# exist). If it has no compile_commands.json, it is reconfigured with
# CMAKE_EXPORT_COMPILE_COMMANDS=ON. Set CLANG_TIDY to pick another binary.
#
# This file is identical in every jamierpond dev-tool repo; change it in all
# of them at once.

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <build-dir>" >&2
    exit 2
fi

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"
build_dir=$(cd "$1" && pwd)
clang_tidy=${CLANG_TIDY:-clang-tidy}
not_our_code='/(build[^/]*|_deps|\.cache|ThirdParty|third_party)/'

if [ ! -f "$build_dir/compile_commands.json" ]; then
    cmake -S . -B "$build_dir" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

compiler=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$build_dir/CMakeCache.txt")

# clang-tidy is a separate clang and does not know where the build's compiler
# finds the C++ and C library headers (nix wrappers, gcc, another Xcode), so
# hand it the compiler's own search list. Each compiler's private builtin
# header dir is left out: clang-tidy has its own, and gcc's does not parse.
include_args=()
in_search_list=0
while IFS= read -r line; do
    case "$line" in
        "#include <...> search starts here:") in_search_list=1; continue ;;
        "End of search list.") in_search_list=0; continue ;;
    esac
    [ "$in_search_list" -eq 1 ] || continue
    dir=${line# }
    if printf '%s\n' "$dir" | grep -Eq '/lib/(gcc|clang)/[^/]+/[^/]+(/[^/]+)?/include(-fixed)?$|/lib/clang/[^/]+/include$'; then
        continue
    fi
    case "$dir" in
        *" (framework directory)")
            include_args+=("--extra-arg=-iframework${dir% (framework directory)}") ;;
        *)
            include_args+=("--extra-arg=-isystem$dir") ;;
    esac
done < <("$compiler" -E -x c++ - -v </dev/null 2>&1)

files_in_database=$(sed -n 's/^ *"file": "\(.*\)",\{0,1\}$/\1/p' \
    "$build_dir/compile_commands.json" | sort -u)
sources=()
while IFS= read -r file; do
    absolute="$repo_root/$file"
    if printf '%s\n' "$files_in_database" | grep -Fxq "$absolute"; then
        sources+=("$absolute")
    fi
done < <(git ls-files '*.cpp' '*.cc' '*.mm' | grep -Ev "^(build[^/]*|_deps|\.cache|ThirdParty|third_party)/")

if [ ${#sources[@]} -eq 0 ]; then
    echo "tidy: no sources of this repo in $build_dir/compile_commands.json" >&2
    exit 2
fi

job_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)
output_dir=$(mktemp -d)
trap 'rm -rf "$output_dir"' EXIT

export clang_tidy build_dir repo_root not_our_code output_dir
printf '%s\0' "${sources[@]}" | xargs -0 -P "$job_count" -I{} bash -c '
    source_file=$1
    shift
    log="$output_dir/$(printf "%s" "$source_file" | tr "/" "_").log"
    "$clang_tidy" -p "$build_dir" --quiet --use-color=false \
        --header-filter="^$repo_root/" --exclude-header-filter="$not_our_code" \
        "$@" "$source_file" > "$log" 2>&1 || true
' _ {} "${include_args[@]+"${include_args[@]}"}"

diagnostic_pattern='^/.*:[0-9]+:[0-9]+: (warning|error): .*\[[a-zA-Z0-9,.-]+\]$'
findings=$(cat "$output_dir"/*.log | grep -E "$diagnostic_pattern" | sort -u || true)
failures=$(cat "$output_dir"/*.log | grep -E '^Error while processing|error: unable to' | sort -u || true)

if [ -n "$findings" ]; then
    printf '%s\n' "$findings"
    echo
    printf '%s\n' "$findings" | sed -E 's/.*\[([^]]+)\]$/\1/' | sort | uniq -c | sort -rn
fi
if [ -n "$failures" ]; then
    printf '%s\n' "$failures"
fi

finding_count=$(printf '%s' "$findings" | grep -c . || true)
echo "tidy: ${#sources[@]} files, $finding_count findings"
if [ "$finding_count" -ne 0 ] || [ -n "$failures" ]; then
    exit 1
fi
