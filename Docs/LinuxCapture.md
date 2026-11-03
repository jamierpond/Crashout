# Linux capture: design

Status: stage 1 built (see "Stage 1 as built" below); stages 2–4 are a
proposal. Target: x86_64 and arm64 Linux, Clang and GCC. Android is out of
scope (its own signal and linker story).

## Design

1. **Launch: the macOS signal-only path, unchanged in shape.** `fork` +
   `execve` (or `posix_spawn`) of the app's own executable with the shared
   memory on fd 3, the crash pipe on 4 and the ready pipe on 5
   (`CrashChannel-macOS.h`, which becomes `CrashChannel-Posix.h`). The handler
   is argued `inherited-descriptors-ptrace` instead of the Mach handshake. The
   shared block is a `memfd_create` file (macOS uses an unlinked file, which
   also works on Linux; memfd avoids touching the disk).

2. **Two capture modes, like macOS's Mach path and its signal fallback.**
   The app's signal handler always records the signal, `siginfo`, the
   `ucontext` registers and the thread id into `SharedCrashInfo`, writes `'C'`
   to the crash pipe and waits. The handler then:
   - **ptrace mode (default):** the app calls `prctl(PR_SET_PTRACER,
     handlerPid)` right after the spawn, so Yama's `ptrace_scope=1` (the Ubuntu
     default) allows the attach. The handler `PTRACE_SEIZE`s + `PTRACE_INTERRUPT`s
     every thread in `/proc/<pid>/task`, reads registers with
     `PTRACE_GETREGSET`, stacks through `process_vm_readv` (or
     `/proc/<pid>/mem`), walks each thread from outside, writes the minidump,
     and detaches. `capturedBy: handler-ptrace`. This is Breakpad/Crashpad's
     design and the Linux twin of the Mach task port.
   - **signal-only fallback** (`ptrace_scope` 2/3, containers without
     `CAP_SYS_PTRACE`, attach refused): the app walks its own stack in the
     handler on the alternate stack and the handler only symbolises, exactly
     macOS `--signals-only` (`capturedBy: handler-signal`). No minidump.

3. **Signal handlers + sigaltstack: reuse macOS's.** `CrashClient-macOS.cpp`'s
   signal half (`caughtSignals`, `handleCrashSignal`, `restoreDefaultAndRaise`,
   `recordCurrentException`, pipe/spawn helpers) is plain POSIX; split it into
   `CrashClient-Posix.cpp` and keep the dyld-image and Mach bits in the macOS
   file. Linux adds `SIGSYS`, `SIGTRAP`, `SI_TKILL` checks and reads the PC
   from `uc_mcontext.gregs[REG_RIP]` / `uc_mcontext.pc`.
   `ThreadHooks-macOS.cpp`'s sigaltstack install/uninstall is likewise POSIX.

4. **Stack walk.** Stage 1 in-app: frame pointers from the `ucontext`
   (build with `-fno-omit-frame-pointer`, which eacp's Clang lanes can force on
   the crash-reporting consumer), with `_Unwind_Backtrace` (libgcc/libunwind,
   async-signal-safe enough in practice, already linked) as the second walker
   for code without frame pointers. glibc `backtrace()` is not used: its first
   call `dlopen`s libgcc_s, so it must be primed (macOS's `primeBacktrace`
   already does this) and it still walks from the handler, not the fault.
   From the handler (ptrace mode): frame-pointer walk over `process_vm_readv`
   (the walker in `HangWatchdog-macOS.cpp`/`TaskCapture-macOS.cpp` is generic
   once its memory read is a callback), with CFI unwinding (`.eh_frame`) left
   for a later stage.

5. **Modules and symbolisation.** Modules come from `dl_iterate_phdr` in the
   app (load address, path, `NT_GNU_BUILD_ID` from the `PT_NOTE`), recorded
   into `SharedCrashInfo.modules` at `start()` and refreshed from a
   `dlopen` hook point (see 8); in ptrace mode `/proc/<pid>/maps` replaces it.
   The handler symbolises the way macOS does: the main executable and libs it
   also loaded by base-offset + `dladdr`; anything else through a new
   `LoadedLibrarySymbolReader-Linux.cpp` that reads `.symtab`/`.dynsym` of the
   ELF on disk. `frame.moduleUuid` is the build id. Demangle with
   `abi::__cxa_demangle`, as now.

6. **Minidump: yes, the writer ports.** `Minidump-macOS.cpp` is already a
   platform-neutral Breakpad/MS writer fed by `MinidumpContents`; rename it
   `Minidump.cpp` and add the Linux pieces Breakpad expects: CPU context for
   x86_64 (`MINIDUMP_CONTEXT_AMD64`, which the Windows-format writer already
   knows) and arm64, `MD_LINUX_MAPS`, `MD_LINUX_PROC_STATUS`,
   `MD_LINUX_CMD_LINE`, `MD_LINUX_AUXV` streams (raw `/proc` files), and the
   build id as the CodeView record (`appendCodeViewRecord` takes bytes, so the
   ELF id fits as Breakpad's `BpEL` format). Only ptrace mode writes one.

7. **Threads.** `/proc/self/task` (app) or `/proc/<pid>/task` (handler) lists
   them; names from `/proc/.../task/<tid>/comm`. Ids are `gettid()`, matching
   `getCurrentThreadId`.

8. **Per-thread hooks (slot release, alt stacks on new threads).** Linux has
   no `pthread_introspection_hook_install`. Options, in order of preference:
   - a `thread_local` object whose destructor releases the slot and frees
     the alt stack: runs on every thread exit through `__cxa_thread_atexit`,
     covers threads that ever called `sendHeartbeat`/touched the TLS. That is
     exactly the set that owns a slot, so slot release is solved this way.
   - alt stacks on *every* new thread: interpose `pthread_create` (define it
     in the static lib and forward to `dlsym(RTLD_NEXT, ...)`), wrapping the
     start routine to install the alt stack and arm the `thread_local`. It is
     what Crashpad and Sentry Native do. Interposition only covers threads
     created after the library is linked into the executable; plugins loaded
     into a foreign host get starting-thread-only behaviour.

9. **Hang detection.** Handler-side, as on Windows: the handler already reads
   heartbeats from the shared block (`StallTracker`). On a stall it either
   - ptrace mode: seizes the stuck thread, reads its registers, walks its
     stack from outside, detaches; the app never runs any code. Or
   - signal-only: writes `'H'`-equivalent request into the block and the
     app's watchdog thread `tgkill`s the stuck thread with a private
     realtime signal (`SIGRTMIN+n`) whose handler records the `ucontext` PC +
     frame-pointer walk into the slot. This is the macOS in-app watchdog with
     `thread_suspend` replaced by a signal; it cannot sample a thread blocked
     with that signal masked or stuck in an uninterruptible syscall (D state).

10. **Exits without signals.** `std::terminate` and uncaught exceptions: the
    existing `std::set_terminate` + `recordCurrentException` path, then
    `abort()` → `SIGABRT`. Throw sites: macOS's `ThrowSites-macOS.cpp`
    interposes `__cxa_throw`; the same works with libstdc++/libc++ on Linux
    (ELF symbol interposition) and becomes `ThrowSites-Posix.cpp`. The
    handler's death is seen through the crash pipe's EOF on the app side and
    `waitpid`/`PR_SET_PDEATHSIG` on the handler side (so it exits with the
    app instead of lingering). `session.active` / `unreportedExit` /
    `handlerUnavailable` are portable already.

## Files

Reuse as is: `CrashHandler.cpp`, `CrashReporting.cpp`, `HandlerReport.cpp`,
`SharedState.*`, `ThreadHeartbeats.cpp`, `CrashReport.*`, `ReportDatabase.*`,
`Faults/*`.

Split out of the macOS files into `-Posix` files built on macOS and Linux:
`CrashChannel-Posix.h`, `CrashClient-Posix.cpp` (signals, spawn, pipes,
terminate), `ThreadHooks-Posix.cpp` (sigaltstack), `EventStack-Posix.cpp`
(`captureCurrentStack` via `backtrace` + `dladdr`), `ThrowSites-Posix.cpp`,
`StackWalk.cpp` (frame-pointer walk over a memory-read callback, out of
`TaskCapture`/`HangWatchdog`), `Minidump.cpp` (renamed writer).

New Linux files: `CrashClient-Linux.cpp` (`dl_iterate_phdr` modules,
`ucontext` registers, `PR_SET_PTRACER`, memfd, thread name/id),
`CrashHandlerProcess-Linux.cpp` (pipe loop, ptrace capture, minidump),
`PtraceCapture-Linux.{h,cpp}` (seize/interrupt/getregset/read/detach),
`ElfImages-Linux.{h,cpp}` (build id, `/proc/<pid>/maps`),
`LoadedLibrarySymbolReader-Linux.cpp`, `ThreadHooks-Linux.cpp`
(`pthread_create` interposition + `thread_local` release),
`HangWatchdog-Linux.cpp`, `MinidumpLinuxStreams-Linux.cpp`.

CMake: drop the `(APPLE AND NOT IOS) OR WIN32` gate to include `LINUX AND NOT
ANDROID`. `Faults/Matrix.cpp`: replace `notAvailable("no crash reporter on
Linux")` with real Linux expectations.

## CrashMatrix on Linux

Apply (signal / expected reason): `nullptr` SIGSEGV, `divide` SIGFPE (x86_64;
arm64 integer divide does not trap: `notAvailable`, as `floatingpoint` already
is for arm64), `stackoverflow` SIGSEGV (needs alt stack on that thread),
`abort` SIGABRT, `purevirtual` SIGABRT, `terminate` std::terminate,
`heapcorruption` and `heapsmash` SIGABRT from glibc's malloc checks (short
stack, as on macOS), `stall` hang, `illegal` SIGILL, `executeprotection`
SIGSEGV, `badsystemcall` SIGSYS (raised or through seccomp).
Not available: `fastfail`, `privileged` (x86_64 `hlt` gives SIGSEGV; could be
reported as that rather than skipped), `guardpage`, `inpageerror` (Windows),
the two macOS-only Mach ones. `floatingpoint` only with `feenableexcept` on
x86_64.

## Hard or not possible

- `ptrace_scope` 2/3, gVisor, some containers and CI sandboxes refuse the
  attach. The signal-only fallback is the answer; the tests must run both
  (force fallback with an option, like `captureMachExceptions = false`).
- A debugger already attached (gdb, `strace`) means ptrace fails. Fallback.
- The in-app walk in the signal handler is not formally async-signal-safe
  past frame pointers; heap corruption can wreck it. ptrace mode exists
  for this reason.
- No hook for every new thread without interposing `pthread_create`; a
  plugin `.so` inside a non-eacp host only covers threads it saw. Threads
  made with raw `clone` (Go, some JITs) are never covered.
- CFI unwinding from outside (code built with `-fomit-frame-pointer`, i.e.
  most distro libs) is real work: Breakpad's or libunwind's remote mode.
  Without it, stacks through system libraries stop early in ptrace mode.
  Stage 1 avoids this by walking in-app with `_Unwind_Backtrace`.
- Symbols in stripped distro libraries only come from `.dynsym` (exported
  names); full names need debuginfod or server-side symbolisation from
  the minidump + build id.
- Hang sampling of a thread in D state or with every signal blocked: ptrace
  mode still reads its registers; signal-only mode cannot.

## Stages

1. **Signal-only crash report with a symbolised stack, Clang x86_64.**
   Posix split of the macOS signal client, memfd + spawn, `ucontext` PC,
   `_Unwind_Backtrace` walk on the alt stack (main thread + starting thread),
   `dl_iterate_phdr` modules with build ids, handler symbolisation with
   `dladdr` + the ELF symbol reader, `std::terminate`. Linux lane in the
   Matrix for `nullptr`, `divide`, `abort`, `terminate`, `illegal`,
   `stackoverflow` (main thread). `capturedBy: handler-signal`.
2. **Every thread, hangs, events.** `pthread_create` interposition +
   `thread_local` slot release, background-thread alt stacks, `tgkill`
   hang sampling, `captureMessage`/`captureException`, throw sites. Full
   Matrix on Linux, GCC lane too, arm64 on the Ubuntu VM.
3. **ptrace mode + minidump.** `PR_SET_PTRACER`, seize all threads, outside
   frame-pointer walk, `/proc` streams, Breakpad-format minidump, every
   thread's stack in the report; hang capture from the handler.
4. **CFI unwinding from outside** for omitted-frame-pointer code, if stage 3's
   stacks are too short in practice.

## Stage 1 as built

Where it differs from the plan above:

- **Modules come from the handler, not the app.** Once the crash byte arrives
  the handler reads `/proc/<pid>/maps` while the app waits in its signal
  handler (one module per mapped file, from its offset-0 mapping to the end of
  its last). No `dl_iterate_phdr` at `start()`, and a library `dlopen`ed later
  is covered with no hook.
- **The walk is the macOS one, unchanged:** glibc `backtrace()`, primed at
  `start()`, from the signal handler on the alternate stack. It unwinds
  through the kernel's signal frame and reports the faulting instruction a
  second time; the handler drops every frame before that repeat.
- **Symbols always come from the ELF file** (`ElfImages-Linux.cpp`,
  `ElfSymbolReader`: `.symtab`, else `.dynsym`), for the executable too,
  because `dladdr` names only exported functions. `moduleUuid` is the first
  16 bytes of the GNU build id in byte order, formatted like an LC_UUID;
  `eacp_enable_crash_reporting` links with `--build-id`.
- **Shared with macOS** through `PosixPlatform.h`, the hooks each platform's
  `CrashClient-<platform>.cpp` fills: `CrashClient-Posix.cpp`,
  `CrashChannel-Posix.h`, `CrashHandlerChannel-Posix.cpp` (the handler's pipe
  and shared-block ends), `ThreadHooks-Posix.cpp`, `EventStack-Posix.cpp`,
  `SymbolNames-Posix.cpp`. Linux has no per-descriptor `F_SETNOSIGPIPE`, so its
  write to the crash pipe holds SIGPIPE back on the thread and takes a raised
  one out again.
- The Matrix has two Linux lanes, `Linux x86_64` and `Linux arm64`. Reported:
  `nullptr`, `abort`, `terminate` and `illegalinstruction` on both threads,
  `stackoverflow` on the main thread, `divide` on x86_64. The other faults
  this platform raises are pending stage 2; the rest cannot be raised.
- Not in stage 1, and so compiled out of the Linux tests: hang sampling,
  alternate stacks on new threads and slot release at thread end (no
  per-thread hook yet), symbols in an event's stack (the in-app walk still
  uses `dladdr`), throw sites.

## Size

Stage 1: ~1,200 lines (≈500 moved from macOS into `-Posix`, ≈700 new: ELF
reader ~250, client ~250, handler ~200), plus ~150 of Matrix/test changes.
Stage 2: ~600. Stage 3: ~1,200 (ptrace capture ~400, minidump Linux streams
and contexts ~400, handler ~400). Stage 4: 1,500+ or a dependency
(libunwind's remote mode). Stages 1–3 together are roughly the size of the
Windows backend (~2,500 lines).
