# -- nghttp3 v1.15.0 --

if(TARGET nghttp3)
  return()
endif()

set(NGHTTP3_DIRECTORY "${DEPENDENCIES_DIRECTORY}/nghttp3")
set(NGHTTP3_OUTPUT_DIRECTORY "${DEPENDENCIES_OUTPUT_DIRECTORY}/nghttp3-dist")


require_initialized_submodule("${NGHTTP3_DIRECTORY}")
require_initialized_submodule("${NGHTTP3_DIRECTORY}/lib/sfparse")

block()
  set(CMAKE_MESSAGE_LOG_LEVEL "NOTICE")

  set(ENABLE_DEBUG OFF)
  set(ENABLE_WERROR OFF)
  set(ENABLE_ASAN OFF)
  set(ENABLE_LIB_ONLY ON)
  set(ENABLE_STATIC_LIB OFF)
  set(ENABLE_SHARED_LIB ON)
  set(ENABLE_STATIC_CRT OFF)
  set(BUILD_TESTING OFF)

  add_subdirectory("${NGHTTP3_DIRECTORY}" "${NGHTTP3_OUTPUT_DIRECTORY}" EXCLUDE_FROM_ALL SYSTEM)
endblock()

if(TARGET nghttp3)
  set(_NGHTTP3_TARGET nghttp3)
elseif(TARGET nghttp3_static)
  set(_NGHTTP3_TARGET nghttp3_static)
  add_library(nghttp3 ALIAS nghttp3_static)
else()
  message(FATAL_ERROR "nghttp3: add_subdirectory did not produce 'nghttp3' or 'nghttp3_static' target")
endif()

target_include_directories(${_NGHTTP3_TARGET} SYSTEM INTERFACE
  $<BUILD_INTERFACE:${NGHTTP3_DIRECTORY}/lib/includes> # nghttp3 has implicit private includedir..
  $<BUILD_INTERFACE:${NGHTTP3_OUTPUT_DIRECTORY}/lib/includes>         # generated headers
)

set_target_properties(${_NGHTTP3_TARGET} PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${NGHTTP3_OUTPUT_DIRECTORY}/lib"
  ARCHIVE_OUTPUT_DIRECTORY "${NGHTTP3_OUTPUT_DIRECTORY}/lib"
  RUNTIME_OUTPUT_DIRECTORY "${NGHTTP3_OUTPUT_DIRECTORY}/bin"
)