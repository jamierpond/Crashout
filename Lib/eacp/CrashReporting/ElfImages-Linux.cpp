#include "ElfImages-Linux.h"
#include "ReadValue.h"

#include <elf.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto gnuNoteOwner = std::string_view {"GNU"};
constexpr auto noteFieldAlignment = std::size_t {4};

std::size_t alignNoteField(std::size_t size)
{
    return (size + noteFieldAlignment - 1) & ~(noteFieldAlignment - 1);
}

ImageUuid makeImageUuid(Span<const std::uint8_t> buildId)
{
    auto uuid = ImageUuid {};
    auto count = std::min(buildId.getSize(), uuid.size());
    std::copy_n(buildId.data(), count, uuid.begin());
    return uuid;
}

// A note section or segment: Elf64_Nhdr, the owner's name and the description,
// each padded to four bytes.
std::optional<ImageUuid> findBuildIdInNotes(Span<const std::uint8_t> notes)
{
    auto offset = std::size_t {0};

    while (auto header = readValue<Elf64_Nhdr>(notes, offset))
    {
        auto nameStart = offset + sizeof(Elf64_Nhdr);
        auto descriptionStart = nameStart + alignNoteField(header->n_namesz);
        auto end = descriptionStart + alignNoteField(header->n_descsz);

        if (descriptionStart + header->n_descsz > notes.getSize())
            return std::nullopt;

        const auto* name = reinterpret_cast<const char*>(notes.data() + nameStart);
        auto owner = std::string_view {name, ::strnlen(name, header->n_namesz)};

        if (header->n_type == NT_GNU_BUILD_ID && owner == gnuNoteOwner)
            return makeImageUuid(
                Span {notes.data() + descriptionStart, header->n_descsz});

        offset = end;
    }

    return std::nullopt;
}

bool hasElfMagic(const Elf64_Ehdr& header)
{
    return std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0
           && header.e_ident[EI_CLASS] == ELFCLASS64;
}

const Elf64_Phdr* getLoadedProgramHeaders(const Elf64_Ehdr& header)
{
    const auto* imageStart = reinterpret_cast<const std::uint8_t*>(&header);
    return reinterpret_cast<const Elf64_Phdr*>(imageStart + header.e_phoff);
}

// The address the image's first byte is linked at: 0 for a shared library or
// a position-independent executable.
std::uint64_t getLinkedImageStart(const Elf64_Phdr* headers, std::size_t count)
{
    for (auto index = std::size_t {0}; index < count; ++index)
    {
        if (headers[index].p_type == PT_LOAD)
            return headers[index].p_vaddr - headers[index].p_offset;
    }

    return 0;
}

std::optional<Elf64_Shdr> readSection(Span<const std::uint8_t> bytes,
                                      const Elf64_Ehdr& header,
                                      std::size_t index)
{
    return readValue<Elf64_Shdr>(bytes, header.e_shoff + index * sizeof(Elf64_Shdr));
}

std::optional<Elf64_Shdr> findSectionOfType(Span<const std::uint8_t> bytes,
                                            const Elf64_Ehdr& header,
                                            std::uint32_t type)
{
    for (auto index = std::size_t {0}; index < header.e_shnum; ++index)
    {
        auto section = readSection(bytes, header, index);

        if (section && section->sh_type == type)
            return section;
    }

    return std::nullopt;
}

bool isDefinedFunction(const Elf64_Sym& symbol)
{
    auto type = ELF64_ST_TYPE(symbol.st_info);
    return (type == STT_FUNC || type == STT_GNU_IFUNC)
           && symbol.st_shndx != SHN_UNDEF;
}

// A symbol with no size runs up to the next one, so only a sized one that
// ends before the address is passed over.
bool mayContain(const Elf64_Sym& symbol, std::uint64_t address)
{
    if (symbol.st_value > address)
        return false;

    return symbol.st_size == 0 || address < symbol.st_value + symbol.st_size;
}

std::optional<std::string_view> readSymbolName(Span<const std::uint8_t> bytes,
                                               const Elf64_Shdr& names,
                                               std::uint32_t nameOffset)
{
    auto offset = names.sh_offset + nameOffset;

    if (nameOffset >= names.sh_size || offset >= bytes.getSize())
        return std::nullopt;

    const auto* start = reinterpret_cast<const char*>(bytes.data() + offset);
    return std::string_view {start, ::strnlen(start, bytes.getSize() - offset)};
}
} // namespace

std::optional<ImageUuid> findLoadedImageBuildId(const void* imageBase)
{
    const auto& header = *static_cast<const Elf64_Ehdr*>(imageBase);

    if (!hasElfMagic(header))
        return std::nullopt;

    const auto* headers = getLoadedProgramHeaders(header);
    auto loadBias = reinterpret_cast<std::uintptr_t>(imageBase)
                    - getLinkedImageStart(headers, header.e_phnum);

    for (auto index = std::size_t {0}; index < header.e_phnum; ++index)
    {
        const auto& segment = headers[index];

        if (segment.p_type != PT_NOTE)
            continue;

        const auto* notes =
            reinterpret_cast<const std::uint8_t*>(loadBias + segment.p_vaddr);

        if (auto buildId = findBuildIdInNotes(Span {notes, segment.p_memsz}))
            return buildId;
    }

    return std::nullopt;
}

ElfSymbolReader::ElfSymbolReader(const FilePath& path)
    : file(path)
{
    if (!file.isValid())
        return;

    auto header = readValue<Elf64_Ehdr>(file.bytes(), 0);
    isElf = header && hasElfMagic(*header);
}

bool ElfSymbolReader::isValid() const
{
    return isElf;
}

std::string ElfSymbolReader::getBuildUuid() const
{
    if (!isElf)
        return {};

    auto bytes = file.bytes();
    auto header = *readValue<Elf64_Ehdr>(bytes, 0);

    for (auto index = std::size_t {0}; index < header.e_shnum; ++index)
    {
        auto section = readSection(bytes, header, index);

        if (!section || section->sh_type != SHT_NOTE
            || section->sh_offset + section->sh_size > bytes.getSize())
            continue;

        auto notes = bytes.subspan(section->sh_offset, section->sh_size);

        if (auto buildId = findBuildIdInNotes(notes))
            return formatImageUuid(*buildId);
    }

    return {};
}

std::optional<FunctionContainingOffset>
    ElfSymbolReader::findFunctionContainingOffset(std::uint64_t offset) const
{
    if (!isElf)
        return std::nullopt;

    auto bytes = file.bytes();
    auto header = *readValue<Elf64_Ehdr>(bytes, 0);
    auto symbols = findSectionOfType(bytes, header, SHT_SYMTAB);

    if (!symbols)
        symbols = findSectionOfType(bytes, header, SHT_DYNSYM);

    if (!symbols || symbols->sh_entsize != sizeof(Elf64_Sym))
        return std::nullopt;

    auto names = readSection(bytes, header, symbols->sh_link);

    if (!names)
        return std::nullopt;

    auto programHeaders = Vector<Elf64_Phdr> {};

    for (auto index = std::size_t {0}; index < header.e_phnum; ++index)
    {
        auto offsetOfHeader = header.e_phoff + index * sizeof(Elf64_Phdr);

        if (auto programHeader = readValue<Elf64_Phdr>(bytes, offsetOfHeader))
            programHeaders.add(*programHeader);
    }

    auto target =
        getLinkedImageStart(programHeaders.data(),
                            static_cast<std::size_t>(programHeaders.size()))
        + offset;
    auto symbolCount = symbols->sh_size / sizeof(Elf64_Sym);
    auto best = std::optional<Elf64_Sym> {};

    for (auto index = std::size_t {0}; index < symbolCount; ++index)
    {
        auto symbol = readValue<Elf64_Sym>(
            bytes, symbols->sh_offset + index * sizeof(Elf64_Sym));

        if (!symbol)
            return std::nullopt;

        auto isCandidate = isDefinedFunction(*symbol) && mayContain(*symbol, target);

        if (isCandidate && (!best || symbol->st_value > best->st_value))
            best = symbol;
    }

    if (!best)
        return std::nullopt;

    auto name = readSymbolName(bytes, *names, best->st_name);

    if (!name)
        return std::nullopt;

    return FunctionContainingOffset {std::string {*name}, target - best->st_value};
}
} // namespace eacp::CrashReporting::Detail
