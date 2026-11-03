#include "CrashReportingPlatform.h"

// Linux has no hook that runs on every thread's start and end; until
// pthread_create is interposed, only threads that install their own stack
// get one, and an ended thread's watch slot stays claimed.
namespace eacp::CrashReporting::Detail
{
void setStackOverflowStackOnNewThreads(bool) {}

void setReleasesWatchOnThreadEnd(bool) {}
} // namespace eacp::CrashReporting::Detail
