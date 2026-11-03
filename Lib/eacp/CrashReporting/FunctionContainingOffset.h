#pragma once

#include <cstdint>
#include <string>

namespace eacp::CrashReporting::Detail
{
struct FunctionContainingOffset
{
    std::string mangledName;
    std::uint64_t distance = 0;
};
} // namespace eacp::CrashReporting::Detail
