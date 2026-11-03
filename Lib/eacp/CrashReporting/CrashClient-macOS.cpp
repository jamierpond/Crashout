#include "CrashChannel-Posix.h"
#include "MachImages-macOS.h"
#include "PosixPlatform.h"
#include "TaskPortHandshake-macOS.h"
#include "ThrowSites-macOS.h"
#include "UncaughtExceptions-macOS.h"

#include <array>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mutex>
#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
std::mutex imageMutex;

void appendSharedModule(const mach_header* header, std::string_view path)
{
    auto state = findInstalledSharedState();

    if (!state)
        return;

    const auto& header64 = *reinterpret_cast<const mach_header_64*>(header);
    auto commandBytes = sizeof(mach_header_64) + header64.sizeofcmds;
    auto lock = std::lock_guard {imageMutex};
    auto& crash = state->get().crash;

    if (crash.moduleCount >= maxSharedModules)
        return;

    auto& module = crash.modules[crash.moduleCount];
    module.base = reinterpret_cast<std::uintptr_t>(header);
    module.size = getTextSegmentSize(header64, commandBytes);
    copyBoundedText(module.path, path);
    ++crash.moduleCount;
}

void recordLoadedImage(const mach_header* header, intptr_t)
{
    auto info = Dl_info {};

    if (::dladdr(header, &info) != 0 && info.dli_fname != nullptr)
        appendSharedModule(header, info.dli_fname);
}

// dyld is not one of the images it reports, and the bottom of every stack is
// in it.
void recordDynamicLinker()
{
    if (auto dynamicLinker = findDynamicLinkerImage())
        appendSharedModule(dynamicLinker->header, dynamicLinker->path);
}
} // namespace

std::uint64_t getInterruptedProgramCounter(const ucontext_t& context)
{
#if defined(__arm64__)
    auto programCounter = std::uint64_t {context.uc_mcontext->__ss.__pc};
#else
    auto programCounter = std::uint64_t {context.uc_mcontext->__ss.__rip};
#endif
    return stripPointerAuthentication(programCounter);
}

int createSharedMemoryFile()
{
    auto pattern = (FilePath::tempDirectory() / "eacp-crash-XXXXXX").str();
    auto descriptor = ::mkstemp(pattern.data());

    if (descriptor >= 0)
        ::unlink(pattern.c_str());

    return descriptor;
}

void startRecordingLoadedModules()
{
    recordDynamicLinker();
    _dyld_register_func_for_add_image(&recordLoadedImage);
}

void installPlatformCrashHandlers()
{
    startRecordingThrowSites();
    installUncaughtExceptionRecorder();
}

void uninstallPlatformCrashHandlers()
{
    stopRecordingThrowSites();
    restorePreviousExceptionPorts();
    uninstallUncaughtExceptionRecorder();
}

void recordThrowSiteOfCurrentException(SharedCrashInfo& crash)
{
    copyThrowSiteOfCurrentException(crash);
}

void keepOnlyHandlerDescriptors(posix_spawn_file_actions_t& actions,
                                posix_spawnattr_t& attributes)
{
    for (auto standardDescriptor: {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
        ::posix_spawn_file_actions_addinherit_np(&actions, standardDescriptor);

    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
}

void preventBrokenPipeSignal(int pipeWriteEnd)
{
    ::fcntl(pipeWriteEnd, F_SETNOSIGPIPE, 1);
}

ssize_t writeWithoutBrokenPipeSignal(int pipeWriteEnd, char byte)
{
    return ::write(pipeWriteEnd, &byte, 1);
}

struct HandlerHandshake::Native
{
    std::optional<HandshakePort> port;
};

HandlerHandshake::HandlerHandshake(const HandlerLaunch& launch)
{
    if (launch.captureMachExceptions)
        native->port = createHandshakePort();
}

HandlerHandshake::~HandlerHandshake()
{
    if (native->port)
        destroyHandshakePort(*native->port);
}

std::string_view HandlerHandshake::getConnectionArgument() const
{
    if (native->port)
        return machTaskPortConnectionArgument;

    return signalOnlyConnectionArgument;
}

void HandlerHandshake::addToSpawnAttributes(posix_spawnattr_t& attributes) const
{
    if (native->port)
        ::posix_spawnattr_setspecialport_np(
            &attributes, native->port->receiveRight, TASK_BOOTSTRAP_PORT);
}

// Answered before the ready byte is read: until the handler has its real
// bootstrap port back, its runtime's own lookups land on this port too, and
// are dropped here rather than left to block it.
bool HandlerHandshake::finishAfterSpawn(bool isHandlerSpawned, Time::MS timeout)
{
    if (!native->port)
        return false;

    auto isCaptureActive =
        isHandlerSpawned && answerHandshakeWithTaskPort(*native->port, timeout);
    destroyHandshakePort(*native->port);
    native->port.reset();
    return isCaptureActive;
}

std::string getLoadedImageUuid(const void* imageBase)
{
    return getImageUuid(*static_cast<const mach_header_64*>(imageBase));
}

std::string getOperatingSystemVersion()
{
    auto version = std::array<char, maxReasonLength> {};
    auto length = version.size();

    if (::sysctlbyname("kern.osproductversion", version.data(), &length, nullptr, 0)
        != 0)
        return {};

    return std::string {readBoundedText(version)};
}

std::uint64_t getCurrentThreadId()
{
    auto threadId = std::uint64_t {0};
    ::pthread_threadid_np(nullptr, &threadId);
    return threadId;
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
    ::pthread_setname_np(std::string {name}.c_str());
}
} // namespace eacp::CrashReporting::Detail
