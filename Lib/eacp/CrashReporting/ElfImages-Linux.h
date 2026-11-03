#pragma once

#include "FunctionContainingOffset.h"
#include "ImageUuid.h"

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/Core/Utils/MemoryMappedFile.h>

#include <cstdint>
#include <optional>
#include <string>

namespace eacp::CrashReporting::Detail
{
// The first 16 bytes of the image's GNU build id, zero-padded when it is
// shorter, read from the program headers of the image the dynamic linker
// loaded at `imageBase`.
std::optional<ImageUuid> findLoadedImageBuildId(const void* imageBase);

// Reads function names and the build id of an ELF image from its file on disk.
// The handler uses it for every module of the app, the executable included,
// since dladdr names only the symbols an image exports.
class ElfSymbolReader
{
public:
    explicit ElfSymbolReader(const FilePath& path);

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] std::string getBuildUuid() const;

    // The function a byte `offset` from the image's first mapping falls in,
    // from .symtab, or from .dynsym when the file has no .symtab.
    [[nodiscard]] std::optional<FunctionContainingOffset>
        findFunctionContainingOffset(std::uint64_t offset) const;

private:
    MemoryMappedFile file;
    bool isElf = false;
};
} // namespace eacp::CrashReporting::Detail
