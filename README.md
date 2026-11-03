# Crashout

Crash reporting for apps built on [eacp](https://github.com/eyalamirmusic/eacp),
moved out of eacp into a library of its own. When the app crashes, a handler
process started from the app's own executable writes a report: the crashed
thread's symbolised stack, a minidump, metadata and attachments. The app's own
code then receives that report. It also reports hangs, stack overflows,
uncaught exceptions and runs that ended with no report. macOS and Windows,
and on Linux crashes and uncaught exceptions so far, with no minidump
(`Docs/LinuxCapture.md`); the report types and the report database
(`eacp-crash-report`) build everywhere, so a service can read the reports with
the same structs.

The namespace and include paths are eacp's: `eacp::CrashReporting`,
`<eacp/CrashReporting/CrashReporting.h>`.

Until eyalamirmusic/eacp #196 and #197 merge, Crashout pins eacp's
`jp/crashout-eacp-base`, a merge of the two: #196 adds Core pieces the library
uses, and #197 removes eacp's own copy of crash reporting, whose targets have
the same names. Once both merge, the pin moves to develop.

## Using it

```cmake
CPMAddPackage(NAME Crashout GITHUB_REPOSITORY jamierpond/Crashout GIT_TAG main)

eacp_add_app(MyApp Main.cpp)
eacp_enable_crash_reporting(MyApp SYMBOLS_DIR ${CMAKE_BINARY_DIR}/symbols)
```

```cpp
#include <eacp/CrashReporting/CrashReporting.h>

int main(int argc, char* argv[])
{
    if (auto code = eacp::CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;

    eacp::CrashReporting::start();
    // ...
}
```

`runHandlerIfRequested` is the handler process's whole life: `start()`
relaunches the app's own executable with a flag that call recognises. Register
a `CrashHandler` with `EACP_REGISTER_CRASH_HANDLER` to receive reports
(`reportWritten`), and to hear when the handler could not start
(`handlerUnavailable`) or a previous run ended with no report
(`unreportedExit`). Reports land under `pending/<id>/report.json` and are read
back with `ReportDatabase`.

`eacp_enable_crash_reporting` links `eacp-crash-reporting`, and gives every
build its symbols (a `.dSYM` on macOS, a PDB on Windows), copied to
`SYMBOLS_DIR` as `<target>-<version>` when one is given. On Windows it copies
`eacp-windows-error-reporting-helper.dll` beside the executable, so fast-fail
and heap-corruption crashes, which no in-process handler sees, still get a
stack.

`Apps/CrashReporting/SelfDestruct` is a window whose buttons crash it in every
supported way and show the report. `Docs/CrashReporting.md` covers how it
works inside.

## Building

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```
