#pragma once

#include "SharedState.h"

#include <functional>
#include <optional>

// The handler's ends of what the app handed it at the fixed descriptors.
namespace eacp::CrashReporting::Detail
{
enum class CrashPipeEvent
{
    crash,
    hang,
    appExited
};

CrashPipeEvent waitForCrashPipeEvent();
std::optional<std::reference_wrapper<SharedState>> mapInheritedSharedState();
void signalHandlerReady();
} // namespace eacp::CrashReporting::Detail
