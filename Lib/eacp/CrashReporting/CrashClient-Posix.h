#pragma once

#include <string_view>

namespace eacp::CrashReporting::Detail
{
// Tells the handler the hang record in the shared block is filled in. False
// with no handler, or once the app is crashing.
bool notifyHandlerOfHang();

// Records why the app is about to abort, so the SIGABRT that follows reports
// it rather than its own name. Joins name and detail without allocating.
void recordCrashMessage(std::string_view reason,
                        std::string_view name,
                        std::string_view detail);
} // namespace eacp::CrashReporting::Detail
