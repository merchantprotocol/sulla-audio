cmake_minimum_required(VERSION 3.16)
project(sulla-audio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Include paths ────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ─── Platform detection ───────────────────────────────────────
if(APPLE)
    set(SULLA_PLATFORM "macos")
    find_library(COREAUDIO_LIB CoreAudio)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    set(PLATFORM_LIBS ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB} ${COREFOUNDATION_LIB})
elseif(WIN32)
    set(SULLA_PLATFORM "windows")
    set(PLATFORM_LIBS ole32 uuid)
else()
    message(FATAL_ERROR "Unsupported platform. sulla-audio requires macOS or Windows.")
endif()

# ─── Library (models, utils, platform) ────────────────────────
add_library(sulla-audio-core STATIC
    src/platform/PlatformFactory.cpp
)
target_include_directories(sulla-audio-core PUBLIC include)
target_link_libraries(sulla-audio-core ${PLATFORM_LIBS})

# ─── Driver executable ────────────────────────────────────────
add_executable(sulla-audio-driver
    src/main.cpp
)
target_link_libraries(sulla-audio-driver sulla-audio-core ${PLATFORM_LIBS})

# ─── Tests ────────────────────────────────────────────────────
option(SULLA_BUILD_TESTS "Build unit tests" ON)

if(SULLA_BUILD_TESTS)
    enable_testing()

    # Fetch GoogleTest
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(sulla-audio-tests
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
    target_link_libraries(sulla-audio-tests
        sulla-audio-core
        GTest::gtest_main
        ${PLATFORM_LIBS}
    )
    target_include_directories(sulla-audio-tests PRIVATE include)

    include(GoogleTest)
    gtest_discover_tests(sulla-audio-tests)
endif()

# ─── Install ──────────────────────────────────────────────────
install(TARGETS sulla-audio-driver DESTINATION bin)

# macOS: BlackHole virtual audio device is an external dependency.
# Install via: brew install --cask blackhole-2ch
# Or run: sudo installer/macos/install.sh (handles BlackHole automatically)
