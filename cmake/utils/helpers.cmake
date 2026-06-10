# -- Convienient Helpers --

# Print compile options of a target.
function(inspect_target target)
  if(NOT TARGET ${target})
    message(STATUS "inspect_target: ${target} does not exist")
    return()
  endif()
  section("Inspect: ${target}")
  get_target_property(_opts ${target} COMPILE_OPTIONS)
  get_target_property(_iopts ${target} INTERFACE_COMPILE_OPTIONS)
  get_target_property(_flags ${target} COMPILE_FLAGS)
  message(STATUS "  COMPILE_OPTIONS           : ${_opts}")
  message(STATUS "  INTERFACE_COMPILE_OPTIONS  : ${_iopts}")
  message(STATUS "  COMPILE_FLAGS              : ${_flags}")
endfunction()

# Check that a submodule directory is non-empty, otherwise fail with msg
macro(require_initialized_submodule DIR)
  file(GLOB _DIR_FILES "${DIR}/*")
  list(LENGTH _DIR_FILES _DIR_CONTENTS_LEN)
  if(_DIR_CONTENTS_LEN EQUAL 0)
    message(FATAL_ERROR "Missing dependency: ${DIR} is empty.\n" "Initialize the git submodule with:\n"
                        "    git -C ${DIR} submodule update --init .\n")
  endif()
endmacro()
