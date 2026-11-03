#include "CrashChannel-Posix.h"
#include "CrashClient-Posix.h"
#include "CrashReportingPlatform.h"
#include "PointerAuthentication.h"
#include "PosixPlatform.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cxxabi.h>
#include <dlfcn.h>
#include <exception>
#include <execinfo.h>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto demangleBufferSize = std::size_t {1024};
constexpr auto handlerCaptureTimeoutSeconds = 60;

constexpr auto terminateReason = std::string_view {"std::terminate"};
constexpr auto unknownExceptionType = std::string_view {"unknown exception"};
constexpr auto messageSeparator = std::string_view {": "};

struct CaughtSignal
{
    int number;
    std::string_view name;
};

constexpr auto caughtSignals = std::to_array<CaughtSignal>({
    {SIGSEGV, "SIGSEGV"},
    {SIGBUS, "SIGBUS"},
    {SIGILL, "SIGILL"},
    {SIGFPE, "SIGFPE"},
    {SIGABRT, "SIGABRT"},
    {SIGTRAP, "SIGTRAP"},
    {SIGSYS, "SIGSYS"},
});

using SignalAction = struct sigaction;

// Everything the signal handler touches, laid out by start().
struct PosixCrashClient
{
    std::optional<std::reference_wrapper<SharedState>> state;
    pid_t handlerProcessId = 0;
    int sharedStateDescriptor = -1;
    int crashPipeWriteEnd = -1;
    int readyPipeReadEnd = -1;

    // The handler took this task's exception port, so a signal's default
    // action reaches it as EXC_CRASH.
    bool isMachCaptureActive = false;
    char* demangleBuffer = nullptr;
    std::size_t demangleBufferLength = 0;
    std::atomic<bool> isCrashing {false};
    std::array<SignalAction, caughtSignals.size()> previousSignalActions {};
    std::terminate_handler previousTerminateHandler = nullptr;
};

PosixCrashClient& getPosixCrashClient()
{
    static auto client = PosixCrashClient {};
    return client;
}

std::string_view findCaughtSignalName(int signalNumber)
{
    for (const auto& caught: caughtSignals)
    {
        if (caught.number == signalNumber)
            return caught.name;
    }

    return "SIGNAL";
}

bool hasRecordedReason(const SharedCrashInfo& crash)
{
    return crash.reason[0] != '\0';
}

void recordSignal(SharedCrashInfo& crash,
                  int signalNumber,
                  const siginfo_t& info,
                  const ucontext_t& context)
{
    if (!hasRecordedReason(crash))
        copyBoundedText(crash.reason, findCaughtSignalName(signalNumber));

    auto faultAddress = reinterpret_cast<std::uintptr_t>(info.si_addr);
    crash.code = static_cast<std::uint64_t>(signalNumber);
    crash.faultAddress = faultAddress;
    crash.threadId = getCurrentThreadId();
    crash.frames[0] = getInterruptedProgramCounter(context);

    auto returnAddresses = std::array<void*, maxCrashFrames - 1> {};
    auto capacity = static_cast<int>(returnAddresses.size());
    auto count = ::backtrace(returnAddresses.data(), capacity);

    for (auto index = 0; index < count; ++index)
    {
        auto address = reinterpret_cast<std::uintptr_t>(returnAddresses[index]);
        crash.frames[index + 1] = stripPointerAuthentication(address);
    }

    crash.frameCount = static_cast<std::uint32_t>(count + 1);
}

bool notifyHandlerProcess(const PosixCrashClient& client, char notification)
{
    while (true)
    {
        auto written =
            writeWithoutBrokenPipeSignal(client.crashPipeWriteEnd, notification);

        if (written == 1)
            return true;

        if (written < 0 && errno == EINTR)
            continue;

        return false;
    }
}

// The handler ends this process once the report is written; the deadline is
// only for a handler that dies part way.
void waitForHandlerToEndProcess()
{
    for (auto second = 0; second < handlerCaptureTimeoutSeconds; ++second)
        ::sleep(1);
}

void restoreDefaultAndRaise(int signalNumber)
{
    auto action = SignalAction {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    ::sigaction(signalNumber, &action, nullptr);
    ::raise(signalNumber);
}

// Async-signal-safe: the handler is this process's child, so a reaped or
// finished one shows here.
bool isHandlerAlive(const PosixCrashClient& client)
{
    auto status = 0;
    return client.handlerProcessId != 0
           && ::waitpid(client.handlerProcessId, &status, WNOHANG) == 0;
}

void handleCrashSignal(int signalNumber, siginfo_t* info, void* context)
{
    auto& client = getPosixCrashClient();

    if (client.isCrashing.exchange(true) || !client.state)
    {
        waitForHandlerToEndProcess();
        restoreDefaultAndRaise(signalNumber);
        return;
    }

    // The default action ends the process through EXC_CRASH, which the
    // handler captures from outside with the reason already recorded.
    if (client.isMachCaptureActive && isHandlerAlive(client))
    {
        restoreDefaultAndRaise(signalNumber);
        return;
    }

    auto& state = client.state->get();
    const auto& userContext = *static_cast<const ucontext_t*>(context);
    recordSignal(state.crash, signalNumber, *info, userContext);
    state.tableSequence.fetch_add(2, std::memory_order_release);

    if (notifyHandlerProcess(client, crashNotificationByte))
        waitForHandlerToEndProcess();
    else

        restoreDefaultAndRaise(signalNumber);
}

std::string_view demangleExceptionType(const std::type_info& type)
{
    auto& client = getPosixCrashClient();
    auto status = 0;
    auto* demangled = abi::__cxa_demangle(
        type.name(), client.demangleBuffer, &client.demangleBufferLength, &status);

    if (status != 0 || demangled == nullptr)
        return type.name();

    client.demangleBuffer = demangled;
    return demangled;
}

void recordCurrentException()
{
    auto exception = std::current_exception();

    if (!exception)
    {
        recordCrashMessage(terminateReason, {}, {});
        return;
    }

    try
    {
        std::rethrow_exception(exception);
    }
    catch (const std::exception& error)
    {
        recordCrashMessage(
            terminateReason, demangleExceptionType(typeid(error)), error.what());
    }
    catch (...)
    {
        recordCrashMessage(terminateReason, unknownExceptionType, {});
    }
}

[[noreturn]] void handleTerminate()
{
    if (auto& client = getPosixCrashClient(); client.state)
        recordThrowSiteOfCurrentException(client.state->get().crash);

    recordCurrentException();
    std::abort();
}

void appendBoundedText(BoundedText<maxCrashMessageLength>& destination,
                       std::size_t& length,
                       std::string_view text)
{
    auto available = destination.size() - 1 - length;
    auto count = std::min(text.size(), available);
    std::copy_n(text.data(), count, destination.data() + length);
    length += count;
    destination[length] = '\0';
}

void installSignalHandlers()
{
    auto action = SignalAction {};
    action.sa_sigaction = &handleCrashSignal;
    action.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigemptyset(&action.sa_mask);

    auto& previous = getPosixCrashClient().previousSignalActions;

    for (auto index = std::size_t {0}; index < caughtSignals.size(); ++index)
        ::sigaction(caughtSignals[index].number, &action, &previous[index]);
}

void restorePreviousSignalHandlers()
{
    const auto& previous = getPosixCrashClient().previousSignalActions;

    for (auto index = std::size_t {0}; index < caughtSignals.size(); ++index)
        ::sigaction(caughtSignals[index].number, &previous[index], nullptr);
}

void waitForHandlerExit(pid_t handlerProcessId)
{
    auto status = 0;

    while (::waitpid(handlerProcessId, &status, 0) < 0 && errno == EINTR)
        continue;
}

void primeBacktrace()
{
    auto returnAddresses = std::array<void*, 1> {};
    ::backtrace(returnAddresses.data(), static_cast<int>(returnAddresses.size()));
}

void prepareDemangleBuffer()
{
    auto& client = getPosixCrashClient();
    client.demangleBufferLength = demangleBufferSize;
    client.demangleBuffer = static_cast<char*>(::malloc(demangleBufferSize));
}

void setCloseOnExec(int descriptor)
{
    ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
}

struct PipeEnds
{
    int readEnd = -1;
    int writeEnd = -1;
};

std::optional<PipeEnds> createPipe()
{
    auto ends = std::array<int, 2> {};

    if (::pipe(ends.data()) != 0)
        return std::nullopt;

    setCloseOnExec(ends[0]);
    setCloseOnExec(ends[1]);
    return PipeEnds {ends[0], ends[1]};
}

// A copy numbered past the handler's fixed slots, so moving one end into its
// slot can never overwrite another end still waiting to move, and dup2 onto
// itself never leaves close-on-exec set.
struct DescriptorAboveHandlerSlots
{
    explicit DescriptorAboveHandlerSlots(int original)
        : descriptor(::fcntl(
              original, F_DUPFD_CLOEXEC, firstFileDescriptorAfterHandlerSlots))
    {
    }

    ~DescriptorAboveHandlerSlots() { ::close(descriptor); }

    int descriptor;
};

Vector<char*> makeArgumentPointers(Vector<std::string>& arguments)
{
    auto pointers = Vector<char*> {};

    for (auto& argument: arguments)
        pointers.add(argument.data());

    pointers.add(nullptr);
    return pointers;
}

std::optional<pid_t> spawnHandler(const HandlerLaunch& launch,
                                  int sharedDescriptor,
                                  const PipeEnds& crashPipe,
                                  const PipeEnds& readyPipe,
                                  const HandlerHandshake& handshake)
{
    auto arguments =
        makeHandlerCommandLine(launch, handshake.getConnectionArgument());
    auto argumentPointers = makeArgumentPointers(arguments);

    auto actions = posix_spawn_file_actions_t {};
    ::posix_spawn_file_actions_init(&actions);
    auto movedShared = DescriptorAboveHandlerSlots {sharedDescriptor};
    auto movedCrashPipe = DescriptorAboveHandlerSlots {crashPipe.readEnd};
    auto movedReadyPipe = DescriptorAboveHandlerSlots {readyPipe.writeEnd};
    ::posix_spawn_file_actions_adddup2(
        &actions, movedShared.descriptor, handlerSharedMemoryFileDescriptor);
    ::posix_spawn_file_actions_adddup2(
        &actions, movedCrashPipe.descriptor, handlerCrashPipeFileDescriptor);
    ::posix_spawn_file_actions_adddup2(
        &actions, movedReadyPipe.descriptor, handlerReadyPipeFileDescriptor);

    auto attributes = posix_spawnattr_t {};
    ::posix_spawnattr_init(&attributes);
    keepOnlyHandlerDescriptors(actions, attributes);
    handshake.addToSpawnAttributes(attributes);

    auto processId = pid_t {};
    auto result = ::posix_spawn(&processId,
                                launch.executable.str().c_str(),
                                &actions,
                                &attributes,
                                argumentPointers.data(),
                                environ);

    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    if (result != 0)
        return std::nullopt;

    return processId;
}

bool waitForHandlerReady(int readyReadEnd, Time::MS timeout)
{
    auto descriptor = pollfd {readyReadEnd, POLLIN, 0};
    auto timeoutMilliseconds = static_cast<int>(timeout.count);

    if (::poll(&descriptor, 1, timeoutMilliseconds) <= 0)
        return false;

    auto byte = char {};
    return ::read(readyReadEnd, &byte, 1) == 1 && byte == handlerReadyByte;
}

} // namespace

void recordCrashMessage(std::string_view reason,
                        std::string_view name,
                        std::string_view detail)
{
    auto& client = getPosixCrashClient();

    if (!client.state)
        return;

    auto& crash = client.state->get().crash;

    if (hasRecordedReason(crash))
        return;

    copyBoundedText(crash.reason, reason);

    auto length = std::size_t {0};
    appendBoundedText(crash.message, length, name);

    if (!name.empty() && !detail.empty())
        appendBoundedText(crash.message, length, messageSeparator);

    appendBoundedText(crash.message, length, detail);
}

std::optional<std::reference_wrapper<SharedState>> findInstalledSharedState()
{
    return getPosixCrashClient().state;
}

bool notifyHandlerOfHang()
{
    auto& client = getPosixCrashClient();

    if (client.crashPipeWriteEnd < 0 || client.isCrashing.load())
        return false;

    return notifyHandlerProcess(client, hangNotificationByte);
}

void installCrashHandlers(SharedState& state)
{
    auto& client = getPosixCrashClient();
    client.state = state;

    primeBacktrace();
    prepareDemangleBuffer();
    startRecordingLoadedModules();
    installSignalHandlers();
    client.previousTerminateHandler = std::set_terminate(&handleTerminate);
    installPlatformCrashHandlers();
}

void uninstallCrashHandlers()
{
    auto& client = getPosixCrashClient();
    restorePreviousSignalHandlers();
    uninstallPlatformCrashHandlers();
    client.isMachCaptureActive = false;
    std::set_terminate(client.previousTerminateHandler);
}

// EOF on the crash pipe with no byte before it is the handler's clean exit.
void stopHandlerProcess(SharedState& state)
{
    auto& client = getPosixCrashClient();
    state.stopRequested.store(1, std::memory_order_release);

    if (client.crashPipeWriteEnd >= 0)
    {
        ::close(client.crashPipeWriteEnd);
        client.crashPipeWriteEnd = -1;
    }

    if (client.handlerProcessId != 0)
    {
        waitForHandlerExit(client.handlerProcessId);
        client.handlerProcessId = 0;
    }
}

SharedState& createSharedState(const HandlerLaunch&)
{
    auto& client = getPosixCrashClient();
    auto descriptor = createSharedMemoryFile();
    auto size = static_cast<off_t>(sizeof(SharedState));

    if (descriptor >= 0 && ::ftruncate(descriptor, size) == 0)
    {
        auto* mapping = ::mmap(nullptr,
                               sizeof(SharedState),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               descriptor,
                               0);

        if (mapping != MAP_FAILED)
        {
            setCloseOnExec(descriptor);
            client.sharedStateDescriptor = descriptor;
            auto& state = *static_cast<SharedState*>(mapping);
            initialiseSharedState(state, getCurrentProcessId());
            return state;
        }
    }

    static auto privateState = SharedState {};
    initialiseSharedState(privateState, getCurrentProcessId());
    return privateState;
}

bool launchHandlerProcess(const HandlerLaunch& launch, SharedState&)
{
    auto& client = getPosixCrashClient();
    auto crashPipe = createPipe();
    auto readyPipe = createPipe();

    if (client.sharedStateDescriptor < 0 || !crashPipe || !readyPipe)
        return false;

    auto handshake = HandlerHandshake {launch};
    auto spawned = spawnHandler(
        launch, client.sharedStateDescriptor, *crashPipe, *readyPipe, handshake);
    ::close(crashPipe->readEnd);
    ::close(readyPipe->writeEnd);

    client.isMachCaptureActive =
        handshake.finishAfterSpawn(spawned.has_value(), launch.startTimeout);

    auto ready = spawned.has_value()
                 && waitForHandlerReady(readyPipe->readEnd, launch.startTimeout);
    ::close(readyPipe->readEnd);

    if (!ready)
    {
        ::close(crashPipe->writeEnd);
        return false;
    }

    preventBrokenPipeSignal(crashPipe->writeEnd);
    client.crashPipeWriteEnd = crashPipe->writeEnd;
    client.handlerProcessId = *spawned;
    return true;
}

void killHandlerProcess()
{
    auto& client = getPosixCrashClient();

    if (client.handlerProcessId == 0)
        return;

    ::kill(client.handlerProcessId, SIGKILL);
    waitForHandlerExit(client.handlerProcessId);
    client.handlerProcessId = 0;
}

bool isHandlerProcessRunning()
{
    auto& client = getPosixCrashClient();

    if (client.handlerProcessId == 0)
        return false;

    auto status = 0;
    return ::waitpid(client.handlerProcessId, &status, WNOHANG) == 0;
}

std::uint64_t getCurrentProcessId()
{
    return static_cast<std::uint64_t>(::getpid());
}

} // namespace eacp::CrashReporting::Detail
