#include "ImageUuid.h"

#include <eacp/Core/Utils/Format.h>
#include <ea_data_structures/Utilities/VectorUtilities.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto uuidGroupEnds = std::to_array<std::size_t>({4, 6, 8, 10});
} // namespace

std::string formatImageUuid(const ImageUuid& bytes)
{
    auto text = std::string {};

    for (auto index = std::size_t {0}; index < bytes.size(); ++index)
    {
        if (EA::Vectors::contains(uuidGroupEnds, index))
            text += '-';

        text += Strings::format("{:02X}", bytes[index]);
    }

    return text;
}
} // namespace eacp::CrashReporting::Detail
