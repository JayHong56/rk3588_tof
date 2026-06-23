cmake_minimum_required(VERSION 3.10)
project(tof_net_collect LANGUAGES CXX)

# Built to follow the examples/data_collect pattern: target-local executable,
# aditof linkage, post-build camera config copy, and FILE-style runtime config.
if(NOT DEFINED CONFIG_DIR_NAME)
    set(CONFIG_DIR_NAME config)
endif()

add_executable(${PROJECT_NAME}
    src/main.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

if(NOT TARGET tof_net_common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tof-net-common ${CMAKE_BINARY_DIR}/tof_net_common)
endif()

if(TARGET aditof)
    target_link_libraries(${PROJECT_NAME} PRIVATE aditof)
    # Also need SDK internal headers (mode_info.h) and TOFI headers
    # (tofi_camera_intrinsics.h) for ADSD3500 dealias export.
    get_filename_component(TOF_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${TOF_SOURCE_ROOT}/sdk/src/cameras/itof-camera"
        "${TOF_SOURCE_ROOT}/sdk/common/adi/tofi"
    )
else()
    get_filename_component(TOF_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(ADITOF_INCLUDE_DIR "${TOF_SOURCE_ROOT}/sdk/include" CACHE PATH "aditof include dir")
    find_library(ADITOF_LIBRARY aditof
        HINTS
            ${TOF_SOURCE_ROOT}/build
            ${TOF_SOURCE_ROOT}/build/sdk
            ${TOF_SOURCE_ROOT}/build/lib
            ${TOF_SOURCE_ROOT}/lib
        REQUIRED
    )
    target_include_directories(${PROJECT_NAME} PRIVATE ${ADITOF_INCLUDE_DIR})
    # Also need SDK internal headers (mode_info.h) and TOFI headers
    # (tofi_camera_intrinsics.h) for ADSD3500 dealias export.
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${TOF_SOURCE_ROOT}/sdk/src/cameras/itof-camera"
        "${TOF_SOURCE_ROOT}/sdk/common/adi/tofi"
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE ${ADITOF_LIBRARY})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    tof_net_common
)

if(NOT WIN32)
    find_library(PTHREAD_LIBRARIES pthread)
    if(PTHREAD_LIBRARIES)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${PTHREAD_LIBRARIES})
    else()
        message(FATAL_ERROR "pthread libraries not found!")
    endif()
endif()

# Same post-build style as examples/data_collect: copy sdk camera config next to
# the executable. The runtime FILE argument is one of these ADI initialization JSON files.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/src/cameras/itof-camera/config
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/${CONFIG_DIR_NAME}/.
    COMMENT "Copying cfgs to build examples"
)
