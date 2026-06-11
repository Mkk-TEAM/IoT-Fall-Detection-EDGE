cmake_minimum_required(VERSION 3.16)

project(edge_streamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgcodecs videoio imgproc)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(CPPHTTPLIB REQUIRED cpp-httplib)
find_library(BROTLICOMMON_LIBRARY NAMES brotlicommon REQUIRED)
find_library(BROTLIDEC_LIBRARY NAMES brotlidec REQUIRED)
find_library(BROTLIENC_LIBRARY NAMES brotlienc REQUIRED)

add_executable(edge-streamer
  src/main.cpp
)

target_link_libraries(edge-streamer
  PRIVATE
    ${OpenCV_LIBS}
    ${CPPHTTPLIB_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    ${BROTLICOMMON_LIBRARY}
    ${BROTLIDEC_LIBRARY}
    ${BROTLIENC_LIBRARY}
)

target_include_directories(edge-streamer
  PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${CPPHTTPLIB_INCLUDE_DIRS}
)

target_compile_options(edge-streamer PRIVATE ${CPPHTTPLIB_CFLAGS_OTHER})
target_link_directories(edge-streamer PRIVATE ${CPPHTTPLIB_LIBRARY_DIRS})
