add_library(eacp-crash-report STATIC)

target_link_libraries(eacp-crash-report PUBLIC eacp-core)

target_sources(eacp-crash-report PRIVATE
        CrashReport.cpp
        ReportDatabase.cpp)

set_default_target_setting(eacp-crash-report)
eacp_enable_unity_build(eacp-crash-report)

if (NOT ((APPLE AND NOT IOS) OR WIN32 OR (LINUX AND NOT ANDROID)))
    return ()
endif ()

add_library(eacp-crash-reporting STATIC)

target_link_libraries(eacp-crash-reporting PUBLIC eacp-crash-report)

target_sources(eacp-crash-reporting PRIVATE
        CrashHandler.cpp
        CrashReporting.cpp
        HandlerReport.cpp
        SharedState.cpp
        ThreadHeartbeats.cpp)

if (APPLE OR LINUX)
    target_sources(eacp-crash-reporting PRIVATE
            CrashClient-Posix.cpp
            CrashHandlerChannel-Posix.cpp
            EventStack-Posix.cpp
            ImageUuid.cpp
            SymbolNames-Posix.cpp
            ThreadHooks-Posix.cpp)
endif ()

if (APPLE)
    target_sources(eacp-crash-reporting PRIVATE
            CrashClient-macOS.cpp
            ThreadHooks-macOS.cpp
            CrashHandlerProcess-macOS.cpp
            HangWatchdog-macOS.cpp
            LoadedLibrarySymbolReader-macOS.cpp
            MachExceptionMessage-macOS.cpp
            MachImages-macOS.cpp
            Minidump-macOS.cpp
            TaskCapture-macOS.cpp
            TaskPortHandshake-macOS.cpp
            ThrowSites-macOS.cpp
            UncaughtExceptions-macOS.mm)
    target_link_libraries(eacp-crash-reporting PRIVATE "-framework Foundation")
elseif (LINUX)
    target_sources(eacp-crash-reporting PRIVATE
            CrashClient-Linux.cpp
            CrashHandlerProcess-Linux.cpp
            ElfImages-Linux.cpp
            HangWatchdog-Linux.cpp
            ThreadHooks-Linux.cpp)
elseif (WIN32)
    target_sources(eacp-crash-reporting PRIVATE
            CrashClient-Windows.cpp
            CppExceptions-Windows.cpp
            CrashHandlerProcess-Windows.cpp
            EventStack-Windows.cpp
            ExceptionNames-Windows.cpp
            ThreadHooks-Windows.cpp
            SymbolNames-Windows.cpp)
    target_link_libraries(eacp-crash-reporting PRIVATE dbghelp)
    add_subdirectory(WindowsErrorReporting)
endif ()

set_default_target_setting(eacp-crash-reporting)
eacp_enable_unity_build(eacp-crash-reporting)

# Deliberate faults for demo and test apps: SelfDestruct's, and any app's that
# wants the same list.
add_library(eacp-crash-faults STATIC)

target_link_libraries(eacp-crash-faults PUBLIC eacp-crash-reporting)

target_sources(eacp-crash-faults PRIVATE
        Faults/FaultDefinitions.cpp
        Faults/Faults.cpp
        Faults/Matrix.cpp)

set_default_target_setting(eacp-crash-faults)
