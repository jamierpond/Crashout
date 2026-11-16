# A project that already added eacp keeps its own eacp and its options.
if (TARGET eacp-core)
    return()
endif ()

include(CPM)

# eacp develop 3892bedb: Crashout uses nothing that is only on jp/develop.
# -DCPM_eacp_SOURCE=<checkout> builds against a local eacp instead.
set(EACP_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(EACP_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EACP_BUILD_WEBVIEW OFF CACHE BOOL "" FORCE)

CPMAddPackage(
        NAME eacp
        GITHUB_REPOSITORY eyalamirmusic/eacp
        GIT_TAG 3892bedb8e4d50001d509b14adc5a6e70ce5af88)
