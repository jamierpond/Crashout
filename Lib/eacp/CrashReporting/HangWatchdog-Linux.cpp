#include "CrashReportingPlatform.h"

// Hangs are not detected on Linux yet: nothing samples a stalled thread.
namespace eacp::CrashReporting::Detail
{
void startHangWatchdog(SharedState&) {}

void stopHangWatchdog() {}

void recordThreadForSampling(SharedWatchedThread&) {}
} // namespace eacp::CrashReporting::Detail
