# A project that already added eacp keeps its own eacp and its options.
if (TARGET eacp-core)
    return()
endif ()

include(CPM)

# eacp jp/develop 55e4023b (tag jp/develop/2026-11-16.3): develop plus one commit per open eacp PR of jamierpond's.
# -DCPM_eacp_SOURCE=<checkout> builds against a local eacp instead.
set(EACP_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(EACP_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EACP_BUILD_WEBVIEW OFF CACHE BOOL "" FORCE)

CPMAddPackage(
        NAME eacp
        GITHUB_REPOSITORY eyalamirmusic/eacp
        GIT_TAG 55e4023b52d30118c083c6c1efada7cd43edf8ba)
