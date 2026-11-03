#pragma once

#include "CrashReportingPlatform.h"

#include <eacp/Core/Utils/Pimpl.h>

#include <csignal>
#include <cstdint>
#include <functional>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/ucontext.h>

// What the -Posix files, shared by macOS and Linux, ask of each platform's
// own half (CrashClient-macOS.cpp, CrashClient-Linux.cpp).
namespace eacp::CrashReporting::Detail
{
std::uint64_t getInterruptedProgramCounter(const ucontext_t& context);

// A file both processes map, which never shows in the file system. -1 when
// none could be made.
int createSharedMemoryFile();

// Called by installCrashHandlers before the signal handlers go in, and after
// std::set_terminate.
void startRecordingLoadedModules();
void installPlatformCrashHandlers();
void uninstallPlatformCrashHandlers();

// Where the exception std::terminate is about to report was thrown, when the
// platform records throws.
void recordThrowSiteOfCurrentException(SharedCrashInfo& crash);

// Leaves the handler only the three standard descriptors and the slots the
// spawn's file actions fill.
void keepOnlyHandlerDescriptors(posix_spawn_file_actions_t& actions,
                                posix_spawnattr_t& attributes);

// Makes a write to the crash pipe after the handler is gone fail with EPIPE
// rather than raise SIGPIPE, where the platform has a per-descriptor setting.
void preventBrokenPipeSignal(int pipeWriteEnd);

// One write to the crash pipe that never raises SIGPIPE: through
// preventBrokenPipeSignal's setting, or, where there is none, with SIGPIPE held
// back on the calling thread and one it raised taken back out. ::write's result.
ssize_t writeWithoutBrokenPipeSignal(int pipeWriteEnd, char byte);

// What the app and the handler set up beside the shared block and the pipes:
// the Mach handshake on macOS, nothing on Linux.
class HandlerHandshake
{
public:
    explicit HandlerHandshake(const HandlerLaunch& launch);
    ~HandlerHandshake();

    [[nodiscard]] std::string_view getConnectionArgument() const;
    void addToSpawnAttributes(posix_spawnattr_t& attributes) const;

    // After the spawn and before the ready byte, releasing the handshake
    // either way: true when the handler now captures crashes from outside.
    bool finishAfterSpawn(bool isHandlerSpawned, Time::MS timeout);

private:
    struct Native;
    Pimpl<Native> native;
};

// The block installCrashHandlers was given, until the next one replaces it.
std::optional<std::reference_wrapper<SharedState>> findInstalledSharedState();

// The build identifier of the image loaded at `imageBase`, as a report's
// moduleUuid spells it; empty when it has none.
std::string getLoadedImageUuid(const void* imageBase);
} // namespace eacp::CrashReporting::Detail
