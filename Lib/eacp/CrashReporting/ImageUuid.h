#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace eacp::CrashReporting::Detail
{
using ImageUuid = std::array<std::uint8_t, 16>;

// 8-4-4-4-12 upper-case hex, as dwarfdump prints an LC_UUID.
std::string formatImageUuid(const ImageUuid& bytes);
} // namespace eacp::CrashReporting::Detail
