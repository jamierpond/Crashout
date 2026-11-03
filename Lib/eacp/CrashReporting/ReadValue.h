#pragma once

#include <eacp/Core/Utils/Common.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace eacp::CrashReporting::Detail
{
// The `Value` whose bytes start `offset` into `bytes`; none when it would
// run past the end.
template <typename Value>
std::optional<Value> readValue(Span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + sizeof(Value) > bytes.getSize())
        return std::nullopt;

    auto value = Value {};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}
} // namespace eacp::CrashReporting::Detail
