# Crashout dev commands. Requires just, cmake, ninja and a C++20 toolchain.

set windows-shell := ["cmd.exe", "/c"]

build_dir := "build"
build_type := "RelWithDebInfo"

# Only eacp-crash-report and its tests build on Linux, which need eacp-core alone.
platform_flags := if os() == "linux" { "-DEACP_BUILD_GRAPHICS=OFF" } else { "" }

default:
    @just --list

[doc('Configure + build the libraries, the demo app and the tests; extra arguments go to the configure step')]
build *cmake_args:
    cmake -G Ninja -S . -B {{build_dir}} -DCMAKE_BUILD_TYPE={{build_type}} -DEACP_UNITY_BUILD=OFF {{platform_flags}} {{cmake_args}}
    cmake --build {{build_dir}}

[doc('Build, then run the tests')]
test: build
    ctest --test-dir {{build_dir}} --output-on-failure -j 4

[doc("Build everything, then run clang-tidy (eacp's checks) over Crashout's sources; fails on any finding")]
[unix]
tidy: build
    scripts/tidy.sh {{build_dir}}

[doc('clang-format every tracked source in place')]
[unix]
format:
    git ls-files '*.h' '*.cpp' '*.mm' | xargs clang-format -i

[doc('Delete the build directory')]
clean:
    cmake -E rm -rf {{build_dir}}
