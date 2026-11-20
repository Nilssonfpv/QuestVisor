cmake_minimum_required(VERSION 3.22.1)
project(questvisor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Hardware decoding only (Android MediaCodec); no third-party libraries.
add_library(
    questvisor
    SHARED
    native_player.cpp
    usb_rndis.cpp
    tinynet.cpp
)

find_library(android-lib android)
find_library(mediandk-lib mediandk)
find_library(log-lib log)

target_include_directories(
    questvisor
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
)

target_link_libraries(
    questvisor
    PRIVATE
    ${android-lib}
    ${mediandk-lib}
    ${log-lib}
)

target_compile_options(
    questvisor
    PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-unused-function
)
