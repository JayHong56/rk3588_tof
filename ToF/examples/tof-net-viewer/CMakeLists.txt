# Copyright (c) Analog Devices, Inc. All rights reserved.
# Minimum Version determined by the following dev environments
# Visual Studio 2017 15.7 - CMake 3.11
cmake_minimum_required(VERSION 3.10)

# set the project name
project(ADIToFNetGUI)
set(ADI_TOF_NET_VIEWER_TARGET ADIToFNetGUI)

option(ENBABLE_PASSIVE_IR "Allow passive IR (PCM) mode" OFF)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

configure_file(ADIToFConfig.h.in ADIToFConfig.h)

# adding submodules and subdirectories
if(NOT TARGET imgui)
    add_subdirectory(external/imgui)
elseif(NOT TARGET imgui::imgui)
    add_library(imgui::imgui ALIAS imgui)
endif()

if(NOT TARGET tof_net_common)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tof-net-common/CMakeLists.txt")
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tof-net-common ${CMAKE_CURRENT_BINARY_DIR}/tof-net-common)
    else()
        message(FATAL_ERROR "tof-net-common is required for network RAW transport")
    endif()
endif()
if( WIN32 )
    set(OS_SPECIFIC_DIR windows)
elseif ( APPLE )
    set(CMAKE_CXX_FLAGS "-x objective-c")
    set(OS_SPECIFIC_SOURCES ${ADIToF_SOURCE_DIR}/fileDialog.mm) 	
elseif ( UNIX )
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
    set(OS_SPECIFIC_DIR linux)
else()
    message(FATAL_ERROR "Platform not supported")
endif()

set(ADIToF_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(${ADI_TOF_NET_VIEWER_TARGET}
			   ${ADIToF_SOURCE_DIR}/wmain.cpp
			   ${ADIToF_SOURCE_DIR}/ADIView.cpp
			   ${ADIToF_SOURCE_DIR}/ADIToFRecorder.cpp
			   ${ADIToF_SOURCE_DIR}/ADIToF.cpp
			   ${ADIToF_SOURCE_DIR}/ADIMainWindow.cpp
			   ${ADIToF_SOURCE_DIR}/ADIInformationPane.cpp
			   ${ADIToF_SOURCE_DIR}/ADIImGUIExtensions.cpp
			   ${ADIToF_SOURCE_DIR}/ADIController.cpp
               ${ADIToF_SOURCE_DIR}/ADINetworkToFStream.cpp
               ${ADIToF_SOURCE_DIR}/ADIOpenFile.cpp			   
               ${OS_SPECIFIC_SOURCES}			   
			   )

set_target_properties(${ADI_TOF_NET_VIEWER_TARGET} PROPERTIES
    OUTPUT_NAME tof-net-viewer
)

# Link Libraries
if(UNIX)
	set(OpenGL_GL_PREFERENCE "LEGACY")
endif()

find_package(OpenGL REQUIRED)
if(DEFINED LIBTOFI_LIBDIR_PATH)
    link_directories(${LIBTOFI_LIBDIR_PATH})
endif()
if( WIN32 )																	 
	target_link_libraries(${ADI_TOF_NET_VIEWER_TARGET} PRIVATE "${PROJECT_SOURCE_DIR}/external/imgui/src/examples/libs/glfw/lib-vc2010-64/glfw3.lib")
elseif(APPLE)
    target_link_libraries(${ADI_TOF_NET_VIEWER_TARGET} PRIVATE "${PROJECT_SOURCE_DIR}/external/imgui/lib/macOS/glfw/lib-x86_64/libglfw3.a")
elseif (UNIX)
		find_package(glfw3 REQUIRED)
		target_link_libraries(${ADI_TOF_NET_VIEWER_TARGET} PRIVATE glfw)
	target_link_libraries(${ADI_TOF_NET_VIEWER_TARGET} PRIVATE glfw)
endif()

target_link_libraries(${ADI_TOF_NET_VIEWER_TARGET} PRIVATE
					  aditof
					  imgui::imgui
					  ${OPENGL_gl_LIBRARY}
					  ${CMAKE_DL_LIBS}
					  cjson
                      tof_net_common
                      tofi_compute
                      tofi_config
					 )

if( WIN32 )					 
	set_target_properties(${ADI_TOF_NET_VIEWER_TARGET} PROPERTIES
	LINK_FLAGS /SUBSYSTEM:WINDOWS
	)
elseif(APPLE)
    set(CMAKE_CXX_FLAGS "-framework Carbon -framework Cocoa -framework OpenGL -framework IOKit -framework CoreVideo")
endif()
	
target_include_directories(${ADI_TOF_NET_VIEWER_TARGET} PUBLIC
						"${PROJECT_BINARY_DIR}"
						"${PROJECT_SOURCE_DIR}/include"
						"${PROJECT_SOURCE_DIR}/external/imgui/src/examples/libs/gl3w"
						"${PROJECT_SOURCE_DIR}/external/imgui/src/examples/libs/glfw/include"
						"${PROJECT_SOURCE_DIR}/external/imgui/src/examples"
						"${PROJECT_SOURCE_DIR}/external/imgui/src"
						"${PROJECT_SOURCE_DIR}/src"
				)

target_include_directories(${ADI_TOF_NET_VIEWER_TARGET} PUBLIC
                        "${CMAKE_SOURCE_DIR}/sdk/src/cameras/itof-camera")

# TOFI headers are laid out differently across libaditof/ADI SDK trees.
# Some versions expose <tofi/tofi_compute.h>; others expose <tofi_compute.h>.
set(TOF_NET_VIEWER_TOFI_INCLUDE_CANDIDATES
    "${CMAKE_SOURCE_DIR}/sdk/include"
    "${CMAKE_SOURCE_DIR}/sdk/src"
    "${CMAKE_SOURCE_DIR}/sdk/src/cameras/itof-camera"
    "${CMAKE_SOURCE_DIR}/sdk/src/cameras/itof-camera/include"
    "${CMAKE_SOURCE_DIR}/sdk/src/cameras/itof-camera/tofi"
    "${CMAKE_SOURCE_DIR}/sdk/src/cameras/itof-camera/tofi/include"
    "${CMAKE_SOURCE_DIR}/sdk/src/cameras/itof-camera/itof"
    "${CMAKE_SOURCE_DIR}/sdk/src/cameras/itof-camera/itof/include"
)

foreach(_tofi_include_dir ${TOF_NET_VIEWER_TOFI_INCLUDE_CANDIDATES})
    if(EXISTS "${_tofi_include_dir}")
        target_include_directories(${ADI_TOF_NET_VIEWER_TARGET} PRIVATE "${_tofi_include_dir}")
    endif()
endforeach()

foreach(_tofi_include_var
        LIBTOFI_INCLUDE_DIR
        LIBTOFI_INCLUDE_DIRS
        LIBTOFI_INCLUDE_PATH
        LIBTOFI_INCLUDEDIR_PATH
        LIBTOFI_INCDIR_PATH)
    if(DEFINED ${_tofi_include_var})
        target_include_directories(${ADI_TOF_NET_VIEWER_TARGET} PRIVATE "${${_tofi_include_var}}")
    endif()
endforeach()

find_path(TOFI_COMPUTE_INCLUDE_DIR
    NAMES tofi_compute.h tofi/tofi_compute.h
    PATHS ${TOF_NET_VIEWER_TOFI_INCLUDE_CANDIDATES}
    NO_DEFAULT_PATH)
if(TOFI_COMPUTE_INCLUDE_DIR)
    target_include_directories(${ADI_TOF_NET_VIEWER_TARGET} PRIVATE "${TOFI_COMPUTE_INCLUDE_DIR}")
    message(STATUS "TOFI compute include dir: ${TOFI_COMPUTE_INCLUDE_DIR}")
endif()

if (ENBABLE_PASSIVE_IR)
	message(STATUS "Building with Passive IR Mode enabled")
	add_definitions(-DENBABLE_PASSIVE_IR)
endif()

# Copying necessary dlls from SDK and the necessary config files
add_custom_command(TARGET ${ADI_TOF_NET_VIEWER_TARGET} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/src/cameras/itof-camera/config $<TARGET_FILE_DIR:${ADI_TOF_NET_VIEWER_TARGET}>/${CONFIG_DIR_NAME}/.
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/tof-viewer_config.json $<TARGET_FILE_DIR:${ADI_TOF_NET_VIEWER_TARGET}>/.
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/camera_calibration_adsd3500.ccb $<TARGET_FILE_DIR:${ADI_TOF_NET_VIEWER_TARGET}>/${CONFIG_DIR_NAME}/.
    )

if(WIN32)
	# copy sdk lib and dc dll to exe directory
	add_custom_command(TARGET ${ADI_TOF_NET_VIEWER_TARGET} POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_directory ${LIBTOFI_LIBDIR_PATH} $<TARGET_FILE_DIR:${ADI_TOF_NET_VIEWER_TARGET}>/.
					COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_LINKER_FILE_DIR:aditof> ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>
					VERBATIM
					COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/src/cameras/itof-camera/config $<TARGET_FILE_DIR:${ADI_TOF_NET_VIEWER_TARGET}>/../${CONFIG_DIR_NAME}/.
					)
	if (WITH_NETWORK)
		add_custom_command(TARGET ${ADI_TOF_NET_VIEWER_TARGET} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${LWS_CMAKE_DIR}/../bin ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>
		)
	endif()
	if (WITH_GLOG_DEPENDENCY)
        add_custom_command(TARGET ${ADI_TOF_NET_VIEWER_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${GLOG_BIN_DIR} ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>
        )
    endif()
endif()
