#include "CrashChannel-Posix.h"
#include "CrashHandlerChannel-Posix.h"
#include "CrashReportingPlatform.h"
#include "ElfImages-Linux.h"
#include "SymbolNames-Posix.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>
#include <ea_data_structures/Structures/MapVector.h>

#include <csignal>
#include <cstdlib>

namespace eacp::CrashReporting::Detail
{
namespace
{
// A /proc/<pid>/maps line: range, permissions, file offset, device, inode,
// then the path, padded with spaces.
constexpr auto mapsFieldCount = 6;
constexpr auto mapsRangeField = 0;
constexpr auto mapsOffsetField = 2;
constexpr auto mapsPathField = 5;

struct AppModule
{
    std::string path;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

struct AppMapping
{
    std::string path;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t fileOffset = 0;
};

FilePath getProcessMapsPath(std::uint64_t processId)
{
    return FilePath {"/proc"} / std::to_string(processId) / "maps";
}

std::optional<AppMapping> parseMapsLine(std::string_view line)
{
    auto fields = Strings::split(line, ' ', mapsFieldCount);

    if (fields.size() < mapsFieldCount)
        return std::nullopt;

    auto path = Strings::trim(fields[mapsPathField]);
    auto range = Strings::split(fields[mapsRangeField], '-');

    if (!path.starts_with('/') || range.size() != 2)
        return std::nullopt;

    auto start = Strings::parseHex(range[0]);
    auto end = Strings::parseHex(range[1]);
    auto fileOffset = Strings::parseHex(fields[mapsOffsetField]);

    if (!start || !end || !fileOffset)
        return std::nullopt;

    return AppMapping {std::string {path}, *start, *end, *fileOffset};
}

// One module per mapped file, from its first byte's mapping to the end of
// its last; the app is stopped in its signal handler while this is read.
Vector<AppModule> readAppModules(std::uint64_t processId)
{
    auto modules = Vector<AppModule> {};

    for (const auto& line:
         Strings::split(Files::readFile(getProcessMapsPath(processId)), '\n'))
    {
        auto mapping = parseMapsLine(line);

        if (!mapping)
            continue;

        auto known = std::ranges::find(modules, mapping->path, &AppModule::path);

        if (known != modules.end())
            known->end = std::max(known->end, mapping->end);
        else if (mapping->fileOffset == 0)
            modules.add({mapping->path, mapping->start, mapping->end});
    }

    return modules;
}

void copyModulesIntoCrashInfo(SharedCrashInfo& crash,
                              const Vector<AppModule>& modules)
{
    auto count =
        std::min(static_cast<std::size_t>(modules.size()), maxSharedModules);

    for (auto index = std::size_t {0}; index < count; ++index)
    {
        const auto& appModule = modules[static_cast<int>(index)];
        auto& module = crash.modules[index];
        module.base = appModule.start;
        module.size = appModule.end - appModule.start;
        copyBoundedText(module.path, appModule.path);
    }

    crash.moduleCount = static_cast<std::uint32_t>(count);
}

using ElfSymbolReaders = EA::MapVector<std::string, OwningPointer<ElfSymbolReader>>;

const ElfSymbolReader& getOrOpenSymbolReader(ElfSymbolReaders& readers,
                                             const std::string& path)
{
    return readers.getOrCreate(path).getOrCreate(FilePath {path});
}

std::string resolveFunctionName(const ElfSymbolReader& reader,
                                std::uint64_t offset,
                                std::uint64_t lookupBackoff)
{
    auto symbol = reader.findFunctionContainingOffset(offset - lookupBackoff);

    if (!symbol)
        return {};

    return demangleSymbol(symbol->mangledName.c_str()) + " + "
           + std::to_string(symbol->distance + lookupBackoff);
}

// A return address points past its call, at whatever follows, which after a
// call that never returns is the next function; one byte back is the call.
StackFrame makeStackFrame(const SharedCrashInfo& crash,
                          ElfSymbolReaders& readers,
                          std::uint64_t address,
                          bool isReturnAddress)
{
    auto frame = StackFrame {};
    auto index = findSharedModuleIndex(crash, address);

    if (!index)
    {
        frame.offset = address;
        return frame;
    }

    const auto& module = crash.modules[*index];
    frame.modulePath = std::string {readBoundedText(module.path)};
    frame.moduleBase = module.base;
    frame.offset = address - module.base;

    const auto& reader = getOrOpenSymbolReader(readers, frame.modulePath);
    auto lookupBackoff = std::uint64_t {isReturnAddress ? 1u : 0u};
    frame.symbol = resolveFunctionName(reader, frame.offset, lookupBackoff);
    frame.moduleUuid = reader.getBuildUuid();
    return frame;
}

// The first address is the faulting instruction. backtrace() walked from the
// signal handler, through the kernel's signal frame, and reports that same
// instruction again where the interrupted code begins: everything before it
// is the reporter's own.
std::size_t countSignalHandlerFrames(const SharedCrashInfo& crash,
                                     std::size_t frameCount)
{
    for (auto index = std::size_t {1}; index < frameCount; ++index)
    {
        if (crash.frames[index] == crash.frames[0])
            return index;
    }

    return 0;
}

Vector<StackFrame> resolveCrashFrames(const SharedCrashInfo& crash)
{
    auto readers = ElfSymbolReaders {};
    auto count =
        std::min(static_cast<std::size_t>(crash.frameCount), maxCrashFrames);
    auto frames = Vector<StackFrame> {};

    if (count == 0)
        return frames;

    frames.add(makeStackFrame(crash, readers, crash.frames[0], false));

    for (auto index = countSignalHandlerFrames(crash, count) + 1; index < count;
         ++index)
        frames.add(makeStackFrame(crash, readers, crash.frames[index], true));

    return frames;
}

int handleCrash(const ReportDatabase& database, const SharedState& state)
{
    auto crash = makeOwned<SharedCrashInfo>(state.crash);
    copyModulesIntoCrashInfo(*crash, readAppModules(state.appProcessId));

    auto report = beginHandlerReport(database, state, CaptureSource::handler);
    report.crashingThread.frames = resolveCrashFrames(*crash);
    finishHandlerReport(database, report, state);

    ::kill(static_cast<pid_t>(state.appProcessId), SIGKILL);
    return deliverReportInHandler(report);
}
} // namespace

int runHandlerProcess(const HandlerArguments& arguments)
{
    if (arguments.channel != signalOnlyConnectionArgument)
        return EXIT_FAILURE;

    auto state = mapInheritedSharedState();

    if (!state)
        return EXIT_FAILURE;

    signalHandlerReady();
    auto database = ReportDatabase {arguments.databaseDirectory};

    while (true)
    {
        auto event = waitForCrashPipeEvent();

        if (event == CrashPipeEvent::appExited)
            return EXIT_SUCCESS;

        if (event == CrashPipeEvent::crash)
            return handleCrash(database, *state);
    }
}
} // namespace eacp::CrashReporting::Detail
