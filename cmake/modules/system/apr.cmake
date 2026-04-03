# -- Apache Portable Runtime (APR) v1.7.0 --

if(TARGET apr)
  return()
endif()

set(APR_DIRECTORY "${DEPENDENCIES_DIRECTORY}/apr")
set(APR_OUTPUT_DIRECTORY "${DEPENDENCIES_OUTPUT_DIRECTORY}/apr-dist")

set(APR_VERSION_MIN "1.7.0")

# -- Find APR config tool --

if(VENDOR_SYSTEM)

  # Build apr from source if not already done
  if(NOT EXISTS "${APR_OUTPUT_DIRECTORY}/.done")
    require_initialized_submodule("${APR_DIRECTORY}")
    file(MAKE_DIRECTORY "${APR_OUTPUT_DIRECTORY}/logs")

    if(EXISTS "${APR_DIRECTORY}/Makefile")

      message(STATUS "[apr] Cleaning previous build artifacts")

      execute_process(
        COMMAND make distclean
        WORKING_DIRECTORY "${APR_DIRECTORY}"
        RESULT_VARIABLE _APR_DISTCLEAN_RESULT
        OUTPUT_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-distclean.log"
        ERROR_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-distclean.log")
      if(NOT _APR_DISTCLEAN_RESULT EQUAL 0)
        message(FATAL_ERROR "apr distclean: failed -- see ${APR_OUTPUT_DIRECTORY}/logs/apr-distclean.log")
      endif()
    endif()

    message(STATUS "[apr] Configuring -> ${APR_OUTPUT_DIRECTORY}")

    execute_process(
      COMMAND ./buildconf
      WORKING_DIRECTORY "${APR_DIRECTORY}"
      RESULT_VARIABLE _APR_BUILDCONF_RESULT
      OUTPUT_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-buildconf.log"
      ERROR_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-buildconf.log")
    if(NOT _APR_BUILDCONF_RESULT EQUAL 0)
      message(FATAL_ERROR "apr buildconf: failed -- see ${APR_OUTPUT_DIRECTORY}/logs/apr-buildconf.log")
    endif()

    execute_process(
      COMMAND ./configure --prefix=${APR_OUTPUT_DIRECTORY}
      WORKING_DIRECTORY "${APR_DIRECTORY}"
      RESULT_VARIABLE _APR_CONFIGURE_RESULT
      OUTPUT_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-configure.log"
      ERROR_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-configure.log")
    if(NOT _APR_CONFIGURE_RESULT EQUAL 0)
      message(FATAL_ERROR "apr configure: failed -- see ${APR_OUTPUT_DIRECTORY}/logs/apr-configure.log")
    endif()

    message(STATUS "[apr] Building (${DEPENDENCIES_PARALLEL} jobs)")

    execute_process(
      COMMAND make -j${DEPENDENCIES_PARALLEL}
      WORKING_DIRECTORY "${APR_DIRECTORY}"
      RESULT_VARIABLE _APR_BUILD_RESULT
      OUTPUT_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-build.log"
      ERROR_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-build.log")
    if(NOT _APR_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "apr build: failed -- see ${APR_OUTPUT_DIRECTORY}/logs/apr-build.log")
    endif()

    message(STATUS "[apr] Installing to ${APR_OUTPUT_DIRECTORY}")

    execute_process(
      COMMAND make install
      WORKING_DIRECTORY "${APR_DIRECTORY}"
      RESULT_VARIABLE _APR_INSTALL_RESULT
      OUTPUT_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-install.log"
      ERROR_FILE "${APR_OUTPUT_DIRECTORY}/logs/apr-install.log")
    if(NOT _APR_INSTALL_RESULT EQUAL 0)
      message(FATAL_ERROR "apr install: failed -- see ${APR_OUTPUT_DIRECTORY}/logs/apr-install.log")
    endif()

    string(TIMESTAMP _APR_DONE_TIME "%Y-%b-%d_%H-%M-%S")
    file(WRITE "${APR_OUTPUT_DIRECTORY}/.done" "${_APR_DONE_TIME}")
  endif()

  # Find the apr we just built

  find_program(
    APR_CONFIG_EXECUTABLE
    NAMES apr-1-config apr-config
    HINTS "${APR_OUTPUT_DIRECTORY}/bin"
    NO_DEFAULT_PATH REQUIRED NO_CACHE)
else()
  find_program(APR_CONFIG_EXECUTABLE NAMES apr-1-config apr-config NO_CACHE)
  if(NOT APR_CONFIG_EXECUTABLE)
      message(FATAL_ERROR
          "apr-config not found. "
          "Install libapr1-dev (Debian/Ubuntu) or apr-devel (RHEL/Fedora), "
          "or set VENDOR_SYSTEM=ON to vendor system dependencies from source."
      )
  endif()
endif()

# -- Extract APR information --

# version
execute_process(
  COMMAND "${APR_CONFIG_EXECUTABLE}" --version
  OUTPUT_VARIABLE APR_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE APR_VERSION_RESULT
)
if(NOT APR_VERSION_RESULT EQUAL 0 OR NOT APR_VERSION OR APR_VERSION VERSION_LESS APR_VERSION_MIN)
  message(FATAL_ERROR
    "apr: APR_CONFIG did not report a valid version\n"
    "  APR_CONFIG  = ${APR_CONFIG_EXECUTABLE}\n"
    "  APR_VERSION = ${APR_VERSION}\n"
    "  result      = ${APR_VERSION_RESULT}")
endif()

# include dir
execute_process(
  COMMAND "${APR_CONFIG_EXECUTABLE}" --includedir
  OUTPUT_VARIABLE APR_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE APR_INCLUDEDIR_RESULT
)
if(NOT APR_INCLUDEDIR_RESULT EQUAL 0 OR NOT APR_INCLUDE_DIR OR NOT EXISTS "${APR_INCLUDE_DIR}/apr.h")
  message(FATAL_ERROR
    "apr: APR_CONFIG did not report a valid include dir\n"
    "  APR_CONFIG = ${APR_CONFIG_EXECUTABLE}\n"
    "  includedir = ${APR_INCLUDE_DIR}\n"
    "  result     = ${APR_INCLUDEDIR_RESULT}")
endif()

# link flags
execute_process(
  COMMAND "${APR_CONFIG_EXECUTABLE}" --link-ld --libs
  OUTPUT_VARIABLE APR_LINK_FLAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE APR_LINK_FLAGS_RESULT
)
if(NOT APR_LINK_FLAGS_RESULT EQUAL 0 OR NOT APR_LINK_FLAGS)
  message(FATAL_ERROR
    "apr: APR_CONFIG did not report valid link flags\n"
    "  APR_CONFIG = ${APR_CONFIG_EXECUTABLE}\n"
    "  link flags = ${APR_LINK_FLAGS}\n"
    "  result     = ${APR_LINK_FLAGS_RESULT}")
endif()

message(STATUS "Found APR (${APR_VERSION}): ${APR_INCLUDE_DIR}")

separate_arguments(APR_LINK_FLAGS_LIST UNIX_COMMAND "${APR_LINK_FLAGS}")
string(REGEX MATCH "-L([^ \t]+)" APR_L_MATCH "${APR_LINK_FLAGS}")
set(APR_LIB_DIR "${CMAKE_MATCH_1}")

add_library(apr INTERFACE)
target_include_directories(apr SYSTEM INTERFACE "${APR_INCLUDE_DIR}")
target_link_options(apr INTERFACE ${APR_LINK_FLAGS_LIST})
if(APR_LIB_DIR)
  target_link_options(apr INTERFACE "-Wl,-rpath,${APR_LIB_DIR}")
endif()