#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "aditof::aditof" for configuration ""
set_property(TARGET aditof::aditof APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(aditof::aditof PROPERTIES
  IMPORTED_LINK_DEPENDENT_LIBRARIES_NOCONFIG "websockets_shared;aditof::tofi_compute;aditof::tofi_config"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libaditof.so.4.2.0"
  IMPORTED_SONAME_NOCONFIG "libaditof.so.1.0"
  )

list(APPEND _IMPORT_CHECK_TARGETS aditof::aditof )
list(APPEND _IMPORT_CHECK_FILES_FOR_aditof::aditof "${_IMPORT_PREFIX}/lib/libaditof.so.4.2.0" )

# Import target "aditof::tofi_compute" for configuration ""
set_property(TARGET aditof::tofi_compute APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(aditof::tofi_compute PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libtofi_compute.so"
  IMPORTED_SONAME_NOCONFIG "libtofi_compute.so"
  )

list(APPEND _IMPORT_CHECK_TARGETS aditof::tofi_compute )
list(APPEND _IMPORT_CHECK_FILES_FOR_aditof::tofi_compute "${_IMPORT_PREFIX}/lib/libtofi_compute.so" )

# Import target "aditof::tofi_config" for configuration ""
set_property(TARGET aditof::tofi_config APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(aditof::tofi_config PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libtofi_config.so"
  IMPORTED_SONAME_NOCONFIG "libtofi_config.so"
  )

list(APPEND _IMPORT_CHECK_TARGETS aditof::tofi_config )
list(APPEND _IMPORT_CHECK_FILES_FOR_aditof::tofi_config "${_IMPORT_PREFIX}/lib/libtofi_config.so" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
