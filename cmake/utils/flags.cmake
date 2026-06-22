# -- Function to apply compile, warning, and sanitizer flags to a target
function(apply_target_flags target)

  # Assertions
  if(NOT TARGET ${target})
    message(FATAL_ERROR "${target} is not a target, no flags can be added.")
  endif()

  if((ENABLE_ASAN OR ENABLE_UBSAN) AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "Sanitizers require Debug build type. Current build type: ${CMAKE_BUILD_TYPE}")
  endif()

  # Set scope based on target type (INTERFACE vs non-INTERFACE)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    set(scope "INTERFACE")
  else()
    set(scope "PRIVATE")
  endif()

  # -- Sanitizer Flags --

  set(_sanitize_parts "")
  if(ENABLE_UBSAN)
    list(APPEND _sanitize_parts "undefined")
  endif()
  if(ENABLE_ASAN)
    list(APPEND _sanitize_parts "address")
  endif()

  if(_sanitize_parts)
    list(JOIN _sanitize_parts "," _sanitize_value)
    target_compile_options(${target} ${scope} -fsanitize=${_sanitize_value} -fno-omit-frame-pointer)
    target_link_options(${target} ${scope} -fsanitize=${_sanitize_value})
  endif()

  # -- Compile flags --
  set(FLAGS_DEBUG "-g3;-O0")
  set(FLAGS_RELEASE "-g;-O3")

  # -- Warning flags --
  set(_WARNINGS
      -Wall # Enable all standard warnings
      -Wextra # Reasonable and standard
      -Wshadow # Warn if a variable declaration shadows one from a parent context
      -Wcast-align # Warn for potential performance problem casts
      -Wno-unused-function # Warn on unused functions
      -Wpedantic # Warn if non-standard C is used
      -Wconversion # Warn on type conversions that may lose data
      -Wsign-conversion # Warn on sign conversions
      -Wnull-dereference # Warn if a null dereference is detected
      -Wdouble-promotion # Warn if float is implicitly promoted to double
      -Wformat=2 # Warn on security issues around functions that format output (ie printf)
      -Wmisleading-indentation # Warn if indentation implies blocks where blocks do not exist
      # GCC Exclusive Warnings:
      -Wduplicated-cond # Warn if if/else chain has duplicated conditions
      -Wduplicated-branches # Warn if if/else branches have duplicated code
      -Wlogical-op # Warn about logical operations being used where bitwise were probably wanted
  )
  if(ENABLE_WERROR)
    list(APPEND _WARNINGS -Werror)
  endif()

  # -- Apply compile and warning flags --
  target_compile_options(
    ${target}
    ${scope}
    $<$<CONFIG:Debug>:${FLAGS_DEBUG}>
    $<$<CONFIG:Release>:${FLAGS_RELEASE}>
    ${_WARNINGS})

endfunction()
