# -- googletest v1.17.x --

if(TARGET gtest)
  return()
endif()

set(GTEST_DIRECTORY "${DEPENDENCIES_DIRECTORY}/googletest")
set(GTEST_OUTPUT_DIRECTORY "${DEPENDENCIES_OUTPUT_DIRECTORY}/googletest-dist")

require_initialized_submodule("${GTEST_DIRECTORY}")

# Build options scoped to the subdirectory only.
block()
  set(CMAKE_MESSAGE_LOG_LEVEL "NOTICE")

  set(BUILD_GMOCK ON)
  set(INSTALL_GTEST OFF)
  set(GTEST_HAS_ABSL OFF)
  set(BUILD_SHARED_LIBS OFF)

  set(gtest_force_shared_crt ON)
  set(gtest_build_tests OFF)
  set(gtest_build_samples OFF)
  set(gtest_disable_pthreads OFF)
  set(gtest_hide_internal_symbols OFF)

  set(gmock_build_tests OFF)

  add_subdirectory("${GTEST_DIRECTORY}" "${GTEST_OUTPUT_DIRECTORY}" EXCLUDE_FROM_ALL SYSTEM)
endblock()

if(TARGET gmock)
  set(_GOOGLETEST_LIBS gmock gtest)
  add_library(googletest ALIAS gmock) # Note that googlemock target already builds googletest.
elseif(TARGET gtest)
  set(_GOOGLETEST_LIBS gtest)
  add_library(googletest ALIAS gtest)
else()
  message(FATAL_ERROR "[googletest] error: add_subdirectory did not produce 'gtest' or 'gmock' target")
endif()

set_target_properties(${_GOOGLETEST_LIBS} PROPERTIES 
  LIBRARY_OUTPUT_DIRECTORY "${GTEST_OUTPUT_DIRECTORY}/lib"
  ARCHIVE_OUTPUT_DIRECTORY "${GTEST_OUTPUT_DIRECTORY}/lib"
  RUNTIME_OUTPUT_DIRECTORY "${GTEST_OUTPUT_DIRECTORY}/bin"
)
