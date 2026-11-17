enable_testing()
include(GoogleTest)

add_executable(sql_test sql_test.cc sql_syntax_test.cc sql2006_test.cc tpch_test.cc executor_spill_test.cc)
target_link_libraries(sql_test PRIVATE tpch_workload bytecode_lib executor_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib gtest gtest_main spdlog)
target_include_directories(sql_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(sql_test)

add_executable(isolation_test isolation_test.cc)
target_link_libraries(isolation_test PRIVATE bytecode_lib executor_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib gtest gtest_main spdlog)
target_include_directories(isolation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(isolation_test)

add_executable(sql_bench sql_bench.cc)
target_link_libraries(sql_bench PRIVATE bytecode_lib executor_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib spdlog)
target_include_directories(sql_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(oltp_bench oltp_bench.cc)
target_link_libraries(oltp_bench PRIVATE bytecode_lib executor_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib spdlog)
target_include_directories(oltp_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(olap_bench olap_bench.cc)
target_link_libraries(olap_bench PRIVATE bytecode_lib executor_lib storage_lib type_lib catalog_lib base_lib parser_lib planner_lib spdlog)
target_include_directories(olap_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
