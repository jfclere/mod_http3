# -- probe httpd build for pyhttpd config --
find_program(APXS_EXECUTABLE NAMES apxs
  HINTS "${HTTPD_OUTPUT_DIRECTORY}/bin"
  NO_CACHE)
if(NOT APXS_EXECUTABLE)
  message(FATAL_ERROR "[test] apxs not found in ${HTTPD_OUTPUT_DIRECTORY}/bin - cannot configure pytest")
endif()

execute_process(COMMAND "${APXS_EXECUTABLE}" -q LIBEXECDIR
  OUTPUT_VARIABLE LIBEXECDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT HTTPD_VERSION)
  execute_process(COMMAND "${APXS_EXECUTABLE}" -q HTTPD_VERSION
    OUTPUT_VARIABLE HTTPD_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT HTTPD_VERSION)
  message(FATAL_ERROR "[test] apxs could not report HTTPD_VERSION")
endif()

set(_HTTPD_CONFIG_VARS "${DEPENDENCIES_DIRECTORY}/httpd/build/config_vars.mk")
set(DSO_MODULES "")
set(MPM_MODULES "")
if(EXISTS "${_HTTPD_CONFIG_VARS}")
  file(STRINGS "${_HTTPD_CONFIG_VARS}" _hcm_lines
       REGEX "^(DSO_MODULES|MPM_MODULES)[ \t]*=")
  foreach(_hcm_line IN LISTS _hcm_lines)
    string(REGEX REPLACE "^(DSO_MODULES|MPM_MODULES)[ \t]*=[ \t]*" "" _hcm_val "${_hcm_line}")
    if(_hcm_line MATCHES "^DSO_MODULES")
      set(DSO_MODULES "${_hcm_val}")
    elseif(_hcm_line MATCHES "^MPM_MODULES")
      set(MPM_MODULES "${_hcm_val}")
    endif()
  endforeach()
endif()
if(NOT DSO_MODULES)
  message(FATAL_ERROR "[test] DSO_MODULES not found in ${_HTTPD_CONFIG_VARS}")
endif()

set(HTTPD_NAME "httpd")
if(EXISTS "${_HTTPD_CONFIG_VARS}")
  file(STRINGS "${_HTTPD_CONFIG_VARS}" _hcm_prog
       REGEX "^progname[ \t]*=")
  foreach(_hcm_line IN LISTS _hcm_prog)
    string(REGEX REPLACE "^progname[ \t]*=[ \t]*" "" _hcm_val "${_hcm_line}")
    if(_hcm_val)
      set(HTTPD_NAME "${_hcm_val}")
    endif()
  endforeach()
endif()

set(HTTPD_PREFIX "${HTTPD_OUTPUT_DIRECTORY}")
if(APR_OUTPUT_DIRECTORY)
  set(APR_BINDIR "${APR_OUTPUT_DIRECTORY}/bin")
else()
  set(APR_BINDIR "${DEPENDENCIES_DIRECTORY}/apr-dist/bin")
endif()

find_program(CURL_BIN_EXECUTABLE NAMES curl
  HINTS "/opt/curl/bin" "/usr/bin" "/usr/local/bin")
if(NOT CURL_BIN_EXECUTABLE)
  set(CURL_BIN_EXECUTABLE "curl")
endif()
set(CURL_BIN "${CURL_BIN_EXECUTABLE}")

# -- pyhttpd config --
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/pyhttpd/config.ini.in
               ${CMAKE_CURRENT_SOURCE_DIR}/pyhttpd/config.ini @ONLY)

# -- pytest target --
find_program(PYTEST_EXECUTABLE NAMES pytest py.test)
if(PYTEST_EXECUTABLE)
  add_custom_target(pytest
    COMMAND ${PYTEST_EXECUTABLE} -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/test
    COMMENT "[pytest] Running pytest test suite..."
    VERBATIM
  )
else()
  message(STATUS "pytest not found - 'cmake --build build --target pytest' will not be available")
  message(STATUS "Install pytest with: pip install pytest")
  message(STATUS "Reconfigure or run tests manually: cd test && pytest -v")
endif()

# -- unit tests --
file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS "unit/*.c")
add_executable(${PROJECT_NAME}_tests ${test_sources})
target_link_libraries(${PROJECT_NAME}_tests PRIVATE ${PROJECT_NAME}-deps)
target_include_directories(${PROJECT_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/mod_http3/include ${CMAKE_CURRENT_SOURCE_DIR}/unit)

# -- register with CTest --
add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)
