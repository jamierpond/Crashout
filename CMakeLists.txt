cmake_minimum_required(VERSION 3.31)

project(Crashout VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 20)

if (APPLE)
    enable_language(OBJCXX)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

option(CRASHOUT_ENABLE_TESTS "Build Crashout's tests" ${PROJECT_IS_TOP_LEVEL})
option(CRASHOUT_ENABLE_EXAMPLES "Build Crashout's demo app" ${PROJECT_IS_TOP_LEVEL})

include(FindEacp)
include(CrashReporting)

if (PROJECT_IS_TOP_LEVEL)
    eacp_default_setup()
endif ()

add_subdirectory(Lib/eacp/CrashReporting)
target_include_directories(eacp-crash-report PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Lib")

if (CRASHOUT_ENABLE_EXAMPLES AND TARGET eacp-crash-reporting AND TARGET eacp-ui)
    add_subdirectory(Apps/CrashReporting)
endif ()

if (CRASHOUT_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif ()
