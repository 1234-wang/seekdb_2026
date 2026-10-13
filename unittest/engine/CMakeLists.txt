add_executable(engine_api_test engine_api_test.cpp)
target_include_directories(engine_api_test PRIVATE "${CMAKE_SOURCE_DIR}/src/observer/include")
get_target_property(_engine_link_libraries seekdb LINK_LIBRARIES)
list(REMOVE_ITEM _engine_link_libraries ob_main malloc_hook)
target_link_libraries(engine_api_test PRIVATE ${_engine_link_libraries})
add_test(NAME engine_api_invalid_arguments COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_engine_test.py" $<TARGET_FILE:engine_api_test> invalid)
add_test(NAME engine_api_lifetime COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_engine_test.py" $<TARGET_FILE:engine_api_test> lifetime)
set_tests_properties(engine_api_invalid_arguments engine_api_lifetime PROPERTIES TIMEOUT 300)

add_test(NAME engine_sdk_packaging COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_package_engine_sdk.py")

add_test(NAME engine_api_startup_failure COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_engine_test.py" $<TARGET_FILE:engine_api_test> startup_failure)
set_tests_properties(engine_api_startup_failure PROPERTIES TIMEOUT 300)

find_program(SEEKDB_TEST_MYSQL mysql)
if(SEEKDB_TEST_MYSQL)
  add_test(NAME engine_process_lifecycle
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_process_lifecycle.py"
      $<TARGET_FILE:seekdb> "${SEEKDB_TEST_MYSQL}")
  set_tests_properties(engine_process_lifecycle PROPERTIES TIMEOUT 180)
endif()
