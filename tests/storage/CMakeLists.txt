enable_testing()
include(GoogleTest)

add_executable(page_test page_test.cc)
target_link_libraries(page_test PRIVATE storage_lib type_lib catalog_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(page_test)

add_executable(disk_manager_test disk_manager_test.cc)
target_link_libraries(disk_manager_test PRIVATE storage_lib type_lib catalog_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(disk_manager_test)

add_executable(row_serializer_test row_serializer_test.cc)
target_link_libraries(row_serializer_test PRIVATE storage_lib type_lib catalog_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(row_serializer_test)

add_executable(buffer_pool_test buffer_pool_test.cc)
target_link_libraries(buffer_pool_test PRIVATE storage_lib type_lib catalog_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(buffer_pool_test)

add_executable(redo_log_test redo_log_test.cc)
target_link_libraries(redo_log_test PRIVATE storage_lib type_lib catalog_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(redo_log_test)

add_executable(table_heap_test table_heap_test.cc)
target_link_libraries(table_heap_test PRIVATE storage_lib type_lib catalog_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(table_heap_test)

add_executable(btree_page_test btree_page_test.cc)
target_link_libraries(btree_page_test PRIVATE storage_lib type_lib catalog_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(btree_page_test)

add_executable(stress_test stress_test.cc)
target_link_libraries(stress_test PRIVATE storage_lib type_lib catalog_lib base_lib gtest gtest_main spdlog)
gtest_discover_tests(stress_test)
