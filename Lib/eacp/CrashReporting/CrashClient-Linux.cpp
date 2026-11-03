#include "CrashChannel-Posix.h"
#include "ElfImages-Linux.h"
#include "PointerAuthentication.h"
#include "PosixPlatform.h"

#include <array>
#include <ctime>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto sharedMemoryFileName = "eacp-crash";

// The kernel keeps 16 bytes of a thread's name, its terminator included, and
// refuses a longer one outright.
constexpr auto threadNameLimit = std::size_t {15};

sigset_t makeBrokenPipeSignalSet()
{
    auto signals = sigset_t {};
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    return signals;
}

bool isBrokenPipeSignalPending()
{
    auto pending = sigset_t {};
    sigpending(&pending);
    return sigismember(&pending, SIGPIPE) == 1;
}
} // namespace

std::uint64_t getInterruptedProgramCounter(const ucontext_t& context)
{
#if defined(__aarch64__)
    return std::uint64_t {context.uc_mcontext.pc};
#else
    return static_cast<std::uint64_t>(context.uc_mcontext.gregs[REG_RIP]);
#endif
}

std::uint64_t stripPointerAuthentication(std::uint64_t address)
{
    return address;
}

int createSharedMemoryFile()
{
    return ::memfd_create(sharedMemoryFileName, MFD_CLOEXEC);
}

// The handler reads the app's modules from /proc once it crashes.
void startRecordingLoadedModules() {}

void installPlatformCrashHandlers() {}

void uninstallPlatformCrashHandlers() {}

void recordThrowSiteOfCurrentException(SharedCrashInfo&) {}

void keepOnlyHandlerDescriptors(posix_spawn_file_actions_t& actions,
                                posix_spawnattr_t&)
{
    ::posix_spawn_file_actions_addclosefrom_np(&actions,
                                               firstFileDescriptorAfterHandlerSlots);
}

void preventBrokenPipeSignal(int) {}

ssize_t writeWithoutBrokenPipeSignal(int pipeWriteEnd, char byte)
{
    auto brokenPipe = makeBrokenPipeSignalSet();
    auto wasAlreadyPending = isBrokenPipeSignalPending();
    auto previousMask = sigset_t {};
    ::pthread_sigmask(SIG_BLOCK, &brokenPipe, &previousMask);

    auto written = ::write(pipeWriteEnd, &byte, 1);

    if (!wasAlreadyPending && isBrokenPipeSignalPending())
    {
        auto noWait = timespec {};
        ::sigtimedwait(&brokenPipe, nullptr, &noWait);
    }

    ::pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
    return written;
}

struct HandlerHandshake::Native
{
};

HandlerHandshake::HandlerHandshake(const HandlerLaunch&) {}

HandlerHandshake::~HandlerHandshake() = default;

std::string_view HandlerHandshake::getConnectionArgument() const
{
    return signalOnlyConnectionArgument;
}

void HandlerHandshake::addToSpawnAttributes(posix_spawnattr_t&) const {}

bool HandlerHandshake::finishAfterSpawn(bool, Time::MS)
{
    return false;
}

std::string getLoadedImageUuid(const void* imageBase)
{
    auto buildId = findLoadedImageBuildId(imageBase);

    if (!buildId)
        return {};

    return formatImageUuid(*buildId);
}

std::string getOperatingSystemVersion()
{
    auto system = utsname {};

    if (::uname(&system) != 0)
        return {};

    return system.release;
}

std::uint64_t getCurrentThreadId()
{
    return static_cast<std::uint64_t>(::gettid());
}

std::string getCurrentThreadName()
{
    auto name = std::array<char, maxThreadNameLength> {};

    if (::pthread_getname_np(::pthread_self(), name.data(), name.size()) != 0)
        return {};

    return name.data();
}

void setCurrentThreadName(std::string_view name)
{
    auto kept = std::string {name.substr(0, threadNameLimit)};
    ::pthread_setname_np(::pthread_self(), kept.c_str());
}
} // namespace eacp::CrashReporting::Detail
