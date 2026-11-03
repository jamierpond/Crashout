#pragma once

#include <cstdint>

namespace eacp::CrashReporting::Detail
{
// Return addresses on arm64e carry a pointer authentication code above the
// address bits.
std::uint64_t stripPointerAuthentication(std::uint64_t address);
} // namespace eacp::CrashReporting::Detail
