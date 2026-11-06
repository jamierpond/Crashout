include(CPM)

# eacp jp/crashout-eacp-base: #196 merged with #197. Pin develop once both merge.
# -DCPM_eacp_SOURCE=<checkout> builds against a local eacp instead.
set(EACP_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(EACP_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EACP_BUILD_WEBVIEW OFF CACHE BOOL "" FORCE)

CPMAddPackage(
        NAME eacp
        GITHUB_REPOSITORY eyalamirmusic/eacp
        GIT_TAG 9f461ce6e3b83c11c004a027de7cff85eaa15159)
