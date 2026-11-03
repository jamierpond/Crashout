# Crash reporting internals


The report types and `ReportDatabase` are `eacp-crash-report`, built on every
platform so a Linux service reads the apps' reports with the same struct;
`eacp-crash-reporting` (macOS, Windows and Linux; `eacp_enable_crash_reporting(<exe>)`
in `CMake/CrashReporting.cmake` links it, `eacp-core` does not) relaunches the
app's own executable as a handler process at `start()`; `main()` begins with
`runHandlerIfRequested`. The app and handler share one fixed-size block
(`SharedState.h`: metadata, attachments, the crash). On macOS the app hands
the handler its task port at `start()` and points its crash Mach exceptions
at the handler's port, so the handler reads a fault from outside — registers,
frame-pointer walk, images through `TASK_DYLD_INFO` — writes a Breakpad
minidump of every thread (`Minidump-macOS.cpp`) and symbolises with
`dladdr` (`capturedBy: handler-mach`); `abort()` and `std::terminate` reach
it as `EXC_CRASH`. The in-app signal handler (`backtrace()` on an alternate
stack) is the fallback when the handshake failed. Linux has only that signal
path, in the same `-Posix` files macOS uses, and its handler reads the app's
modules from `/proc/<pid>/maps` and their symbols from the ELF files
(`Docs/LinuxCapture.md`). On Windows
the handler writes a minidump and walks the stack with DbgHelp. Reports land in
`pending/<id>/report.json` and reach the app's `CrashHandler`
(`EACP_REGISTER_CRASH_HANDLER`), whose one `reportWritten` takes every
`ReportKind`. Hangs are a watched thread that stops sending heartbeats: the
main thread's, from a timer on its loop, and any thread's that calls
`CrashReporting::sendHeartbeat()`, which claims it a slot on its first call
(found again through a thread-local) and is freed when the thread ends through
the same per-thread hook as stack-overflow reporting; Core knows nothing of
it. They share one 32-slot table in the shared block, and threads silent in the same check, a deadlock, are one
`kind: "hang"` report with `threadName` and `stalledThreads`.
`Apps/CrashReporting/SelfDestruct` crashes
itself and says on each button what the report will be; `CrashReportingTests`
relaunches itself with `--crashing-process` to raise every fault on the main
and a background thread, one case per cell of `Faults/Matrix.h`; the faults themselves are `eacp-crash-faults` (`CrashReporting/Faults/`),
`Options::stackOverflowReporting` gives the starting thread, every
thread (the default) or none a stack to report an overflow from (an alternate signal stack on
macOS, through `pthread_introspection_hook_install`; a `SetThreadStackGuarantee`
reserve on Windows, from a TLS callback), macOS records
throw sites through its own `__cxa_throw`. With the handler gone a crash goes
unreported; `start()` then returns false and calls `handlerUnavailable`, and a
`session.active` marker that a clean exit removes lets the next `start()` call
`unreportedExit` for a run that ended with no report.
`captureMessage`/`captureException` write a handled `kind: event` report from
the app itself, with the calling thread's stack, and hand it to
`CrashHandler::reportWritten` without ending the app.

