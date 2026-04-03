# -- Apache httpd v2.4.x (20211221) --

if(TARGET httpd)
  return()
endif()

set(HTTPD_DIRECTORY "${DEPENDENCIES_DIRECTORY}/httpd")
set(HTTPD_OUTPUT_DIRECTORY "${DEPENDENCIES_OUTPUT_DIRECTORY}/httpd-dist")

set(HTTPD_VERSION_MIN "2.4.x")
set(HTTPD_MMN_MIN "20211221")

# -- Find apxs config tool --

if(VENDOR_SYSTEM)

  # httpd depends on openssl
  require_initialized_submodule("${DEPENDENCIES_DIRECTORY}/openssl")
  if(NOT OPENSSL_OUTPUT_DIRECTORY OR NOT EXISTS "${OPENSSL_OUTPUT_DIRECTORY}/.done" OR NOT TARGET openssl)
    message(FATAL_ERROR "httpd: cannot vendor httpd without vendored openssl -- please build openssl first")
  endif()

  # httpd depends on apr
  require_initialized_submodule("${DEPENDENCIES_DIRECTORY}/apr")
  if(NOT APR_OUTPUT_DIRECTORY OR NOT EXISTS "${APR_OUTPUT_DIRECTORY}/.done" OR NOT TARGET apr)
    message(FATAL_ERROR "httpd: cannot vendor httpd without vendored apr -- please build apr first")
  endif()

  # httpd depends on apu (until APR v2 release - apr-2.x bundles apu)
  require_initialized_submodule("${DEPENDENCIES_DIRECTORY}/apr-util")
  if(NOT APU_OUTPUT_DIRECTORY OR NOT EXISTS "${APU_OUTPUT_DIRECTORY}/.done" OR NOT TARGET apu)
    message(FATAL_ERROR "httpd: cannot vendor httpd without vendored apu -- please build apu first")
  endif()

  # Build httpd from source if not already done
  if(NOT EXISTS "${HTTPD_OUTPUT_DIRECTORY}/.done")
    require_initialized_submodule("${HTTPD_DIRECTORY}")
    file(MAKE_DIRECTORY "${HTTPD_OUTPUT_DIRECTORY}/logs")

    if(EXISTS "${HTTPD_DIRECTORY}/Makefile")

      message(STATUS "[httpd] Cleaning previous build artifacts")

      execute_process(
        COMMAND make distclean
        WORKING_DIRECTORY "${HTTPD_DIRECTORY}"
        RESULT_VARIABLE _HTTPD_DISTCLEAN_RESULT
        OUTPUT_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-distclean.log"
        ERROR_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-distclean.log")
      if(NOT _HTTPD_DISTCLEAN_RESULT EQUAL 0)
        message(FATAL_ERROR "httpd distclean: failed -- see ${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-distclean.log")
      endif()
    endif()

    message(STATUS "[httpd] Configuring -> ${HTTPD_OUTPUT_DIRECTORY}")

    execute_process(
      COMMAND ./buildconf --with-apr=${DEPENDENCIES_DIRECTORY}/apr --with-apr-util=${DEPENDENCIES_DIRECTORY}/apr-util
      WORKING_DIRECTORY "${HTTPD_DIRECTORY}"
      RESULT_VARIABLE _HTTPD_BUILDCONF_RESULT
      OUTPUT_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-buildconf.log"
      ERROR_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-buildconf.log")
    if(NOT _HTTPD_BUILDCONF_RESULT EQUAL 0)
      message(FATAL_ERROR "httpd buildconf: failed -- see ${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-buildconf.log")
    endif()

    # Resolve OpenSSL lib dir for rpath - valid assumption that we have already built OpenSSL if we're building httpd from source
    get_filename_component(_HTTPD_OPENSSL_LIBDIR "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)

    execute_process(
      COMMAND ${CMAKE_COMMAND} -E env LDFLAGS=-Wl,-rpath,${_HTTPD_OPENSSL_LIBDIR}
        "${HTTPD_DIRECTORY}/configure"
          --prefix=${HTTPD_OUTPUT_DIRECTORY}
          --with-apr=${APR_OUTPUT_DIRECTORY}
          --with-apr-util=${APU_OUTPUT_DIRECTORY}
          --enable-so
          --with-mpm=event
          --enable-mods-shared=all
          --enable-ssl
          --with-ssl=${DEPENDENCIES_OUTPUT_DIRECTORY}/openssl
      WORKING_DIRECTORY "${HTTPD_DIRECTORY}"
      RESULT_VARIABLE _HTTPD_CONFIGURE_RESULT
      OUTPUT_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-configure.log"
      ERROR_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-configure.log")
    if(NOT _HTTPD_CONFIGURE_RESULT EQUAL 0)
      message(FATAL_ERROR "httpd configure: failed -- see ${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-configure.log")
    endif()

    message(STATUS "[httpd] Building (${DEPENDENCIES_PARALLEL} jobs)")

    execute_process(
      COMMAND make -j${DEPENDENCIES_PARALLEL}
      WORKING_DIRECTORY "${HTTPD_DIRECTORY}"
      RESULT_VARIABLE _HTTPD_BUILD_RESULT
      OUTPUT_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-build.log"
      ERROR_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-build.log")
    if(NOT _HTTPD_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "httpd build: failed -- see ${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-build.log")
    endif()

    message(STATUS "[httpd] Installing to ${HTTPD_OUTPUT_DIRECTORY}")

    execute_process(
      COMMAND make install
      WORKING_DIRECTORY "${HTTPD_DIRECTORY}"
      RESULT_VARIABLE _HTTPD_INSTALL_RESULT
      OUTPUT_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-install.log"
      ERROR_FILE "${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-install.log")
    if(NOT _HTTPD_INSTALL_RESULT EQUAL 0)
      message(FATAL_ERROR "httpd install: failed -- see ${HTTPD_OUTPUT_DIRECTORY}/logs/httpd-install.log")
    endif()

    string(TIMESTAMP _HTTPD_DONE_TIME "%Y-%b-%d_%H-%M-%S")
    file(WRITE "${HTTPD_OUTPUT_DIRECTORY}/.done" "${_HTTPD_DONE_TIME}")
  endif()

  # Find the httpd we just built

  find_program(
    APXS_EXECUTABLE
    NAMES apxs apxs2
    HINTS "${HTTPD_OUTPUT_DIRECTORY}/bin"
    NO_DEFAULT_PATH REQUIRED NO_CACHE)
else()
  list(APPEND CMAKE_PREFIX_PATH "/opt/httpd")
  find_program(APXS_EXECUTABLE NAMES apxs apxs2 NO_CACHE)
  if(NOT APXS_EXECUTABLE)
      message(FATAL_ERROR
          "apxs not found. "
          "Install apache2-dev (Debian/Ubuntu) or httpd-devel (RHEL/Fedora), "
          "or set VENDOR_SYSTEM=ON to vendor system dependencies from source."
      )
  endif()
endif()

# -- Extract httpd information --

# version
execute_process(
  COMMAND "${APXS_EXECUTABLE}" -q HTTPD_VERSION
  OUTPUT_VARIABLE HTTPD_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE HTTPD_VERSION_RESULT
)
if(NOT HTTPD_VERSION_RESULT EQUAL 0 OR NOT HTTPD_VERSION OR HTTPD_VERSION VERSION_LESS HTTPD_VERSION_MIN)
  message(FATAL_ERROR
    "httpd: apxs did not report a valid HTTPD_VERSION (need at least ${HTTPD_VERSION_MIN})\n"
    "  apxs          = ${APXS_EXECUTABLE}\n"
    "  HTTPD_VERSION = ${HTTPD_VERSION}\n"
    "  result        = ${HTTPD_VERSION_RESULT}")
endif()

# include dir
execute_process(
  COMMAND "${APXS_EXECUTABLE}" -q INCLUDEDIR
  OUTPUT_VARIABLE HTTPD_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE HTTPD_INCLUDE_RESULT
)
if(NOT HTTPD_INCLUDE_RESULT EQUAL 0 OR NOT EXISTS "${HTTPD_INCLUDE_DIR}/httpd.h")
  message(FATAL_ERROR
    "httpd: could not locate headers via apxs\n"
    "  apxs       = ${APXS_EXECUTABLE}\n"
    "  INCLUDEDIR = ${HTTPD_INCLUDE_DIR}\n"
    "  result     = ${HTTPD_INCLUDE_RESULT}")
endif()

# module magic number
execute_process(
  COMMAND "${APXS_EXECUTABLE}" -q HTTPD_MMN
  OUTPUT_VARIABLE HTTPD_MMN
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE HTTPD_MMN_RESULT
)
if(NOT HTTPD_MMN_RESULT EQUAL 0 OR NOT HTTPD_MMN OR HTTPD_MMN LESS HTTPD_MMN_MIN)
  message(FATAL_ERROR
    "httpd: apxs did not report a valid HTTPD_MMN (need at least ${HTTPD_MMN_MIN})\n"
    "  apxs       = ${APXS_EXECUTABLE}\n"
    "  HTTPD_MMN  = ${HTTPD_MMN}\n"
    "  result     = ${HTTPD_MMN_RESULT}")
endif()

message(STATUS "Found HTTPD (${HTTPD_VERSION}): ${HTTPD_INCLUDE_DIR}")

add_library(httpd INTERFACE)
target_include_directories(httpd SYSTEM INTERFACE "${HTTPD_INCLUDE_DIR}")
