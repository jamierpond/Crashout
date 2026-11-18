#include "CrashReportingPlatform.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <winternl.h>

#include <optional>
#include <vector>

namespace eacp::CrashReporting::Detail
{
namespace
{
// KTHREAD_STATE and KWAIT_REASON values, as wdm.h numbers them.
constexpr auto threadStateWaiting = ULONG {5};
constexpr auto waitReasonSuspended = ULONG {5};
constexpr auto waitReasonSuspendedInKernel = ULONG {12};
constexpr auto waitReasonWindowMessages = ULONG {13};
constexpr auto statusInfoLengthMismatch = NTSTATUS(0xC0000004L);
constexpr auto initialSnapshotBytes = ULONG {512 * 1024};

struct SystemThreadInformation
{
    LARGE_INTEGER kernelTime;
    LARGE_INTEGER userTime;
    LARGE_INTEGER createTime;
    ULONG waitTime;
    PVOID startAddress;
    CLIENT_ID clientId;
    LONG priority;
    LONG basePriority;
    ULONG contextSwitches;
    ULONG threadState;
    ULONG waitReason;
};

std::vector<std::byte> takeProcessSnapshot()
{
    auto snapshot = std::vector<std::byte>(initialSnapshotBytes);

    while (true)
    {
        auto needed = ULONG {0};
        auto status = ::NtQuerySystemInformation(SystemProcessInformation,
                                                 snapshot.data(),
                                                 static_cast<ULONG>(snapshot.size()),
                                                 &needed);

        if (status != statusInfoLengthMismatch)
            return NT_SUCCESS(status) ? snapshot : std::vector<std::byte> {};

        snapshot.resize(std::max<std::size_t>(needed, snapshot.size() * 2));
    }
}

bool isWaitingForWindowMessages(const SystemThreadInformation& thread)
{
    return thread.threadState == threadStateWaiting
           && thread.waitReason == waitReasonWindowMessages;
}

bool isSuspended(const SystemThreadInformation& thread)
{
    auto isSuspendReason = thread.waitReason == waitReasonSuspended
                           || thread.waitReason == waitReasonSuspendedInKernel;
    return thread.threadState == threadStateWaiting && isSuspendReason;
}

std::optional<SystemThreadInformation>
    findThreadInSnapshot(const std::vector<std::byte>& snapshot,
                         std::uint64_t threadId)
{
    auto offset = std::size_t {0};

    while (offset < snapshot.size())
    {
        auto* process =
            reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(&snapshot[offset]);
        auto* threads =
            reinterpret_cast<const SystemThreadInformation*>(process + 1);

        for (auto index = ULONG {0}; index < process->NumberOfThreads; ++index)
        {
            auto id = reinterpret_cast<std::uintptr_t>(
                threads[index].clientId.UniqueThread);

            if (id == threadId)
                return threads[index];
        }

        if (process->NextEntryOffset == 0)
            break;

        offset += process->NextEntryOffset;
    }

    return std::nullopt;
}

// A suspended thread was frozen from outside the app (NtSuspendProcess, a
// debugger, a suspended package); only the main thread idles in a message wait.
bool isIdleOrFrozen(const SystemThreadInformation& thread,
                    const SharedHangInfo& hang,
                    const SharedWatchedThread& watched)
{
    auto isMainThread = &watched == &hang.threads[mainThreadSlot];
    return isSuspended(thread)
           || (isMainThread && isWaitingForWindowMessages(thread));
}
} // namespace

// GetMessage, WaitMessage and MsgWaitForMultipleObjects all park the thread in
// the kernel with this wait reason; a lock or Sleep never does.
bool isThreadWaitingForWindowMessages(std::uint64_t threadId)
{
    auto thread = findThreadInSnapshot(takeProcessSnapshot(), threadId);
    return thread && isWaitingForWindowMessages(*thread);
}

void heartbeatWatchedThreadsThatAreNotHung(SharedHangInfo& hang, std::uint64_t now)
{
    constexpr auto noGrace = std::uint64_t {0};
    auto snapshot = std::vector<std::byte> {};

    for (auto slot = std::size_t {0}; slot < maxWatchedThreads; ++slot)
    {
        auto& watched = hang.threads[slot];

        if (findStallDuration(hang, watched, now, noGrace) == 0)
            continue;

        if (snapshot.empty())
            snapshot = takeProcessSnapshot();

        auto thread = findThreadInSnapshot(snapshot, watched.threadId);

        if (thread && isIdleOrFrozen(*thread, hang, watched))
            sendWatchedThreadHeartbeat(watched);
    }
}
} // namespace eacp::CrashReporting::Detail
