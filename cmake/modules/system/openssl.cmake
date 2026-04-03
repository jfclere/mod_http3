# -- OpenSSL v3.5.0 --

if(TARGET openssl)
  return()
endif()

set(OPENSSL_DIRECTORY "${DEPENDENCIES_DIRECTORY}/openssl")
set(OPENSSL_OUTPUT_DIRECTORY "${DEPENDENCIES_OUTPUT_DIRECTORY}/openssl-dist")

set(OPENSSL_VERSION_MIN "3.5.0")

if(VENDOR_SYSTEM)

  # Build OpenSSL from source if not already done
  if(NOT EXISTS "${OPENSSL_OUTPUT_DIRECTORY}/.done")
    require_initialized_submodule("${OPENSSL_DIRECTORY}")
    file(MAKE_DIRECTORY "${OPENSSL_OUTPUT_DIRECTORY}/logs")


    if(EXISTS "${OPENSSL_DIRECTORY}/Makefile")

      message(STATUS "[openssl] Cleaning previous build artifacts")

      execute_process(
        COMMAND make distclean
        WORKING_DIRECTORY "${OPENSSL_DIRECTORY}"
        RESULT_VARIABLE _OPENSSL_DISTCLEAN_RESULT
        OUTPUT_FILE "${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-distclean.log"
        ERROR_FILE "${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-distclean.log")
      if(NOT _OPENSSL_DISTCLEAN_RESULT EQUAL 0)
        message(FATAL_ERROR "openssl distclean: failed -- see ${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-distclean.log")
      endif()
    endif()

    message(STATUS "[openssl] Configuring -> ${OPENSSL_OUTPUT_DIRECTORY}")

    execute_process(
      COMMAND ./config --prefix=${OPENSSL_OUTPUT_DIRECTORY} --openssldir=${OPENSSL_OUTPUT_DIRECTORY}/ssl shared
      WORKING_DIRECTORY "${OPENSSL_DIRECTORY}"
      RESULT_VARIABLE _OPENSSL_CONFIG_RESULT
      OUTPUT_FILE "${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-configure.log"
      ERROR_FILE "${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-configure.log")
    if(NOT _OPENSSL_CONFIG_RESULT EQUAL 0)
      message(FATAL_ERROR "openssl configure: failed -- see ${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-configure.log")
    endif()

    message(STATUS "[openssl] Building (${DEPENDENCIES_PARALLEL} jobs)")

    execute_process(
      COMMAND make -j${DEPENDENCIES_PARALLEL}
      WORKING_DIRECTORY "${OPENSSL_DIRECTORY}"
      RESULT_VARIABLE _OPENSSL_BUILD_RESULT
      OUTPUT_FILE "${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-build.log"
      ERROR_FILE "${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-build.log")
    if(NOT _OPENSSL_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "openssl build: failed -- see ${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-build.log")
    endif()

    message(STATUS "[openssl] Installing to ${OPENSSL_OUTPUT_DIRECTORY}")

    execute_process(
      COMMAND make install_sw
      WORKING_DIRECTORY "${OPENSSL_DIRECTORY}"
      RESULT_VARIABLE _OPENSSL_INSTALL_RESULT
      OUTPUT_FILE "${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-install.log"
      ERROR_FILE "${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-install.log")
    if(NOT _OPENSSL_INSTALL_RESULT EQUAL 0)
      message(FATAL_ERROR "openssl install: failed -- see ${OPENSSL_OUTPUT_DIRECTORY}/logs/openssl-install.log")
    endif()

    # Copy private headers not installed by make install_sw; symlink the openssl/ mirror
    # so both #include "internal/foo.h" and #include "openssl/internal/foo.h" resolve.
    message(STATUS "[openssl] Copying private headers to install prefix")
    file(COPY "${OPENSSL_DIRECTORY}/include/internal"
         DESTINATION "${OPENSSL_OUTPUT_DIRECTORY}/include")
    file(COPY "${OPENSSL_DIRECTORY}/include/crypto"
         DESTINATION "${OPENSSL_OUTPUT_DIRECTORY}/include")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E create_symlink
        "../internal" "${OPENSSL_OUTPUT_DIRECTORY}/include/openssl/internal"
      RESULT_VARIABLE _OSSL_SYMLINK_INTERNAL)
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E create_symlink
        "../crypto" "${OPENSSL_OUTPUT_DIRECTORY}/include/openssl/crypto"
      RESULT_VARIABLE _OSSL_SYMLINK_CRYPTO)
    if(NOT _OSSL_SYMLINK_INTERNAL EQUAL 0 OR NOT _OSSL_SYMLINK_CRYPTO EQUAL 0)
      message(FATAL_ERROR "openssl: failed to symlink private headers under include/openssl/")
    endif()

    string(TIMESTAMP _OPENSSL_DONE_TIME "%Y-%b-%d_%H-%M-%S")
    file(WRITE "${OPENSSL_OUTPUT_DIRECTORY}/.done" "${_OPENSSL_DONE_TIME}")
  endif()

  # Find the OpenSSL we just built

  set(OPENSSL_ROOT_DIR "${OPENSSL_OUTPUT_DIRECTORY}")
  find_package(OpenSSL ${OPENSSL_VERSION_MIN} REQUIRED QUIET COMPONENTS Crypto SSL PATHS "${OPENSSL_OUTPUT_DIRECTORY}" NO_DEFAULT_PATH)
else()
  list(APPEND CMAKE_PREFIX_PATH "/opt/openssl")
  find_package(OpenSSL ${OPENSSL_VERSION_MIN} QUIET COMPONENTS Crypto SSL)
  if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR
        "openssl >= ${OPENSSL_VERSION_MIN} not found. "
        "Install libssl-dev (Debian/Ubuntu) or openssl-devel (RHEL/Fedora), "
        "or set VENDOR_SYSTEM=ON to vendor system dependencies from source."
    )
  endif()
endif()

# Check for include/{crypto,internal} to verify that the OpenSSL installation includes the internal headers we need.
if(NOT EXISTS "${OPENSSL_INCLUDE_DIR}/internal" OR NOT IS_DIRECTORY "${OPENSSL_INCLUDE_DIR}/internal"
  OR NOT EXISTS "${OPENSSL_INCLUDE_DIR}/crypto" OR NOT IS_DIRECTORY "${OPENSSL_INCLUDE_DIR}/crypto")
  set(MISSING_OPENSSL_STATIC TRUE)
  message(WARNING "openssl internal headers not found - some features may be unavailable. VENDOR_SYSTEM=ON to vendor system dependencies from source.")
endif()

message(STATUS "Found openssl (${OPENSSL_VERSION}): ${OPENSSL_INCLUDE_DIR}")

add_library(openssl INTERFACE)
target_link_libraries(openssl INTERFACE OpenSSL::Crypto OpenSSL::SSL)


# Manually put together a static library target 
get_filename_component(OPENSSL_LIBRARY_PATH "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)
find_library(OPENSSL_SSL_STATIC NAMES libssl.a HINTS "${OPENSSL_LIBRARY_PATH}" NO_DEFAULT_PATH NO_CACHE)
find_library(OPENSSL_CRYPTO_STATIC NAMES libcrypto.a HINTS "${OPENSSL_LIBRARY_PATH}" NO_DEFAULT_PATH NO_CACHE)


if(NOT OPENSSL_SSL_STATIC OR NOT OPENSSL_CRYPTO_STATIC)
  message(WARNING
      "openssl: could not find static libraries. VENDOR_SYSTEM=ON to vendor system dependencies from source."
      "  library: ${OPENSSL_LIBRARY_PATH}"
      "  ssl static: ${OPENSSL_SSL_STATIC}"
      "  crypto static: ${OPENSSL_CRYPTO_STATIC}"
  )
  set(MISSING_OPENSSL_STATIC TRUE)
else()
  add_library(openssl_static INTERFACE)
  target_include_directories(openssl_static SYSTEM INTERFACE "${OPENSSL_INCLUDE_DIR}")
  target_link_libraries(openssl_static INTERFACE "${OPENSSL_SSL_STATIC}" "${OPENSSL_CRYPTO_STATIC}" ${CMAKE_DL_LIBS})
endif()
