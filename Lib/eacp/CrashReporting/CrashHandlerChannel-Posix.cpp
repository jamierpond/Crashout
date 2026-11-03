#include "CrashHandlerChannel-Posix.h"
#include "CrashChannel-Posix.h"

#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>

namespace eacp::CrashReporting::Detail
{
CrashPipeEvent waitForCrashPipeEvent()
{
    auto byte = char {};

    while (true)
    {
        auto count = ::read(handlerCrashPipeFileDescriptor, &byte, 1);

        if (count < 0 && errno == EINTR)
            continue;

        if (count == 1 && byte == crashNotificationByte)
            return CrashPipeEvent::crash;

        if (count == 1 && byte == hangNotificationByte)
            return CrashPipeEvent::hang;

        return CrashPipeEvent::appExited;
    }
}

std::optional<std::reference_wrapper<SharedState>> mapInheritedSharedState()
{
    auto* mapping = ::mmap(nullptr,
                           sizeof(SharedState),
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           handlerSharedMemoryFileDescriptor,
                           0);

    if (mapping == MAP_FAILED)
        return std::nullopt;

    auto& state = *static_cast<SharedState*>(mapping);

    if (!isSharedStateValid(state))
        return std::nullopt;

    return state;
}

void signalHandlerReady()
{
    [[maybe_unused]] auto written =
        ::write(handlerReadyPipeFileDescriptor, &handlerReadyByte, 1);
    ::close(handlerReadyPipeFileDescriptor);
}
} // namespace eacp::CrashReporting::Detail
