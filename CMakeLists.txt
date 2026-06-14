cmake_minimum_required(VERSION 3.16)

project(edge_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Shared dependencies ───────────────────────────────────────────────────────

find_package(OpenCV REQUIRED COMPONENTS core imgcodecs videoio imgproc)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(CPPHTTPLIB REQUIRED cpp-httplib)
find_library(BROTLICOMMON_LIBRARY NAMES brotlicommon REQUIRED)
find_library(BROTLIDEC_LIBRARY   NAMES brotlidec    REQUIRED)
find_library(BROTLIENC_LIBRARY   NAMES brotlienc    REQUIRED)

set(HTTPLIB_LINK_LIBS
    ${CPPHTTPLIB_LIBRARIES}
    OpenSSL::SSL OpenSSL::Crypto
    ZLIB::ZLIB
    ${BROTLICOMMON_LIBRARY} ${BROTLIDEC_LIBRARY} ${BROTLIENC_LIBRARY}
    pthread
)

# ── Target: edge-streamer (camera MJPEG server — unchanged) ──────────────────

add_executable(edge-streamer src/main.cpp)

target_include_directories(edge-streamer PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${CPPHTTPLIB_INCLUDE_DIRS}
)
target_link_libraries(edge-streamer PRIVATE
    ${OpenCV_LIBS}
    ${HTTPLIB_LINK_LIBS}
)
target_compile_options(edge-streamer PRIVATE ${CPPHTTPLIB_CFLAGS_OTHER})
target_link_directories(edge-streamer PRIVATE ${CPPHTTPLIB_LIBRARY_DIRS})

# ── Target: imu_packet_parser_test (no BLE dependency — always built) ─────────
#
# Build:  cmake --build build --target imu_packet_parser_test
# Run:    ./build/imu_packet_parser_test

add_executable(imu_packet_parser_test
    tests/imu_packet_parser_test.cpp
)
target_include_directories(imu_packet_parser_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# ── Target: edge-imu-ble (BLE IMU client) ─────────────────────────────────────
#
# Requires: sudo apt install libdbus-1-dev
# Fetches SimpleBLE via CMake FetchContent on first build.
#
# Disable with:  cmake -DBUILD_BLE_CLIENT=OFF ..

option(BUILD_BLE_CLIENT "Build BLE IMU client (requires libdbus-1-dev + internet for SimpleBLE)" ON)

if(BUILD_BLE_CLIENT)
    include(FetchContent)

    # SimpleBLE — C++ BLE library for Linux (BlueZ/D-Bus), macOS, Windows.
    # SOURCE_SUBDIR points to the library subfolder inside the monorepo.
    FetchContent_Declare(
        simpleble
        GIT_REPOSITORY https://github.com/OpenBluetoothToolbox/SimpleBLE.git
        GIT_TAG        v0.7.1
        SOURCE_SUBDIR  simpleble
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
    )

    # Suppress SimpleBLE's own example/test targets to keep build clean.
    set(SIMPLEBLE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SIMPLEBLE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(simpleble)

    # ── fall_detection static library ────────────────────────────────────────────
    add_library(fall_detection STATIC
        src/fall/FallDetector.cpp
        src/fall/EventStore.cpp
        src/fall/EventPoster.cpp
    )
    target_include_directories(fall_detection PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CPPHTTPLIB_INCLUDE_DIRS}
    )
    target_link_libraries(fall_detection PUBLIC ${HTTPLIB_LINK_LIBS})
    target_compile_options(fall_detection PRIVATE ${CPPHTTPLIB_CFLAGS_OTHER})
    target_link_directories(fall_detection PUBLIC ${CPPHTTPLIB_LIBRARY_DIRS})

    # ── edge-imu-ble (main BLE gateway) ──────────────────────────────────────────
    add_executable(edge-imu-ble
        src/imu_gateway_main.cpp
        src/imu/BleImuClient.cpp
    )

    target_include_directories(edge-imu-ble PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CPPHTTPLIB_INCLUDE_DIRS}
    )

    target_link_libraries(edge-imu-ble PRIVATE
        simpleble::simpleble
        fall_detection
        ${HTTPLIB_LINK_LIBS}
    )

    target_compile_options(edge-imu-ble PRIVATE ${CPPHTTPLIB_CFLAGS_OTHER})
    target_link_directories(edge-imu-ble PRIVATE ${CPPHTTPLIB_LIBRARY_DIRS})

    # ── fall_detector_test (synthetic unit tests, no BLE) ────────────────────────
    #
    # Build:  cmake --build build --target fall_detector_test
    # Run:    ./build/fall_detector_test
    add_executable(fall_detector_test
        tests/fall_detector_test.cpp
    )
    target_include_directories(fall_detector_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(fall_detector_test PRIVATE fall_detection)

    # ── replay_tool (feed saved imu_window.csv back through detector) ────────────
    #
    # Build:  cmake --build build --target replay_tool
    # Run:    ./build/replay_tool data/events/<id>/imu_window.csv [config.json]
    add_executable(replay_tool
        tests/replay_tool.cpp
    )
    target_include_directories(replay_tool PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(replay_tool PRIVATE fall_detection)

    message(STATUS "BLE client target: edge-imu-ble")
else()
    message(STATUS "BUILD_BLE_CLIENT=OFF — skipping edge-imu-ble")
endif()
