# -- pyhttpd config --
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/pyhttpd/config.ini.in ${CMAKE_CURRENT_SOURCE_DIR}/pyhttpd/config.ini @ONLY)

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

