cmake_minimum_required(VERSION 3.14)
project(aeron-sbe-poc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Profiling-friendly compiler flags: optimized with debug symbols and frame pointers
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -fno-omit-frame-pointer")

# ==============================================================================
# Dependencies via FetchContent
# ==============================================================================
include(FetchContent)

# Fetch Aeron from GitHub
FetchContent_Declare(
    aeron
    GIT_REPOSITORY https://github.com/real-logic/aeron.git
    GIT_TAG        1.42.1
    GIT_SHALLOW    TRUE
)

# Only build the Aeron C++ client library, skip Java and samples
set(BUILD_AERON_DRIVER ON CACHE BOOL "" FORCE)
set(AERON_TESTS OFF CACHE BOOL "" FORCE)
set(AERON_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(AERON_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(BUILD_AERON_ARCHIVE_API ON CACHE BOOL "" )

FetchContent_MakeAvailable(aeron)

# ==============================================================================
# SBE Code Generation
# ==============================================================================
set(SBE_VERSION "1.33.0")
set(SBE_JAR_URL "https://repo1.maven.org/maven2/uk/co/real-logic/sbe-all/${SBE_VERSION}/sbe-all-${SBE_VERSION}.jar")
set(SBE_JAR "${CMAKE_BINARY_DIR}/sbe-all-${SBE_VERSION}.jar")
set(SBE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated/sbe")
set(SBE_SCHEMA "${CMAKE_SOURCE_DIR}/schema/messages.xml")

# Download the SBE jar if not already present
if(NOT EXISTS "${SBE_JAR}")
    message(STATUS "Downloading SBE jar ${SBE_VERSION}...")
    file(DOWNLOAD "${SBE_JAR_URL}" "${SBE_JAR}"
         STATUS SBE_DOWNLOAD_STATUS
         SHOW_PROGRESS)
    list(GET SBE_DOWNLOAD_STATUS 0 SBE_DOWNLOAD_ERROR)
    if(SBE_DOWNLOAD_ERROR)
        message(FATAL_ERROR "Failed to download SBE jar: ${SBE_DOWNLOAD_STATUS}")
    endif()
endif()

# Create output directory for generated headers
file(MAKE_DIRECTORY "${SBE_OUTPUT_DIR}")

# Custom command to generate SBE C++ codec headers from schema
add_custom_command(
    OUTPUT "${SBE_OUTPUT_DIR}/aeron_poc/MarketData.h"
           "${SBE_OUTPUT_DIR}/aeron_poc/MessageHeader.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SBE_OUTPUT_DIR}"
    COMMAND java
            -Dsbe.output.dir=${SBE_OUTPUT_DIR}
            -Dsbe.target.language=Cpp
            -jar ${SBE_JAR}
            ${SBE_SCHEMA}
    DEPENDS "${SBE_SCHEMA}"
    COMMENT "Generating SBE C++ codecs from schema/messages.xml"
    VERBATIM
)

# Custom target to drive SBE code generation
add_custom_target(sbe_codecs
    DEPENDS "${SBE_OUTPUT_DIR}/aeron_poc/MarketData.h"
            "${SBE_OUTPUT_DIR}/aeron_poc/MessageHeader.h"
)

# ==============================================================================
# Executable Targets
# ==============================================================================

# Producer binary
add_executable(producer src/producer.cpp)
target_include_directories(producer PRIVATE "${SBE_OUTPUT_DIR}")
target_link_libraries(producer PRIVATE aeron_client)
add_dependencies(producer sbe_codecs)

# Consumer binary
add_executable(consumer src/consumer.cpp)
target_include_directories(consumer PRIVATE "${SBE_OUTPUT_DIR}")
target_link_libraries(consumer PRIVATE aeron_client)
add_dependencies(consumer sbe_codecs)

# Recorder binary (Aeron Archive record/replay)
add_executable(recorder src/recorder.cpp)
target_include_directories(recorder PRIVATE "${SBE_OUTPUT_DIR}")
target_link_libraries(recorder PRIVATE aeron_archive_client aeron_client)
add_dependencies(recorder sbe_codecs)

# ==============================================================================
# Testing: Google Test + RapidCheck
# ==============================================================================
enable_testing()

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# Prevent GoogleTest from overriding parent project compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest rapidcheck)

# Include GoogleTest CMake module for gtest_discover_tests
include(GoogleTest)

# Test executable
add_executable(aeron_archive_tests tests/test_recorder.cpp)
target_include_directories(aeron_archive_tests PRIVATE "${SBE_OUTPUT_DIR}")
target_link_libraries(aeron_archive_tests PRIVATE GTest::gtest_main rapidcheck)
add_dependencies(aeron_archive_tests sbe_codecs)

gtest_discover_tests(aeron_archive_tests)
