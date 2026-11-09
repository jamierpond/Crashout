#include <eacp/Core/Utils/Time.h>
#include <eacp/CrashReporting/CrashReportingPlatform.h>
#include <eacp/CrashReporting/SharedState.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <memory>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <time.h>
#else
#include <eacp/Core/Utils/WinInclude.h>
#endif

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting::Detail;

namespace EventWaitsTests
{
constexpr auto testThreshold = std::uint64_t {500};
constexpr auto testPollInterval = std::uint64_t {100};
constexpr auto stateChangeWaitLimit = Time::MS {5000};
constexpr auto stateChangePollInterval = Time::MS {1};
constexpr auto clockAgreementMilliseconds = std::uint64_t {50};

#if defined(__APPLE__)
std::uint64_t readClockThatStopsInSleep()
{
    constexpr auto nanosecondsPerMillisecond = std::uint64_t {1000000};
    return ::clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / nanosecondsPerMillisecond;
}
#else
std::uint64_t readClockThatStopsInSleep()
{
    constexpr auto interruptTicksPerMillisecond = ULONGLONG {10000};
    auto ticks = ULONGLONG {0};
    ::QueryUnbiasedInterruptTime(&ticks);
    return ticks / interruptTicksPerMillisecond;
}
#endif

std::uint64_t getDistance(std::uint64_t first, std::uint64_t second)
{
    return first > second ? first - second : second - first;
}

// A stall's length is read off this clock, so an hour asleep adds nothing.
auto tHeartbeatClockStopsWhileAsleep =
    test("EventWaits/heartbeatClockStopsInSleep") = []
{
    auto heartbeatClock = getSteadyMilliseconds();
    auto sleepExcludingClock = readClockThatStopsInSleep();

    check(getDistance(heartbeatClock, sleepExcludingClock)
          <= clockAgreementMilliseconds);
};

eacp::OwningPointer<SharedState> createWatchedState()
{
    auto state = eacp::makeOwned<SharedState>();
    initialiseSharedState(*state, 1);
    state->hang.thresholdMilliseconds = testThreshold;
    state->hang.pollIntervalMilliseconds = testPollInterval;
    watchMainThread(state->hang, 1);
    sendWatchedThreadHeartbeat(state->hang.threads[mainThreadSlot]);
    return state;
}

template <typename Condition>
bool waitUntil(Condition condition)
{
    auto deadline = Time::Deadline {stateChangeWaitLimit};

    while (!condition() && !deadline.expired())
        Time::sleep(stateChangePollInterval);

    return condition();
}

#if defined(__APPLE__)
constexpr auto heartbeatStarvedFor = std::uint64_t {600000};

bool isStallReportedLongAfterLastHeartbeat(const SharedHangInfo& hang)
{
    auto tracker = StallTracker {};
    auto heartbeat = hang.threads[mainThreadSlot].lastHeartbeatMilliseconds.load();
    return !takeNewStalls(hang, tracker, heartbeat + heartbeatStarvedFor).empty();
}

constexpr auto farFuture = CFTimeInterval {1.0e9};

// A run loop thread standing in for the main thread: its loop runs the observer
// start() installs, and `onFirstTimer` runs on it as the loop's first work. The
// timer repeats only so the loop has a source left and keeps waiting.
struct RunLoopThread
{
    template <typename Work>
    RunLoopThread(SharedWatchedThread& watched, Work onFirstTimer)
        : thread([this, &watched, onFirstTimer] { run(watched, onFirstTimer); })
    {
    }

    ~RunLoopThread()
    {
        waitUntil([this] { return runLoop.load() != nullptr; });
        CFRunLoopStop(runLoop.load());
        thread.join();
    }

    template <typename Work>
    void run(SharedWatchedThread& watched, Work onFirstTimer)
    {
        observeEventWaitsOnCurrentThread(watched);
        auto timer = CFRunLoopTimerCreateWithHandler(
            kCFAllocatorDefault,
            CFAbsoluteTimeGetCurrent(),
            farFuture,
            0,
            0,
            ^(CFRunLoopTimerRef) { onFirstTimer(); });
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
        runLoop = CFRunLoopGetCurrent();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, farFuture, false);
        CFRunLoopTimerInvalidate(timer);
        CFRelease(timer);
        stopObservingEventWaits();
    }

    std::atomic<CFRunLoopRef> runLoop {nullptr};
    std::thread thread;
};

// App Nap or timer coalescing can hold the heartbeat back for minutes while
// the loop sleeps; a thread parked waiting for its next event is idle.
auto tIdleThreadWithStarvedHeartbeatIsNotReported =
    test("EventWaits/idleThreadWithStarvedHeartbeatIsNotReported") = []
{
    auto state = createWatchedState();
    auto& watched = state->hang.threads[mainThreadSlot];
    auto timerRan = std::atomic<bool> {false};
    auto loop = RunLoopThread {watched, [&timerRan] { timerRan = true; }};

    check(waitUntil([&] { return timerRan && watched.waitingForEvents != 0; }));
    check(!isStallReportedLongAfterLastHeartbeat(state->hang));
};

auto tBusyThreadIsReported = test("EventWaits/busyThreadIsReported") = []
{
    auto state = createWatchedState();
    auto& watched = state->hang.threads[mainThreadSlot];
    auto busy = std::atomic<bool> {false};
    auto release = std::atomic<bool> {false};
    auto loop = RunLoopThread {watched,
                               [&]
                               {
                                   busy = true;
                                   waitUntil([&] { return release.load(); });
                               }};

    check(waitUntil([&] { return busy.load(); }));
    check(watched.waitingForEvents == 0);
    check(isStallReportedLongAfterLastHeartbeat(state->hang));
    release = true;
};
#else
// A message-loop thread standing in for the main thread: `work` runs on it,
// then it parks in GetMessage until told to quit.
struct MessageLoopThread
{
    template <typename Work>
    explicit MessageLoopThread(Work work)
        : thread([this, work] { run(work); })
    {
    }

    ~MessageLoopThread()
    {
        waitUntil([this] { return threadId.load() != 0; });
        ::PostThreadMessageW(threadId.load(), WM_QUIT, 0, 0);
        thread.join();
    }

    template <typename Work>
    void run(Work work)
    {
        auto message = MSG {};
        ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        threadId = ::GetCurrentThreadId();
        work();

        while (::GetMessageW(&message, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&message);
    }

    std::atomic<DWORD> threadId {0};
    std::thread thread;
};

constexpr auto silenceBeforeCheck = 10 * testThreshold;

bool isStallReportedAfterHandlerCheck(SharedHangInfo& hang, std::size_t slot);

auto tIdleThreadWithStarvedHeartbeatIsNotReported =
    test("EventWaits/idleThreadWithStarvedHeartbeatIsNotReported") = []
{
    auto state = createWatchedState();
    auto loop = MessageLoopThread {[] {}};

    check(waitUntil(
        [&]
        {
            auto id = loop.threadId.load();
            return id != 0 && isThreadWaitingForWindowMessages(id);
        }));
    watchMainThread(state->hang, loop.threadId.load());
    check(!isStallReportedAfterHandlerCheck(state->hang, mainThreadSlot));
};

auto tBusyThreadIsReported = test("EventWaits/busyThreadIsReported") = []
{
    auto state = createWatchedState();
    auto busy = std::atomic<bool> {false};
    auto release = std::atomic<bool> {false};
    auto loop = MessageLoopThread {[&]
                                   {
                                       busy = true;
                                       waitUntil([&] { return release.load(); });
                                   }};

    check(waitUntil([&] { return busy.load(); }));
    watchMainThread(state->hang, loop.threadId.load());
    check(!isThreadWaitingForWindowMessages(loop.threadId.load()));
    check(isStallReportedAfterHandlerCheck(state->hang, mainThreadSlot));
    release = true;
};

// Efficiency mode or a suspended package freezes every thread of the app; a
// thread that is suspended is frozen, not hung, whichever slot watches it.
struct SuspendedThread
{
    SuspendedThread()
    {
        waitUntil([this] { return threadId.load() != 0; });
        handle = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadId.load());
        ::SuspendThread(handle);
    }

    ~SuspendedThread()
    {
        ::ResumeThread(handle);
        ::CloseHandle(handle);
        release = true;
        thread.join();
    }

    std::atomic<DWORD> threadId {0};
    std::atomic<bool> release {false};
    std::thread thread {[this]
                        {
                            threadId = ::GetCurrentThreadId();
                            waitUntil([this] { return release.load(); });
                        }};
    HANDLE handle = nullptr;
};

bool isStallReportedAfterHandlerCheck(SharedHangInfo& hang, std::size_t slot)
{
    auto& watched = hang.threads[slot];
    auto now = getSteadyMilliseconds();
    watched.lastHeartbeatMilliseconds = now - silenceBeforeCheck;
    heartbeatWatchedThreadsThatAreNotHung(hang, now);
    auto tracker = StallTracker {};
    return !takeNewStalls(hang, tracker, getSteadyMilliseconds()).empty();
}

auto tSuspendedMainThreadIsNotReported =
    test("EventWaits/suspendedMainThreadIsNotReported") = []
{
    auto state = createWatchedState();
    auto suspended = SuspendedThread {};

    watchMainThread(state->hang, suspended.threadId.load());
    check(!isStallReportedAfterHandlerCheck(state->hang, mainThreadSlot));
};

auto tSuspendedWorkerThreadIsNotReported =
    test("EventWaits/suspendedWorkerThreadIsNotReported") = []
{
    auto state = createWatchedState();
    auto suspended = SuspendedThread {};
    auto slot = claimWatchedThread(state->hang, "worker", suspended.threadId.load());

    check(slot.has_value());
    check(!isStallReportedAfterHandlerCheck(state->hang, *slot));
};
#endif
} // namespace EventWaitsTests
