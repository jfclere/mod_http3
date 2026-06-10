# -- nghttp3 v1.15.90 --

if(TARGET nghttp3)
  return()
endif()

set(NGHTTP3_VERSION_MIN "1.15.90")

if(WITH_NGHTTP3)
  find_package(nghttp3 QUIET PATHS
    "${WITH_NGHTTP3}/lib/cmake/nghttp3"
    "${WITH_NGHTTP3}/lib64/cmake/nghttp3"
    NO_DEFAULT_PATH)
  if(NOT nghttp3_FOUND)
    message(FATAL_ERROR
        "[nghttp3] error: nghttp3 not found at WITH_NGHTTP3=${WITH_NGHTTP3}."
    )
  endif()
  set(NGHTTP3_OUTPUT_DIRECTORY "${WITH_NGHTTP3}")
else()

  set(NGHTTP3_DIRECTORY "${DEPENDENCIES_DIRECTORY}/nghttp3")
  set(NGHTTP3_OUTPUT_DIRECTORY "${DEPENDENCIES_OUTPUT_DIRECTORY}/nghttp3-dist")

  # Build nghttp3 from source if not already done
  if(NOT EXISTS "${NGHTTP3_OUTPUT_DIRECTORY}/.done")
    require_initialized_submodule("${NGHTTP3_DIRECTORY}")
    require_initialized_submodule("${NGHTTP3_DIRECTORY}/lib/sfparse")
    file(MAKE_DIRECTORY "${NGHTTP3_OUTPUT_DIRECTORY}/logs")

    if(EXISTS "${NGHTTP3_DIRECTORY}/CMakeCache.txt")
      message(STATUS "[nghttp3] Cleaning previous build artifacts")
      file(REMOVE "${NGHTTP3_DIRECTORY}/CMakeCache.txt")
    endif()

    message(STATUS "[nghttp3] Configuring -> ${NGHTTP3_OUTPUT_DIRECTORY}")

    execute_process(
      COMMAND cmake -B .
        -DENABLE_DEBUG=OFF
        -DENABLE_WERROR=OFF
        -DENABLE_ASAN=OFF
        -DENABLE_LIB_ONLY=ON
        -DENABLE_STATIC_LIB=ON
        -DENABLE_SHARED_LIB=ON
        -DENABLE_STATIC_CRT=OFF
        -DBUILD_TESTING=OFF
        -DCMAKE_INSTALL_PREFIX=${NGHTTP3_OUTPUT_DIRECTORY}
        --fresh
      WORKING_DIRECTORY "${NGHTTP3_DIRECTORY}"
      RESULT_VARIABLE _NGHTTP3_CONFIGURE_RESULT
      OUTPUT_FILE "${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-configure.log"
      ERROR_FILE "${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-configure.log")
    if(NOT _NGHTTP3_CONFIGURE_RESULT EQUAL 0)
      message(FATAL_ERROR "[nghttp3] error: configure failed -- see ${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-configure.log")
    endif()

    message(STATUS "[nghttp3] Building (${DEPENDENCIES_PARALLEL} jobs)")

    execute_process(
      COMMAND cmake --build . -j${DEPENDENCIES_PARALLEL} --clean-first
      WORKING_DIRECTORY "${NGHTTP3_DIRECTORY}"
      RESULT_VARIABLE _NGHTTP3_BUILD_RESULT
      OUTPUT_FILE "${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-build.log"
      ERROR_FILE "${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-build.log")
    if(NOT _NGHTTP3_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "[nghttp3] error: build failed -- see ${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-build.log")
    endif()

    message(STATUS "[nghttp3] Installing to ${NGHTTP3_OUTPUT_DIRECTORY}")

    execute_process(
      COMMAND cmake --install .
      WORKING_DIRECTORY "${NGHTTP3_DIRECTORY}"
      RESULT_VARIABLE _NGHTTP3_INSTALL_RESULT
      OUTPUT_FILE "${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-install.log"
      ERROR_FILE "${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-install.log")
    if(NOT _NGHTTP3_INSTALL_RESULT EQUAL 0)
      message(FATAL_ERROR "[nghttp3] error: install failed -- see ${NGHTTP3_OUTPUT_DIRECTORY}/logs/nghttp3-install.log")
    endif()

    string(TIMESTAMP _NGHTTP3_DONE_TIME "%Y-%b-%d_%H-%M-%S")
    file(WRITE "${NGHTTP3_OUTPUT_DIRECTORY}/.done" "${_NGHTTP3_DONE_TIME}")
  endif()

  # Find the nghttp3 we just built

  find_package(nghttp3 REQUIRED PATHS
    "${NGHTTP3_OUTPUT_DIRECTORY}/lib/cmake/nghttp3"
    "${NGHTTP3_OUTPUT_DIRECTORY}/lib64/cmake/nghttp3"
    NO_DEFAULT_PATH)
endif()

# Verify version is >= NGHTTP3_VERSION_MIN
if(nghttp3_VERSION VERSION_LESS NGHTTP3_VERSION_MIN)
  message(FATAL_ERROR
      "[nghttp3] error: found version ${nghttp3_VERSION} but require >= ${NGHTTP3_VERSION_MIN}."
  )
endif()

message(STATUS "[nghttp3] found (${nghttp3_VERSION}): ${NGHTTP3_OUTPUT_DIRECTORY}")

add_library(nghttp3 INTERFACE)
target_link_libraries(nghttp3 INTERFACE nghttp3::nghttp3)

# Extras

if(TARGET nghttp3::nghttp3_static)
  add_library(nghttp3_static INTERFACE)
  target_link_libraries(nghttp3_static INTERFACE nghttp3::nghttp3_static)
endif()
