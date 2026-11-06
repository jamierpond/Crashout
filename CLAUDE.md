# CLAUDE.md

## Git Rules

Claude must never commit or push without explicit permission from the user in
the current conversation.

Claude must never appear in commit metadata: not as author or committer, and
not as a `Co-Authored-By` trailer or a "Generated with Claude" line in the
message. The hooks under `.githooks/` refuse such a commit; activate them once
per clone with `git config core.hooksPath .githooks`.

## Project Overview

Crashout is eacp's crash reporting library and its demo app, moved out of eacp
unchanged: namespace `eacp::CrashReporting`, includes
`<eacp/CrashReporting/...>`, targets `eacp-crash-report`,
`eacp-crash-reporting`, `eacp-crash-faults` and
`eacp-windows-error-reporting-helper`. eacp is fetched by CPM
(`CMake/Findeacp.cmake`); `-DCPM_eacp_SOURCE=$HOME/...` builds against a local
checkout. `README.md` is for users of the library; `Docs/CrashReporting.md`
is how it works inside.

- `Lib/eacp/CrashReporting`: the library, `Faults/` and the Windows Error
  Reporting helper DLL
- `Apps/CrashReporting/SelfDestruct`: the demo app
- `Tests/CrashReporting`: the suites; `CrashReportingTests` relaunches its own
  binary with `--crashing-process`, and never uses the demo app
- `CMake/CrashReporting.cmake`: `eacp_enable_crash_reporting`

## Build Commands

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Code Style

Before writing a helper, read eacp's `Docs/Toolbox.md`.

Always use the most modern C++ and RAII practices.
Use auto for variables and whenever possible.
Don't use auto for functions and member functions

Don't use comments unless absolutely needed. Use named functions to make code self documenting.

Give std::function members a non-null default — a no-op lambda, or one
returning an empty value (e.g. `[] { return Image {}; }`) — so call sites
invoke them directly without null checks.


Enforced via `.clang-format`:
- Allman brace style
- 85 column limit
- 4-space indentation (no tabs)
- Pointer alignment: left (`int* ptr`)
- Break constructor initializers before comma

Always run clang-format for edited code files
