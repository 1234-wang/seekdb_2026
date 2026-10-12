# Build and run with the exact production code under test. Each binary has one
# process-wide Google Test main; individual cases must not define another main.
find_package(Threads REQUIRED)
if(NOT EXISTS "${DEP_DIR}/lib/libgtest.a")
  message(FATAL_ERROR "Lifecycle regression tests require the initialized Google Test dependency")
endif()

add_executable(lifecycle_component_tests EXCLUDE_FROM_ALL
  lifecycle_test_main.cpp
  test_reusable_services.cpp)
target_link_libraries(lifecycle_component_tests PRIVATE
  oblib ob_malloc config "${DEP_DIR}/lib/libgtest.a" Threads::Threads)

add_executable(server_lifecycle_tests EXCLUDE_FROM_ALL
  lifecycle_test_main.cpp
  test_server_lifecycle.cpp
  test_px_pool_lifecycle.cpp
  test_dependency_queue.cpp
  test_bandwidth_throttle_lifecycle.cpp
  test_ddl_speed_lifecycle.cpp
  test_thread_idling_lifecycle.cpp
  test_io_lifecycle.cpp
  test_dtl_lifecycle.cpp
  test_storage_logger_lifecycle.cpp
  test_interrupt_registry.cpp
  test_debug_sync_lifecycle.cpp
  test_mds_lifecycle.cpp
  test_timezone_lifecycle.cpp
  test_dml_timezone_snapshot.cpp
  test_sql_statics.cpp)
target_compile_definitions(server_lifecycle_tests PRIVATE SEEKDB_SERVER_LIFECYCLE_TEST=1)
target_link_libraries(server_lifecycle_tests PRIVATE
  oceanbase "${DEP_DIR}/lib/libgtest.a" Threads::Threads)

add_custom_target(lifecycle_tests
  DEPENDS lifecycle_component_tests server_lifecycle_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/component/log")
foreach(lifecycle_target lifecycle_component_tests server_lifecycle_tests)
  add_test(NAME ${lifecycle_target}
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/run_completed_test.py"
      "$<TARGET_FILE:${lifecycle_target}>")
  set_tests_properties(${lifecycle_target} PROPERTIES
    LABELS "lifecycle"
    TIMEOUT 300
    RUN_SERIAL TRUE
    ENVIRONMENT "LD_LIBRARY_PATH=${DEVTOOLS_DIR}/lib64:$ENV{LD_LIBRARY_PATH}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/component")
endforeach()
