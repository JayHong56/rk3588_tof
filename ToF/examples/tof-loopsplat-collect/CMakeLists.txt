cmake_minimum_required(VERSION 3.10)
project(tof_loopsplat_collect LANGUAGES CXX)

if(NOT DEFINED CONFIG_DIR_NAME)
    set(CONFIG_DIR_NAME config)
endif()

add_executable(${PROJECT_NAME}
    src/main.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

# Find OpenCV (required for cv::imwrite 16-bit PNG)
find_package(OpenCV REQUIRED)

# Link aditof SDK
if(TARGET aditof)
    target_link_libraries(${PROJECT_NAME} PRIVATE aditof)
    get_filename_component(TOF_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
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
    target_link_libraries(${PROJECT_NAME} PRIVATE ${ADITOF_LIBRARY})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})

if(NOT WIN32)
    find_library(PTHREAD_LIBRARIES pthread)
    if(PTHREAD_LIBRARIES)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${PTHREAD_LIBRARIES})
    else()
        message(FATAL_ERROR "pthread libraries not found!")
    endif()
endif()

# Copy camera config JSON files to build directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/src/cameras/itof-camera/config
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/${CONFIG_DIR_NAME}/.
    COMMENT "Copying camera config files to build directory"
)
