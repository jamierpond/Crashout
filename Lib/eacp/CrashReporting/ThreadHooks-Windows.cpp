#include "CrashReportingPlatform.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <atomic>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto stackOverflowReserveBytes = ULONG {64 * 1024};
// A thread sized by its creator, a driver or a library, keeps all of it: a 64 KB
// bite from a 128 KB stack overflows it at once.
constexpr auto smallestStackThatCanSpareTheReserveBytes =
    ULONG_PTR {8 * stackOverflowReserveBytes};

std::atomic<bool> shouldInstallOnNewThreads {false};
std::atomic<bool> shouldReleaseWatchOnThreadEnd {false};

// The loader calls every module's TLS callbacks on each thread's start, before
// its start routine, and on its end, whichever module or thread pool created it.
void NTAPI handleThreadAttachAndDetach(PVOID, DWORD reason, PVOID)
{
    if (reason == DLL_THREAD_ATTACH
        && shouldInstallOnNewThreads.load(std::memory_order_relaxed))
        installStackOverflowStackOnCurrentThread();

    if (reason == DLL_THREAD_DETACH
        && shouldReleaseWatchOnThreadEnd.load(std::memory_order_relaxed))
        releaseWatchOfThread(::GetCurrentThreadId());
}

bool canCurrentThreadSpareTheReserve()
{
    auto lowest = ULONG_PTR {0};
    auto highest = ULONG_PTR {0};
    ::GetCurrentThreadStackLimits(&lowest, &highest);
    return highest - lowest >= smallestStackThatCanSpareTheReserveBytes;
}
} // namespace

// The reserve can only grow, so a thread that has as much keeps what it has.
bool installStackOverflowStackOnCurrentThread()
{
    if (!canCurrentThreadSpareTheReserve())
        return false;

    auto current = ULONG {0};
    ::SetThreadStackGuarantee(&current);

    if (current >= stackOverflowReserveBytes)
        return false;

    auto reserve = stackOverflowReserveBytes;
    return ::SetThreadStackGuarantee(&reserve) != 0;
}

void setStackOverflowStackOnNewThreads(bool shouldInstall)
{
    shouldInstallOnNewThreads.store(shouldInstall);
}

void setReleasesWatchOnThreadEnd(bool shouldRelease)
{
    shouldReleaseWatchOnThreadEnd.store(shouldRelease);
}
} // namespace eacp::CrashReporting::Detail

// A static library's object is linked only for its symbols, so the linker is
// told to keep both the TLS directory and this callback in the image.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:eacpHandleThreadAttachAndDetach")
#pragma section(".CRT$XLE", long, read)

extern "C" __declspec(allocate(".CRT$XLE"))
const PIMAGE_TLS_CALLBACK eacpHandleThreadAttachAndDetach =
    &eacp::CrashReporting::Detail::handleThreadAttachAndDetach;
