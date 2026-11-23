# Integrate as cmake/Tests.cmake under the build owner's authority.
file(GLOB_RECURSE UOS_TEST_DISCOVERY CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/LINKS.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/links.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-tests.py")
execute_process(COMMAND /usr/bin/python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-tests.py"
  --registry "${CMAKE_CURRENT_BINARY_DIR}/tests-registry.json"
  --cmake "${CMAKE_CURRENT_BINARY_DIR}/registered-tests.cmake"
  RESULT_VARIABLE registry_result OUTPUT_VARIABLE registry_message ERROR_VARIABLE registry_error)
if(NOT registry_result EQUAL 0)
  message(FATAL_ERROR "Links test registration failed: ${registry_error}")
endif()
message(STATUS "Links test registration: ${registry_message}")
include("${CMAKE_CURRENT_BINARY_DIR}/registered-tests.cmake")
