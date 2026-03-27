cmake_minimum_required(VERSION 3.16)
project(audio-driver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Include paths ────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ─── Platform detection ───────────────────────────────────────
if(APPLE)
    set(AUDIO_DRIVER_PLATFORM "macos")
    find_library(COREAUDIO_LIB CoreAudio)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    set(PLATFORM_LIBS ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB} ${COREFOUNDATION_LIB})
elseif(WIN32)
    set(AUDIO_DRIVER_PLATFORM "windows")
    set(PLATFORM_LIBS ole32 uuid)
else()
    message(FATAL_ERROR "Unsupported platform. audio-driver requires macOS or Windows.")
endif()

# ─── Library (models, utils, platform) ────────────────────────
add_library(audio-driver-core STATIC
    src/platform/PlatformFactory.cpp
)
target_include_directories(audio-driver-core PUBLIC include)
target_link_libraries(audio-driver-core ${PLATFORM_LIBS})

# ─── Build version from git ───────────────────────────────────
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
endif()

execute_process(
    COMMAND git log -1 --format=%cd --date=short
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_COMMIT_DATE)
    set(GIT_COMMIT_DATE "unknown")
endif()

# ─── Driver executable ────────────────────────────────────────
add_executable(audio-driver
    src/main.cpp
)
target_compile_definitions(audio-driver PRIVATE
    AUDIO_DRIVER_VERSION="${PROJECT_VERSION}"
    AUDIO_DRIVER_COMMIT="${GIT_COMMIT_HASH}"
    AUDIO_DRIVER_BUILD_DATE="${GIT_COMMIT_DATE}"
)
target_link_libraries(audio-driver audio-driver-core ${PLATFORM_LIBS})

# ─── Tests ────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    enable_testing()

    # Fetch GoogleTest
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(audio-driver-tests
        tests/test_AudioFormat.cpp
        tests/test_AudioBuffer.cpp
        tests/test_RingBuffer.cpp
        tests/test_FormatConverter.cpp
        tests/test_Resampler.cpp
        tests/test_DriverConfig.cpp
        tests/test_GatewaySession.cpp
        tests/test_DeviceController.cpp
        tests/test_AuthCredentials.cpp
        tests/test_AudioChunk.cpp
        tests/test_Logger.cpp
    )
    target_link_libraries(audio-driver-tests
        audio-driver-core
        GTest::gtest_main
        ${PLATFORM_LIBS}
    )
    target_include_directories(audio-driver-tests PRIVATE include)

    include(GoogleTest)
    gtest_discover_tests(audio-driver-tests)
endif()

# ─── Install ──────────────────────────────────────────────────
install(TARGETS audio-driver DESTINATION bin)

# macOS: BlackHole virtual audio device is an external dependency.
# Install via: brew install --cask blackhole-2ch
# Or run: sudo installer/macos/install.sh (handles BlackHole automatically)
