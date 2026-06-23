cmake_minimum_required(VERSION 3.10)
project(tof_net_common LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    src/protocol.cpp
    src/socket.cpp
    src/zstd_codec.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Ubuntu 20.04/CMake 3.16 does not provide a reliable built-in FindZSTD.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_ZSTD QUIET libzstd)
endif()

find_path(ZSTD_INCLUDE_DIR
    NAMES zstd.h
    HINTS
        ${PC_ZSTD_INCLUDEDIR}
        ${PC_ZSTD_INCLUDE_DIRS}
)

find_library(ZSTD_LIBRARY
    NAMES zstd libzstd
    HINTS
        ${PC_ZSTD_LIBDIR}
        ${PC_ZSTD_LIBRARY_DIRS}
)

if(NOT ZSTD_INCLUDE_DIR OR NOT ZSTD_LIBRARY)
    message(FATAL_ERROR
        "Zstd development files were not found. Install them with:\n"
        "  sudo apt update && sudo apt install -y libzstd-dev\n"
        "or configure manually with:\n"
        "  -DZSTD_INCLUDE_DIR=/path/to/include -DZSTD_LIBRARY=/path/to/libzstd.so")
endif()

message(STATUS "Zstd include dir: ${ZSTD_INCLUDE_DIR}")
message(STATUS "Zstd library: ${ZSTD_LIBRARY}")

target_include_directories(${PROJECT_NAME} PUBLIC ${ZSTD_INCLUDE_DIR})
target_link_libraries(${PROJECT_NAME} PUBLIC ${ZSTD_LIBRARY})

if(UNIX)
    target_link_libraries(${PROJECT_NAME} PUBLIC pthread)
endif()
