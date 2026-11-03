#include "CrashReportingPlatform.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <pthread/introspection.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
std::atomic<bool> shouldInstallOnNewThreads {false};
std::atomic<bool> shouldReleaseWatchOnThreadEnd {false};
std::optional<pthread_introspection_hook_t> previousIntrospectionHook;

std::uint64_t getThreadId(pthread_t thread)
{
    auto threadId = std::uint64_t {0};
    ::pthread_threadid_np(thread, &threadId);
    return threadId;
}

void handleThreadEvent(unsigned int event,
                       pthread_t thread,
                       void* address,
                       std::size_t size)
{
    auto isStartingHere =
        event == PTHREAD_INTROSPECTION_THREAD_START && thread == ::pthread_self();

    if (isStartingHere && shouldInstallOnNewThreads.load(std::memory_order_relaxed))
        installStackOverflowStackOnCurrentThread();

    if (event == PTHREAD_INTROSPECTION_THREAD_TERMINATE
        && shouldReleaseWatchOnThreadEnd.load(std::memory_order_relaxed))
        releaseWatchOfThread(getThreadId(thread));

    if (previousIntrospectionHook)
        (*previousIntrospectionHook)(event, thread, address, size);
}

void installIntrospectionHookOnce()
{
    static auto once = std::once_flag {};
    std::call_once(once,
                   []
                   {
                       auto previous =
                           ::pthread_introspection_hook_install(&handleThreadEvent);

                       if (previous != nullptr)
                           previousIntrospectionHook = previous;
                   });
}
} // namespace

void setStackOverflowStackOnNewThreads(bool shouldInstall)
{
    if (shouldInstall)
        installIntrospectionHookOnce();

    shouldInstallOnNewThreads.store(shouldInstall);
}

void setReleasesWatchOnThreadEnd(bool shouldRelease)
{
    if (shouldRelease)
        installIntrospectionHookOnce();

    shouldReleaseWatchOnThreadEnd.store(shouldRelease);
}
} // namespace eacp::CrashReporting::Detail
